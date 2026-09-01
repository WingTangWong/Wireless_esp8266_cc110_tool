// Host unit tests for src/decode.cpp - the pure pulse-timing kernels.
//   pio test -e native
#include <unity.h>

#include <cstring>
#include <vector>

#include "decode.h"

using namespace rfd;

void setUp() {}
void tearDown() {}

// Build a PulseSpan from a {dur, level} list. The vectors must outlive the span.
struct Capture {
  std::vector<uint32_t> dur;
  std::vector<uint8_t> lvl;
  void add(uint32_t d, uint8_t l) { dur.push_back(d); lvl.push_back(l); }
  PulseSpan span() const { return PulseSpan{dur.data(), lvl.data(), (uint16_t)dur.size()}; }
};

static Capture ppm(const std::vector<int>& bits, uint32_t shrt = 500,
                   uint32_t lng = 1500, uint32_t gap = 6000) {
  Capture c;
  for (int b : bits) {
    c.add(shrt, 1);
    c.add(b ? lng : shrt, 0);
  }
  c.add(gap, 0);
  return c;
}

static Capture pwm(const std::vector<int>& bits, uint32_t shrt = 500,
                   uint32_t lng = 1500, uint32_t lowgap = 500) {
  Capture c;
  for (int b : bits) {
    c.add(b ? lng : shrt, 1);
    c.add(lowgap, 0);
  }
  return c;
}

// --- nearDuration --------------------------------------------------------

void test_near_duration() {
  TEST_ASSERT_TRUE(nearDuration(1000, 1000, 0));
  TEST_ASSERT_TRUE(nearDuration(1200, 1000, 200));
  TEST_ASSERT_TRUE(nearDuration(800, 1000, 200));
  TEST_ASSERT_FALSE(nearDuration(1201, 1000, 200));
  TEST_ASSERT_FALSE(nearDuration(0, 1000, 200));  // no unsigned wrap
}

// --- k-means -----------------------------------------------------------

void test_kmeans_two_clusters() {
  Capture c;
  for (int i = 0; i < 5; ++i) {
    c.add(500, 1); c.add(520, 0); c.add(1500, 1); c.add(1480, 0);
  }
  Clusters k = kmeans2All(c.span());
  TEST_ASSERT_UINT16_WITHIN(1, 20, k.count);
  TEST_ASSERT_FLOAT_WITHIN(30.0f, 510.0f, k.shortUs);
  TEST_ASSERT_FLOAT_WITHIN(30.0f, 1490.0f, k.longUs);
  TEST_ASSERT_TRUE(k.shortUs < k.longUs);
}

void test_kmeans_filters_range() {
  Capture c;
  c.add(10, 1); c.add(500, 1); c.add(1500, 0); c.add(99999, 0);
  Clusters k = kmeans2All(c.span());
  TEST_ASSERT_EQUAL_UINT16(2, k.count);  // 10 and 99999 dropped
}

void test_kmeans_empty() {
  Clusters k = kmeans2All(PulseSpan{nullptr, nullptr, 0});
  TEST_ASSERT_EQUAL_UINT16(0, k.count);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, k.shortUs);
}

void test_kmeans_by_level() {
  Capture c = pwm({1, 0, 1, 0, 1, 1});
  Clusters hi = kmeans2Level(c.span(), 1);
  Clusters lo = kmeans2Level(c.span(), 0);
  TEST_ASSERT_TRUE(hi.longUs > hi.shortUs);   // HIGH carries the bits
  TEST_ASSERT_FLOAT_WITHIN(1.0f, lo.shortUs, lo.longUs);  // LOW is constant
}

// --- encoding classifier --------------------------------------------

void test_classify_pulse_distance() {
  Capture c = ppm({1, 0, 1, 1, 0, 0, 1, 0});
  Clusters a = kmeans2All(c.span());
  Clusters h = kmeans2Level(c.span(), 1);
  Clusters l = kmeans2Level(c.span(), 0);
  TEST_ASSERT_EQUAL(ENC_PULSE_DISTANCE, classifyEncoding(a, h, l));
}

void test_classify_pulse_width() {
  Capture c = pwm({1, 0, 1, 0, 1, 1, 0, 0});
  Clusters a = kmeans2All(c.span());
  Clusters h = kmeans2Level(c.span(), 1);
  Clusters l = kmeans2Level(c.span(), 0);
  TEST_ASSERT_EQUAL(ENC_PULSE_WIDTH, classifyEncoding(a, h, l));
}

void test_classify_unknown() {
  Capture c;
  for (int i = 0; i < 12; ++i) { c.add(500, 1); c.add(500, 0); }
  Clusters a = kmeans2All(c.span());
  Clusters h = kmeans2Level(c.span(), 1);
  Clusters l = kmeans2Level(c.span(), 0);
  TEST_ASSERT_EQUAL(ENC_UNKNOWN, classifyEncoding(a, h, l));
}

void test_encoding_names() {
  TEST_ASSERT_EQUAL_STRING("pulse-distance / PPM-style", encodingName(ENC_PULSE_DISTANCE));
  TEST_ASSERT_EQUAL_STRING("unknown / mixed timing", encodingName(ENC_UNKNOWN));
}

// --- candidate bits ------------------------------------------------

static BitExtraction extract(const Capture& c, Encoding forced) {
  Clusters a = kmeans2All(c.span());
  Clusters h = kmeans2Level(c.span(), 1);
  Clusters l = kmeans2Level(c.span(), 0);
  return candidateBits(c.span(), a, h, l, forced);
}

void test_candidate_bits_pulse_distance() {
  // long LOW = 1, short LOW = 0 (no trailing gap: keeps the LOW cluster clean)
  Capture c;
  for (int b : {1, 0, 1, 1, 0}) { c.add(500, 1); c.add(b ? 1500 : 500, 0); }
  BitExtraction be = extract(c, ENC_PULSE_DISTANCE);
  TEST_ASSERT_EQUAL_STRING("10110", be.bits);
  TEST_ASSERT_EQUAL_STRING("01001", be.inverted);
}

void test_candidate_bits_pulse_width() {
  Capture c;
  for (int b : {1, 1, 0, 1, 0}) { c.add(b ? 1500 : 500, 1); c.add(500, 0); }
  BitExtraction be = extract(c, ENC_PULSE_WIDTH);
  TEST_ASSERT_EQUAL_STRING("11010", be.bits);
}

void test_candidate_bits_burst_separator() {
  // two 3-bit bursts split by a long sync gap
  Capture c;
  for (int b : {1, 0, 1}) { c.add(500, 1); c.add(b ? 1500 : 500, 0); }
  c.add(40000, 0);  // > sync threshold -> '|'
  for (int b : {0, 1, 0}) { c.add(500, 1); c.add(b ? 1500 : 500, 0); }
  BitExtraction be = extract(c, ENC_PULSE_DISTANCE);
  TEST_ASSERT_EQUAL_STRING("101|010", be.bits);
}

void test_candidate_bits_cap_220() {
  Capture c;
  for (int i = 0; i < 400; ++i) { c.add(500, 1); c.add(1500, 0); }
  BitExtraction be = extract(c, ENC_PULSE_DISTANCE);
  TEST_ASSERT_LESS_OR_EQUAL_size_t(220, strlen(be.bits));
}

// --- MegaCode recognizer ------------------------------------------

static const MegaParams MEGA = {1000, 300, 6000, 1200, 10000, 13000, 3400};

// Synthesize a 24-bit MegaCode-style frame for tryMegaCode's timing model:
// 13 ms header low, a 1 ms start-bit HIGH, then per data bit a LOW gap sized so
// teLast lands on the long (bit 1, ~5000 us) or short (bit 0, ~2000 us) target,
// followed by a 1 ms HIGH. teLast = lastBit ? lowGap : lowGap - symbol/2.
// Requires bit 23 of code24 set (the start bit).
static Capture megacode(uint32_t code24) {
  Capture c;
  c.add(13000, 0);                       // header low
  c.add(1000, 1);                        // start bit -> data = 1
  uint8_t prevBit = 1;
  for (int i = 22; i >= 0; --i) {
    const uint8_t bit = (code24 >> i) & 1;
    uint32_t lowGap;
    if (prevBit) lowGap = bit ? 5000u : 2000u;
    else         lowGap = bit ? 8000u : 5000u;
    c.add(lowGap, 0);
    c.add(1000, 1);
    prevBit = bit;
  }
  c.add(12000, 0);                        // frame gap -> emit
  return c;
}

void test_megacode_roundtrip() {
  // top bit must be 1 (start bit); pick an arbitrary 24-bit value
  const uint32_t code = 0x800000u | 0x2ABCD8u;
  Capture c = megacode(code);
  ProtocolDecode d = tryMegaCode(c.span(), MEGA);
  TEST_ASSERT_TRUE(d.matched);
  TEST_ASSERT_EQUAL_STRING("MegaCode 24-bit", d.name);
  TEST_ASSERT_EQUAL_UINT32(code, d.data);
  TEST_ASSERT_EQUAL_size_t(24, strlen(d.bits));
}

// --- EV1527 / PT2262 recognizer ---------------------------------

static Capture ev1527(uint32_t code24, uint32_t te = 350) {
  Capture c;
  c.add(te, 1);              // brief preamble HIGH
  c.add(te * 31, 0);         // sync gap
  for (int i = 23; i >= 0; --i) {
    if ((code24 >> i) & 1) { c.add(te * 3, 1); c.add(te, 0); }   // 1
    else                   { c.add(te, 1);     c.add(te * 3, 0); } // 0
  }
  c.add(te * 31, 0);         // trailing sync
  return c;
}

void test_ev1527_roundtrip() {
  const uint32_t code = 0xA5C3Du & 0xFFFFFF;
  Capture c = ev1527(code);
  ProtocolDecode d = tryEV1527(c.span(), 350);
  TEST_ASSERT_TRUE(d.matched);
  TEST_ASSERT_EQUAL_STRING("EV1527 / PT2262 24-bit", d.name);
  TEST_ASSERT_EQUAL_UINT32(code, d.data);
  TEST_ASSERT_EQUAL_size_t(24, strlen(d.bits));
}

void test_ev1527_te_from_clusters() {
  Capture c = ev1527(0x123456u, 300);
  Clusters a = kmeans2All(c.span());
  ProtocolDecode d = tryEV1527(c.span(), (uint32_t)a.shortUs);
  TEST_ASSERT_TRUE(d.matched);
  TEST_ASSERT_EQUAL_UINT32(0x123456u, d.data);
}

void test_ev1527_rejects_megacode() {
  Capture c = megacode(0x800000u | 0x155555u);
  ProtocolDecode d = tryEV1527(c.span(), 1000);
  TEST_ASSERT_FALSE(d.matched);
}

void test_megacode_rejects_noise() {
  Capture c;
  for (int i = 0; i < 40; ++i) { c.add(700, 1); c.add(700, 0); }
  ProtocolDecode d = tryMegaCode(c.span(), MEGA);
  TEST_ASSERT_FALSE(d.matched);
}

// --- histogram ------------------------------------------------------

void test_histogram_bins_sum() {
  Capture c = ppm({1, 0, 1, 1, 0, 1, 0, 0});
  uint16_t counts[28], highs[28], lows[28];
  uint32_t bw = histogram(c.span(), 28, counts, highs, lows);
  TEST_ASSERT_TRUE(bw >= 1);
  uint32_t total = 0;
  for (int i = 0; i < 28; ++i) {
    TEST_ASSERT_EQUAL_UINT16(counts[i], (uint16_t)(highs[i] + lows[i]));
    total += counts[i];
  }
  // the 6 ms gap is > 12000? no -> counted; all 17 pulses fit
  TEST_ASSERT_EQUAL_UINT32(c.dur.size(), total);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_near_duration);
  RUN_TEST(test_kmeans_two_clusters);
  RUN_TEST(test_kmeans_filters_range);
  RUN_TEST(test_kmeans_empty);
  RUN_TEST(test_kmeans_by_level);
  RUN_TEST(test_classify_pulse_distance);
  RUN_TEST(test_classify_pulse_width);
  RUN_TEST(test_classify_unknown);
  RUN_TEST(test_encoding_names);
  RUN_TEST(test_candidate_bits_pulse_distance);
  RUN_TEST(test_candidate_bits_pulse_width);
  RUN_TEST(test_candidate_bits_burst_separator);
  RUN_TEST(test_candidate_bits_cap_220);
  RUN_TEST(test_megacode_roundtrip);
  RUN_TEST(test_megacode_rejects_noise);
  RUN_TEST(test_ev1527_roundtrip);
  RUN_TEST(test_ev1527_te_from_clusters);
  RUN_TEST(test_ev1527_rejects_megacode);
  RUN_TEST(test_histogram_bins_sum);
  return UNITY_END();
}
