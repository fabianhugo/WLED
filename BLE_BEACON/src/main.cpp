#include <Arduino.h>
#include <NimBLEDevice.h>
#include <FastLED.h>

// ── Configuration — adjust per deployment ────────────────────────────────────

// All beacons that should see each other must share the same GROUP_ID.
// Change this if you have multiple independent deployments in the same space.
static constexpr uint8_t GROUP_ID = 0xBE;

// RSSI threshold above which a peer is considered "close" (full magenta).
// NOTE: this only controls colour, NOT detection range — any decodable packet
// marks the peer present. Set low so peers anywhere in usable range read as near;
// Coded PHY sensitivity bottoms out around -103 dBm.
//   -65 dBm ≈ 1 m  |  -75 dBm ≈ 3 m  |  -85 dBm ≈ 8 m  |  -95 dBm ≈ 25 m
static constexpr int8_t RSSI_NEAR = -95;

// How long without a packet before a peer is considered gone (ms).
// At a 1 s advertising interval this must comfortably exceed the interval so a
// few missed packets at long range don't drop the peer. 8 s = ~8 chances.
static constexpr uint32_t TIMEOUT_MS = 8000;

// BLE advertising interval in units of 0.625 ms.
// 1600 = 1000 ms between advertisement bursts. Longer interval = lower average
// current and less RF congestion, at the cost of slower detection.
static constexpr uint16_t ADV_INTERVAL = 1600;

// TX power in dBm. ESP32-S3 maximum is ~+20 dBm; NimBLE rounds up to the top
// power level internally. Max range. NOTE: +20 dBm may exceed regulatory limits
// in some regions — reduce if that matters for your deployment.
static constexpr int TX_POWER_DBM = 20;

// LED strip wiring. Override per board from platformio.ini build_flags
// (e.g. -DLED_PIN=10 -DNUM_LEDS=1); the defaults below target the lolin_s3_mini.
#ifndef LED_PIN
#define LED_PIN 48          // WS2812B data GPIO (48 = onboard LED on lolin_s3_mini)
#endif
#ifndef NUM_LEDS
#define NUM_LEDS 10         // strip length
#endif
static constexpr uint8_t BRIGHTNESS = 60; // global FastLED brightness (0–255)

// BOOT button (active-low) toggles the radio on a long press. GPIO0 on most
// ESP32-S3 boards; GPIO9 on most ESP32-C3 boards incl. the Seeed Xiao C3.
#ifndef BOOT_BUTTON_PIN
#define BOOT_BUTTON_PIN 0
#endif
static constexpr uint32_t LONG_PRESS_MS = 1000; // hold this long to toggle

// ─────────────────────────────────────────────────────────────────────────────

CRGB leds[NUM_LEDS];

// Radio on/off, toggled by a long press on BOOT. Read by the scan callback
// (BLE task) and the loop, so volatile. When false, advertising and scanning
// are stopped and onScanEnd must NOT auto-restart the scan.
static volatile bool radioEnabled = true;

// Updated inside the BLE scan callback (runs on a BLE task, not the Arduino loop).
// uint32_t wraps safely; int8_t is atomic on Xtensa/RISC-V for single-byte reads.
static volatile bool     peerSeen   = false;
static volatile uint32_t lastSeenMs = 0;
static volatile int8_t   lastRSSI   = -127;

// Exponential moving average applied in loop() to smooth noisy RSSI readings.
// ALPHA=1.0 means raw (no smoothing); 0.1 means very slow to react.
static constexpr float ALPHA = 0.2f;
static float smoothRSSI = -127.0f;

// ── BLE scan callback ─────────────────────────────────────────────────────────

class BeaconCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    if (!dev->haveManufacturerData()) return;

    // Manufacturer data layout: [company_lo, company_hi, group_id]
    // We use 0xFFFF (internal/test company ID) so no company registration needed.
    auto mfr = dev->getManufacturerData();
    if (mfr.size() < 3 || static_cast<uint8_t>(mfr[2]) != GROUP_ID) return;

    peerSeen   = true;
    lastSeenMs = millis();
    lastRSSI   = dev->getRSSI();
  }

  // NimBLE can stop the scan after radio arbitration or internal timeouts.
  // Restart immediately so detection never permanently stalls — but not when
  // the user has switched the radio off, or we'd fight the disable.
  void onScanEnd(const NimBLEScanResults&, int) override {
    if (radioEnabled) NimBLEDevice::getScan()->start(0);
  }
};

// ── Radio control & visual feedback ────────────────────────────────────────────

// Single, reusable scan-callback instance. NimBLEScan does not delete its
// callbacks, and deinit(true) deletes the scan object — so a `new` here would
// leak on every re-enable. A file-scope instance survives init/deinit cycles.
static BeaconCallbacks scanCallbacks;

// Fully bring up the BLE radio: init the controller, configure Coded-PHY
// extended advertising and continuous scanning, and start both. Safe to call
// after deinit() — getScan()/getAdvertising() recreate their objects.
static void startBle() {
  NimBLEDevice::init("beacon");
  NimBLEDevice::setPower(TX_POWER_DBM);

  // Read back what the controller actually applied. If this prints less than
  // TX_POWER_DBM, the chip clamped it to its hardware ceiling (ESP32-S3 BLE
  // tops out at +18..+20 dBm depending on module/efuse).
  Serial.printf("TX power applied: requested=%d  adv=%d  scan=%d dBm\n",
                TX_POWER_DBM,
                NimBLEDevice::getPower(NimBLETxPowerType::Advertise),
                NimBLEDevice::getPower(NimBLETxPowerType::Scan));

  // ── Extended advertising on Coded PHY (BLE 5 Long Range, ~125 kbps) ────────
  NimBLEExtAdvertisement adData(BLE_HCI_LE_PHY_CODED, BLE_HCI_LE_PHY_CODED);
  adData.setTxPower(TX_POWER_DBM); // explicit; don't rely on ctor readback timing
  adData.setLegacyAdvertising(false);
  adData.setScannable(false);
  adData.setConnectable(false);
  adData.setMinInterval(ADV_INTERVAL);
  adData.setMaxInterval(ADV_INTERVAL);

  uint8_t mfr[] = {0xFF, 0xFF, GROUP_ID};
  adData.setManufacturerData(mfr, sizeof(mfr));

  NimBLEExtAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setInstanceData(0, adData);
  adv->start(0); // instance ID 0, advertise indefinitely

  // ── Extended passive scan on Coded PHY, continuous ─────────────────────────
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCallbacks, false); // reuse static instance, don't delete
  scan->setActiveScan(false);
  scan->setPhy(NimBLEScan::SCAN_CODED);
  scan->setInterval(160); // 100 ms
  scan->setWindow(160);   // 100 ms RX → continuous
  // Disable the duplicate filter (must be set AFTER setScanCallbacks, which
  // re-enables it): otherwise a peer is reported once and lastSeenMs never
  // refreshes, so it "times out" while still present. 0 = report every packet.
  scan->setDuplicateFilter(0);
  scan->start(0);         // 0 = run indefinitely

  Serial.printf("BLE beacon started  group=0x%02X  rssi_near=%d dBm\n",
                GROUP_ID, RSSI_NEAR);
}

// Turn the radio fully on or off. ON re-initialises the whole stack. OFF stops
// advertising (per-instance — the no-arg stop() uses ext_adv_clear which fails
// while an instance is active, leaving it advertising) and scanning, then
// deinit(true) tears down the NimBLE port and powers down the BLE controller
// for maximum power saving.
static void setRadio(bool on) {
  if (on) {
    startBle();
  } else {
    NimBLEDevice::getScan()->stop();
    NimBLEDevice::getAdvertising()->stop(0); // stop instance 0, NOT clear-all
    peerSeen = false;                        // let LED state lapse to idle
    NimBLEDevice::deinit(true);              // free controller + scan/adv objects
  }
}

// Blink the whole strip a colour `times` times as user feedback. Blocks briefly
// (~times × 200 ms); fine for a button acknowledgement. The next loop() redraw
// restores the normal pattern.
static void flashFeedback(const CRGB& color, int times) {
  for (int i = 0; i < times; i++) {
    fill_solid(leds, NUM_LEDS, color);
    FastLED.show();
    delay(120);
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    FastLED.show();
    delay(120);
  }
}

// ── Setup ─────────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  // With ARDUINO_USB_CDC_ON_BOOT, Serial is native USB CDC: it re-enumerates on
  // every reset, so prints in setup() are lost unless we wait for the host to
  // open the port. Wait up to 3 s (so a headless/battery boot still proceeds).
  for (uint32_t t0 = millis(); !Serial && millis() - t0 < 3000;) {
    delay(10);
  }
  delay(100); // let the CDC channel settle before the first prints

  // Drop the CPU from the 240 MHz default to 80 MHz. Active current scales
  // roughly with clock, so this is the single biggest heat/power lever that
  // works on the stock Arduino framework (true light sleep would require a
  // custom IDF sdkconfig with CONFIG_PM_ENABLE). 80 MHz is the minimum the
  // BLE controller supports, and is far more than this workload needs.
  setCpuFrequencyMhz(80);

  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP); // BOOT is active-low

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(BRIGHTNESS);
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();

  startBle();
}

// ── Main loop ─────────────────────────────────────────────────────────────────

void loop() {
  const uint32_t now    = millis();

  // ── BOOT long-press → toggle radio ──────────────────────────────────────────
  // Fire once when the button has been held continuously for LONG_PRESS_MS;
  // re-arm only after release. Active-low (pressed == LOW).
  static bool     btnDown    = false;
  static uint32_t btnDownAt  = 0;
  static bool     longFired  = false;
  const bool      pressed    = (digitalRead(BOOT_BUTTON_PIN) == LOW);

  if (pressed && !btnDown) {           // press began
    btnDown   = true;
    btnDownAt = now;
    longFired = false;
  } else if (pressed && btnDown && !longFired && now - btnDownAt >= LONG_PRESS_MS) {
    longFired    = true;               // long press recognised — toggle once
    radioEnabled = !radioEnabled;
    setRadio(radioEnabled);
    flashFeedback(radioEnabled ? CRGB::Green : CRGB::Red, 3);
    Serial.printf("Radio %s by long press\n", radioEnabled ? "ENABLED" : "DISABLED");
  } else if (!pressed) {               // released — re-arm
    btnDown = false;
  }

  const bool     nearby = peerSeen && (now - lastSeenMs) < TIMEOUT_MS;
  const int8_t   rssi   = lastRSSI;

  // Update smoothed RSSI; reset when peer disappears so there is no stale value
  // on next appearance.
  if (nearby) {
    smoothRSSI = ALPHA * rssi + (1.0f - ALPHA) * smoothRSSI;
  } else {
    smoothRSSI = -127.0f;
  }

  // Advances every frame so the idle rainbow keeps scrolling. uint8_t wraps
  // at 255 → seamless hue loop.
  static uint8_t idleHue = 0;
  idleHue += 1;

  if (!nearby) {
    // Idle: slowly cycling rainbow spread across the strip (WLED-style).
    // 255 / NUM_LEDS spaces ~one full spectrum over all pixels (255 keeps the
    // delta within uint8_t even at NUM_LEDS=1); idleHue rotates it each frame.
    fill_rainbow(leds, NUM_LEDS, idleHue, 255 / NUM_LEDS);
  } else if (smoothRSSI >= RSSI_NEAR) {
    // Close: pulsing magenta. beatsin8 is a sine wave driven by millis():
    // 40 bpm → a full breathe every ~1.5 s, ramping brightness 40↔255.
    // Magenta = equal red + blue, so scaling both channels pulses brightness.
    uint8_t pulse = beatsin8(40, 40, 255);
    fill_solid(leds, NUM_LEDS, CRGB(pulse, 0, pulse));
  } else {
    // Approaching: interpolate blue → magenta as RSSI rises toward threshold
    float t = constrain((smoothRSSI + 127.0f) / (RSSI_NEAR + 127.0f), 0.0f, 1.0f);
    fill_solid(leds, NUM_LEDS, CRGB(static_cast<uint8_t>(255 * t), 0, 255));
  }

  FastLED.show();

  Serial.printf("nearby=%d  rssi=%d  smooth=%.1f\n", nearby, rssi, smoothRSSI);

  delay(100);
}
