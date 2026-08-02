# Factory provisioning of WLED (pins, WiFi, presets)

Working notes for baking a fixed set of settings into devices at flash time / first boot.
Base: WLED v16.0.1 (this checkout).

## 2026-08-02 — Findings

### Where settings actually live

| Setting group | File on LittleFS | Notes |
|---|---|---|
| Pins (LED buses, buttons, relay, IR) | `/cfg.json` → `hw.led.ins[]`, `hw.btn.ins[]` | |
| WiFi SSID / AP config | `/cfg.json` → `wifi`, `ap` | |
| Passwords (WiFi PSK, AP, MQTT, OTA) | `/wsec.json` | not readable/listable over HTTP, see below |
| Presets | `/presets.json` | |

Important: `deserializeConfig()` **does** honor a `psk` key inside `cfg.json` if present —
[cfg.cpp:99](../wled00/cfg.cpp#L99) *"password is not normally present but if it is, use it"*.
Same for the AP password at [cfg.cpp:144](../wled00/cfg.cpp#L144). So a hand-written provisioning
`cfg.json` can carry credentials and `wsec.json` is not needed. WLED strips them again on the next
`serializeConfig()`, moving them into `wsec.json`.

### Boot order (wled.cpp `WLED::setup()`)

```
457  WLED_FS.begin()               FS mounted
467  initPresetsFile()             creates empty presets.json ONLY if missing
478  verifyConfig/restoreConfig/resetConfig
484  deserializeConfigFromFS()     reads cfg.json, buses/pins created here
604  strip.finalizeInit()
501  UsermodManager::setup()       <-- usermods run AFTER config + strip init
```

Consequence: a usermod cannot inject pin config for the *current* boot; it must write the file
and set `doReboot = true`.

### HTTP endpoints relevant to provisioning

- Uploading `cfg.json` via `/edit` triggers an automatic reboot —
  [wled_server.cpp:216-218](../wled00/wled_server.cpp#L216-L218).
- Uploading `presets.json` refreshes `presetsModifiedTime` — [wled_server.cpp:209](../wled00/wled_server.cpp#L209).
- `wsec.json` is hidden from listing and 403s on read — [wled_server.cpp:262,294](../wled00/wled_server.cpp#L294).
- `/edit` is PIN-protected when a PIN is set.

### Compile-time route (`my_config.h`)

- Enabled automatically by PlatformIO: [pio-scripts/user_config_copy.py](../pio-scripts/user_config_copy.py)
  copies `my_config_sample.h` → `my_config.h` if absent; included from [wled.h:124](../wled00/wled.h#L124).
- Covers `CLIENT_SSID`/`CLIENT_PASS`, `BTNPIN`/`BTNTYPE`, `DATA_PINS`, `MDNS_NAME`, defaults etc.
- **Cannot** carry presets.
- Caveat documented in the sample: hardcoded WiFi survives a factory reset.

### Filesystem-image route

- `platformio.ini:38` sets `data_dir = ./wled00/data`, which is the **web UI source**. Running
  `pio run -t uploadfs`/`buildfs` unmodified would flash the UI sources into LittleFS. `data_dir`
  must be overridden for a provisioning image.
- FS partition offset comes from the board's partition CSV, e.g.
  `tools/WLED_ESP32_4MB_1MB_FS.csv` → spiffs @ `0x310000`, size `0xF0000`.

## 2026-08-02 — Decision

**Recommended: ship a prepared LittleFS image containing `cfg.json` + `presets.json`, flashed
alongside the firmware.**

Why:
- Zero source changes; works with stock/official firmware binaries.
- The files are byte-identical to what WLED writes itself, so no format drift.
- Covers all three requested groups (pins, WiFi, presets) — the only route that does.

Rejected alternatives:
- *`my_config.h` only* — genuinely one file, but structurally cannot carry presets.
- *Provisioning usermod with embedded JSON* — is one file, but needs a double boot (usermods run
  after config load), costs flash for the embedded JSON, and duplicates config-format knowledge in
  C++. Kept as fallback if a single-binary flash is a hard requirement.
- *HTTP provisioning script after first boot* — good for retrofitting existing devices, but is not
  "applied during flashing" and needs the device on a network.

## 2026-08-02 — Concrete setup for esp32c3dev_qio

Provisioning files live in `fsdata/` (`cfg.json`, `presets.json`).

`platformio_override.ini` (gitignored) does the whole job — no build script needed:

```ini
[platformio]
data_dir = fsdata          ; instead of ./wled00/data (the web UI sources!)

[env:esp32c3dev_qio]
extends = env:esp32c3dev
build_flags = ${common.build_flags} ${esp32c3.build_flags} -D WLED_RELEASE_NAME=\"ESP32-C3-QIO\"
board_build.flash_mode = qio
board_build.filesystem = littlefs   ; espressif32 platform defaults to spiffs
```

`board_build.filesystem = littlefs` is required: the partition is *named* `spiffs`
(`tools/WLED_ESP32_4MB_1MB_FS.csv`) but WLED mounts it with LittleFS ([wled.h:245-252](../wled00/wled.h#L245-L252)).

Verified: `pio run -e esp32c3dev_qio -t buildfs` → `.pio/build/esp32c3dev_qio/littlefs.bin`,
983040 bytes = 0xF0000, matching the FS partition at offset 0x310000 (read back from the built
`partitions.bin`). `-t uploadfs` derives the offset itself.

`build_ui.py` does **not** read `data_dir` (it only shells out to `npm run build`), so overriding
`data_dir` globally is safe for firmware builds.

Local toolchain: `pio` and `esptool` at `/home/lop/priv/lichterketten/plattformioVenv/bin/`;
`mklittlefs` v3.2.0 (arduino-esp32 config) at `~/.platformio/packages/tool-mklittlefs/`.

Open item: the prepared `cfg.json` still has the placeholder network
(`nw.ins[0].ssid = "Your_Network"`, no `psk`) — devices flashed with it come up in AP mode.
