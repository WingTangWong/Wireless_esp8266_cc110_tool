// WeMOS D1 Mini + ST7789 240x240 IPS display, hardware SPI.
//
// Off-screen GFXcanvas16 sprites composited onto the panel each tick: a
// counter in an outlined box, a ball bouncing off that box and the screen
// edges, and up to five spinning invincibility stars.
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
#include <math.h>
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
static const uint32_t TICK_MS = 10;

// --- counter box ------------------------------------------------------------
// The outline is static: drawn once in setup() and never touched again. That
// is only safe because every moving sprite collides with the box as a solid
// AABB, so no sprite's erase rect can ever reach the outline pixels. Only the
// digits live in a sprite, which keeps the per-tick blit small.
static const int16_t BOX_W = 100;
static const int16_t BOX_H = 44;
static const int16_t BOX_X = (SCR_W - BOX_W) / 2;  // 70
static const int16_t BOX_Y = (SCR_H - BOX_H) / 2;  // 98

static const int16_t TEXT_W = 80;
static const int16_t TEXT_H = 26;
static const int16_t TEXT_X = BOX_X + (BOX_W - TEXT_W) / 2;  // 80
static const int16_t TEXT_Y = BOX_Y + (BOX_H - TEXT_H) / 2;  // 107
static GFXcanvas16 textSprite(TEXT_W, TEXT_H);

// --- ball sprite ------------------------------------------------------------
static const int16_t BALL_D = 16;  // bounding box; radius is BALL_D/2
static GFXcanvas16 ballSprite(BALL_D, BALL_D);

static int16_t ballX = 20, ballY = 20;
static int8_t ballVX = 3, ballVY = 2;
static int16_t ballPX = 20, ballPY = 20;  // previous position, for the erase

// --- star sprites -----------------------------------------------------------
// One canvas, reused: each star is rendered into it and blitted immediately,
// so all MAX_STARS of them cost one buffer rather than eight.
static const int16_t STAR_D = 20;
static const uint8_t MAX_STARS = 8;
static const uint32_t STAR_LIFE_MS = 5000;
static const uint32_t STAR_FADE_MS = 1500;  // fades over the tail of its life
static GFXcanvas16 starSprite(STAR_D, STAR_D);

// Ball speed is |(3,2)| = 3.61 px/tick; stars run ~1.5x that, so ~5.4.
static const int8_t STAR_DIRS[][2] = {{5, 2},  {2, 5},  {-5, 2},  {-2, 5},
                                      {5, -2}, {2, -5}, {-5, -2}, {-2, -5},
                                      {4, 4},  {-4, 4}, {4, -4},  {-4, -4}};
static const uint8_t STAR_DIR_COUNT = sizeof(STAR_DIRS) / sizeof(STAR_DIRS[0]);

struct Star {
  bool active;
  int16_t x, y;
  int16_t px, py;  // previous position, for the erase
  int8_t vx, vy;
  uint8_t rot;         // index into the precomputed rotation table
  uint8_t colorPhase;  // per-star offset so they don't all flash in unison
  uint32_t bornMs;
};
static Star stars[MAX_STARS];

// A 5-pointed star is symmetric every 72 degrees, so the rotation table only
// has to cover that much. Ten vertices per step: outer, inner, outer, ...
// Precomputing kills ~100 sinf/cosf calls per frame on a soft-float CPU.
static const uint8_t ROT_STEPS = 12;
static int8_t starVX[ROT_STEPS][10];
static int8_t starVY[ROT_STEPS][10];

static uint16_t counter = 0;
static uint32_t lastTickMs = 0;
static uint8_t rotTick = 0;

// The ESP8266's ~3.2 s software watchdog cannot actually be widened or turned
// off: ESP.wdtEnable()'s timeout argument is ignored, and ESP.wdtDisable()
// (system_soft_wdt_stop) is re-armed by the SDK on the next yield()/delay().
// So don't pretend to - feed it, after every blit. The ~8 s hardware watchdog
// is the backstop.
//
// Feeding is free; the yield is not. delay(0)/yield() hands off to the cont
// stack on every single call, so optimistic_yield is used instead - it only
// actually switches once >1 ms has passed, which at eight calls a frame keeps
// the SDK fed without paying the context switch per blit.
static inline void serviceWdt() {
  ESP.wdtFeed();
  optimistic_yield(1000);
}

// Adafruit_SPITFT has bulk-SPI fast paths for ESP32, nRF52, RP2040 and SAMD -
// but none for the ESP8266, so drawRGBBitmap() and fillRect() fall through to
// a per-pixel loop. Measured on this board: 3.6 us/px to blit and 7.2 us/px to
// fill, roughly 4x worse than the 20 MHz clock allows, because the cost is
// per-call overhead rather than clock time. Driving the address window here and
// handing the whole buffer to the ESP8266's bulk SPI writes fixes that.
static void blitSprite(int16_t x, int16_t y, GFXcanvas16 &c, int16_t w,
                       int16_t h) {
  c.byteSwap();  // canvas is little-endian, the panel wants MSB first
  tft.startWrite();
  tft.setAddrWindow(x, y, w, h);
  SPI.writeBytes((const uint8_t *)c.getBuffer(), (uint32_t)w * h * 2);
  tft.endWrite();
  // No swap back: every sprite is re-rendered from fillScreen() each frame.
}

static void fillRectFast(int16_t x, int16_t y, int16_t w, int16_t h,
                         uint16_t color) {
  const uint8_t pat[2] = {(uint8_t)(color >> 8), (uint8_t)color};
  tft.startWrite();
  tft.setAddrWindow(x, y, w, h);
  SPI.writePattern(pat, 2, (uint32_t)w * h);
  tft.endWrite();
}

// Do the two axis-aligned boxes overlap?
static bool overlaps(int16_t ax, int16_t ay, int16_t aw, int16_t ah, int16_t bx,
                     int16_t by, int16_t bw, int16_t bh) {
  return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}

// Bounce an axis-aligned sprite off the screen edges and off the counter box.
// Resolving one axis at a time keeps the sprite's rect from ever overlapping
// the box, which is what lets the outline stay static and the erase be a blind
// rectangle fill. Returns true if it bounced off the box this step.
static bool stepAndBounce(int16_t &x, int16_t &y, int8_t &vx, int8_t &vy,
                          int16_t size) {
  int16_t nx = x + vx;
  int16_t ny = y + vy;

  if (nx < 0) {
    nx = 0;
    vx = -vx;
  } else if (nx > SCR_W - size) {
    nx = SCR_W - size;
    vx = -vx;
  }
  if (ny < 0) {
    ny = 0;
    vy = -vy;
  } else if (ny > SCR_H - size) {
    ny = SCR_H - size;
    vy = -vy;
  }

  bool hitBox = false;
  if (overlaps(nx, y, size, size, BOX_X, BOX_Y, BOX_W, BOX_H)) {
    nx = x;
    vx = -vx;
    hitBox = true;
  }
  if (overlaps(nx, ny, size, size, BOX_X, BOX_Y, BOX_W, BOX_H)) {
    ny = y;
    vy = -vy;
    hitBox = true;
  }

  x = nx;
  y = ny;
  return hitBox;
}

// Scale an RGB565 colour toward black. level 255 = untouched, 0 = black.
static uint16_t fade565(uint16_t c, uint8_t level) {
  uint16_t r = ((c >> 11) & 0x1F) * level / 255;
  uint16_t g = ((c >> 5) & 0x3F) * level / 255;
  uint16_t b = (c & 0x1F) * level / 255;
  return (r << 11) | (g << 5) | b;
}

// Mario-style invincibility flash: white -> yellow -> orange -> red and back.
static uint16_t starColor(const Star &s, uint32_t now) {
  static const uint16_t shiny[] = {0xFFFF, 0xFFE0, 0xFD20,
                                   0xF800, 0xFD20, 0xFFE0};
  static const uint8_t n = sizeof(shiny) / sizeof(shiny[0]);

  uint16_t c = shiny[((now / 70) + s.colorPhase) % n];

  uint32_t age = now - s.bornMs;
  if (age > STAR_LIFE_MS - STAR_FADE_MS) {
    uint32_t remaining = STAR_LIFE_MS - age;
    c = fade565(c, (uint8_t)(255UL * remaining / STAR_FADE_MS));
  }
  return c;
}

static void buildStarTable() {
  const float rOut = STAR_D / 2.0f - 1.0f;
  const float rIn = rOut * 0.382f;  // 1/phi^2 - a regular pentagram
  for (uint8_t s = 0; s < ROT_STEPS; s++) {
    float base = s * (2.0f * (float)M_PI / 5.0f) / ROT_STEPS;
    for (uint8_t i = 0; i < 10; i++) {
      float r = (i & 1) ? rIn : rOut;
      float a = base + i * (float)(M_PI / 5.0);
      starVX[s][i] = (int8_t)lroundf(r * sinf(a));
      starVY[s][i] = (int8_t)lroundf(-r * cosf(a));
    }
  }
}

// Claim a free slot and drop a star somewhere clear of the counter box.
// Returns false when all five slots are busy - the spawn is simply lost.
static bool spawnStar() {
  uint8_t slot = MAX_STARS;
  for (uint8_t i = 0; i < MAX_STARS; i++) {
    if (!stars[i].active) {
      slot = i;
      break;
    }
  }
  if (slot == MAX_STARS) return false;

  int16_t x = 0, y = 0;
  bool placed = false;
  for (uint8_t tries = 0; tries < 12; tries++) {
    x = random(0, SCR_W - STAR_D + 1);
    y = random(0, SCR_H - STAR_D + 1);
    if (!overlaps(x, y, STAR_D, STAR_D, BOX_X, BOX_Y, BOX_W, BOX_H)) {
      placed = true;
      break;
    }
  }
  if (!placed) return false;

  Star &s = stars[slot];
  const uint8_t d = random(0, STAR_DIR_COUNT);
  s.active = true;
  s.x = s.px = x;
  s.y = s.py = y;
  s.vx = STAR_DIRS[d][0];
  s.vy = STAR_DIRS[d][1];
  s.rot = random(0, ROT_STEPS);
  s.colorPhase = random(0, 6);
  s.bornMs = millis();
  return true;
}

static void drawCounter() {
  char buf[8];
  snprintf(buf, sizeof(buf), "%04u", counter);

  textSprite.fillScreen(ST77XX_BLACK);
  textSprite.setTextSize(3);
  textSprite.setTextColor(ST77XX_GREEN);
  textSprite.setCursor((TEXT_W - 4 * 6 * 3) / 2, (TEXT_H - 7 * 3) / 2);
  textSprite.print(buf);

  blitSprite(TEXT_X, TEXT_Y, textSprite, TEXT_W, TEXT_H);
  serviceWdt();
}

static void drawBall() {
  ballSprite.fillScreen(ST77XX_BLACK);
  ballSprite.fillCircle(BALL_D / 2, BALL_D / 2, BALL_D / 2 - 1, ST77XX_YELLOW);
  blitSprite(ballX, ballY, ballSprite, BALL_D, BALL_D);
  serviceWdt();
}

static void drawStar(const Star &s, uint16_t color) {
  const int16_t c = STAR_D / 2;
  const int8_t *ox = starVX[s.rot];
  const int8_t *oy = starVY[s.rot];

  starSprite.fillScreen(ST77XX_BLACK);
  // Fan of ten triangles from the centre - a pentagram is star-shaped about
  // its centre, so the union of those triangles is exactly the filled star.
  for (uint8_t i = 0; i < 10; i++) {
    uint8_t j = (i + 1) % 10;
    starSprite.fillTriangle(c, c, c + ox[i], c + oy[i], c + ox[j], c + oy[j],
                            color);
  }

  blitSprite(s.x, s.y, starSprite, STAR_D, STAR_D);
  serviceWdt();
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.printf("\nST7789 240x240: counter + ball + stars @ %u MHz\n",
                ESP.getCpuFreqMHz());

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);  // backlight on

  randomSeed(micros());
  buildStarTable();

  SPI.begin();  // ESP8266 hardware SPI: SCK = D5, MOSI = D7
  tft.init(240, 240, SPI_MODE2);
  tft.setSPISpeed(20000000);
  tft.fillScreen(ST77XX_BLACK);
  serviceWdt();  // the full-screen clear is the longest single write here

  tft.drawRect(BOX_X, BOX_Y, BOX_W, BOX_H, ST77XX_CYAN);  // static outline
  drawCounter();
  drawBall();
  lastTickMs = millis();
  Serial.println("init done");
}

void loop() {
  uint32_t now = millis();
  if (now - lastTickMs < TICK_MS) return;

  // Fixed 10 ms step, but resync rather than spiral if a frame runs long.
  if (now - lastTickMs > 200) {
    lastTickMs = now;
  } else {
    lastTickMs += TICK_MS;
  }

  // --- update -------------------------------------------------------------
  ballPX = ballX;
  ballPY = ballY;
  bool ballHitBox = stepAndBounce(ballX, ballY, ballVX, ballVY, BALL_D);

  counter = (counter + 1) % 10000;

  if (ballHitBox) spawnStar();
  if (counter % 100 == 0) spawnStar();

  bool spin = (++rotTick >= 2);  // ~0.8 rev/s
  if (spin) rotTick = 0;

  for (uint8_t i = 0; i < MAX_STARS; i++) {
    Star &s = stars[i];
    if (!s.active) continue;
    s.px = s.x;
    s.py = s.y;
    if (now - s.bornMs >= STAR_LIFE_MS) continue;  // expires in the erase pass
    stepAndBounce(s.x, s.y, s.vx, s.vy, STAR_D);
    if (spin) s.rot = (s.rot + 1) % ROT_STEPS;
  }

  // --- erase pass ---------------------------------------------------------
  // Every old rect is cleared before anything is drawn, so one sprite's erase
  // can never punch a hole in another sprite that was already redrawn.
  if (ballPX != ballX || ballPY != ballY) {
    fillRectFast(ballPX, ballPY, BALL_D, BALL_D, ST77XX_BLACK);
  }
  for (uint8_t i = 0; i < MAX_STARS; i++) {
    Star &s = stars[i];
    if (!s.active) continue;
    bool expired = (now - s.bornMs >= STAR_LIFE_MS);
    if (expired || s.px != s.x || s.py != s.y) {
      fillRectFast(s.px, s.py, STAR_D, STAR_D, ST77XX_BLACK);
    }
    if (expired) s.active = false;
  }
  serviceWdt();

  // --- draw pass ----------------------------------------------------------
  uint32_t drawStart = micros();
  drawCounter();
  drawBall();
  uint8_t live = 0;
  for (uint8_t i = 0; i < MAX_STARS; i++) {
    if (stars[i].active) {
      drawStar(stars[i], starColor(stars[i], now));
      live++;
    }
  }

  // Heartbeat: the frame budget is the thing that breaks first if a sprite is
  // added, so keep it visible. ticks/s should read 100 - if it sags, the draw
  // pass has outgrown TICK_MS and every animation is running slow.
  static uint32_t worstUs = 0, statMs = 0, ticks = 0;
  uint32_t drawUs = micros() - drawStart;
  if (drawUs > worstUs) worstUs = drawUs;
  ticks++;
  if (now - statMs >= 5000) {
    Serial.printf("draw worst %lu us | stars %u | %lu ticks/s | counter %u\n",
                  (unsigned long)worstUs, live,
                  (unsigned long)(ticks * 1000UL / (now - statMs)), counter);
    worstUs = 0;
    ticks = 0;
    statMs = now;
  }
}
