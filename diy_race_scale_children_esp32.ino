
/*
traviscea DIY Race Scales – Version 1.0
Copyright (c) 2026 Travis Way
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Preferences.h>
#include "HX711.h"

// ---------- ACK SUPPORT ----------
// Send a simple "OK" ACK back to the master after each payload transmission.



// ---------- Compile-time constants ----------
// Maximum number of HX711 modules that can be compiled in.
#define MAX_HX711 3

/* ---------- PAD ID ---------- */
#define PAD_ID "RR"

/* ---------- HX711 PINS ---------- */
#define HX_DT_A 4
#define HX_SCK_A 5
#define HX_DT_B 16
#define HX_SCK_B 17
#define HX_DT_C 18
#define HX_SCK_C 19
#define BAT_PIN 34

// ---------- Global objects ----------
HX711 scaleA;
HX711 scaleB;
HX711 scaleC;
Preferences prefs;
uint8_t hxCount = 1;

/* ---------- CAL + OFFSET ---------- */
float calibration_factor = -3685.4;
float offset = 0;

/* ---------- ESP-NOW DATA ---------- */
typedef struct {
  char pad[4];
  float weight;
  float wA;
  float wB;
  float wC;
  float battery;
} ScaleData;

ScaleData data;


/* ---------- MASTER MAC ADDRESS ---------- */
/* Replace with MAC of your ESP32-S3
Example, if your brain/main esp32 outputs: AP MAC: 4E:DD:76:6F:A5:45
then your masterAddress =
const uint8_t masterAddress[] = {0x4E,0xDD,0x76,0x6F,0xA5,0x45};
uint8_t masterMac[6] = {0x4E,0xDD,0x76,0x6F,0xA5,0x45};
*/

/* ---------- MASTER MAC ADDRESS ---------- */
const uint8_t masterAddress[] = {0x30,0xC6,0xF7,0x31,0x9A,0xC5};
uint8_t masterMac[6] = {0x30,0xC6,0xF7,0x31,0x9A,0xC5}; // Defaults to masterAddress

// Volatile flags for config changes (handled inside loop to avoid stack/WDT crash in system callbacks)
volatile bool pendingConfigChange = false;
volatile uint8_t pendingHxCount = 1;

// ---------- PEER MANAGEMENT ----------
void addPeerIfNeeded(const uint8_t *mac) {
  if (!esp_now_is_peer_exist(mac)) {
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, mac, 6);
    peerInfo.channel = 1;
    peerInfo.encrypt = false;
    if (esp_now_add_peer(&peerInfo) == ESP_OK) {
      Serial.printf("Added new peer: %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
      Serial.println("Failed to add new peer");
    }
  }
}

// ---------- ACK SUPPORT ----------
void sendAck() {
  addPeerIfNeeded(masterMac);
  const char ackMsg[2] = {'O', 'K'};
  esp_now_send(masterMac, (uint8_t *)ackMsg, sizeof(ackMsg));
  Serial.println("Sent ACK");
}

// ---------- ESP‑NOW CONFIG RECEPTION ----------
void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len) {
  // Receive configuration commands via ESP-NOW
  if (len < 2) return;
  if (incomingData[0] == 'C') {
    uint8_t newCount = incomingData[1];
    if (newCount == 1 || newCount == 3) {
      // Save sender MAC address as our current master address
      memcpy(masterMac, info->src_addr, 6);
      pendingHxCount = newCount;
      pendingConfigChange = true;
    }
  }
}

//* ---------- SEND CALLBACK ---------- */
void onSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  Serial.print("Send Status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail");
}

/* ---------- SETUP ---------- */
void setup() {

  Serial.begin(115200);
  WiFi.mode(WIFI_STA);

  Serial.println("Pad Booting");

  /* PREFS LOAD */
  prefs.begin("scale", false);
  hxCount = prefs.getUChar("hxCount", 1);
  calibration_factor = prefs.getFloat("cal", calibration_factor);
  offset = prefs.getFloat("offset", 0);

  Serial.print("Loaded cal: ");
  Serial.println(calibration_factor);
  Serial.print("Loaded offset: ");
  Serial.println(offset);

  /* ESP-NOW INIT */
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW Init Failed");
    return;
  }

  esp_now_register_send_cb(onSent);
  esp_now_register_recv_cb(OnDataRecv);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, masterAddress, 6);
  peerInfo.channel = 1;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Peer Add Failed");
    return;
  }

  /* HX711 INIT */
  scaleA.begin(HX_DT_A, HX_SCK_A);
#if MAX_HX711 >= 3
  scaleB.begin(HX_DT_B, HX_SCK_B);
  scaleC.begin(HX_DT_C, HX_SCK_C);
#endif

  strcpy(data.pad, PAD_ID);
  pinMode(BAT_PIN, INPUT);

  /* AUTO-TARE IF NO SAVED OFFSET */
  delay(500);

  if(offset == 0) {
    if (hxCount == 1 && scaleA.is_ready()) {
      Serial.println("No saved offset, auto-taring (1 sensor)...");
      offset = scaleA.read_average(20);
      prefs.putFloat("offset", offset);
    } else if (hxCount == 3 && scaleA.is_ready() && scaleB.is_ready() && scaleC.is_ready()) {
      Serial.println("No saved offset, auto-taring (3 sensors)...");
      offset = scaleA.read_average(20) + scaleB.read_average(20) + scaleC.read_average(20);
      prefs.putFloat("offset", offset);
    }
  }

  Serial.println("Setup complete");
}

/* ---------- LOOP ---------- */
void loop() {
  // Handle pending configuration changes from master
  if (pendingConfigChange) {
    pendingConfigChange = false;
    hxCount = pendingHxCount;
    prefs.putUChar("hxCount", hxCount);
    
    // Reset offset so it auto-tares with the new sensor count on reboot
    offset = 0;
    prefs.putFloat("offset", 0);
    
    Serial.printf("Config changed! HX711 Count set to %d. Sending ACK...\n", hxCount);
    
    // Send ACK multiple times to ensure master gets it
    for (int i = 0; i < 5; i++) {
      sendAck();
      delay(100);
    }
    
    Serial.println("Rebooting child to apply new configuration cleanly...");
    delay(500);
    ESP.restart();
  }

  /* SCALE READ */
  if (hxCount == 1) {
    if(scaleA.is_ready()){
      float rawA = scaleA.read_average(10);
      float weight = rawA - offset;
      data.weight = weight;
      data.wA = rawA;
      data.wB = 0;
      data.wC = 0;
      Serial.print("Raw: "); Serial.print(rawA);
      Serial.print(" | Weight: "); Serial.println(weight);
    }
  } else {
    // Read each sensor if ready; missing sensors get 0
    float rawA = 0, rawB = 0, rawC = 0;
    if (scaleA.is_ready()) rawA = scaleA.read_average(10);
    if (scaleB.is_ready()) rawB = scaleB.read_average(10);
    if (scaleC.is_ready()) rawC = scaleC.read_average(10);
    float rawTotal = rawA + rawB + rawC;
    float weight = rawTotal - offset;
    data.weight = weight;
    data.wA = rawA;
    data.wB = rawB;
    data.wC = rawC;
    Serial.print("Raw Total: "); Serial.print(rawTotal);
    Serial.print(" | Weight: "); Serial.println(weight);
  }

  /* BATTERY */
  int rawBat = analogRead(BAT_PIN);
  float voltage = (rawBat / 4095.0) * 3.3 * 2.0;
  data.battery = voltage;

  /* TRANSMIT DATA */
  addPeerIfNeeded(masterMac);
  esp_now_send(masterMac, (uint8_t *)&data, sizeof(data));

  /* SERIAL COMMANDS (OPTIONAL DEBUG CONTROL) */
  if(Serial.available()){
    char c = Serial.read();

    if(c == 't'){  // tare
      Serial.println("Manual tare...");
      if (hxCount == 1) {
        offset = scaleA.read_average(20);
      } else {
        offset = scaleA.read_average(20) + scaleB.read_average(20) + scaleC.read_average(20);
      }
      prefs.putFloat("offset", offset);
    }

    if(c == 'c'){  // reset calibration
      Serial.println("Reset calibration");
      calibration_factor = -3685.4;
      prefs.putFloat("cal", calibration_factor);
    }
  }

  delay(100);
}
