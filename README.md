# DIY ESP32 Race Scales

A fully wireless, low-cost corner-weight scale system built using ESP32 boards, HX711 load cell amplifiers, and a simple web interface.

Build your own race scales for approximately $80–$100 instead of spending $1000+ on commercial systems.

---

## Demo / Build Video

https://youtu.be/h9uxLq12wOU

---

## Features

- Wireless communication using ESP-NOW between pads and master  
- Real-time corner weights (FL / FR / RL / RR)  
- Mobile-friendly web UI (no app required)  
- Battery monitoring for each pad(coming soon - batteries lasts 8-10hrs so was not in a hurry to do this)
- Stability locking (auto-detect when weight settles)  
- Unit switching (lbs / kg)  
- Calibration and zeroing from browser  
- Smart filtering for stable readings  

---

## Hardware Required

### Per Pad (x4)
- ESP32 (Lite or Dev board)  
- HX711 amplifier  
- 4 load cells (or a single combined platform)  
- Wiring and connectors  
- Battery (optional)  

### Master (Controller)
- ESP32 (recommended: ESP32-S3 or ESP32 Dev board)  
- Optional HX711 (for FL local pad)  
- WiFi capability (runs its own access point)  

---

## Architecture

- Each pad ESP32 reads load cell data and transmits it  
- Master ESP32:
  - Receives all pad data  
  - Applies calibration  
  - Hosts the web interface  

---

## How It Works

1. Each pad reads raw load cell data via HX711  
2. Pads apply offset (tare) locally  
3. Data is transmitted via ESP-NOW  
4. Master ESP32:
   - Applies calibration  
   - Filters and stabilizes readings  
   - Serves data through a web interface  

---

## Web Interface

Connect to WiFi:

- SSID: `DIY_Race_Scales`  
- Password: `12345678`  

Open in browser: http://192.168.4.1



### Controls

- ZERO: Tare all pads  
- CAL: Calibrate individual pad  
- Units: Toggle between lbs and kg  

---

## Calibration

1. Zero the system  
2. Place a known weight on a pad  
3. Click CAL  
4. Enter:
   - Pad (FL / FR / RL / RR)  
   - Known weight  

---

## Setup

### 1. Flash Firmware

- Upload pad firmware to each pad ESP32  
- Upload master firmware to the main ESP32  

### 2. Configure MAC Address

On pads:

```cpp
uint8_t masterAddress[] = {0xXX,0xXX,0xXX,0xXX,0xXX,0xXX};
```
Retrieve the MAC address from the master ESP32 serial output.

### 3. Set Pad ID

```cpp
#define PAD_ID "FR"   // Change per board
```

## Important Notes

## Stability Tips

- Let scales settle before zeroing
- Use solid ground wiring
- Add capacitors to HX711 (0.1µF + 10µF)
- Avoid long signal wires

---

## Known Issues / Troubleshooting

### Jumping Values

**Check:**
- HX711 wiring
- Grounding
- Offset correctness
- Packet validation

### Watchdog Resets (ESP32)

Add:

```cpp
delay(1);
WiFi.setSleep(false);
```


## Example Use Cases

- Track day corner balancing
- Suspension tuning
- Weight distribution analysis
- DIY race car setup

---

## Inspiration

This project was created as an affordable, open-source alternative to professional race scales, which typically cost $1000 or more.

---

## Contributing

Contributions are welcome, including:

- UI improvements
- Filtering algorithms
- BLE or logging features
- Hardware variations

---

## License

This project is source-available and free for personal and educational use. Commercial use is NOT permitted.

---

## Support

If this project helped you:

- Star the repository
- Share your build
- Contribute improvements
