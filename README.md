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
- 12 load cells 
- Wiring and connectors(comes with hx711's)  
- Battery(optional)

Disclaimer: Some of the links below are affiliate links, which means I may earn a commission at no extra cost to you.

As an Amazon Associate I earn from qualifying purchases.

Esp32 lite(4 pack) = $17
https://www.amazon.com/Bluetooth-Development-Antenna-MicroPython-Arduino/dp/B0D7V9GKHV?pd_rd_w=qu7ib&content-id=amzn1.sym.a1c22caf-2d4a-4c58-b920-210b8f6b9234&pf_rd_p=a1c22caf-2d4a-4c58-b920-210b8f6b9234&pf_rd_r=1K3H3M4S7FN856PNXRZX&pd_rd_wg=qjO1W&pd_rd_r=3da1b217-4232-4600-9ed4-17c113e8f9a5&pd_rd_i=B0D7V9GKHV&psc=1&linkCode=ll2&tag=traviscea05-20&linkId=ddd1caaf149a3d7df813d882b684685d&language=en_US&ref_=as_li_ss_tl

Alternate link for esp32 lites if others are sold out(you need 4 esp32s):
https://www.amazon.com/V1-0-0-Bluetuth-Development-ESP-32-MicroPython/dp/B0FBGKK484?crid=17WSD3V9NQ413&dib=eyJ2IjoiMSJ9.0mMutUw27FQh3oBplnQp1ehm2_8dKdtx9ewAsDJyPX2TvjIiAv0tHRh221LBU5XGbP78VNZbeMEqlELbOma7iFtdZbbSTFPk0mZLl5Yqcgoe_Exm8BTwBnzIoaSUUZM4gp5d5-_vyUAqpObyvbEQcYg1qrUN0IIQcIhH8B1XkMQyrHryTqTa_Y1MONCmQ7wu_7TbilEwf21As29RzhMz1QMHID95MqGbWyS1HaGAPak.Z9JD3uAwjzWMCq5MB7yQeq_6o0ThsDc1C0nS0Tonipk&dib_tag=se&keywords=esp32+lite+4&qid=1777433267&sprefix=esp32+lite+%2Caps%2C191&sr=8-17&linkCode=ll2&tag=traviscea05-20&linkId=6603be8a29e5e0e344c7640111766d7e&language=en_US&ref_=as_li_ss_tl

HX711(4 pack) = $7 
https://www.amazon.com/dp/B0BLND4VF6?social_share=cm_sw_r_cp_ud_dp_XQKKQSD12G3YQSVKNPX8&linkCode=ll2&tag=traviscea05-20&linkId=eac704287f110a03faf7c303873c94fd&language=en_US&ref_=as_li_ss_tl

Battery Pack(4 pack) = $17 
https://www.amazon.com/MakerHawk-Rechargeable-Protection-Connector-Electronic/dp/B0D9K7HQHT?_encoding=UTF8&pd_rd_w=EenFZ&content-id=amzn1.sym.a9c4acee-9ca0-46be-bae3-532a2b4b0d29%3Aamzn1.symc.5a16118f-86f0-44cd-8e3e-6c5f82df43d0&pf_rd_p=a9c4acee-9ca0-46be-bae3-532a2b4b0d29&pf_rd_r=J8X3P7FS184DMQ0HEJRD&pd_rd_wg=f3ZF4&pd_rd_r=d263d5cc-fec2-4817-bc85-5528fa1dd645&th=1&linkCode=ll2&tag=traviscea05-20&linkId=37cb3096cedf42353d0263f6f533e836&language=en_US&ref_=as_li_ss_tl

JST battery connectors(for more length) = 6$
https://www.amazon.com/dp/B0FW4Y4FSL?&linkCode=ll2&tag=traviscea05-20&linkId=2dc5248c0eb4ad13a03cd092e03af09c&language=en_US&ref_=as_li_ss_tl

Load cells(48, buy 3 sets of 20) = $45 
https://www.amazon.com/dp/B09KGSGL18?social_share=cm_sw_r_cp_ud_dp_BH1Y8XC925MPHF1E74TW&linkCode=ll2&tag=traviscea05-20&linkId=46be772bed19824dc515417869be07c8&language=en_US&ref_=as_li_ss_tl

Wood 2x12x8 = $20 
***I used 18in sections here for my 245 tires. You may want more or less distance depending on your tire width.


## 3d Print files
PETG filament I used: [Amazon link](https://www.amazon.com/OVERTURE-Filament-Consumables-Dimensional-Accuracy/dp/B07PGYHYV8?adgrpid=191564566452&dib=eyJ2IjoiMSJ9.uV75SqbTLCgAy78vKWUZ0VUSWFtXi-5DzJINPhuQ9V8zrYwjYe66q0YGr8sG1nKs_rOAuqN59tx6vZ4L2lL_fWAbo0rkwGZ-Y7fKun_2db5yr8mI7cER3wtzvZrGoTCWxbB5r1Yics1vA5P2UVPJt1qCaX9FMctx4sXoK_W5GvG-Gf7wX5gsMw5-k1r5VXXaxdwAc9fhtHGWi4gdZoppUZdZbnXRd2UimD1gf57p_nyHo_3RnHGDTfJXPCMz0ZKtY-bOKHTBhSEb3nVChVI1alxOgWRryRfuJ16npHx-8OI.Pm-9rvDukxqjzlyPPor2StnBqBybCGUJVkUTXMVSx7Q&dib_tag=se&hvadid=779547513984&hvdev=c&hvexpln=0&hvlocphy=9194515&hvnetw=g&hvocijid=10685258310100459603--&hvqmt=e&hvrand=10685258310100459603&hvtargid=kwd-307270553510&hydadcr=8497_13833035_2116124&keywords=petg%2Bblack&mcid=471a93842d0535b09ebc346ed5f9d949&qid=1777687529&s=industrial&sr=1-4&th=1&linkCode=ll2&tag=traviscea05-20&linkId=5f6861a79bd44ac9079275ba2786f080&language=en_US&ref_=as_li_ss_tl)

Load cell holder:
<br>
Bambu 3mf: https://makerworld.com/en/models/2728603-diy-race-scale-50kg-load-cell-mounts#profileId-3023804

Stl file: https://drive.google.com/file/d/1BfqhXBpTUUBheZa0adTtnxa-Y_LcbjTP/view?usp=sharing


Box for Esp32:
<br>
Bambu 3mf: https://makerworld.com/en/models/2737068-diy-race-scale-esp32-mount-box#profileId-3034590

Stl file: https://drive.google.com/file/d/1Z9YkdcgCBxQTEjgYmJWulF77Iv5ffJcg/view?usp=sharing


## Wiring
Make the wheatstone bridges as I have described in the video above(black to black, white to white). 
Then wire the all the red wires from each corner as shown below

```
PAD layout:
______________
| A        C |
|            |
| B        D |
______________

PAD(Labeled above) → HX711
A → E+
D → E-
B → A-
C → A+

HX711 → ESP32
VCC → 3.3V
GND → GND
DT  → GPIO 4
SCK → GPIO 5
```

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
