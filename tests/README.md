# On-device tests

`tests/test_device.py` drives the firmware's HTTP API on a real, flashed,
networked D1 Mini and checks the JSON responses. It is **skipped entirely**
unless a device is reachable.

```bash
python -m pip install -r requirements-dev.txt

# no device -> everything skips
pytest

# with a device
CC1101_HOST=cc1101.local pytest        # or an IP, e.g. 192.168.4.1
```

The suite gates on `GET /api/selftest`: if `CC1101_HOST` is unset or the device
does not answer, all tests skip rather than fail.

Some tests briefly transmit nothing but do start real captures and sweeps on the
configured band; run them somewhere that is legal and safe to receive on
~318 MHz.

`tools/rfprobe.py` is the same API client as a standalone CLI — see its
`--help`.
