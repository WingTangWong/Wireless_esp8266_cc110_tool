/*
  remote_main.cpp - Wi-Fi remote control + display for the CC1101 318 MHz tool.

  Target: a second WeMOS/LOLIN D1 Mini (NO CC1101). It joins the main unit's
  SoftAP (cc1101-setup-<chipId>, paired via the shared ap_pass from secrets.ini),
  polls the main unit's HTTP API for status, drives an SSD1306 128x64 I2C OLED
  (SDA=D2/GPIO4, SCL=D1/GPIO5), and has two momentary buttons to GND:
    BTN_INNER -> D5 / GPIO14   (POST /api/gate/fire?which=inner)
    BTN_OUTER -> D6 / GPIO12   (POST /api/gate/fire?which=outer)
  Onboard LED (D4 / GPIO2, active-low) gives feedback.

  Build:  pio run -e d1_mini_remote [-t upload]
          -DREMOTE_HEADLESS for a display-less build (serial + LED only).

  This file is the whole remote firmware. src/main.cpp / src/decode.* are the
  main unit and are excluded from this env via build_src_filter.
*/
#include <Arduino.h>

#ifndef CFG_FW_VERSION
#define CFG_FW_VERSION "0.1.0-nogit"
#endif
static constexpr char FIRMWARE_VERSION[] = CFG_FW_VERSION;

static constexpr uint8_t PIN_LED = LED_BUILTIN;   // D4 / GPIO2, active-low

// -----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.printf("cc1101 remote %s  role=remote  (scaffold)\n", FIRMWARE_VERSION);

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);   // off
}

void loop() {
  // Heartbeat until the real firmware lands (feat/remote-firmware).
  digitalWrite(PIN_LED, LOW);
  delay(40);
  digitalWrite(PIN_LED, HIGH);
  delay(1960);
}
