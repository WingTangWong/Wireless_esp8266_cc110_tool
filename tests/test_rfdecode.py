"""Unit tests for tools/rfdecode.py - the pure-Python port of src/decode.cpp.
Always run (no device, no MCU toolchain)."""

import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))

import rfdecode

# --- synthetic pulse-train builders (mirror test_decode.cpp) ------------

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


def _clusters(pulses):
    return (rfdecode.kmeans2_all(pulses),
            rfdecode.kmeans2_level(pulses, 1),
            rfdecode.kmeans2_level(pulses, 0))


def _megacode(code24):
    """13 ms header, 1 ms start bit, then per bit a LOW gap sized so te_last
    lands on the long/short target, then a 1 ms HIGH. Requires bit 23 set."""
    out = [(13000, 0), (1000, 1)]
    prev = 1
    for i in range(22, -1, -1):
        bit = (code24 >> i) & 1
        low = (5000 if bit else 2000) if prev else (8000 if bit else 5000)
        out += [(low, 0), (1000, 1)]
        prev = bit
    out.append((12000, 0))
    return out


def _ev1527(code24, te=350):
    out = [(te, 1), (te * 31, 0)]
    for i in range(23, -1, -1):
        out += ([(te * 3, 1), (te, 0)] if (code24 >> i) & 1 else [(te, 1), (te * 3, 0)])
    out.append((te * 31, 0))
    return out


# --- near --------------------------------------------------------------

def test_near():
    assert rfdecode.near(1000, 1000, 0)
    assert rfdecode.near(1200, 1000, 200)
    assert rfdecode.near(800, 1000, 200)
    assert not rfdecode.near(1201, 1000, 200)


# --- clustering ------------------------------------------------------

def test_kmeans_two_clusters():
    pulses = [(500, 1), (520, 0), (1500, 1), (1480, 0)] * 5
    c = rfdecode.kmeans2_all(pulses)
    assert 480 <= c.short_us <= 540
    assert 1450 <= c.long_us <= 1520
    assert c.count == 20
    assert c.short_us < c.long_us


def test_kmeans_filters_range():
    c = rfdecode.kmeans2_all([(10, 1), (500, 1), (1500, 0), (99999, 0)])
    assert c.count == 2  # 10 and 99999 dropped


def test_kmeans_empty():
    assert rfdecode.kmeans2_all([]) == rfdecode.Clusters()


def test_kmeans_by_level():
    p = _pwm([1, 0, 1, 0, 1, 1])
    hi = rfdecode.kmeans2_level(p, 1)
    lo = rfdecode.kmeans2_level(p, 0)
    assert hi.long_us > hi.short_us
    assert abs(lo.short_us - lo.long_us) <= 1.0


# --- encoding classifier -------------------------------------------

def test_classify_pulse_distance():
    d = rfdecode.analyze(_ppm([1, 0, 1, 1, 0, 0, 1, 0]))
    assert d["encoding"] == "pulse-distance / PPM-style"
    assert d["ratio"] > 1.7


def test_classify_pulse_width():
    assert rfdecode.analyze(_pwm([1, 0, 1, 0, 1, 1, 0, 0]))["encoding"] == "pulse-width / PWM-style"


def test_classify_unknown():
    assert rfdecode.analyze([(500, 1), (500, 0)] * 12)["encoding"] == "unknown / mixed timing"


# --- candidate bits ----------------------------------------------

def test_candidate_bits_pulse_distance():
    p = []
    for b in (1, 0, 1, 1, 0):
        p += [(500, 1), (1500 if b else 500, 0)]
    be = rfdecode.candidate_bits(p, *_clusters(p), "pulse-distance / PPM-style")
    assert be["bits"] == "10110"
    assert be["inverted"] == "01001"


def test_candidate_bits_pulse_width():
    p = []
    for b in (1, 1, 0, 1, 0):
        p += [(1500 if b else 500, 1), (500, 0)]
    be = rfdecode.candidate_bits(p, *_clusters(p), "pulse-width / PWM-style")
    assert be["bits"] == "11010"


def test_candidate_bits_burst_separator():
    p = []
    for b in (1, 0, 1):
        p += [(500, 1), (1500 if b else 500, 0)]
    p.append((40000, 0))
    for b in (0, 1, 0):
        p += [(500, 1), (1500 if b else 500, 0)]
    be = rfdecode.candidate_bits(p, *_clusters(p), "pulse-distance / PPM-style")
    assert be["bits"] == "101|010"


def test_candidate_bits_cap_220():
    p = [(500, 1), (1500, 0)] * 400
    be = rfdecode.candidate_bits(p, *_clusters(p), "pulse-distance / PPM-style")
    assert len(be["bits"]) <= 220


# --- histogram --------------------------------------------------

def test_histogram_bins_sum():
    p = _ppm([1, 0, 1, 1, 0, 1, 0, 0])
    h = rfdecode.histogram(p, 28)
    assert h["bin_width_us"] >= 1
    for c, hi, lo in zip(h["counts"], h["highs"], h["lows"]):
        assert c == hi + lo
    assert sum(h["counts"]) == len(p)


# --- Linear recognizer ----------------------------------------

def test_linear_roundtrip():
    # header 21 ms LOW, then 10 bit pairs (0 = 500/1500, 1 = 1500/500), 21 ms guard
    bits = [1, 0, 1, 1, 0, 0, 1, 0, 1, 1]
    p = [(21000, 0)]
    for b in bits:
        p += ([(1500, 1), (500, 0)] if b else [(500, 1), (1500, 0)])
    p[-1] = (21000, 0)  # final LOW is the frame guard
    p.append((1500 if bits[-1] else 500, 1))  # trailing HIGH so guard is seen after a saved high
    p.append((21000, 0))
    d = rfdecode.try_linear(p)
    assert d is not None
    assert d["name"] == "Linear 10-bit"
    assert len(d["bits"]) == 10


def test_linear_rejects_noise():
    assert rfdecode.try_linear([(700, 1), (700, 0)] * 40) is None


# --- MegaCode recognizer ------------------------------------

def test_megacode_roundtrip():
    code = 0x800000 | 0x2ABCD8
    d = rfdecode.try_megacode(_megacode(code))
    assert d is not None
    assert d["name"] == "MegaCode 24-bit"
    assert d["data"] == code
    assert len(d["bits"]) == 24


def test_megacode_fields():
    code = 0x800000 | (0x0A << 19) | (0x1234 << 3) | 0x5
    d = rfdecode.try_megacode(_megacode(code & 0xFFFFFF | 0x800000))
    assert d is not None
    assert d["facility"] == (d["data"] >> 19) & 0x0F
    assert d["button"] == d["data"] & 0x07


def test_megacode_rejects_noise():
    assert rfdecode.try_megacode([(700, 1), (700, 0)] * 40) is None


# --- EV1527 recognizer -------------------------------------

def test_ev1527_roundtrip():
    code = 0xA5C3D & 0xFFFFFF
    d = rfdecode.try_ev1527(_ev1527(code), te_hint=350)
    assert d is not None
    assert d["data"] == code
    assert d["address"] == (code >> 4) & 0xFFFFF
    assert d["button"] == code & 0xF


def test_ev1527_te_from_clusters():
    p = _ev1527(0x123456, te=300)
    d = rfdecode.try_ev1527(p, int(rfdecode.kmeans2_all(p).short_us))
    assert d is not None and d["data"] == 0x123456


def test_ev1527_rejects_megacode():
    assert rfdecode.try_ev1527(_megacode(0x800000 | 0x155555), te_hint=1000) is None


def test_ev1527_none_on_noise():
    assert rfdecode.try_ev1527([(700, 1), (700, 0)] * 30, te_hint=350) is None
