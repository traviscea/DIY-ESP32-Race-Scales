/*
 * traviscea DIY Race Scales – Version 1.1 (accuracy patch)
 * Based on Version 1.0, Copyright (c) 2026 Travis Way
 *
 * CHANGES vs 1.0 (pad side):
 * - Pad now sends RAW HX711 counts (~10 Hz) instead of a blocking
 *   10-sample average. All filtering, tare, and calibration live on
 *   the master.
 * - Wire format upgraded: ScalePacket (ScaleProtocol.h) replaces the
 *   old char-pad struct. Packet carries magic bytes + protocol version
 *   so mismatched firmware is detected immediately.
 * - Pad identity is a typed numeric PadId stored in NVS — one binary
 *   for all pads; set/change over serial (no per-board source edit).
 * - Every packet carries a per-pad sequence counter so the master can
 *   measure packet loss.
 * - Empty (all-zero) masterAddress produces a FATAL startup message
 *   and halts — it never invents an address.
 * - Battery via analogReadMilliVolts() (uses ESP32 ADC cal data).
 *
 * Flash master and ALL pads together — v1.0 and v1.1 use incompatible
 * wire formats and will silently produce garbage if mixed.
 */

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include "HX711.h"
#include "ScaleProtocol.h"

/* ── ESP-NOW callback arg type (core 2.x vs 3.x) ───────────────────────
 * Core 3.x passes wifi_tx_info_t*; core 2.x passes the peer MAC as
 * uint8_t*.  A version-selected typedef gives onSent() one un-guarded
 * signature.  This must sit ABOVE the first function definition: the
 * Arduino .ino prototype generator hoists all prototypes to that point
 * and does not evaluate #if guards, so a guarded signature (or a
 * typedef declared any later) fails to compile on core 2.x.           */
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
typedef wifi_tx_info_t sp_tx_info_t;
#else
typedef uint8_t sp_tx_info_t;          /* core 2.x: const uint8_t *mac */
#endif

/* ── PAD IDENTITY — stored in NVS, one binary for all pads ─────────────
 * FR / RL / RR  (FL is wired directly to the master; do not flash this
 * firmware on the master board).
 *
 * First boot (or after erase): the pad prompts on serial (115200) —
 * type FR, RL, or RR + Enter. The choice persists in NVS.
 * To change later: type "ID FR" (etc.) into serial at any time; the
 * pad saves and reboots. No per-board source edit or reflash needed.  */
static Preferences padPrefs;
static uint8_t thisPad = 0;      /* PadId value, loaded from NVS */

static uint8_t parsePadId(const String &s) {
    if (s == "FR") return (uint8_t)PAD_FR;
    if (s == "RL") return (uint8_t)PAD_RL;
    if (s == "RR") return (uint8_t)PAD_RR;
    return 0;
}

static const char *padIdName(uint8_t id) {
    switch (id) {
        case PAD_FR: return "FR";
        case PAD_RL: return "RL";
        case PAD_RR: return "RR";
        default:     return "??";
    }
}

/* ── HX711 PINS ─────────────────────────────────────────────────────── */
#define HX_DT   4
#define HX_SCK  5
#define BAT_PIN 34

HX711 scale;

/* Packet reused every loop iteration; header fields are constant. */
static ScalePacket pkt;

/* ── MASTER MAC ADDRESS ─────────────────────────────────────────────
 * Replace with the AP MAC printed by the master ESP32 on its serial
 * output at first boot ("AP MAC: XX:XX:XX:XX:XX:XX").
 *
 * Leaving all bytes zero will produce a FATAL error at startup and
 * halt this pad — it will NOT transmit to address 00:00:00:00:00:00.
 */
static const uint8_t masterAddress[6] = {0, 0, 0, 0, 0, 0};

static unsigned long lastBatRead = 0;
static float battVolts = 0.0f;

/* ── ESP-NOW send callback ──────────────────────────────────────────
 * Signature type sp_tx_info_t is version-selected near the includes.  */
void onSent(const sp_tx_info_t *info, esp_now_send_status_t status) {
    (void)info;
    /* Uncomment for link debugging:
    Serial.println(status == ESP_NOW_SEND_SUCCESS ? "TX ok" : "TX fail"); */
}

/* ── setup ─────────────────────────────────────────────────────────── */
void setup() {
    Serial.begin(115200);

    /* ── Pad identity from NVS; interactive first-boot config ────────── */
    padPrefs.begin("pad");
    thisPad = padPrefs.getUChar("id", 0);
    if (parsePadId(padIdName(thisPad)) == 0) {   /* unset or corrupt */
        Serial.println("PAD ID not configured.");
        Serial.println("Type FR, RL, or RR + Enter to set it (saved to NVS):");
        while (thisPad == 0 || parsePadId(padIdName(thisPad)) == 0) {
            if (Serial.available()) {
                String s = Serial.readStringUntil('\n');
                s.trim(); s.toUpperCase();
                uint8_t id = parsePadId(s);
                if (id) {
                    thisPad = id;
                    padPrefs.putUChar("id", id);
                    Serial.print("Saved pad ID: ");
                    Serial.println(padIdName(id));
                } else {
                    Serial.println("Invalid — type FR, RL, or RR:");
                }
            }
            delay(50);
        }
    }

    /* ── Guard: refuse to run with an unconfigured master MAC ────────
     * All-zero is the default value; a real MAC will have at least one
     * non-zero byte.  Halting here is safer than silently transmitting
     * to the broadcast/zero address and looking like it is working.   */
    static const uint8_t zeroMAC[6] = {0, 0, 0, 0, 0, 0};
    if (memcmp(masterAddress, zeroMAC, 6) == 0) {
        Serial.println("FATAL: masterAddress[] is all-zero.");
        Serial.println("  1. Flash the master board first and read its");
        Serial.println("     AP MAC from the serial output.");
        Serial.println("  2. Fill masterAddress[] in this source file.");
        Serial.println("  3. Re-flash this pad.");
        Serial.println("  This pad will not transmit until the MAC is set.");
        while (true) {
            delay(1000);
        }
    }

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);                            /* avoids TX gaps */
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);  /* match master AP */

    Serial.print("Pad ID: ");
    Serial.print(padIdName(thisPad));
    Serial.print(" (");
    Serial.print(thisPad);
    Serial.print(")  booting v1.1  ScaleProtocol 0x");
    Serial.println(SP_VERSION, HEX);
    Serial.println("Type \"ID FR\" / \"ID RL\" / \"ID RR\" + Enter at any time to reassign.");

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW Init Failed — halting");
        while (true) delay(1000);
    }
    esp_now_register_send_cb(onSent);

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, masterAddress, 6);
    peerInfo.channel = 1;
    peerInfo.encrypt = false;
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("Peer Add Failed — halting");
        while (true) delay(1000);
    }

    /* HX711: gain 128, RATE pin low = 10 SPS (low-noise mode).
     * Do NOT jumper to 80 SPS for load-cell scales.                   */
    scale.begin(HX_DT, HX_SCK);

    /* Pre-fill the constant header fields once. */
    pkt.magic0  = SP_MAGIC0;
    pkt.magic1  = SP_MAGIC1;
    pkt.version = SP_VERSION;
    pkt.padId   = thisPad;
    pkt.seq     = 0;

    pinMode(BAT_PIN, INPUT);
    analogSetPinAttenuation(BAT_PIN, ADC_11db);

    /* Discard first few conversions — HX711 output wanders on power-up. */
    for (int i = 0; i < 4; i++) {
        while (!scale.is_ready()) delay(5);
        scale.read();
    }

    Serial.println("Setup complete");
}

/* ── loop ──────────────────────────────────────────────────────────── */
void loop() {
    /* Runtime pad reassignment: "ID FR" / "ID RL" / "ID RR" + Enter.
     * Saves to NVS and reboots so every subsystem picks up the new
     * identity cleanly.                                               */
    if (Serial.available()) {
        String s = Serial.readStringUntil('\n');
        s.trim(); s.toUpperCase();
        if (s.startsWith("ID ")) {
            uint8_t id = parsePadId(s.substring(3));
            if (id) {
                padPrefs.putUChar("id", id);
                Serial.print("Pad ID saved: ");
                Serial.print(padIdName(id));
                Serial.println(" — rebooting");
                delay(200);
                ESP.restart();
            } else {
                Serial.println("Invalid — use \"ID FR\", \"ID RL\", or \"ID RR\"");
            }
        }
    }

    /* One conversion per HX711 cycle (~100 ms at 10 SPS).
     * Master accumulates ~3 s of samples per pad (rolling window) and
     * computes a 50% trimmed mean there — better statistics, and
     * CAL/ZERO become instant because the window is always fresh.     */
    if (scale.is_ready()) {
        pkt.raw = (float)scale.read();
        pkt.seq++;   /* wraps at 255 — master derives per-pad loss */

        if (millis() - lastBatRead > 2000) {
            lastBatRead = millis();
            /* mV at pin * 2 (voltage divider) → volts */
            battVolts = analogReadMilliVolts(BAT_PIN) * 2.0f / 1000.0f;
        }
        pkt.battery = battVolts;

        esp_now_send(masterAddress,
                     reinterpret_cast<const uint8_t *>(&pkt),
                     sizeof(pkt));
    }

    delay(5);
}
