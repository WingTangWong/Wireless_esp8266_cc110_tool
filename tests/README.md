# Tests

```bash
python -m pip install -r requirements-dev.txt

pytest                                  # pure tests only; device tests skip
CC1101_HOST=cc1101.local pytest         # + the main unit's on-device checks
CC1101_REMOTE_HOST=192.168.4.23 \
CC1101_HOST=192.168.4.1 pytest          # + the Wi-Fi remote, incl. the
                                        #   remote-trigger end-to-end check
```

The remote joins the main unit's SoftAP, so run the remote checks from a host
on that SoftAP (main unit = `192.168.4.1`, remote = its DHCP lease).

Groups:

- **`test_rfdecode.py`** — unit tests for `tools/rfdecode.py`, the Python port
  of `src/decode.cpp` (clustering, encoding classifier, candidate bits,
  histogram, the Linear / MegaCode / EV1527 recognizers). Always run; no device
  or MCU toolchain needed. This is how the decode logic is regression-tested —
  the firmware itself only has an ESP8266 build.
- **`test_rfprobe_cli.py`** — drives `tools/rfprobe.py` against an in-process
  fake device (`_fakedevice.py`). Always run.
- **`test_device.py`** — drives the main unit's HTTP API on a real, flashed,
  networked D1 Mini. **Skipped** unless `CC1101_HOST` is set and the device
  answers `GET /api/selftest`.
- **`test_remote.py`** — the Wi-Fi remote (`src/remote/`): pairing, `/api/remote/
  status` shape, `mainReachable`, and a `POST /api/remote/press` that must
  propagate to the main unit's `/api/gate/fire`. **Skipped** unless
  `CC1101_REMOTE_HOST` is set (the end-to-end press test also needs
  `CC1101_HOST`).

Some tests briefly transmit nothing but do start real captures and sweeps on the
configured band; run them somewhere that is legal and safe to receive on
~318 MHz.

`tools/rfprobe.py` is the same API client as a standalone CLI — see its
`--help`.
