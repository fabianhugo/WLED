# EspNowRemote usermod

Combines WLED LED control with a WiZmote-compatible ESP-NOW remote in a single firmware.  
The device drives its own LED strip **and** sends remote-control commands to other WLED nodes over ESP-NOW — no separate remote hardware required.

---

## Features

| Feature | Detail |
|---|---|
| WiZmote button commands | Sends WiZmote-format ESP-NOW packets to a target WLED device on GPIO button press |
| Heartbeat sync | Broadcasts the current WLED state every 20 s so other nodes stay in sync without a WiFi link |
| AP disabled by default | Suppresses the automatic access-point fallback; AP is only opened when the WLED button is held for 6 s |

---

## How it works

WLED already initialises a global `quickEspNow` instance (via the [QuickEspNow](https://github.com/blazoncek/QuickESPNow) library) for its own sync/remote-receive features.  
This usermod reuses that instance to **send** two kinds of ESP-NOW frames:

### WiZmote button packet
Built from `WizMoteMsg_t` — identical to the structure in `wled00/remote.cpp`.  
A 14-byte frame with a program byte (`0x91` for ON, `0x81` for all others), a 32-bit incrementing sequence number, the button code, and a nominal battery level.  
Sent to the configured `targetMac` unicast address (default: broadcast `FF:FF:FF:FF:FF:FF`).

### Heartbeat sync packet
A 44-byte `PartialEspNowPkt_t` frame (magic `'W'`, packet 0, noOfPackets 1) wrapping the 41-byte global-state payload produced by WLED's `notify()` function (compatibility version 12).  
Contains brightness, primary/secondary/tertiary colours, effect, speed, intensity, palette, CCT, nightlight state, timebase and sync groups.  
Sent to `ESPNOW_BROADCAST_ADDRESS` every 20 s.  
Any WLED receiver in a matching sync group will apply the state as if it had received a UDP notification.

### AP behaviour
On every boot, if `apBehavior` is still at one of the two automatic-AP defaults (`AP_BEHAVIOR_BOOT_NO_CONN` or `AP_BEHAVIOR_NO_CONN`), it is overridden to `AP_BEHAVIOR_BUTTON_ONLY`.  
A value the user has explicitly saved via the WLED web UI is left untouched.

---

## Installation

Add to `platformio_override.ini`:

```ini
[env:your_board]
custom_usermods = espnow_remote
```

`WLED_DISABLE_ESPNOW` must **not** be defined (it is not defined by default).

---

## Configuration

Open **Config → Usermod Settings** in the WLED web UI, or edit `cfg.json` directly under the `"um"` → `"EspNowRemote"` key.

| Key | Type | Default | Description |
|---|---|---|---|
| `enabled` | bool | `true` | Enable/disable the usermod at runtime |
| `targetMac` | string (12 hex chars) | `"FFFFFFFFFFFF"` | MAC of the target WLED device for button commands; broadcast sends to all |
| `pin` | int8 array [4] | `[-1,-1,-1,-1]` | GPIO numbers for up to 4 buttons; `-1` = unused |
| `btnCode` | uint8 array [4] | `[1,2,9,8]` | WiZmote button code sent when the corresponding pin is pressed |

### WiZmote button codes

| Code | Action |
|---|---|
| 1 | ON |
| 2 | OFF |
| 3 | Night mode |
| 8 | Brightness down |
| 9 | Brightness up |
| 16 | Preset 1 |
| 17 | Preset 2 |
| 18 | Preset 3 |
| 19 | Preset 4 |

### Example cfg.json snippet

```json
"EspNowRemote": {
  "enabled": true,
  "targetMac": "AABBCCDDEEFF",
  "pin": [4, 5, -1, -1],
  "btnCode": [1, 2, 9, 8]
}
```

---

## Target device setup

The receiving WLED device must have ESP-NOW enabled and the sender's MAC registered:

1. **Config → WiFi Setup → Wireless Remote**
2. Enable **ESP-NOW Enable Remote**
3. Add the sender's MAC address to **Hardware MAC**

For heartbeat sync the receiver must also have **Sync → Receive** enabled and share a sync group with the sender.

---

## Channel constraint

ESP-NOW requires both devices to be on the same WiFi channel.  
WLED enforces this automatically for its own ESP-NOW features (the AP channel or the connected STA channel is reused).

---

## Limitations vs. standalone remote

| Standalone `WLEDRemoteControl.ino` | This usermod |
|---|---|
| Deep-sleep between presses (battery-friendly) | Always-on (WLED must run continuously) |
| Dedicated remote hardware | One device: LED controller + remote |
| Raw `esp_now.h` | Shares WLED's `QuickEspNow` instance |

---

## Files

| Path | Purpose |
|---|---|
| `usermods/espnow_remote/usermod_espnow_remote.cpp` | Usermod implementation |
| `usermods/espnow_remote/library.json` | PlatformIO library manifest |
| `wled00/const.h` | `USERMOD_ID_ESPNOW_REMOTE = 59` added |
