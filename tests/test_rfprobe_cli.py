"""Exercise tools/rfprobe.py end to end against an in-process fake device.
Pure - always runs, no hardware."""

import json
import pathlib
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))

import rfprobe  # noqa: E402
from _fakedevice import FakeDevice  # noqa: E402


@pytest.fixture
def dev():
    with FakeDevice() as d:
        yield d


def run(dev, *argv):
    rfprobe.main(["--host", dev.host, *argv])


def test_status_and_selftest(dev, capsys):
    run(dev, "status")
    assert "fw 0.0-test" in capsys.readouterr().out
    run(dev, "selftest")
    assert "PASS" in capsys.readouterr().out


def test_json_flag_emits_valid_json(dev, capsys):
    run(dev, "--json", "status")
    obj = json.loads(capsys.readouterr().out)
    assert obj["radio"] is True


def test_tune_and_sweep(dev, capsys):
    run(dev, "tune", "317.5")
    assert "317.500000" in capsys.readouterr().out
    run(dev, "sweep", "317.8", "318.2", "--step", "20")
    out = capsys.readouterr().out
    assert "points" in out and "peak=" in out


def test_record_decode(dev, capsys):
    run(dev, "record", "300", "--decode")
    out = capsys.readouterr().out
    assert "captured 16 pulses" in out
    assert "pulse-distance" in out


def test_sample_lifecycle(dev, capsys):
    run(dev, "sample", "save", "unit_test")
    run(dev, "sample", "list")
    assert "unit_test" in capsys.readouterr().out
    run(dev, "sample", "play", "unit_test", "--repeat", "3")
    assert "x3" in capsys.readouterr().out
    run(dev, "sample", "delete", "unit_test")


def test_sample_play_needs_name(dev):
    with pytest.raises(SystemExit):
        run(dev, "sample", "play")


def test_unreachable_host_exits_2():
    with pytest.raises(SystemExit) as e:
        rfprobe.main(["--host", "127.0.0.1:1", "--timeout", "1", "status"])
    assert e.value.code == 2
