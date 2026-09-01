"""On-device checks for the Wi-Fi remote (src/remote/).

Auto-skipped unless CC1101_REMOTE_HOST points at a flashed, reachable remote
board. The end-to-end "remote trigger" test additionally needs CC1101_HOST
(the main unit) - run it from a host on the main unit's SoftAP. See conftest.py.
"""

import time

import pytest
import rfprobe

# --- /api/remote/status ------------------------------------------------

def test_remote_status_schema(remote_api):
    d = remote_api("/api/remote/status")
    assert d["ok"] is True
    assert d["role"] == "remote"
    for key in ("firmwareVersion", "linkUp", "joinedSsid", "mainIp",
                "mainReachable", "lastPollAgeMs", "main"):
        assert key in d, f"missing {key}"
    assert isinstance(d["linkUp"], bool)


def test_remote_is_linked_and_paired(remote_api):
    d = remote_api("/api/remote/status")
    assert d["linkUp"] is True, "remote has no Wi-Fi link"
    assert d["joinedSsid"], "remote did not record which SSID it joined"
    assert "cc1101" in d["joinedSsid"].lower(), "remote joined an unexpected SSID"


def test_remote_reaches_main(remote_api):
    d = remote_api("/api/remote/status")
    assert d["mainReachable"] is True, "remote cannot reach the main unit"
    assert 0 <= d["lastPollAgeMs"] < 8000, "remote's cached main status is stale"
    m = d["main"]
    assert m["ok"] is True
    assert isinstance(m["radio"], bool)
    assert m["mode"] in ("idle", "recording", "sweeping", "playback")
    assert m["firmwareVersion"]


# --- POST /api/remote/press: the remote -> main fire path --------------

def test_remote_press_rejects_bad_button(remote_api):
    with pytest.raises(rfprobe.ApiError):
        remote_api("/api/remote/press", {"which": "sideways"}, method="POST")


def test_remote_press_propagates_to_main(remote_api):
    """A press on the remote must reach the main unit's /api/gate/fire. Works
    whether or not a gate is assigned - an unassigned gate still proves the
    command traversed remote -> main."""
    d = remote_api.raw("/api/remote/press?which=inner", method="POST")
    assert d["via"] == "remote"
    assert d["which"] == "inner"
    assert "ok" in d["main"], "no wrapped main response"
    if d["main"]["ok"]:
        assert "sampleName" in d["main"]
    else:
        assert d["main"].get("error"), "expected an error string from the main unit"


# --- end-to-end: remote press moves the main unit --------------------

@pytest.mark.parametrize("which", ["inner", "outer"])
def test_remote_press_end_to_end(remote_api, api, which):
    ready = api("/api/gate")[which].get("ready")

    remote_api.raw(f"/api/remote/press?which={which}", method="POST")

    if ready:
        seen_busy = False
        for _ in range(25):
            if api("/api/status").get("busy"):
                seen_busy = True
                break
            time.sleep(0.2)
        rfprobe._wait_idle(api.host, 15.0)
        assert seen_busy, "gate is ready but the main unit never went busy after a remote press"
    else:
        assert api("/api/gate")["lastFireError"], "main did not record the failed remote fire"
