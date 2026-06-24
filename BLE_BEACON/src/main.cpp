#include <Arduino.h>
#include <NimBLEDevice.h>
#include <FastLED.h>

// ── Configuration — adjust per deployment ────────────────────────────────────

// All beacons that should see each other must share the same GROUP_ID.
// Change this if you have multiple independent deployments in the same space.
static constexpr uint8_t GROUP_ID = 0xBE;

// RSSI threshold above which a peer is considered "close".
// Raise (e.g. -70) to shrink the detection radius; lower (e.g. -90) to expand it.
// Rough rule of thumb with PCB antenna in open air:
//   -65 dBm ≈ 1 m  |  -75 dBm ≈ 3 m  |  -85 dBm ≈ 8 m  |  -95 dBm ≈ 25 m
static constexpr int8_t RSSI_NEAR = -80;

// How long without a packet before a peer is considered gone (ms).
static constexpr uint32_t TIMEOUT_MS = 8000;

// BLE advertising interval in units of 0.625 ms.
// 160 = 100 ms between advertisement bursts.
static constexpr uint16_t ADV_INTERVAL = 160;

// TX power in dBm. Legal max varies by region; chip max is 9–20 dBm.
// Lower values save power at the cost of range. 6 is a safe starting point.
static constexpr int TX_POWER_DBM = 9;

// LED strip wiring
static constexpr int  LED_PIN   = 8;
static constexpr int  NUM_LEDS  = 10;
static constexpr uint8_t BRIGHTNESS = 60; // global FastLED brightness (0–255)

// ─────────────────────────────────────────────────────────────────────────────

CRGB leds[NUM_LEDS];

// Updated inside the BLE scan callback (runs on a BLE task, not the Arduino loop).
// uint32_t wraps safely; int8_t is atomic on Xtensa/RISC-V for single-byte reads.
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

    lastSeenMs = millis();
    lastRSSI   = dev->getRSSI();
  }
};

// ── Setup ─────────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(BRIGHTNESS);
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();

  NimBLEDevice::init("beacon");
  NimBLEDevice::setPower(TX_POWER_DBM);

  // ── Extended advertising on Coded PHY (BLE 5 Long Range, ~125 kbps) ────────
  // Coded PHY improves receiver sensitivity by ~5 dB over standard BLE 1M,
  // roughly doubling usable range at the same TX power.
  NimBLEExtAdvertisement adData(BLE_HCI_LE_PHY_CODED, BLE_HCI_LE_PHY_CODED);
  adData.setLegacyAdvertising(false);
  adData.setScannable(false);
  adData.setConnectable(false);
  adData.setMinInterval(ADV_INTERVAL);
  adData.setMaxInterval(ADV_INTERVAL);

  uint8_t mfr[] = {0xFF, 0xFF, GROUP_ID};
  adData.setManufacturerData(mfr, sizeof(mfr));

  // getAdvertising() returns NimBLEExtAdvertising* when CONFIG_BT_NIMBLE_EXT_ADV is set
  NimBLEExtAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setInstanceData(0, adData);
  adv->start(0); // instance ID 0, advertise indefinitely

  // ── Extended scan on Coded PHY ────────────────────────────────────────────
  // Passive scan: we never send scan-request packets, so we are radio-silent
  // from a scanner's perspective (lower power, lower RF congestion).
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(new BeaconCallbacks());
  scan->setActiveScan(false);
  scan->setPhy(NimBLEScan::SCAN_CODED);
  scan->setInterval(160); // scan window repeats every 100 ms
  scan->setWindow(80);    // RX on for 50 ms of each 100 ms (50% duty cycle)
  scan->start(0);         // 0 = run indefinitely

  Serial.printf("BLE beacon started  group=0x%02X  rssi_near=%d dBm\n",
                GROUP_ID, RSSI_NEAR);
}

// ── Main loop ─────────────────────────────────────────────────────────────────

void loop() {
  const uint32_t now    = millis();
  const bool     nearby = (now - lastSeenMs) < TIMEOUT_MS;
  const int8_t   rssi   = lastRSSI;

  // Update smoothed RSSI; reset when peer disappears so there is no stale value
  // on next appearance.
  if (nearby) {
    smoothRSSI = ALPHA * rssi + (1.0f - ALPHA) * smoothRSSI;
  } else {
    smoothRSSI = -127.0f;
  }

  CRGB color;
  if (!nearby) {
    // Idle: very dim white
    color = CRGB(15, 15, 15);
  } else if (smoothRSSI >= RSSI_NEAR) {
    // Close: full magenta
    color = CRGB(255, 0, 255);
  } else {
    // Approaching: interpolate blue → magenta as RSSI rises toward threshold
    float t = constrain((smoothRSSI + 127.0f) / (RSSI_NEAR + 127.0f), 0.0f, 1.0f);
    color = CRGB(static_cast<uint8_t>(255 * t), 0, 255);
  }

  fill_solid(leds, NUM_LEDS, color);
  FastLED.show();

  Serial.printf("nearby=%d  rssi=%d  smooth=%.1f\n", nearby, rssi, smoothRSSI);

  delay(100);
}
