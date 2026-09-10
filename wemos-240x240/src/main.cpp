// WeMOS D1 Mini + ST7789 240x240 IPS display, hardware SPI.
//
// Two off-screen sprites (GFXcanvas16 double buffers) composited onto the
// panel each tick: a counter in an outlined box, and a ball that bounces off
// that box and off the screen edges.
//
// ST7789        Wemos D1 Mini
// --------------------------
// GND   ------ GND
// VCC   ------ 3V3
// SCL   ------ D5   (GPIO14, HSPI SCLK)  fixed by the SPI peripheral
// SDA   ------ D7   (GPIO13, HSPI MOSI)  fixed by the SPI peripheral
// RES   ------ D4   (GPIO2)
// DC    ------ D3   (GPIO0)
// BLK   ------ D8   (GPIO15)  backlight, HIGH = on
//
// The panel has no CS pin (tied low on the module), so TFT_CS is -1 and the
// controller must be clocked in SPI_MODE2.

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#define TFT_CS -1
#define TFT_DC D3
#define TFT_RST D4
#define TFT_BL D8
// SCK = D5, MOSI = D7 - hardware SPI pins, not configurable on the ESP8266.

static Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

static const int16_t SCR_W = 240;
static const int16_t SCR_H = 240;

// --- counter box sprite -----------------------------------------------------
static const int16_t BOX_W = 100;
static const int16_t BOX_H = 44;
static const int16_t BOX_X = (SCR_W - BOX_W) / 2;  // 70
static const int16_t BOX_Y = (SCR_H - BOX_H) / 2;  // 98
static GFXcanvas16 boxSprite(BOX_W, BOX_H);

// --- ball sprite ------------------------------------------------------------
static const int16_t BALL_D = 16;  // bounding box; radius is BALL_D/2
static GFXcanvas16 ballSprite(BALL_D, BALL_D);

static int16_t ballX = 20, ballY = 20;
static int16_t ballVX = 3, ballVY = 2;

static uint16_t counter = 0;
static uint32_t lastTickMs = 0;

// The ESP8266's ~3.2 s software watchdog cannot actually be widened or turned
// off: ESP.wdtEnable()'s timeout argument is ignored, and ESP.wdtDisable()
// (system_soft_wdt_stop) is re-armed by the SDK on the next yield()/delay().
// So don't pretend to - feed it. Every blit below is only a few ms, but the
// SPI writes run with interrupts masked, so give the SDK a slot between them.
// The ~8 s hardware watchdog is the backstop.
static inline void serviceWdt() {
  ESP.wdtFeed();
  delay(0);  // == yield() on the ESP8266: run SDK/background work
}

// Do the two axis-aligned boxes overlap?
static bool overlaps(int16_t ax, int16_t ay, int16_t aw, int16_t ah, int16_t bx,
                     int16_t by, int16_t bw, int16_t bh) {
  return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}

// Render the counter + its outline into the off-screen buffer, then blit.
static void drawBoxSprite() {
  char buf[8];
  snprintf(buf, sizeof(buf), "%04u", counter);

  boxSprite.fillScreen(ST77XX_BLACK);
  boxSprite.drawRect(0, 0, BOX_W, BOX_H, ST77XX_CYAN);

  boxSprite.setTextSize(3);
  boxSprite.setTextColor(ST77XX_GREEN);
  boxSprite.setCursor((BOX_W - 4 * 6 * 3) / 2, (BOX_H - 7 * 3) / 2);
  boxSprite.print(buf);

  tft.drawRGBBitmap(BOX_X, BOX_Y, boxSprite.getBuffer(), BOX_W, BOX_H);
  serviceWdt();
}

// Advance the ball one step, bouncing off the screen edges and the box.
static void moveBall() {
  int16_t nx = ballX + ballVX;
  int16_t ny = ballY + ballVY;

  // Screen edges.
  if (nx < 0) {
    nx = 0;
    ballVX = -ballVX;
  } else if (nx > SCR_W - BALL_D) {
    nx = SCR_W - BALL_D;
    ballVX = -ballVX;
  }
  if (ny < 0) {
    ny = 0;
    ballVY = -ballVY;
  } else if (ny > SCR_H - BALL_D) {
    ny = SCR_H - BALL_D;
    ballVY = -ballVY;
  }

  // The box, one axis at a time: if the step into that axis would land the
  // ball inside the box, cancel the step and flip that component. Resolving
  // per-axis keeps the ball's rect from ever overlapping the box, so erasing
  // its old position can never chew a hole in the outline.
  if (overlaps(nx, ballY, BALL_D, BALL_D, BOX_X, BOX_Y, BOX_W, BOX_H)) {
    nx = ballX;
    ballVX = -ballVX;
  }
  if (overlaps(nx, ny, BALL_D, BALL_D, BOX_X, BOX_Y, BOX_W, BOX_H)) {
    ny = ballY;
    ballVY = -ballVY;
  }

  ballX = nx;
  ballY = ny;
}

static void drawBallSprite() {
  ballSprite.fillScreen(ST77XX_BLACK);
  ballSprite.fillCircle(BALL_D / 2, BALL_D / 2, BALL_D / 2 - 1, ST77XX_YELLOW);
  tft.drawRGBBitmap(ballX, ballY, ballSprite.getBuffer(), BALL_D, BALL_D);
  serviceWdt();
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\nST7789 240x240: bouncing ball + counter");

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);  // backlight on

  SPI.begin();  // ESP8266 hardware SPI: SCK = D5, MOSI = D7
  tft.init(240, 240, SPI_MODE2);
  tft.setSPISpeed(20000000);
  tft.fillScreen(ST77XX_BLACK);
  serviceWdt();  // the full-screen clear is the longest single write here

  drawBoxSprite();
  drawBallSprite();
  lastTickMs = millis();
  Serial.println("init done");
}

void loop() {
  uint32_t now = millis();
  if (now - lastTickMs < 10) return;

  // Fixed 10 ms step, but resync rather than spiral if a frame runs long.
  if (now - lastTickMs > 200) {
    lastTickMs = now;
  } else {
    lastTickMs += 10;
  }

  int16_t prevX = ballX, prevY = ballY;
  moveBall();

  counter = (counter + 1) % 10000;
  drawBoxSprite();

  if (prevX != ballX || prevY != ballY) {
    tft.fillRect(prevX, prevY, BALL_D, BALL_D, ST77XX_BLACK);
  }
  drawBallSprite();
}
