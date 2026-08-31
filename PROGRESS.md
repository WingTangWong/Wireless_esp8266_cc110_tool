# Progress

_Last updated: 2026-08-31_

## Status summary

Firmware builds and runs on the D1 Mini + CC1101. Wi-Fi station mode, the web UI,
the RSSI sweep, raw capture, timing analysis, LittleFS sample storage, and Timer1
replay are all implemented in a single `src/main.cpp`. Per the `scan works now`
commit (2026-08-29), the frequency sweep is confirmed working on hardware.

## What works

| Area | State | Notes |
|------|-------|-------|
| PlatformIO build | ✅ | Board `d1_mini`, Arduino framework |
| Wi-Fi credentials out of source | ✅ | `secrets.ini` (git-ignored) → `CFG_WIFI_*` build defines; `platformio.ini` holds only `CHANGE_ME` placeholders. Builds without `secrets.ini` (SoftAP-only). |
| Verification endpoints | ✅ | `/api/selftest` (SPI/radio/FS/heap/network), plus fw version+build, CC1101 partnum/version, LittleFS usage on `/api/status`; `/api/sweep/data` gains a `summary`. Builds; not yet hit on hardware. |
| Build metadata | ✅ | `scripts/version.py` injects `git describe` as `CFG_FW_VERSION` (fallback `0.1.0-nogit`); `static_assert` rejects a SoftAP password that is non-empty and < 8 chars. |
| CI | ✅ | `.github/workflows/ci.yml`: `pio run` (d1_mini, no secrets), `compileall`, `pytest` (device tests skip). Not yet run on GitHub — no remote configured. |
| Host API client `tools/rfprobe.py` | ✅ | Stdlib CLI: status/selftest/tune/sweep/record/decode/samples/gate, `--json`. Verified against a mock device. |
| `tests/` pytest suite | ✅ | 14 tests, auto-skip without `CC1101_HOST`; all pass against a mock device. Not yet run against real hardware. |
| CC1101 SPI init | ✅ | `radio.getCC1101()` reports presence as `radioOk` |
| Wi-Fi station + auto-reconnect | ✅ | 30 s connect timeout, 10 s retry loop |
| Concurrent SoftAP (`WIFI_AP_STA`) | ⚠️ | Implemented + builds; AP `cc1101-setup` at `192.168.4.1`, WPA2, re-asserted in `serviceWifi()`. Not yet checked on hardware. |
| mDNS (`cc1101.local`) | ✅ | Started once Wi-Fi is up |
| Web UI (single PROGMEM page) | ✅ | Self-contained, `localStorage`-backed settings |
| `/api/status` polling | ✅ | UI polls every 900 ms |
| RSSI frequency sweep | ✅ | Confirmed on hardware ("scan works now") |
| Raw OOK capture (GDO2 ISR) | ✅ | Edge-timed, glitch filter, 3072-pulse buffer |
| Pulse-width histogram | ✅ | 28 bins, high/low split |
| Generic timing analysis | ✅ | 2-means clustering, encoding-family guess, candidate bits |
| Linear 10-bit recognizer | ⚠️ | Implemented; not yet validated against a real remote |
| MegaCode 24-bit recognizer | ⚠️ | Implemented; not yet confirmed decoding a live 318 MHz MegaCode remote |
| Save / load / list / delete samples (LittleFS) | ✅ | `rf315_<name>.bin`, `"315R"` format v1 |
| Timer1 raw replay | ⚠️ | Implemented; end-to-end "captured remote actuates the door" not yet verified |
| Gate control pages (`/gate`, `/gate/config`) | ⚠️ | Implemented + builds. Two-button operator page + assignment page; assignments persist to `/gate.bin` (`"GATE"` v1). POSTs are same-origin checked. Not yet exercised on hardware. |
| Gate fire forces max TX power | ⚠️ | `fireGate()` clamps `txPowerDbm` to +12 and floors repeats at 4, restores dashboard state after. Not yet verified to actuate a real gate. |

## Not yet started

- **Nothing in this repo has been run on real hardware since the SoftAP, gate
  control, and verification-endpoint work landed.** Everything below the first
  three rows of the table is "builds, unverified". The `tests/` suite is the
  intended way to check most of it once a board is flashed.
- On-hardware check of the SoftAP: phone joins `cc1101-setup`, dashboard loads at
  `192.168.4.1`, `apClients` count updates, STA + AP stay up together.
- Gate control: captive-portal / auth for the operator page, and a config
  toggle to disable it. Currently anyone on the LAN or SoftAP can fire a gate.
- `[env:native]` Unity tests for the pure DSP helpers (no hardware needed).
- Frequency auto-detect from the sweep (currently manual tune).
- Serial/debug diagnostics build.
- Downloading/uploading sample files over HTTP.

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
- **Gate control has no authentication.** The same-origin check on the POST
  endpoints only stops other websites; anyone who can reach the device can open
  `/gate` and fire a gate. Acceptable only on a trusted network / private SoftAP.
- `/api/gate/fire` responds `ok` before the fire runs (fire-and-forget via
  `loop()`), matching `/api/sample/play`; a load failure is not reported back.
- Gate `fireGate()` calls `loadSample()`, which overwrites whatever capture is
  currently in memory on the dashboard.
- `unused platformio.ini dependency`: `mfurga/CC1101 @ ^1.5.0` is declared but
  not `#include`d anywhere in `src/`. Either wire it in or drop it.
- `getMHZ()` readback from some SmartRC/CC1101 combos returns 0.0; code
  deliberately treats the requested frequency as authoritative (`setRadioFrequency`).
- Long captures near `MAX_PULSES` (3072) silently stop the ISR — expected, but
  the UI does not flag truncation.
- Replay timing is bounded to 2 µs–1.6 s per pulse; captures with longer gaps
  are clamped.

## Hardware verification checklist

- [x] CC1101 detected over SPI
- [x] Wi-Fi joins, web UI reachable
- [ ] Re-flash current firmware; `CC1101_HOST=… pytest` passes (14 tests)
- [ ] `tools/rfprobe.py selftest` returns `ok:true` with a sane radio version
- [ ] SoftAP `cc1101-setup` visible; phone joins and loads `192.168.4.1`
- [ ] STA and AP stay up together; `apClients` count tracks connections
- [x] RSSI sweep produces a sensible spectrum
- [ ] Assign a sample to each gate button on `/gate/config`; survives reboot
- [ ] `/gate` buttons fire; assigned gate actuates at forced max power
- [ ] Capture a real 318 MHz remote and see clean pulse pairs
- [ ] MegaCode recognizer decodes that remote's facility/serial/button
- [ ] Replay a saved sample and actuate the target device
- [ ] Confirm replay range / antenna adequacy
