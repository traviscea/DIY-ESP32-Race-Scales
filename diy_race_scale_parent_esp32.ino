/*
traviscea DIY Race Scales – Version 1.0
Copyright (c) 2026 Travis Way
*/


#define ARDUINO_USB_CDC_ON_BOOT 1

#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <Preferences.h>
#include "HX711.h"

Preferences prefs;


//Change to match your "master board"
#define HX_DT_A 4
#define HX_SCK_A 5
#define HX_DT_B 16
#define HX_SCK_B 17
#define HX_DT_C 18
#define HX_SCK_C 19
#define FL_BAT_PIN 34

typedef struct {
  char pad[4];
  float weight;
  float wA;
  float wB;
  float wC;
  float battery;
} ScaleData;

ScaleData incomingData;

bool scalePresent = false;
bool scaleInitialized = false;
unsigned long lastScaleCheck = 0;

unsigned long FR_lastSeen = 0;
unsigned long RL_lastSeen = 0;
unsigned long RR_lastSeen = 0;

float FL_batt=0;
float FR_batt=0;
float RL_batt=0;
float RR_batt=0;

HX711 scaleA;
HX711 scaleB;
HX711 scaleC;

float FL_wA=0, FL_wB=0, FL_wC=0;
float FR_wA=0, FR_wB=0, FR_wC=0;
float RL_wA=0, RL_wB=0, RL_wC=0;
float RR_wA=0, RR_wB=0, RR_wC=0;

float FL_cal = 1.0;
float FR_cal = 1.0;
float RL_cal = 1.0;
float RR_cal = 1.0;

float FR_raw = 0;
float RL_raw = 0;
float RR_raw = 0;

float FL_offset = 0;
float FR_offset = 0;
float RL_offset = 0;
float RR_offset = 0;

//smoothing
float FL_filtered = 0;
float FR_filtered = 0;
float RL_filtered = 0;
float RR_filtered = 0;

const float alpha = 0.8; 

float lastFL = 0, lastFR = 0, lastRL = 0, lastRR = 0;

unsigned long stableTimeFL = 0;
unsigned long stableTimeFR = 0;
unsigned long stableTimeRL = 0;
unsigned long stableTimeRR = 0;

bool FL_locked = false;
bool FR_locked = false;
bool RL_locked = false;
bool RR_locked = false;

/* tuning */
const float stabilityThreshold = 0.8;   // lbs change allowed
const int stabilityTime = 1000;         // ms to lock


WebServer server(80);

float FL=0;
float FR=0;
float RL=0;
float RR=0;

bool shouldRestart = false;
unsigned long restartTime = 0;
int hxCount = 3;

// ---------- DYNAMIC MAC & ACK TRACKING ----------
uint8_t FR_mac[6] = {0};
uint8_t RL_mac[6] = {0};
uint8_t RR_mac[6] = {0};
bool FR_mac_set = false;
bool RL_mac_set = false;
bool RR_mac_set = false;

bool is_updating = false;
bool FR_ack_required = false;
bool RL_ack_required = false;
bool RR_ack_required = false;

bool FR_ack_received = false;
bool RL_ack_received = false;
bool RR_ack_received = false;

int battPercent(float v){
  int p = (v - 3.0) * 100 / 1.2;
  if(p>100) p=100;
  if(p<0) p=0;
  return p;
}

void applyStability(float value, float &lastValue, bool &locked) {
  float diff = abs(value - lastValue);
  if (diff < 0.5) {
    locked = true;
  } else if (diff > 1.0) { // hysteresis (important)
    locked = false;
    lastValue = value;
  }
}

void handleData(){

  bool FR_online = (millis() - FR_lastSeen) < 3000;
  bool RL_online = (millis() - RL_lastSeen) < 3000;
  bool RR_online = (millis() - RR_lastSeen) < 3000;
  bool FL_online = scalePresent;

  // force unlock if offline
  if(!FL_online) FL_locked = false;
  if(!FR_online) FR_locked = false;
  if(!RL_online) RL_locked = false;
  if(!RR_online) RR_locked = false;

  // sanitize values
  auto safe = [](float v){
    if(isnan(v) || v < 0) return 0.0f;
    return v;
  };

  float FL_w = FL_online ? safe(FL) : 0;
  float FR_w = FR_online ? safe(FR) : 0;
  float RL_w = RL_online ? safe(RL) : 0;
  float RR_w = RR_online ? safe(RR) : 0;

  float total = FL_w + FR_w + RL_w + RR_w;

  float front=FL_w+FR_w;
  float rear=RL_w+RR_w;

  float left=FL_w+RL_w;
  float right=FR_w+RR_w;

  float cross=FL_w+RR_w;

  float frontpct=0, rearpct=0, leftpct=0, rightpct=0, crosspct=0;

  if(total > 0){
    frontpct=(front/total)*100;
    rearpct=(rear/total)*100;
    leftpct=(left/total)*100;
    rightpct=(right/total)*100;
    crosspct=(cross/total)*100;
  }

  String json="{";

  json+="\"fl\":"+String(FL_w)+",";
  json+="\"fr\":"+String(FR_w)+",";
  json+="\"rl\":"+String(RL_w)+",";
  json+="\"rr\":"+String(RR_w)+",";

  json += "\"fl_online\":" + String(FL_online ? "true":"false") + ",";
  json += "\"fr_online\":" + String(FR_online ? "true":"false") + ",";
  json += "\"rl_online\":" + String(RL_online ? "true":"false") + ",";
  json += "\"rr_online\":" + String(RR_online ? "true":"false") + ",";

  json += "\"fl_locked\":" + String(FL_locked?"true":"false") + ",";
  json += "\"fr_locked\":" + String(FR_locked?"true":"false") + ",";
  json += "\"rr_locked\":" + String(RR_locked?"true":"false") + ",";
  json += "\"rl_locked\":" + String(RL_locked?"true":"false") + ",";

  json += "\"fl_batt\":" + String(battPercent(FL_batt)) + ",";
  json+="\"fr_batt\":"+String(battPercent(FR_batt))+",";
  json+="\"rl_batt\":"+String(battPercent(RL_batt))+",";
  json+="\"rr_batt\":"+String(battPercent(RR_batt))+",";

  json+="\"total\":"+String(total)+",";

  json+="\"front\":"+String(front)+",";
  json+="\"rear\":"+String(rear)+",";

  json+="\"left\":"+String(left)+",";
  json+="\"right\":"+String(right)+",";

  json+="\"frontpct\":"+String(frontpct,1)+",";
  json+="\"rearpct\":"+String(rearpct,1)+",";
  json+="\"leftpct\":"+String(leftpct,1)+",";
  json+="\"rightpct\":"+String(rightpct,1)+",";

  json+="\"cross\":"+String(crosspct,1)+",";

  json+="\"fl_wa\":"+String(FL_wA)+",\"fl_wb\":"+String(FL_wB)+",\"fl_wc\":"+String(FL_wC)+",";
  json+="\"fr_wa\":"+String(FR_wA)+",\"fr_wb\":"+String(FR_wB)+",\"fr_wc\":"+String(FR_wC)+",";
  json+="\"rl_wa\":"+String(RL_wA)+",\"rl_wb\":"+String(RL_wB)+",\"rl_wc\":"+String(RL_wC)+",";
  json+="\"rr_wa\":"+String(RR_wA)+",\"rr_wb\":"+String(RR_wB)+",\"rr_wc\":"+String(RR_wC)+",";
  json+="\"hx_count\":"+String(hxCount);

  json+="}";

  server.send(200,"application/json",json);
}

void handleTare(){

  if (hxCount == 1) {
    FL_offset = scaleA.read_average(10);
  } else {
    FL_offset = scaleA.read_average(10) + scaleB.read_average(10) + scaleC.read_average(10);
  }
  FR_offset = FR_raw;
  RL_offset = RL_raw;
  RR_offset = RR_raw;


  prefs.putFloat("FL_offset", FL_offset); 

  server.send(200,"text/plain","OK");

}

void handleRoot(){

  String html = R"rawliteral(

  <!DOCTYPE html>
  <html>
  <head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">

  <style>
  :root {
    --bg-main: #111;
    --bg-main-alt: #0c0c0c;
    --bg-panel: #0e0e0e;
    --text-main: white;
    --accent: #3b82f6;
  }
  body.light-theme {
    --bg-main: #f0f0f0;
    --bg-main-alt: #e4e4e4;
    --bg-panel: #ffffff;
    --text-main: #111;
  }

  body{
    margin:0;
    font-family:Arial;
    color:var(--text-main);
    text-align:center;

  background:
    repeating-linear-gradient(
      45deg,
      var(--bg-main),
      var(--bg-main) 6px,
      var(--bg-main-alt) 6px,
      var(--bg-main-alt) 12px
    );
  }

  .header{
    background:var(--accent);
    padding:8px;
    font-weight:bold;
  }

  .cararea{
    position:relative;
    width:90vw;        /* scales with screen width */
    max-width:420px;   /* prevents it getting huge on tablets */
    aspect-ratio: 8 / 7;
    margin:auto;
  }
.battery-panel{
  margin:10px;
  padding:10px;
  border:2px solid var(--accent);
  border-radius:10px;
  background:#0e0e0e;

  display:flex;
  justify-content:space-around;
  align-items:center;
}

.battery-item{
  display:flex;
  flex-direction:column;
  align-items:center;
  gap:4px;
  color:var(--accent);
  font-size:12px;
}

.battery-icon{
  width:34px;
  height:14px;
  border:2px solid var(--accent);
  border-radius:3px;
  position:relative;
  overflow:hidden;
}

.battery-icon::after{
  content:'';
  position:absolute;
  right:-5px;
  top:3px;
  width:3px;
  height:6px;
  background:var(--accent);
  border-radius:1px;
}

.battery-fill{
  height:100%;
  width:100%;
  background:var(--accent);
  transition:width .2s linear;
}

.battery-text{
  font-size:11px;
}


  .vline{
    position:absolute;
    top:0;
    bottom:0;
    left:50%;
    width:2px;
    background:var(--accent);
  }

  .hline{
    position:absolute;
    left:0;
    right:0;
    top:50%;
    height:2px;
    background:var(--accent);
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
    color:var(--accent);
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

  #cgSymbol {
    transition: transform 0.3s cubic-bezier(0.25, 0.8, 0.25, 1), opacity 0.3s ease;
  }

  .top{
    display:flex;
    align-items:center;
    gap:4px;
    font-size:14px;
    color:var(--accent);
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
    border:2px solid var(--accent);
    border-radius:10px;
    padding:12px;
    background:var(--bg-panel);
  }

  /* table */

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
    width:60px;   /* fixed width prevents shift */
  }

  .val{
    color:var(--accent);
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

  /* buttons */

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
    background:var(--accent);
    color:var(--bg-main);
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
    background:var(--accent);
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
    color:var(--accent);
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

  /* RIGHT side */
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

  /* settings modal */
  .modal-overlay{
    position:fixed; top:0; left:0; width:100%; height:100%;
    background:rgba(0,0,0,0.8); display:none; justify-content:center; align-items:center; z-index:99;
  }
  .modal{
    background:var(--bg-panel); border:2px solid var(--accent); border-radius:10px; padding:20px;
    width:90%; max-width:400px; text-align:left; color:var(--text-main);
  }
  .modal h3 { margin-top:0; color:var(--accent); }
  .settings-row { display:flex; justify-content:space-between; align-items:center; margin-bottom:15px; }
  .settings-btn { background:var(--bg-main-alt); color:var(--text-main); border:1px solid var(--accent); padding:8px 16px; border-radius:4px; cursor:pointer;}
  .settings-icon { position:absolute; right:15px; top:5px; cursor:pointer; font-size:20px; }
  
  /* spinner animation */
  @keyframes spin {
    0% { transform: rotate(0deg); }
    100% { transform: rotate(360deg); }
  }
  </style>

  </head>

  <body>

  <div class="header">
  <span data-i18n="title">Corner Weight System</span>
  <div class="settings-icon" onclick="toggleSettings()">⚙️</div>
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
    <defs>
      <!-- Premium metallic body gradient -->
      <linearGradient id="bodyGrad" x1="0%" y1="0%" x2="0%" y2="100%">
        <stop offset="0%" stop-color="#2c2f38"/>
        <stop offset="50%" stop-color="#1c1e24"/>
        <stop offset="100%" stop-color="#121317"/>
      </linearGradient>
      <!-- Glass canopy gradient -->
      <linearGradient id="glassGrad" x1="0%" y1="0%" x2="0%" y2="100%">
        <stop offset="0%" stop-color="#16181f"/>
        <stop offset="100%" stop-color="#0a0b0e"/>
      </linearGradient>
    </defs>

    <!-- Group 1: Simple (Default) -->
    <g id="car_style_default" class="car-style-group" style="display:none;">
      <path fill="var(--accent)" stroke="#111" stroke-width="3" d="
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
      <polygon points="55,120 45,130 55,140" fill="var(--accent)"/>
      <polygon points="165,120 175,130 165,140" fill="var(--accent)"/>
    </g>

    <!-- Group 2: Modern Supercar -->
    <g id="car_style_supercar" class="car-style-group" style="display:none;">
      <!-- Wheels (Tires & Rims) -->
      <rect x="49" y="46" width="12" height="34" rx="3" fill="#121212"/>
      <rect x="52" y="52" width="6" height="22" rx="2" fill="#555"/>
      <rect x="159" y="46" width="12" height="34" rx="3" fill="#121212"/>
      <rect x="162" y="52" width="6" height="22" rx="2" fill="#555"/>
      <rect x="42" y="206" width="16" height="40" rx="4" fill="#121212"/>
      <rect x="45" y="214" width="8" height="24" rx="2" fill="#555"/>
      <rect x="162" y="206" width="16" height="40" rx="4" fill="#121212"/>
      <rect x="167" y="214" width="8" height="24" rx="2" fill="#555"/>

      <!-- Mirrors -->
      <path d="M 68, 102 C 54, 99 52, 92 56, 90 C 60, 89 64, 95 66, 98 Z" fill="var(--accent)" stroke="#111" stroke-width="1"/>
      <path d="M 152, 102 C 166, 99 168, 92 164, 90 C 160, 89 156, 95 154, 98 Z" fill="var(--accent)" stroke="#111" stroke-width="1"/>

      <!-- Body -->
      <path fill="url(#bodyGrad)" stroke="var(--accent)" stroke-width="2.5" d="
        M 110, 20
        C 125,20 145,22 152,30
        C 160,38 165,50 165,65
        C 165,80 160,95 154,105
        C 150,118 148,135 152,160
        C 156,180 172,195 172,225
        C 172,242 166,260 156,272
        C 148,280 135,282 110,282
        C 85,282 72,280 64,272
        C 54,260 48,242 48,225
        C 48,195 64,180 68,160
        C 72,135 70,118 66,105
        C 60,95 55,80 55,65
        C 55,50 60,38 68,30
        C 75,22 95,20 110,20
        Z"/>

      <!-- Hood Vents -->
      <path d="M 92,52 L 98,42 L 100,52 Z" fill="#121212" stroke="#2c2e35" stroke-width="1"/>
      <path d="M 128,52 L 122,42 L 120,52 Z" fill="#121212" stroke="#2c2e35" stroke-width="1"/>

      <!-- LED Headlights -->
      <path d="M 72, 38 L 68, 52 L 72, 60" fill="none" stroke="var(--accent)" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"/>
      <path d="M 148, 38 L 152, 52 L 148, 60" fill="none" stroke="var(--accent)" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"/>

      <!-- Glass Canopy -->
      <path fill="url(#glassGrad)" stroke="#374151" stroke-width="1.5" d="
        M 110, 80
        C 125,80 136,92 136,110
        C 136,135 130,165 124,195
        C 120,215 116,215 110,215
        C 104,215 100,215 96,195
        C 90,165 84,135 84,110
        C 84,92 95,80 110,80
        Z"/>
      <path d="M 90,105 C 105,88 115,88 130,105 C 122,112 112,114 90,105 Z" fill="rgba(255,255,255,0.12)"/>

      <!-- Engine Cover Carbon Accent & Vents -->
      <path d="M 96,195 L 124,195 L 120,245 L 100,245 Z" fill="#141518" stroke="#24262f" stroke-width="1.5"/>
      <line x1="102" y1="205" x2="118" y2="205" stroke="var(--accent)" stroke-width="2" stroke-linecap="round" opacity="0.7"/>
      <line x1="104" y1="218" x2="116" y2="218" stroke="var(--accent)" stroke-width="2" stroke-linecap="round" opacity="0.7"/>
      <line x1="106" y1="230" x2="114" y2="230" stroke="var(--accent)" stroke-width="2" stroke-linecap="round" opacity="0.7"/>

      <!-- Rear LED Lightbar -->
      <path d="M 85, 276 Q 110, 278 135, 276" stroke="#ef4444" stroke-width="2.5" fill="none" stroke-linecap="round"/>

      <!-- Active Aero Wing / GT3 Rear Spoiler -->
      <line x1="94" y1="252" x2="94" y2="268" stroke="#374151" stroke-width="2"/>
      <line x1="126" y1="252" x2="126" y2="268" stroke="#374151" stroke-width="2"/>
      <path d="M 64,266 C 88,270 132,270 156,266" fill="none" stroke="var(--accent)" stroke-width="4.5" stroke-linecap="round"/>
      <line x1="63" y1="260" x2="65" y2="272" stroke="#121212" stroke-width="3" stroke-linecap="round"/>
      <line x1="157" y1="260" x2="155" y2="272" stroke="#121212" stroke-width="3" stroke-linecap="round"/>
    </g>

    <!-- Group 3: Classic Sports Car (Datsun 240Z Style) -->
    <g id="car_style_classic" class="car-style-group" style="display:none;">
      <!-- Tires visible under the wheel arches -->
      <rect x="63" y="55" width="8" height="30" rx="2" fill="#121212" opacity="0.4"/>
      <rect x="149" y="55" width="8" height="30" rx="2" fill="#121212" opacity="0.4"/>
      <rect x="62" y="202" width="10" height="34" rx="3" fill="#121212" opacity="0.4"/>
      <rect x="148" y="202" width="10" height="34" rx="3" fill="#121212" opacity="0.4"/>

      <!-- Fender Mirrors (Classic Japanese Placement) -->
      <!-- Left Fender Mirror -->
      <line x1="69" y1="64" x2="64" y2="60" stroke="var(--accent)" stroke-width="1.5"/>
      <ellipse cx="62" cy="58" rx="3.5" ry="2" fill="var(--accent)" transform="rotate(-20 62 58)"/>
      <!-- Right Fender Mirror -->
      <line x1="151" y1="64" x2="156" y2="60" stroke="var(--accent)" stroke-width="1.5"/>
      <ellipse cx="158" cy="58" rx="3.5" ry="2" fill="var(--accent)" transform="rotate(20 158 58)"/>

      <!-- Main Body Contour -->
      <path fill="var(--bg-panel)" stroke="var(--accent)" stroke-width="2.5" d="
        M 110, 25
        C 125,25 142,28 146,35
        C 150,42 153,52 153,65
        C 153,90 151,90 151,90
        C 150,110 150,140 150,170
        C 150,170 154,200 154,215
        C 154,230 152,245 152,245
        C 152,260 148,270 144,274
        C 134,275 120,275 110,275
        C 100,275 86,275 76,274
        C 72,270 68,260 68,245
        C 68,245 66,230 66,215
        C 66,200 70,170 70,170
        C 70,140 70,110 70,90
        C 70,90 67,90 67,65
        C 67,52 70,42 74,35
        C 78,28 95,25 110,25
        Z"/>

      <!-- Internal Detail Lines (CAD/Blueprint Style) -->
      <g stroke="var(--text-main)" stroke-width="1.2" fill="none" opacity="0.45" stroke-linecap="round" stroke-linejoin="round">
        <!-- Windshield -->
        <path d="M 76, 115 C 110, 110 110, 110 144, 115"/>
        <path d="M 80, 138 C 110, 134 110, 134 140, 138"/>
        <line x1="76" y1="115" x2="80" y2="138"/>
        <line x1="144" y1="115" x2="140" y2="138"/>

        <!-- Windshield Wipers -->
        <line x1="86" y1="114" x2="98" y2="111" stroke-width="1"/>
        <line x1="112" y1="113" x2="124" y2="110" stroke-width="1"/>

        <!-- Side Glass & Roof rails -->
        <path d="M 80, 138 C 82, 160 84, 185 86, 205"/>
        <path d="M 140, 138 C 138, 160 136, 185 136, 205"/>
        <path d="M 72, 120 C 72, 155 72, 185 74, 206"/>
        <path d="M 148, 120 C 148, 155 148, 185 146, 206"/>

        <!-- Hood Crease (Power Bulge) -->
        <path d="M 98, 100 L 102, 52 C 105, 45 115, 45 118, 52 L 122, 100"/>
        
        <!-- Cowl Cooling Slat Vents -->
        <rect x="94" y="104" width="6" height="8" rx="0.5" stroke-width="0.8"/>
        <line x1="94" y1="108" x2="100" y2="108" stroke-width="0.8"/>
        <rect x="120" y="104" width="6" height="8" rx="0.5" stroke-width="0.8"/>
        <line x1="120" y1="108" x2="126" y2="108" stroke-width="0.8"/>

        <!-- Door Seams -->
        <line x1="70" y1="122" x2="76" y2="122"/>
        <line x1="150" y1="122" x2="144" y2="122"/>
        <line x1="70" y1="202" x2="86" y2="202"/>
        <line x1="150" y1="202" x2="136" y2="202"/>

        <!-- Rear Glass Hatch Window -->
        <rect x="84" y="208" width="52" height="42" rx="4" stroke-width="1.2"/>
        
        <!-- Hatch Panel Seams -->
        <path d="M 80, 205 L 76, 258"/>
        <path d="M 140, 205 L 144, 258"/>
        <path d="M 76, 258 C 110, 260 110, 260 144, 258"/>

        <!-- Fuel Cap (Right side, classic round) -->
        <circle cx="149" cy="232" r="3.5"/>
      </g>

      <!-- Recessed Sugar-Scoop Round Headlights -->
      <g stroke-linecap="round" stroke-linejoin="round">
        <path d="M 86, 32 C 76, 34 70, 42 72, 54 C 74, 58 82, 56 86, 48 Z" fill="none" stroke="var(--accent)" stroke-width="1.5"/>
        <path d="M 134, 32 C 144, 34 150, 42 148, 54 C 146, 58 138, 56 134, 48 Z" fill="none" stroke="var(--accent)" stroke-width="1.5"/>
        <circle cx="79" cy="46" r="4.5" fill="#fffae0" stroke="var(--accent)" stroke-width="1.2"/>
        <circle cx="141" cy="46" r="4.5" fill="#fffae0" stroke="var(--accent)" stroke-width="1.2"/>
      </g>

      <!-- Rear Bumper Lines -->
      <line x1="74" y1="274" x2="146" y2="274" stroke="var(--accent)" stroke-width="3.5" stroke-linecap="round"/>
    </g>

    <!-- Group 4: Blueprint Hatchback -->
    <g id="car_style_hatchback" class="car-style-group" style="display:none;">
      <path fill="var(--bg-panel)" stroke="var(--accent)" stroke-width="2.5" d="
        M 110, 25
        C 128,25 148,27 154,34
        C 158,39 160,48 160,56
        C 160,75 159,100 159,130
        C 159,160 160,200 160,240
        C 160,252 158,261 154,266
        C 148,273 128,275 110,275
        C 92,275 72,273 66,266
        C 62,261 60,252 60,240
        C 60,160 61,160 61,130
        C 61,100 60,75 60,56
        C 60,48 62,39 66,34
        C 72,27 92,25 110,25
        Z"/>
      <path d="M 72, 98 C 62, 95 56, 88 58, 86 C 60, 84 66, 92 72, 94 Z" fill="var(--accent)" stroke="var(--accent)" stroke-width="1" opacity="0.8"/>
      <path d="M 148, 98 C 158, 95 164, 88 162, 86 C 160, 84 154, 92 148, 94 Z" fill="var(--accent)" stroke="var(--accent)" stroke-width="1" opacity="0.8"/>

      <g stroke="var(--text-main)" stroke-width="1.2" fill="none" opacity="0.45" stroke-linecap="round" stroke-linejoin="round">
        <path d="M 72, 90 C 110, 85 110, 85 148, 90"/>
        <path d="M 76, 115 C 110, 112 110, 112 144, 115"/>
        <line x1="72" y1="90" x2="76" y2="115"/>
        <line x1="148" y1="90" x2="144" y2="115"/>
        <path d="M 76, 115 C 78, 140 78, 175 78, 205"/>
        <path d="M 144, 115 C 142, 140 142, 175 142, 205"/>
        <path d="M 64, 95 C 65, 130 65, 175 64, 210"/>
        <path d="M 156, 95 C 155, 130 155, 175 156, 210"/>
        <path d="M 78, 205 C 110, 208 110, 208 142, 205"/>
        <path d="M 70, 235 C 110, 238 110, 238 150, 235"/>
        <line x1="78" y1="205" x2="70" y2="235"/>
        <line x1="142" y1="205" x2="150" y2="235"/>
        <path d="M 74, 90 C 72, 60 74, 40 82, 32"/>
        <path d="M 146, 90 C 148, 60 146, 40 138, 32"/>
        <path d="M 82, 32 C 110, 30 110, 30 138, 32"/>
        <line x1="110" y1="25" x2="110" y2="30"/>
        <line x1="61" y1="135" x2="78" y2="135"/>
        <line x1="159" y1="135" x2="142" y2="135"/>
        <line x1="64" y1="162" x2="78" y2="162"/>
        <line x1="156" y1="162" x2="142" y2="162"/>
        <line x1="61" y1="190" x2="78" y2="190"/>
        <line x1="159" y1="190" x2="142" y2="190"/>
        <path d="M 68, 264 C 74, 258 74, 250 70, 235"/>
        <path d="M 152, 264 C 146, 258 146, 250 150, 235"/>
        <path d="M 68, 264 C 110, 268 110, 268 152, 264"/>
      </g>
      <g stroke-linecap="round" stroke-linejoin="round">
        <path d="M 64, 34 C 68, 36 72, 42 70, 48 C 66, 46 64, 40 64, 34 Z" fill="none" stroke="var(--accent)" stroke-width="1.5"/>
        <path d="M 156, 34 C 152, 36 148, 42 150, 48 C 154, 46 156, 40 156, 34 Z" fill="none" stroke="var(--accent)" stroke-width="1.5"/>
        <path d="M 62, 252 C 65, 255 70, 258 68, 263 C 64, 262 62, 258 62, 252 Z" fill="none" stroke="#ef4444" stroke-width="1.5"/>
        <path d="M 158, 252 C 155, 255 150, 258 152, 263 C 156, 262 158, 258 158, 252 Z" fill="none" stroke="#ef4444" stroke-width="1.5"/>
      </g>
    </g>

    <!-- Group 5: Blueprint Coupe -->
    <g id="car_style_coupe" class="car-style-group" style="display:none;">
      <path fill="var(--bg-panel)" stroke="var(--accent)" stroke-width="2.5" d="
        M 110, 25
        C 126,25 142,27 148,34
        C 154,39 158,48 158,56
        C 158,75 152,100 148,135
        C 146,160 162,200 162,230
        C 162,246 156,262 146,270
        C 136,275 124,275 110,275
        C 96,275 84,275 74,270
        C 64,262 58,246 58,230
        C 58,200 74,160 72,135
        C 68,100 62,75 62,56
        C 62,48 66,39 72,34
        C 78,27 94,25 110,25
        Z"/>
      <path d="M 72, 98 C 62, 95 56, 88 58, 86 C 60, 84 66, 92 72, 94 Z" fill="var(--accent)" stroke="var(--accent)" stroke-width="1" opacity="0.8"/>
      <path d="M 148, 98 C 158, 95 164, 88 162, 86 C 160, 84 154, 92 148, 94 Z" fill="var(--accent)" stroke="var(--accent)" stroke-width="1" opacity="0.8"/>

      <g stroke="var(--text-main)" stroke-width="1.2" fill="none" opacity="0.45" stroke-linecap="round" stroke-linejoin="round">
        <path d="M 74, 100 C 110, 95 110, 95 146, 100"/>
        <path d="M 78, 122 C 110, 118 110, 118 142, 122"/>
        <line x1="74" y1="100" x2="78" y2="122"/>
        <line x1="146" y1="100" x2="142" y2="122"/>
        <path d="M 78, 122 C 80, 145 82, 175 84, 195"/>
        <path d="M 142, 122 C 140, 145 138, 175 136, 195"/>
        <path d="M 68, 105 C 68, 140 68, 175 66, 200"/>
        <path d="M 152, 105 C 152, 140 152, 175 154, 200"/>
        <path d="M 84, 195 C 110, 198 110, 198 136, 195"/>
        <path d="M 76, 230 C 110, 234 110, 234 144, 230"/>
        <line x1="84" y1="195" x2="76" y2="230"/>
        <line x1="136" y1="195" x2="144" y2="230"/>
        <path d="M 76, 100 C 74, 65 76, 42 84, 33"/>
        <path d="M 144, 100 C 146, 65 144, 42 136, 33"/>
        <path d="M 84, 33 C 110, 31 110, 31 136, 33"/>
        <line x1="110" y1="25" x2="110" y2="31"/>
        <line x1="63" y1="115" x2="75" y2="115"/>
        <line x1="157" y1="115" x2="145" y2="115"/>
        <line x1="65" y1="155" x2="81" y2="155"/>
        <line x1="155" y1="155" x2="139" y2="155"/>
        <line x1="67" y1="195" x2="84" y2="195"/>
        <line x1="153" y1="195" x2="136" y2="195"/>
        <path d="M 72, 266 C 78, 255 78, 245 76, 230"/>
        <path d="M 148, 266 C 142, 255 142, 245 144, 230"/>
        <path d="M 72, 266 C 110, 270 110, 270 148, 266"/>
      </g>
      <g stroke-linecap="round" stroke-linejoin="round">
        <path d="M 66, 35 C 70, 37 74, 43 72, 49 C 68, 47 66, 41 66, 35 Z" fill="none" stroke="var(--accent)" stroke-width="1.5"/>
        <path d="M 154, 35 C 150, 37 146, 43 148, 49 C 152, 47 154, 41 154, 35 Z" fill="none" stroke="var(--accent)" stroke-width="1.5"/>
        <path d="M 62, 254 C 66, 256 72, 258 70, 262" fill="none" stroke="#ef4444" stroke-width="1.5"/>
        <path d="M 158, 254 C 154, 256 148, 258 150, 262" fill="none" stroke="#ef4444" stroke-width="1.5"/>
      </g>
    </g>

    <!-- CG (Center of Gravity) Indicator Symbol -->
    <g id="cgSymbol" transform="translate(110, 150)" style="opacity: 0.3;">
      <circle cx="0" cy="0" r="12" fill="var(--bg-panel)" stroke="var(--text-main)" stroke-width="2"/>
      <path d="M0,0 L0,-12 A12,12 0 0,1 12,0 Z" fill="var(--text-main)"/>
      <path d="M0,0 L0,12 A12,12 0 0,1 -12,0 Z" fill="var(--text-main)"/>
      <circle cx="0" cy="0" r="3" fill="var(--accent)"/>
    </g>
  </svg>

  </div>

  </div>

  <div class="panel">
    <table class="stats">
    <thead>
      <tr>
        <th></th>
        <th data-i18n="current">Current</th>
        <th>%</th>
      </tr>
    </thead>
    <tbody>
      <tr>
        <td data-i18n="cross">Cross</td>
        <td>-</td>
        <td id="crosspct"></td>
      </tr>
      <tr>
        <td data-i18n="left">Left</td>
        <td class="val" id="left"></td>
        <td id="leftpct"></td>
      </tr>
      <tr>
        <td data-i18n="right">Right</td>
        <td class="val" id="right"></td>
        <td id="rightpct"></td>
      </tr>
      <tr>
        <td data-i18n="front">Front</td>
        <td class="val" id="front"></td>
        <td id="frontpct"></td>
      </tr>
      <tr>
        <td data-i18n="rear">Rear</td>
        <td class="val" id="rear"></td>
        <td id="rearpct"></td>
      </tr>
      <tr class="totalrow">
        <td data-i18n="total">Total</td>
        <td class="val" id="total"></td>
      <td></td>
      </tr>
    </tbody>
    </table>
  </div>
  <div class="buttons">
    <button class="zero" onclick="tare()" data-i18n="zero">ZERO</button>
    <button class="cal" onclick="openCalWizard()" data-i18n="cal">CAL</button>
    <button onclick="toggleUnits()" id="unitBtn">KG</button>
  </div>
  
<div class="battery-panel">

  <div class="battery-item">
    <div class="battery-label">FL</div>
    <div class="battery-icon">
      <div class="battery-fill" id="fl_fill"></div>
    </div>
    <div class="battery-text" id="fl_batt">100%</div>
  </div>

  <div class="battery-item">
    <div class="battery-label">FR</div>
    <div class="battery-icon">
      <div class="battery-fill" id="fr_fill"></div>
    </div>
    <div class="battery-text" id="fr_batt">100%</div>
  </div>

  <div class="battery-item">
    <div class="battery-label">RL</div>
    <div class="battery-icon">
      <div class="battery-fill" id="rl_fill"></div>
    </div>
    <div class="battery-text" id="rl_batt">100%</div>
  </div>

  <div class="battery-item">
    <div class="battery-label">RR</div>
    <div class="battery-icon">
      <div class="battery-fill" id="rr_fill"></div>
    </div>
    <div class="battery-text" id="rr_batt">100%</div>
  </div>

</div>

  <div class="modal-overlay" id="settingsModal">
    <div class="modal">
      <h3 data-i18n="settings_title">Settings</h3>
      
      <div class="settings-row">
        <span data-i18n="theme">Theme:</span>
        <button class="settings-btn" onclick="toggleTheme()" id="themeBtn">Dark Mode</button>
      </div>

      <div class="settings-row">
        <span data-i18n="lang_label">Language:</span>
        <select id="langSelect" onchange="setLanguage(this.value)" style="background:var(--bg-main-alt); color:var(--text-main); border:1px solid var(--accent); padding:6px 12px; border-radius:4px; cursor:pointer; font-size:14px; outline:none;">
          <option value="en">English</option>
          <option value="de">Deutsch</option>
          <option value="es">Español</option>
        </select>
      </div>

      <div class="settings-row">
        <span data-i18n="car_style_label">Car Style:</span>
        <select id="carStyleSelect" onchange="applyCarStyle(this.value)" style="background:var(--bg-main-alt); color:var(--text-main); border:1px solid var(--accent); padding:6px 12px; border-radius:4px; cursor:pointer; font-size:14px; outline:none;">
          <option value="default" data-i18n="style_default">Simple</option>
          <option value="supercar" data-i18n="style_supercar">Modern Supercar</option>
          <option value="classic" data-i18n="style_classic">Classic Sports Car</option>
          <option value="hatchback" data-i18n="style_hatchback">Blueprint Hatchback</option>
          <option value="coupe" data-i18n="style_coupe">Blueprint Coupe</option>
        </select>
      </div>

      <div class="settings-row">
        <span data-i18n="hx_label">HX711 Count:</span>
        <select id="hxSelect" onchange="checkHxChange()" style="background:var(--bg-main-alt); color:var(--text-main); border:1px solid var(--accent); padding:6px 12px; border-radius:4px; cursor:pointer; font-size:14px; outline:none;">
          <option value="1">1</option>
          <option value="3">3</option>
        </select>
      </div>
      <div id="hxRebootMsg" style="display:none; font-size:12px; color:#ef4444; margin-top:-10px; margin-bottom:10px; text-align:left; line-height:1.4;" data-i18n="hx_reboot_msg">
        Reboot required to apply HX711 count change. Click Save & Reboot below.
      </div>
      
      <div class="settings-row">
        <span data-i18n="accent">Accent Color:</span>
        <input type="color" id="accentPicker" onchange="updateAccent(this.value)">
      </div>
      
      <hr style="border-color:var(--accent); margin:15px 0;">
      
      <div style="font-size:14px; margin-bottom:10px; font-weight:bold; color:var(--accent);" data-i18n="wifi_title">WiFi Access Point</div>
      <div class="settings-row" style="flex-direction:column; align-items:stretch; gap:4px; margin-bottom:10px;">
        <label style="font-size:12px; opacity:0.8; text-align:left;" data-i18n="wifi_ssid">Network Name (SSID):</label>
        <input type="text" id="wifiSsidInput" style="background:var(--bg-main-alt); color:var(--text-main); border:1px solid var(--accent); padding:8px; border-radius:4px; font-size:14px;">
      </div>
      <div class="settings-row" style="flex-direction:column; align-items:stretch; gap:4px; margin-bottom:15px;">
        <label style="font-size:12px; opacity:0.8; text-align:left;" data-i18n="wifi_pass">Password (min 8 chars):</label>
        <input type="password" id="wifiPassInput" style="background:var(--bg-main-alt); color:var(--text-main); border:1px solid var(--accent); padding:8px; border-radius:4px; font-size:14px;">
      </div>
      <div class="settings-row" style="justify-content:flex-end;">
        <button class="settings-btn" style="background:var(--accent); color:var(--bg-main); font-weight:bold; border:none;" onclick="saveWifiSettings()" data-i18n="save_reboot">Save & Reboot</button>
      </div>
      
      <hr style="border-color:var(--accent); margin:15px 0;">
      
      <div id="debugPanel" style="font-size:12px; color:var(--accent);">
        <h4 style="margin:0 0 8px 0; color:var(--text-main);" data-i18n="debug_title">Raw Debug Info</h4>
        <div id="debugContent"></div>
      </div>
      
      <div style="text-align:right; margin-top:20px;">
        <button class="settings-btn" onclick="toggleSettings()" data-i18n="close">Close</button>
      </div>
    </div>
  </div>

  <!-- Updating Overlay Modal -->
  <div class="modal-overlay" id="updateModal">
    <div class="modal" style="max-width:400px; text-align:center;">
      <h3 id="updateTitle" style="margin-bottom:15px; color:var(--accent);">Applying Settings</h3>
      <div id="updateSpinner" class="spinner" style="margin:20px auto; width:40px; height:40px; border:4px solid #333; border-top:4px solid var(--accent); border-radius:50%; animation:spin 1s linear infinite;"></div>
      <div style="font-size:14px; margin-bottom:20px; line-height:1.4;" id="updateStatusText">Broadcasting settings update to scales...</div>
      
      <div style="text-align:left; background:var(--bg-main-alt); border:1px solid #333; border-radius:6px; padding:12px; margin-bottom:20px; font-size:13px;">
        <div style="display:flex; justify-content:space-between; margin-bottom:8px;">
          <span>FR Pad:</span>
          <span id="update_fr_status" style="font-weight:bold;">Offline</span>
        </div>
        <div style="display:flex; justify-content:space-between; margin-bottom:8px;">
          <span>RL Pad:</span>
          <span id="update_rl_status" style="font-weight:bold;">Offline</span>
        </div>
        <div style="display:flex; justify-content:space-between;">
          <span>RR Pad:</span>
          <span id="update_rr_status" style="font-weight:bold;">Offline</span>
        </div>
      </div>
      
      <button class="settings-btn" id="forceRebootBtn" style="background:#ef4444; border:none; color:white; font-weight:bold; display:none;" onclick="forceReboot()">Force Reboot Master</button>
    </div>
  </div>

  <!-- Calibration Wizard Modal -->
  <div class="modal-overlay" id="calModal">
    <div class="modal" style="max-width:450px;">
      <h3 data-i18n="cal_wizard">Calibration Wizard</h3>
      
      <!-- Step 1: Select Pad & Tare -->
      <div id="calStep1" class="cal-step">
        <div style="font-size:14px; margin-bottom:15px; line-height:1.4;" data-i18n="cal_step1_desc">Select the scale pad you wish to calibrate, clear all weight from it, and click Zero to tare.</div>
        
        <div class="settings-row" style="margin-bottom:15px;">
          <span data-i18n="cal_pad_select">Select Pad:</span>
          <select id="calPad" style="background:var(--bg-main-alt); color:var(--text-main); border:1px solid var(--accent); padding:6px 12px; border-radius:4px; font-size:14px; outline:none; cursor:pointer;">
            <option value="FL">FL (Front Left)</option>
            <option value="FR">FR (Front Right)</option>
            <option value="RL">RL (Rear Left)</option>
            <option value="RR">RR (Rear Right)</option>
          </select>
        </div>
        
        <div style="text-align:right; margin-top:20px;">
          <button class="settings-btn" onclick="closeCalWizard()" style="margin-right:8px;" data-i18n="close">Close</button>
          <button class="settings-btn" style="background:var(--accent); color:var(--bg-main); font-weight:bold; border:none;" onclick="calWizardTare()" data-i18n="zero">ZERO</button>
        </div>
      </div>
      
      <!-- Step 2: Place Known Weight & Input Value -->
      <div id="calStep2" class="cal-step" style="display:none;">
        <div style="font-size:14px; margin-bottom:15px; line-height:1.4;" data-i18n="cal_step2_desc">Place a known heavy weight on the selected scale pad and enter its value below:</div>
        
        <div class="settings-row" style="flex-direction:column; align-items:stretch; gap:6px; margin-bottom:15px;">
          <label style="font-size:12px; opacity:0.8; text-align:left;" id="calWeightLabel"></label>
          <input type="number" id="calKnownWeight" step="0.1" style="background:var(--bg-main-alt); color:var(--text-main); border:1px solid var(--accent); padding:8px; border-radius:4px; font-size:14px; outline:none;">
        </div>
        
        <div style="text-align:right; margin-top:20px;">
          <button class="settings-btn" onclick="showCalStep(1)" style="margin-right:8px;" data-i18n="cal_btn_prev">Back</button>
          <button class="settings-btn" style="background:var(--accent); color:var(--bg-main); font-weight:bold; border:none;" onclick="calWizardSubmit()" data-i18n="cal_btn_finish">Calibrate & Save</button>
        </div>
      </div>

      <!-- Step 3: Success Screen -->
      <div id="calStep3" class="cal-step" style="display:none;">
        <div style="text-align:center; padding:20px 0;">
          <div style="font-size:40px; margin-bottom:15px;">✅</div>
          <h4 style="margin:0 0 10px 0; color:var(--accent);" data-i18n="cal_success">Calibration Completed!</h4>
        </div>
        <div style="text-align:right; margin-top:20px;">
          <button class="settings-btn" onclick="closeCalWizard()" data-i18n="close">Close</button>
        </div>
      </div>
      
    </div>
   <script>
  let useKg = true;

  const i18n = {
    en: {
      title: "Corner Weight System",
      theme: "Theme:",
      theme_dark: "Dark Mode",
      theme_light: "Light Mode",
      accent: "Accent Color:",
      lang_label: "Language:",
      wifi_title: "WiFi Access Point",
      wifi_ssid: "Network Name (SSID):",
      wifi_pass: "Password (min 8 chars):",
      save_reboot: "Save & Reboot",
      close: "Close",
      current: "Current",
      cross: "Cross",
      left: "Left",
      right: "Right",
      front: "Front",
      rear: "Rear",
      total: "Total",
      zero: "ZERO",
      cal: "CAL",
      kg: "KG",
      lbs: "LBS",
      settings_title: "Settings",
      cal_wizard: "Calibration Wizard",
      cal_step1_desc: "Select the scale pad you wish to calibrate, clear all weight from it, and click Zero to tare.",
      cal_pad_select: "Select Pad:",
      cal_step2_desc: "Place a known heavy weight on the selected scale pad and enter its value below:",
      cal_btn_prev: "Back",
      cal_btn_finish: "Calibrate & Save",
      cal_success: "Calibration Completed!",
      debug_title: "Raw Debug Info",
      wifi_confirm: "Are you sure you want to change settings? The device will reboot and you will need to reconnect to: ",
      wifi_success: "Settings saved successfully. Reconnect to the new network in a few seconds.",
      wifi_error: "Failed to save settings: ",
      ssid_empty: "SSID cannot be empty",
      pass_short: "Password must be at least 8 characters",
      hx_label: "HX711 Count:",
      hx_reboot_msg: "Reboot required to apply HX711 count change. Click Save & Reboot below.",
      car_style_label: "Car Style:",
      style_default: "Simple",
      style_supercar: "Modern Supercar",
      style_classic: "Classic Sports Car",
      style_hatchback: "Blueprint Hatchback",
      style_coupe: "Blueprint Coupe"
    },
    de: {
      title: "Radlastwaagen-System",
      theme: "Design:",
      theme_dark: "Dunkelmodus",
      theme_light: "Hellmodus",
      accent: "Akzentfarbe:",
      lang_label: "Sprache:",
      wifi_title: "WLAN-Zugangspunkt",
      wifi_ssid: "Netzwerkname (SSID):",
      wifi_pass: "Passwort (mind. 8 Zeichen):",
      save_reboot: "Speichern & Neustart",
      close: "Schließen",
      current: "Aktuell",
      cross: "Diagonale",
      left: "Links",
      right: "Rechts",
      front: "Vorne",
      rear: "Hinten",
      total: "Gesamt",
      zero: "NULLEN",
      cal: "KAL",
      kg: "KG",
      lbs: "LBS",
      settings_title: "Einstellungen",
      cal_wizard: "Kalibrierungs-Assistent",
      cal_step1_desc: "Wählen Sie das zu kalibrierende Radlast-Pad aus, entfernen Sie jegliche Last und klicken Sie auf Nullen.",
      cal_pad_select: "Pad wählen:",
      cal_step2_desc: "Legen Sie ein bekanntes Gewicht auf das ausgewählte Pad und tragen Sie den Wert unten ein:",
      cal_btn_prev: "Zurück",
      cal_btn_finish: "Kalibrieren & Speichern",
      cal_success: "Kalibrierung abgeschlossen!",
      debug_title: "Rohdaten-Debug",
      wifi_confirm: "Sind Sie sicher, dass Sie die Einstellungen ändern möchten? Das Gerät wird neu gestartet und Sie müssen sich erneut verbinden mit: ",
      wifi_success: "Einstellungen erfolgreich gespeichert. Verbinden Sie sich in Kürze mit dem neuen Netzwerk.",
      wifi_error: "Einstellungen konnten nicht gespeichert werden: ",
      ssid_empty: "SSID darf nicht leer sein",
      pass_short: "Passwort muss mindestens 8 Zeichen lang sein",
      hx_label: "Anzahl HX711:",
      hx_reboot_msg: "Neustart erforderlich, um die HX711-Anzahl zu ändern. Unten auf Speichern & Neustart klicken.",
      car_style_label: "Fahrzeug-Stil:",
      style_default: "Einfach",
      style_supercar: "Moderner Supersportwagen",
      style_classic: "Klassischer Sportwagen",
      style_hatchback: "Fließheck-Blaupause",
      style_coupe: "Coupe-Blaupause"
    },
    es: {
      title: "Sistema de Pesaje",
      theme: "Tema:",
      theme_dark: "Modo Oscuro",
      theme_light: "Modo Claro",
      accent: "Color de Acento:",
      lang_label: "Idioma:",
      wifi_title: "Punto de Acceso WiFi",
      wifi_ssid: "Nombre de Red (SSID):",
      wifi_pass: "Contraseña (mín. 8 caracteres):",
      save_reboot: "Guardar y Reiniciar",
      close: "Cerrar",
      current: "Actual",
      cross: "Cruzado",
      left: "Izquierda",
      right: "Derecha",
      front: "Delantero",
      rear: "Trasero",
      total: "Total",
      zero: "CERO",
      cal: "CAL",
      kg: "KG",
      lbs: "LBS",
      settings_title: "Ajustes",
      cal_wizard: "Asistente de Calibración",
      cal_step1_desc: "Seleccione el plato de pesaje que desea calibrar, retire cualquier carga y haga clic en Cero.",
      cal_pad_select: "Seleccionar Plato:",
      cal_step2_desc: "Coloque un peso conocido en el plato seleccionado e ingrese su valor a continuación:",
      cal_btn_prev: "Atrás",
      cal_btn_finish: "Calibrar y Guardar",
      cal_success: "¡Calibración Completada!",
      debug_title: "Info de Debug",
      wifi_confirm: "¿Está seguro de que desea cambiar la configuración? El dispositivo se reiniciará y deberá volver a conectarse a: ",
      wifi_success: "Configuración guardada. Conéctese a la nueva red en unos segundos.",
      wifi_error: "Error al guardar la configuración: ",
      ssid_empty: "El SSID no puede estar vacío",
      pass_short: "La contraseña debe tener al menos 8 caracteres",
      hx_label: "Cantidad de HX711:",
      hx_reboot_msg: "Se requiere reiniciar para aplicar el cambio de cantidad de HX711. Haga clic en Guardar y Reiniciar abajo.",
      car_style_label: "Estilo de Auto:",
      style_default: "Simple",
      style_supercar: "Superdeportivo Moderno",
      style_classic: "Auto Deportivo Clásico",
      style_hatchback: "Plano Hatchback",
      style_coupe: "Plano Cupé"
    }
  };

  // --- UI Settings Logic ---
  let currentTheme = localStorage.getItem('theme') || 'dark';
  let currentAccent = localStorage.getItem('accent') || '#3b82f6';
  let currentLang = localStorage.getItem('lang') || 'en';
  let currentCarStyle = localStorage.getItem('carStyle') || 'coupe';

  function applySettings() {
    if(currentTheme === 'light') {
      document.body.classList.add('light-theme');
    } else {
      document.body.classList.remove('light-theme');
    }
    setLanguage(currentLang);
    document.documentElement.style.setProperty('--accent', currentAccent);
    document.getElementById('accentPicker').value = currentAccent;
    applyCarStyle(currentCarStyle);
  }

  function applyCarStyle(style) {
    currentCarStyle = style;
    localStorage.setItem('carStyle', style);
    document.querySelectorAll('.car-style-group').forEach(el => {
      el.style.display = 'none';
    });
    let selGroup = document.getElementById('car_style_' + style);
    if (selGroup) {
      selGroup.style.display = 'block';
    }
    let selectEl = document.getElementById('carStyleSelect');
    if (selectEl) {
      selectEl.value = style;
    }
  }

  function setLanguage(lang) {
    currentLang = lang;
    localStorage.setItem('lang', lang);
    document.getElementById("langSelect").value = lang;

    // Translate all static nodes with data-i18n
    document.querySelectorAll("[data-i18n]").forEach(el => {
      let key = el.getAttribute("data-i18n");
      if (i18n[lang] && i18n[lang][key]) {
        el.innerText = i18n[lang][key];
      }
    });

    // Update dynamic text
    if (currentTheme === 'light') {
      document.getElementById('themeBtn').innerText = i18n[lang].theme_light;
    } else {
      document.getElementById('themeBtn').innerText = i18n[lang].theme_dark;
    }

    // Refresh unit button text
    document.getElementById("unitBtn").innerText = useKg ? i18n[lang].kg : i18n[lang].lbs;
  }

  function toggleTheme() {
    currentTheme = currentTheme === 'dark' ? 'light' : 'dark';
    localStorage.setItem('theme', currentTheme);
    applySettings();
  }

  function updateAccent(color) {
    currentAccent = color;
    localStorage.setItem('accent', color);
    applySettings();
  }

  let originalHxCount = 3;
  function loadWifiConfig() {
    fetch("/wifi_config")
    .then(r => r.json())
    .then(data => {
      document.getElementById("wifiSsidInput").value = data.ssid || "";
      document.getElementById("wifiPassInput").value = data.pass || "";
      document.getElementById("hxSelect").value = data.hx_count || 3;
      originalHxCount = data.hx_count || 3;
      checkHxChange();
    })
    .catch(err => console.error("Error loading WiFi config:", err));
  }

  function checkHxChange() {
    let currentHx = document.getElementById("hxSelect").value;
    let msg = document.getElementById("hxRebootMsg");
    if (currentHx != originalHxCount) {
      msg.style.display = "block";
    } else {
      msg.style.display = "none";
    }
  }

  function toggleSettings() {
    let m = document.getElementById("settingsModal");
    if (m.style.display !== "flex") {
      loadWifiConfig();
      m.style.display = "flex";
    } else {
      m.style.display = "none";
    }
  }

  function saveWifiSettings() {
    let ssid = document.getElementById("wifiSsidInput").value.trim();
    let pass = document.getElementById("wifiPassInput").value.trim();
    let hx = document.getElementById("hxSelect").value;
    if (ssid.length === 0) {
      alert(i18n[currentLang].ssid_empty);
      return;
    }
    if (pass.length < 8) {
      alert(i18n[currentLang].pass_short);
      return;
    }
    if (confirm(i18n[currentLang].wifi_confirm + ssid)) {
      fetch("/save_wifi?ssid=" + encodeURIComponent(ssid) + "&pass=" + encodeURIComponent(pass) + "&hx_count=" + hx)
      .then(r => {
        if (r.ok) {
          toggleSettings();
          showUpdateModal();
        } else {
          r.text().then(text => alert(i18n[currentLang].wifi_error + text));
        }
      })
      .catch(err => alert(i18n[currentLang].wifi_error + err));
    }
  }

  let pollInterval = null;

  function showUpdateModal() {
    document.getElementById("updateModal").style.display = "flex";
    document.getElementById("forceRebootBtn").style.display = "none";
    document.getElementById("updateSpinner").style.display = "block";
    document.getElementById("updateStatusText").innerText = "Broadcasting settings update to scales...";
    document.getElementById("update_fr_status").innerText = "Checking...";
    document.getElementById("update_rl_status").innerText = "Checking...";
    document.getElementById("update_rr_status").innerText = "Checking...";
    
    // Start polling status
    if (pollInterval) clearInterval(pollInterval);
    pollInterval = setInterval(pollUpdateStatus, 500);
    // Show force reboot button after 8 seconds in case of problems
    setTimeout(() => {
      let btn = document.getElementById("forceRebootBtn");
      if (btn) btn.style.display = "inline-block";
    }, 8000);
  }

  function pollUpdateStatus() {
    fetch("/update_status")
    .then(r => r.json())
    .then(data => {
      if (!data.updating) return; // Not in updating mode yet or already done
      
      // Update FR status
      let fr_status = document.getElementById("update_fr_status");
      if (!data.fr_req) {
        fr_status.innerText = "Offline (Skipped)";
        fr_status.style.color = "#888";
      } else if (data.fr_ack) {
        fr_status.innerText = "OK (Updated)";
        fr_status.style.color = "#10b981";
      } else {
        fr_status.innerText = "Updating...";
        fr_status.style.color = "#f59e0b";
      }
      
      // Update RL status
      let rl_status = document.getElementById("update_rl_status");
      if (!data.rl_req) {
        rl_status.innerText = "Offline (Skipped)";
        rl_status.style.color = "#888";
      } else if (data.rl_ack) {
        rl_status.innerText = "OK (Updated)";
        rl_status.style.color = "#10b981";
      } else {
        rl_status.innerText = "Updating...";
        rl_status.style.color = "#f59e0b";
      }
      
      // Update RR status
      let rr_status = document.getElementById("update_rr_status");
      if (!data.rr_req) {
        rr_status.innerText = "Offline (Skipped)";
        rr_status.style.color = "#888";
      } else if (data.rr_ack) {
        rr_status.innerText = "OK (Updated)";
        rr_status.style.color = "#10b981";
      } else {
        rr_status.innerText = "Updating...";
        rr_status.style.color = "#f59e0b";
      }
      
      if (data.all_done) {
        clearInterval(pollInterval);
        document.getElementById("updateSpinner").style.display = "none";
        document.getElementById("updateStatusText").innerText = "All scales updated successfully! Rebooting system...";
        setTimeout(() => {
          location.reload();
        }, 5000);
      }
    })
    .catch(err => {
      // If endpoint goes offline, it likely means the board is rebooting
      console.log("Connection lost, check if master rebooted:", err);
    });
  }

  function forceReboot() {
    clearInterval(pollInterval);
    document.getElementById("updateStatusText").innerText = "Forcing master reboot...";
    fetch("/reboot")
    .then(() => {
      setTimeout(() => {
        location.reload();
      }, 5000);
    })
    .catch(() => {
      setTimeout(() => {
        location.reload();
      }, 4000);
    });
  }

  // --- Calibration Wizard Logic ---
  function openCalWizard() {
    showCalStep(1);
    document.getElementById("calKnownWeight").value = "";
    document.getElementById("calWeightLabel").innerText = useKg ? i18n[currentLang].kg : i18n[currentLang].lbs;
    document.getElementById("calModal").style.display = "flex";
  }

  function closeCalWizard() {
    document.getElementById("calModal").style.display = "none";
  }

  function showCalStep(step) {
    document.querySelectorAll(".cal-step").forEach(el => el.style.display = "none");
    document.getElementById("calStep" + step).style.display = "block";
  }

  function calWizardTare() {
    fetch("/tare")
    .then(r => {
      if (r.ok) {
        showCalStep(2);
      } else {
        alert("Zero failed. Check connection.");
      }
    })
    .catch(err => alert("Error: " + err));
  }

  function calWizardSubmit() {
    let pad = document.getElementById("calPad").value;
    let weightVal = parseFloat(document.getElementById("calKnownWeight").value);
    if (isNaN(weightVal) || weightVal <= 0) {
      alert("Please enter a valid weight");
      return;
    }
    let weightSend = weightVal;
    if (useKg) {
      weightSend = weightVal * 2.20462;
    }
    fetch("/calibrate?pad=" + pad + "&weight=" + weightSend)
    .then(r => {
      if (r.ok) {
        showCalStep(3);
      } else {
        alert("Calibration failed.");
      }
    })
    .catch(err => alert("Error: " + err));
  }

  applySettings();
  // -------------------------

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
    let str = useKg
      ? value.toFixed(1)
      : (Math.round(value * 2) / 2).toFixed(1)
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

  let lastRenderedHxCount = null;
  function updateDebugPanelStructure(hxCount) {
    if (lastRenderedHxCount === hxCount) return;
    lastRenderedHxCount = hxCount;
    let content = document.getElementById("debugContent");
    if (!content) return;
    if (hxCount == 1) {
      content.innerHTML = `
        <div style="display:flex; justify-content:space-between; margin-bottom:4px;">
          <span><b>FL:</b> <span id="fl_wa">-</span></span>
          <span><b>FR:</b> <span id="fr_wa">-</span></span>
        </div>
        <div style="display:flex; justify-content:space-between;">
          <span><b>RL:</b> <span id="rl_wa">-</span></span>
          <span><b>RR:</b> <span id="rr_wa">-</span></span>
        </div>
      `;
    } else {
      content.innerHTML = `
        <div style="display:flex; justify-content:space-between; margin-bottom:4px;">
          <span><b>FL</b> A:<span id="fl_wa">-</span> B:<span id="fl_wb">-</span> C:<span id="fl_wc">-</span></span>
          <span><b>FR</b> A:<span id="fr_wa">-</span> B:<span id="fr_wb">-</span> C:<span id="fr_wc">-</span></span>
        </div>
        <div style="display:flex; justify-content:space-between;">
          <span><b>RL</b> A:<span id="rl_wa">-</span> B:<span id="rl_wb">-</span> C:<span id="rl_wc">-</span></span>
          <span><b>RR</b> A:<span id="rr_wa">-</span> B:<span id="rr_wb">-</span> C:<span id="rr_wc">-</span></span>
        </div>
      `;
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
      
      let activeHxCount = d.hx_count || originalHxCount || 3;
      updateDebugPanelStructure(activeHxCount);

      document.getElementById("fl_wa").innerText = (d.fl_wa||0).toFixed(0);
      document.getElementById("fr_wa").innerText = (d.fr_wa||0).toFixed(0);
      document.getElementById("rl_wa").innerText = (d.rl_wa||0).toFixed(0);
      document.getElementById("rr_wa").innerText = (d.rr_wa||0).toFixed(0);

      if (activeHxCount == 3) {
        document.getElementById("fl_wb").innerText = (d.fl_wb||0).toFixed(0);
        document.getElementById("fl_wc").innerText = (d.fl_wc||0).toFixed(0);
        document.getElementById("fr_wb").innerText = (d.fr_wb||0).toFixed(0);
        document.getElementById("fr_wc").innerText = (d.fr_wc||0).toFixed(0);
        document.getElementById("rl_wb").innerText = (d.rl_wb||0).toFixed(0);
        document.getElementById("rl_wc").innerText = (d.rl_wc||0).toFixed(0);
        document.getElementById("rr_wb").innerText = (d.rr_wb||0).toFixed(0);
        document.getElementById("rr_wc").innerText = (d.rr_wc||0).toFixed(0);
      }

      setStatus("fl_status",d.fl_online)
      setStatus("fr_status",d.fr_online)
      setStatus("rl_status",d.rl_online)
      setStatus("rr_status",d.rr_online)

      setLock("fl_lock", d.fl_locked)
      setLock("fr_lock", d.fr_locked)
      setLock("rl_lock", d.rl_locked)
      setLock("rr_lock", d.rr_locked)

      document.getElementById("fl_batt").innerText = d.fl_batt + "%";
document.getElementById("fr_batt").innerText = d.fr_batt + "%";
document.getElementById("rl_batt").innerText = d.rl_batt + "%";
document.getElementById("rr_batt").innerText = d.rr_batt + "%";

document.getElementById("fl_fill").style.width = d.fl_batt + "%";
document.getElementById("fr_fill").style.width = d.fr_batt + "%";
document.getElementById("rl_fill").style.width = d.rl_batt + "%";
document.getElementById("rr_fill").style.width = d.rr_batt + "%";

      // CG (Center of Gravity) dynamic location calculation
      let totalW = d.total || 0;
      let cgSymbol = document.getElementById("cgSymbol");
      if (cgSymbol) {
        let cgX = 110;
        let cgY = 150;
        if (totalW > 0) {
          let leftW = (d.fl_online ? d.fl : 0) + (d.rl_online ? d.rl : 0);
          let rightW = (d.fr_online ? d.fr : 0) + (d.rr_online ? d.rr : 0);
          let rearW = (d.rl_online ? d.rl : 0) + (d.rr_online ? d.rr : 0);
          
          let rightPct = (rightW / totalW) * 100;
          let rearPct = (rearW / totalW) * 100;
          
          let xMin = 80;
          let xMax = 140;
          cgX = xMin + (rightPct / 100) * (xMax - xMin);
          
          let yMin = 80;
          let yMax = 220;
          cgY = yMin + (rearPct / 100) * (yMax - yMin);
          
          cgSymbol.style.opacity = "1";
        } else {
          cgSymbol.style.opacity = "0.3";
        }
        cgSymbol.setAttribute("transform", `translate(${cgX.toFixed(1)}, ${cgY.toFixed(1)})`);
      }

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

void handleCalibrate(){

  String pad = server.arg("pad");
  float known = server.arg("weight").toFloat();

  if(pad=="FL"){
    float reading = 0;
    if (hxCount == 1) {
      reading = scaleA.read_average(20);
    } else {
      reading = scaleA.read_average(20) + scaleB.read_average(20) + scaleC.read_average(20);
    }
    float net = reading - FL_offset;

    FL_cal = net / known;
    prefs.putFloat("FL_cal",FL_cal);

  }

  if(pad=="FR"){
    float net = FR_raw - FR_offset;
    FR_cal = net / known;
    prefs.putFloat("FR_cal",FR_cal);
  }

  if(pad=="RL"){
    float net = RL_raw - RL_offset;
    RL_cal = net / known;
    prefs.putFloat("RL_cal",RL_cal);

  }

  if(pad=="RR"){
    float net = RR_raw - RR_offset;
    RR_cal = net / known;
    prefs.putFloat("RR_cal",RR_cal);

  }
  server.send(200,"text/plain","CAL OK");

}


void onReceive(const esp_now_recv_info *info, const uint8_t *data, int len){

  if (len == 2 && data[0] == 'O' && data[1] == 'K') {
    if (FR_mac_set && memcmp(info->src_addr, FR_mac, 6) == 0) {
      FR_ack_received = true;
      Serial.println("ACK received from FR");
    } else if (RL_mac_set && memcmp(info->src_addr, RL_mac, 6) == 0) {
      RL_ack_received = true;
      Serial.println("ACK received from RL");
    } else if (RR_mac_set && memcmp(info->src_addr, RR_mac, 6) == 0) {
      RR_ack_received = true;
      Serial.println("ACK received from RR");
    }
    
    if (is_updating) {
      bool all_done = (!FR_ack_required || FR_ack_received) &&
                      (!RL_ack_required || RL_ack_received) &&
                      (!RR_ack_required || RR_ack_received);
      if (all_done) {
        Serial.println("All required ACKs received. Rebooting master in 1.5 seconds.");
        shouldRestart = true;
        restartTime = millis() + 1500;
      }
    }
    return;
  }

  if(len != sizeof(ScaleData)){
    Serial.println("BAD PACKET SIZE");
    return;
  }

  memcpy(&incomingData, data, sizeof(incomingData));

  if(
    strncmp(incomingData.pad,"FL",2)!=0 &&
    strncmp(incomingData.pad,"FR",2)!=0 &&
    strncmp(incomingData.pad,"RL",2)!=0 &&
    strncmp(incomingData.pad,"RR",2)!=0
  ){
    Serial.println("INVALID PAD - IGNORE");
    return;
  }

  if(strcmp(incomingData.pad,"FR")==0){
    if (!FR_mac_set) {
      memcpy(FR_mac, info->src_addr, 6);
      FR_mac_set = true;
      Serial.printf("Learned FR MAC: %02X:%02X:%02X:%02X:%02X:%02X\n", FR_mac[0], FR_mac[1], FR_mac[2], FR_mac[3], FR_mac[4], FR_mac[5]);
    }
    FR_wA = incomingData.wA;
    FR_wB = incomingData.wB;
    FR_wC = incomingData.wC;
    FR_raw = incomingData.weight;
    float FR_new = (FR_raw - FR_offset) / FR_cal;
    applyStability(FR_new, lastFR, FR_locked);
    if(FR_filtered == 0){
      FR_filtered = FR_new;
    }
    FR_filtered = FR_filtered + alpha * (FR_new - FR_filtered);
    FR = FR_filtered;
    if(abs(FR) < 0.5) FR = 0;
    FR = round(FR * 2) / 2.0;
    FR_batt = incomingData.battery;
    FR_lastSeen = millis();
  }

  if(strcmp(incomingData.pad,"RL")==0){
    if (!RL_mac_set) {
      memcpy(RL_mac, info->src_addr, 6);
      RL_mac_set = true;
      Serial.printf("Learned RL MAC: %02X:%02X:%02X:%02X:%02X:%02X\n", RL_mac[0], RL_mac[1], RL_mac[2], RL_mac[3], RL_mac[4], RL_mac[5]);
    }
    RL_wA = incomingData.wA;
    RL_wB = incomingData.wB;
    RL_wC = incomingData.wC;
    RL_raw = incomingData.weight;
    float RL_new = (RL_raw - RL_offset) / RL_cal;
    applyStability(RL_new, lastRL, RL_locked);
    if(RL_filtered == 0){
      RL_filtered = RL_new;
    }
    RL_filtered = RL_filtered + alpha * (RL_new - RL_filtered);
    RL = RL_filtered;
    if(abs(RL) < 0.5) RL = 0;
    RL = round(RL * 2) / 2.0;
    RL_batt = incomingData.battery;
    RL_lastSeen = millis();
  }

  if(strcmp(incomingData.pad,"RR")==0){
    if (!RR_mac_set) {
      memcpy(RR_mac, info->src_addr, 6);
      RR_mac_set = true;
      Serial.printf("Learned RR MAC: %02X:%02X:%02X:%02X:%02X:%02X\n", RR_mac[0], RR_mac[1], RR_mac[2], RR_mac[3], RR_mac[4], RR_mac[5]);
    }
    RR_wA = incomingData.wA;
    RR_wB = incomingData.wB;
    RR_wC = incomingData.wC;
    RR_raw = incomingData.weight;
    float RR_new = (RR_raw - RR_offset) / RR_cal;
    applyStability(RR_new, lastRR, RR_locked);
    if(RR_filtered == 0){
      RR_filtered = RR_new;
    }
    RR_filtered = RR_filtered + alpha * (RR_new - RR_filtered);
    RR = RR_filtered;
    if(abs(RR) < 0.5) RR = 0;
    RR = round(RR * 2) / 2.0;
    RR_batt = incomingData.battery;
    RR_lastSeen = millis();
  }

}

void handleWifiConfig(){
  String ssid = prefs.getString("wifi_ssid", "Race_Scales");
  String pass = prefs.getString("wifi_pass", "12345678");
  int hx = prefs.getInt("hx_count", 3);
  String json = "{\"ssid\":\"" + ssid + "\",\"pass\":\"" + pass + "\",\"hx_count\":" + String(hx) + "}";
  server.send(200, "application/json", json);
}

// Handle saving WiFi and HX711 count settings, then broadcast the new count to children
void handleSaveWifi(){
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  String hxVal = server.arg("hx_count");
  if(ssid.length() > 0 && pass.length() >= 8){
    prefs.putString("wifi_ssid", ssid);
    prefs.putString("wifi_pass", pass);
    
    int hx = hxCount;
    if(hxVal.length() > 0){
      hx = hxVal.toInt();
      if(hx == 1 || hx == 3){
        prefs.putInt("hx_count", hx);
        hxCount = hx;
      }
    }

    bool FR_online = (millis() - FR_lastSeen) < 3000;
    bool RL_online = (millis() - RL_lastSeen) < 3000;
    bool RR_online = (millis() - RR_lastSeen) < 3000;

    is_updating = true;
    FR_ack_required = FR_online;
    RL_ack_required = RL_online;
    RR_ack_required = RR_online;
    
    FR_ack_received = false;
    RL_ack_received = false;
    RR_ack_received = false;

    // Send broadcast config update
    uint8_t payload[2] = {'C', (uint8_t)hx};
    uint8_t broadcastAddr[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    esp_now_send(broadcastAddr, payload, sizeof(payload));

    bool any_online = FR_online || RL_online || RR_online;
    if (!any_online) {
      // No children online, reboot master after a short delay
      shouldRestart = true;
      restartTime = millis() + 2000;
    }

    String json = "{\"status\":\"updating\",";
    json += "\"fr_online\":" + String(FR_online ? "true" : "false") + ",";
    json += "\"rl_online\":" + String(RL_online ? "true" : "false") + ",";
    json += "\"rr_online\":" + String(RR_online ? "true" : "false") + "}";
    server.send(200, "application/json", json);
  } else {
    server.send(400, "text/plain", "Invalid parameters. Password must be at least 8 characters.");
  }
}


void setup(){

  Serial.begin(115200);

  prefs.begin("scales");
  hxCount = prefs.getInt("hx_count", 3);

  scaleA.begin(HX_DT_A, HX_SCK_A);
  if(hxCount == 3){
    scaleB.begin(HX_DT_B, HX_SCK_B);
    scaleC.begin(HX_DT_C, HX_SCK_C);
  }

  Serial.println("HX711 optional - will detect automatically");

  String ssid = prefs.getString("wifi_ssid", "Race_Scales");
  String pass = prefs.getString("wifi_pass", "12345678");

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ssid.c_str(), pass.c_str(), 1);

  WiFi.setSleep(false);

  Serial.print("AP MAC: ");
  Serial.println(WiFi.softAPmacAddress());

  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }
  esp_now_register_recv_cb(onReceive);

  // Add broadcast peer
  esp_now_peer_info_t peerInfo = {};
  memset(&peerInfo, 0, sizeof(peerInfo));
  memset(peerInfo.peer_addr, 0xFF, 6);
  peerInfo.channel = 1;
  peerInfo.encrypt = false;
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add broadcast peer");
  }

  server.on("/",handleRoot);
  server.on("/data",handleData);
  server.on("/tare",handleTare);
  server.on("/calibrate",handleCalibrate);
  server.on("/wifi_config", handleWifiConfig);
  server.on("/save_wifi", handleSaveWifi);
  server.on("/update_status", [](){
    bool all_done = (!FR_ack_required || FR_ack_received) &&
                    (!RL_ack_required || RL_ack_received) &&
                    (!RR_ack_required || RR_ack_received);
    String json = "{";
    json += "\"updating\":" + String(is_updating ? "true" : "false") + ",";
    json += "\"fr_req\":" + String(FR_ack_required ? "true" : "false") + ",";
    json += "\"fr_ack\":" + String(FR_ack_received ? "true" : "false") + ",";
    json += "\"rl_req\":" + String(RL_ack_required ? "true" : "false") + ",";
    json += "\"rl_ack\":" + String(RL_ack_received ? "true" : "false") + ",";
    json += "\"rr_req\":" + String(RR_ack_required ? "true" : "false") + ",";
    json += "\"rr_ack\":" + String(RR_ack_received ? "true" : "false") + ",";
    json += "\"all_done\":" + String(all_done ? "true" : "false");
    json += "}";
    server.send(200, "application/json", json);
  });
  server.on("/reboot", [](){
    server.send(200, "text/plain", "REBOOTING");
    delay(500);
    ESP.restart();
  });
  server.on("/reset", [](){
    prefs.begin("scales", false);
    prefs.clear();
    prefs.end();
    server.send(200, "text/plain", "RESET DONE");
    delay(1000);
    ESP.restart();
  });

  FL_cal = prefs.getFloat("FL_cal",1.0);
  FR_cal = prefs.getFloat("FR_cal",1.0);
  RL_cal = prefs.getFloat("RL_cal",1.0);
  RR_cal = prefs.getFloat("RR_cal",1.0);

  FL_offset = prefs.getFloat("FL_offset", 0);

  server.begin();
}

void loop(){
  /* detect scale if plugged in later */
  if(!scaleInitialized && millis() - lastScaleCheck > 1000){
    lastScaleCheck = millis();
    bool ready = false;
    if(hxCount == 1){
      ready = scaleA.is_ready();
    } else {
      ready = scaleA.is_ready() && scaleB.is_ready() && scaleC.is_ready();
    }
    
    if(ready){
      Serial.println("HX711 detected");

      if(FL_offset == 0){   
        delay(500);
        if(hxCount == 1){
          FL_offset = scaleA.read_average(20);
        } else {
          FL_offset = scaleA.read_average(20) + scaleB.read_average(20) + scaleC.read_average(20);
        }
        prefs.putFloat("FL_offset", FL_offset);
      }

      scalePresent = true;
      scaleInitialized = true;
    }
  }

  /* read FL scale */
  bool canRead = false;
  if(hxCount == 1){
    canRead = scalePresent && scaleA.is_ready();
  } else {
    canRead = scalePresent && scaleA.is_ready() && scaleB.is_ready() && scaleC.is_ready();
  }

  if(canRead){
    float rawA = scaleA.read_average(10);
    float raw = 0;
    if(hxCount == 1){
      raw = rawA;
      FL_wA = rawA;
      FL_wB = 0;
      FL_wC = 0;
    } else {
      float rawB = scaleB.read_average(10);
      float rawC = scaleC.read_average(10);
      raw = rawA + rawB + rawC;
      FL_wA = rawA;
      FL_wB = rawB;
      FL_wC = rawC;
    }
    Serial.println(raw);
    float FL_new = (raw - FL_offset) / FL_cal;
    applyStability(FL_new, lastFL, FL_locked);
    if(FL_filtered == 0){
      FL_filtered = FL_new;
    }
    FL_filtered = FL_filtered + alpha * (FL_new - FL_filtered);
    FL = FL_filtered;
    if(abs(FL) < 0.5) FL = 0;
    FL = round(FL * 2) / 2.0;
  }

   int raw = analogRead(FL_BAT_PIN);\
   float voltage = (raw / 4095.0) * 3.3 * 2.0;  // *2 for voltage divider
   FL_batt = voltage;

  server.handleClient();

  // Handle periodic config re-broadcast and update timeout if in updating mode
  static unsigned long lastBroadcastTime = 0;
  static unsigned long updateStartTime = 0;
  static bool prevUpdating = false;

  if (is_updating) {
    if (!prevUpdating) {
      updateStartTime = millis();
      lastBroadcastTime = millis();
      prevUpdating = true;
    }
    
    // Broadcast config update payload every 500ms
    if (millis() - lastBroadcastTime > 500) {
      lastBroadcastTime = millis();
      uint8_t payload[2] = {'C', (uint8_t)hxCount};
      uint8_t broadcastAddr[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
      esp_now_send(broadcastAddr, payload, sizeof(payload));
      Serial.printf("Re-broadcasting config update (HX711 Count: %d) to children...\n", hxCount);
    }

    // Safety timeout: reboot master if stuck updating for more than 10 seconds
    if (millis() - updateStartTime > 10000) {
      Serial.println("Update timeout reached. Rebooting master...");
      ESP.restart();
    }
  } else {
    prevUpdating = false;
  }

  if (shouldRestart && millis() > restartTime) {
    ESP.restart();
  }

  delay(1);

}
