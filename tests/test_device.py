"""On-device HTTP API checks. Auto-skipped unless CC1101_HOST points at a
reachable, flashed module. See conftest.py."""


import pytest


def _is_number(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


# --- /api/selftest ---------------------------------------------------------

def test_selftest_passes(api):
    d = api("/api/selftest")
    assert d["ok"] is True, f"selftest failed: {d.get('checks')}"
    for name, value in d["checks"].items():
        assert value is True, f"check {name!r} failed"


def test_selftest_reports_radio_identity(api):
    d = api("/api/selftest")
    # CC1101 VERSION register is commonly 0x14; never 0x00 / 0xFF on a live part.
    assert 0 < d["radioVersion"] < 255
    assert _is_number(d["radioPartnum"])


# --- /api/status ---------------------------------------------------------

STATUS_FIELDS = {
    "ok": bool, "radio": bool, "firmwareVersion": str, "firmwareBuild": str,
    "radioPartnum": (int,), "radioVersion": (int,), "mode": str,
    "frequencyHz": (int,), "bandwidthKhz": (int,), "heap": (int,),
    "fsUsedBytes": (int,), "fsTotalBytes": (int,), "pulses": (int,),
    "captureDone": bool, "captureTruncated": bool, "maxPulses": (int,),
    "sweepDone": bool, "busy": bool,
}


def test_status_schema(api):
    d = api("/api/status")
    for key, typ in STATUS_FIELDS.items():
        assert key in d, f"missing {key}"
        if typ is bool:
            assert isinstance(d[key], bool), key
        elif typ is str:
            assert isinstance(d[key], str), key
        else:
            assert _is_number(d[key]), key


def test_status_radio_detected(api):
    assert api("/api/status")["radio"] is True


def test_status_filesystem_sane(api):
    d = api("/api/status")
    assert d["fsTotalBytes"] > 0
    assert 0 <= d["fsUsedBytes"] <= d["fsTotalBytes"]


# --- /api/tune ---------------------------------------------------------

@pytest.mark.parametrize("mhz", [317.5, 318.0, 318.25])
def test_tune_round_trips(api, mhz):
    hz = round(mhz * 1e6)
    d = api("/api/tune", {"hz": hz, "bw": 650})
    assert abs(d["frequencyHz"] - hz) <= 1000
    assert abs(api("/api/status")["frequencyHz"] - hz) <= 1000


def test_tune_rejects_out_of_band(api):
    import rfprobe
    with pytest.raises(rfprobe.ApiError):
        api("/api/tune", {"hz": 250_000_000, "bw": 650})


# --- /api/sweep ---------------------------------------------------------

def test_sweep_returns_points_in_range(api, host):
    import rfprobe
    start, stop, step = 317_800_000, 318_200_000, 20_000
    api("/api/sweep/start",
        {"start": start, "stop": stop, "step": step, "dwell": 3, "bw": 650})
    rfprobe._wait_idle(host, 30.0)
    d = api("/api/sweep/data")

    expected = (stop - start) // step + 1
    assert d["count"] == expected == len(d["points"])
    for pt in d["points"]:
        assert start <= pt["hz"] <= stop
        assert -128 <= pt["rssi"] <= 0
    summ = d["summary"]
    assert summ["rssiMinDbm"] <= summ["rssiMaxDbm"]
    assert start <= summ["peakHz"] <= stop


# --- /api/capture ---------------------------------------------------------

def test_capture_cycle_moves_through_modes(api, host):
    import rfprobe
    api("/api/capture/start", {"ms": 800})
    mid = api("/api/status")["mode"]
    assert mid in ("recording", "idle")  # may already be done on a very quiet band
    rfprobe._wait_idle(host, 10.0)
    st = api("/api/status")
    assert st["mode"] == "idle"
    assert st["busy"] is False


def test_histogram_bins_sum_to_pulse_count(api, host):
    import rfprobe
    api("/api/capture/start", {"ms": 600})
    rfprobe._wait_idle(host, 8.0)
    pulses = api("/api/status")["pulses"]
    h = api("/api/capture/histogram")
    assert len(h["counts"]) == len(h["high"]) == len(h["low"]) == len(h["labels"])
    for i, total in enumerate(h["counts"]):
        assert total == h["high"][i] + h["low"][i]
    # histogram drops pulses longer than its window, so <=, not ==
    assert sum(h["counts"]) <= pulses or pulses == 0


# --- /api/samples & /api/gate ------------------------------------------

def test_samples_list_schema(api):
    d = api("/api/samples")
    assert isinstance(d["samples"], list)
    for s in d["samples"]:
        assert set(s) >= {"name", "frequencyHz", "pulseCount", "durationUs"}
        assert all(_is_number(s[k]) for k in ("frequencyHz", "pulseCount", "durationUs"))


def test_decode_matches_python_port(api, host):
    """Cross-check the firmware's clustering/encoding against tools/rfdecode.py
    over the same raw pulses. Skips if the current capture is too small."""
    import rfdecode
    import rfprobe

    api("/api/capture/start", {"ms": 1500})
    rfprobe._wait_idle(host, 12.0)
    raw = api("/api/capture/pulses")
    if raw["count"] < 8:
        pytest.skip("not enough pulses on the band to cross-check decode")

    dev = api("/api/decode/current")
    ours = rfdecode.analyze([(d, lvl) for d, lvl in raw["pulses"]])

    # same k-means, same inputs -> cluster centres within rounding noise
    assert abs(dev["short_us"] - ours["short_us"]) <= max(2.0, ours["short_us"] * 0.02)
    assert abs(dev["long_us"] - ours["long_us"]) <= max(2.0, ours["long_us"] * 0.02)
    assert dev["encoding"] == ours["encoding"]


def test_gate_status_schema(api):
    d = api("/api/gate")
    assert d["txPowerForcedDbm"] == 12
    assert d["minRepeats"] >= 1
    for which in ("inner", "outer"):
        a = d[which]
        assert set(a) >= {"assigned", "sampleName", "sampleExists", "frequencyHz",
                          "bandwidthKhz", "repeats", "invert", "ready"}
        assert isinstance(a["assigned"], bool)
