"""Shared fixtures for the on-device HTTP API tests.

These tests only run when a real, flashed, networked module is reachable:

    CC1101_HOST=cc1101.local pytest                 # main unit -> test_device.py
    CC1101_REMOTE_HOST=192.168.4.23 pytest          # remote    -> test_remote.py

The remote joins the main unit's SoftAP, so the remote-trigger healthcheck
(both vars set) is run from a host on that SoftAP, with CC1101_HOST=192.168.4.1.
Anything without its host var set is skipped, not failed.
"""

import os
import pathlib
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))

import rfprobe

HOST = os.environ.get("CC1101_HOST")
REMOTE_HOST = os.environ.get("CC1101_REMOTE_HOST")


def device_reachable(host):
    try:
        d = rfprobe.call(host, "/api/selftest", timeout=4.0)
        return bool(d.get("firmwareVersion"))
    except rfprobe.ApiError:
        return False


@pytest.fixture(scope="session")
def host():
    if not HOST:
        pytest.skip("CC1101_HOST not set; skipping on-device tests")
    if not device_reachable(HOST):
        pytest.skip(f"device at {HOST} did not answer /api/selftest")
    return HOST


class _Api:
    """Callable API client that also exposes .host and a raw request helper."""

    def __init__(self, host):
        self.host = host

    def __call__(self, path, params=None, method="GET", timeout=15.0):
        return rfprobe.call(self.host, path, params=params, method=method, timeout=timeout)

    def raw(self, path, method="GET", timeout=15.0):
        """Return the parsed JSON body regardless of the ok flag (rfprobe.call
        raises on ok:false; this doesn't)."""
        import json
        import urllib.request
        req = urllib.request.Request(f"http://{self.host}{path}", method=method)
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8", "replace"))


@pytest.fixture
def api(host):
    return _Api(host)


def remote_reachable(host):
    try:
        d = rfprobe.call(host, "/api/remote/status", timeout=4.0)
        return d.get("role") == "remote"
    except rfprobe.ApiError:
        return False


@pytest.fixture(scope="session")
def remote_host():
    if not REMOTE_HOST:
        pytest.skip("CC1101_REMOTE_HOST not set; skipping remote tests")
    if not remote_reachable(REMOTE_HOST):
        pytest.skip(f"remote at {REMOTE_HOST} did not answer /api/remote/status")
    return REMOTE_HOST


@pytest.fixture
def remote_api(remote_host):
    return _Api(remote_host)


@pytest.fixture(autouse=True)
def _require_idle(request):
    """Fail fast if a prior test left the radio busy."""
    if "api" not in request.fixturenames:
        return
    api = request.getfixturevalue("api")
    st = api("/api/status")
    if st.get("busy"):
        rfprobe._wait_idle(request.getfixturevalue("host"), 15.0)
