#define ARDUINO_USB_CDC_ON_BOOT 1

#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <Preferences.h>
#include "HX711.h"

Preferences prefs;


//Change to match your "master board"
#define HX_DT 4
#define HX_SCK 5

typedef struct {
  char pad[4];
  float weight;
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

HX711 scale;

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

int battPercent(float v){
  int p = (v - 3.0) * 100 / 1.2;
  if(p>100) p=100;
  if(p<0) p=0;
  return p;
}

void applyStability(float value, float &lastValue, bool &locked){

  float diff = abs(value - lastValue);

  if(diff < .5){
    locked = true;
  } else if(diff > 1.0){   // hysteresis (important)
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

  json+="\"cross\":"+String(crosspct,1);

  json+="}";

  server.send(200,"application/json",json);
}

void handleTare(){

  FL_offset = scale.read_average(10);
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
    width:90vw;        /* scales with screen width */
    max-width:420px;   /* prevents it getting huge on tablets */
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
    <button onclick="toggleUnits()" id="unitBtn">LBS</button>
  </div>
  <script>
  let useKg = false;

  function calibrate() {
    let pad = prompt("Pad (FL FR RL RR)")
    let weight = prompt("Known weight")
    fetch("/calibrate?pad="+pad+"&weight="+weight)
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
    float reading = scale.read_average(20);
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

void setup(){

  Serial.begin(115200);

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
    Serial.println("ESP-NOW init failed");
    return;
  }
  esp_now_register_recv_cb(onReceive);

  server.on("/",handleRoot);
  server.on("/data",handleData);
  server.on("/tare",handleTare);
  server.on("/calibrate",handleCalibrate);

  prefs.begin("scales");

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
    if(scale.is_ready()){
      Serial.println("HX711 detected");

      if(FL_offset == 0){   // 🔥 only if no saved value
        delay(500);
        FL_offset = scale.read_average(20);
        prefs.putFloat("FL_offset", FL_offset);
      }

      scalePresent = true;
      scaleInitialized = true;
    }
  }

  /* read FL scale */
  if(scalePresent && scale.is_ready()){
    float raw = scale.read_average(10);
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

  // int raw = analogRead(FL_BAT_PIN);\
  // float voltage = (raw / 4095.0) * 3.3 * 2.0;  // *2 for voltage divider
  // FL_batt = voltage;

  server.handleClient();

  delay(1);

}
