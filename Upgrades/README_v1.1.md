# DIY ESP32 Race Scales — v1.1 Accuracy Patch

Firmware changes and operating procedure for the accuracy-focused fork of
[traviscea/DIY-ESP32-Race-Scales](https://github.com/traviscea/DIY-ESP32-Race-Scales).

**Flash the master and all three remote pads together.** Protocol `0x12`
uses a 13-byte versioned packet with raw HX711 counts and a sequence counter.
The master rejects v1.0 and pre-sequence v1.1 packets rather than interpreting
them as weights.

---

## What changed and why

| Change | Error it removes |
|---|---|
| Pads stream raw single samples at ~10 Hz; master keeps a 32-sample (~3 s) rolling window per pad with a 50% trimmed mean | HX711 spike outliers polluting plain averages; 1 Hz update lag |
| Multi-point piecewise calibration (up to 3 points/pad, NVS) | Nonlinearity of 50 kg half-bridge cells — a single cal point taken with a light weight and extrapolated to a 500–700 lb corner is the largest error source in the v1.0 design |
| CAL/ZERO use the trimmed mean of the full window; CAL rejected unless settled | Single-packet calibration snapshots |
| Statistical stability lock (window stddev < 0.35 lb → lock and hold value; > 1.0 lb move → unlock) | Cosmetic lock icon with a stale reference |
| Full float precision through all aggregation; rounding only at display | 0.5 lb per-corner quantization and zero-snap feeding cross-weight % |
| All tare on the master; pads are stateless | Months-old NVS tare on pads drifting with temperature |
| Rotation calibration (`ROT`): solves per-pad gain trims using the car as a transfer standard | Pad-to-pad gain mismatch between load-cell clusters — the error that directly corrupts cross weight |
| `SAVE` button: downloads a full JSON snapshot to the phone/laptop | No record of weigh sessions |
| Cross weight = RF + LR | v1.0 used LF + RR (opposite of standard convention) |
| Battery via `analogReadMilliVolts()`; HX711 warm-up conversions discarded | ADC nonlinearity; power-up wander |
| Pad identity stored in NVS, set/changed over serial — one binary for all pads | Wrong `#define` flashed to the wrong board |
| Per-pad packet sequence counter; master reports `rx` / `lost` per pad in `/snapshot` | "The pad seems flaky" with no number behind it |
| `/data` JSON built with a single `snprintf` into a static buffer | Heap fragmentation from ~30 String concats at 5 req/s over a multi-hour session |
| `/reset` flushes the sample windows and link stats, not just cal state | Stale calibrated values lingering on screen after a reset |
| Snapshot clears the lock flag for offline pads before recording | A stale `locked: true` saved against a pad that dropped mid-session |

---

## Operating procedure

### One-time (per weighing location)

1. **Level and height-match the plates.** Use a digital angle gauge zeroed
   on one reference plate; shim the other three to within ~0.3° of it.
   Then match plate heights to ~1–2 mm (straightedge or laser line).
   Height mismatch shifts *real* load between corners — a few mm can
   outweigh the entire electronics error budget. Relative agreement
   between plates is what matters, not absolute level.
2. Mark tire landing spots on each plate so the contact patch hits the
   same cell-cluster region every placement.

### Weight calibration (per pad, occasionally)

1. Empty pads → **ZERO**.
2. **CAL** → pad → known weight. Add up to 3 points per pad; use two
   loads bracketing your actual corner weights (e.g. ~150 lb and ~500 lb).
   Enter weight `0` to clear a pad's points.
3. Below the first calibration point, readings use a slope through the
   origin (forces cal through zero near tare); above the last point,
   readings extrapolate with the last segment's slope.

### Rotation calibration (after weight cal, or standalone)

Uses the car itself to measure pad-to-pad gain differences. Corner
weights must be identical across placements: same fuel, same
driver/ballast, sway bar state, tire spots.

1. **ROT → 1 (start)**.
2. Lower the car onto the plates, bounce/settle each corner, steering
   straight, wait for all four lock icons. **ROT → 2 (record)** —
   confirm the pad→station map (defaults assume one clockwise rotation
   per placement: FL→FR→RR→RL).
3. Lift the car, rotate the physical pads one station clockwise, repeat.
   2 placements minimum, 3–4 better. The record response shows total
   weight delta vs. the first placement — if it drifts more than a few
   lb, something changed and the session is suspect.
4. **ROT → 3 (solve)**. Reported trims are your real cluster-to-cluster
   gain variance; the max residual is your true repeatability floor.
   Trims persist in NVS and apply on top of weight cal.
   **ROT → 5** clears trims.

If you re-shim the floor setup, don't reuse placements recorded before
the shim. Re-running weight CAL leaves trims applied (usually correct);
if you redo weight cal on all four pads from scratch, clear trims and
re-run a rotation session.

### Every weigh session

1. Power on, let everything warm up a few minutes (HX711 zero drifts
   with temperature).
2. Empty pads → **ZERO**.
3. Lower/roll the car on with driver or equivalent ballast, known fuel
   load, steering dead straight. Bounce/settle each corner.
4. Wait for **all four locks** — the numbers are statistically settled.
5. **SAVE** to capture a JSON snapshot (corner weights, cross %, noise,
   cal state, trims) with a session note.

### Accuracy expectations

Target for corner balancing is cross weight 50.0% ± 0.5%. On a ~2,500 lb
car that's ~12 lb, so ±3–5 lb per corner is fully sufficient — and the
practical limit is suspension stiction and placement repeatability
(±5–10 lb), not the electronics. Fuel load (~6 lb/gal, rear-biased) and
driver ballast dwarf scale error; control those first.

---

## HTTP API

| Endpoint | Purpose |
|---|---|
| `/data` | Live JSON (UI poll) |
| `/tare` | Zero all online pads (trimmed window mean) |
| `/calibrate?pad=FL&weight=150` | Add cal point; `weight=0` clears pad |
| `/rotcal?cmd=start\|record&map=..\|solve\|status\|clear` | Rotation cal |
| `/snapshot` | Full setup-sheet JSON |
| `/reset` | Wipe all cal, offsets, trims |

---

## ⚠️ Protocol compatibility

v1.1 uses a new wire format (`ScaleProtocol.h`, `SP_VERSION = 0x12`,
13-byte packet with a per-pad sequence counter). The packet carries
explicit magic bytes and a version field so a version mismatch is
detected immediately rather than producing silent garbage readings.
Earlier formats (`0x10` v1.0 and the pre-seq `0x11`) are rejected.

**Flash all four nodes (master + three remote pads) at the same time.**
A v1.0 pad sending to a v1.1 master (or vice-versa) will be rejected by
the validator on every packet — the pad will appear permanently offline.

---

## Master MAC — required before flashing pads

The remote-pad firmware (`diy_race_scale_children_esp32.ino`) ships with
`masterAddress[]` set to all zeros.  Flashing a pad without filling in the
real master MAC produces a **FATAL** startup message on the pad's serial
output and halts the pad — it will not transmit to the zero address.

**Procedure:**

1. Flash and power the **master** board first.
2. Open its serial monitor (115200 baud).
3. Copy the line `AP MAC: XX:XX:XX:XX:XX:XX`.
4. Paste those six bytes into `masterAddress[]` in the child sketch:
   ```cpp
   static const uint8_t masterAddress[6] = {0xXX,0xXX,0xXX,0xXX,0xXX,0xXX};
   ```
5. Flash each child pad.

## Pad identity — set over serial, not in source

The child firmware is **one binary for all three remote pads**. The pad
ID (FR / RL / RR) lives in NVS, not in a `#define`:

1. On first boot (or after a flash erase) the pad prompts on serial
   (115200): type `FR`, `RL`, or `RR` + Enter. The choice persists.
2. To reassign a pad later, type `ID FR` / `ID RL` / `ID RR` + Enter at
   any time — the pad saves the new identity and reboots.

Each pad prints `Pad ID: XX (n)  booting v1.1` on startup to confirm
its identity. Label the physical plates to match.

---

## Verification and build

Use the single verification entry point from the repository root. It runs
the host protocol/source checks, installs PlatformIO when necessary, and
then compiles both authoritative `.ino` sketches with pinned dependencies:

```bash
# Full gate; default board is lolin32_lite
python Upgrades/tools/verify.py

# Override board
python Upgrades/tools/verify.py --board esp32dev

# Fast host-only gate
python Upgrades/tools/verify.py --skip-build
```

Pinned versions (in `build_check.py`):

| Item | Pinned version |
|---|---|
| PlatformIO platform | `espressif32@6.5.0` |
| Arduino ESP32 core | 2.0.14 (bundled with above) |
| HX711 library | `bogde/HX711@0.7.5` |
| Default board ID | `lolin32_lite` |

The board ID defaults to `lolin32_lite`, which matches the classic ESP32
Rev1, 4 MB flash, LiPo-interface board family listed in the BOM. The exact
ACEIRMC clone schematic is not published, so use `--board` to override the
profile if the delivered board identifies itself differently.

The host gate verifies the 13-byte packet layout, validator behavior
(length, magic, version, and pad ID), sequence tracking, and source-level
queue/freshness invariants in both `.ino` files and `ScaleProtocol.h`.

---

## Architecture — queue discipline

The ESP-NOW receive callback (`onReceive`) runs in the **WiFi task**
context, not a hardware ISR, but it must not block and must not mutate
shared pad state that `loop()` reads mid-update.

v1.1 enforces a clean ownership boundary:

| Where | Owns |
|---|---|
| `onReceive` callback | Validate packet (`scalePacketValid`), copy to `rxQueue` with `xQueueSend(..., 0)` (nonblocking), return |
| `loop()` | Drain `rxQueue` each iteration, call `pushSample`, update battery, recompute stats |

The queue (`rxQueue`, depth 16) is created in `setup()` **before**
`esp_now_register_recv_cb` so the callback always has a valid handle.
Packets are dropped when the queue is full; at 10 Hz
this requires ~1.6 s of missed `loop()` iterations which indicates a
separate hang.

`xQueueSend` (not the `FromISR` variant) is correct here because
the ESP-NOW callback is a task context.

---

## Hardware status

**Not yet validated on physical hardware.** The ordered parts are an
ACEIRMC ESP32 Lite V1.0.0 (classic ESP32 Rev1, CH340G, 4 MB flash, advertised
LiPo interface with up to 500 mA charge current) and a Stemedu HX711 breakout
(advertised 2.6–5.5 V supply range). This confirms that 3.3 V operation is
within the module's advertised range and identifies a compatible build
profile. It does not establish the clone boards' regulator, charger,
battery-divider, or HX711 AVDD circuitry; verify those on the delivered
hardware. No load-cell weigh session has been run with this exact code.
Treat v1.1 as review-ready, not production-ready, until a full session
(zero → calibrate → rotate → weigh) has been confirmed on the bench.

---

## Deferred: NVS migration

v1.0 and v1.1 reuse the `scales` NVS namespace but use different key
schemas. The old v1.0 values are orphaned; v1.1 loads defaults rather than
converting them. A formal migration path (versioned namespace and schema
upgrade on boot) is **deferred**. After upgrading, run `/reset`, then perform
a fresh ZERO, multi-point calibration, and rotation calibration before use.
