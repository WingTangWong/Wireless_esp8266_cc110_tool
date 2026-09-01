# Tasks

Backlog for the ESP8266 + CC1101 318 MHz tool. Roughly priority-ordered within
each section. Check items off as they land; move notes to `PROGRESS.md`.

## Working agreement (git)

- `main` stays clean and buildable. No direct commits to `main`.
- One branch per unit of work, branched from an up-to-date `main`:
  - `feat/<slug>` – new features
  - `fix/<slug>` – bug fixes
  - `docs/<slug>` – documentation only
  - `chore/<slug>` – deps, tooling, refactors
- Keep the working tree tidy: don't commit build output (`.pio/` is
  git-ignored), editor files, or secrets. Commit `platformio.ini` lockstep with
  code that needs the new dep.
- Small, focused commits with imperative messages ("Add MegaCode field decode",
  not "updates").
- Merge back via PR (or a reviewed fast-forward) once the branch builds and any
  relevant hardware checklist item in `PROGRESS.md` is ticked.
- Delete the branch after merge.

## P0 – Security / hygiene

- [x] Move `WIFI_SSID` / `WIFI_PASS` (and the SoftAP SSID/pass) out of
      `src/main.cpp`. Done via git-ignored `secrets.ini` → `${wifi.*}` →
      `-DCFG_WIFI_*` build defines in `platformio.ini`; `secrets.ini.example`
      committed as the template. Source keeps `#ifndef` placeholder fallbacks so
      it still builds with no `secrets.ini` (SoftAP-only).
- [x] Add `secrets.ini` / `*.local.ini` to `.gitignore`.
- [x] Rotate the SoftAP password (was `<redacted>`, a committed constant) — new
      value lives only in `secrets.ini`.
- [ ] **Rotate the actual station Wi-Fi password on the router** — the old one
      (`<redacted>` / `<redacted>`) is still readable in git history; moving it
      out of `HEAD` does not un-leak it.
- [ ] Scrub git history of the old credentials (`git filter-repo` / BFG on the
      strings) if this repo will ever be published. Rewrites all commit hashes.
- [ ] Consider a first-boot captive portal (WiFiManager) so a fresh unit needs
      no `secrets.ini` at all — the reusable end state. Bigger change; separate
      from this pass.
- [x] Add a short "authorized use only" disclaimer to the dashboard footer
      (the `/gate` page already has one).

## P1 – Core functionality verification

### Device test & measurement harness

Goal: flash a real D1 Mini, drive its HTTP API from a host, and pull JSON back
to verify the firmware actually works — plus an automated test suite that runs
when (and only when) a device is reachable.

Implemented in `feat/device-test-harness` (firmware builds; CLI + suite verified
against a mock device — 14 tests). Hardware run still pending.

- [x] Document `pio run -t upload` to the Wemos D1 Mini (port autodetect,
      `--upload-port`, `upload_speed`, boot log at 74880) — `README.md`.
- [x] `tools/rfprobe.py` — stdlib-only CLI: `status`, `selftest`, `tune`,
      `sweep` (waits for idle, dumps `summary`), `record` (`--histogram`
      `--decode`), `histogram`, `decode`, `samples`, `gate`, `fire`, `raw`;
      `--json` for piping to `jq`.
- [x] Verification fields: `/api/status` gains `firmwareVersion`,
      `firmwareBuild`, `radioPartnum`, `radioVersion`, `fsUsedBytes`,
      `fsTotalBytes`, `busy`; `/api/sweep/data` gains `count` + `summary`
      (`rssiMin/Max/MeanDbm`, `peakHz`).
- [x] `/api/selftest` (GET, JSON) — `{ok, checks:{spi,radioVersionSane,
      littlefs,heap,network}, radioPartnum, radioVersion, heap, fs*}`.
- [x] `tests/test_device.py` (14 tests) + `tests/conftest.py` — auto-skip
      unless `CC1101_HOST` is set and `/api/selftest` answers. Covers selftest,
      status schema + radio-detected, FS sanity, tune round-trip + out-of-band
      reject, sweep point count / RSSI range / summary, capture mode cycle,
      histogram bin sums, samples + gate schema. `pytest.ini`,
      `requirements-dev.txt`, `tests/README.md` added.
- [x] README + `PROGRESS.md` updated with how to run it.
- [ ] Run it against real hardware and tick the `PROGRESS.md` checklist rows.
- [x] `platformio.ini` `[env:native]` + Unity tests — `test/test_decode/`,
      12 tests over `kmeans2*`, `nearDuration`, `classifyEncoding`,
      `tryMegaCode` (synthetic frame round-trip + noise reject), `histogram`.
      `pio test -e native`, wired into CI.
  - [ ] Add `tryLinear` positive coverage (needs a hand-built valid frame or a
        recorded sample) and `safeName` once it also moves out of `main.cpp`.

### Follow-on tasks found while building the harness

- [x] **Extract the pure DSP helpers** into `src/decode.{h,cpp}` (namespace
      `rfd`, Arduino-free): `Clusters`/`kmeans2All`/`kmeans2Level`,
      `nearDuration`, `classifyEncoding`+`encodingName`, `tryLinear`,
      `tryMegaCode`, `histogram`. `main.cpp` now passes a `PulseSpan` over the
      volatile buffers and only does JSON. `ProtocolDecode` uses fixed char
      buffers instead of `String`.
  - [x] Candidate-bits extraction moved into `rfd::candidateBits`
        (`BitExtraction` result), with 4 Unity tests (PPM/PWM bit strings,
        burst separator, 220-char cap). `decodeCurrentJson` holds it in a
        function-local `static` to keep ~450 bytes off the handler stack.
- [x] **Derive `FIRMWARE_VERSION` from git** — `scripts/version.py`
      (`pre:` extra_script) injects `git describe --tags --always --dirty` as
      `CFG_FW_VERSION`, fallback `0.1.0-nogit`.
- [x] **Validate the SoftAP password at build time** — `static_assert` in
      `main.cpp`: `CFG_WIFI_AP_PASS` must be empty or ≥ 8 chars.
- [x] **CI** (`.github/workflows/ci.yml`): builds `d1_mini` (no `secrets.ini`,
      exercising the placeholder path), `compileall` on `tools/`+`tests/`+
      `scripts/`, and `pytest` (device tests skip). PlatformIO/pip cached.
  - [x] Add `pio test -e native` to CI.
  - [x] `ruff` lint step — `ruff.toml` (E/F/I/UP/RUF, len 110), `ruff check .`
        in CI, `ruff>=0.6` in `requirements-dev.txt`. Tree is clean.
- [x] **Surface the new status fields in the dashboard** — an info line shows
      fw version/build, CC1101 version, LittleFS used/total; a "Self-test"
      button calls `/api/selftest` and shows PASS / failed-check names.
- [x] **`tools/rfdecode.py` + `tests/test_rfdecode.py`** — pure-Python port of
      `kmeans2*` and the encoding classifier, unit-tested on synthetic pulse
      trains, and cross-checked against the device in
      `test_decode_matches_python_port` (via the new `/api/capture/pulses`).
  - [ ] Port the bit-extraction and the Linear / MegaCode recognizers to
        `rfdecode.py` too (EV1527 is ported), so the device cross-check can
        cover `protocol_candidate` / `protocol_bits`.
- [x] `rfprobe.py`: `sample {list,save,load,decode,delete,play}` subcommands,
      global `--timeout`, `watch` mode. `tests/test_rfprobe_cli.py` +
      `tests/_fakedevice.py` exercise the CLI in-process (7 tests, always run).

### Real-signal verification

- [ ] Capture a real 318 MHz MegaCode remote; confirm the ISR yields clean
      ~1 ms pulses in ~6 ms frames.
- [ ] Validate the MegaCode 24-bit recognizer against that capture
      (facility / serial / button fields).
- [ ] Validate the Linear 10-bit recognizer against a real Linear remote.
- [ ] End-to-end replay test: saved sample actuates the target device.
- [ ] Tune replay TX power / antenna for reliable range.

## P2 – Features

### Gate control page + assignment page

Goal: a dead-simple operator page, separate from the analysis dashboard, with
two big buttons — **Inner gate** and **Outer gate** — that each replay a
previously captured sample. Which sample (and with what radio settings) each
button fires is set on a separate assignment/config page.

Implemented in `feat/gate-control` (builds clean, RAM 62.9% / Flash 39.0%);
on-hardware verification still pending — see `PROGRESS.md`.

- [x] **Operator page** `GET /gate` — two big touch buttons, per-button assigned
  sample name, fired/busy/error toast, buttons disabled unless the assignment is
  ready, polls `/api/gate` every 4 s, links to `/gate/config` and `/`. Served on
  both STA and SoftAP.
- [x] **Assignment page** `GET /gate/config` — sample dropdown from
  `/api/samples`, editable frequency / bandwidth / repeats / invert per gate,
  "Save" and "Save & test fire".
- [x] **Persist assignments** to LittleFS `/gate.bin` — packed `GateConfigFile`
  (`"GATE"` v1) with a `GateAssignment` per button (`sampleName[24]`,
  `frequencyHz`, `bandwidthKhz`, `txPowerDbm`, `repeats`, `invert`). Loaded on
  boot via `gateLoadConfig()`; missing file / deleted sample ⇒ button unassigned
  and `fire` errors instead of transmitting.
- [x] **API** — `GET /api/gate`, `POST /api/gate/assign` (validates `which`,
  sample exists, freq 300–348 MHz, bw ∈ {58,270,650}, repeats 1–10; empty
  `sampleName` clears the button), `POST /api/gate/fire?which=inner|outer`
  (fire-and-forget via a new `gateFireRequested` path in `loop()`).
- [x] **Under-the-hood send settings** — `fireGate()` re-applies the
  assignment's frequency/bandwidth through `configureRawOok()` +
  `setRadioFrequency()`, clamps `txPowerDbm` **up to +12 dBm** on every fire
  (mismatched antenna / marginal matching — deliberate; regulatory limits are
  the operator's problem), floors `repeats` at `GATE_MIN_REPEATS` (4), and
  restores the dashboard's target frequency + RX state afterward.
- [x] **Docs** — `/gate`, `/gate/config`, `/api/gate*` documented in
  `README.md`; status + risks in `PROGRESS.md`; "authorized use only" line on
  the `/gate` page.
- [~] **Security / footgun review** — POST endpoints now reject cross-origin
  requests (`Origin` host vs `Host`). Still **no authentication**: anyone on the
  LAN/SoftAP can fire a gate.
  - [x] Runtime enable/disable toggle — `gateEnabled` in `gate.bin` (v2),
        `POST /api/gate/enable?on=0|1`, checkbox on `/gate/config`; `fire`
        returns 403 when off; the operator page shows the disabled state.
  - [x] `fireGate()` load failures now set `gateLastFireError`, surfaced on
        `/api/gate` and shown on the operator page's next poll.
  - [x] Picking a sample on `/gate/config` auto-fills the frequency from that
        sample's capture.
  - [ ] Still open: an actual secret (PIN / basic-auth) on `/gate*` for when the
        network itself is not trusted.

### Other P2 items

- [x] Run AP + STA concurrently (`WIFI_AP_STA`): keep joining the configured
      home AP, but also always bring up the device's own SoftAP so a phone can
      connect directly and reach the web portal/dashboard. Serve the same
      `ESP8266WebServer` on both interfaces; mDNS on the STA side, a fixed AP IP
      (e.g. `192.168.4.1`) on the AP side. Give the SoftAP a WPA2 password
      (configurable, not blank) and show the AP SSID/IP/client count on
      `/api/status` and in the UI. Ties in with the P0 credential work — the
      SoftAP is also the natural place for first-boot provisioning.
      _Done in `feat/softap-concurrent` (builds clean); on-hardware check still
      pending — see `PROGRESS.md`._
  - [x] Captive portal — `DNSServer` catch-all on the AP + a 302 in
        `onNotFound` for AP clients (`client().localIP() == WIFI_AP_IP`),
        so the "sign in to network" sheet opens the dashboard.
  - [x] SoftAP SSID is `<ap_ssid>-<chipId>` (`apSsidEffective`), reported on
        `/api/status`. (AP password out of source: done in the P0 pass.)
- [x] After a sweep the UI shows the strongest bin, its offset in kHz vs the
      configured target, and a "Tune to peak" button.
- [x] Flag capture truncation — `captureTruncated` + `maxPulses` on
      `/api/status`, `truncated` on `/api/capture/pulses`, and the dashboard
      capture line says "TRUNCATED at N pulses".
- [x] Export capture as a Flipper `.sub` file / CSV / signed-µs text —
      `rfprobe.py export <out> [--sample NAME]` (loads the sample, pulls
      `/api/capture/pulses`, writes the chosen format).
- [ ] Still open: raw `.bin` upload back to the device, and download straight
      from the browser (rfprobe covers the host path).
- [x] EV1527 / PT2262 24-bit recognizer (`rfd::tryEV1527`, self-calibrating Te
      from the sync gap; 3 Unity tests + a Python port in `rfdecode.py`).
- [ ] More recognizers: Hs2303, Nice FLO, CAME, Holtek HT6P20 — same pattern
      (add to `decode.cpp`, Unity-test, wire into the `specific` chain).
- [ ] Optional rolling-code detection / warning (KeeLoq-style) so the user
      knows a fixed replay will not work.
- [ ] Per-sample notes / rename in the UI.
- [x] Show the sweep-peak frequency offset vs. the configured target (done with
      the "Tune to peak" item above).

## P3 – Code quality

- [ ] Split `main.cpp` into modules (radio, capture, web, storage). **`decode`
      is done** — `src/decode.{h,cpp}`. Next candidates: `sample.{h,cpp}`
      (LittleFS `315R` format + `safeName`/`samplePath`), `gate.{h,cpp}`.
- [ ] Replace hand-built JSON strings with a small builder or ArduinoJson.
- [x] Removed the unused `mfurga/CC1101` dependency from `platformio.ini`
      (only `SmartRC_CC1101.h` was ever included).
- [x] Optional serial-trace build — `[env:d1_mini_debug]` adds `-DCC1101_TRACE`;
      `DBG(...)` macro is a no-op otherwise. Traces boot, radio id, Wi-Fi/AP,
      capture start/stop, gate fire. (Name is `CC1101_TRACE`, not `DEBUG_SERIAL`
      — that identifier is used inside the ESP8266 core.)
- [ ] Guard against `server.arg()` parsing surprises (empty / non-numeric).
- [x] `/api/samples` is chunk-streamed now (one sample row at a time);
      `/api/capture/pulses` too. `/api/sweep/data` still builds one String
      (bounded at 601 points → ~15 KB; fine but could stream too).

## P4 – Docs / project

- [ ] Wiring photo / diagram in `README.md`.
- [ ] Screenshot of the web UI.
- [x] Document the sample `.bin` format — `docs/sample-format.md`.
- [ ] Note tested CC1101 board variant(s) and antenna used.
