#include "wled.h"

#ifndef WLED_DISABLE_ESPNOW

/*
 * EspNowRemote usermod
 *
 * Makes WLED simultaneously act as a WiZmote-compatible ESP-NOW remote:
 *  - Sends WiZmote button commands to a configurable target MAC on GPIO button press
 *  - Broadcasts a WLED sync heartbeat every 20 s via QuickEspNow so other nodes
 *    stay synchronised without a WiFi connection
 *  - Forces "AP button-only" behaviour on boot so the access point is not opened
 *    automatically (it can still be triggered by holding the WLED button for 6 s)
 *  - Detects heartbeats from other boards running the same firmware and switches
 *    between an idle effect and an active effect accordingly
 *
 * Configuration (Usermod Settings page or cfg.json "um" → "EspNowRemote"):
 *   targetMac       – 12-char uppercase hex MAC of the target WLED (default: broadcast)
 *   pin             – array of up to 4 GPIO numbers for buttons (-1 = unused)
 *   btnCode         – WiZmote button code sent for each pin (see constants below)
 *   idlePreset      – preset ID applied when no heartbeat is present (0 = direct effect)
 *   activePreset    – preset ID applied when a heartbeat is detected (0 = direct effect)
 *   idleFx          – effect ID for idle state when idlePreset=0  (default: 65 = Palette)
 *   activeFx        – effect ID for active state when activePreset=0 (default: 38 = Aurora)
 *   timeoutSec      – seconds after last heartbeat before reverting to idle (default: 45)
 *   transitionMs    – crossfade duration in ms when switching states (default: 1500)
 *
 * Button codes (WiZmote protocol):
 *   1  ON      2  OFF     3  NIGHT
 *   8  BRIGHT_DOWN        9  BRIGHT_UP
 *   16 PRESET_1  17 PRESET_2  18 PRESET_3  19 PRESET_4
 *
 * To include this usermod add it to platformio_override.ini:
 *   custom_usermods = espnow_remote
 */

// ── WiZmote button constants (mirrored from remote.cpp, local scope) ──────────
#ifndef WIZMOTE_BUTTON_ON
  #define WIZMOTE_BUTTON_ON           1
  #define WIZMOTE_BUTTON_OFF          2
  #define WIZMOTE_BUTTON_NIGHT        3
  #define WIZMOTE_BUTTON_BRIGHT_DOWN  8
  #define WIZMOTE_BUTTON_BRIGHT_UP    9
  #define WIZMOTE_BUTTON_ONE         16
  #define WIZMOTE_BUTTON_TWO         17
  #define WIZMOTE_BUTTON_THREE       18
  #define WIZMOTE_BUTTON_FOUR        19
#endif

#define ESPNOW_REMOTE_HEARTBEAT_MS  1000UL   // heartbeat interval
#define ESPNOW_REMOTE_MAX_BUTTONS   4        // configurable GPIO buttons
#define ESPNOW_REMOTE_DEBOUNCE_MS   50       // debounce window in ms

class EspNowRemoteUsermod : public Usermod {
 private:
  bool enabled = true;

  // ── Target MAC for outgoing WiZmote commands (default = broadcast) ──────────
  char    targetMacStr[13] = "FFFFFFFFFFFF";
  uint8_t targetMac[6]     = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

  // ── Per-button config ────────────────────────────────────────────────────────
  int8_t  buttonPins[ESPNOW_REMOTE_MAX_BUTTONS]  = {-1, -1, -1, -1};
  uint8_t buttonCodes[ESPNOW_REMOTE_MAX_BUTTONS] = {
    WIZMOTE_BUTTON_ON,
    WIZMOTE_BUTTON_OFF,
    WIZMOTE_BUTTON_BRIGHT_UP,
    WIZMOTE_BUTTON_BRIGHT_DOWN
  };

  // ── Debounce state ───────────────────────────────────────────────────────────
  bool          rawState[ESPNOW_REMOTE_MAX_BUTTONS]    = {};
  bool          stableState[ESPNOW_REMOTE_MAX_BUTTONS] = {};
  unsigned long lastRawChange[ESPNOW_REMOTE_MAX_BUTTONS] = {};

  // ── Heartbeat presence detection ─────────────────────────────────────────────
  // Preset / effect shown when no peer heartbeat is active
  uint8_t idlePreset    = 0;   // 0 = use idleFx directly
  uint8_t idleFx        = FX_MODE_PALETTE;
  uint8_t idleSpeed     = 20;  // slow drift
  uint8_t idlePalette   = 0;   // default palette

  // Preset / effect shown while a peer heartbeat is being received
  uint8_t activePreset  = 0;   // 0 = use activeFx directly
  uint8_t activeFx      = FX_MODE_AURORA;
  uint8_t activeSpeed   = 24;  // Aurora default (sx=24 from FX.cpp)
  uint8_t activePalette = 50;  // Aurora default (pal=50 from FX.cpp)

  uint16_t timeoutSec   = 21;  // revert to idle after this many seconds without a heartbeat
  uint16_t transitionMs = 1500;

  // Runtime state
  enum PresenceState : uint8_t { IDLE, ACTIVE } presenceState = IDLE;
  unsigned long lastPeerHeartbeat = 0;  // millis() of most recent received heartbeat
  bool presenceInitDone = false;        // set after first effect is applied in loop()
  uint8_t lastPeerMac[6] = {};          // MAC of the most recent heartbeat sender

  // ── Misc ─────────────────────────────────────────────────────────────────────
  unsigned long lastHeartbeat = 0;
  uint32_t      seq           = 1;  // outgoing WiZmote sequence counter

  static const char _name[];
  static const char _enabled[];

  // ── Helpers ──────────────────────────────────────────────────────────────────

  void parseMacStr(const char* s, uint8_t* mac) {
    for (int i = 0; i < 6; i++) {
      char buf[3] = {s[i * 2], s[i * 2 + 1], '\0'};
      mac[i] = (uint8_t)strtoul(buf, nullptr, 16);
    }
  }

  // WiZmote on-air packet format (matches remote.cpp WizMoteMessageStructure)
  typedef struct __attribute__((packed)) {
    uint8_t program;            // 0x91 for ON, 0x81 for all others
    uint8_t seq[4];             // 32-bit counter, LSB first
    uint8_t dt1;                // data type 1 = 0x20
    uint8_t button;             // button code
    uint8_t dt2;                // data type 2 = 0x01
    uint8_t batLevel;           // battery level 0-100
    uint8_t byte10, byte11, byte12, byte13;
  } WizMoteMsg_t;

  // QuickEspNow framing header (matches udp.cpp PartialEspNowPacket)
  typedef struct __attribute__((packed)) {
    uint8_t magic;              // 'W'
    uint8_t packet;             // 0-based packet index
    uint8_t noOfPackets;        // total number of packets
    uint8_t data[247];          // payload
  } PartialEspNowPkt_t;

  void sendWizMoteCommand(uint8_t button) {
    if (statusESPNow != ESP_NOW_STATE_ON) return;

    WizMoteMsg_t msg;
    msg.program  = (button == WIZMOTE_BUTTON_ON) ? 0x91 : 0x81;
    msg.seq[0]   = (uint8_t)(seq);
    msg.seq[1]   = (uint8_t)(seq >> 8);
    msg.seq[2]   = (uint8_t)(seq >> 16);
    msg.seq[3]   = (uint8_t)(seq >> 24);
    msg.dt1      = 0x20;
    msg.button   = button;
    msg.dt2      = 0x01;
    msg.batLevel = 100;
    msg.byte10 = msg.byte11 = msg.byte12 = msg.byte13 = 0;
    seq++;

    quickEspNow.send(targetMac, (const uint8_t*)&msg, sizeof(msg));
    DEBUG_PRINTF_P(PSTR("EspNowRemote: button %u -> %02X%02X%02X%02X%02X%02X\n"),
      (unsigned)button,
      targetMac[0], targetMac[1], targetMac[2],
      targetMac[3], targetMac[4], targetMac[5]);
  }

  // Broadcast current WLED state as a single-packet QuickEspNow sync frame.
  // Mirrors the global-state portion of notify() in udp.cpp (compat-byte 12).
  // Per-segment data is omitted; receivers with matching sync-groups will update
  // their global brightness, effect and colour from this packet.
  void sendHeartbeat() {
    if (statusESPNow != ESP_NOW_STATE_ON) return;

    PartialEspNowPkt_t buf;
    buf.magic       = 'W';
    buf.packet      = 0;
    buf.noOfPackets = 1;

    Segment&  ms   = strip.getMainSegment();
    uint32_t  col  = ms.colors[0];
    uint32_t  col2 = ms.colors[1];
    uint32_t  col3 = ms.colors[2];

    buf.data[0]  = 0;                        // wled notifier protocol ID
    buf.data[1]  = CALL_MODE_DIRECT_CHANGE;
    buf.data[2]  = bri;
    buf.data[3]  = R(col);
    buf.data[4]  = G(col);
    buf.data[5]  = B(col);
    buf.data[6]  = (uint8_t)nightlightActive;
    buf.data[7]  = nightlightDelayMins;
    buf.data[8]  = ms.mode;
    buf.data[9]  = ms.speed;
    buf.data[10] = W(col);
    buf.data[11] = 12;                       // compatibility version byte
    buf.data[12] = R(col2);
    buf.data[13] = G(col2);
    buf.data[14] = B(col2);
    buf.data[15] = W(col2);
    buf.data[16] = ms.intensity;
    buf.data[17] = (uint8_t)(transitionDelay & 0xFF);
    buf.data[18] = (uint8_t)(transitionDelay >> 8);
    buf.data[19] = ms.palette;
    buf.data[20] = R(col3);
    buf.data[21] = G(col3);
    buf.data[22] = B(col3);
    buf.data[23] = W(col3);
    buf.data[24] = 0;                        // followUp = false

    uint32_t t = millis() + strip.timebase;
    buf.data[25] = (uint8_t)(t >> 24);
    buf.data[26] = (uint8_t)(t >> 16);
    buf.data[27] = (uint8_t)(t >>  8);
    buf.data[28] = (uint8_t)(t >>  0);

    buf.data[29] = toki.getTimeSource();
    Toki::Time tm = toki.getTime();
    uint32_t unix = tm.sec;
    buf.data[30] = (uint8_t)(unix >> 24);
    buf.data[31] = (uint8_t)(unix >> 16);
    buf.data[32] = (uint8_t)(unix >>  8);
    buf.data[33] = (uint8_t)(unix >>  0);
    uint16_t ms_frac = tm.ms;
    buf.data[34] = (uint8_t)(ms_frac >> 8);
    buf.data[35] = (uint8_t)(ms_frac >> 0);

    buf.data[36] = syncGroups;
    buf.data[37] = strip.hasCCTBus() ? 0 : 255;
    buf.data[38] = ms.cct;
    buf.data[39] = 0;                        // 0 active segments in heartbeat
    buf.data[40] = 36;                       // UDP_SEG_SIZE

    // header is 3 bytes (magic + packet + noOfPackets), payload is 41 bytes
    quickEspNow.send(ESPNOW_BROADCAST_ADDRESS, (const uint8_t*)&buf, 3 + 41);
    DEBUG_PRINTLN(F("EspNowRemote: heartbeat sent"));
  }

  void configurePins() {
    for (int i = 0; i < ESPNOW_REMOTE_MAX_BUTTONS; i++) {
      if (buttonPins[i] >= 0) {
        pinMode(buttonPins[i], INPUT_PULLUP);
      }
    }
  }

  // Apply the idle or active visual state.
  // Uses a preset if configured (> 0), otherwise sets the effect directly.
  void applyPresenceEffect(bool active) {
    uint8_t preset  = active ? activePreset  : idlePreset;
    uint8_t fxId    = active ? activeFx      : idleFx;
    uint8_t speed   = active ? activeSpeed   : idleSpeed;
    uint8_t palette = active ? activePalette : idlePalette;

    strip.setTransition(transitionMs);

    if (preset > 0) {
      applyPreset(preset, CALL_MODE_DIRECT_CHANGE);
    } else {
      Segment& seg = strip.getMainSegment();
      seg.setMode(fxId, true);
      seg.speed   = speed;
      seg.palette = palette;
      stateUpdated(CALL_MODE_DIRECT_CHANGE);
    }

    DEBUG_PRINTF_P(PSTR("EspNowRemote: switched to %s effect\n"), active ? "ACTIVE" : "IDLE");
  }

 public:

  // ── WLED lifecycle ────────────────────────────────────────────────────────────

  void setup() override {
    // Change AP behaviour: don't open automatically; only on explicit button hold.
    // This only overrides the two "auto-open" values; if the user has explicitly
    // chosen another value (e.g. AP_BEHAVIOR_ALWAYS) it is left untouched.
    // if (apBehavior == AP_BEHAVIOR_BOOT_NO_CONN || apBehavior == AP_BEHAVIOR_NO_CONN) {
    //   apBehavior = AP_BEHAVIOR_BUTTON_ONLY;
    // }

    // Ensure the ESP-NOW stack will be started (harmless if already enabled via cfg).
    enableESPNow = true;

    configurePins();
  }

  void loop() override {
    if (!enabled) return;

    unsigned long now = millis();

    // ── Heartbeat tx ─────────────────────────────────────────────────────────
    if (now - lastHeartbeat >= ESPNOW_REMOTE_HEARTBEAT_MS) {
      lastHeartbeat = now;
      sendHeartbeat();
    }

    // ── Presence state machine ────────────────────────────────────────────────
    bool peerAlive = lastPeerHeartbeat > 0 &&
                     (now - lastPeerHeartbeat) < (uint32_t)timeoutSec * 1000UL;

    if (!presenceInitDone) {
      // Apply initial effect once the strip is ready
      presenceState   = peerAlive ? ACTIVE : IDLE;
      presenceInitDone = true;
      applyPresenceEffect(presenceState == ACTIVE);
    } else if (peerAlive && presenceState == IDLE) {
      presenceState = ACTIVE;
      applyPresenceEffect(true);
    } else if (!peerAlive && presenceState == ACTIVE) {
      presenceState = IDLE;
      applyPresenceEffect(false);
    }

    // ── Button scan with debounce ─────────────────────────────────────────────
    for (int i = 0; i < ESPNOW_REMOTE_MAX_BUTTONS; i++) {
      if (buttonPins[i] < 0) continue;

      bool raw = (digitalRead(buttonPins[i]) == LOW);   // active-low

      if (raw != rawState[i]) {
        rawState[i]      = raw;
        lastRawChange[i] = now;
      }

      if ((now - lastRawChange[i]) >= ESPNOW_REMOTE_DEBOUNCE_MS &&
          raw != stableState[i]) {
        stableState[i] = raw;
        if (raw) {
          // Button just pressed (stable falling edge on active-low input)
          sendWizMoteCommand(buttonCodes[i]);
        }
      }
    }
  }

  // ── ESP-NOW receive hook ──────────────────────────────────────────────────────

  // Called by WLED before it processes any incoming ESP-NOW packet.
  // Returning true consumes the packet (WLED will not apply it as a state sync).
  // We intercept our own heartbeat format (magic 'W', packet 0) so that the
  // receiving node stays in control of its own effect rather than being overwritten
  // by the sender's state.
  bool onEspNowMessage(uint8_t* sender, uint8_t* data, uint8_t len) override {
    // Our heartbeat: outer wrapper = { 'W', 0, 1, ... }, inner data[0]=0 (notifier proto)
    if (len >= 5 && data[0] == 'W' && data[1] == 0 && data[3] == 0) {
      lastPeerHeartbeat = millis();
      if (sender) memcpy(lastPeerMac, sender, 6);
      DEBUG_PRINTF_P(PSTR("EspNowRemote: heartbeat from %02X:%02X:%02X:%02X:%02X:%02X\n"),
        lastPeerMac[0], lastPeerMac[1], lastPeerMac[2],
        lastPeerMac[3], lastPeerMac[4], lastPeerMac[5]);
      return true;  // consume — do not let WLED apply the foreign state
    }
    return false;
  }

  // ── Config serialisation ──────────────────────────────────────────────────────

  void addToConfig(JsonObject& root) override {
    JsonObject top = root.createNestedObject(FPSTR(_name));
    top[FPSTR(_enabled)] = enabled;
    top["targetMac"]     = targetMacStr;
    JsonArray pins  = top.createNestedArray("pin");
    JsonArray codes = top.createNestedArray("btnCode");
    for (int i = 0; i < ESPNOW_REMOTE_MAX_BUTTONS; i++) {
      pins.add(buttonPins[i]);
      codes.add(buttonCodes[i]);
    }
    top["idlePreset"]   = idlePreset;
    top["activePreset"] = activePreset;
    top["idleFx"]       = idleFx;
    top["activeFx"]     = activeFx;
    top["idleSpeed"]    = idleSpeed;
    top["activeSpeed"]  = activeSpeed;
    top["idlePal"]      = idlePalette;
    top["activePal"]    = activePalette;
    top["timeoutSec"]   = timeoutSec;
    top["transMs"]      = transitionMs;
  }

  bool readFromConfig(JsonObject& root) override {
    JsonObject top          = root[FPSTR(_name)];
    bool       configComplete = !top.isNull();

    configComplete &= getJsonValue(top[FPSTR(_enabled)], enabled, true);

    String mac = targetMacStr;
    if (getJsonValue(top["targetMac"], mac) && mac.length() == 12) {
      mac.toUpperCase();
      strncpy(targetMacStr, mac.c_str(), 12);
      targetMacStr[12] = '\0';
      parseMacStr(targetMacStr, targetMac);
    }

    static const uint8_t defaultCodes[ESPNOW_REMOTE_MAX_BUTTONS] = {
      WIZMOTE_BUTTON_ON, WIZMOTE_BUTTON_OFF,
      WIZMOTE_BUTTON_BRIGHT_UP, WIZMOTE_BUTTON_BRIGHT_DOWN
    };
    for (int i = 0; i < ESPNOW_REMOTE_MAX_BUTTONS; i++) {
      configComplete &= getJsonValue(top["pin"][i],     buttonPins[i],  (int8_t)-1);
      configComplete &= getJsonValue(top["btnCode"][i], buttonCodes[i], defaultCodes[i]);
    }

    configComplete &= getJsonValue(top["idlePreset"],   idlePreset,    (uint8_t)0);
    configComplete &= getJsonValue(top["activePreset"], activePreset,  (uint8_t)0);
    configComplete &= getJsonValue(top["idleFx"],       idleFx,        (uint8_t)FX_MODE_PALETTE);
    configComplete &= getJsonValue(top["activeFx"],     activeFx,      (uint8_t)FX_MODE_AURORA);
    configComplete &= getJsonValue(top["idleSpeed"],    idleSpeed,     (uint8_t)20);
    configComplete &= getJsonValue(top["activeSpeed"],  activeSpeed,   (uint8_t)24);
    configComplete &= getJsonValue(top["idlePal"],      idlePalette,   (uint8_t)0);
    configComplete &= getJsonValue(top["activePal"],    activePalette, (uint8_t)50);
    configComplete &= getJsonValue(top["timeoutSec"],   timeoutSec,    (uint16_t)45);
    configComplete &= getJsonValue(top["transMs"],      transitionMs,  (uint16_t)1500);

    // (Re)configure pins in case this is called after setup() on a settings save
    configurePins();

    // Reset presence state so the new effect config takes effect immediately
    presenceInitDone = false;

    return configComplete;
  }

  // ── Info panel ────────────────────────────────────────────────────────────────

  void addToJsonInfo(JsonObject& root) override {
    JsonObject user = root["u"];
    if (user.isNull()) user = root.createNestedObject("u");

    // Own MAC
    char ownMac[18];
    uint8_t own[6];
    WiFi.macAddress(own);
    snprintf_P(ownMac, sizeof(ownMac), PSTR("%02X:%02X:%02X:%02X:%02X:%02X"),
      own[0], own[1], own[2], own[3], own[4], own[5]);
    JsonArray ownArr = user.createNestedArray(F("ESP-NOW own MAC"));
    ownArr.add(ownMac);

    // Last peer MAC (zero = none seen yet)
    char peerMac[18];
    snprintf_P(peerMac, sizeof(peerMac), PSTR("%02X:%02X:%02X:%02X:%02X:%02X"),
      lastPeerMac[0], lastPeerMac[1], lastPeerMac[2],
      lastPeerMac[3], lastPeerMac[4], lastPeerMac[5]);
    JsonArray peerArr = user.createNestedArray(F("ESP-NOW last peer"));
    peerArr.add(presenceState == ACTIVE ? peerMac : "-");
  }

  // ── Identity ──────────────────────────────────────────────────────────────────

  uint16_t getId() override { return USERMOD_ID_ESPNOW_REMOTE; }
};

const char EspNowRemoteUsermod::_name[]    PROGMEM = "EspNowRemote";
const char EspNowRemoteUsermod::_enabled[] PROGMEM = "enabled";

static EspNowRemoteUsermod espnow_remote_usermod;
REGISTER_USERMOD(espnow_remote_usermod);

#endif  // WLED_DISABLE_ESPNOW
