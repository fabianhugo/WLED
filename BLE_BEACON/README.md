# BLE_BEACON

Minimal proximity beacon firmware for ESP32-S3 / ESP32-C3.  
No WLED, no WiFi — just BLE 5 Coded PHY advertising + scanning and a WS2812B strip.

Each node simultaneously advertises its presence and scans for peers.  
When a peer is detected above the RSSI threshold the LEDs shift from dim white → blue → magenta.

---

## Hardware

| Item | Notes |
|---|---|
| MCU | ESP32-S3 or ESP32-C3 |
| LED strip | WS2812B (or any FastLED-compatible RGB strip) |
| LED data pin | GPIO 8 (change `LED_PIN` in `main.cpp`) |
| Power | 5 V via USB or regulated supply; strip current depends on length and brightness |

---

## Build & flash

Requires [PlatformIO](https://platformio.org/) (CLI or VS Code extension).

```sh
# S3 board
pio run -e beacon_s3 -t upload

# C3 board
pio run -e beacon_c3 -t upload

# Monitor serial output (115200 baud)
pio device monitor
```

First build downloads NimBLE-Arduino and FastLED automatically.

---

## Tuning

All knobs are at the top of `src/main.cpp`:

| Constant | Default | Effect |
|---|---|---|
| `GROUP_ID` | `0xBE` | Only beacons with the same value see each other. Change if you have two independent groups in the same space. |
| `RSSI_NEAR` | `-80 dBm` | Signal level above which a peer counts as "close". Raise to shrink detection radius, lower to expand it. |
| `TIMEOUT_MS` | `8000 ms` | How long without a packet before a peer is considered gone. |
| `ADV_INTERVAL` | `160 × 0.625 ms = 100 ms` | How often each node advertises. Lower = faster detection, higher current. |
| `TX_POWER_DBM` | `9 dBm` | Transmit power. Reduce for shorter range and lower peak current. |
| `ALPHA` | `0.2` | RSSI smoothing factor. Lower = smoother but slower to react to movement. |
| `LED_PIN` | `8` | GPIO for WS2812B data line. |
| `NUM_LEDS` | `10` | Strip length. |
| `BRIGHTNESS` | `60` | Global FastLED brightness (0–255). Biggest single lever for power consumption. |

---

## Range & power

With PCB trace antenna and default settings:

| Situation | Approximate range |
|---|---|
| Open air | 200–400 m |
| Office / indoors | 20–50 m |
| Through concrete walls | 5–15 m |

Radio current draw (excluding LEDs):

| | mA average |
|---|---|
| Advertising only | 2–4 mA |
| Scanning 50 % duty cycle | 10–15 mA |
| Both combined | 12–18 mA |

LED current is separate and dominates: roughly `NUM_LEDS × 6 mA` at `BRIGHTNESS=60`.

---

## Debugging

Serial output (115200 baud) prints one line per 100 ms:

```
nearby=1  rssi=-74  smooth=-76.3
```

**Coded PHY is not visible to standard phone BLE scanners** (iOS/Android only support legacy BLE 4.x advertising). To debug RSSI from a phone, temporarily change `adData.setLegacyAdvertising(true)` — this switches to BLE 4.x range but makes the packet visible to nRF Connect etc.

---

## How it works

1. On boot each node starts an **extended BLE 5 advertisement** on Coded PHY (S=8, 125 kbps).  
   The payload is 3 bytes of manufacturer data: `[0xFF, 0xFF, GROUP_ID]`.

2. Simultaneously each node runs a **passive extended scan** on Coded PHY, listening for advertisements matching `GROUP_ID`.

3. When a matching packet arrives the scan callback records `millis()` and the raw RSSI.

4. `loop()` applies an exponential moving average to the RSSI and maps it to an LED colour:
   - No peer (or peer gone > `TIMEOUT_MS`): dim white
   - Peer detected but distant: blue
   - Peer above `RSSI_NEAR`: magenta

Coded PHY improves receiver sensitivity by ~5 dB over standard BLE 1M, which is roughly 1.8× more range at the same TX power.
