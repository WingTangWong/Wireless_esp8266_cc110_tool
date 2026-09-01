# Progress

_Last updated: 2026-09-01_

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
`tools/rfdecode.py` - a full Python port of the decode kernels with its own
unit tests - a pytest suite (pure + device), `ruff`, and GitHub Actions CI.

Builds: `d1_mini` ✅ (Flash ~40%, RAM ~65%), `d1_mini_debug` ✅,
`d1_mini_remote` ✅ (Flash ~26%, RAM ~35% — scaffold only). This is MCU
firmware - there is no host/`native` PlatformIO env; the decode logic is
verified by the Python port instead.

**Wi-Fi remote (second D1 Mini):** `src/remote/` firmware is functional and
**verified on hardware** — flashed to the second board, it auto-joined the main
unit's SoftAP (`cc1101-setup-d0867b`, paired via the shared `ap_pass`), polled
`/api/status` + `/api/gate`, and parsed them correctly (`mode=idle radio=1`,
RSSI -31). Buttons + `/api/remote/press` fire the gate via the main unit; a
`/api/remote/status` endpoint exposes the cached view. OLED shows OFFLINE / node
idle-SENDING-BUSY / per-button READY; the onboard LED PWM-pulses per state.
`tests/test_remote.py` covers the remote surface + the remote→main trigger path
(skips without `CC1101_REMOTE_HOST`).

The **`d1_mini_remote_selftest`** build was run on the second board: it pulled
`/api/status|gate|selftest|config|samples` from the node over Wi-Fi to serial,
then drove `/api/tune` (freq changed and read back) and `/api/capture/start`
(node reached `recording`) — **2 PASS / 2 SKIP → OK, repeatable**. The two
button/gate steps skip because the user's gates are live+assigned (would actuate
real hardware); a physical press / `POST /api/remote/press` /
`-DREMOTE_SELFTEST_FIRE` exercises that path.

Still to verify: OLED rendering + a physical button press (display not wired).
See TASKS "Wi-Fi remote".

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
| CI | ✅ | `.github/workflows/ci.yml`: `pio run` for `d1_mini` + `d1_mini_debug`, `ruff check .`, `pytest`. Runs on GitHub. |
| `tests/` device suite on hardware | ✅ | 31/31 pass against the flashed board (`CC1101_HOST=192.168.100.114 pytest`). |
| Serial-trace build | ✅ | `[env:d1_mini_debug]` = d1_mini + `-DCC1101_TRACE`; `DBG()` no-op otherwise. Both envs build. |
| DSP module + Python port | ✅ | `src/decode.{h,cpp}` — clustering, encoding classifier, candidate-bit extraction, Linear/MegaCode/EV1527 recognizers, histogram; `main.cpp` is JSON glue over it. `tools/rfdecode.py` is a line-for-line Python port with 22 pytest cases; a device test cross-checks the two. |
| Host API client `tools/rfprobe.py` | ✅ | Stdlib CLI: status/selftest/tune/sweep/record/histogram/pulses/decode/samples/gate/fire/`sample …`/watch/raw; `--json`, `--timeout`. |
| `tests/` pytest suite | ✅ | Always-run pure tests (`rfdecode.py` port + `rfprobe` CLI vs. an in-process fake device) plus device tests that need `CC1101_HOST`. The device set passed 31/31 against the flashed board on 2026-08-31. |
| `/api/capture/pulses` | ✅ | Raw pulse list endpoint, chunk-streamed so a full 3072-pulse buffer can't exhaust the heap. |
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
| Linear 10-bit recognizer | ⚠️ | Implemented + Python-port-tested; not yet validated against a real remote |
| MegaCode 24-bit recognizer | ⚠️ | Implemented + Python-port-tested (synthetic frame); not yet confirmed on a live remote |
| EV1527 / PT2262 recognizer | ⚠️ | Implemented + Python-port-tested; self-calibrates Te from the sync gap; not yet validated on a live remote |
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

- **Wi-Fi credentials** live only in git-ignored `secrets.ini` (via
  `CFG_WIFI_*` build defines). Git history was scrubbed on 2026-09-01 (early
  commits squashed into "Initial import", reflog expired + `gc`d, doc prose
  cleaned) *before* the repo went public — a full object scan finds no
  credential strings. **Residual risk:** the station password sat on the build
  machine pre-scrub; rotate it on the router.
- `main.cpp` is still large (~1900 lines) though the decode kernels are split
  into `decode.{h,cpp}`; a further `sample.*` / `gate.*` split is a TODO.
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
