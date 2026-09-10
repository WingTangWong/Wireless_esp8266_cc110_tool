# wemos-240x240

Standalone PlatformIO sketch: WeMOS D1 Mini driving a 240x240 ST7789 IPS
display over hardware SPI. Independent of the CC1101 firmware in the parent
repo — its own `platformio.ini`, build it from this directory.

## Wiring

| ST7789 | D1 Mini | ESP8266 | Notes |
|--------|---------|---------|-------|
| GND    | GND     | —       | |
| VCC    | 3V3     | —       | |
| SCL    | D5      | GPIO14  | HSPI SCLK — fixed, not remappable |
| SDA    | D7      | GPIO13  | HSPI MOSI — fixed, not remappable |
| RES    | D4      | GPIO2   | reset |
| DC     | D3      | GPIO0   | data/command |
| BLK    | D8      | GPIO15  | backlight, driven HIGH in `setup()` |

Two things this panel needs that cost an afternoon to find:

- **No CS pin.** The breakout ties chip-select low on the module, so
  `TFT_CS` is `-1`.
- **`SPI_MODE2`.** With CS held low the controller never syncs in the default
  mode 0 — the sketch runs, the backlight is on, and the screen stays black
  with no other symptom.

`RES` must not go to `D0`/GPIO16: that pin is driven through a separate RTC
register and the reset pulse doesn't land, leaving the panel uninitialised.
`DC` must not go to `D6`/GPIO12 either — that's HSPI MISO, and `SPI.begin()`
claims it.

## Build

```sh
cd wemos-240x240
~/.platformio/penv/bin/pio run -e d1_mini -t upload
~/.platformio/penv/bin/pio device monitor
```

## What it does

Off-screen `GFXcanvas16` sprites are composited onto the panel every 10 ms:

- **Counter box** (100x44, centred) — a cyan outline with a 4-digit
  zero-padded counter inside, `+1` per tick, wrapping `9999 → 0000`.
- **Ball** (16x16) — a yellow circle bouncing off the screen edges and the box.
- **Stars** (20x20, up to 8) — spinning pentagrams that flash through
  white/yellow/orange/red like an invincibility star, move at ~1.5x the ball's
  speed, and fade to black over the last 1.5 s of a 5 s life. One spawns when
  the ball bounces off the counter box and every 100 counts. With all eight
  slots busy a spawn is simply dropped.

The star canvas is a single buffer reused for all of them: each star renders
into it and blits immediately. Geometry comes from a precomputed table of 12
rotation steps (a pentagram repeats every 72°), which keeps ~100 `sinf`/`cosf`
calls per frame off a soft-float CPU.

Collision against the box is resolved one axis at a time, so a sprite's
bounding rect never overlaps the box. That is what lets the outline be drawn
once and left alone, and lets each erase be a blind rectangle fill. Every old
rect is cleared before anything is drawn, so one sprite's erase can't punch a
hole in another that was already redrawn.

## Performance

Two things had to be fixed to fit a screenful of stars into a 10 ms tick.
Figures below are worst-case draw time at 5 stars, the limit before the fixes.

**Adafruit_SPITFT has no ESP8266 bulk-SPI path.** It has one for ESP32,
nRF52, RP2040 and SAMD; the ESP8266 falls through to a per-pixel loop.
Measured on this board, that costs 3.6 µs/px for `drawRGBBitmap` and 7.2 µs/px
for `fillRect` — about 4x worse than the 20 MHz clock allows, because the cost
is per-call overhead, not clock time. `blitSprite()`/`fillRectFast()` drive the
address window directly and hand the buffer to `SPI.writeBytes()` /
`SPI.writePattern()`: **17.6 ms → 6.8 ms**.

**160 MHz.** `board_build.f_cpu = 160000000L`. The remaining CPU-bound half
(canvas fills, the triangle fan per star) halves: **6.8 ms → 5.2 ms**. The SPI
clock comes from the fixed 80 MHz APB clock, not `F_CPU`, so transfer time is
unchanged. `millis()`/`micros()` are frequency-independent in the ESP8266
core, so no timing constant needed adjusting — confirmed on the board by
counting ticks against wall clock.

The sketch prints a heartbeat every 5 s:

```
draw worst 6845 us | stars 7 | 100 ticks/s | counter 901
```

`ticks/s` should read 100. If it sags, the draw pass has outgrown `TICK_MS`
and every animation is running slow — that is the first thing to break if you
add another sprite.

With all 8 star slots live the draw pass measures 6.85 ms against the 10 ms
tick (5 stars: 5.2 ms), so a star costs ~0.55 ms — mostly the 400 px erase
plus 400 px blit. That puts the ceiling somewhere near 13 stars before the
tick overruns. Raising SPI from 20 to 40 MHz is the next lever if you want
more, but 20 MHz is the speed this panel is known-good at, so check it on
glass before trusting it.

## Watchdog

Same rule as the main firmware: the ESP8266's ~3.2 s software watchdog can't
be widened or switched off. `ESP.wdtEnable()`'s timeout argument is ignored,
and `ESP.wdtDisable()` is re-armed by the SDK on the next `yield()`/`delay()`.
So `serviceWdt()` is called after every blit and after the full-screen clear.
The ~8 s hardware watchdog is the backstop.

It feeds with `ESP.wdtFeed()` and yields with `optimistic_yield(1000)`, not
`delay(0)`. Feeding is free; the yield is not — `delay(0)`/`yield()` hands off
to the cont stack on every single call, and this runs eight-plus times a
frame. `optimistic_yield` only actually switches once >1 ms has passed, so the
SDK still gets its slot without paying the context switch per blit.

(Worth noting: `delay(0)` was *not* what was blowing the frame budget. That was
the first guess, and measuring showed swapping it changed nothing — see
Performance above for what actually cost the time.)
