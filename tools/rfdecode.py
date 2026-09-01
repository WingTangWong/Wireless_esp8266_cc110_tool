"""Pure-Python port of the firmware's pulse-timing analysis.

Mirrors kmeans2All / kmeans2Level and the encoding-family classifier in
`src/main.cpp` (decodeCurrentJson). Used to cross-check what the device
reports on /api/decode/current, and unit-tested on synthetic pulse trains.

A "pulse" here is (duration_us, level) with level 1 = HIGH, 0 = LOW,
matching the DiskPulse layout.
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass
class Clusters:
    short_us: float = 0.0
    long_us: float = 0.0
    count: int = 0


def _kmeans2(values, min_v=40, max_v=12000, iters=12):
    vs = [v for v in values if min_v <= v <= max_v]
    if not vs:
        return Clusters()
    c0, c1 = float(min(vs)), float(max(vs))
    for _ in range(iters):
        s0 = s1 = 0.0
        n0 = n1 = 0
        for v in vs:
            if abs(v - c0) <= abs(v - c1):
                s0 += v
                n0 += 1
            else:
                s1 += v
                n1 += 1
        if n0:
            c0 = s0 / n0
        if n1:
            c1 = s1 / n1
    if c0 > c1:
        c0, c1 = c1, c0
    return Clusters(c0, c1, len(vs))


def kmeans2_all(pulses, min_v=40, max_v=12000):
    return _kmeans2([d for d, _ in pulses], min_v, max_v)


def kmeans2_level(pulses, level, min_v=40, max_v=12000):
    return _kmeans2([d for d, lv in pulses if lv == level], min_v, max_v)


def classify_encoding(all_c: Clusters, high_c: Clusters, low_c: Clusters) -> str:
    all_ratio = all_c.long_us / all_c.short_us if all_c.short_us > 0 else 0.0
    high_ratio = high_c.long_us / high_c.short_us if high_c.short_us > 0 else 1.0
    low_ratio = low_c.long_us / low_c.short_us if low_c.short_us > 0 else 1.0

    if high_c.count and low_c.count:
        if high_ratio < 1.45 and low_ratio > 1.70:
            return "pulse-distance / PPM-style"
        if low_ratio < 1.45 and high_ratio > 1.70:
            return "pulse-width / PWM-style"
        if high_ratio > 1.70 and low_ratio > 1.70 and 1.7 < all_ratio < 4.8:
            return "complementary short-long pulse pairs"
        if 1.7 < all_ratio < 4.8:
            return "short-long timing family"
    return "unknown / mixed timing"


def near(value, target, tol):
    return abs(value - target) <= tol


def try_ev1527(pulses, te_hint):
    """Port of rfd::tryEV1527 - returns dict or None."""
    te_hint = max(50, min(1200, te_hint))
    sync_min, sync_max = te_hint * 8, te_hint * 80
    n = len(pulses)
    for start in range(n - 1):
        d, lv = pulses[start]
        if lv != 0 or not (sync_min <= d <= sync_max):
            continue
        te = d // 31
        if not (40 <= te <= 1500):
            continue
        t_short, t_long = te, te * 3
        tol_short, tol_long = (te * 3) // 4, te * 2
        data, bits, i, bad = 0, 0, start + 1, False
        while bits < 24 and i + 1 < n:
            (h, hl), (low, ll) = pulses[i], pulses[i + 1]
            if hl != 1 or ll != 0:
                bad = True
                break
            if near(h, t_short, tol_short) and near(low, t_long, tol_long):
                data = (data << 1)
            elif near(h, t_long, tol_long) and near(low, t_short, tol_short):
                data = (data << 1) | 1
            else:
                bad = True
                break
            bits += 1
            i += 2
        if not bad and bits == 24:
            return {"name": "EV1527 / PT2262 24-bit", "data": data,
                    "address": (data >> 4) & 0xFFFFF, "button": data & 0xF, "te_us": te}
    return None


def analyze(pulses):
    """Return the fields the firmware puts on /api/decode/current that this
    port covers: encoding, short_us, long_us, ratio."""
    all_c = kmeans2_all(pulses)
    high_c = kmeans2_level(pulses, 1)
    low_c = kmeans2_level(pulses, 0)
    ratio = all_c.long_us / all_c.short_us if all_c.short_us > 0 else 0.0
    return {
        "encoding": classify_encoding(all_c, high_c, low_c),
        "short_us": round(all_c.short_us, 1),
        "long_us": round(all_c.long_us, 1),
        "ratio": round(ratio, 2),
    }
