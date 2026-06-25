# BLE_BEACON

Minimal proximity beacon firmware for ESP32-S3 / ESP32-C3.
No WLED, no WiFi — just BLE 5 Coded PHY advertising + scanning and a WS2812B strip.

Each node simultaneously advertises its presence and scans for peers.
When no peer is around the strip shows a slowly cycling rainbow; when a peer is
detected the LEDs shift through blue → magenta and **pulse** once a peer is close.

A **long press on the BOOT button** toggles the radio fully on/off (see *Controls*).

---

## Hardware

| Item | Notes |
|---|---|
| MCU | ESP32-S3 (default env targets `lolin_s3_mini`, 4 MB flash / 2 MB PSRAM) or ESP32-C3 |
| LED strip | WS2812B (or any FastLED-compatible RGB strip) |
| LED data pin | GPIO 48 on lolin_s3_mini (change `LED_PIN` in `main.cpp` for other boards) |
| Button | on-board **BOOT** button = GPIO 0 (active-low) |
| Power | 5 V via USB or regulated supply; strip current dominates — see *Range & power* |

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

> **Seeing the boot log:** `Serial` is native USB CDC, so `setup()` waits up to 3 s
> for the monitor to attach. Start `pio device monitor` (or press reset with it open)
> within that window to catch the `TX power applied:` and `BLE beacon started` lines.

---

## Controls

| Action | Effect | Feedback |
|---|---|---|
| **Long-press BOOT (≥ 1 s)** while radio is on | Stops advertising + scanning and **fully powers down the BLE controller** (`NimBLEDevice::deinit`) | **3 red flashes** |
| **Long-press BOOT (≥ 1 s)** while radio is off | Re-initialises the stack and resumes advertising + scanning | **3 green flashes** |

The toggle fires once per hold and re-arms on release. With the radio off the node
neither transmits nor receives — a peer will drop it after its `TIMEOUT_MS` elapses.

---

## LED behaviour

| State | Appearance |
|---|---|
| No peer (or peer gone > `TIMEOUT_MS`) | Slowly cycling **rainbow** across the strip |
| Peer detected, distant | **Blue → magenta** gradient (brighter as RSSI rises toward `RSSI_NEAR`) |
| Peer above `RSSI_NEAR` | **Pulsing magenta** (~1.5 s breathe) |

---

## Tuning

All knobs are at the top of `src/main.cpp`:

| Constant | Default | Effect |
|---|---|---|
| `GROUP_ID` | `0xBE` | Only beacons with the same value see each other. Change for independent groups in one space. |
| `RSSI_NEAR` | `-95 dBm` | Colour-only threshold for "close" (full/pulsing magenta). **Does not** set detection range — any decodable packet marks a peer present. |
| `TIMEOUT_MS` | `8000 ms` | How long without a packet before a peer is considered gone. Must exceed `ADV_INTERVAL` with margin. |
| `ADV_INTERVAL` | `1600 × 0.625 ms = 1000 ms` | How often each node advertises. Lower = faster detection, higher average current. |
| `TX_POWER_DBM` | `20 dBm` | Transmit power; the chip clamps to its ceiling (~+18…+20 dBm). The boot log prints the value actually applied. May exceed regional regulatory limits. |
| `ALPHA` | `0.2` | RSSI smoothing factor. Lower = smoother but slower to react. |
| `LED_PIN` | `48` | GPIO for WS2812B data line. 48 = onboard LED on lolin_s3_mini. |
| `NUM_LEDS` | `10` | Strip length. |
| `BRIGHTNESS` | `60` | Global FastLED brightness (0–255). Biggest single lever for power consumption. |
| `BOOT_BUTTON_PIN` | `0` | GPIO of the radio-toggle button (BOOT). |
| `LONG_PRESS_MS` | `1000` | Hold duration required to toggle the radio. |

Scan duty cycle is **continuous** (window = interval = 100 ms) so the sparse 1 s
advertisements aren't missed and weak edge-of-range packets are caught; this is the
main RX-power lever if you need to reduce consumption. The CPU is pinned to 80 MHz
(`setCpuFrequencyMhz`) — the lowest clock the BLE controller supports — to cut active draw.

---

## Range & power

> All figures below are **rough estimates**, not bench measurements. Actual numbers
> depend heavily on antenna, enclosure, environment, and your strip. Measure your own
> setup before relying on these.

### Range

Detection range is set by TX power + Coded-PHY receiver sensitivity (~-103 dBm) and,
indoors, is usually **limited by 2.4 GHz interference and the board's small PCB/chip
antenna** rather than by link budget. Both nodes at +20 dBm on Coded PHY:

| Situation | Approximate range |
|---|---|
| Open air, line of sight | 50–150 m (antenna-limited; far less than the theoretical Coded-PHY maximum) |
| Office / indoors | 10–30 m |
| Through walls / floors | 5–15 m |

Notes:
- **Frequency / channel choice barely affects range** (~0.3 dB across the band). The
  three advertising channels (37/38/39) are used together for frequency diversity.
- Elevation and keeping the boards away from bodies, metal, and ground planes typically
  helps more than any radio setting.
- An **external antenna** is the real range unlock, but the lolin_s3_mini has only a
  PCB antenna and no u.FL connector.

### Current draw

Powered from 5 V over USB. The **LED strip dominates** — at `BRIGHTNESS=60` a magenta
or rainbow pixel draws roughly 6–10 mA, so 10 px ≈ 40–80 mA. Radio and CPU are smaller:

| Subsystem | Approx. average |
|---|---|
| CPU @ 80 MHz + base (USB CDC active) | ~20–35 mA |
| BLE advertising @ 1 s interval, +20 dBm | ~1–3 mA (tiny duty cycle, brief high-power bursts) |
| BLE continuous Coded-PHY scan (100 % RX) | ~15–25 mA |
| LED strip (10 px @ `BRIGHTNESS=60`) | ~40–80 mA |

Indicative whole-system current (10-pixel strip):

| State | Approx. total @ 5 V |
|---|---|
| Radio **on**, idle rainbow | ~80–130 mA |
| Radio **on**, peer near (pulsing magenta, averages dimmer) | ~70–110 mA |
| Radio **off** (long-press): BLE powered down, rainbow only | ~60–110 mA |

To go lower: reduce `BRIGHTNESS`/`NUM_LEDS` (biggest win), drop the scan duty cycle,
or put the MCU to sleep while the radio is off (not yet implemented).

---

## Debugging

Serial output (115200 baud). On boot:

```
TX power applied: requested=20  adv=20  scan=20 dBm
BLE beacon started  group=0xBE  rssi_near=-95 dBm
```

Then one line per loop (~100 ms):

```
nearby=1  rssi=-74  smooth=-76.3
```

Long-pressing BOOT logs `Radio DISABLED by long press` / `Radio ENABLED by long press`.

If `TX power applied:` shows less than requested, the chip clamped to its hardware
ceiling — that is the true maximum, no software change will exceed it.

**Coded PHY is not visible to standard phone BLE scanners** (iOS/Android only support
legacy BLE 4.x advertising). To debug RSSI from a phone, temporarily set
`adData.setLegacyAdvertising(true)` — this drops to BLE 4.x range but makes the packet
visible to nRF Connect etc.

---

## How it works

1. On boot (and on every radio re-enable) each node starts an **extended BLE 5
   advertisement** on Coded PHY (S=8, 125 kbps). The payload is 3 bytes of
   manufacturer data: `[0xFF, 0xFF, GROUP_ID]`.

2. Simultaneously each node runs a **passive extended scan** on Coded PHY (continuous),
   listening for advertisements matching `GROUP_ID`. The duplicate filter is disabled so
   every packet refreshes the peer's last-seen time (otherwise an unchanging beacon is
   reported only once and would falsely "time out").

3. When a matching packet arrives the scan callback records `millis()` and the raw RSSI.

4. `loop()` applies an exponential moving average to the RSSI and maps it to the LED
   behaviour described in *LED behaviour* above. It also polls the BOOT button for the
   long-press radio toggle.

Coded PHY improves receiver sensitivity by ~5 dB over standard BLE 1M, roughly 1.8× more
range at the same TX power — though indoors that advantage is often masked by interference.
