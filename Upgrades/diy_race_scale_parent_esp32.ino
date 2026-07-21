/*
traviscea DIY Race Scales – Version 1.1 (accuracy patch)
Based on Version 1.0, Copyright (c) 2026 Travis Way

CHANGES vs 1.0 (master side):
1. Per-pad rolling window (32 raw samples, ~3 s at 10 Hz) with a 50%
   trimmed mean — outlier spikes from the HX711 no longer pollute
   readings the way plain read_average() did.
2. Multi-point piecewise-linear calibration (up to 3 points per pad,
   stored in NVS). Single-point cal extrapolated from a light weight to
   a ~600 lb corner load is the largest error source with these cells.
   Recommended: cal each pad at ~2 loads bracketing your corner weights
   (e.g. ~150 lb and ~500 lb). Entering weight 0 in CAL clears that
   pad's points.
3. CAL and ZERO now use the trimmed mean of the full window (all pads,
   local FL included) instead of one instantaneous packet, and CAL is
   rejected unless the pad is statistically settled.
4. Stability lock is now real: locks when the stddev of the calibrated
   window is < 0.35 lb, holds the locked value on screen, unlocks on a
   > 1.0 lb move. The old lock was cosmetic and compared against a
   stale reference.
5. Full float precision kept internally; per-corner 0.5 lb rounding and
   the +/-0.5 lb zero snap no longer feed the total / front / rear /
   left / right / cross math. Rounding happens only at JSON output.
6. Pads stream RAW counts (flash pad v1.1 on all children). Tare lives
   entirely on the master, so a stale months-old NVS tare on a pad can
   no longer bias readings across temperature swings. Re-ZERO at the
   start of every session.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <Preferences.h>
#include "HX711.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "ScaleProtocol.h"

Preferences prefs;

// Change to match your "master board"
#define HX_DT 4
#define HX_SCK 5

/* ---------- ESP-NOW receive queue ----------
   Callback enqueues validated ScalePackets here.
   loop() is the sole owner of pad state; it drains this queue.     */
#define RX_QUEUE_LEN  16
static QueueHandle_t rxQueue = nullptr;

/* ---------- per-pad state ---------- */
#define RING_N        32     // ~3.2 s window at 10 Hz
#define MIN_SAMPLES   12     // minimum window fill for tare
#define CAL_SAMPLES   20     // minimum window fill for calibration
#define MAX_PTS       3      // calibration points per pad
#define LOCK_SD       0.35f  // lbs: lock when window stddev below this
#define UNLOCK_DELTA  1.0f   // lbs: unlock when value moves this far

struct Pad {
  const char* id;
  float ring[RING_N];
  int   count = 0;
  int   head  = 0;
  float offset = 0;             // raw counts
  int   nPts = 0;
  float ptRaw[MAX_PTS];         // net raw counts, ascending
  float ptLbs[MAX_PTS];
  float gain = 1.0f;            // rotation-cal trim: corrected = cal / gain
  float value = 0;              // calibrated lbs, full precision
  float sd    = 0;              // window stddev, lbs
  bool  locked = false;
  float held   = 0;             // frozen display value while locked
  float batt   = 0;
  unsigned long lastSeen = 0;
  bool  local = false;          // FL is wired to the master
  /* link quality (remote pads only; FL stays 0/0) */
  uint8_t  lastSeq   = 0;
  bool     seqInit   = false;
  uint32_t rxCount   = 0;
  uint32_t lossCount = 0;       // gaps derived from the packet seq field
};

Pad pads[4];
enum { P_FL = 0, P_FR = 1, P_RL = 2, P_RR = 3 };

bool scalePresent = false;
bool scaleInitialized = false;
unsigned long lastScaleCheck = 0;

HX711 scale;
WebServer server(80);

/* ---------- helpers ---------- */

int battPercent(float v){
  int p = (v - 3.0f) * 100 / 1.2f;
  if(p > 100) p = 100;
  if(p < 0) p = 0;
  return p;
}

float sanitizeFloat(float v){
  if(isnan(v) || isinf(v)) return 0.0f;
  return v;
}

/* Piecewise-linear calibration.
   0 pts: raw counts pass through (uncalibrated).
   1 pt : slope through origin (same behavior as v1.0).
   2-3  : interpolate between points; extrapolate with the end segment. */
float applyCal(Pad &p, float net){
  if(p.nPts == 0) return net;
  if(p.nPts == 1){
    if(fabsf(p.ptRaw[0]) < 1.0f) return 0.0f;
    return net * (p.ptLbs[0] / p.ptRaw[0]);
  }
  if(net <= p.ptRaw[0]){
    if(fabsf(p.ptRaw[0]) < 1.0f) return 0.0f;
    return net * (p.ptLbs[0] / p.ptRaw[0]);   // through origin below 1st pt
  }
  for(int i = 0; i < p.nPts - 1; i++){
    if(net <= p.ptRaw[i+1]){
      float dr = p.ptRaw[i+1] - p.ptRaw[i];
      if(fabsf(dr) < 1.0f) return p.ptLbs[i];
      float t = (net - p.ptRaw[i]) / dr;
      return p.ptLbs[i] + t * (p.ptLbs[i+1] - p.ptLbs[i]);
    }
  }
  int i = p.nPts - 2;                          // extrapolate past last pt
  float dr = p.ptRaw[i+1] - p.ptRaw[i];
  if(fabsf(dr) < 1.0f) return p.ptLbs[p.nPts-1];
  float slope = (p.ptLbs[i+1] - p.ptLbs[i]) / dr;
  return p.ptLbs[p.nPts-1] + (net - p.ptRaw[p.nPts-1]) * slope;
}

/* Trimmed mean of the raw window: sort, drop the top/bottom 25%. */
float trimmedRawMean(Pad &p){
  int n = p.count;
  if(n == 0) return 0;
  float buf[RING_N];  /* automatic (stack) — safe per-call, no shared-static hazard */
  for(int i = 0; i < n; i++) buf[i] = p.ring[i];
  // insertion sort — n <= 32
  for(int i = 1; i < n; i++){
    float k = buf[i]; int j = i - 1;
    while(j >= 0 && buf[j] > k){ buf[j+1] = buf[j]; j--; }
    buf[j+1] = k;
  }
  int lo = n / 4, hi = n - n / 4;
  if(hi <= lo){ lo = 0; hi = n; }
  double sum = 0;
  for(int i = lo; i < hi; i++) sum += buf[i];
  return (float)(sum / (hi - lo));
}

/* Recompute calibrated value, window stddev (in lbs), and lock state. */
void updatePadStats(Pad &p){
  if(p.count == 0){ p.value = 0; p.sd = 999; p.locked = false; return; }

  float meanRaw = trimmedRawMean(p);
  p.value = applyCal(p, meanRaw - p.offset) / p.gain;

  // stddev of calibrated window (untrimmed — we want to SEE the noise)
  double s = 0, s2 = 0;
  for(int i = 0; i < p.count; i++){
    float v = applyCal(p, p.ring[i] - p.offset) / p.gain;
    s += v; s2 += (double)v * v;
  }
  double m = s / p.count;
  double var = s2 / p.count - m * m;
  p.sd = var > 0 ? sqrtf((float)var) : 0;

  if(!p.locked){
    if(p.count >= CAL_SAMPLES && p.sd < LOCK_SD){
      p.locked = true;
      p.held = p.value;
    }
  } else {
    if(fabsf(p.value - p.held) > UNLOCK_DELTA) p.locked = false;
  }
}

float padDisplay(Pad &p){
  return p.locked ? p.held : p.value;
}

void pushSample(Pad &p, float raw){
  p.ring[p.head] = raw;
  p.head = (p.head + 1) % RING_N;
  if(p.count < RING_N) p.count++;
  p.lastSeen = millis();
  updatePadStats(p);
}

bool padOnline(Pad &p){
  /* FL (local) uses the same freshness criterion as remote pads.
     scalePresent gates hardware reads but must not keep FL online
     forever — use the same lastSeen window so a disconnected local
     HX711 goes offline after ~3 s just like a silent remote pad.  */
  return (p.count > 0) && ((millis() - p.lastSeen) < 3000UL);
}

/* ---------- NVS ---------- */

void savePad(int i){
  Pad &p = pads[i];
  char key[16];
  snprintf(key, sizeof(key), "%s_off", p.id); prefs.putFloat(key, p.offset);
  snprintf(key, sizeof(key), "%s_n",   p.id); prefs.putInt(key, p.nPts);
  snprintf(key, sizeof(key), "%s_g",   p.id); prefs.putFloat(key, p.gain);
  for(int j = 0; j < MAX_PTS; j++){
    snprintf(key, sizeof(key), "%s_r%d", p.id, j); prefs.putFloat(key, p.ptRaw[j]);
    snprintf(key, sizeof(key), "%s_w%d", p.id, j); prefs.putFloat(key, p.ptLbs[j]);
  }
}

void loadPad(int i){
  Pad &p = pads[i];
  char key[16];
  snprintf(key, sizeof(key), "%s_off", p.id); p.offset = prefs.getFloat(key, 0);
  snprintf(key, sizeof(key), "%s_n",   p.id); p.nPts   = prefs.getInt(key, 0);
  snprintf(key, sizeof(key), "%s_g",   p.id); p.gain   = prefs.getFloat(key, 1.0f);
  if(!(p.gain > 0.8f && p.gain < 1.25f)) p.gain = 1.0f;  // corrupt/insane trim guard
  if(p.nPts < 0 || p.nPts > MAX_PTS) p.nPts = 0;
  for(int j = 0; j < MAX_PTS; j++){
    snprintf(key, sizeof(key), "%s_r%d", p.id, j); p.ptRaw[j] = prefs.getFloat(key, 0);
    snprintf(key, sizeof(key), "%s_w%d", p.id, j); p.ptLbs[j] = prefs.getFloat(key, 0);
  }
}

/* Insert a cal point keeping ptRaw ascending. If full, replace nearest. */
void addCalPoint(Pad &p, float netRaw, float lbs){
  if(p.nPts < MAX_PTS){
    int pos = p.nPts;
    while(pos > 0 && p.ptRaw[pos-1] > netRaw){
      p.ptRaw[pos] = p.ptRaw[pos-1];
      p.ptLbs[pos] = p.ptLbs[pos-1];
      pos--;
    }
    p.ptRaw[pos] = netRaw;
    p.ptLbs[pos] = lbs;
    p.nPts++;
  } else {
    int best = 0; float bd = fabsf(p.ptRaw[0] - netRaw);
    for(int j = 1; j < MAX_PTS; j++){
      float d = fabsf(p.ptRaw[j] - netRaw);
      if(d < bd){ bd = d; best = j; }
    }
    p.ptRaw[best] = netRaw;
    p.ptLbs[best] = lbs;
    // re-sort (tiny)
    for(int a = 1; a < p.nPts; a++){
      float kr = p.ptRaw[a], kw = p.ptLbs[a]; int b = a - 1;
      while(b >= 0 && p.ptRaw[b] > kr){
        p.ptRaw[b+1] = p.ptRaw[b]; p.ptLbs[b+1] = p.ptLbs[b]; b--;
      }
      p.ptRaw[b+1] = kr; p.ptLbs[b+1] = kw;
    }
  }
}

/* ---------- HTTP handlers ---------- */

void handleData(){
  float w[4];
  bool online[4];
  for(int i = 0; i < 4; i++){
    online[i] = padOnline(pads[i]);
    if(!online[i]) pads[i].locked = false;
    float v = online[i] ? sanitizeFloat(padDisplay(pads[i])) : 0;
    if(v < 0) v = 0;
    w[i] = v;                       // FULL precision — no rounding here
  }

  float total = w[0] + w[1] + w[2] + w[3];
  float front = w[P_FL] + w[P_FR];
  float rear  = w[P_RL] + w[P_RR];
  float left  = w[P_FL] + w[P_RL];
  float right = w[P_FR] + w[P_RR];
  float cross = w[P_FR] + w[P_RL];  // RF+LR — standard cross weight

  float frontpct=0, rearpct=0, leftpct=0, rightpct=0, crosspct=0;
  if(total > 0){
    frontpct = front / total * 100;
    rearpct  = rear  / total * 100;
    leftpct  = left  / total * 100;
    rightpct = right / total * 100;
    crosspct = cross / total * 100;
  }

  /* Single snprintf into a static buffer — the old ~30-concat String
     build fragments the heap at 5 req/s over a multi-hour session and
     is the classic slow-death mode for this WebServer pattern.        */
  static char buf[1024];
  int n = snprintf(buf, sizeof(buf),
    "{\"fl\":%.1f,\"fr\":%.1f,\"rl\":%.1f,\"rr\":%.1f,"
    "\"fl_online\":%s,\"fr_online\":%s,\"rl_online\":%s,\"rr_online\":%s,"
    "\"fl_locked\":%s,\"fr_locked\":%s,\"rl_locked\":%s,\"rr_locked\":%s,"
    "\"fl_batt\":%d,\"fr_batt\":%d,\"rl_batt\":%d,\"rr_batt\":%d,"
    "\"total\":%.1f,\"front\":%.1f,\"rear\":%.1f,\"left\":%.1f,\"right\":%.1f,"
    "\"frontpct\":%.1f,\"rearpct\":%.1f,\"leftpct\":%.1f,\"rightpct\":%.1f,"
    "\"cross\":%.1f}",
    w[P_FL], w[P_FR], w[P_RL], w[P_RR],
    online[P_FL] ? "true" : "false", online[P_FR] ? "true" : "false",
    online[P_RL] ? "true" : "false", online[P_RR] ? "true" : "false",
    pads[P_FL].locked ? "true" : "false", pads[P_FR].locked ? "true" : "false",
    pads[P_RL].locked ? "true" : "false", pads[P_RR].locked ? "true" : "false",
    battPercent(pads[P_FL].batt), battPercent(pads[P_FR].batt),
    battPercent(pads[P_RL].batt), battPercent(pads[P_RR].batt),
    total, front, rear, left, right,
    frontpct, rearpct, leftpct, rightpct,
    crosspct);

  if(n < 0 || n >= (int)sizeof(buf)){
    server.send(500, "text/plain", "JSON OVERFLOW");
    return;
  }
  server.send(200, "application/json", buf);
}

void handleTare(){
  String out = "TARE:";
  for(int i = 0; i < 4; i++){
    Pad &p = pads[i];
    if(padOnline(p) && p.count >= MIN_SAMPLES){
      p.offset = trimmedRawMean(p);
      p.locked = false;
      savePad(i);
      updatePadStats(p);
      out += String(" ") + p.id;
    }
  }
  server.send(200, "text/plain", out);
}

void handleCalibrate(){
  String padArg = server.arg("pad");
  float known = server.arg("weight").toFloat();

  int idx = -1;
  for(int i = 0; i < 4; i++) if(padArg == pads[i].id) idx = i;
  if(idx < 0){ server.send(400, "text/plain", "INVALID PAD"); return; }

  Pad &p = pads[idx];

  /* weight 0 => clear this pad's calibration points */
  if(fabsf(known) < 0.001f){
    p.nPts = 0;
    savePad(idx);
    updatePadStats(p);
    server.send(200, "text/plain", "CAL CLEARED");
    return;
  }

  if(!padOnline(p)){
    server.send(400, "text/plain", padArg + " OFFLINE");
    return;
  }
  if(p.count < CAL_SAMPLES){
    server.send(400, "text/plain", "NOT ENOUGH SAMPLES - WAIT");
    return;
  }
  /* Gate on settled reading. If uncalibrated (nPts==0) sd is in raw
     counts and the lbs threshold is meaningless — allow first point,
     but require settling once a scale factor exists. */
  if(p.nPts > 0 && p.sd > LOCK_SD * 2){
    server.send(400, "text/plain", "UNSTABLE - WAIT FOR LOCK");
    return;
  }

  float net = trimmedRawMean(p) - p.offset;
  if(fabsf(net) < 100.0f){   // essentially no load on the pad
    server.send(400, "text/plain", "NO LOAD DETECTED - TARE FIRST?");
    return;
  }

  addCalPoint(p, net, known);
  savePad(idx);
  p.locked = false;
  updatePadStats(p);
  server.send(200, "text/plain", "CAL OK (" + String(p.nPts) + " pts)");
}

/* ---------- rotation calibration ----------
   The car itself is the transfer standard: with fixed floor stations,
   physically rotate the pads between placements and re-lower the car.
   Corner weights w[s] are constant across placements (same fuel, same
   driver/ballast, same tire spots on the pads); per-pad gain errors
   g[p] are what we solve for:  meas[k][s] = g[padAt[k][s]] * w[s].
   Solved by alternating least squares in ~20 iterations; g normalized
   to mean 1 and stored as a trim on top of the piecewise cal. */

#define ROT_MAX 4
float rotMeas[ROT_MAX][4];
int   rotPadAt[ROT_MAX][4];
int   rotCount = 0;

int padIndexById(const String &s){
  for(int i = 0; i < 4; i++) if(s == pads[i].id) return i;
  return -1;
}

/* value with piecewise cal but WITHOUT gain trim (solver input) */
float padUntrimmed(Pad &p){
  return applyCal(p, trimmedRawMean(p) - p.offset);
}

void handleRotCal(){
  String cmd = server.arg("cmd");

  if(cmd == "start"){
    rotCount = 0;
    server.send(200, "text/plain", "ROTCAL STARTED - place car, wait for 4 locks, then record");
    return;
  }

  if(cmd == "clear"){
    rotCount = 0;
    for(int i = 0; i < 4; i++){ pads[i].gain = 1.0f; savePad(i); updatePadStats(pads[i]); }
    server.send(200, "text/plain", "GAIN TRIMS CLEARED");
    return;
  }

  if(cmd == "status"){
    String out = "placements: " + String(rotCount) + " | trims:";
    for(int i = 0; i < 4; i++)
      out += String(" ") + pads[i].id + "=" + String((pads[i].gain - 1.0f) * 100, 2) + "%";
    server.send(200, "text/plain", out);
    return;
  }

  if(cmd == "record"){
    if(rotCount >= ROT_MAX){
      server.send(400, "text/plain", "SESSION FULL - solve or start over");
      return;
    }
    /* map = pad located at each STATION, order FL,FR,RL,RR */
    String map = server.arg("map");
    int p0 = 0, idx = 0, at[4] = {-1,-1,-1,-1};
    for(int s = 0; s < 4 && idx >= 0; s++){
      int comma = map.indexOf(',', p0);
      String tok = (comma < 0) ? map.substring(p0) : map.substring(p0, comma);
      tok.trim();
      at[s] = padIndexById(tok);
      if(at[s] < 0) idx = -1;
      p0 = comma + 1;
    }
    bool used[4] = {false,false,false,false};
    if(idx >= 0) for(int s = 0; s < 4; s++){
      if(at[s] < 0 || used[at[s]]) idx = -1; else used[at[s]] = true;
    }
    if(idx < 0){
      server.send(400, "text/plain", "BAD MAP - need e.g. RL,FL,RR,FR (each pad once)");
      return;
    }
    for(int s = 0; s < 4; s++){
      Pad &p = pads[at[s]];
      if(!padOnline(p) || !p.locked){
        server.send(400, "text/plain", String(p.id) + " NOT LOCKED - settle first");
        return;
      }
    }
    for(int s = 0; s < 4; s++)
      rotMeas[rotCount][s] = padUntrimmed(pads[at[s]]);
    for(int s = 0; s < 4; s++) rotPadAt[rotCount][s] = at[s];
    rotCount++;

    /* consistency check: total should be invariant across placements */
    String out = "RECORDED " + String(rotCount) + "/" + String(ROT_MAX);
    if(rotCount > 1){
      float t0 = 0, tk = 0;
      for(int s = 0; s < 4; s++){ t0 += rotMeas[0][s]; tk += rotMeas[rotCount-1][s]; }
      out += " | total delta vs first: " + String(tk - t0, 1) + " lb";
    }
    server.send(200, "text/plain", out);
    return;
  }

  if(cmd == "solve"){
    if(rotCount < 2){
      server.send(400, "text/plain", "NEED AT LEAST 2 PLACEMENTS");
      return;
    }
    float g[4] = {1,1,1,1};
    float w[4] = {0,0,0,0};
    for(int s = 0; s < 4; s++){
      for(int k = 0; k < rotCount; k++) w[s] += rotMeas[k][s];
      w[s] /= rotCount;
      if(w[s] < 1.0f){ server.send(400, "text/plain", "DEGENERATE DATA"); return; }
    }
    for(int it = 0; it < 25; it++){
      float gs[4] = {0,0,0,0}; int gn[4] = {0,0,0,0};
      for(int k = 0; k < rotCount; k++)
        for(int s = 0; s < 4; s++){
          int p = rotPadAt[k][s];
          gs[p] += rotMeas[k][s] / w[s];
          gn[p]++;
        }
      float gm = 0;
      for(int p = 0; p < 4; p++){ g[p] = gn[p] ? gs[p] / gn[p] : 1.0f; gm += g[p]; }
      gm /= 4;
      for(int p = 0; p < 4; p++) g[p] /= gm;   // normalize: mean gain = 1
      for(int s = 0; s < 4; s++){
        float ws = 0;
        for(int k = 0; k < rotCount; k++) ws += rotMeas[k][s] / g[rotPadAt[k][s]];
        w[s] = ws / rotCount;
      }
    }
    float maxRes = 0;
    for(int k = 0; k < rotCount; k++)
      for(int s = 0; s < 4; s++){
        float r = fabsf(rotMeas[k][s] - g[rotPadAt[k][s]] * w[s]);
        if(r > maxRes) maxRes = r;
      }
    for(int p = 0; p < 4; p++){
      if(!(g[p] > 0.8f && g[p] < 1.25f)){
        server.send(400, "text/plain", "SOLVE REJECTED - trim out of range, check data");
        return;
      }
    }
    String out = "TRIMS:";
    for(int p = 0; p < 4; p++){
      pads[p].gain = g[p];
      savePad(p);
      pads[p].locked = false;
      updatePadStats(pads[p]);
      out += String(" ") + pads[p].id + "=" + String((g[p] - 1.0f) * 100, 2) + "%";
    }
    out += " | max residual " + String(maxRes, 1)
         + " lb (this is your real repeatability floor)";
    rotCount = 0;
    server.send(200, "text/plain", out);
    return;
  }

  server.send(400, "text/plain", "cmd = start | record | solve | status | clear");
}

/* Extended snapshot for setup records / offline logging.
   Everything needed to reconstruct the weigh: corner values, noise,
   lock state, cal points, trims, offsets. Client downloads as JSON. */
void handleSnapshot(){
  float w[4];
  bool online[4];
  for(int i = 0; i < 4; i++){
    online[i] = padOnline(pads[i]);
    if(!online[i]) pads[i].locked = false;   // never record a stale lock
    float v = online[i] ? sanitizeFloat(padDisplay(pads[i])) : 0;
    w[i] = v < 0 ? 0 : v;
  }
  float total = w[0]+w[1]+w[2]+w[3];
  float cross = w[P_FR] + w[P_RL];

  String j = "{\"uptime_ms\":" + String(millis());
  j += ",\"total\":" + String(total, 2);
  j += ",\"cross_pct\":" + String(total > 0 ? cross/total*100 : 0, 3);
  j += ",\"front_pct\":" + String(total > 0 ? (w[P_FL]+w[P_FR])/total*100 : 0, 3);
  j += ",\"left_pct\":"  + String(total > 0 ? (w[P_FL]+w[P_RL])/total*100 : 0, 3);
  j += ",\"pads\":[";
  for(int i = 0; i < 4; i++){
    Pad &p = pads[i];
    if(i) j += ",";
    j += "{\"id\":\"" + String(p.id) + "\"";
    j += ",\"lbs\":" + String(w[i], 2);
    j += ",\"sd\":" + String(p.sd, 3);
    j += ",\"locked\":" + String(p.locked ? "true":"false");
    j += ",\"online\":" + String(online[i] ? "true":"false");
    j += ",\"gain_trim_pct\":" + String((p.gain - 1.0f) * 100, 3);
    j += ",\"offset_raw\":" + String(p.offset, 0);
    j += ",\"batt_v\":" + String(p.batt, 2);
    if(!p.local){
      /* link quality — loss derived from the packet seq field */
      j += ",\"rx\":" + String(p.rxCount);
      j += ",\"lost\":" + String(p.lossCount);
    }
    j += ",\"cal_pts\":[";
    for(int k = 0; k < p.nPts; k++){
      if(k) j += ",";
      j += "{\"raw\":" + String(p.ptRaw[k], 0) + ",\"lbs\":" + String(p.ptLbs[k], 1) + "}";
    }
    j += "]}";
  }
  j += "]}";
  server.send(200, "application/json", j);
}

void handleRoot(){

  String html = R"rawliteral(

  <!DOCTYPE html>
  <html>
  <head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">

  <style>

  body{
    margin:0;
    font-family:Arial;
    color:white;
    text-align:center;

  background:
    repeating-linear-gradient(
      45deg,
      #111,
      #111 6px,
      #0c0c0c 6px,
      #0c0c0c 12px
    );
  }

  .header{
    background:#16a34a;
    padding:8px;
    font-weight:bold;
  }

  .cararea{
    position:relative;
    width:90vw;
    max-width:420px;
    aspect-ratio: 8 / 7;
    margin:auto;
  }

  .vline{
    position:absolute;
    top:0;
    bottom:0;
    left:50%;
    width:2px;
    background:#16a34a;
  }

  .hline{
    position:absolute;
    left:0;
    right:0;
    top:50%;
    height:2px;
    background:#16a34a;
  }

  .weight{
    padding-top:8px;
    display:flex;
    flex-direction:column;
    align-items:center;
    justify-content:center;
    position:absolute;
    padding-bottom:8px;
  }

  .big{
    font-size:28px;
    font-weight:bold;
    font-family: 'Roboto Mono', monospace;
    font-variant-numeric: tabular-nums;
    letter-spacing:1px;
    text-align:right;
    display:block;
    margin-bottom:4px;
    width:80px;
  }

  .small{
    font-size:14px;
    color:#22c55e;
    display:flex;
    flex-direction:column;
    align-items:center;
    justify-content:center;
    line-height:1.2;
    gap:2px;
  }

  .fl{ top:10px; left:-1%; }
  .fr{ top:10px; right:5%; }
  .rl{ bottom:10px; left:-1%; }
  .rr{ bottom:10px; right:5%; }

  .car{
    position:absolute;
    top:50%;
    left:50%;
    transform:translate(-50%,-50%);
  }

  .top{
    display:flex;
    align-items:center;
    gap:4px;
    font-size:14px;
    color:#22c55e;
    min-width:40px;
    width:auto;
  }

  .value{
    display:flex;
    align-items:center;
    justify-content:center;
    gap:6px;
    min-height:34px;
    position:relative;
    margin-top: 35px;
  }

  .panel{
    margin:10px;
    border:2px solid #16a34a;
    border-radius:10px;
    padding:12px;
    background:#0e0e0e;
  }

  .stats{
    width:100%;
    font-size:18px;
    border-collapse:collapse;
    table-layout:fixed;
  }

  .stats th{
    font-size:14px;
    opacity:.8;
    padding-bottom:6px;
  }

  .stats td{
    padding:6px 4px;
  }

  .stats th:nth-child(1),
  .stats td:nth-child(1){
    width:30%;
  }

  .stats th:nth-child(2),
  .stats td:nth-child(2){
    width:35%;
  }

  .stats th:nth-child(3),
  .stats td:nth-child(3){
    width:35%;
  }

  #frontpct,
  #rearpct,
  #leftpct,
  #rightpct,
  #crosspct{
    font-variant-numeric: tabular-nums;
    text-align:right;
    display:inline-block;
    width:60px;
  }

  .val{
    color:#22c55e;
    font-weight:bold;
    font-variant-numeric: tabular-nums;
    text-align:right;
  }

  .totalrow td{
    font-size:20px;
    font-weight:bold;
    border-top:1px solid #333;
    padding-top:10px;
  }

  .buttons{
    margin-top:10px;
    display:flex;
    justify-content:center;
    gap:12px;
  }

  button{
    font-size:18px;
    padding:12px 24px;
    border-radius:8px;
    border:none;
    cursor:pointer;
  }

  .zero{
    background:#16a34a;
    color:white;
  }

  .cal{
    background:#333;
    color:white;
  }

  .status{
    display:inline-block;
    width:10px;
    height:10px;
    border-radius:50%;
    margin-left:5px;
    background:#444;
  }

  .online{
    background:#22c55e;
  }

  .offline{
    background:#ef4444;
  }

  .lock{
    width:18px;
    display:inline-block;
    text-align:left;
    opacity:0.3;
    line-height:1;
    font-size:14px;
  }

  .locked{
    opacity:1;
    color:#22c55e;
  }
  .digits{
    display:flex;
    justify-content:center;
    align-items:center;
    gap:2px;
    height:32px;
  }

  .digit{
    width:16px;
    text-align:center;
    font-size:24px;
    font-weight:bold;
    font-family: 'Roboto Mono', monospace;
  }

  .decimal{
    width:16px;
  }

  .fl .top,
  .rl .top{
    left:6px;
    right:auto;
    justify-content:flex-start;
    flex-direction:row-reverse;
  }

  .fr .top,
  .rr .top{
    right:6px;
    left:auto;
    justify-content:flex-end;
  }

  .fl .top,
  .fr .top{
    position:absolute;
    top:4px;
  }

  .rl .top,
  .rr .top{
    position:absolute;
    bottom:4px;
    top:auto;
  }

  .rl .value,
  .rr .value{
    margin-top:0px;
    margin-bottom:35px;
  }

  </style>

  </head>

  <body>

  <div class="header">
  DIY Corner Weight System
  </div>

  <div class="cararea">

  <div class="vline"></div>
  <div class="hline"></div>

  <div class="weight fl">
    <div class="top">
      <span>FL</span>
      <span class="status" id="fl_status"></span>
    </div>
    <div class="value">
      <div class="digits" id="fl"></div>
      <span id="fl_lock" class="lock"></span>
    </div>
    <span id="fl_batt"></span>
  </div>

  <div class="weight fr">
    <div class="top">
      <span>FR</span>
      <span class="status" id="fr_status"></span>
      </div>
      <div class="value">
        <div class="digits" id="fr"></div>
        <span id="fr_lock" class="lock"></span>
      </div>
      <span id="fr_batt"></span>
  </div>

  <div class="weight rl">
    <div class="top">
      <span>RL</span>
      <span class="status" id="rl_status"></span>
      </div>
      <div class="value">
        <div class="digits" id="rl"></div>
        <span id="rl_lock" class="lock"></span>
    </div>
    <span id="rl_batt"></span>
  </div>

  <div class="weight rr">
    <div class="top">
      <span>RR</span>
      <span class="status" id="rr_status"></span>
      </div>
      <div class="value">
        <div class="digits" id="rr"></div>
        <span id="rr_lock" class="lock"></span>
    </div>
    <span id="rr_batt"></span>
  </div>

  <div class="car">

  <svg width="120" height="220" viewBox="0 0 220 300">

  <path fill="#16a34a" stroke="#111" stroke-width="3" d="
  M85 10
  L135 10
  Q155 20 160 50
  L165 100
  L165 200
  L160 250
  Q155 280 135 290
  L85 290
  Q65 280 60 250
  L55 200
  L55 100
  L60 50
  Q65 20 85 10
  Z"/>

  <rect x="75" y="55" width="70" height="40" rx="10" fill="#2f2f2f"/>
  <rect x="75" y="215" width="70" height="40" rx="10" fill="#2f2f2f"/>

  <polygon points="55,120 45,130 55,140" fill="#16a34a"/>
  <polygon points="165,120 175,130 165,140" fill="#16a34a"/>

  </svg>

  </div>

  </div>

  <div class="panel">
    <table class="stats">
    <thead>
      <tr>
        <th></th>
        <th>Current</th>
        <th>%</th>
      </tr>
    </thead>
    <tbody>
      <tr>
        <td>Cross</td>
        <td>-</td>
        <td id="crosspct"></td>
      </tr>
      <tr>
        <td>Left</td>
        <td class="val" id="left"></td>
        <td id="leftpct"></td>
      </tr>
      <tr>
        <td>Right</td>
        <td class="val" id="right"></td>
        <td id="rightpct"></td>
      </tr>
      <tr>
        <td>Front</td>
        <td class="val" id="front"></td>
        <td id="frontpct"></td>
      </tr>
      <tr>
      <td>Rear</td>
        <td class="val" id="rear"></td>
        <td id="rearpct"></td>
      </tr>
      <tr class="totalrow">
        <td>Total</td>
        <td class="val" id="total"></td>
      <td></td>
      </tr>
    </tbody>
    </table>
  </div>
  <div class="buttons">
    <button class="zero" onclick="tare()">ZERO</button>
    <button class="cal" onclick="calibrate()">CAL</button>
    <button class="cal" onclick="rotcal()">ROT</button>
    <button class="cal" onclick="saveSnap()">SAVE</button>
    <button onclick="toggleUnits()" id="unitBtn">LBS</button>
  </div>
  <script>
  let useKg = false;

  function calibrate(){

    let pad = prompt("Pad (FL FR RL RR)");
    if(!pad) return;
    pad = pad.trim().toUpperCase();

    let weight = parseFloat(prompt(
      "Known weight (" + (useKg ? "KG" : "LBS") + ")\n" +
      "Up to 3 points per pad - use 2 loads bracketing your corner weights.\n" +
      "Enter 0 to CLEAR this pad's calibration."
    ));

    if(isNaN(weight)) return;

    if(useKg){
      weight = weight * 2.20462;
    }

    fetch("/calibrate?pad=" + pad + "&weight=" + weight)
      .then(r => r.text())
      .then(t => alert(t));
  }

  function format(val){
    return val.toFixed(1);
  }

  function convert(val){
    let v = useKg ? val * 0.453592 : val;
    return v;
  }

  function toggleUnits(){
    useKg = !useKg;
    document.getElementById("unitBtn").innerText = useKg ? "KG" : "LBS";
  }

  function renderDigits(id, value){
    let el = document.getElementById(id)
    let str = value.toFixed(1)
    str = str.padStart(6, " ")
    el.innerHTML = ""
    for(let c of str){
      let d = document.createElement("span")
      d.className = "digit"
      if(c === "."){
        d.classList.add("decimal")
      }
      d.innerText = c
      el.appendChild(d)
    }
  }

  function refresh(){
    fetch("/data")
    .then(r=>r.json())
    .then(d=>{
      renderDigits("fl", convert(d.fl))
      renderDigits("fr", convert(d.fr))
      renderDigits("rl", convert(d.rl))
      renderDigits("rr", convert(d.rr))

      total.innerText = format(convert(d.total))
      front.innerText = format(convert(d.front))
      rear.innerText = format(convert(d.rear))
      left.innerText = format(convert(d.left))
      right.innerText = format(convert(d.right))
      frontpct.innerText=d.frontpct
      rearpct.innerText=d.rearpct
      leftpct.innerText=d.leftpct
      rightpct.innerText=d.rightpct
      crosspct.innerText=d.cross

      setStatus("fl_status",d.fl_online)
      setStatus("fr_status",d.fr_online)
      setStatus("rl_status",d.rl_online)
      setStatus("rr_status",d.rr_online)

      setLock("fl_lock", d.fl_locked)
      setLock("fr_lock", d.fr_locked)
      setLock("rl_lock", d.rl_locked)
      setLock("rr_lock", d.rr_locked)

    })

  }

  function setStatus(id,online){
    let el=document.getElementById(id)
    if(online){
      el.className="status online"
    } else {
      el.className="status offline"
    }
  }

  function tare(){
    fetch("/tare")
  }

  /* Save a weigh snapshot as a JSON file on the phone/laptop.
     ESP32 keeps no history — the browser is the storage. */
  function saveSnap(){
    let note = prompt("Session note (car / fuel / driver / changes)?", "");
    fetch("/snapshot")
      .then(r => r.json())
      .then(d => {
        d.saved_at = new Date().toISOString();
        d.note = note || "";
        let blob = new Blob([JSON.stringify(d, null, 2)],
                            {type: "application/json"});
        let a = document.createElement("a");
        a.href = URL.createObjectURL(blob);
        a.download = "cornerweights_" +
          new Date().toISOString().replace(/[:.]/g,"-") + ".json";
        a.click();
        URL.revokeObjectURL(a.href);
      });
  }

  /* Rotation cal: pads rotate CW one station per placement while the
     floor stations stay fixed. Defaults below = which PAD sits at each
     STATION (order FL,FR,RL,RR) after k CW steps. */
  const rotDefaults = [
    "FL,FR,RL,RR",
    "RL,FL,RR,FR",
    "RR,RL,FR,FL",
    "FR,RR,FL,RL"
  ];
  let rotStep = 0;

  function rotcal(){
    let a = prompt(
      "ROTATION CAL\n" +
      "1=start session  2=record placement  3=solve  4=status  5=clear trims\n" +
      "Procedure: lift car, place/rotate pads, lower, bounce corners,\n" +
      "wait for all 4 locks, then record. Same fuel + driver every time."
    );
    if(!a) return;

    if(a === "1"){
      rotStep = 0;
      fetch("/rotcal?cmd=start").then(r=>r.text()).then(t=>alert(t));
    } else if(a === "2"){
      let def = rotDefaults[rotStep % 4];
      let map = prompt(
        "Which PAD is at each STATION?\n" +
        "Order: FL,FR,RL,RR stations.\n" +
        "(Default = " + (rotStep) + " CW rotations)", def
      );
      if(!map) return;
      fetch("/rotcal?cmd=record&map=" + encodeURIComponent(map.toUpperCase()))
        .then(r=>r.text())
        .then(t=>{ if(t.startsWith("RECORDED")) rotStep++; alert(t); });
    } else if(a === "3"){
      fetch("/rotcal?cmd=solve").then(r=>r.text()).then(t=>alert(t));
    } else if(a === "4"){
      fetch("/rotcal?cmd=status").then(r=>r.text()).then(t=>alert(t));
    } else if(a === "5"){
      fetch("/rotcal?cmd=clear").then(r=>r.text()).then(t=>alert(t));
    }
  }

  function setLock(id, locked){
    let el = document.getElementById(id)

    if(locked){
      el.innerText = "🔒"
      el.className = "lock locked"
    } else {
      el.innerText = ""
      el.className = "lock"
    }
  }

  setInterval(refresh,200)

  refresh()

  </script>

  </body>
  </html>

  )rawliteral";

  server.send(200,"text/html",html);
}

/* ---------- ESP-NOW receive ----------
   Callback runs in the WiFi task context — NOT a hardware ISR.
   Contract (queue discipline):
     - Validate the incoming bytes with scalePacketValid().
     - Copy the validated packet into a local ScalePacket.
     - Post to rxQueue with xQueueSend (0 ticks = nonblocking); drop
       silently if the queue is full.
     - Return.  No pad/ring/stats mutation here whatsoever.
   loop() owns all pad state; it drains rxQueue every iteration.
   xQueueSend (not FromISR) is correct because this is a task context. */

#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
void onReceive(const esp_now_recv_info_t *info, const uint8_t *buf, int len)
#else
void onReceive(const uint8_t *mac, const uint8_t *buf, int len)
#endif
{
  if (!scalePacketValid(buf, len)) return;
  ScalePacket pkt;
  memcpy(&pkt, buf, sizeof(pkt));
  xQueueSend(rxQueue, &pkt, 0);   /* nonblocking; drop if full */
}

/* ---------- setup / loop ---------- */

void setup(){

  Serial.begin(115200);

  pads[P_FL].id = "FL"; pads[P_FL].local = true;
  pads[P_FR].id = "FR";
  pads[P_RL].id = "RL";
  pads[P_RR].id = "RR";

  scale.begin(HX_DT, HX_SCK);
  Serial.println("HX711 optional - will detect automatically");

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("DIY_Race_Scales","12345678",1);
  WiFi.setSleep(false);

  Serial.print("AP MAC: ");
  Serial.println(WiFi.softAPmacAddress());
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  if (esp_now_init() != ESP_OK) {
    Serial.println("FATAL: ESP-NOW init failed — halting");
    while (true) delay(1000);
  }
  /* Create the receive queue BEFORE registering the callback so the
     callback always finds a valid handle when it calls xQueueSend.  */
  rxQueue = xQueueCreate(RX_QUEUE_LEN, sizeof(ScalePacket));
  if (!rxQueue) {
    Serial.println("FATAL: rxQueue alloc failed — halting");
    while (true) delay(1000);
  }
  esp_now_register_recv_cb(onReceive);

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/tare", handleTare);
  server.on("/calibrate", handleCalibrate);
  server.on("/rotcal", handleRotCal);
  server.on("/snapshot", handleSnapshot);
  server.on("/reset", [](){
    /* NOTE: relies on prefs.begin("scales") having run in setup().
       The handler is *registered* before prefs.begin but cannot be
       *invoked* until server.begin() — do not reorder setup() so that
       the server starts before prefs is open.                        */
    prefs.clear();
    for(int i = 0; i < 4; i++){
      Pad &p = pads[i];
      p.offset = 0;
      p.nPts = 0;
      p.gain = 1.0f;
      p.locked = false;
      /* flush the sample window + link stats too — otherwise stale
         calibrated values keep displaying until the ring refills     */
      p.count = 0;
      p.head = 0;
      p.rxCount = 0;
      p.lossCount = 0;
      p.seqInit = false;
      updatePadStats(p);
    }
    server.send(200, "text/plain", "RESET DONE");
  });

  prefs.begin("scales");
  for(int i = 0; i < 4; i++) loadPad(i);

  server.begin();
}

void loop(){

  /* detect local FL scale if plugged in later */
  if(!scaleInitialized && millis() - lastScaleCheck > 1000){
    lastScaleCheck = millis();
    if(scale.is_ready()){
      /* discard warm-up conversions */
      for(int i = 0; i < 4; i++){
        while(!scale.is_ready()) delay(5);
        scale.read();
      }
      scalePresent = true;
      scaleInitialized = true;
    }
  }

  /* read local FL: single sample per HX711 cycle, into its ring */
  if(scalePresent && scale.is_ready()){
    pushSample(pads[P_FL], (float)scale.read());
  }

  /* Drain the ESP-NOW receive queue.
     All pad-state mutation (ring push, battery, stats) happens here in
     loop() — the onReceive callback only validates and enqueues.
     xQueueReceive with 0 ticks is nonblocking; we consume everything
     that arrived since the last iteration then move on.               */
  {
    ScalePacket qpkt;
    while (xQueueReceive(rxQueue, &qpkt, 0) == pdTRUE) {
      int idx = -1;
      switch ((PadId)qpkt.padId) {
        case PAD_FR: idx = P_FR; break;
        case PAD_RL: idx = P_RL; break;
        case PAD_RR: idx = P_RR; break;
        default:     break;
      }
      if (idx >= 0) {
        Pad &pp = pads[idx];
        /* link quality from the per-pad seq counter: any gap > 1 in
           the (mod-256) difference is lost packets. Resets at pad
           reboot look like one large gap — acceptable noise for a
           diagnostic counter.                                        */
        if (pp.seqInit) {
          uint8_t gap = (uint8_t)(qpkt.seq - pp.lastSeq);
          if (gap > 1) pp.lossCount += (uint32_t)(gap - 1);
        }
        pp.lastSeq = qpkt.seq;
        pp.seqInit = true;
        pp.rxCount++;
        pp.batt = sanitizeFloat(qpkt.battery);
        pushSample(pp, sanitizeFloat(qpkt.raw));
      }
    }
  }

  server.handleClient();

  delay(1);
}
