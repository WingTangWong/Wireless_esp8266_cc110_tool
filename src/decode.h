// decode.h - pure pulse-timing analysis, no Arduino / radio dependency.
//
// Extracted from main.cpp so the DSP kernels can be unit-tested on the host
// (pio test -e native) and cross-checked against tools/rfdecode.py. The JSON
// formatting and the volatile capture buffers stay in main.cpp.
//
// A pulse is (durationUs, level) with level 1 = HIGH, 0 = LOW - the DiskPulse
// layout. Behaviour here must match the firmware exactly.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace rfd {

struct PulseSpan {
  const uint32_t* dur;   // durations, microseconds
  const uint8_t*  lvl;   // 1 = HIGH, 0 = LOW
  uint16_t        n;
};

struct Clusters {
  float    shortUs = 0.0f;
  float    longUs  = 0.0f;
  uint16_t count   = 0;
};

enum Encoding {
  ENC_UNKNOWN = 0,          // "unknown / mixed timing"
  ENC_PULSE_DISTANCE,       // "pulse-distance / PPM-style"
  ENC_PULSE_WIDTH,          // "pulse-width / PWM-style"
  ENC_COMPLEMENTARY,        // "complementary short-long pulse pairs"
  ENC_SHORT_LONG_FAMILY,    // "short-long timing family"
};

struct MegaParams {
  uint32_t pulseUs;
  uint32_t pulseTolUs;
  uint32_t symbolUs;
  uint32_t symbolTolUs;
  uint32_t frameGapUs;
  uint32_t headerLowUs;
  uint32_t headerTolUs;
};

struct ProtocolDecode {
  bool     matched = false;
  char     name[24]    = {0};
  char     bits[40]    = {0};
  uint32_t data        = 0;
  char     details[96] = {0};
};

// |value - target| <= tolerance, overflow-safe on unsigned.
bool nearDuration(uint32_t value, uint32_t target, uint32_t tolerance);

// 2-means split of pulse widths in [minV, maxV]. Result is short/long ordered;
// count is the number of in-range samples considered.
Clusters kmeans2All(const PulseSpan& p, uint32_t minV = 40, uint32_t maxV = 12000);
Clusters kmeans2Level(const PulseSpan& p, uint8_t wantedLevel,
                      uint32_t minV = 40, uint32_t maxV = 12000);

Encoding    classifyEncoding(const Clusters& all, const Clusters& high, const Clusters& low);
const char* encodingName(Encoding e);

struct BitExtraction {
  char  bits[224]     = {0};   // best-effort bitstring, '|' between bursts
  char  inverted[224] = {0};   // same with 0/1 flipped
  float syncThresholdUs = 0.0f;
};

// Best-effort candidate bitstring from HIGH/LOW pulse pairs. Matches the loop
// that was inline in decodeCurrentJson (caps at 220 characters).
BitExtraction candidateBits(const PulseSpan& p, const Clusters& all,
                            const Clusters& high, const Clusters& low, Encoding enc);

ProtocolDecode tryLinear(const PulseSpan& p);
ProtocolDecode tryMegaCode(const PulseSpan& p, const MegaParams& mp);

// EV1527 / PT2262-style 24-bit OOK: a ~31*Te sync gap, then 24 bits where
// 0 = short HIGH / long LOW and 1 = long HIGH / short LOW (ratio ~1:3).
// teUs is the base symbol time; pass the short-pulse cluster width.
ProtocolDecode tryEV1527(const PulseSpan& p, uint32_t teUs);

// Pulse-width histogram bins (matches pulseHistogramJson). counts/highs/lows
// must have room for `bins` entries. Returns the bin width in microseconds.
uint32_t histogram(const PulseSpan& p, uint8_t bins,
                   uint16_t* counts, uint16_t* highs, uint16_t* lows);

}  // namespace rfd
