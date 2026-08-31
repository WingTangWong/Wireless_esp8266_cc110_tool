"""Unit tests for tools/rfdecode.py (pure Python, always run - no device)."""

import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))

import rfdecode  # noqa: E402


def _ppm(bits, short=500, long=1500, gap=6000):
    """HIGH pulse then a LOW whose length encodes the bit (pulse-distance)."""
    out = []
    for b in bits:
        out.append((short, 1))
        out.append((long if b else short, 0))
    out.append((gap, 0))
    return out


def _pwm(bits, short=500, long=1500, lowgap=500):
    """HIGH length encodes the bit, LOW is constant (pulse-width)."""
    out = []
    for b in bits:
        out.append((long if b else short, 1))
        out.append((lowgap, 0))
    return out


def test_kmeans_separates_two_obvious_clusters():
    pulses = [(500, 1), (520, 0), (1500, 1), (1480, 0)] * 5
    c = rfdecode.kmeans2_all(pulses)
    assert 480 <= c.short_us <= 540
    assert 1450 <= c.long_us <= 1520
    assert c.count == 20


def test_kmeans_filters_out_of_range():
    pulses = [(10, 1), (500, 1), (1500, 0), (99999, 0)]
    c = rfdecode.kmeans2_all(pulses)
    assert c.count == 2  # 10 and 99999 dropped


def test_classify_pulse_distance():
    d = rfdecode.analyze(_ppm([1, 0, 1, 1, 0, 0, 1, 0]))
    assert d["encoding"] == "pulse-distance / PPM-style"
    assert d["ratio"] > 1.7


def test_classify_pulse_width():
    d = rfdecode.analyze(_pwm([1, 0, 1, 0, 1, 1, 0, 0]))
    assert d["encoding"] == "pulse-width / PWM-style"


def test_classify_unknown_on_uniform_timing():
    pulses = [(500, 1), (500, 0)] * 12
    assert rfdecode.analyze(pulses)["encoding"] == "unknown / mixed timing"


def test_empty_input_is_safe():
    c = rfdecode.kmeans2_all([])
    assert c == rfdecode.Clusters()
