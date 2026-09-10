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

Two off-screen `GFXcanvas16` sprites are composited onto the panel every
10 ms:

- **Counter box** (100x44, centred) — a cyan outline with a 4-digit
  zero-padded counter inside, `+1` per tick, wrapping `9999 → 0000`.
  Re-blitted whole each tick, so the outline is self-healing.
- **Ball** (16x16) — a yellow circle that bounces off all four screen edges
  *and* off the counter box.

Collision against the box is resolved one axis at a time, so the ball's
bounding rect never overlaps the box. That matters: the old ball position is
erased with a plain `fillRect`, and if the rects could overlap that erase
would punch a hole in the outline.

Rendering into a canvas and blitting once with `drawRGBBitmap` keeps each
sprite flicker-free — drawing text or a circle straight to the panel shows
the intermediate clear.

## Watchdog

Same rule as the main firmware: the ESP8266's ~3.2 s software watchdog can't
be widened or switched off. `ESP.wdtEnable()`'s timeout argument is ignored,
and `ESP.wdtDisable()` is re-armed by the SDK on the next `yield()`/`delay()`.
So `serviceWdt()` — `ESP.wdtFeed()` plus `delay(0)` — is called after every
blit and after the full-screen clear. The SPI writes run with interrupts
masked, so those `delay(0)` calls are what give the SDK a slot. The ~8 s
hardware watchdog is the backstop.
