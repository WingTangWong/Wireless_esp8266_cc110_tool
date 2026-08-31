"""Shared fixtures for the on-device HTTP API tests.

These tests only run when a real, flashed, networked module is reachable.
Set the host and run:

    CC1101_HOST=cc1101.local pytest tests/

Without CC1101_HOST set, or if the device does not answer /api/selftest,
every test in tests/test_device.py is skipped.
"""

import os
import sys
import pathlib

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))

import rfprobe  # noqa: E402

HOST = os.environ.get("CC1101_HOST")


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


@pytest.fixture
def api(host):
    def _call(path, params=None, method="GET", timeout=15.0):
        return rfprobe.call(host, path, params=params, method=method, timeout=timeout)
    return _call


@pytest.fixture(autouse=True)
def _require_idle(request):
    """Fail fast if a prior test left the radio busy."""
    if "api" not in request.fixturenames:
        return
    api = request.getfixturevalue("api")
    st = api("/api/status")
    if st.get("busy"):
        rfprobe._wait_idle(request.getfixturevalue("host"), 15.0)
