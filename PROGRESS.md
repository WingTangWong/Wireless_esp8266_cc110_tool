# Progress

_Last updated: 2026-08-31_

## Status summary

Firmware (`src/main.cpp` + `src/decode.{h,cpp}`): concurrent AP+station Wi-Fi
with a per-device SSID and captive portal, the analysis dashboard, RSSI sweep
with peak/offset readout, raw capture + timing analysis, Linear / MegaCode /
EV1527 recognizers, a rolling-code advisory, LittleFS sample storage with
rename, Timer1 replay, the two-button gate control pages with an enable toggle
and fire-error reporting, and verification endpoints (`/api/selftest`,
`/api/capture/pulses`, richer `/api/status` + `/api/sweep/data`). Optional
`-DCC1101_TRACE` serial-trace build.

Host side: `tools/rfprobe.py` CLI (incl. `export` to Flipper `.sub`),
`tools/rfdecode.py` analysis port, a pytest suite (pure + device), 19 native
Unity tests for the DSP kernels, `ruff`, and GitHub Actions CI.

Builds: `d1_mini` ✅ (Flash ~40%, RAM ~65%), `d1_mini_debug` ✅, `native` ✅.

Hardware: firmware `0b136ca` flashed to a D1 Mini (MAC 5c:cf:7f:d0:86:7b) on
2026-08-31 and **the full `tests/` device suite passes (31/31, no skips)** against
it — selftest, status schema, tune round-trip + out-of-band reject, RSSI sweep
(point count / range / summary), capture mode cycle, histogram sums, gate/sample
schemas, and a live decode cross-check against `rfdecode.py`. Free heap ~18 KB.
Still unverified: end-to-end RF (capturing a real remote, protocol recognizers on
real signals, replay actuating a gate), the SoftAP/captive-portal from a phone.

## What works

| Area | State | Notes |
|------|-------|-------|
| PlatformIO build | ✅ | Board `d1_mini`, Arduino framework |
| Wi-Fi credentials out of source | ✅ | `secrets.ini` (git-ignored) → `CFG_WIFI_*` build defines; `platformio.ini` holds only `CHANGE_ME` placeholders. Builds without `secrets.ini` (SoftAP-only). |
| Verification endpoints | ✅ | `/api/selftest` (SPI/radio/FS/heap/network), fw version+build, CC1101 partnum/version, LittleFS usage on `/api/status`; `/api/sweep/data` `summary`. **Verified on hardware** (selftest ok, radio version 0x14). |
| Build metadata | ✅ | `scripts/version.py` injects `git describe` as `CFG_FW_VERSION` (fallback `0.1.0-nogit`); `static_assert` rejects a SoftAP password that is non-empty and < 8 chars. |
| CI | ✅ | `.github/workflows/ci.yml`: `pio run -e d1_mini` (no secrets), `pio test -e native`, `ruff check .`, `pytest`. Not yet run on GitHub — no remote configured. |
| `tests/` device suite on hardware | ✅ | 31/31 pass against the flashed board (`CC1101_HOST=192.168.100.114 pytest`). |
| Serial-trace build | ✅ | `[env:d1_mini_debug]` = d1_mini + `-DCC1101_TRACE`; `DBG()` no-op otherwise. Both envs build. |
| Pure DSP module + native unit tests | ✅ | `src/decode.{h,cpp}` — clustering, encoding classifier, candidate-bit extraction, Linear/MegaCode recognizers, histogram. `test/test_decode/` — 16 Unity tests (`pio test -e native`). `main.cpp` is JSON glue over it. d1_mini builds (Flash 39.4%, RAM 64.6%). |
| Host API client `tools/rfprobe.py` | ✅ | Stdlib CLI: status/selftest/tune/sweep/record/histogram/pulses/decode/samples/gate/fire/`sample …`/watch/raw; `--json`, `--timeout`. |
| `tests/` pytest suite | ✅ | 16 pure tests (rfdecode + rfprobe-CLI via in-process fake device, always run) + 15 device tests. **31/31 pass against the flashed board.** |
| `/api/capture/pulses` + `tools/rfdecode.py` | ✅ | Raw pulse list endpoint (chunk-streamed) and a pure-Python port of the clustering/encoding classifier; a device test cross-checks the two. |
| CC1101 SPI init | ✅ | `radio.getCC1101()` reports presence as `radioOk` |
| Wi-Fi station + auto-reconnect | ✅ | 30 s connect timeout, 10 s retry loop |
| Concurrent SoftAP (`WIFI_AP_STA`) | ⚠️ | STA side verified on hardware (device joined Wi-Fi, mDNS + HTTP reachable, `apSsid` = `cc1101-setup-d0867b`). AP-side join + captive portal from a phone still unverified. |
| mDNS (`cc1101.local`) | ✅ | Started once Wi-Fi is up |
| Web UI (single PROGMEM page) | ✅ | Self-contained, `localStorage`-backed settings; info line (fw/CC1101/FS), Self-test button, authorized-use footer |
| `/api/status` polling | ✅ | UI polls every 900 ms |
| RSSI frequency sweep | ✅ | Confirmed on hardware; `/api/sweep` test passes (point count, RSSI range, summary) |
| Raw OOK capture (GDO2 ISR) | ✅ | Edge-timed, glitch filter, 3072-pulse buffer |
| Pulse-width histogram | ✅ | 28 bins, high/low split |
| Generic timing analysis | ✅ | 2-means clustering, encoding-family guess, candidate bits |
| Linear 10-bit recognizer | ⚠️ | Implemented + Unity-tested; not yet validated against a real remote |
| MegaCode 24-bit recognizer | ⚠️ | Implemented + Unity-tested (synthetic frame); not yet confirmed on a live remote |
| EV1527 / PT2262 recognizer | ⚠️ | Implemented + Unity-tested + Python port; self-calibrates Te from the sync gap; not yet validated on a live remote |
| Save / load / list / delete samples (LittleFS) | ✅ | `rf315_<name>.bin`, `"315R"` format v1 |
| Timer1 raw replay | ⚠️ | Implemented; end-to-end "captured remote actuates the door" not yet verified |
| Gate control pages (`/gate`, `/gate/config`) | ✅ | Both pages + `/api/gate*` return 200 on hardware; assignments persist to `/gate.bin` v2. Same-origin checked. |
| Gate fire forces max TX power | ⚠️ | `fireGate()` clamps `txPowerDbm` to +12 and floors repeats at 4, restores dashboard state after. Path exercised via tests; not yet verified to actuate a real gate. |
| Gate enable toggle + fire-error reporting | ✅ | `gate.bin` v2 (`enabled`), `POST /api/gate/enable`, `lastFireError` on `/api/gate` (returns `enabled:true` on hardware), config-page checkbox, sample→frequency auto-fill. |

## Not yet started

- **End-to-end RF is unverified**: capturing a real 318 MHz remote, the
  Linear / MegaCode / EV1527 recognizers on real signals, and a replay
  actually actuating a gate. The HTTP/decode/DSP layers are hardware-verified;
  the antenna-to-antenna path is not.
- SoftAP from a phone: join `cc1101-setup-d0867b`, captive portal opens the
  dashboard, `apClients` updates, STA + AP stay up together.
- Gate PIN / basic-auth (the enable toggle exists; a real secret does not).
- Raw `.bin` upload back to the device.

## Known issues / risks

- **Wi-Fi credentials are still in git history.** They no longer live in
  `src/main.cpp` (moved to git-ignored `secrets.ini` via `CFG_WIFI_*` build
  defines), and the SoftAP password was rotated. But every commit up to and
  including `be0b666` still contains the old station SSID/password and the old
  SoftAP password `<redacted>` — rotate the actual router/AP passwords and scrub
  history before sharing this repo.
- Everything lives in one file (~2000 lines); no module split yet.
- `firmwareBuild` is `__DATE__ " " __TIME__`, which only changes when `main.cpp`
  itself recompiles (not on a docs-only or dep change).
- **Gate control has no real authentication.** There is a runtime enable/disable
  toggle and a same-origin check, but while enabled anyone who can reach the
  device can fire a gate. A PIN/basic-auth is still a TODO for untrusted nets.
- `/api/gate/fire` responds `ok` before the fire runs (fire-and-forget via
  `loop()`); a load failure now lands in `lastFireError` on the next `/api/gate`.
- Gate `fireGate()` calls `loadSample()`, which overwrites whatever capture is
  currently in memory on the dashboard.
- `getMHZ()` readback from some SmartRC/CC1101 combos returns 0.0; code
  deliberately treats the requested frequency as authoritative (`setRadioFrequency`).
- Static RAM ~65%; **measured free heap on hardware is ~18 KB** while idle and
  serving. Workable but not generous — watch it if adding features.
- Replay timing is bounded to 2 µs–1.6 s per pulse; captures with longer gaps
  are clamped.

## Hardware verification checklist

- [x] CC1101 detected over SPI (version reg 0x14)
- [x] Wi-Fi joins, web UI + all `/api/*` reachable; mDNS resolves
- [x] Current firmware flashed; `CC1101_HOST=… pytest` → 31/31
- [x] `/api/selftest` → `ok:true`, all checks green
- [x] RSSI sweep produces a sensible spectrum; `summary` correct
- [x] `/gate`, `/gate/config`, `/api/gate` all return 200
- [x] Live decode cross-check (firmware vs `rfdecode.py`) passes
- [ ] SoftAP `cc1101-setup-d0867b` visible; phone joins, captive portal opens
- [ ] STA and AP stay up together; `apClients` count tracks connections
- [ ] Assign a sample to each gate button on `/gate/config`; survives reboot
- [ ] `/gate` buttons fire; assigned gate actuates at forced max power
- [ ] Capture a real 318 MHz remote and see clean pulse pairs
- [ ] MegaCode / Linear / EV1527 recognizer decodes a real remote
- [ ] Replay a saved sample and actuate the target device
- [ ] Confirm replay range / antenna adequacy
