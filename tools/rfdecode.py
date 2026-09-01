"""Pure-Python port of the firmware's pulse-timing analysis (src/decode.cpp).

Mirrors kmeans2All / kmeans2Level, the encoding-family classifier, the
candidate-bit extractor, and the Linear / MegaCode / EV1527 recognizers.
Used to unit-test the decode logic on the host and to cross-check what the
device reports on /api/decode/current.

A "pulse" is (duration_us, level) with level 1 = HIGH, 0 = LOW, matching the
DiskPulse layout.
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass
class Clusters:
    short_us: float = 0.0
    long_us: float = 0.0
    count: int = 0


def near(value, target, tol):
    return abs(value - target) <= tol


# --- clustering -----------------------------------------------------------

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


# --- candidate bits ----------------------------------------------------

def candidate_bits(pulses, all_c: Clusters, high_c: Clusters, low_c: Clusters, encoding: str):
    """Port of rfd::candidateBits -> {"bits", "inverted", "sync_threshold_us"}."""
    all_split = (all_c.short_us + all_c.long_us) * 0.5
    high_split = (high_c.short_us + high_c.long_us) * 0.5
    low_split = (low_c.short_us + low_c.long_us) * 0.5
    sync = max(5000.0, all_c.long_us * 4.0)

    bits = []
    inv = []
    n = len(pulses)
    i = 0
    while i < n and len(bits) < 220:
        dur, lvl = pulses[i]
        if dur > sync:
            if bits and bits[-1] != "|":
                bits.append("|")
                inv.append("|")
            i += 1
            continue
        if lvl != 1 or i + 1 >= n or pulses[i + 1][1] != 0:
            i += 1
            continue
        h = pulses[i][0]
        low = pulses[i + 1][0]
        if low > sync:
            if bits and bits[-1] != "|":
                bits.append("|")
                inv.append("|")
            i += 2
            continue

        bit = "?"
        if encoding.startswith("pulse-distance"):
            bit = "0" if low < low_split else "1"
        elif encoding.startswith("pulse-width"):
            bit = "0" if h < high_split else "1"
        else:
            hs = h < all_split
            ls = low < all_split
            if hs and not ls:
                bit = "0"
            elif not hs and ls:
                bit = "1"
        bits.append(bit)
        inv.append("1" if bit == "0" else "0" if bit == "1" else "?")
        i += 2

    return {"bits": "".join(bits), "inverted": "".join(inv), "sync_threshold_us": sync}


# --- histogram --------------------------------------------------------

def histogram(pulses, bins=28):
    counts = [0] * bins
    highs = [0] * bins
    lows = [0] * bins
    if not pulses:
        return {"bin_width_us": 1, "counts": counts, "highs": highs, "lows": lows}

    max_v = max((d for d, _ in pulses if d <= 12000), default=0)
    max_v = max(max_v, 1000)
    bin_w = max(1, -(-max_v // bins))  # ceil

    for d, lv in pulses:
        if d > max_v:
            continue
        bi = min(bins - 1, d // bin_w)
        counts[bi] += 1
        if lv:
            highs[bi] += 1
        else:
            lows[bi] += 1
    return {"bin_width_us": bin_w, "counts": counts, "highs": highs, "lows": lows}


# --- focused recognizers ---------------------------------------------

def try_linear(pulses):
    """Port of rfd::tryLinear -> dict or None."""
    state = "WAIT_HEADER"
    saved_high = 0
    data = 0
    bits = 0
    for dur, lvl in pulses:
        if state == "WAIT_HEADER":
            if lvl == 0 and near(dur, 21000, 5250):
                data = 0
                bits = 0
                state = "SAVE_HIGH"
        elif state == "SAVE_HIGH":
            if lvl == 1:
                saved_high = dur
                state = "CHECK_LOW"
            else:
                state = "WAIT_HEADER"
        else:  # CHECK_LOW
            if lvl != 0:
                state = "WAIT_HEADER"
                continue
            if dur >= 2500:
                if near(dur, 21000, 5250):
                    if near(saved_high, 500, 350):
                        data = data << 1
                        bits += 1
                    elif near(saved_high, 1500, 350):
                        data = (data << 1) | 1
                        bits += 1
                    if bits == 10:
                        return {
                            "name": "Linear 10-bit", "data": data,
                            "bits": format(data, "010b"),
                            "details": "~500/1500 us pulse-pair timing with ~21 ms frame guard",
                        }
                state = "WAIT_HEADER"
            elif near(saved_high, 500, 350) and near(dur, 1500, 350):
                data = data << 1
                bits += 1
                state = "SAVE_HIGH"
            elif near(saved_high, 1500, 350) and near(dur, 500, 350):
                data = (data << 1) | 1
                bits += 1
                state = "SAVE_HIGH"
            else:
                state = "WAIT_HEADER"
    return None


MEGA_DEFAULTS = dict(
    pulse_us=1000, pulse_tol_us=300, symbol_us=6000, symbol_tol_us=1200,
    frame_gap_us=10000, header_low_us=13000, header_tol_us=3400,
)


def try_megacode(pulses, params=None):
    """Port of rfd::tryMegaCode -> dict or None."""
    p = {**MEGA_DEFAULTS, **(params or {})}
    half_symbol = p["symbol_us"] // 2
    long_low = p["symbol_us"] - p["pulse_us"] if p["symbol_us"] > p["pulse_us"] else 1
    short_low = half_symbol - p["pulse_us"] if half_symbol > p["pulse_us"] else 1

    state = "WAIT_HEADER"
    data = 0
    bits = 0
    last_bit = 0
    te_last = 0
    for dur, lvl in pulses:
        if state == "WAIT_HEADER":
            if lvl == 0 and near(dur, p["header_low_us"], p["header_tol_us"]):
                state = "START_BIT"
        elif state == "START_BIT":
            if lvl == 1 and near(dur, p["pulse_us"], p["pulse_tol_us"]):
                data = 1
                bits = 1
                last_bit = 1
                state = "SAVE_LOW"
            else:
                state = "WAIT_HEADER"
        elif state == "SAVE_LOW":
            if lvl != 0:
                state = "WAIT_HEADER"
                continue
            if dur >= p["frame_gap_us"]:
                if bits == 24 and (data >> 23) & 1:
                    return {
                        "name": "MegaCode 24-bit", "data": data,
                        "bits": format(data, "024b"),
                        "facility": (data >> 19) & 0x0F,
                        "serial": (data >> 3) & 0xFFFF,
                        "button": data & 0x07,
                    }
                state = "WAIT_HEADER"
                continue
            te_last = dur if last_bit else dur - half_symbol
            state = "CHECK_HIGH"
        elif state == "CHECK_HIGH":
            if lvl != 1:
                state = "WAIT_HEADER"
                continue
            if near(dur, p["pulse_us"], p["pulse_tol_us"]) and te_last > 0 and \
                    near(te_last, long_low, p["symbol_tol_us"]):
                data = (data << 1) | 1
                bits += 1
                last_bit = 1
                state = "SAVE_LOW"
            elif near(dur, p["pulse_us"], p["pulse_tol_us"]) and te_last > 0 and \
                    near(te_last, short_low, p["symbol_tol_us"] // 2):
                data = data << 1
                bits += 1
                last_bit = 0
                state = "SAVE_LOW"
            else:
                state = "WAIT_HEADER"
    return None


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
                data = data << 1
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
    """The subset of /api/decode/current fields this port reproduces."""
    all_c = kmeans2_all(pulses)
    high_c = kmeans2_level(pulses, 1)
    low_c = kmeans2_level(pulses, 0)
    ratio = all_c.long_us / all_c.short_us if all_c.short_us > 0 else 0.0
    enc = classify_encoding(all_c, high_c, low_c)
    linear = try_linear(pulses)
    mega = try_megacode(pulses)
    ev = try_ev1527(pulses, int(all_c.short_us))
    return {
        "encoding": enc,
        "short_us": round(all_c.short_us, 1),
        "long_us": round(all_c.long_us, 1),
        "ratio": round(ratio, 2),
        "candidate_bits": candidate_bits(pulses, all_c, high_c, low_c, enc)["bits"],
        "protocol": (linear or mega or ev),
    }
