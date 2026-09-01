// decode.cpp - see decode.h. Mechanical extraction from main.cpp; logic and
// constants must stay identical to the firmware's previous inline versions.
#include "decode.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace rfd {

bool nearDuration(uint32_t value, uint32_t target, uint32_t tolerance) {
  return value > target ? (value - target <= tolerance) : (target - value <= tolerance);
}

static Clusters kmeans2(const PulseSpan& p, bool byLevel, uint8_t wantedLevel,
                        uint32_t minV, uint32_t maxV) {
  Clusters r;
  const uint16_t n = p.n;

  float lo = 1e9f;
  float hi = 0.0f;
  uint16_t good = 0;

  for (uint16_t i = 0; i < n; ++i) {
    if (byLevel && p.lvl[i] != wantedLevel) continue;
    const uint32_t v = p.dur[i];
    if (v < minV || v > maxV) continue;
    if (v < lo) lo = (float)v;
    if (v > hi) hi = (float)v;
    ++good;
  }
  if (!good) return r;

  float c0 = lo;
  float c1 = hi;
  for (uint8_t iter = 0; iter < 12; ++iter) {
    double s0 = 0, s1 = 0;
    uint16_t n0 = 0, n1 = 0;
    for (uint16_t i = 0; i < n; ++i) {
      if (byLevel && p.lvl[i] != wantedLevel) continue;
      const uint32_t v = p.dur[i];
      if (v < minV || v > maxV) continue;
      if (fabsf((float)v - c0) <= fabsf((float)v - c1)) {
        s0 += v; ++n0;
      } else {
        s1 += v; ++n1;
      }
    }
    if (n0) c0 = (float)(s0 / n0);
    if (n1) c1 = (float)(s1 / n1);
  }

  if (c0 > c1) {
    const float t = c0; c0 = c1; c1 = t;
  }
  r.shortUs = c0;
  r.longUs = c1;
  r.count = good;
  return r;
}

Clusters kmeans2All(const PulseSpan& p, uint32_t minV, uint32_t maxV) {
  if (!p.n) return Clusters();
  return kmeans2(p, false, 0, minV, maxV);
}

Clusters kmeans2Level(const PulseSpan& p, uint8_t wantedLevel, uint32_t minV, uint32_t maxV) {
  return kmeans2(p, true, wantedLevel, minV, maxV);
}

Encoding classifyEncoding(const Clusters& all, const Clusters& high, const Clusters& low) {
  const float allRatio  = all.shortUs  > 0 ? all.longUs  / all.shortUs  : 0.0f;
  const float highRatio = high.shortUs > 0 ? high.longUs / high.shortUs : 1.0f;
  const float lowRatio  = low.shortUs  > 0 ? low.longUs  / low.shortUs  : 1.0f;

  if (high.count && low.count) {
    if (highRatio < 1.45f && lowRatio > 1.70f) return ENC_PULSE_DISTANCE;
    if (lowRatio < 1.45f && highRatio > 1.70f) return ENC_PULSE_WIDTH;
    if (highRatio > 1.70f && lowRatio > 1.70f && allRatio > 1.7f && allRatio < 4.8f)
      return ENC_COMPLEMENTARY;
    if (allRatio > 1.7f && allRatio < 4.8f) return ENC_SHORT_LONG_FAMILY;
  }
  return ENC_UNKNOWN;
}

const char* encodingName(Encoding e) {
  switch (e) {
    case ENC_PULSE_DISTANCE:    return "pulse-distance / PPM-style";
    case ENC_PULSE_WIDTH:       return "pulse-width / PWM-style";
    case ENC_COMPLEMENTARY:     return "complementary short-long pulse pairs";
    case ENC_SHORT_LONG_FAMILY: return "short-long timing family";
    case ENC_UNKNOWN:
    default:                    return "unknown / mixed timing";
  }
}

BitExtraction candidateBits(const PulseSpan& p, const Clusters& all,
                            const Clusters& high, const Clusters& low, Encoding enc) {
  BitExtraction r;

  const float allSplit  = (all.shortUs  + all.longUs)  * 0.5f;
  const float highSplit = (high.shortUs + high.longUs) * 0.5f;
  const float lowSplit  = (low.shortUs  + low.longUs)  * 0.5f;
  const float sync = (5000.0f > all.longUs * 4.0f) ? 5000.0f : all.longUs * 4.0f;
  r.syncThresholdUs = sync;

  const uint16_t n = p.n;
  size_t bl = 0;
  const size_t cap = 220;

  for (uint16_t i = 0; i < n && bl < cap; ++i) {
    if ((float)p.dur[i] > sync) {
      if (bl && r.bits[bl - 1] != '|') { r.bits[bl] = '|'; r.inverted[bl] = '|'; ++bl; }
      continue;
    }

    if (p.lvl[i] != 1 || i + 1 >= n || p.lvl[i + 1] != 0) continue;

    const uint32_t h = p.dur[i];
    const uint32_t l = p.dur[i + 1];
    if ((float)l > sync) {
      if (bl && r.bits[bl - 1] != '|') { r.bits[bl] = '|'; r.inverted[bl] = '|'; ++bl; }
      ++i;
      continue;
    }

    char bit = '?';
    if (enc == ENC_PULSE_DISTANCE) {
      bit = ((float)l < lowSplit) ? '0' : '1';
    } else if (enc == ENC_PULSE_WIDTH) {
      bit = ((float)h < highSplit) ? '0' : '1';
    } else {
      const bool hs = (float)h < allSplit;
      const bool ls = (float)l < allSplit;
      if (hs && !ls) bit = '0';
      else if (!hs && ls) bit = '1';
    }

    r.bits[bl] = bit;
    r.inverted[bl] = (bit == '0') ? '1' : (bit == '1') ? '0' : '?';
    ++bl;
    ++i;
  }
  r.bits[bl] = 0;
  r.inverted[bl] = 0;
  return r;
}

ProtocolDecode tryLinear(const PulseSpan& p) {
  ProtocolDecode out;
  enum State { WAIT_HEADER, SAVE_HIGH, CHECK_LOW } state = WAIT_HEADER;
  uint32_t savedHigh = 0;
  uint32_t data = 0;
  uint8_t bitCount = 0;

  for (uint16_t i = 0; i < p.n; ++i) {
    const uint8_t level = p.lvl[i];
    const uint32_t dur = p.dur[i];

    if (state == WAIT_HEADER) {
      if (level == 0 && nearDuration(dur, 21000, 5250)) {
        data = 0;
        bitCount = 0;
        state = SAVE_HIGH;
      }
    } else if (state == SAVE_HIGH) {
      if (level == 1) {
        savedHigh = dur;
        state = CHECK_LOW;
      } else {
        state = WAIT_HEADER;
      }
    } else {
      if (level != 0) {
        state = WAIT_HEADER;
        continue;
      }

      if (dur >= 2500) {
        const bool guard = nearDuration(dur, 21000, 5250);
        if (guard) {
          if (nearDuration(savedHigh, 500, 350)) { data = (data << 1) | 0; ++bitCount; }
          else if (nearDuration(savedHigh, 1500, 350)) { data = (data << 1) | 1; ++bitCount; }

          if (bitCount == 10) {
            out.matched = true;
            strcpy(out.name, "Linear 10-bit");
            out.data = data;
            for (int b = 9; b >= 0; --b) {
              const size_t k = 9 - (size_t)b;
              out.bits[k] = ((data >> b) & 1) ? '1' : '0';
            }
            strcpy(out.details, "~500/1500 us pulse-pair timing with ~21 ms frame guard");
            return out;
          }
        }
        state = WAIT_HEADER;
      } else if (nearDuration(savedHigh, 500, 350) && nearDuration(dur, 1500, 350)) {
        data = (data << 1) | 0; ++bitCount;
        state = SAVE_HIGH;
      } else if (nearDuration(savedHigh, 1500, 350) && nearDuration(dur, 500, 350)) {
        data = (data << 1) | 1; ++bitCount;
        state = SAVE_HIGH;
      } else {
        state = WAIT_HEADER;
      }
    }
  }
  return out;
}

ProtocolDecode tryMegaCode(const PulseSpan& p, const MegaParams& mp) {
  ProtocolDecode out;
  enum State { WAIT_HEADER, START_BIT, SAVE_LOW, CHECK_HIGH } state = WAIT_HEADER;
  uint32_t data = 0;
  uint8_t bitCount = 0;
  uint8_t lastBit = 0;
  int32_t teLast = 0;
  const uint32_t halfSymbolUs = mp.symbolUs / 2;
  const uint32_t longLowTargetUs = (mp.symbolUs > mp.pulseUs) ? (mp.symbolUs - mp.pulseUs) : 1;
  const uint32_t shortLowTargetUs = (halfSymbolUs > mp.pulseUs) ? (halfSymbolUs - mp.pulseUs) : 1;

  for (uint16_t i = 0; i < p.n; ++i) {
    const uint8_t level = p.lvl[i];
    const uint32_t dur = p.dur[i];

    switch (state) {
      case WAIT_HEADER:
        if (level == 0 && nearDuration(dur, mp.headerLowUs, mp.headerTolUs)) state = START_BIT;
        break;

      case START_BIT:
        if (level == 1 && nearDuration(dur, mp.pulseUs, mp.pulseTolUs)) {
          data = 0;
          bitCount = 0;
          data = (data << 1) | 1; ++bitCount;
          lastBit = 1;
          state = SAVE_LOW;
        } else {
          state = WAIT_HEADER;
        }
        break;

      case SAVE_LOW:
        if (level != 0) {
          state = WAIT_HEADER;
          break;
        }
        if (dur >= mp.frameGapUs) {
          if (bitCount == 24 && ((data >> 23) & 1)) {
            out.matched = true;
            strcpy(out.name, "MegaCode 24-bit");
            out.data = data;
            for (int b = 23; b >= 0; --b) {
              const size_t k = 23 - (size_t)b;
              out.bits[k] = ((data >> b) & 1) ? '1' : '0';
            }
            const uint32_t serial = (data >> 3) & 0xFFFF;
            const uint32_t facility = (data >> 19) & 0x0F;
            const uint32_t button = data & 0x07;
            snprintf(out.details, sizeof(out.details),
                     "facility=%lu, serial=%lu, button=%lu",
                     (unsigned long)facility, (unsigned long)serial, (unsigned long)button);
            return out;
          }
          state = WAIT_HEADER;
          break;
        }

        teLast = lastBit ? (int32_t)dur : (int32_t)dur - (int32_t)halfSymbolUs;
        state = CHECK_HIGH;
        break;

      case CHECK_HIGH:
        if (level != 1) {
          state = WAIT_HEADER;
          break;
        }
        if (nearDuration(dur, mp.pulseUs, mp.pulseTolUs) && teLast > 0 &&
            nearDuration((uint32_t)teLast, longLowTargetUs, mp.symbolTolUs)) {
          data = (data << 1) | 1; ++bitCount;
          lastBit = 1;
          state = SAVE_LOW;
        } else if (nearDuration(dur, mp.pulseUs, mp.pulseTolUs) && teLast > 0 &&
                   nearDuration((uint32_t)teLast, shortLowTargetUs, mp.symbolTolUs / 2)) {
          data = (data << 1) | 0; ++bitCount;
          lastBit = 0;
          state = SAVE_LOW;
        } else {
          state = WAIT_HEADER;
        }
        break;
    }
  }
  return out;
}

ProtocolDecode tryEV1527(const PulseSpan& p, uint32_t teHintUs) {
  ProtocolDecode out;
  if (teHintUs < 50) teHintUs = 50;
  if (teHintUs > 1200) teHintUs = 1200;

  // Plausible sync-gap window, loosely bounded by the caller's Te hint.
  const uint32_t syncMin = teHintUs * 8;
  const uint32_t syncMax = teHintUs * 80;

  for (uint16_t start = 0; start + 1 < p.n; ++start) {
    // sync = a long LOW gap; Te is derived from it (EV1527 sync ~= 31*Te).
    if (p.lvl[start] != 0) continue;
    const uint32_t gap = p.dur[start];
    if (gap < syncMin || gap > syncMax) continue;

    uint32_t te = gap / 31;
    if (te < 40 || te > 1500) continue;

    const uint32_t tShort = te;
    const uint32_t tLong = te * 3;
    const uint32_t tolShort = (te * 3) / 4;   // OOK timing is noisy
    const uint32_t tolLong = te * 2;

    uint32_t data = 0;
    uint8_t bitCount = 0;
    uint16_t i = start + 1;
    bool bad = false;

    while (bitCount < 24 && i + 1 < p.n) {
      if (p.lvl[i] != 1 || p.lvl[i + 1] != 0) { bad = true; break; }
      const uint32_t h = p.dur[i];
      const uint32_t l = p.dur[i + 1];

      if (nearDuration(h, tShort, tolShort) && nearDuration(l, tLong, tolLong)) {
        data = (data << 1) | 0;
      } else if (nearDuration(h, tLong, tolLong) && nearDuration(l, tShort, tolShort)) {
        data = (data << 1) | 1;
      } else {
        bad = true;
        break;
      }
      ++bitCount;
      i += 2;
    }

    if (!bad && bitCount == 24) {
      const uint32_t teUs = te;
      out.matched = true;
      strcpy(out.name, "EV1527 / PT2262 24-bit");
      out.data = data;
      for (int b = 23; b >= 0; --b) {
        const size_t k = 23 - (size_t)b;
        out.bits[k] = ((data >> b) & 1) ? '1' : '0';
      }
      const uint32_t address = (data >> 4) & 0xFFFFF;   // 20-bit code
      const uint32_t button = data & 0x0F;              // 4-bit data/button
      snprintf(out.details, sizeof(out.details),
               "Te~%luus, address=0x%05lX, button=0x%lX",
               (unsigned long)teUs, (unsigned long)address, (unsigned long)button);
      return out;
    }
  }
  return out;
}

uint32_t histogram(const PulseSpan& p, uint8_t bins,
                   uint16_t* counts, uint16_t* highs, uint16_t* lows) {
  for (uint8_t i = 0; i < bins; ++i) { counts[i] = 0; highs[i] = 0; lows[i] = 0; }

  const uint16_t n = p.n;
  if (!n) return 1;

  uint32_t maxV = 0;
  for (uint16_t i = 0; i < n; ++i) {
    const uint32_t v = p.dur[i];
    if (v <= 12000 && v > maxV) maxV = v;
  }
  if (maxV < 1000) maxV = 1000;

  uint32_t binW = (maxV + bins - 1) / bins;
  if (binW == 0) binW = 1;

  for (uint16_t i = 0; i < n; ++i) {
    const uint32_t v = p.dur[i];
    if (v > maxV) continue;
    uint32_t bi = v / binW;
    if (bi >= bins) bi = bins - 1;
    ++counts[bi];
    if (p.lvl[i]) ++highs[bi];
    else ++lows[bi];
  }
  return binW;
}

}  // namespace rfd
