/*
  main.cpp

  Target environment
    PlatformIO / Arduino framework
    Board: d1_mini (LOLIN/WEMOS D1 R2 & mini, ESP8266)
    Library: SmartRC-CC1101-Driver-Lib 3.0.2

  CC1101 wiring
    SCK   -> D5 / GPIO14
    MISO  -> D6 / GPIO12
    MOSI  -> D7 / GPIO13
    CSN   -> D8 / GPIO15
    GDO0  -> D2 / GPIO4   (asynchronous TX data input)
    GDO2  -> D1 / GPIO5   (asynchronous RX data output)
    VCC   -> 3V3
    GND   -> GND

  Web UI / Wi-Fi
    Concurrent AP + station (WIFI_AP_STA).
    - Joins the configured home network, then browse to http://cc1101.local/
      or the router-assigned DHCP address.
    - Always hosts its own SoftAP so a phone can connect directly and reach
      http://192.168.4.1/ with no shared network.
    Credentials come from secrets.ini (git-ignored) via the CFG_WIFI_* build
    defines; copy secrets.ini.example to get started.

  Functions
    - RSSI frequency sweep with 318 MHz garage-remote defaults
    - Tune and fine-adjust around a target frequency
    - Raw OOK/ASK capture from GDO2 and raw playback through GDO0
    - Pulse-width histogram and generic timing/encoding analysis
    - Focused Linear 10-bit and MegaCode 24-bit recognizers
    - Save multiple raw samples to LittleFS
    - Load/decode/delete any saved sample
    - Timer1-driven raw playback of any saved sample
    - Two-button gate operator page (/gate) + assignment page (/gate/config):
      each button replays an assigned sample; gate fires force max TX power

  Endpoints
    /              analysis dashboard
    /gate          two-button operator page (Inner / Outer)
    /gate/config   assign a saved sample + radio settings to each gate button
    /api/*         JSON API (see README); /api/selftest for a one-call health check

  Host tooling
    tools/rfprobe.py   CLI over the JSON API
    tests/             pytest suite (skips unless CC1101_HOST points at a device)
*/

#include <Arduino.h>
#include <SPI.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <LittleFS.h>
#include <SmartRC_CC1101.h>
#include <math.h>

// -----------------------------------------------------------------------------
// Firmware identity (reported on /api/status and /api/selftest for verification)
// -----------------------------------------------------------------------------
#ifndef CFG_FW_VERSION
#define CFG_FW_VERSION "0.1.0-nogit"   // real value comes from scripts/version.py
#endif
static constexpr char FIRMWARE_VERSION[] = CFG_FW_VERSION;
static const char FIRMWARE_BUILD[] = __DATE__ " " __TIME__;

// Cached CC1101 identity registers, filled once in setup().
uint8_t radioPartnum = 0;
uint8_t radioVersionReg = 0;

// -----------------------------------------------------------------------------
// Hardware
// -----------------------------------------------------------------------------
static constexpr uint8_t PIN_CC1101_SCK  = D5;
static constexpr uint8_t PIN_CC1101_MISO = D6;
static constexpr uint8_t PIN_CC1101_MOSI = D7;
static constexpr uint8_t PIN_CC1101_CS   = D8;
static constexpr uint8_t PIN_CC1101_GDO0 = D2;  // module pin 3: async TX data input
static constexpr uint8_t PIN_CC1101_GDO2 = D1;  // module pin 8: async RX data output

// -----------------------------------------------------------------------------
// Wi-Fi settings (concurrent AP + station)
// -----------------------------------------------------------------------------
// The device always runs its own SoftAP so a phone can connect directly to the
// dashboard, and at the same time keeps trying to join the configured home
// network. The same web server answers on both interfaces.
//
// Credentials are NOT stored here. They come from secrets.ini (git-ignored) via
// the CFG_WIFI_* compile-time defines in platformio.ini. The placeholders below
// only apply when nothing overrode them, in which case the station join fails
// and only the SoftAP comes up. See secrets.ini.example.
#ifndef CFG_WIFI_SSID
#define CFG_WIFI_SSID "CHANGE_ME_SSID"
#endif
#ifndef CFG_WIFI_PASS
#define CFG_WIFI_PASS "CHANGE_ME_PASSWORD"
#endif
#ifndef CFG_WIFI_AP_SSID
#define CFG_WIFI_AP_SSID "cc1101-setup"
#endif
#ifndef CFG_WIFI_AP_PASS
#define CFG_WIFI_AP_PASS "CHANGE_ME_AP_PASSWORD"
#endif

// Station: the 2.4 GHz Wi-Fi network the D1 Mini should join.
static constexpr char WIFI_SSID[] = CFG_WIFI_SSID;
static constexpr char WIFI_PASS[] = CFG_WIFI_PASS;
static constexpr char WIFI_HOSTNAME[] = "cc1101";
static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 30000UL;
static constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 10000UL;

// SoftAP: WPA2 password must be at least 8 characters (or empty for an open AP,
// which is not recommended). Browse to http://192.168.4.1/ once joined.
static constexpr char WIFI_AP_SSID[] = CFG_WIFI_AP_SSID;
static constexpr char WIFI_AP_PASS[] = CFG_WIFI_AP_PASS;
// WPA2 needs >= 8 chars; empty is allowed (open AP). Catch a bad secrets.ini
// ap_pass at compile time instead of a silently-dead SoftAP. sizeof includes NUL.
static_assert(sizeof(WIFI_AP_PASS) == 1 || sizeof(WIFI_AP_PASS) >= 9,
              "CFG_WIFI_AP_PASS (secrets.ini [wifi] ap_pass) must be empty or at least 8 characters");
static constexpr uint8_t WIFI_AP_CHANNEL = 6;
static constexpr uint8_t WIFI_AP_MAX_CLIENTS = 4;
static const IPAddress WIFI_AP_IP(192, 168, 4, 1);
static const IPAddress WIFI_AP_GATEWAY(192, 168, 4, 1);
static const IPAddress WIFI_AP_NETMASK(255, 255, 255, 0);

bool mdnsStarted = false;
bool apActive = false;
uint32_t lastWifiRetryMs = 0;

// -----------------------------------------------------------------------------
// Working limits
// -----------------------------------------------------------------------------
static constexpr uint32_t DEFAULT_TARGET_HZ = 318000000UL;
static constexpr uint16_t DEFAULT_CAPTURE_BW_KHZ = 650;
static constexpr uint32_t DEFAULT_SWEEP_START_HZ = 317700000UL;
static constexpr uint32_t DEFAULT_SWEEP_STOP_HZ = 318300000UL;
static constexpr uint32_t DEFAULT_SWEEP_STEP_HZ = 20000UL;
static constexpr uint16_t DEFAULT_SWEEP_DWELL_MS = 3;
static constexpr uint16_t DEFAULT_SWEEP_BW_KHZ = 650;
static constexpr uint32_t DEFAULT_CAPTURE_MS = 2500UL;
static constexpr uint32_t DEFAULT_MIN_PULSE_US = 40UL;

// MegaCode-oriented decode defaults. These remain configurable from the Web UI.
static constexpr uint32_t DEFAULT_MEGA_PULSE_US = 1000UL;
static constexpr uint32_t DEFAULT_MEGA_PULSE_TOL_US = 300UL;
static constexpr uint32_t DEFAULT_MEGA_SYMBOL_US = 6000UL;
static constexpr uint32_t DEFAULT_MEGA_SYMBOL_TOL_US = 1200UL;
static constexpr uint32_t DEFAULT_MEGA_FRAME_GAP_US = 10000UL;
static constexpr uint32_t DEFAULT_MEGA_HEADER_LOW_US = 13000UL;
static constexpr uint32_t DEFAULT_MEGA_HEADER_TOL_US = 3400UL;

static constexpr uint16_t MAX_PULSES = 3072;
static constexpr uint16_t MAX_SWEEP_POINTS = 601;
static constexpr uint32_t MAX_CAPTURE_MS = 10000;

SmartRC_CC1101 radio;
ESP8266WebServer server(80);

// -----------------------------------------------------------------------------
// Sample format
// -----------------------------------------------------------------------------
#pragma pack(push, 1)
struct SampleHeader {
  char magic[4];          // "315R"
  uint8_t version;        // 1
  uint8_t reserved[3];
  uint32_t frequencyHz;
  uint32_t pulseCount;
  uint32_t durationUs;
};

struct DiskPulse {
  uint32_t durationUs;
  uint8_t level;
};
#pragma pack(pop)

// -----------------------------------------------------------------------------
// Raw pulse buffer
// Each entry describes the logic level held for durationUs.
// -----------------------------------------------------------------------------
volatile uint32_t pulseDurations[MAX_PULSES];
volatile uint8_t pulseLevels[MAX_PULSES];
volatile uint16_t pulseCount = 0;

volatile bool captureActive = false;
volatile uint32_t captureLastEdgeUs = 0;
volatile uint8_t capturePreviousLevel = LOW;
uint32_t captureStartUs = 0;
uint32_t captureDeadlineMs = 0;
bool captureDone = false;
uint32_t captureDurationUs = 0;

// -----------------------------------------------------------------------------
// Spectrum sweep state
// -----------------------------------------------------------------------------
uint32_t sweepHz[MAX_SWEEP_POINTS];
int16_t sweepRssi[MAX_SWEEP_POINTS];
uint16_t sweepCount = 0;

bool sweepActive = false;
bool sweepDone = false;
uint32_t sweepStartHz = DEFAULT_SWEEP_START_HZ;
uint32_t sweepStopHz = DEFAULT_SWEEP_STOP_HZ;
uint32_t sweepStepHz = DEFAULT_SWEEP_STEP_HZ;
uint32_t sweepCurrentHz = DEFAULT_SWEEP_START_HZ;
uint16_t sweepDwellMs = DEFAULT_SWEEP_DWELL_MS;
uint16_t sweepBandwidthKhz = DEFAULT_SWEEP_BW_KHZ;

// -----------------------------------------------------------------------------
// Radio/application state
// -----------------------------------------------------------------------------
uint32_t targetFrequencyHz = DEFAULT_TARGET_HZ;
uint16_t targetBandwidthKhz = DEFAULT_CAPTURE_BW_KHZ;
float radioDataRateKBaud = 38.4f;
int txPowerDbm = 10;
bool radioOk = false;
String currentMode = "idle";

// Runtime capture/decode parameters. The browser restores these from localStorage
// and pushes them to the device on page load.
uint32_t minPulseUs = DEFAULT_MIN_PULSE_US;
uint32_t megaPulseUs = DEFAULT_MEGA_PULSE_US;
uint32_t megaPulseTolUs = DEFAULT_MEGA_PULSE_TOL_US;
uint32_t megaSymbolUs = DEFAULT_MEGA_SYMBOL_US;
uint32_t megaSymbolTolUs = DEFAULT_MEGA_SYMBOL_TOL_US;
uint32_t megaFrameGapUs = DEFAULT_MEGA_FRAME_GAP_US;
uint32_t megaHeaderLowUs = DEFAULT_MEGA_HEADER_LOW_US;
uint32_t megaHeaderTolUs = DEFAULT_MEGA_HEADER_TOL_US;

// -----------------------------------------------------------------------------
// Playback state: Timer1 is 5 MHz with TIM_DIV16 => 5 ticks/us.
// -----------------------------------------------------------------------------
volatile uint16_t txIndex = 0;
volatile uint16_t txCount = 0;
volatile bool txDone = true;
volatile bool txInvert = false;

bool playbackRequested = false;
uint8_t playbackRepeats = 1;
bool playbackInvertRequested = false;

// -----------------------------------------------------------------------------
// Gate control (two-button operator page + assignment page)
// -----------------------------------------------------------------------------
// Each gate button replays one saved sample. The assignment (sample + radio
// settings) persists to LittleFS. Every gate fire forces the CC1101 to maximum
// output power: this module has a mismatched/stub antenna and marginal RF
// matching, so link budget is poor and "just open the gate" wins over finesse.
// Staying within local RF regulations is the operator's responsibility.
static constexpr int GATE_TX_POWER_DBM = 12;    // CC1101 max PA setting
static constexpr uint8_t GATE_MIN_REPEATS = 4;  // marginal link => repeat bursts
static constexpr char GATE_CONFIG_PATH[] = "/gate.bin";

#pragma pack(push, 1)
struct GateAssignment {
  char sampleName[24];
  uint32_t frequencyHz;
  uint16_t bandwidthKhz;
  int8_t txPowerDbm;      // kept for the UI; transmit clamps up to GATE_TX_POWER_DBM
  uint8_t repeats;
  uint8_t invert;
  uint8_t reserved[3];
};

struct GateConfigFile {
  char magic[4];          // "GATE"
  uint8_t version;        // 1
  uint8_t reserved[3];
  GateAssignment inner;
  GateAssignment outer;
};
#pragma pack(pop)

GateAssignment gateInner{};
GateAssignment gateOuter{};

bool gateFireRequested = false;
GateAssignment gateFirePlan{};

// -----------------------------------------------------------------------------
// Utility
// -----------------------------------------------------------------------------
static String jsonBool(bool v) {
  return v ? "true" : "false";
}


static String jsonEscape(const String& input) {
  String out;
  out.reserve(input.length() + 8);

  static const char hex[] = "0123456789ABCDEF";
  for(size_t i = 0; i < input.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(input[i]);
    switch(c) {
      case '"': out += F("\\\""); break;
      case '\\': out += F("\\\\"); break;
      case '\b': out += F("\\b"); break;
      case '\f': out += F("\\f"); break;
      case '\n': out += F("\\n"); break;
      case '\r': out += F("\\r"); break;
      case '\t': out += F("\\t"); break;
      default:
        if(c < 0x20) {
          out += F("\\u00");
          out += hex[(c >> 4) & 0x0F];
          out += hex[c & 0x0F];
        } else {
          out += static_cast<char>(c);
        }
        break;
    }
  }
  return out;
}

static bool valid315BandFrequency(uint32_t hz) {
  return hz >= 300000000UL && hz <= 348000000UL;
}

static String safeName(String n) {
  n.trim();
  String out;
  for(size_t i = 0; i < n.length() && out.length() < 24; ++i) {
    char c = n[i];
    if((c >= 'a' && c <= 'z') ||
       (c >= 'A' && c <= 'Z') ||
       (c >= '0' && c <= '9') || c == '-' || c == '_') {
      out += c;
    }
  }
  if(!out.length()) out = "sample";
  return out;
}

static String samplePath(const String& name) {
  return "/rf315_" + safeName(name) + ".bin";
}

static void sendJson(const String& body, int code = 200) {
  server.send(code, "application/json", body);
}

static void sendError(const String& msg, int code = 400) {
  sendJson("{\"ok\":false,\"error\":\"" + msg + "\"}", code);
}

static bool isBusy() {
  return captureActive || sweepActive || currentMode == "playback";
}

static void writeGdoFast(uint8_t level) {
  if(level) GPOS = (1U << PIN_CC1101_GDO0);
  else GPOC = (1U << PIN_CC1101_GDO0);
}

// -----------------------------------------------------------------------------
// CC1101 configuration
// -----------------------------------------------------------------------------
static uint32_t setRadioFrequency(uint32_t hz) {
  // Keep the requested frequency authoritative. Some SmartRC/CC1101 combinations
  // have returned 0.0 from getMHZ() even though setMHZ() programmed the radio.
  // Feeding that readback into application state caused the old Web UI to fall
  // back to 0 MHz and made every subsequent sweep target zero.
  radio.setSidle();
  radio.setMHZ((float)hz / 1000000.0f);
  return hz;
}

static void configureRawOok(uint16_t bwKhz) {
  radio.setSidle();

  // SmartRC's external/async mode plus explicit settings for raw GDO0 operation.
  radio.setCCMode(false);
  radio.setModulation(2);       // ASK/OOK
  radio.setDRate(radioDataRateKBaud); // async modem/data-rate reference
  radio.setRxBW((float)bwKhz);
  radio.setSyncMode(0);         // no preamble/sync qualifier
  radio.setWhiteData(false);
  radio.setManchester(false);
  radio.setFEC(false);
  radio.setCrc(false);
  radio.setPktFormat(3);        // asynchronous serial mode
  radio.setPA(txPowerDbm);

  // In asynchronous serial mode GDO0 is the TX data input. Route demodulated
  // RX serial data to GDO2 so RX capture and TX playback use separate MCU pins.
  radio.SpiWriteReg(CC1101_IOCFG0, 0x2E); // GDO0 high impedance while receiving
  radio.SpiWriteReg(CC1101_IOCFG2, 0x0D); // GDO2 serial data output
}

static void configureAnalyzer(uint16_t bwKhz) {
  configureRawOok(bwKhz);

  // Focused RSSI analyzer front-end values used by the Flipper-style approach.
  // SmartRC remains responsible for SPI, frequency calculation and mode control.
  radio.SpiWriteReg(CC1101_MDMCFG3, 0x7F);
  radio.SpiWriteReg(CC1101_AGCCTRL2, 0x07);
  radio.SpiWriteReg(CC1101_AGCCTRL1, 0x08);
  radio.SpiWriteReg(CC1101_AGCCTRL0, 0x30);
  radio.setRxBW((float)bwKhz);
}

static void restoreTargetRx() {
  configureRawOok(targetBandwidthKhz);
  setRadioFrequency(targetFrequencyHz);
  radio.SpiWriteReg(CC1101_IOCFG0, 0x2E);
  radio.SpiWriteReg(CC1101_IOCFG2, 0x0D);
  pinMode(PIN_CC1101_GDO0, INPUT);
  pinMode(PIN_CC1101_GDO2, INPUT);
  radio.SetRx();
}

// -----------------------------------------------------------------------------
// Raw capture ISR and capture control
// -----------------------------------------------------------------------------
void IRAM_ATTR rawEdgeISR() {
  if(!captureActive) return;

  uint32_t now = micros();
  uint32_t duration = now - captureLastEdgeUs;
  uint8_t currentLevel = (GPI & (1U << PIN_CC1101_GDO2)) ? HIGH : LOW;
  uint8_t endedLevel = capturePreviousLevel;

  capturePreviousLevel = currentLevel;
  captureLastEdgeUs = now;

  // Ignore very short async glitches. Levels are stored explicitly so skipped
  // glitches cannot corrupt the later HIGH/LOW sequence.
  if(duration < minPulseUs) return;

  uint16_t i = pulseCount;
  if(i >= MAX_PULSES) {
    captureActive = false;
    return;
  }

  pulseDurations[i] = duration;
  pulseLevels[i] = endedLevel;
  pulseCount = i + 1;
}

static void normalizeCapture() {
  uint16_t n = pulseCount;
  if(!n) return;

  uint16_t out = 0;
  for(uint16_t i = 0; i < n; ++i) {
    uint32_t d = pulseDurations[i];
    uint8_t level = pulseLevels[i];
    if(d < minPulseUs) continue;

    if(out && pulseLevels[out - 1] == level) {
      pulseDurations[out - 1] += d;
    } else {
      pulseDurations[out] = d;
      pulseLevels[out] = level;
      ++out;
    }
  }
  pulseCount = out;
}

static void startCapture(uint32_t maxMs) {
  pulseCount = 0;
  captureDone = false;
  captureDurationUs = 0;

  configureRawOok(targetBandwidthKhz);
  setRadioFrequency(targetFrequencyHz);
  radio.SpiWriteReg(CC1101_IOCFG0, 0x2E);
  radio.SpiWriteReg(CC1101_IOCFG2, 0x0D);
  pinMode(PIN_CC1101_GDO0, INPUT);
  pinMode(PIN_CC1101_GDO2, INPUT);
  radio.SetRx();
  delayMicroseconds(300);

  capturePreviousLevel = digitalRead(PIN_CC1101_GDO2) ? HIGH : LOW;
  captureStartUs = micros();
  captureLastEdgeUs = captureStartUs;
  captureDeadlineMs = millis() + maxMs;
  captureActive = true;
  currentMode = "recording";

  attachInterrupt(digitalPinToInterrupt(PIN_CC1101_GDO2), rawEdgeISR, CHANGE);
}

static void stopCapture() {
  if(!captureActive && currentMode != "recording") return;

  detachInterrupt(digitalPinToInterrupt(PIN_CC1101_GDO2));

  noInterrupts();
  bool wasActive = captureActive;
  captureActive = false;
  uint32_t now = micros();
  uint32_t tail = now - captureLastEdgeUs;
  uint8_t tailLevel = capturePreviousLevel;
  uint16_t n = pulseCount;
  if(wasActive && tail >= minPulseUs && n < MAX_PULSES) {
    pulseDurations[n] = tail;
    pulseLevels[n] = tailLevel;
    pulseCount = n + 1;
  }
  interrupts();

  radio.setSidle();
  captureDurationUs = now - captureStartUs;
  normalizeCapture();
  captureDone = pulseCount > 0;
  currentMode = "idle";
  restoreTargetRx();
}

// -----------------------------------------------------------------------------
// LittleFS sample storage
// -----------------------------------------------------------------------------
static bool saveCurrentSample(const String& name) {
  if(!captureDone || pulseCount == 0) return false;

  File f = LittleFS.open(samplePath(name), "w");
  if(!f) return false;

  SampleHeader h = {{'3','1','5','R'}, 1, {0,0,0}, targetFrequencyHz, pulseCount, captureDurationUs};
  if(f.write(reinterpret_cast<const uint8_t*>(&h), sizeof(h)) != sizeof(h)) {
    f.close();
    return false;
  }

  for(uint16_t i = 0; i < pulseCount; ++i) {
    DiskPulse p = {pulseDurations[i], pulseLevels[i]};
    if(f.write(reinterpret_cast<const uint8_t*>(&p), sizeof(p)) != sizeof(p)) {
      f.close();
      return false;
    }
  }

  f.close();
  return true;
}

static bool loadSample(const String& name, SampleHeader* headerOut = nullptr) {
  File f = LittleFS.open(samplePath(name), "r");
  if(!f) return false;

  SampleHeader h{};
  if(f.read(reinterpret_cast<uint8_t*>(&h), sizeof(h)) != sizeof(h) ||
     memcmp(h.magic, "315R", 4) != 0 || h.version != 1 ||
     h.pulseCount == 0 || h.pulseCount > MAX_PULSES ||
     !valid315BandFrequency(h.frequencyHz)) {
    f.close();
    return false;
  }

  for(uint32_t i = 0; i < h.pulseCount; ++i) {
    DiskPulse p{};
    if(f.read(reinterpret_cast<uint8_t*>(&p), sizeof(p)) != sizeof(p)) {
      f.close();
      return false;
    }
    pulseDurations[i] = p.durationUs;
    pulseLevels[i] = p.level ? HIGH : LOW;
  }
  f.close();

  pulseCount = (uint16_t)h.pulseCount;
  captureDurationUs = h.durationUs;
  captureDone = true;
  targetFrequencyHz = h.frequencyHz;
  normalizeCapture();

  if(headerOut) *headerOut = h;
  return true;
}

// -----------------------------------------------------------------------------
// Timer1 raw playback
// -----------------------------------------------------------------------------
void IRAM_ATTR rawPlaybackTimerISR() {
  ++txIndex;

  if(txIndex >= txCount) {
    writeGdoFast(LOW);
    timer1_disable();
    txDone = true;
    return;
  }

  uint8_t level = pulseLevels[txIndex];
  if(txInvert) level = !level;
  writeGdoFast(level);

  uint32_t us = pulseDurations[txIndex];
  if(us < 2) us = 2;
  if(us > 1600000UL) us = 1600000UL;
  timer1_write(us * 5U);
}

static void replayCurrent(uint8_t repeats, bool invert) {
  if(!captureDone || pulseCount == 0) return;

  currentMode = "playback";
  configureRawOok(targetBandwidthKhz);
  setRadioFrequency(targetFrequencyHz);

  // During asynchronous TX the MCU drives GDO0 as the serial TX-data input.
  // Put the CC1101's configurable GDO output in high impedance first.
  radio.SpiWriteReg(CC1101_IOCFG0, 0x2E);
  radio.SpiWriteReg(CC1101_IOCFG2, 0x2E);
  pinMode(PIN_CC1101_GDO0, OUTPUT);
  writeGdoFast(LOW);
  radio.SetTx();
  delayMicroseconds(300);

  repeats = constrain(repeats, (uint8_t)1, (uint8_t)10);

  for(uint8_t rep = 0; rep < repeats; ++rep) {
    txIndex = 0;
    txCount = pulseCount;
    txInvert = invert;
    txDone = false;

    uint8_t firstLevel = pulseLevels[0];
    if(txInvert) firstLevel = !firstLevel;
    writeGdoFast(firstLevel);

    timer1_disable();
    timer1_detachInterrupt();
    timer1_isr_init();
    timer1_attachInterrupt(rawPlaybackTimerISR);
    timer1_enable(TIM_DIV16, TIM_EDGE, TIM_SINGLE);

    uint32_t firstUs = pulseDurations[0];
    if(firstUs < 2) firstUs = 2;
    if(firstUs > 1600000UL) firstUs = 1600000UL;
    timer1_write(firstUs * 5U);

    while(!txDone) delay(0);

    timer1_disable();
    timer1_detachInterrupt();
    writeGdoFast(LOW);

    if(rep + 1 < repeats) delay(20);
  }

  radio.setSidle();
  writeGdoFast(LOW);
  pinMode(PIN_CC1101_GDO0, INPUT);
  pinMode(PIN_CC1101_GDO2, INPUT);
  currentMode = "idle";
  restoreTargetRx();
}

// -----------------------------------------------------------------------------
// Gate control: storage + fire
// -----------------------------------------------------------------------------
static bool gateAssignmentValid(const GateAssignment& a) {
  return a.sampleName[0] != '\0' &&
         valid315BandFrequency(a.frequencyHz) &&
         (a.bandwidthKhz == 58 || a.bandwidthKhz == 270 || a.bandwidthKhz == 650) &&
         a.repeats >= 1 && a.repeats <= 10;
}

static bool gateSampleExists(const GateAssignment& a) {
  if(a.sampleName[0] == '\0') return false;
  return LittleFS.exists(samplePath(String(a.sampleName)));
}

static void gateLoadConfig() {
  gateInner = GateAssignment{};
  gateOuter = GateAssignment{};

  File f = LittleFS.open(GATE_CONFIG_PATH, "r");
  if(!f) return;

  GateConfigFile c{};
  bool ok = f.read(reinterpret_cast<uint8_t*>(&c), sizeof(c)) == sizeof(c) &&
            memcmp(c.magic, "GATE", 4) == 0 && c.version == 1;
  f.close();
  if(!ok) return;

  gateInner = c.inner;
  gateOuter = c.outer;
}

static bool gateSaveConfig() {
  GateConfigFile c{};
  memcpy(c.magic, "GATE", 4);
  c.version = 1;
  c.inner = gateInner;
  c.outer = gateOuter;

  File f = LittleFS.open(GATE_CONFIG_PATH, "w");
  if(!f) return false;
  bool ok = f.write(reinterpret_cast<const uint8_t*>(&c), sizeof(c)) == sizeof(c);
  f.close();
  return ok;
}

// Runs from loop() when the radio is free. Applies the assignment's stored
// frequency/bandwidth, forces maximum TX power, replays, then restores the
// dashboard's RX state and target frequency.
static bool fireGate(const GateAssignment& a) {
  if(!gateAssignmentValid(a)) return false;

  // Remember the dashboard's radio state before loadSample() overwrites it.
  const uint32_t savedFreq = targetFrequencyHz;
  const uint16_t savedBw = targetBandwidthKhz;
  const int savedPwr = txPowerDbm;

  if(!loadSample(String(a.sampleName))) return false;  // note: clobbers in-memory capture

  targetFrequencyHz = a.frequencyHz;
  targetBandwidthKhz = a.bandwidthKhz;
  txPowerDbm = GATE_TX_POWER_DBM;  // deliberate: mismatched antenna, weak link

  uint8_t repeats = a.repeats;
  if(repeats < GATE_MIN_REPEATS) repeats = GATE_MIN_REPEATS;

  replayCurrent(repeats, a.invert != 0);

  targetFrequencyHz = savedFreq;
  targetBandwidthKhz = savedBw;
  txPowerDbm = savedPwr;
  restoreTargetRx();
  return true;
}

static String gateAssignmentJson(const GateAssignment& a) {
  String s = "{";
  s += "\"assigned\":" + jsonBool(a.sampleName[0] != '\0');
  s += ",\"sampleName\":\"" + jsonEscape(String(a.sampleName)) + "\"";
  s += ",\"sampleExists\":" + jsonBool(gateSampleExists(a));
  s += ",\"frequencyHz\":" + String(a.frequencyHz);
  s += ",\"bandwidthKhz\":" + String(a.bandwidthKhz);
  s += ",\"txPowerDbm\":" + String(a.txPowerDbm);
  s += ",\"repeats\":" + String(a.repeats);
  s += ",\"invert\":" + jsonBool(a.invert != 0);
  s += ",\"ready\":" + jsonBool(gateAssignmentValid(a) && gateSampleExists(a));
  s += "}";
  return s;
}

// -----------------------------------------------------------------------------
// Generic timing clustering / decode helpers
// -----------------------------------------------------------------------------
struct Clusters {
  float shortUs = 0;
  float longUs = 0;
  uint16_t count = 0;
};

static Clusters kmeans2All(uint32_t minV = 40, uint32_t maxV = 12000) {
  Clusters r{};
  uint16_t n = pulseCount;
  if(!n) return r;

  float lo = 1e9f;
  float hi = 0;
  uint16_t good = 0;

  for(uint16_t i = 0; i < n; ++i) {
    uint32_t v = pulseDurations[i];
    if(v < minV || v > maxV) continue;
    if(v < lo) lo = v;
    if(v > hi) hi = v;
    ++good;
  }
  if(!good) return r;

  float c0 = lo;
  float c1 = hi;
  for(uint8_t iter = 0; iter < 12; ++iter) {
    double s0 = 0, s1 = 0;
    uint16_t n0 = 0, n1 = 0;
    for(uint16_t i = 0; i < n; ++i) {
      uint32_t v = pulseDurations[i];
      if(v < minV || v > maxV) continue;
      if(fabsf((float)v - c0) <= fabsf((float)v - c1)) {
        s0 += v; ++n0;
      } else {
        s1 += v; ++n1;
      }
    }
    if(n0) c0 = (float)(s0 / n0);
    if(n1) c1 = (float)(s1 / n1);
  }

  if(c0 > c1) {
    float t = c0; c0 = c1; c1 = t;
  }
  r.shortUs = c0;
  r.longUs = c1;
  r.count = good;
  return r;
}

static Clusters kmeans2Level(uint8_t wantedLevel, uint32_t minV = 40, uint32_t maxV = 12000) {
  Clusters r{};
  uint16_t n = pulseCount;
  float lo = 1e9f;
  float hi = 0;
  uint16_t good = 0;

  for(uint16_t i = 0; i < n; ++i) {
    if(pulseLevels[i] != wantedLevel) continue;
    uint32_t v = pulseDurations[i];
    if(v < minV || v > maxV) continue;
    if(v < lo) lo = v;
    if(v > hi) hi = v;
    ++good;
  }
  if(!good) return r;

  float c0 = lo;
  float c1 = hi;
  for(uint8_t iter = 0; iter < 12; ++iter) {
    double s0 = 0, s1 = 0;
    uint16_t n0 = 0, n1 = 0;
    for(uint16_t i = 0; i < n; ++i) {
      if(pulseLevels[i] != wantedLevel) continue;
      uint32_t v = pulseDurations[i];
      if(v < minV || v > maxV) continue;
      if(fabsf((float)v - c0) <= fabsf((float)v - c1)) {
        s0 += v; ++n0;
      } else {
        s1 += v; ++n1;
      }
    }
    if(n0) c0 = (float)(s0 / n0);
    if(n1) c1 = (float)(s1 / n1);
  }

  if(c0 > c1) {
    float t = c0; c0 = c1; c1 = t;
  }
  r.shortUs = c0;
  r.longUs = c1;
  r.count = good;
  return r;
}

static bool nearDuration(uint32_t value, uint32_t target, uint32_t tolerance) {
  return value > target ? (value - target <= tolerance) : (target - value <= tolerance);
}

struct ProtocolDecode {
  bool matched = false;
  String name;
  String bits;
  uint32_t data = 0;
  String details;
};

// Focused recognizer for common Linear 10-bit timing used in the 315/318 MHz area.
static ProtocolDecode tryLinear() {
  ProtocolDecode out;
  enum State { WAIT_HEADER, SAVE_HIGH, CHECK_LOW } state = WAIT_HEADER;
  uint32_t savedHigh = 0;
  uint32_t data = 0;
  uint8_t bitCount = 0;

  auto addBit = [&](uint8_t bit) {
    data = (data << 1) | bit;
    ++bitCount;
  };

  for(uint16_t i = 0; i < pulseCount; ++i) {
    uint8_t level = pulseLevels[i];
    uint32_t dur = pulseDurations[i];

    if(state == WAIT_HEADER) {
      if(level == LOW && nearDuration(dur, 21000, 5250)) {
        data = 0;
        bitCount = 0;
        state = SAVE_HIGH;
      }
    } else if(state == SAVE_HIGH) {
      if(level == HIGH) {
        savedHigh = dur;
        state = CHECK_LOW;
      } else {
        state = WAIT_HEADER;
      }
    } else {
      if(level != LOW) {
        state = WAIT_HEADER;
        continue;
      }

      if(dur >= 2500) {
        bool guard = nearDuration(dur, 21000, 5250);
        if(guard) {
          if(nearDuration(savedHigh, 500, 350)) addBit(0);
          else if(nearDuration(savedHigh, 1500, 350)) addBit(1);

          if(bitCount == 10) {
            out.matched = true;
            out.name = "Linear 10-bit";
            out.data = data;
            for(int b = 9; b >= 0; --b) out.bits += ((data >> b) & 1) ? '1' : '0';
            out.details = "~500/1500 us pulse-pair timing with ~21 ms frame guard";
            return out;
          }
        }
        state = WAIT_HEADER;
      } else if(nearDuration(savedHigh, 500, 350) && nearDuration(dur, 1500, 350)) {
        addBit(0);
        state = SAVE_HIGH;
      } else if(nearDuration(savedHigh, 1500, 350) && nearDuration(dur, 500, 350)) {
        addBit(1);
        state = SAVE_HIGH;
      } else {
        state = WAIT_HEADER;
      }
    }
  }
  return out;
}

// Focused recognizer for common 24-bit MegaCode timing.
static ProtocolDecode tryMegaCode() {
  ProtocolDecode out;
  enum State { WAIT_HEADER, START_BIT, SAVE_LOW, CHECK_HIGH } state = WAIT_HEADER;
  uint32_t data = 0;
  uint8_t bitCount = 0;
  uint8_t lastBit = 0;
  int32_t teLast = 0;
  const uint32_t halfSymbolUs = megaSymbolUs / 2;
  const uint32_t longLowTargetUs = (megaSymbolUs > megaPulseUs) ? (megaSymbolUs - megaPulseUs) : 1;
  const uint32_t shortLowTargetUs = (halfSymbolUs > megaPulseUs) ? (halfSymbolUs - megaPulseUs) : 1;

  auto addBit = [&](uint8_t bit) {
    data = (data << 1) | bit;
    ++bitCount;
  };

  for(uint16_t i = 0; i < pulseCount; ++i) {
    uint8_t level = pulseLevels[i];
    uint32_t dur = pulseDurations[i];

    switch(state) {
      case WAIT_HEADER:
        if(level == LOW && nearDuration(dur, megaHeaderLowUs, megaHeaderTolUs)) state = START_BIT;
        break;

      case START_BIT:
        if(level == HIGH && nearDuration(dur, megaPulseUs, megaPulseTolUs)) {
          data = 0;
          bitCount = 0;
          addBit(1);
          lastBit = 1;
          state = SAVE_LOW;
        } else {
          state = WAIT_HEADER;
        }
        break;

      case SAVE_LOW:
        if(level != LOW) {
          state = WAIT_HEADER;
          break;
        }
        if(dur >= megaFrameGapUs) {
          if(bitCount == 24 && ((data >> 23) & 1)) {
            out.matched = true;
            out.name = "MegaCode 24-bit";
            out.data = data;
            for(int b = 23; b >= 0; --b) out.bits += ((data >> b) & 1) ? '1' : '0';
            uint32_t serial = (data >> 3) & 0xFFFF;
            uint32_t facility = (data >> 19) & 0x0F;
            uint32_t button = data & 0x07;
            out.details = "facility=" + String(facility) + ", serial=" + String(serial) + ", button=" + String(button);
            return out;
          }
          state = WAIT_HEADER;
          break;
        }

        teLast = lastBit ? (int32_t)dur : (int32_t)dur - (int32_t)halfSymbolUs;
        state = CHECK_HIGH;
        break;

      case CHECK_HIGH:
        if(level != HIGH) {
          state = WAIT_HEADER;
          break;
        }
        if(nearDuration(dur, megaPulseUs, megaPulseTolUs) && teLast > 0 && nearDuration((uint32_t)teLast, longLowTargetUs, megaSymbolTolUs)) {
          addBit(1);
          lastBit = 1;
          state = SAVE_LOW;
        } else if(nearDuration(dur, megaPulseUs, megaPulseTolUs) && teLast > 0 && nearDuration((uint32_t)teLast, shortLowTargetUs, megaSymbolTolUs / 2)) {
          addBit(0);
          lastBit = 0;
          state = SAVE_LOW;
        } else {
          state = WAIT_HEADER;
        }
        break;
    }
  }
  return out;
}

static String decodeCurrentJson() {
  uint16_t n = pulseCount;
  if(n < 4) return "{\"ok\":false,\"error\":\"not enough pulse data\"}";

  Clusters all = kmeans2All();
  Clusters high = kmeans2Level(HIGH);
  Clusters low = kmeans2Level(LOW);
  if(!all.count) return "{\"ok\":false,\"error\":\"no usable pulse widths\"}";

  float allSplit = (all.shortUs + all.longUs) * 0.5f;
  float allRatio = all.shortUs > 0 ? all.longUs / all.shortUs : 0;
  float highRatio = high.shortUs > 0 ? high.longUs / high.shortUs : 1;
  float lowRatio = low.shortUs > 0 ? low.longUs / low.shortUs : 1;
  float highSplit = (high.shortUs + high.longUs) * 0.5f;
  float lowSplit = (low.shortUs + low.longUs) * 0.5f;

  String encoding = "unknown / mixed timing";
  if(high.count && low.count) {
    if(highRatio < 1.45f && lowRatio > 1.70f) {
      encoding = "pulse-distance / PPM-style";
    } else if(lowRatio < 1.45f && highRatio > 1.70f) {
      encoding = "pulse-width / PWM-style";
    } else if(highRatio > 1.70f && lowRatio > 1.70f && allRatio > 1.7f && allRatio < 4.8f) {
      encoding = "complementary short-long pulse pairs";
    } else if(allRatio > 1.7f && allRatio < 4.8f) {
      encoding = "short-long timing family";
    }
  }

  float syncThreshold = max(5000.0f, all.longUs * 4.0f);
  String bits;
  String inverted;

  for(uint16_t i = 0; i < n && bits.length() < 220; ++i) {
    if(pulseDurations[i] > syncThreshold) {
      if(bits.length() && bits[bits.length() - 1] != '|') {
        bits += '|';
        inverted += '|';
      }
      continue;
    }

    if(pulseLevels[i] != HIGH || i + 1 >= n || pulseLevels[i + 1] != LOW) continue;

    uint32_t h = pulseDurations[i];
    uint32_t l = pulseDurations[i + 1];
    if(l > syncThreshold) {
      if(bits.length() && bits[bits.length() - 1] != '|') {
        bits += '|'; inverted += '|';
      }
      ++i;
      continue;
    }

    char bit = '?';
    if(encoding.startsWith("pulse-distance")) {
      bit = (l < lowSplit) ? '0' : '1';
    } else if(encoding.startsWith("pulse-width")) {
      bit = (h < highSplit) ? '0' : '1';
    } else {
      bool hs = h < allSplit;
      bool ls = l < allSplit;
      if(hs && !ls) bit = '0';
      else if(!hs && ls) bit = '1';
    }

    bits += bit;
    inverted += (bit == '0') ? '1' : (bit == '1') ? '0' : '?';
    ++i;
  }

  ProtocolDecode linear = tryLinear();
  ProtocolDecode mega = tryMegaCode();
  ProtocolDecode specific = linear.matched ? linear : mega;

  String out = "{\"ok\":true";
  out += ",\"frequencyHz\":" + String(targetFrequencyHz);
  out += ",\"pulses\":" + String(n);
  out += ",\"encoding\":\"" + encoding + "\"";
  out += ",\"short_us\":" + String(all.shortUs, 1);
  out += ",\"long_us\":" + String(all.longUs, 1);
  out += ",\"ratio\":" + String(allRatio, 2);
  out += ",\"sync_gap_threshold_us\":" + String(syncThreshold, 0);
  out += ",\"high_clusters_us\":[" + String(high.shortUs, 1) + "," + String(high.longUs, 1) + "]";
  out += ",\"low_clusters_us\":[" + String(low.shortUs, 1) + "," + String(low.longUs, 1) + "]";
  out += ",\"candidate_bits\":\"" + bits + "\"";
  out += ",\"candidate_bits_inverted\":\"" + inverted + "\"";

  if(specific.matched) {
    out += ",\"protocol_candidate\":\"" + specific.name + "\"";
    out += ",\"protocol_bits\":\"" + specific.bits + "\"";
    out += ",\"protocol_data_dec\":" + String(specific.data);
    out += ",\"protocol_details\":\"" + specific.details + "\"";
  } else {
    out += ",\"protocol_candidate\":null";
  }

  out += "}";
  return out;
}

static String pulseHistogramJson() {
  static constexpr uint8_t BINS = 28;
  uint16_t counts[BINS] = {0};
  uint16_t highs[BINS] = {0};
  uint16_t lows[BINS] = {0};

  uint16_t n = pulseCount;
  if(!n) return "{\"counts\":[],\"high\":[],\"low\":[],\"labels\":[]}";

  uint32_t maxV = 0;
  for(uint16_t i = 0; i < n; ++i) {
    uint32_t v = pulseDurations[i];
    if(v <= 12000 && v > maxV) maxV = v;
  }
  if(maxV < 1000) maxV = 1000;

  uint32_t binW = (maxV + BINS - 1) / BINS;
  if(binW == 0) binW = 1;

  for(uint16_t i = 0; i < n; ++i) {
    uint32_t v = pulseDurations[i];
    if(v > maxV) continue;
    uint32_t bi = v / binW;
    if(bi >= BINS) bi = BINS - 1;
    ++counts[bi];
    if(pulseLevels[i]) ++highs[bi];
    else ++lows[bi];
  }

  String s = "{\"binWidthUs\":" + String(binW) + ",\"counts\":[";
  for(uint8_t i = 0; i < BINS; ++i) {
    if(i) s += ',';
    s += String(counts[i]);
  }
  s += "],\"high\":[";
  for(uint8_t i = 0; i < BINS; ++i) {
    if(i) s += ',';
    s += String(highs[i]);
  }
  s += "],\"low\":[";
  for(uint8_t i = 0; i < BINS; ++i) {
    if(i) s += ',';
    s += String(lows[i]);
  }
  s += "],\"labels\":[";
  for(uint8_t i = 0; i < BINS; ++i) {
    if(i) s += ',';
    s += '"'; s += String(i * binW); s += '"';
  }
  s += "]}";
  return s;
}

// -----------------------------------------------------------------------------
// Spectrum sweep
// -----------------------------------------------------------------------------
static void startSweep(uint32_t startHz, uint32_t stopHz, uint32_t stepHz,
                       uint16_t dwellMs, uint16_t bwKhz) {
  sweepStartHz = startHz;
  sweepStopHz = stopHz;
  sweepStepHz = stepHz;
  sweepCurrentHz = startHz;
  sweepDwellMs = dwellMs;
  sweepBandwidthKhz = bwKhz;
  sweepCount = 0;
  sweepDone = false;
  sweepActive = true;
  currentMode = "sweeping";
  configureAnalyzer(sweepBandwidthKhz);
}

static void serviceSweep() {
  if(!sweepActive) return;

  if(sweepCurrentHz > sweepStopHz || sweepCount >= MAX_SWEEP_POINTS) {
    sweepActive = false;
    sweepDone = true;
    currentMode = "idle";
    restoreTargetRx();
    return;
  }

  setRadioFrequency(sweepCurrentHz);
  radio.SetRx();
  delay(sweepDwellMs);
  int rssi = radio.getRssi();
  radio.setSidle();

  sweepHz[sweepCount] = sweepCurrentHz;
  sweepRssi[sweepCount] = (int16_t)rssi;
  ++sweepCount;

  if(UINT32_MAX - sweepCurrentHz < sweepStepHz) {
    sweepCurrentHz = sweepStopHz + 1;
  } else {
    sweepCurrentHz += sweepStepHz;
  }
}

// -----------------------------------------------------------------------------
// Web UI
// -----------------------------------------------------------------------------
static const char WEB_UI[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CC1101 318 MHz Multifunction</title>
<style>
:root{font-family:system-ui,-apple-system,Segoe UI,sans-serif;color:#eee;background:#101214}
body{max-width:1180px;margin:auto;padding:16px}
h1{margin-bottom:6px}.sub{color:#9aa0a6;margin-top:0}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(330px,1fr));gap:12px}
.card{background:#191c20;border:1px solid #30343a;border-radius:12px;padding:14px}
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:end;margin:8px 0}
label{font-size:12px;color:#aeb4bc;display:flex;flex-direction:column;gap:4px}
input,select,button{background:#22262b;color:#eee;border:1px solid #454b53;border-radius:8px;padding:8px}
button{cursor:pointer}button:hover{background:#2d3238}.primary{border-color:#6aa8ff}.danger{border-color:#ce6262}
.badge{display:inline-block;padding:4px 8px;border:1px solid #555c65;border-radius:999px}
.big{font-size:26px;font-weight:700}.muted{color:#979da5;font-size:12px}
canvas{width:100%;height:240px;background:#111418;border-radius:8px;border:1px solid #292d32}
pre{white-space:pre-wrap;word-break:break-word;background:#111418;padding:10px;border-radius:8px;max-height:380px;overflow:auto}
table{width:100%;border-collapse:collapse;font-size:13px}td,th{padding:7px;border-bottom:1px solid #30343a;text-align:left}
.tiny{font-size:11px;padding:5px 7px}.check{flex-direction:row;align-items:center;font-size:13px}
#peaks{font-family:ui-monospace,monospace;font-size:12px;white-space:pre-wrap}
</style>
</head>
<body>
<h1>CC1101 318 MHz Multifunction</h1>
<p class="sub">WeMOS D1 Mini + SmartRC CC1101 3.0.2 &nbsp;·&nbsp; <a href="/gate" style="color:#6aa8ff">Gate control</a></p>
<div class="row"><span id="radio" class="badge">radio…</span><span id="mode" class="badge">idle</span><span id="wifi" class="badge">Wi-Fi…</span><span id="ap" class="badge">AP…</span><span id="heap" class="muted"></span></div>
<div class="row"><button class="tiny" onclick="selftest()">Self-test</button><span id="fwinfo" class="muted"></span><span id="selftest" class="muted"></span></div>

<div class="grid">
<section class="card">
<h2>Target frequency</h2>
<div class="big"><span id="targetText">318.000000</span> MHz</div>
<div class="row">
<label>Frequency MHz<input id="target" type="number" value="318.000000" step="0.001" min="300" max="348"></label>
<label>Capture BW
<select id="bw"><option value="58">58 kHz</option><option value="270">270 kHz</option><option value="650" selected>650 kHz</option></select>
</label>
<button class="primary" onclick="tune()">Tune</button>
</div>
<div class="row">
<button onclick="nudge(-100000)">−100 kHz</button><button onclick="nudge(-20000)">−20 kHz</button><button onclick="nudge(-5000)">−5 kHz</button>
<button onclick="nudge(5000)">+5 kHz</button><button onclick="nudge(20000)">+20 kHz</button><button onclick="nudge(100000)">+100 kHz</button>
</div>
<div class="muted">SPI controls tuning/RSSI/mode. GDO2/D1 captures asynchronous RX data; GDO0/D2 drives asynchronous TX data.</div>
</section>

<section class="card">
<h2>Frequency population / RSSI sweep</h2>
<div class="row">
<label>Start MHz<input id="s0" value="317.700" type="number" step="0.001" min="300" max="348"></label>
<label>Stop MHz<input id="s1" value="318.300" type="number" step="0.001" min="300" max="348"></label>
<label>Step kHz<input id="ss" value="20" type="number" min="5"></label>
<label>Dwell ms<input id="sd" value="3" type="number" min="2" max="25"></label>
<label>RX BW<select id="sbw"><option value="58">58 kHz</option><option value="270">270 kHz</option><option value="650" selected>650 kHz</option></select></label>
</div>
<div class="row"><button class="primary" onclick="sweep()">Sweep</button><button onclick="focusSweep()">Target ±300 kHz</button></div>
<canvas id="spectrum" width="960" height="270"></canvas>
<div id="peaks" class="muted"></div>
</section>

<section class="card">
<h2>Raw record</h2>
<div class="row">
<label>Auto-stop ms<input id="recms" value="2500" type="number" min="50" max="10000"></label>
<button class="primary" onclick="recordStart()">Start record</button>
<button onclick="recordStop()">Stop</button>
<button onclick="decodeCurrent()">Decode current</button>
</div>
<div class="row">
<label>Sample name<input id="sampleName" value="sample01" maxlength="24"></label>
<button onclick="saveCurrent()">Save sample</button>
</div>
<div id="recinfo" class="muted">No capture loaded.</div>
<canvas id="pulses" width="960" height="270"></canvas>
</section>

<section class="card">
<h2>Capture / decoder parameters</h2>
<div class="row">
<label>Async data rate kBaud<input id="dataRate" value="38.4" type="number" step="0.1" min="0.02" max="1621.83"></label>
<label>TX power dBm<select id="txPower"><option>-30</option><option>-20</option><option>-15</option><option>-10</option><option>-6</option><option>0</option><option>5</option><option>7</option><option selected>10</option><option>11</option><option>12</option></select></label>
<label>Minimum pulse µs<input id="minPulse" value="40" type="number" min="5" max="2000"></label>
<label>Mega pulse µs<input id="megaPulse" value="1000" type="number" min="100" max="3000"></label>
<label>Pulse tolerance µs<input id="megaPulseTol" value="300" type="number" min="25" max="1500"></label>
<label>Symbol period µs<input id="megaSymbol" value="6000" type="number" min="1000" max="20000"></label>
<label>Symbol tolerance µs<input id="megaSymbolTol" value="1200" type="number" min="100" max="5000"></label>
<label>Frame gap µs<input id="megaFrameGap" value="10000" type="number" min="2000" max="50000"></label>
<label>Header low µs<input id="megaHeaderLow" value="13000" type="number" min="2000" max="50000"></label>
<label>Header tolerance µs<input id="megaHeaderTol" value="3400" type="number" min="100" max="10000"></label>
</div>
<div class="row"><button class="primary" onclick="saveUiSettings(true)">Save browser settings</button><button onclick="resetUiDefaults()">Reset 318 MHz defaults</button><span id="saved" class="muted"></span></div>
<div class="muted">Settings are stored in this browser and restored when the page reopens. Runtime decoder values are pushed back to the ESP8266 automatically.</div>
</section>

<section class="card">
<h2>Decode</h2>
<pre id="decode">No decode yet.</pre>
</section>
</div>

<section class="card">
<h2>Stored samples</h2>
<div class="row">
<label>Playback repeats<input id="repeats" type="number" min="1" max="10" value="1"></label>
<label class="check"><input id="invert" type="checkbox"> Invert playback logic</label>
<button onclick="listSamples()">Refresh</button>
</div>
<table>
<thead><tr><th>Name</th><th>MHz</th><th>Pulses</th><th>Duration</th><th>Actions</th></tr></thead>
<tbody id="samples"></tbody>
</table>
</section>

<script>
const $=id=>document.getElementById(id);
const STORE='cc1101_318_ui_v2';
const SWEEP_STORE='cc1101_318_last_sweep_v2';
const PULSE_STORE='cc1101_318_last_pulse_hist_v2';
const DECODE_STORE='cc1101_318_last_decode_v2';
const RECINFO_STORE='cc1101_318_last_recinfo_v2';
const FIELD_IDS=['target','bw','s0','s1','ss','sd','sbw','recms','sampleName','repeats','invert','dataRate','txPower','minPulse','megaPulse','megaPulseTol','megaSymbol','megaSymbolTol','megaFrameGap','megaHeaderLow','megaHeaderTol'];
let lastSweepDone=false,lastCaptureSignature='',statusTimer=null;

const DEFAULTS={
 target:'318.000000',bw:'650',s0:'317.700',s1:'318.300',ss:'20',sd:'3',sbw:'650',recms:'2500',sampleName:'sample01',repeats:'1',invert:false,
 dataRate:'38.4',txPower:'10',minPulse:'40',megaPulse:'1000',megaPulseTol:'300',megaSymbol:'6000',megaSymbolTol:'1200',megaFrameGap:'10000',megaHeaderLow:'13000',megaHeaderTol:'3400'
};

async function j(url){let r=await fetch(url,{cache:'no-store'});let t=await r.text();let o;try{o=JSON.parse(t)}catch(e){throw Error(t)}if(!r.ok||o.ok===false)throw Error(o.error||t);return o}
function esc(s){return String(s).replace(/[&<>"']/g,m=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[m]))}
function err(e){$('decode').textContent='ERROR: '+e.message;localStorage.setItem(DECODE_STORE,$('decode').textContent)}
function num(id,fallback){let raw=String($(id).value??'').trim();if(!raw)return fallback;let v=Number(raw);return Number.isFinite(v)?v:fallback}

function saveUiSettings(showMessage=false){
 let x={};for(let id of FIELD_IDS){let el=$(id);x[id]=el.type==='checkbox'?el.checked:el.value}
 localStorage.setItem(STORE,JSON.stringify(x));
 if(showMessage){$('saved').textContent='Saved';setTimeout(()=>{$('saved').textContent=''},1400)}
 pushRuntimeConfig().catch(()=>{});
}
function loadUiSettings(){
 let x={};try{x=JSON.parse(localStorage.getItem(STORE)||'{}')||{}}catch(e){}
 for(let id of FIELD_IDS){let el=$(id),v=(id in x)?x[id]:DEFAULTS[id];if(v===undefined)continue;if(el.type==='checkbox')el.checked=!!v;else el.value=v}
 let d=localStorage.getItem(DECODE_STORE);if(d)$('decode').textContent=d;
 let ri=localStorage.getItem(RECINFO_STORE);if(ri)$('recinfo').textContent=ri;
 try{let p=JSON.parse(localStorage.getItem(SWEEP_STORE)||'null');if(p&&p.length)drawSpectrum(p)}catch(e){}
 try{let h=JSON.parse(localStorage.getItem(PULSE_STORE)||'null');if(h&&h.counts)drawPulseHistogram(h)}catch(e){}
}
function resetUiDefaults(){for(let id of FIELD_IDS){let el=$(id),v=DEFAULTS[id];if(v===undefined)continue;if(el.type==='checkbox')el.checked=!!v;else el.value=v}saveUiSettings(true)}
async function pushRuntimeConfig(){
 let q=new URLSearchParams({dataRate:num('dataRate',38.4),txPower:Math.round(num('txPower',10)),minPulse:Math.round(num('minPulse',40)),megaPulse:Math.round(num('megaPulse',1000)),megaPulseTol:Math.round(num('megaPulseTol',300)),megaSymbol:Math.round(num('megaSymbol',6000)),megaSymbolTol:Math.round(num('megaSymbolTol',1200)),megaFrameGap:Math.round(num('megaFrameGap',10000)),megaHeaderLow:Math.round(num('megaHeaderLow',13000)),megaHeaderTol:Math.round(num('megaHeaderTol',3400))});
 return j('/api/config?'+q.toString());
}

async function status(){
 try{
  let x=await j('/api/status');
  $('radio').textContent=x.radio?'CC1101 OK':'CC1101 ?';
  $('mode').textContent=x.mode;
  $('wifi').textContent=x.wifiConnected?(x.wifiSsid+' · '+x.wifiIp+' · '+x.wifiRssi+' dBm'):'Wi-Fi reconnecting…';
  $('wifi').title=x.hostname||'';
  $('ap').textContent=x.apActive?('AP '+x.apSsid+' · '+x.apIp+' · '+x.apClients+' client'+(x.apClients==1?'':'s')):'AP down';
  $('heap').textContent='heap '+x.heap+' B';
  if(x.fsTotalBytes){let pct=(100*x.fsUsedBytes/x.fsTotalBytes).toFixed(0);
   $('fwinfo').textContent='fw '+x.firmwareVersion+' ('+x.firmwareBuild+') · CC1101 v'+x.radioVersion+' · FS '+x.fsUsedBytes+'/'+x.fsTotalBytes+' B ('+pct+'%)';}
  // Device status is display-only. Never overwrite editable frequency/sweep fields.
  $('targetText').textContent=(x.frequencyHz/1e6).toFixed(6);
  let sig=x.captureDone+':'+x.pulses+':'+x.captureDurationUs;
  if(x.captureDone&&sig!==lastCaptureSignature){lastCaptureSignature=sig;let ri=x.pulses+' pulses, '+(x.captureDurationUs/1000).toFixed(2)+' ms';$('recinfo').textContent=ri;localStorage.setItem(RECINFO_STORE,ri);pulseData()}
  if(x.sweepDone&&!lastSweepDone){lastSweepDone=true;spectrumData()}
  if(!x.sweepDone)lastSweepDone=false;
 }catch(e){$('mode').textContent='offline';$('wifi').textContent='Wi-Fi/Web offline'}
}

async function selftest(){
 $('selftest').textContent='running…';
 try{let x=await j('/api/selftest');
  let fails=Object.entries(x.checks).filter(([k,v])=>!v).map(([k])=>k);
  $('selftest').textContent=x.ok?'PASS':'FAIL: '+fails.join(', ');
  $('selftest').style.color=x.ok?'#6ac36a':'#ce6262';
 }catch(e){$('selftest').textContent='error: '+e.message;$('selftest').style.color='#ce6262'}
}

async function tune(){try{saveUiSettings();await pushRuntimeConfig();let hz=Math.round(num('target',318)*1e6),bw=$('bw').value;let x=await j('/api/tune?hz='+hz+'&bw='+bw);$('targetText').textContent=(x.frequencyHz/1e6).toFixed(6)}catch(e){err(e)}}
async function nudge(d){try{let hz=Math.round(num('target',318)*1e6)+d;$('target').value=(hz/1e6).toFixed(6);saveUiSettings();await tune()}catch(e){err(e)}}

async function sweep(){
 try{saveUiSettings();await pushRuntimeConfig();let a=Math.round(num('s0',317.7)*1e6),b=Math.round(num('s1',318.3)*1e6),st=Math.round(num('ss',20)*1e3),d=Math.round(num('sd',3)),bw=$('sbw').value;lastSweepDone=false;await j(`/api/sweep/start?start=${a}&stop=${b}&step=${st}&dwell=${d}&bw=${bw}`)}catch(e){err(e)}
}
async function focusSweep(){let t=Math.round(num('target',318)*1e6);$('s0').value=((t-300000)/1e6).toFixed(3);$('s1').value=((t+300000)/1e6).toFixed(3);$('ss').value=20;$('sd').value=3;$('sbw').value=650;saveUiSettings();await sweep()}
async function spectrumData(){try{let x=await j('/api/sweep/data');drawSpectrum(x.points);localStorage.setItem(SWEEP_STORE,JSON.stringify(x.points))}catch(e){err(e)}}

async function recordStart(){try{saveUiSettings();await pushRuntimeConfig();let hz=Math.round(num('target',318)*1e6),bw=$('bw').value;await j('/api/tune?hz='+hz+'&bw='+bw);lastCaptureSignature='';await j('/api/capture/start?ms='+Math.round(num('recms',2500)))}catch(e){err(e)}}
async function recordStop(){try{await j('/api/capture/stop');status()}catch(e){err(e)}}
async function pulseData(){try{let x=await j('/api/capture/histogram');drawPulseHistogram(x);localStorage.setItem(PULSE_STORE,JSON.stringify(x))}catch(e){err(e)}}
async function decodeCurrent(){try{saveUiSettings();await pushRuntimeConfig();let x=await j('/api/decode/current'),txt=JSON.stringify(x,null,2);$('decode').textContent=txt;localStorage.setItem(DECODE_STORE,txt)}catch(e){err(e)}}
async function saveCurrent(){try{saveUiSettings();await j('/api/sample/save?name='+encodeURIComponent($('sampleName').value));listSamples()}catch(e){err(e)}}

async function listSamples(){
 try{let x=await j('/api/samples'),h='';for(let s of x.samples){let n=encodeURIComponent(s.name);h+=`<tr><td>${esc(s.name)}</td><td>${(s.frequencyHz/1e6).toFixed(6)}</td><td>${s.pulseCount}</td><td>${(s.durationUs/1000).toFixed(1)} ms</td><td><button class="tiny" onclick="sampleLoad('${n}')">Load</button> <button class="tiny" onclick="sampleDecode('${n}')">Decode</button> <button class="tiny" onclick="samplePlay('${n}')">Play</button> <button class="tiny danger" onclick="sampleDelete('${n}')">Delete</button></td></tr>`}$('samples').innerHTML=h||'<tr><td colspan="5" class="muted">No saved samples.</td></tr>'}catch(e){err(e)}
}
async function sampleLoad(n){try{let x=await j('/api/sample/load?name='+n);$('target').value=(x.frequencyHz/1e6).toFixed(6);saveUiSettings();lastCaptureSignature='';await status();await pulseData()}catch(e){err(e)}}
async function sampleDecode(n){try{await pushRuntimeConfig();let x=await j('/api/sample/decode?name='+n),txt=JSON.stringify(x,null,2);$('decode').textContent=txt;localStorage.setItem(DECODE_STORE,txt);lastCaptureSignature='';await status();await pulseData()}catch(e){err(e)}}
async function samplePlay(n){try{saveUiSettings();let r=Math.max(1,Math.min(10,parseInt($('repeats').value)||1)),inv=$('invert').checked?1:0;await j('/api/sample/play?name='+n+'&repeat='+r+'&invert='+inv)}catch(e){err(e)}}
async function sampleDelete(n){try{await j('/api/sample/delete?name='+n);listSamples()}catch(e){err(e)}}

function baseCanvas(id){let c=$(id),g=c.getContext('2d'),w=c.width,h=c.height;g.clearRect(0,0,w,h);g.fillStyle='#111418';g.fillRect(0,0,w,h);g.font='11px sans-serif';return{c,g,w,h}}

function drawSpectrum(points){
 let {g,w,h}=baseCanvas('spectrum');if(!points.length)return;
 let min=-120,max=-20,pad=44,plotH=h-38,plotW=w-pad-8;
 g.strokeStyle='#363b42';g.fillStyle='#aab0b7';
 for(let k=0;k<=5;k++){let y=8+plotH*k/5;g.beginPath();g.moveTo(pad,y);g.lineTo(w-5,y);g.stroke();g.fillText((max-(max-min)*k/5).toFixed(0),4,y+4)}
 let bw=plotW/points.length;
 for(let i=0;i<points.length;i++){let v=Math.max(min,Math.min(max,points[i].rssi)),bh=(v-min)/(max-min)*plotH;g.fillStyle='#6aa8ff';g.fillRect(pad+i*bw,h-22-bh,Math.max(1,bw),bh)}
 let every=Math.max(1,Math.ceil(points.length/8));g.fillStyle='#aab0b7';for(let i=0;i<points.length;i+=every)g.fillText((points[i].hz/1e6).toFixed(3),pad+i*bw,h-5);
 let peaks=[...points].sort((a,b)=>b.rssi-a.rssi).slice(0,8);$('peaks').textContent='Strongest: '+peaks.map(p=>(p.hz/1e6).toFixed(6)+' MHz '+p.rssi+' dBm').join('   |   ')
}

function drawPulseHistogram(x){
 let {g,w,h}=baseCanvas('pulses');if(!x.counts.length)return;
 let max=Math.max(...x.counts,1),pad=42,plotH=h-40,plotW=w-pad-8,bw=plotW/x.counts.length;
 g.strokeStyle='#363b42';g.fillStyle='#aab0b7';for(let k=0;k<=4;k++){let y=8+plotH*k/4;g.beginPath();g.moveTo(pad,y);g.lineTo(w-5,y);g.stroke();g.fillText(Math.round(max*(1-k/4)),4,y+4)}
 for(let i=0;i<x.counts.length;i++){let total=x.counts[i],hc=x.high[i],lc=x.low[i],bh=total/max*plotH,lh=lc/max*plotH,hh=hc/max*plotH;g.fillStyle='#5c8fd8';g.fillRect(pad+i*bw,h-22-lh,Math.max(1,bw-1),lh);g.fillStyle='#9bc4ff';g.fillRect(pad+i*bw,h-22-lh-hh,Math.max(1,bw-1),hh)}
 let every=Math.max(1,Math.ceil(x.labels.length/8));g.fillStyle='#aab0b7';for(let i=0;i<x.labels.length;i+=every)g.fillText(x.labels[i]+'us',pad+i*bw,h-5)
}


for(let id of FIELD_IDS){$(id).addEventListener('change',()=>saveUiSettings(false))}
loadUiSettings();
pushRuntimeConfig().catch(()=>{});
tune();
status();listSamples();
statusTimer=setInterval(status,900);
</script>
<p class="muted" style="margin-top:24px;border-top:1px solid #30343a;padding-top:10px">
Authorized use only — receive and transmit only on frequencies and equipment you
are permitted to use. RF transmission is regulated and is your responsibility.
</p>
</body>
</html>
)HTML";

// -----------------------------------------------------------------------------
// Gate control operator page (/gate)
// -----------------------------------------------------------------------------
static const char GATE_PAGE[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Gate control</title>
<style>
:root{font-family:system-ui,-apple-system,Segoe UI,sans-serif;color:#eee;background:#101214}
body{max-width:560px;margin:auto;padding:18px}
h1{margin:0 0 2px}.sub{color:#9aa0a6;margin:0 0 16px;font-size:13px}
button.gate{width:100%;font-size:24px;font-weight:700;padding:34px 12px;margin:10px 0;border-radius:16px;background:#1c2733;color:#eee;border:2px solid #3f6b9e;cursor:pointer}
button.gate:active{background:#24445f}
button.gate:disabled{opacity:.45;cursor:not-allowed}
.name{display:block;font-size:13px;font-weight:400;color:#9fb6cf;margin-top:6px}
#toast{min-height:22px;margin-top:8px;font-size:14px}
.warn{color:#e0a94a;font-size:12px;margin-top:18px}
a{color:#6aa8ff;font-size:12px}
</style></head><body>
<h1>Gate control</h1>
<p class="sub">Authorized use only. Operate equipment you own or are permitted to test.</p>
<button class="gate" id="b-inner" onclick="fire('inner')" disabled>Inner gate<span class="name" id="n-inner">loading…</span></button>
<button class="gate" id="b-outer" onclick="fire('outer')" disabled>Outer gate<span class="name" id="n-outer">loading…</span></button>
<div id="toast"></div>
<p class="warn">Each press transmits at maximum power. <a href="/gate/config">Assign samples</a> &nbsp;·&nbsp; <a href="/">Dashboard</a></p>
<script>
const $=i=>document.getElementById(i);
let busy=false;
async function load(){
 try{let r=await fetch('/api/gate',{cache:'no-store'});let g=await r.json();
  for(let w of ['inner','outer']){let a=g[w];
   $('n-'+w).textContent=a.assigned?(a.sampleName+(a.ready?'':' — unavailable')):'unassigned';
   $('b-'+w).disabled=busy||!a.ready;}
 }catch(e){$('toast').textContent='status error';}
}
async function fire(w){
 if(busy)return;busy=true;$('b-inner').disabled=true;$('b-outer').disabled=true;
 $('toast').textContent='Sending '+w+' gate…';
 try{let r=await fetch('/api/gate/fire?which='+w,{method:'POST'});let o=await r.json();
  $('toast').textContent=o.ok?('Sent '+w+' gate ('+o.sampleName+')'):('Error: '+(o.error||r.status));}
 catch(e){$('toast').textContent='Error: '+e.message;}
 busy=false;load();
}
load();setInterval(load,4000);
</script></body></html>
)HTML";

// -----------------------------------------------------------------------------
// Gate control assignment page (/gate/config)
// -----------------------------------------------------------------------------
static const char GATE_CONFIG_PAGE[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Gate assignment</title>
<style>
:root{font-family:system-ui,-apple-system,Segoe UI,sans-serif;color:#eee;background:#101214}
body{max-width:720px;margin:auto;padding:16px}
h1{margin:0 0 8px}h2{margin:0 0 6px;font-size:16px}
.card{background:#191c20;border:1px solid #30343a;border-radius:12px;padding:14px;margin:12px 0}
label{font-size:12px;color:#aeb4bc;display:flex;flex-direction:column;gap:4px;margin:6px 8px 6px 0}
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:end}
input,select,button{background:#22262b;color:#eee;border:1px solid #454b53;border-radius:8px;padding:8px}
button{cursor:pointer}.primary{border-color:#6aa8ff}
.msg{font-size:12px;color:#9fb6cf;margin-left:6px}
a{color:#6aa8ff;font-size:12px}.muted{color:#8b9199;font-size:12px}
</style></head><body>
<h1>Gate sample assignment</h1>
<p class="muted">Pick which saved recording each gate button replays. On a fire the
device re-applies the frequency/bandwidth set here and transmits at the CC1101's
maximum power (+12 dBm) regardless of any stored power value.</p>
<div id="cards"></div>
<p><a href="/gate">Gate control</a> &nbsp;·&nbsp; <a href="/">Dashboard</a></p>
<template id="tpl">
<div class="card">
<h2 class="t"></h2>
<div class="row">
<label>Sample<select class="f-sample"></select></label>
<label>Frequency MHz<input class="f-freq" type="number" step="0.001" min="300" max="348"></label>
<label>Bandwidth<select class="f-bw"><option value="58">58 kHz</option><option value="270">270 kHz</option><option value="650">650 kHz</option></select></label>
<label>Repeats<input class="f-rep" type="number" min="1" max="10"></label>
<label style="flex-direction:row;align-items:center;gap:6px"><input class="f-inv" type="checkbox">Invert</label>
</div>
<div class="row">
<button class="primary f-save">Save</button>
<button class="f-test">Save &amp; test fire</button>
<span class="msg f-msg"></span>
</div>
</div>
</template>
<script>
const WHICH=['inner','outer'];
async function j(u,o){let r=await fetch(u,o||{cache:'no-store'});let t=await r.json();if(!r.ok||t.ok===false)throw Error(t.error||r.status);return t;}
async function load(){
 let s=await j('/api/samples');
 let g=await j('/api/gate');
 let root=document.getElementById('cards');root.innerHTML='';
 for(let w of WHICH){
  let n=document.getElementById('tpl').content.cloneNode(true);
  n.querySelector('.t').textContent=w[0].toUpperCase()+w.slice(1)+' gate';
  let card=n.querySelector('.card');
  let sel=card.querySelector('.f-sample');
  sel.innerHTML='<option value="">- unassigned -</option>'+s.samples.map(x=>`<option>${x.name}</option>`).join('');
  let a=g[w];
  sel.value=a.assigned?a.sampleName:'';
  card.querySelector('.f-freq').value=((a.frequencyHz||318000000)/1e6).toFixed(3);
  card.querySelector('.f-bw').value=a.bandwidthKhz||650;
  card.querySelector('.f-rep').value=a.repeats||4;
  card.querySelector('.f-inv').checked=!!a.invert;
  let body=()=>{let p=new URLSearchParams();p.set('which',w);
   p.set('sampleName',card.querySelector('.f-sample').value);
   p.set('frequencyHz',Math.round(parseFloat(card.querySelector('.f-freq').value||'0')*1e6));
   p.set('bandwidthKhz',card.querySelector('.f-bw').value);
   p.set('repeats',card.querySelector('.f-rep').value);
   p.set('invert',card.querySelector('.f-inv').checked?1:0);return p;};
  let msg=card.querySelector('.f-msg');
  let save=async()=>{msg.textContent='saving…';
   try{await j('/api/gate/assign',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body().toString()});msg.textContent='saved';return true;}
   catch(e){msg.textContent='error: '+e.message;return false;}};
  card.querySelector('.f-save').onclick=save;
  card.querySelector('.f-test').onclick=async()=>{if(await save()){msg.textContent='firing…';
   try{let o=await j('/api/gate/fire?which='+w,{method:'POST'});msg.textContent='fired: '+o.sampleName;}
   catch(e){msg.textContent='fire error: '+e.message;}}};
  root.appendChild(n);
 }
}
load().catch(e=>document.getElementById('cards').textContent='Load error: '+e.message);
</script></body></html>
)HTML";

// -----------------------------------------------------------------------------
// Wi-Fi station/client service
// -----------------------------------------------------------------------------
static void startMdnsIfNeeded() {
  if(WiFi.status() != WL_CONNECTED || mdnsStarted) return;
  mdnsStarted = MDNS.begin(WIFI_HOSTNAME);
  if(mdnsStarted) MDNS.addService("http", "tcp", 80);
}

static void startWifiAp() {
  WiFi.softAPConfig(WIFI_AP_IP, WIFI_AP_GATEWAY, WIFI_AP_NETMASK);

  const bool open = WIFI_AP_PASS[0] == '\0';
  apActive = WiFi.softAP(WIFI_AP_SSID,
                         open ? nullptr : WIFI_AP_PASS,
                         WIFI_AP_CHANNEL,
                         false,                 // not hidden
                         WIFI_AP_MAX_CLIENTS);
}

static void startWifi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.hostname(WIFI_HOSTNAME);
  WiFi.setAutoReconnect(true);

  // Bring the SoftAP up first so the dashboard is reachable even if the home
  // network is out of range or misconfigured.
  startWifiAp();

  WiFi.begin(WIFI_SSID, WIFI_PASS);

  const uint32_t started = millis();
  while(WiFi.status() != WL_CONNECTED &&
        (uint32_t)(millis() - started) < WIFI_CONNECT_TIMEOUT_MS) {
    delay(50);
    yield();
  }

  startMdnsIfNeeded();
}

static void serviceWifi() {
  // Keep the SoftAP alive regardless of station state.
  if(!apActive || (WiFi.getMode() & WIFI_AP) == 0) {
    startWifiAp();
  }

  if(WiFi.status() == WL_CONNECTED) {
    startMdnsIfNeeded();
    if(mdnsStarted) MDNS.update();
    return;
  }

  const uint32_t now = millis();
  if((uint32_t)(now - lastWifiRetryMs) >= WIFI_RETRY_INTERVAL_MS) {
    lastWifiRetryMs = now;
    WiFi.reconnect();
  }
}

// -----------------------------------------------------------------------------
// HTTP routes
// -----------------------------------------------------------------------------
// Reject cross-site POSTs (e.g. a page on another origin trying to fire a gate).
// A same-origin fetch either omits Origin or sends one whose host matches Host;
// native curl/tools omit it too and are allowed.
static bool sameOriginOk() {
  if(!server.hasHeader("Origin")) return true;
  String origin = server.header("Origin");
  String host = server.hostHeader();
  if(!host.length()) return false;
  int ss = origin.indexOf("//");
  String originHost = (ss >= 0) ? origin.substring(ss + 2) : origin;
  return originHost.equalsIgnoreCase(host);
}

static const char* HTTP_COLLECT_HEADERS[] = {"Origin"};

static void setupRoutes() {
  server.collectHeaders(HTTP_COLLECT_HEADERS,
                        sizeof(HTTP_COLLECT_HEADERS) / sizeof(HTTP_COLLECT_HEADERS[0]));

  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", WEB_UI);
  });

  server.on("/gate", HTTP_GET, []() {
    server.send_P(200, "text/html", GATE_PAGE);
  });

  server.on("/gate/config", HTTP_GET, []() {
    server.send_P(200, "text/html", GATE_CONFIG_PAGE);
  });

  server.on("/api/status", HTTP_GET, []() {
    String s = "{\"ok\":true";
    s += ",\"radio\":" + jsonBool(radioOk);
    s += ",\"firmwareVersion\":\"" + String(FIRMWARE_VERSION) + "\"";
    s += ",\"firmwareBuild\":\"" + jsonEscape(String(FIRMWARE_BUILD)) + "\"";
    s += ",\"radioPartnum\":" + String(radioPartnum);
    s += ",\"radioVersion\":" + String(radioVersionReg);
    s += ",\"mode\":\"" + currentMode + "\"";
    s += ",\"wifiConnected\":" + jsonBool(WiFi.status() == WL_CONNECTED);
    s += ",\"wifiSsid\":\"" + jsonEscape(WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String(WIFI_SSID)) + "\"";
    s += ",\"wifiIp\":\"" + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("0.0.0.0")) + "\"";
    s += ",\"wifiRssi\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
    s += ",\"hostname\":\"" + String(WIFI_HOSTNAME) + ".local\"";
    s += ",\"apActive\":" + jsonBool(apActive);
    s += ",\"apSsid\":\"" + jsonEscape(String(WIFI_AP_SSID)) + "\"";
    s += ",\"apIp\":\"" + WiFi.softAPIP().toString() + "\"";
    s += ",\"apClients\":" + String(WiFi.softAPgetStationNum());
    s += ",\"frequencyHz\":" + String(targetFrequencyHz);
    s += ",\"bandwidthKhz\":" + String(targetBandwidthKhz);
    s += ",\"gdo0Pin\":" + String(PIN_CC1101_GDO0);
    s += ",\"gdo2Pin\":" + String(PIN_CC1101_GDO2);
    s += ",\"heap\":" + String(ESP.getFreeHeap());
    FSInfo fsi{};
    LittleFS.info(fsi);
    s += ",\"fsUsedBytes\":" + String(fsi.usedBytes);
    s += ",\"fsTotalBytes\":" + String(fsi.totalBytes);
    s += ",\"pulses\":" + String(pulseCount);
    s += ",\"captureDone\":" + jsonBool(captureDone);
    s += ",\"captureDurationUs\":" + String(captureDurationUs);
    s += ",\"sweepDone\":" + jsonBool(sweepDone);
    s += ",\"sweepCount\":" + String(sweepCount);
    s += ",\"busy\":" + jsonBool(isBusy());
    s += "}";
    sendJson(s);
  });

  server.on("/api/selftest", HTTP_GET, []() {
    const bool spiOk = radio.getCC1101();
    const uint8_t pn = radio.SpiReadStatus(CC1101_PARTNUM);
    const uint8_t ver = radio.SpiReadStatus(CC1101_VERSION);
    FSInfo fsi{};
    const bool fsOk = LittleFS.info(fsi);
    const uint32_t heap = ESP.getFreeHeap();

    const bool versionSane = (ver != 0x00 && ver != 0xFF);
    const bool heapOk = heap > 8000UL;
    const bool wifiOk = (WiFi.status() == WL_CONNECTED) || apActive;
    const bool allOk = spiOk && versionSane && fsOk && heapOk && wifiOk;

    String s = "{\"ok\":" + jsonBool(allOk);
    s += ",\"firmwareVersion\":\"" + String(FIRMWARE_VERSION) + "\"";
    s += ",\"firmwareBuild\":\"" + jsonEscape(String(FIRMWARE_BUILD)) + "\"";
    s += ",\"checks\":{";
    s += "\"spi\":" + jsonBool(spiOk);
    s += ",\"radioVersionSane\":" + jsonBool(versionSane);
    s += ",\"littlefs\":" + jsonBool(fsOk);
    s += ",\"heap\":" + jsonBool(heapOk);
    s += ",\"network\":" + jsonBool(wifiOk);
    s += "}";
    s += ",\"radioPartnum\":" + String(pn);
    s += ",\"radioVersion\":" + String(ver);
    s += ",\"heap\":" + String(heap);
    s += ",\"fsUsedBytes\":" + String(fsi.usedBytes);
    s += ",\"fsTotalBytes\":" + String(fsi.totalBytes);
    s += ",\"mode\":\"" + currentMode + "\"";
    s += ",\"busy\":" + jsonBool(isBusy());
    s += "}";
    sendJson(s);
  });

  server.on("/api/config", HTTP_GET, []() {
    if(server.hasArg("dataRate")) {
      float v = server.arg("dataRate").toFloat();
      if(v < 0.02f) v = 0.02f;
      if(v > 1621.83f) v = 1621.83f;
      radioDataRateKBaud = v;
    }
    if(server.hasArg("txPower")) {
      int v = server.arg("txPower").toInt();
      switch(v) {
        case -30: case -20: case -15: case -10: case -6: case 0:
        case 5: case 7: case 10: case 11: case 12: txPowerDbm = v; break;
        default: txPowerDbm = 10; break;
      }
    }
    if(server.hasArg("minPulse")) minPulseUs = constrain((uint32_t)server.arg("minPulse").toInt(), 5UL, 2000UL);
    if(server.hasArg("megaPulse")) megaPulseUs = constrain((uint32_t)server.arg("megaPulse").toInt(), 100UL, 3000UL);
    if(server.hasArg("megaPulseTol")) megaPulseTolUs = constrain((uint32_t)server.arg("megaPulseTol").toInt(), 25UL, 1500UL);
    if(server.hasArg("megaSymbol")) megaSymbolUs = constrain((uint32_t)server.arg("megaSymbol").toInt(), 1000UL, 20000UL);
    if(server.hasArg("megaSymbolTol")) megaSymbolTolUs = constrain((uint32_t)server.arg("megaSymbolTol").toInt(), 100UL, 5000UL);
    if(server.hasArg("megaFrameGap")) megaFrameGapUs = constrain((uint32_t)server.arg("megaFrameGap").toInt(), 2000UL, 50000UL);
    if(server.hasArg("megaHeaderLow")) megaHeaderLowUs = constrain((uint32_t)server.arg("megaHeaderLow").toInt(), 2000UL, 50000UL);
    if(server.hasArg("megaHeaderTol")) megaHeaderTolUs = constrain((uint32_t)server.arg("megaHeaderTol").toInt(), 100UL, 10000UL);
    if(megaSymbolUs <= megaPulseUs + 500UL) megaSymbolUs = min(20000UL, megaPulseUs + 500UL);
    String s = "{\"ok\":true";
    s += ",\"dataRate\":" + String(radioDataRateKBaud, 2);
    s += ",\"txPower\":" + String(txPowerDbm);
    s += ",\"minPulse\":" + String(minPulseUs);
    s += ",\"megaPulse\":" + String(megaPulseUs);
    s += ",\"megaPulseTol\":" + String(megaPulseTolUs);
    s += ",\"megaSymbol\":" + String(megaSymbolUs);
    s += ",\"megaSymbolTol\":" + String(megaSymbolTolUs);
    s += ",\"megaFrameGap\":" + String(megaFrameGapUs);
    s += ",\"megaHeaderLow\":" + String(megaHeaderLowUs);
    s += ",\"megaHeaderTol\":" + String(megaHeaderTolUs);
    s += "}";
    sendJson(s);
  });

  server.on("/api/tune", HTTP_GET, []() {
    if(isBusy()) return sendError("busy", 409);
    uint32_t hz = (uint32_t)server.arg("hz").toInt();
    uint16_t bw = (uint16_t)server.arg("bw").toInt();
    if(!valid315BandFrequency(hz)) return sendError("frequency must be 300-348 MHz");
    if(bw != 58 && bw != 270 && bw != 650) bw = DEFAULT_CAPTURE_BW_KHZ;

    targetFrequencyHz = hz;
    targetBandwidthKhz = bw;
    restoreTargetRx();
    sendJson("{\"ok\":true,\"frequencyHz\":" + String(targetFrequencyHz) + "}");
  });

  server.on("/api/nudge", HTTP_GET, []() {
    if(isBusy()) return sendError("busy", 409);
    int32_t delta = server.arg("delta").toInt();
    int64_t candidate = (int64_t)targetFrequencyHz + delta;
    if(candidate < 0 || !valid315BandFrequency((uint32_t)candidate)) return sendError("result outside 300-348 MHz");
    targetFrequencyHz = (uint32_t)candidate;
    restoreTargetRx();
    sendJson("{\"ok\":true,\"frequencyHz\":" + String(targetFrequencyHz) + "}");
  });

  server.on("/api/sweep/start", HTTP_GET, []() {
    if(isBusy()) return sendError("busy", 409);

    uint32_t a = (uint32_t)server.arg("start").toInt();
    uint32_t b = (uint32_t)server.arg("stop").toInt();
    uint32_t step = (uint32_t)server.arg("step").toInt();
    int dwell = server.arg("dwell").toInt();
    int bw = server.arg("bw").toInt();

    if(!valid315BandFrequency(a) || !valid315BandFrequency(b) || b <= a || step < 5000) {
      return sendError("bad sweep parameters");
    }

    uint32_t points = ((b - a) / step) + 1;
    if(points > MAX_SWEEP_POINTS) return sendError("too many sweep points; increase step size");
    if(bw != 58 && bw != 270 && bw != 650) bw = 650;
    dwell = constrain(dwell, 2, 25);

    startSweep(a, b, step, (uint16_t)dwell, (uint16_t)bw);
    sendJson("{\"ok\":true}");
  });

  server.on("/api/sweep/data", HTTP_GET, []() {
    String s = "{\"ok\":true,\"count\":" + String(sweepCount) + ",\"points\":[";
    int32_t rssiMin = 127;
    int32_t rssiMax = -128;
    int32_t rssiSum = 0;
    uint32_t peakHz = 0;
    for(uint16_t i = 0; i < sweepCount; ++i) {
      if(i) s += ',';
      s += "{\"hz\":" + String(sweepHz[i]) + ",\"rssi\":" + String(sweepRssi[i]) + "}";
      const int32_t r = sweepRssi[i];
      if(r < rssiMin) rssiMin = r;
      if(r > rssiMax) { rssiMax = r; peakHz = sweepHz[i]; }
      rssiSum += r;
    }
    s += "],\"summary\":";
    if(sweepCount) {
      s += "{\"rssiMinDbm\":" + String(rssiMin);
      s += ",\"rssiMaxDbm\":" + String(rssiMax);
      s += ",\"rssiMeanDbm\":" + String((float)rssiSum / (float)sweepCount, 1);
      s += ",\"peakHz\":" + String(peakHz) + "}";
    } else {
      s += "null";
    }
    s += "}";
    sendJson(s);
  });

  server.on("/api/capture/start", HTTP_GET, []() {
    if(isBusy()) return sendError("busy", 409);
    uint32_t ms = (uint32_t)server.arg("ms").toInt();
    ms = constrain(ms, (uint32_t)50, MAX_CAPTURE_MS);
    startCapture(ms);
    sendJson("{\"ok\":true}");
  });

  server.on("/api/capture/stop", HTTP_GET, []() {
    if(currentMode != "recording") return sendError("not recording", 409);
    stopCapture();
    sendJson("{\"ok\":true,\"pulses\":" + String(pulseCount) + "}");
  });

  server.on("/api/capture/histogram", HTTP_GET, []() {
    if(captureActive) return sendError("capture in progress", 409);
    sendJson(pulseHistogramJson());
  });

  // Raw pulse list of the in-memory capture, for off-device analysis and for
  // cross-checking /api/decode/current. [ [durationUs, level], ... ].
  // Streamed in chunks so a full 3072-pulse buffer cannot exhaust the heap.
  server.on("/api/capture/pulses", HTTP_GET, []() {
    if(captureActive) return sendError("capture in progress", 409);
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "application/json", "");
    String s = "{\"ok\":true,\"frequencyHz\":" + String(targetFrequencyHz);
    s += ",\"durationUs\":" + String(captureDurationUs);
    s += ",\"count\":" + String(pulseCount) + ",\"pulses\":[";
    for(uint16_t i = 0; i < pulseCount; ++i) {
      if(i) s += ',';
      s += '[' + String(pulseDurations[i]) + ',' + String(pulseLevels[i]) + ']';
      if(s.length() > 1024) { server.sendContent(s); s = ""; }
    }
    s += "]}";
    server.sendContent(s);
    server.sendContent("");
  });

  server.on("/api/decode/current", HTTP_GET, []() {
    if(isBusy()) return sendError("busy", 409);
    sendJson(decodeCurrentJson());
  });

  server.on("/api/sample/save", HTTP_GET, []() {
    if(isBusy()) return sendError("busy", 409);
    String name = safeName(server.arg("name"));
    if(!saveCurrentSample(name)) return sendError("nothing to save or LittleFS error");
    sendJson("{\"ok\":true,\"name\":\"" + name + "\",\"frequencyHz\":" + String(targetFrequencyHz) + "}");
  });

  server.on("/api/sample/load", HTTP_GET, []() {
    if(isBusy()) return sendError("busy", 409);
    String name = safeName(server.arg("name"));
    if(!loadSample(name)) return sendError("sample not found");
    restoreTargetRx();
    sendJson("{\"ok\":true,\"name\":\"" + name + "\",\"frequencyHz\":" + String(targetFrequencyHz) + "}");
  });

  server.on("/api/sample/decode", HTTP_GET, []() {
    if(isBusy()) return sendError("busy", 409);
    String name = safeName(server.arg("name"));
    if(!loadSample(name)) return sendError("sample not found");
    sendJson(decodeCurrentJson());
  });

  server.on("/api/sample/play", HTTP_GET, []() {
    if(isBusy() || playbackRequested) return sendError("busy", 409);
    String name = safeName(server.arg("name"));
    if(!loadSample(name)) return sendError("sample not found");

    int repeat = server.arg("repeat").toInt();
    playbackRepeats = (uint8_t)constrain(repeat, 1, 10);
    playbackInvertRequested = server.arg("invert").toInt() != 0;
    playbackRequested = true;
    sendJson("{\"ok\":true}");
  });

  server.on("/api/sample/delete", HTTP_GET, []() {
    if(isBusy()) return sendError("busy", 409);
    String name = safeName(server.arg("name"));
    bool ok = LittleFS.remove(samplePath(name));
    sendJson(String("{\"ok\":") + jsonBool(ok) + "}");
  });

  server.on("/api/samples", HTTP_GET, []() {
    String s = "{\"ok\":true,\"samples\":[";
    bool first = true;
    Dir dir = LittleFS.openDir("/");

    while(dir.next()) {
      String fn = dir.fileName();
      String base = fn;
      if(base.startsWith("/")) base = base.substring(1, base.length());
      if(!base.startsWith("rf315_") || !base.endsWith(".bin")) continue;

      File f = dir.openFile("r");
      SampleHeader h{};
      bool good = f &&
                  f.read(reinterpret_cast<uint8_t*>(&h), sizeof(h)) == sizeof(h) &&
                  memcmp(h.magic, "315R", 4) == 0 && h.version == 1;
      f.close();
      if(!good) continue;

      int start = String("rf315_").length();
      String name = base.substring(start, base.length() - 4);
      if(!first) s += ',';
      first = false;
      s += "{\"name\":\"" + name + "\"";
      s += ",\"frequencyHz\":" + String(h.frequencyHz);
      s += ",\"pulseCount\":" + String(h.pulseCount);
      s += ",\"durationUs\":" + String(h.durationUs);
      s += "}";
    }

    s += "]}";
    sendJson(s);
  });

  // ---- Gate control -------------------------------------------------------
  server.on("/api/gate", HTTP_GET, []() {
    String s = "{\"ok\":true";
    s += ",\"txPowerForcedDbm\":" + String(GATE_TX_POWER_DBM);
    s += ",\"minRepeats\":" + String(GATE_MIN_REPEATS);
    s += ",\"inner\":" + gateAssignmentJson(gateInner);
    s += ",\"outer\":" + gateAssignmentJson(gateOuter);
    s += "}";
    sendJson(s);
  });

  server.on("/api/gate/assign", HTTP_POST, []() {
    if(!sameOriginOk()) return sendError("cross-origin request rejected", 403);

    String which = server.arg("which");
    if(which != "inner" && which != "outer") {
      return sendError("which must be inner or outer");
    }

    GateAssignment a{};
    const bool clearing = server.arg("sampleName").length() == 0;

    if(!clearing) {
      String name = safeName(server.arg("sampleName"));
      if(!LittleFS.exists(samplePath(name))) return sendError("sample not found");

      uint32_t hz = (uint32_t)server.arg("frequencyHz").toInt();
      uint16_t bw = (uint16_t)server.arg("bandwidthKhz").toInt();
      if(!valid315BandFrequency(hz)) return sendError("frequency must be 300-348 MHz");
      if(bw != 58 && bw != 270 && bw != 650) bw = 650;

      strncpy(a.sampleName, name.c_str(), sizeof(a.sampleName) - 1);
      a.frequencyHz = hz;
      a.bandwidthKhz = bw;
      a.txPowerDbm = GATE_TX_POWER_DBM;
      a.repeats = (uint8_t)constrain(server.arg("repeats").toInt(), 1, 10);
      a.invert = server.arg("invert").toInt() != 0 ? 1 : 0;
    }

    if(which == "inner") gateInner = a;
    else gateOuter = a;

    if(!gateSaveConfig()) return sendError("could not persist gate config", 500);
    sendJson("{\"ok\":true}");
  });

  server.on("/api/gate/fire", HTTP_POST, []() {
    if(!sameOriginOk()) return sendError("cross-origin request rejected", 403);
    if(isBusy() || playbackRequested || gateFireRequested) return sendError("busy", 409);

    String which = server.arg("which");
    const GateAssignment* a = (which == "inner") ? &gateInner
                            : (which == "outer") ? &gateOuter
                            : nullptr;
    if(!a) return sendError("which must be inner or outer");
    if(a->sampleName[0] == '\0') return sendError("gate is unassigned");
    if(!gateAssignmentValid(*a)) return sendError("assignment is invalid");
    if(!LittleFS.exists(samplePath(String(a->sampleName)))) {
      return sendError("assigned sample is missing");
    }

    gateFirePlan = *a;
    gateFireRequested = true;
    sendJson(String("{\"ok\":true,\"which\":\"") + which +
             "\",\"sampleName\":\"" + jsonEscape(String(a->sampleName)) + "\"}");
  });

  server.onNotFound([]() {
    server.send(404, "text/plain", "not found");
  });
}

// -----------------------------------------------------------------------------
// Arduino setup / loop
// -----------------------------------------------------------------------------
void setup() {
  pinMode(PIN_CC1101_GDO0, INPUT);
  pinMode(PIN_CC1101_GDO2, INPUT);

  // D1 Mini hardware SPI: SCK=D5, MISO=D6, MOSI=D7, CSN=D8.
  // Separate raw paths: GDO0=D2 for async TX, GDO2=D1 for async RX.
  radio.setSpiPin(PIN_CC1101_SCK, PIN_CC1101_MISO, PIN_CC1101_MOSI, PIN_CC1101_CS);
  radio.setGDO(PIN_CC1101_GDO0, PIN_CC1101_GDO2);
  radio.Init();
  radioOk = radio.getCC1101();
  radioPartnum = radio.SpiReadStatus(CC1101_PARTNUM);
  radioVersionReg = radio.SpiReadStatus(CC1101_VERSION);

  configureRawOok(targetBandwidthKhz);
  setRadioFrequency(targetFrequencyHz);
  restoreTargetRx();

  if(!LittleFS.begin()) {
    LittleFS.format();
    LittleFS.begin();
  }

  gateLoadConfig();

  startWifi();

  setupRoutes();
  server.begin();
}

void loop() {
  server.handleClient();
  serviceWifi();

  if(captureActive) {
    if((int32_t)(millis() - captureDeadlineMs) >= 0 || pulseCount >= MAX_PULSES) {
      stopCapture();
    }
  }

  serviceSweep();

  if(playbackRequested && !isBusy()) {
    playbackRequested = false;
    replayCurrent(playbackRepeats, playbackInvertRequested);
  }

  if(gateFireRequested && !isBusy()) {
    gateFireRequested = false;
    fireGate(gateFirePlan);
  }

  yield();
}

