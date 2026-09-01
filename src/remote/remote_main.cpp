/*
  remote_main.cpp - Wi-Fi remote control + display for the CC1101 318 MHz tool.

  Target: a second WeMOS/LOLIN D1 Mini (NO CC1101).
    OLED   SSD1306 128x64 I2C   SDA -> D2 / GPIO4,  SCL -> D1 / GPIO5,  addr 0x3C
    BTN_A  inner gate           D5 / GPIO14  -> GND, INPUT_PULLUP (active low)
    BTN_B  outer gate           D6 / GPIO12  -> GND, INPUT_PULLUP (active low)
    LED    status               D4 / GPIO2 (onboard, active low)

  It joins the main unit's SoftAP (cc1101-setup-<chipId>) using the shared
  ap_pass from secrets.ini - so the two are paired by default - polls the main
  unit's HTTP API for status, and POSTs /api/gate/fire when a button is pressed.

  OLED:
    - no Wi-Fi link / no reply from the node -> big "OFFLINE"
    - linked -> node idle / SENDING / BUSY state, and for each button whether a
      sample is assigned and READY.

  Onboard LED (also the indicator when headless):
    - offline / out of range  -> slow dim pulse
    - online / in range       -> slow mid-brightness pulse
    - button A pressed         -> fast single-pulse loop  (~2.5 s)
    - button B pressed         -> fast double-pulse + long gap  (~2.5 s)

  A tiny HTTP server on the remote exposes /api/remote/status and
  POST /api/remote/press?which=inner|outer for the build healthcheck.

  Build:  pio run -e d1_mini_remote [-t upload]
          -e d1_mini_remote_headless    display-less (serial + LED only)
          -e d1_mini_remote_selftest    headless + verbose serial + a loop that
                                        drives the node and checks state changes

  This file is the whole remote firmware; src/main.cpp / src/decode.* (the main
  unit) are excluded from this env via build_src_filter.
*/
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>

#ifndef REMOTE_HEADLESS
#include <SSD1306Wire.h>
#endif

// -----------------------------------------------------------------------------
// Compile-time config (from platformio.ini / secrets.ini)
// -----------------------------------------------------------------------------
#ifndef CFG_FW_VERSION
#define CFG_FW_VERSION "0.1.0-nogit"
#endif
#ifndef CFG_WIFI_AP_SSID
#define CFG_WIFI_AP_SSID "cc1101-setup"
#endif
#ifndef CFG_WIFI_AP_PASS
#define CFG_WIFI_AP_PASS "CHANGE_ME_AP_PASSWORD"
#endif
#ifndef CFG_REMOTE_TARGET_SSID
#define CFG_REMOTE_TARGET_SSID ""          // exact SSID to pin; "" = scan by prefix
#endif
#ifndef CFG_REMOTE_MAIN_IP
#define CFG_REMOTE_MAIN_IP "192.168.4.1"   // main unit's SoftAP address
#endif

static constexpr char FIRMWARE_VERSION[] = CFG_FW_VERSION;
static constexpr char AP_SSID_BASE[]     = CFG_WIFI_AP_SSID;
static constexpr char AP_PASS[]          = CFG_WIFI_AP_PASS;
static constexpr char TARGET_SSID_PIN[]  = CFG_REMOTE_TARGET_SSID;
static constexpr char MAIN_IP[]          = CFG_REMOTE_MAIN_IP;
static constexpr char HOSTNAME[]         = "cc1101-remote";

// -----------------------------------------------------------------------------
// Pins
// -----------------------------------------------------------------------------
static constexpr uint8_t PIN_LED   = LED_BUILTIN;  // D4 / GPIO2, active low
static constexpr uint8_t PIN_BTN_A = D5;           // GPIO14 - inner
static constexpr uint8_t PIN_BTN_B = D6;           // GPIO12 - outer

// -----------------------------------------------------------------------------
// Status LED (D4/GPIO2, active-low PWM). This is the primary indicator on a
// headless remote:
//   offline / out of range  -> slow, dim pulse
//   online / in range       -> slow, mid-brightness pulse
//   button A pressed         -> fast single-pulse loop  (~2.5 s)
//   button B pressed         -> fast double-pulse with a long gap  (~2.5 s)
// -----------------------------------------------------------------------------
enum LedMode { LED_OFFLINE, LED_ONLINE, LED_BTN_A, LED_BTN_B };
static LedMode  ledMode = LED_OFFLINE;
static uint32_t ledOverrideUntil = 0;

static void ledButton(char which) {
  ledMode = (which == 'A') ? LED_BTN_A : LED_BTN_B;
  ledOverrideUntil = millis() + 2500;
}

// b: 0 (off) .. 255 (full); LED is active-low, PWM range 0..1023
static inline void ledLevel(int b) {
  analogWrite(PIN_LED, (255 - constrain(b, 0, 255)) * 4);
}

static void updateLed(bool online) {
  const uint32_t t = millis();
  if((ledMode == LED_BTN_A || ledMode == LED_BTN_B) && t < ledOverrideUntil) {
    // fast pulse patterns
    if(ledMode == LED_BTN_A) {                       // single pulse, 200 ms
      uint32_t p = t % 200;
      ledLevel(p < 100 ? p * 255 / 100 : (200 - p) * 255 / 100);
    } else {                                         // double pulse then gap, 900 ms
      uint32_t p = t % 900;
      bool on = (p < 90) || (p >= 200 && p < 290);
      ledLevel(on ? 255 : 0);
    }
    return;
  }
  ledMode = online ? LED_ONLINE : LED_OFFLINE;
  uint32_t p = t % 3000;                             // slow triangle, 3 s period
  int tri = (p < 1500 ? p : 3000 - p);               // 0..1500
  if(ledMode == LED_ONLINE) ledLevel(50 + tri * 130 / 1500);   // mid: 50..180
  else                      ledLevel(3 + tri * 30 / 1500);     // dim: 3..33
}

// -----------------------------------------------------------------------------
// Timing
// -----------------------------------------------------------------------------
static constexpr uint32_t POLL_INTERVAL_MS   = 1500;
static constexpr uint32_t RECONNECT_EVERY_MS = 8000;
static constexpr uint32_t BTN_DEBOUNCE_MS    = 30;
static constexpr uint32_t TOAST_MS           = 2500;
static constexpr uint32_t HTTP_TIMEOUT_MS    = 4000;

// -----------------------------------------------------------------------------
// State
// -----------------------------------------------------------------------------
ESP8266WebServer server(80);

#ifndef REMOTE_HEADLESS
SSD1306Wire display(0x3c, D2, D1);   // SDA=D2/GPIO4, SCL=D1/GPIO5
#endif

String  joinedSsid;
bool    linkUp = false;
uint32_t lastPollMs = 0;
uint32_t lastReconnectMs = 0;
uint32_t bootMs = 0;

// Cached view of the main unit
struct MainView {
  bool    valid = false;
  uint32_t fetchedMs = 0;
  bool    ok = false;
  bool    radio = false;
  bool    busy = false;
  String  mode = "?";
  long    frequencyHz = 0;
  long    heap = 0;
  int     wifiRssi = 0;
  String  fwVersion = "?";
  // gate
  bool    gateEnabled = false;
  String  innerSample, outerSample;
  bool    innerReady = false, outerReady = false;
  String  lastFireError;
} mainView;

// Transient on-screen message
String   toast;
uint32_t toastUntilMs = 0;

static void setToast(const String& s) {
  toast = s;
  toastUntilMs = millis() + TOAST_MS;
}

// -----------------------------------------------------------------------------
// Tiny JSON value extraction (no allocations beyond the returned String)
// Handles `"key":"str"`, `"key":123`, `"key":true/false/null`.
// -----------------------------------------------------------------------------
static bool jsonRaw(const String& body, const char* key, String& out) {
  String needle = String("\"") + key + "\":";
  int k = body.indexOf(needle);
  if(k < 0) return false;
  int i = k + needle.length();
  while(i < (int)body.length() && (body[i] == ' ' || body[i] == '\t')) ++i;
  if(i >= (int)body.length()) return false;

  if(body[i] == '"') {
    int end = i + 1;
    while(end < (int)body.length() && body[end] != '"') {
      if(body[end] == '\\') ++end;
      ++end;
    }
    out = body.substring(i + 1, end);
    return true;
  }
  if(body[i] == '{' || body[i] == '[') {
    char open = body[i], close = (open == '{') ? '}' : ']';
    int depth = 0, end = i;
    for(; end < (int)body.length(); ++end) {
      if(body[end] == open) ++depth;
      else if(body[end] == close && --depth == 0) { ++end; break; }
    }
    out = body.substring(i, end);
    return true;
  }
  int end = i;
  while(end < (int)body.length() && body[end] != ',' && body[end] != '}' && body[end] != ']') ++end;
  out = body.substring(i, end);
  out.trim();
  return true;
}

static long jsonInt(const String& body, const char* key, long def = 0) {
  String v;
  return jsonRaw(body, key, v) ? v.toInt() : def;
}
static bool jsonBool(const String& body, const char* key, bool def = false) {
  String v;
  if(!jsonRaw(body, key, v)) return def;
  return v == "true" || v == "1";
}
static String jsonStr(const String& body, const char* key) {
  String v;
  jsonRaw(body, key, v);
  return v;
}

// -----------------------------------------------------------------------------
// HTTP to the main unit
// -----------------------------------------------------------------------------
static bool mainRequest(const char* method, const String& path, String& bodyOut, int* codeOut = nullptr) {
  if(codeOut) *codeOut = 0;
  if(WiFi.status() != WL_CONNECTED) return false;
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  String url = String("http://") + MAIN_IP + path;
  if(!http.begin(client, url)) return false;

  int code = (strcmp(method, "POST") == 0) ? http.POST("") : http.GET();
  if(codeOut) *codeOut = code;
  bool got = code > 0;
  if(got) bodyOut = http.getString();
  http.end();
#ifdef REMOTE_SELFTEST
  Serial.printf("  %s %s -> %d%s\n", method, path.c_str(), code,
                got ? (String("  ") + bodyOut).c_str() : "  (no response)");
#endif
  return got && code >= 200 && code < 300;
}

static void refreshMainView() {
  String body;
  MainView v;
  v.fetchedMs = millis();

  if(!mainRequest("GET", "/api/status", body)) {
    mainView.valid = mainView.valid;  // keep previous; mark stale via age
    mainView.fetchedMs = 0;
    return;
  }
  v.valid = true;
  v.ok = jsonBool(body, "ok");
  v.radio = jsonBool(body, "radio");
  v.busy = jsonBool(body, "busy");
  v.mode = jsonStr(body, "mode");
  v.frequencyHz = jsonInt(body, "frequencyHz");
  v.heap = jsonInt(body, "heap");
  v.wifiRssi = (int)jsonInt(body, "wifiRssi");
  v.fwVersion = jsonStr(body, "firmwareVersion");

  if(mainRequest("GET", "/api/gate", body)) {
    v.gateEnabled = jsonBool(body, "enabled");
    v.lastFireError = jsonStr(body, "lastFireError");
    String inner, outer;
    jsonRaw(body, "inner", inner);
    jsonRaw(body, "outer", outer);
    v.innerSample = jsonStr(inner, "sampleName");
    v.outerSample = jsonStr(outer, "sampleName");
    v.innerReady = jsonBool(inner, "ready");
    v.outerReady = jsonBool(outer, "ready");
  }

  mainView = v;
}

// which = "inner" | "outer"; returns the main unit's response text (or an error).
// *okOut = the fire was accepted; *codeOut = HTTP status (0 = no response).
static String fireGateOnMain(const char* which, bool* okOut, int* codeOut = nullptr) {
  String body;
  int code = 0;
  bool ok = mainRequest("POST", String("/api/gate/fire?which=") + which, body, &code);
  if(okOut) *okOut = ok && jsonBool(body, "ok");
  if(codeOut) *codeOut = code;
  if(code == 0 && body.length() == 0) body = "{\"ok\":false,\"error\":\"no response from main unit\"}";
  return body;
}

// -----------------------------------------------------------------------------
// Self-test (build with -DREMOTE_SELFTEST / env d1_mini_remote_selftest):
// pull every config/status endpoint over Wi-Fi and dump it to serial, then
// drive a command and both "button" commands and confirm the main unit's
// state actually changed. Runs on a timer so it can be watched on the monitor.
// -----------------------------------------------------------------------------
#ifdef REMOTE_SELFTEST
static void stSnapshot() {
  String b;
  Serial.println(F("\n========== MAIN UNIT SNAPSHOT =========="));
  for(const char* p : {"/api/status", "/api/gate", "/api/selftest", "/api/config", "/api/samples"}) {
    if(mainRequest("GET", p, b)) Serial.printf("%-16s %s\n", p, b.c_str());
    else                         Serial.printf("%-16s (no 2xx)\n", p);
    yield();
  }
}

// Waits (up to `ms`) for the main unit to report busy=false.
static void stWaitIdle(uint32_t ms) {
  uint32_t end = millis() + ms;
  String b;
  while(millis() < end) {
    if(mainRequest("GET", "/api/status", b) && !jsonBool(b, "busy")) return;
    delay(200);
    yield();
  }
}

// -DREMOTE_SELFTEST_FIRE additionally lets the self-test fire an assigned+ready
// gate (which actuates real hardware). Off by default.
static void stRun() {
  Serial.println(F("\n========== REMOTE SELF-TEST =========="));
  String b;
  int pass = 0, skip = 0, total = 0;

  // --- 1. remote command -> node state change: retune (no TX) --------------
  ++total;
  mainRequest("GET", "/api/status", b);
  long f0 = jsonInt(b, "frequencyHz", 318000000);
  long bw0 = jsonInt(b, "bandwidthKhz", 650);
  long f1 = (f0 + 40000 <= 348000000) ? f0 + 40000 : f0 - 40000;
  Serial.printf("[1] retune  %ld -> %ld Hz\n", f0, f1);
  bool tuned = mainRequest("GET", String("/api/tune?hz=") + f1 + "&bw=" + bw0, b);
  delay(150);
  mainRequest("GET", "/api/status", b);
  long fNow = jsonInt(b, "frequencyHz");
  bool p1 = tuned && (fNow == f1);
  Serial.printf("    node frequencyHz now %ld   %s\n", fNow, p1 ? "PASS" : "FAIL");
  mainRequest("GET", String("/api/tune?hz=") + f0 + "&bw=" + bw0, b);   // restore
  if(p1) ++pass;

  // --- 2. remote command -> node activity: a short capture (RX only) -------
  ++total;
  Serial.println(F("[2] GET /api/capture/start?ms=1200  (poll for 'recording')"));
  bool started = mainRequest("GET", "/api/capture/start?ms=1200", b);
  bool p2 = false;
  String mode;
  for(int i = 0; i < 10 && !p2; ++i) {
    if(mainRequest("GET", "/api/status", b)) {
      mode = jsonStr(b, "mode");
      if(started && (mode == "recording" || jsonBool(b, "busy"))) p2 = true;
    }
    if(!p2) delay(120);
  }
  Serial.printf("    node reached mode='%s'   %s\n", p2 ? "recording" : mode.c_str(), p2 ? "PASS" : "FAIL");
  stWaitIdle(4000);
  if(p2) ++pass;

  // --- 3+4. simulate each button; confirm the press reached the node ------
  for(const char* which : {"inner", "outer"}) {
    ++total;
    mainRequest("GET", "/api/gate", b);
    String whichObj;
    jsonRaw(b, which, whichObj);
    bool ready = jsonBool(whichObj, "ready");
    bool assigned = jsonStr(whichObj, "sampleName").length() > 0;
    String errBefore = jsonStr(b, "lastFireError");
    Serial.printf("[%d] simulate BTN %s  (assigned=%d ready=%d)\n", total, which, assigned, ready);

    int  fireCode = 0;
    bool fireOk = false;
#ifndef REMOTE_SELFTEST_FIRE
    if(ready) {
      Serial.println(F("    gate is live -> SKIP (build with -DREMOTE_SELFTEST_FIRE, or"));
      Serial.println(F("    press the button / POST /api/remote/press to test the live fire)"));
      ++skip;
      continue;
    }
#endif
    ledButton(strcmp(which, "inner") == 0 ? 'A' : 'B');
    String resp = fireGateOnMain(which, &fireOk, &fireCode);
    Serial.printf("    fire resp: %s\n", resp.c_str());
    delay(250);
    mainRequest("GET", "/api/gate", b);
    String errAfter = jsonStr(b, "lastFireError");
    mainRequest("GET", "/api/status", b);
    String modeAfter = jsonStr(b, "mode");
    Serial.printf("    lastFireError '%s' -> '%s'   node mode '%s'\n",
                  errBefore.c_str(), errAfter.c_str(), modeAfter.c_str());

    // PASS = the press reached the node (real HTTP reply) AND, if it fired,
    // the node reacted (mode/playback or a new lastFireError).
    bool reached = fireCode > 0;
    bool reacted = fireOk || modeAfter == "playback" || jsonBool(b, "busy") ||
                   (errAfter.length() && errAfter != errBefore) ||
                   (!ready && fireCode >= 400);   // unassigned -> expected reject
    bool p = reached && reacted;
    Serial.printf("    press->node: reached=%d reacted=%d  %s (HTTP %d)\n",
                  reached, reacted, p ? "PASS" : "FAIL", fireCode);
    if(p) ++pass;
    stWaitIdle(4000);
  }

  Serial.printf("========== SELF-TEST  %d PASS / %d SKIP / %d total  -> %s ==========\n\n",
                pass, skip, total, (pass + skip == total) ? "OK" : "FAIL");
}
#endif  // REMOTE_SELFTEST

// -----------------------------------------------------------------------------
// Wi-Fi pairing: join the main unit's SoftAP
// -----------------------------------------------------------------------------
static String pickTargetSsid() {
  if(TARGET_SSID_PIN[0] != '\0') return String(TARGET_SSID_PIN);

  int n = WiFi.scanNetworks();
  String best;
  int bestRssi = -999;
  String prefix = String(AP_SSID_BASE) + "-";
  for(int i = 0; i < n; ++i) {
    String s = WiFi.SSID(i);
    if((s.startsWith(prefix) || s == AP_SSID_BASE) && WiFi.RSSI(i) > bestRssi) {
      bestRssi = WiFi.RSSI(i);
      best = s;
    }
  }
  WiFi.scanDelete();
  return best;
}

static void connectToMain() {
  lastReconnectMs = millis();
  String ssid = pickTargetSsid();
  if(ssid.length() == 0) {
    linkUp = false;
    return;
  }
  joinedSsid = ssid;
  WiFi.begin(ssid, AP_PASS);

  uint32_t start = millis();
  while(WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
    delay(100);
    yield();
  }
  linkUp = (WiFi.status() == WL_CONNECTED);
  if(linkUp && MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
}

// -----------------------------------------------------------------------------
// Display
// -----------------------------------------------------------------------------
#ifndef REMOTE_HEADLESS
// One button row: "A inner  sample01  READY" / "B outer  (no sample)".
static void drawGateRow(int y, char label, const char* which, const String& sample, bool ready) {
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(0, y, String(label) + " " + which);
  display.drawString(46, y, sample.length() ? sample : String("(no sample)"));
  display.setTextAlignment(TEXT_ALIGN_RIGHT);
  display.drawString(128, y, sample.length() ? (ready ? "READY" : "wait") : "");
}

static void drawStatus() {
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);

  const bool online = linkUp && mainView.valid &&
                      mainView.fetchedMs && (millis() - mainView.fetchedMs < 6000);

  if(!online) {
    display.setFont(ArialMT_Plain_24);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 6, "OFFLINE");
    display.setFont(ArialMT_Plain_10);
    if(!linkUp) {
      display.drawString(64, 34, "out of range");
      display.drawString(64, 48, "scanning " + String(AP_SSID_BASE) + "-*");
    } else {
      display.drawString(64, 34, "no reply from node");
      display.drawString(64, 48, joinedSsid);
    }
    display.display();
    return;
  }

  // --- online ---------------------------------------------------------------
  // node state: idle / SENDING (playback) / BUSY (recording|sweeping)
  const char* state = "idle";
  if(mainView.mode == "playback")      state = "SENDING";
  else if(mainView.busy)               state = "BUSY";

  display.setFont(ArialMT_Plain_16);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(0, 0, "ONLINE");
  display.setTextAlignment(TEXT_ALIGN_RIGHT);
  display.drawString(128, 2, state);

  display.drawLine(0, 20, 128, 20);

  drawGateRow(24, 'A', "inner", mainView.innerSample, mainView.innerReady);
  drawGateRow(38, 'B', "outer", mainView.outerSample, mainView.outerReady);

  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  if(toast.length() && millis() < toastUntilMs) {
    display.drawString(0, 52, toast);
  } else {
    display.drawString(0, 52, "node " + (mainView.radio ? String("radio ok") : String("radio ?")));
    display.setTextAlignment(TEXT_ALIGN_RIGHT);
    display.drawString(128, 52, String(WiFi.RSSI()) + "dBm");
  }
  display.display();
}
#endif

static void showToast(const String& s) {
  setToast(s);
  Serial.println("[remote] " + s);
#ifndef REMOTE_HEADLESS
  drawStatus();
#endif
}

// -----------------------------------------------------------------------------
// Buttons
// -----------------------------------------------------------------------------
struct Button {
  uint8_t  pin;
  char     id;              // 'A' inner, 'B' outer
  const char* which;
  bool     stable = true;   // pull-up: released = HIGH = true
  bool     lastRead = true;
  uint32_t lastChangeMs = 0;
};
Button btnA{PIN_BTN_A, 'A', "inner"};
Button btnB{PIN_BTN_B, 'B', "outer"};

static void doFire(char id, const char* which) {
  ledButton(id);
  if(!linkUp) { showToast("no link to main"); return; }
  if(mainView.valid && mainView.busy) { showToast("main busy"); return; }

  showToast(String("firing ") + which + "...");
  bool ok = false;
  String resp = fireGateOnMain(which, &ok);

  if(ok) {
    showToast(String("sent ") + which + ": " + jsonStr(resp, "sampleName"));
  } else {
    String err = jsonStr(resp, "error");
    showToast(String("error: ") + (err.length() ? err : "fire failed"));
  }
  lastPollMs = 0;  // refresh view soon
}

static void serviceButton(Button& b) {
  bool r = digitalRead(b.pin);
  if(r != b.lastRead) {
    b.lastRead = r;
    b.lastChangeMs = millis();
  }
  if(millis() - b.lastChangeMs > BTN_DEBOUNCE_MS && r != b.stable) {
    b.stable = r;
    if(r == LOW) doFire(b.id, b.which);   // falling edge = press
  }
}

// -----------------------------------------------------------------------------
// Remote's own HTTP server (health / debug / test hook)
// -----------------------------------------------------------------------------
static void handleRemoteStatus() {
  const bool stale = (mainView.fetchedMs == 0) || (millis() - mainView.fetchedMs > 6000);
  String s = "{\"ok\":true,\"role\":\"remote\"";
  s += ",\"firmwareVersion\":\"" + String(FIRMWARE_VERSION) + "\"";
  s += ",\"uptimeMs\":" + String(millis() - bootMs);
  s += ",\"linkUp\":" + String(linkUp ? "true" : "false");
  s += ",\"joinedSsid\":\"" + joinedSsid + "\"";
  s += ",\"mainIp\":\"" + String(MAIN_IP) + "\"";
  s += ",\"apRssi\":" + String(linkUp ? WiFi.RSSI() : 0);
  s += ",\"mainReachable\":" + String((linkUp && mainView.valid && !stale) ? "true" : "false");
  s += ",\"lastPollAgeMs\":" + String(mainView.fetchedMs ? (millis() - mainView.fetchedMs) : -1);
  s += ",\"main\":{";
  s += "\"ok\":" + String(mainView.ok ? "true" : "false");
  s += ",\"radio\":" + String(mainView.radio ? "true" : "false");
  s += ",\"mode\":\"" + mainView.mode + "\"";
  s += ",\"busy\":" + String(mainView.busy ? "true" : "false");
  s += ",\"frequencyHz\":" + String(mainView.frequencyHz);
  s += ",\"heap\":" + String(mainView.heap);
  s += ",\"firmwareVersion\":\"" + mainView.fwVersion + "\"";
  s += ",\"gateEnabled\":" + String(mainView.gateEnabled ? "true" : "false");
  s += ",\"innerSample\":\"" + mainView.innerSample + "\"";
  s += ",\"outerSample\":\"" + mainView.outerSample + "\"";
  s += ",\"lastFireError\":\"" + mainView.lastFireError + "\"";
  s += "}}";
  server.send(200, "application/json", s);
}

static void handleRemotePress() {
  String which = server.arg("which");
  if(which != "inner" && which != "outer") {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"which must be inner or outer\"}");
    return;
  }
  ledButton(which == "inner" ? 'A' : 'B');
  if(!linkUp) {
    server.send(502, "application/json", "{\"ok\":false,\"error\":\"remote has no link to main\"}");
    return;
  }
  bool ok = false;
  String resp = fireGateOnMain(which.c_str(), &ok);
  setToast(String("press ") + which + (ok ? " ok" : " err"));
  lastPollMs = 0;
  String s = String("{\"ok\":") + (ok ? "true" : "false") + ",\"via\":\"remote\",\"which\":\"" + which + "\",\"main\":" + resp + "}";
  server.send(ok ? 200 : 502, "application/json", s);
}

static void handleRoot() {
  String h = "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>";
  h += "<title>cc1101 remote</title><body style='font-family:system-ui;max-width:32rem;margin:2rem auto'>";
  h += "<h2>cc1101 remote</h2><p>fw " + String(FIRMWARE_VERSION) + " &middot; link ";
  h += linkUp ? ("up (" + joinedSsid + ", " + String(WiFi.RSSI()) + " dBm)") : "down";
  h += "</p><p>main: " + mainView.mode + ", radio " + (mainView.radio ? "OK" : "?") + ", "
       + String(mainView.frequencyHz / 1e6, 3) + " MHz</p>";
  h += "<p><button onclick=\"fetch('/api/remote/press?which=inner',{method:'POST'})\">Inner</button> ";
  h += "<button onclick=\"fetch('/api/remote/press?which=outer',{method:'POST'})\">Outer</button></p>";
  h += "<p><a href='/api/remote/status'>/api/remote/status</a></p>";
  server.send(200, "text/html", h);
}

// -----------------------------------------------------------------------------
void setup() {
  bootMs = millis();
  Serial.begin(115200);
  Serial.println();
  Serial.printf("cc1101 remote %s  role=remote\n", FIRMWARE_VERSION);

  pinMode(PIN_LED, OUTPUT);
  analogWriteRange(1023);
  ledLevel(0);
  pinMode(PIN_BTN_A, INPUT_PULLUP);
  pinMode(PIN_BTN_B, INPUT_PULLUP);

#ifndef REMOTE_HEADLESS
  display.init();
  display.flipScreenVertically();
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0, "cc1101 remote");
  display.drawString(0, 14, FIRMWARE_VERSION);
  display.drawString(0, 34, "pairing...");
  display.display();
#endif

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.hostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  connectToMain();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/remote/status", HTTP_GET, handleRemoteStatus);
  server.on("/api/remote/press", HTTP_POST, handleRemotePress);
  server.begin();
}

void loop() {
  server.handleClient();
  if(linkUp) MDNS.update();

  serviceButton(btnA);
  serviceButton(btnB);

  const uint32_t now = millis();

  if(WiFi.status() != WL_CONNECTED) {
    linkUp = false;
    if(now - lastReconnectMs > RECONNECT_EVERY_MS) connectToMain();
  } else {
    linkUp = true;
  }

  if(linkUp && now - lastPollMs > POLL_INTERVAL_MS) {
    lastPollMs = now;
    refreshMainView();
  }

#ifdef REMOTE_SELFTEST
  static uint32_t lastSelftestMs = 0;
  static bool selftestPrimed = false;
  if(linkUp && mainView.valid && (!selftestPrimed || now - lastSelftestMs > 45000)) {
    selftestPrimed = true;
    lastSelftestMs = now;
    stSnapshot();
    stRun();
    lastPollMs = 0;   // refresh the cached view after
  }
#endif

#ifndef REMOTE_HEADLESS
  static uint32_t lastDraw = 0;
  if(now - lastDraw > 250) { lastDraw = now; drawStatus(); }
#endif

  static uint32_t lastLog = 0;
  if(now - lastLog > 5000) {
    lastLog = now;
    Serial.printf("[remote] link=%d ssid=%s main.valid=%d reachable=%d mode=%s radio=%d rssi=%d\n",
                  linkUp, joinedSsid.c_str(), mainView.valid,
                  (mainView.fetchedMs && now - mainView.fetchedMs < 6000), mainView.mode.c_str(),
                  mainView.radio, linkUp ? WiFi.RSSI() : 0);
  }

  updateLed(linkUp && mainView.valid && mainView.fetchedMs &&
            (now - mainView.fetchedMs < 6000));

  yield();
}
