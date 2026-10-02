// Host tests: RSSI filter, distance model and zone hysteresis.
#include <unity.h>
#include <math.h>
#include <string.h>
#include "fr_rssi.h"
#include "fr_zones.h"

using namespace fr;

void setUp() {}
void tearDown() {}

// Small deterministic PRNG for repeatable noise.
static uint32_t rng = 12345;
static float urand() { rng = rng * 1664525u + 1013904223u; return (float)(rng >> 8) / 16777216.f; }
static float gauss() {                       // Irwin-Hall approximation, sd ~1
  float s = 0; for (int i = 0; i < 12; ++i) s += urand(); return s - 6.f;
}

static void test_constant_input_converges() {
  RssiFilter f;
  for (int i = 0; i < 50; ++i) f.add(-70, (uint32_t)i * 100);
  TEST_ASSERT_TRUE(f.valid());
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -70.f, f.value());
  TEST_ASSERT_EQUAL_UINT16(50, f.count());
}

static void test_first_sample_initialises() {
  RssiFilter f;
  TEST_ASSERT_FALSE(f.valid());
  f.add(-63, 1000);
  TEST_ASSERT_TRUE(f.valid());
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -63.f, f.value());
  TEST_ASSERT_EQUAL_INT(-63, f.lastRaw());
}

static void test_single_spike_is_rejected() {
  RssiFilter f;
  uint32_t t = 0;
  for (int i = 0; i < 20; ++i, t += 100) f.add(-75, t);
  f.add(-40, t); t += 100;                       // +35 dB one-packet spike
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -75.f, f.value());
  f.add(-99, t); t += 100;                       // deep fade, one packet
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -75.f, f.value());
  for (int i = 0; i < 5; ++i, t += 100) f.add(-75, t);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -75.f, f.value());
}

static void test_step_response_time_constant() {
  RssiFilter f(1200.f);
  uint32_t t = 0;
  for (int i = 0; i < 30; ++i, t += 100) f.add(-80, t);
  // Step to -60 at 10 Hz. After ~1.2 s plus the median's 2-sample delay the
  // EMA should have covered roughly 63 % of the 20 dB step.
  for (int i = 0; i < 14; ++i, t += 100) f.add(-60, t);
  float covered = (f.value() - (-80.f)) / 20.f;
  TEST_ASSERT_TRUE(covered > 0.50f);
  TEST_ASSERT_TRUE(covered < 0.80f);
  for (int i = 0; i < 60; ++i, t += 100) f.add(-60, t);
  TEST_ASSERT_FLOAT_WITHIN(0.2f, -60.f, f.value());
}

static void test_rate_independence() {
  // The same physical step reaches a similar level after the same wall time
  // whether packets come at 20 Hz or 4 Hz.
  RssiFilter fast, slow;
  uint32_t t;
  for (t = 0; t < 3000; t += 50) fast.add(-80, t);
  for (t = 0; t < 3000; t += 250) slow.add(-80, t);
  for (t = 3000; t <= 5000; t += 50) fast.add(-60, t);
  for (t = 3000; t <= 5000; t += 250) slow.add(-60, t);
  TEST_ASSERT_FLOAT_WITHIN(3.5f, fast.value(), slow.value());
}

static void test_gap_resets_history() {
  RssiFilter f(1200.f, 6000);
  for (uint32_t t = 0; t < 2000; t += 100) f.add(-90, t);
  f.add(-55, 2000 + 7000);                       // back after 7 s of silence
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -55.f, f.value());
  TEST_ASSERT_EQUAL_UINT16(1, f.count());
}

static void test_invalid_samples_ignored() {
  RssiFilter f;
  f.add(127, 0);       // HCI "not available"
  f.add(0, 10);
  f.add(-120, 20);
  TEST_ASSERT_FALSE(f.valid());
  f.add(-66, 30);
  TEST_ASSERT_TRUE(f.valid());
}

static void test_equal_timestamps_still_converge() {
  RssiFilter f;
  f.add(-90, 500);
  for (int i = 0; i < 80; ++i) f.add(-60, 500);  // a burst stamped identically
  TEST_ASSERT_FLOAT_WITHIN(1.0f, -60.f, f.value());
}

static void test_noise_reduction() {
  RssiFilter f;
  rng = 99;
  float sumSq = 0; int n = 0;
  for (uint32_t t = 0; t < 60000; t += 100) {
    int raw = (int)lroundf(-70.f + 6.f * gauss());
    f.add(raw, t);
    if (t > 5000) { float e = f.value() + 70.f; sumSq += e * e; ++n; }
  }
  float rms = sqrtf(sumSq / n);
  TEST_ASSERT_TRUE(rms < 2.0f);                   // 6 dB raw jitter -> < 2 dB
}

static void test_distance_model() {
  ZoneConfig c;
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f, estimateDistanceM(-60.f, -60, c.pathLossExp));
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 10.0f, estimateDistanceM(-85.f, -60, 2.5f));
  TEST_ASSERT_FLOAT_WITHIN(0.02f, 0.4f, estimateDistanceM(-50.f, -60, 2.5f));
  TEST_ASSERT_EQUAL_FLOAT(0.1f, estimateDistanceM(-10.f, -60, 2.5f));   // clamped
  TEST_ASSERT_EQUAL_FLOAT(99.f, estimateDistanceM(-120.f, -60, 2.0f));  // clamped
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 25.f, boundaryDb(10.f, 2.5f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.f, boundaryDb(1.f, 2.5f));
}

static void test_classify_path_loss() {
  ZoneConfig c;    // boundaries ~ -3.9, 11.9, 25.0 dB
  TEST_ASSERT_EQUAL((int)Zone::RightHere, (int)classifyPathLoss(-6.f, c));
  TEST_ASSERT_EQUAL((int)Zone::Near, (int)classifyPathLoss(0.f, c));
  TEST_ASSERT_EQUAL((int)Zone::Near, (int)classifyPathLoss(11.f, c));
  TEST_ASSERT_EQUAL((int)Zone::Around, (int)classifyPathLoss(13.f, c));
  TEST_ASSERT_EQUAL((int)Zone::Far, (int)classifyPathLoss(26.f, c));
}

static void test_approx_meters_text() {
  char b[16];
  formatApproxMeters(0.4f, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("<1 m", b);
  formatApproxMeters(2.4f, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("~2 m", b);
  formatApproxMeters(2.6f, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("~3 m", b);
  formatApproxMeters(14.f, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("10+ m", b);
}

static void test_tracker_first_fix_is_immediate() {
  ZoneConfig c;
  ZoneTracker z;
  TEST_ASSERT_FALSE(z.initialised());
  TEST_ASSERT_EQUAL((int)Zone::Around, (int)z.update(-80.f, -60, 0, c));   // 20 dB
  TEST_ASSERT_TRUE(z.initialised());
}

static void test_tracker_hysteresis_no_flapping() {
  // Filtered RSSI wandering +/-2.5 dB around the Near/Around boundary (11.9 dB)
  // must not toggle the zone, because 2.5 < the 3 dB hysteresis band.
  ZoneConfig c;
  ZoneTracker z;
  const int ref = -60;
  z.update(ref - 11.9f - 0.1f, ref, 0, c);
  Zone first = z.zone();
  int changes = 0;
  Zone last = first;
  rng = 7;
  for (uint32_t t = 100; t < 120000; t += 100) {
    float rssi = ref - 11.9f + (urand() * 5.f - 2.5f);
    Zone now = z.update(rssi, ref, t, c);
    if (now != last) { ++changes; last = now; }
  }
  TEST_ASSERT_EQUAL(0, changes);
}

static void test_tracker_moves_closer_after_dwell() {
  ZoneConfig c;
  ZoneTracker z;
  const int ref = -60;
  z.update(-80.f, ref, 0, c);                         // Around (20 dB)
  // Jump to clearly Near (5 dB) at t = 100. Not committed before the dwell...
  uint32_t t = 100;
  for (; t < 100 + c.dwellCloserMs; t += 50)
    TEST_ASSERT_EQUAL((int)Zone::Around, (int)z.update(-65.f, ref, t, c));
  TEST_ASSERT_EQUAL((int)Zone::Near, (int)z.pending());
  // ...committed once it has persisted that long.
  z.update(-65.f, ref, t, c);
  TEST_ASSERT_EQUAL((int)Zone::Near, (int)z.zone());
}

static void test_tracker_moving_away_needs_longer_dwell() {
  ZoneConfig c;
  TEST_ASSERT_TRUE(c.dwellFartherMs > c.dwellCloserMs);
  ZoneTracker z;
  const int ref = -60;
  z.update(-63.f, ref, 0, c);                         // Near (3 dB)
  uint32_t t = 100;
  for (; t < 100 + c.dwellFartherMs; t += 50)
    TEST_ASSERT_EQUAL((int)Zone::Near, (int)z.update(-80.f, ref, t, c));
  z.update(-80.f, ref, t, c);
  TEST_ASSERT_EQUAL((int)Zone::Around, (int)z.zone());
}

static void test_tracker_brief_excursion_ignored() {
  ZoneConfig c;
  ZoneTracker z;
  const int ref = -60;
  z.update(-80.f, ref, 0, c);                         // Around
  // A dip shorter than the dwell (one fade-free moment) must not commit.
  uint32_t t = 100;
  for (; t < 100 + c.dwellCloserMs - 100; t += 50) z.update(-62.f, ref, t, c);
  for (; t <= 4000; t += 50) z.update(-80.f, ref, t, c);
  TEST_ASSERT_EQUAL((int)Zone::Around, (int)z.zone());
}

static void test_tracker_multi_zone_jump() {
  ZoneConfig c;
  ZoneTracker z;
  const int ref = -60;
  z.update(-95.f, ref, 0, c);                         // Far (35 dB)
  TEST_ASSERT_EQUAL((int)Zone::Far, (int)z.zone());
  for (uint32_t t = 100; t <= 1000; t += 100) z.update(-50.f, ref, t, c);   // -10 dB
  TEST_ASSERT_EQUAL((int)Zone::RightHere, (int)z.zone());
}

// End-to-end statistics over many seeded walks with 5 dB raw jitter, using
// the engine's rule that the first fix needs 3 samples. A friend walks from
// 20 m to 1.5 m at 1 m/s, then stands there chatting for a minute.
static void test_filter_plus_tracker_walkup_statistics() {
  const int kWalks = 200;
  int flapsWhileStanding = 0, walksWithApproachReversal = 0, slowNear = 0, endNear = 0;
  for (int w = 0; w < kWalks; ++w) {
    rng = 1000u + (uint32_t)w;
    ZoneConfig c;
    RssiFilter f;
    ZoneTracker z;
    const int ref = -60;
    int last = -1;
    bool reversed = false;
    uint32_t nearAt = 0;
    for (uint32_t t = 0; t <= 18500 + 60000; t += 100) {
      float d = t >= 18500 ? 1.5f : 20.f - (float)t / 1000.f;
      float rssi = ref - 10.f * c.pathLossExp * log10f(d) + 5.f * gauss();
      f.add((int)lroundf(rssi), t);
      if (f.count() < 3) continue;
      int zone = (int)z.update(f.value(), ref, t, c);
      if (last >= 0 && zone > last) {
        if (t >= 18500) ++flapsWhileStanding; else reversed = true;
      }
      if (zone <= (int)Zone::Near && !nearAt) nearAt = t;
      last = zone;
    }
    if (reversed) ++walksWithApproachReversal;
    if (!nearAt || nearAt > 17000 + 4000) ++slowNear;       // crosses 3 m at t = 17 s
    if (last == (int)Zone::Near) ++endNear;
  }
  TEST_ASSERT_EQUAL_MESSAGE(0, flapsWhileStanding, "zone must hold steady at rest");
  TEST_ASSERT_TRUE_MESSAGE(walksWithApproachReversal <= kWalks / 20, "<= 5% of walks reverse once");
  TEST_ASSERT_TRUE_MESSAGE(slowNear <= kWalks / 20, "Near within 4 s of crossing 3 m in >= 95%");
  TEST_ASSERT_EQUAL(kWalks, endNear);
}

// A friend standing 6 m away for five minutes must never be called Near
// (that would be a false alert).
static void test_no_false_near_at_six_metres() {
  ZoneConfig c;
  const int ref = -60;
  for (int w = 0; w < 20; ++w) {
    rng = 500u + (uint32_t)w;
    RssiFilter f;
    ZoneTracker z;
    for (uint32_t t = 0; t <= 300000; t += 100) {
      float rssi = ref - 10.f * c.pathLossExp * log10f(6.f) + 6.f * gauss();
      f.add((int)lroundf(rssi), t);
      if (f.count() < 3) continue;
      Zone zone = z.update(f.value(), ref, t, c);
      TEST_ASSERT_TRUE(zone == Zone::Around || zone == Zone::Far);
    }
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_constant_input_converges);
  RUN_TEST(test_first_sample_initialises);
  RUN_TEST(test_single_spike_is_rejected);
  RUN_TEST(test_step_response_time_constant);
  RUN_TEST(test_rate_independence);
  RUN_TEST(test_gap_resets_history);
  RUN_TEST(test_invalid_samples_ignored);
  RUN_TEST(test_equal_timestamps_still_converge);
  RUN_TEST(test_noise_reduction);
  RUN_TEST(test_distance_model);
  RUN_TEST(test_classify_path_loss);
  RUN_TEST(test_approx_meters_text);
  RUN_TEST(test_tracker_first_fix_is_immediate);
  RUN_TEST(test_tracker_hysteresis_no_flapping);
  RUN_TEST(test_tracker_moves_closer_after_dwell);
  RUN_TEST(test_tracker_moving_away_needs_longer_dwell);
  RUN_TEST(test_tracker_brief_excursion_ignored);
  RUN_TEST(test_tracker_multi_zone_jump);
  RUN_TEST(test_filter_plus_tracker_walkup_statistics);
  RUN_TEST(test_no_false_near_at_six_metres);
  return UNITY_END();
}
