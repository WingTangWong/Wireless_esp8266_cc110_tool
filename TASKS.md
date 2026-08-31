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
- [ ] Add a short "authorized use only" disclaimer to the dashboard footer
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
- [ ] `platformio.ini` `[env:native]` + Unity tests for the pure helpers
      (`kmeans2All`/`kmeans2Level`, `nearDuration`, `safeName`, the Linear /
      MegaCode recognizers fed synthetic pulse arrays). Needs the DSP helpers
      split into a header includable without the Arduino/radio stack — see the
      "extract pure DSP" task below.

### Follow-on tasks found while building the harness

- [ ] **Extract the pure DSP helpers** (`Clusters`/`kmeans2*`, `nearDuration`,
      `tryLinear`, `tryMegaCode`, `pulseHistogramJson` math) into a
      `src/decode.h` / `.cpp` that compiles for `native` with no Arduino
      dependency, so they can be unit-tested and so `main.cpp` shrinks. Prereq
      for the `[env:native]` item above and the P3 module split.
- [x] **Derive `FIRMWARE_VERSION` from git** — `scripts/version.py`
      (`pre:` extra_script) injects `git describe --tags --always --dirty` as
      `CFG_FW_VERSION`, fallback `0.1.0-nogit`.
- [x] **Validate the SoftAP password at build time** — `static_assert` in
      `main.cpp`: `CFG_WIFI_AP_PASS` must be empty or ≥ 8 chars.
- [ ] **CI** (GitHub Actions): `pio run` for `d1_mini` (+ `native` once it
      exists), `ruff`/`python -m py_compile` on `tools/` + `tests/`, and
      `pytest` (skips with no device). Cache `~/.platformio`.
- [ ] **Surface the new status fields in the dashboard** — show firmware
      version/build and LittleFS used/total in the header or an "info" line;
      add a "Self-test" button that calls `/api/selftest`.
- [ ] **`rfprobe.py` niceties** — `--timeout` global flag, a `watch` mode that
      re-polls `status`, and `sample play/save/delete` subcommands for
      completeness.
- [ ] Consider a **`tests/test_helpers.py`** (pure-Python reimplementation of
      the pulse math) as a cross-check against the firmware's numbers during
      the on-device `decode` test.

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
  - [ ] Follow-up: optional PIN / basic-auth on `/gate*`, or a config flag to
        disable the operator page entirely.
  - [ ] Follow-up: report load failures from `fireGate()` back to the caller
        (currently `/api/gate/fire` answers `ok` before the fire runs).
  - [ ] Follow-up: "use the sample's captured frequency" one-click default and a
        visible forced-power notice on `/gate/config` (notice done; default not).

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
  - [ ] Follow-up: captive-portal redirect on the SoftAP (DNS catch-all → the
        dashboard) so the phone's "sign in to network" prompt opens it.
  - [ ] Follow-up: make the SoftAP SSID unique per device (append chip ID) and
        move both passwords out of source with the P0 work.
- [ ] Auto-pick the strongest sweep peak and offer "tune here" in the UI.
- [ ] Flag capture truncation in the UI when `pulseCount` hits `MAX_PULSES`.
- [ ] HTTP download of a saved `.bin` sample (and upload) for off-device analysis.
- [ ] Export capture as a Flipper `.sub` file or a raw µs list.
- [ ] Add more protocol recognizers (Princeton PT2262/EV1527, Hs2303, Nice
      FLO, CAME) — the generic candidate-bits output is the starting point.
- [ ] Optional rolling-code detection / warning (KeeLoq-style) so the user
      knows a fixed replay will not work.
- [ ] Per-sample notes / rename in the UI.
- [ ] Show decoded frequency offset from the sweep vs. the configured target.

## P3 – Code quality

- [ ] Split `main.cpp` into modules (radio, capture, decode, web, storage).
- [ ] Replace hand-built JSON strings with a small builder or ArduinoJson.
- [ ] Remove the unused `mfurga/CC1101` dependency from `platformio.ini`, or
      integrate it and document why both libraries are needed.
- [ ] Add an optional debug build (`-D DEBUG_SERIAL`) with serial tracing of
      capture / decode steps.
- [ ] Guard against `server.arg()` parsing surprises (empty / non-numeric).
- [ ] Consider `ESP8266WebServer` chunked responses for `/api/samples` and
      `/api/sweep/data` to avoid large `String` allocations on low heap.

## P4 – Docs / project

- [ ] Wiring photo / diagram in `README.md`.
- [ ] Screenshot of the web UI.
- [ ] Document the sample `.bin` format in its own short spec file.
- [ ] Note tested CC1101 board variant(s) and antenna used.
