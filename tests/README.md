# Tests

```bash
python -m pip install -r requirements-dev.txt

pytest                                  # runs the pure tests, skips device tests
CC1101_HOST=cc1101.local pytest         # + the on-device checks (or an IP)
```

Two groups:

- **`test_rfdecode.py`** — unit tests for `tools/rfdecode.py`, the Python port
  of `src/decode.cpp` (clustering, encoding classifier, candidate bits,
  histogram, the Linear / MegaCode / EV1527 recognizers). Always run; no device
  or MCU toolchain needed. This is how the decode logic is regression-tested —
  the firmware itself only has an ESP8266 build.
- **`test_rfprobe_cli.py`** — drives `tools/rfprobe.py` against an in-process
  fake device (`_fakedevice.py`). Always run.
- **`test_device.py`** — drives the firmware's HTTP API on a real, flashed,
  networked D1 Mini. **Skipped entirely** unless `CC1101_HOST` is set and the
  device answers `GET /api/selftest`.

Some tests briefly transmit nothing but do start real captures and sweeps on the
configured band; run them somewhere that is legal and safe to receive on
~318 MHz.

`tools/rfprobe.py` is the same API client as a standalone CLI — see its
`--help`.
