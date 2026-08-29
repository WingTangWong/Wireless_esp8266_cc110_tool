# ESP8266 + CC1101 318 MHz Multifunction Tool

A browser-controlled 318 MHz (300–348 MHz band) sub-GHz analysis and replay tool
built on a WeMOS/LOLIN D1 Mini (ESP8266) and a CC1101 transceiver. It provides an
RSSI spectrum sweep, raw OOK/ASK capture and replay, pulse-timing analysis, and
focused recognizers for Linear 10-bit and MegaCode 24-bit garage/gate remotes.

The original target is a MegaCode-style garage door remote at 318.000 MHz, but
anything in the CC1101 300–348 MHz range can be tuned, captured, and replayed.

> **Legal note:** Only capture and replay signals from devices you own or are
> explicitly authorized to test. RF transmission is regulated; you are
> responsible for complying with local law.

## Hardware

| Function | CC1101 pin | D1 Mini pin | GPIO |
|----------|-----------|-------------|------|
| SPI SCK  | SCK   | D5 | GPIO14 |
| SPI MISO | MISO  | D6 | GPIO12 |
| SPI MOSI | MOSI  | D7 | GPIO13 |
| SPI CSN  | CSN   | D8 | GPIO15 |
| Async TX data in  | GDO0 | D2 | GPIO4 |
| Async RX data out | GDO2 | D1 | GPIO5 |
| Power | VCC | 3V3 | — |
| Ground | GND | GND | — |

RX capture (GDO2) and TX playback (GDO0) use separate MCU pins so the two paths
never contend. The CC1101 runs in asynchronous serial mode (`PKTFORMAT=3`),
ASK/OOK modulation, no sync/preamble/whitening/Manchester/FEC/CRC.

## Software

- **PlatformIO** with the Arduino framework (`platformio.ini`, board `d1_mini`).
- Library: `lsatan/SmartRC-CC1101-Driver-Lib @ ^3.0.2`.
- On-device storage: LittleFS (holds saved raw samples as `rf315_<name>.bin`).
- Web server: `ESP8266WebServer` on port 80, plus mDNS.

### Configure Wi-Fi, then build & flash

Credentials are **not** in source. Copy the template and fill it in:

```bash
cp secrets.ini.example secrets.ini   # git-ignored; edit with your values
```

```ini
# secrets.ini
[wifi]
sta_ssid = your-2g4-network
sta_pass = your-wifi-password
ap_ssid  = cc1101-setup
ap_pass  = pick-a-strong-passphrase   # >= 8 chars, or "" for an open AP
```

PlatformIO injects these as `CFG_WIFI_*` defines (`platformio.ini` →
`extra_configs`). If `secrets.ini` is missing, the placeholder values in
`platformio.ini` are used, the firmware still builds, and only the SoftAP comes
up (the station join just fails).

```bash
pio run                 # build
pio run -t upload       # flash over USB
pio device monitor      # serial (optional; no debug output by default)
```

### Reaching the dashboard

The device runs **concurrent AP + station** (`WIFI_AP_STA`): it always hosts its
own access point *and* keeps trying to join your home network.

| Path | URL |
|------|-----|
| Via your network (mDNS) | `http://cc1101.local/` |
| Via your network (DHCP) | address shown by your router / on `/api/status` |
| Direct — join the SoftAP (`ap_ssid`) from a phone | `http://192.168.4.1/` |

The same web server answers on both interfaces. The SoftAP stays up even when
the home network is out of range, so the dashboard is always reachable.

> The Wi-Fi credentials that were committed before this change are still in git
> history — rotate the actual router/AP passwords and see `TASKS.md` for the
> history-scrub item.

## Web UI

Single self-contained page served from PROGMEM. A status row across the top shows
radio state, current mode, station Wi-Fi (SSID / IP / RSSI), SoftAP (SSID / IP /
connected client count), and free heap. Sections:

- **Target frequency** – tune to an exact MHz value, pick capture bandwidth
  (58 / 270 / 650 kHz), and nudge ±5/20/100 kHz.
- **Frequency population / RSSI sweep** – sweep a start/stop range at a given
  step, dwell, and RX bandwidth; bar-graph spectrum with the 8 strongest bins.
- **Raw record** – capture raw OOK edges from GDO2 with an auto-stop timeout,
  then decode or save.
- **Capture / decoder parameters** – async data rate, TX power, minimum pulse
  width, and the MegaCode timing window (pulse, symbol, frame gap, header).
  Stored in the browser's `localStorage` and pushed to the device.
- **Decode** – JSON result of the generic timing analysis plus any focused
  protocol match.
- **Stored samples** – list / load / decode / play / delete saved captures.
  Playback supports 1–10 repeats and logic inversion.

A **Gate control** link in the header opens `/gate` (see below), a separate
minimal page for day-to-day gate operation.

## HTTP API

All endpoints return JSON (`{"ok":true,...}` or `{"ok":false,"error":"..."}`).
The analysis endpoints below are `GET`; the gate-control write endpoints are
`POST`. `409 busy` is returned while a capture, sweep, or playback is running.

| Endpoint | Purpose |
|----------|---------|
| `/api/status` | Radio / Wi-Fi / SoftAP / capture / sweep state, free heap |
| `/api/config?...` | Set runtime data rate, TX power, and decoder timing params |
| `/api/tune?hz=&bw=` | Set target frequency (Hz) and bandwidth (kHz) |
| `/api/nudge?delta=` | Shift target frequency by delta Hz |
| `/api/sweep/start?start=&stop=&step=&dwell=&bw=` | Begin an RSSI sweep |
| `/api/sweep/data` | Sweep results as `[{hz,rssi}]` |
| `/api/capture/start?ms=` | Begin raw capture with auto-stop (50–10000 ms) |
| `/api/capture/stop` | Stop capture now |
| `/api/capture/histogram` | Pulse-width histogram (28 bins, high/low split) |
| `/api/decode/current` | Analyze the in-memory capture |
| `/api/sample/save?name=` | Save current capture to LittleFS |
| `/api/sample/load?name=` | Load a saved sample into memory |
| `/api/sample/decode?name=` | Load and analyze a saved sample |
| `/api/sample/play?name=&repeat=&invert=` | Timer1 raw replay of a saved sample |
| `/api/sample/delete?name=` | Delete a saved sample |
| `/api/samples` | List saved samples with header metadata |

### Gate control

A separate, deliberately minimal operator surface for actuating two gates.

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/gate` | GET | Two-button operator page: **Inner gate** / **Outer gate** |
| `/gate/config` | GET | Assign a saved sample + radio settings to each button |
| `/api/gate` | GET | Current assignments, resolved sample state, forced-power/min-repeat values |
| `/api/gate/assign` | POST | Set a button's `sampleName`, `frequencyHz`, `bandwidthKhz`, `repeats`, `invert` (empty `sampleName` clears it) |
| `/api/gate/fire?which=inner\|outer` | POST | Replay that button's assigned sample |

- The POST endpoints reject cross-origin requests (an `Origin` header whose host
  doesn't match `Host`). `/api/gate/fire` returns `409 busy` during any capture,
  sweep, or playback.
- Assignments persist to LittleFS as `/gate.bin` (packed `"GATE"` v1 record,
  one `GateAssignment` per button) and are loaded on boot. A missing file or a
  since-deleted sample leaves that button unassigned and `fire` returns an error
  instead of transmitting.
- **Every gate fire forces the CC1101 to its maximum output power (+12 dBm),**
  regardless of the stored per-assignment value, because the module has a
  mismatched/stub antenna and marginal RF matching. Repeats are floored at 4.
  After the burst the dashboard's target frequency and RX state are restored.
- Served on both the station and SoftAP interfaces, so the gate page works from
  a phone joined to `cc1101-setup` with no other network.

> Anyone who can reach the device (LAN or SoftAP) can actuate the gates. There
> is no authentication yet — see `TASKS.md`.

## How it works

### Capture
`rawEdgeISR` on GDO2 (CHANGE interrupt) timestamps every edge with `micros()`,
discards pulses shorter than `minPulseUs`, and stores explicit level + duration
pairs (up to `MAX_PULSES = 3072`). After stop, `normalizeCapture()` merges
same-level runs.

### Analysis (`decodeCurrentJson`)
2-means clustering of pulse widths (overall, and per HIGH/LOW level) classifies
the encoding family (pulse-distance/PPM, pulse-width/PWM, complementary pairs),
emits a best-effort candidate bitstring (normal and inverted), and runs two
focused recognizers:

- **Linear 10-bit** – ~500/1500 µs pulse pairs with a ~21 ms frame guard.
- **MegaCode 24-bit** – ~1 ms pulse in a 6 ms PPM frame, 13 ms header low,
  leading `1` start bit; decodes facility / serial / button fields.

### Replay (`replayCurrent`)
Timer1 in one-shot mode (5 MHz / `TIM_DIV16`, 5 ticks per µs) drives GDO0 through
the stored level/duration list while the CC1101 is in TX. 1–10 repeats with an
optional logic inversion.

### Sample file format
`#pragma pack(1)` — 20-byte `SampleHeader` (`"315R"` magic, version 1,
frequencyHz, pulseCount, durationUs) followed by `pulseCount` × 5-byte
`DiskPulse` (`uint32 durationUs`, `uint8 level`).

## Repository layout

```
platformio.ini       PlatformIO project, library deps, [wifi] placeholders
secrets.ini.example  Wi-Fi credential template -> copy to secrets.ini (git-ignored)
src/main.cpp          Entire firmware (hardware, web UI, API, DSP)
src/notes.md          Reference notes on MegaCode / Flipper OOK650 preset
```

See `PROGRESS.md` for current status and `TASKS.md` for the backlog.
