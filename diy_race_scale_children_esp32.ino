/*
traviscea DIY Race Scales – Version 1.0
Copyright (c) 2026 Travis Way
*/

#include <WiFi.h>
#include <esp_now.h>
#include <Preferences.h>
#include "HX711.h"

/* ---------- PAD ID ---------- */
#define PAD_ID "RR"

/* ---------- HX711 PINS ---------- */
#define HX_DT 4
#define HX_SCK 5
#define BAT_PIN 34

HX711 scale;
Preferences prefs;

/* ---------- CAL + OFFSET ---------- */
float calibration_factor = -3685.4;
float offset = 0;

/* ---------- ESP-NOW DATA ---------- */
typedef struct {
  char pad[4];
  float weight;
  float battery;
} ScaleData;

ScaleData data;

/* ---------- MASTER MAC ADDRESS ---------- */
/* Replace with MAC of your ESP32-S3
Example, if your brain/main esp32 outputs: AP MAC: 4E:DD:76:6F:A5:45
then your masterAddress =
uint8_t masterAddress[] = {0x4E,0xDD,0x76,0x6F,0xA5,0x45};
*/
uint8_t masterAddress[] = {};


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

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, masterAddress, 6);
  peerInfo.channel = 1;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Peer Add Failed");
    return;
  }

  /* HX711 INIT */
  scale.begin(HX_DT, HX_SCK);

  strcpy(data.pad, PAD_ID);

  pinMode(BAT_PIN, INPUT);

  /* AUTO-TARE IF NO SAVED OFFSET */
  delay(500);

  if(offset == 0 && scale.is_ready()){
    Serial.println("No saved offset, auto-taring...");
    offset = scale.read_average(20);
    prefs.putFloat("offset", offset);
  }

  Serial.println("Setup complete");
}

/* ---------- LOOP ---------- */
void loop() {

  if(scale.is_ready()){

    float raw = scale.read_average(10);

    float weight = raw - offset;

    data.weight = weight;

    Serial.print("Raw: ");
    Serial.print(raw);
    Serial.print(" | Weight: ");
    Serial.println(weight);
    Serial.println(offset);
  }

  /* BATTERY */
  int rawBat = analogRead(BAT_PIN);
  float voltage = (rawBat / 4095.0) * 3.3 * 2.0;
  data.battery = voltage;

  /* SEND */
  esp_now_send(masterAddress,(uint8_t*)&data,sizeof(data));

  /* SERIAL COMMANDS (OPTIONAL DEBUG CONTROL) */
  if(Serial.available()){
    char c = Serial.read();

    if(c == 't'){  // tare
      Serial.println("Manual tare...");
      offset = scale.read_average(20);
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
