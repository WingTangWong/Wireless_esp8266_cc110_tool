"""Exercise tools/rfprobe.py end to end against an in-process fake device.
Pure - always runs, no hardware."""

import json
import pathlib
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))

import rfprobe
from _fakedevice import FakeDevice


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


def test_export_sub_and_csv(dev, tmp_path, capsys):
    sub = tmp_path / "cap.sub"
    run(dev, "export", str(sub), "--sample", "sample01")
    text = sub.read_text()
    assert text.startswith("Filetype: Flipper SubGhz RAW File")
    assert "Protocol: RAW" in text
    assert "RAW_Data: " in text
    # fake device returns 16 signed values on one line
    raw_line = next(ln for ln in text.splitlines() if ln.startswith("RAW_Data:"))
    vals = raw_line.split(":", 1)[1].split()
    assert len(vals) == 16
    assert any(int(v) > 0 for v in vals) and any(int(v) < 0 for v in vals)

    csv = tmp_path / "cap.csv"
    run(dev, "export", str(csv))
    assert csv.read_text().splitlines()[0] == "duration_us,level"


def test_unreachable_host_exits_2():
    with pytest.raises(SystemExit) as e:
        rfprobe.main(["--host", "127.0.0.1:1", "--timeout", "1", "status"])
    assert e.value.code == 2
