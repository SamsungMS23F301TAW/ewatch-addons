// Shake detector tests: a real shake must trigger once, quickly, and end
// soon after it stops; walking, running, jolts, wrist turns and tremor must
// never trigger. Signals are synthetic but sampled the way taskIO samples:
// about 45 Hz with timing jitter, 14-bit quantised and clipped at +-2 g.
#include <math.h>
#include <stdint.h>
#include <unity.h>

#include <functional>
#include <initializer_list>

#include "shake_detector.h"

using namespace oracle;

namespace {

constexpr float kPi = 3.14159265f;

struct Lcg {
  uint32_t s;
  explicit Lcg(uint32_t seed) : s(seed) {}
  float unit() {   // [0, 1)
    s = s * 1664525u + 1013904223u;
    return (s >> 8) * (1.0f / 16777216.0f);
  }
  float sym() { return unit() * 2.f - 1.f; }
};

struct Vec { float x, y, z; };
using Signal = std::function<Vec(float tS)>;

// The resting wrist: watch held up to read, screen tilted toward the face.
Vec rest() { return {0.0f, 0.55f, 0.83f}; }

float quantise(float g) {
  float c = roundf(g * 4096.f);
  if (c > 8191.f) c = 8191.f;
  if (c < -8192.f) c = -8192.f;
  return c / 4096.f;
}

struct Result {
  int   starts = 0, stops = 0;
  float firstStart = -1, firstStop = -1;
};

Result run(const Signal &sig, float seconds, uint32_t t0 = 1000, uint32_t seed = 7,
           float noiseG = 0.01f, uint32_t gapAt = 0, uint32_t gapMs = 0) {
  ShakeDetector det;
  Lcg rng(seed);
  Result r;
  uint32_t t = 0;
  while (t < (uint32_t)(seconds * 1000.f)) {
    if (!(gapMs && t >= gapAt && t < gapAt + gapMs)) {
      Vec a = sig(t * 0.001f);
      float ax = quantise(a.x + noiseG * rng.sym());
      float ay = quantise(a.y + noiseG * rng.sym());
      float az = quantise(a.z + noiseG * rng.sym());
      auto ev = det.feed(t0 + t, ax, ay, az);
      if (ev == ShakeDetector::Event::Started) {
        r.starts++;
        if (r.firstStart < 0) r.firstStart = t * 0.001f;
      } else if (ev == ShakeDetector::Event::Stopped) {
        r.stops++;
        if (r.firstStop < 0) r.firstStop = t * 0.001f;
      }
    }
    t += (uint32_t)(22 + (int)(rng.sym() * 4.f));   // ~45 Hz, jittery, like taskIO
  }
  // Let the detector notice the end even if samples stopped.
  if (det.idle(t0 + t + 1000) == ShakeDetector::Event::Stopped) {
    r.stops++;
    if (r.firstStop < 0) r.firstStop = (t + 1000) * 0.001f;
  }
  return r;
}

// A deliberate shake along a unit direction, with a short ramp in and out.
Signal shake(float from, float to, float hz, float ampG, Vec dir) {
  return [=](float t) {
    Vec a = rest();
    if (t >= from && t < to) {
      float env = fminf(1.f, (t - from) / 0.1f) * fminf(1.f, (to - t) / 0.08f);
      float s = env * ampG * sinf(2.f * kPi * hz * (t - from));
      a.x += dir.x * s;
      a.y += dir.y * s;
      a.z += dir.z * s;
    }
    return a;
  };
}

void assertShakeDetected(const Result &r, float from, float to) {
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, r.starts, "exactly one start");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, r.stops, "exactly one stop");
  TEST_ASSERT_TRUE_MESSAGE(r.firstStart > from, "starts after the shake begins");
  TEST_ASSERT_TRUE_MESSAGE(r.firstStart < from + 1.0f, "starts within a second");
  TEST_ASSERT_TRUE_MESSAGE(r.firstStop > to, "stops after the shake ends");
  TEST_ASSERT_TRUE_MESSAGE(r.firstStop < to + 0.75f, "stops promptly");
}

}  // namespace

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------- triggers

void test_side_to_side_shake_triggers_once() {
  Result r = run(shake(1.0f, 2.5f, 4.0f, 1.8f, {1, 0, 0}), 5.f);
  assertShakeDetected(r, 1.0f, 2.5f);
}

void test_shake_along_every_axis_triggers() {
  const Vec dirs[] = {{0, 1, 0}, {0, 0, 1}, {0.7071f, 0.7071f, 0}, {0.577f, -0.577f, 0.577f}};
  for (const Vec &d : dirs) {
    Result r = run(shake(1.0f, 2.4f, 4.5f, 1.6f, d), 5.f);
    assertShakeDetected(r, 1.0f, 2.4f);
  }
}

void test_slow_and_fast_shakes_trigger() {
  Result slow = run(shake(1.0f, 2.8f, 2.4f, 2.0f, {1, 0, 0}), 5.f);
  assertShakeDetected(slow, 1.0f, 2.8f);
  Result fast = run(shake(1.0f, 2.2f, 6.5f, 1.3f, {1, 0, 0}), 5.f);
  assertShakeDetected(fast, 1.0f, 2.2f);
}

void test_stirring_circular_motion_triggers() {
  Signal sig = [](float t) {
    Vec a = rest();
    if (t >= 1.0f && t < 2.6f) {
      float w = 2.f * kPi * 3.5f * (t - 1.0f);
      a.x += 1.6f * cosf(w);
      a.y += 1.6f * sinf(w);
    }
    return a;
  };
  Result r = run(sig, 5.f);
  assertShakeDetected(r, 1.0f, 2.6f);
}

void test_violent_shake_clipped_at_2g_triggers() {
  Result r = run(shake(1.0f, 2.2f, 5.f, 3.5f, {1, 0, 0}), 5.f);
  assertShakeDetected(r, 1.0f, 2.2f);
}

void test_long_shake_does_not_flap() {
  Result r = run(shake(1.0f, 7.0f, 4.0f, 1.7f, {1, 0, 0}), 9.f);
  assertShakeDetected(r, 1.0f, 7.0f);
}

void test_shake_survives_a_short_sample_gap() {
  // taskIO hiccups for 150 ms in the middle of the shake.
  Result r = run(shake(1.0f, 2.6f, 4.0f, 1.8f, {1, 0, 0}), 5.f, 1000, 7, 0.01f, 1500, 150);
  TEST_ASSERT_EQUAL_INT(1, r.starts);
  TEST_ASSERT_EQUAL_INT(1, r.stops);
}

void test_millis_wraparound() {
  Result r = run(shake(1.0f, 2.5f, 4.0f, 1.8f, {1, 0, 0}), 5.f, 0xFFFFF000u);
  assertShakeDetected(r, 1.0f, 2.5f);
}

void test_shake_ends_when_samples_stop() {
  ShakeDetector det;
  Lcg rng(3);
  uint32_t t = 0;
  bool started = false;
  for (; t < 1500; t += 22) {
    float s = 1.8f * sinf(2.f * kPi * 4.f * t * 0.001f);
    if (det.feed(t, quantise(s), 0.55f, 0.83f) == ShakeDetector::Event::Started) started = true;
  }
  TEST_ASSERT_TRUE(started);
  TEST_ASSERT_TRUE(det.shaking());
  // No more samples (IMU read failing); the view calls idle() each frame.
  TEST_ASSERT_EQUAL(ShakeDetector::Event::None, det.idle(t + 100));
  TEST_ASSERT_EQUAL(ShakeDetector::Event::Stopped, det.idle(t + 600));
  TEST_ASSERT_FALSE(det.shaking());
}

void test_intensity_tracks_vigour() {
  ShakeDetector soft, hard;
  for (uint32_t t = 0; t < 2000; t += 22) {
    float w = 2.f * kPi * 4.f * t * 0.001f;
    soft.feed(t, quantise(1.1f * sinf(w)), 0.55f, 0.83f);
    hard.feed(t, quantise(2.5f * sinf(w)), 0.55f, 0.83f);
  }
  TEST_ASSERT_TRUE(soft.shaking());
  TEST_ASSERT_TRUE(hard.shaking());
  TEST_ASSERT_TRUE(hard.intensity() > soft.intensity());
  TEST_ASSERT_TRUE(hard.intensity() <= 1.f);
}

void test_irregular_human_shakes_trigger() {
  // People don't shake like metronomes: every half-swing here varies by up
  // to +-30% in length and amplitude, at 2.5-6 Hz and 1.2-2.4 g.
  Lcg rng(31337);
  int detected = 0;
  const int kTrials = 100;
  for (int k = 0; k < kTrials; k++) {
    float hz = 2.5f + 3.5f * rng.unit(), amp = 1.2f + 1.2f * rng.unit();
    float edges[64], amps[64];
    int n = 0;
    float t = 1.0f;
    edges[n] = t;
    amps[n++] = amp * (0.7f + 0.6f * rng.unit());
    while (t < 2.6f && n < 63) {
      t += (0.5f / hz) * (1.f + 0.3f * rng.sym());
      edges[n] = t;
      amps[n++] = amp * (0.7f + 0.6f * rng.unit());
    }
    Signal sig = [=](float ts) {
      Vec a = rest();
      for (int i = 0; i + 1 < n; i++)
        if (ts >= edges[i] && ts < edges[i + 1])
          a.x += (i % 2 ? -1.f : 1.f) * amps[i] * sinf(kPi * (ts - edges[i]) / (edges[i + 1] - edges[i]));
      return a;
    };
    Result r = run(sig, 4.f, 1000, 500 + k);
    if (r.starts == 1 && r.firstStart < 2.0f) detected++;
  }
  TEST_ASSERT_TRUE_MESSAGE(detected >= 98, "irregular shakes are still shakes");
}

void test_alternating_gaps_do_not_chain() {
  // Strong opposite lobes 110 ms then 240 ms apart, over and over: the beat
  // a foot strike and its rebound make. Off-rhythm, so it never chains.
  Signal sig = [](float t) {
    Vec a = rest();
    float period = 0.35f, u = fmodf(t, period);
    if (u < 0.05f) a.y += 1.6f * sinf(kPi * u / 0.05f);
    else if (u >= 0.11f && u < 0.16f) a.y -= 1.4f * sinf(kPi * (u - 0.11f) / 0.05f);
    return a;
  };
  TEST_ASSERT_EQUAL_INT(0, run(sig, 30.f).starts);
}

// ---------------------------------------------------------------- rejects

void test_walking_never_triggers() {
  // Arm swing at half the step rate, a vertical bounce at the step rate,
  // heel-strike transients and the wrist pitching with the swing.
  for (float cadence : {1.6f, 1.9f, 2.2f}) {
    for (float swing : {0.45f, 0.8f}) {
      Signal sig = [=](float t) {
        float ph = 2.f * kPi * (cadence * 0.5f) * t;
        float tilt = 0.26f * sinf(ph);   // ~15 degrees of pitch
        Vec a = {0.f, 0.55f * cosf(tilt) - 0.83f * sinf(tilt) * 0.3f, 0.83f * cosf(tilt)};
        a.x += swing * sinf(ph);
        a.y += 0.25f * sinf(2.f * kPi * cadence * t);
        float since = fmodf(t, 1.f / cadence);
        a.y += 0.6f * expf(-since / 0.03f) * (since < 0.12f ? 1.f : 0.f);
        return a;
      };
      Result r = run(sig, 60.f, 1000, 11, 0.03f);
      TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.starts, "walking must not trigger");
    }
  }
}

void test_running_never_triggers() {
  // Faster cadence, a strong arm swing and sharp asymmetric foot strikes.
  Signal sig = [](float t) {
    const float cadence = 2.8f;
    float ph = 2.f * kPi * (cadence * 0.5f) * t;
    Vec a = rest();
    a.x += 1.2f * sinf(ph);
    float since = fmodf(t, 1.f / cadence);
    a.y += 1.3f * expf(-since / 0.035f) - 0.35f * expf(-fabsf(since - 0.12f) / 0.05f);
    a.z += 0.3f * sinf(2.f * ph);
    return a;
  };
  Result r = run(sig, 60.f, 1000, 13, 0.05f);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.starts, "running must not trigger");
}

void test_single_jolts_never_trigger() {
  // Fifty knocks in random directions, each ringing out at 6-20 Hz.
  Lcg rng(99);
  for (int k = 0; k < 50; k++) {
    float th = rng.unit() * 2.f * kPi, ph = (rng.unit() - 0.5f) * kPi;
    Vec d = {cosf(ph) * cosf(th), cosf(ph) * sinf(th), sinf(ph)};
    float peak = 2.f + 2.f * rng.unit();
    float hz = 6.f + 14.f * rng.unit();
    float tau = 0.03f + 0.05f * rng.unit();
    Signal sig = [=](float t) {
      Vec a = rest();
      if (t >= 1.0f) {
        float u = t - 1.0f;
        float s = peak * expf(-u / tau) * cosf(2.f * kPi * hz * u);
        a.x += d.x * s;
        a.y += d.y * s;
        a.z += d.z * s;
      }
      return a;
    };
    Result r = run(sig, 3.f, 1000, 100 + k);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.starts, "a single jolt must not trigger");
  }
}

void test_two_knocks_never_trigger() {
  // Two sharp knocks 250 ms apart (tapping the watch on a table twice).
  Signal sig = [](float t) {
    Vec a = rest();
    for (float at : {1.0f, 1.25f}) {
      if (t >= at) {
        float u = t - at;
        a.x += 3.f * expf(-u / 0.04f) * cosf(2.f * kPi * 12.f * u);
      }
    }
    return a;
  };
  Result r = run(sig, 3.f);
  TEST_ASSERT_EQUAL_INT(0, r.starts);
}

void test_wrist_turns_never_trigger() {
  // Raise-to-look flicks, then twisting the wrist back and forth at 1 Hz.
  Signal flick = [](float t) {
    float k = fminf(1.f, fmaxf(0.f, (t - 1.0f) / 0.35f));
    float ang = kPi * 0.5f * (k * k * (3.f - 2.f * k));
    return Vec{0.f, sinf(ang) * 0.55f, cosf(ang) * 0.83f + (1.f - cosf(ang)) * -0.2f};
  };
  TEST_ASSERT_EQUAL_INT(0, run(flick, 3.f).starts);
  Signal twist = [](float t) {
    float ang = 1.2f * sinf(2.f * kPi * 1.0f * t);
    return Vec{sinf(ang), 0.55f * cosf(ang), 0.83f * cosf(ang)};
  };
  TEST_ASSERT_EQUAL_INT(0, run(twist, 20.f).starts);
}

void test_tremor_and_typing_never_trigger() {
  Signal sig = [](float t) {
    Vec a = rest();
    a.x += 0.15f * sinf(2.f * kPi * 9.f * t);
    a.y += 0.12f * sinf(2.f * kPi * 11.f * t + 1.f);
    return a;
  };
  TEST_ASSERT_EQUAL_INT(0, run(sig, 20.f, 1000, 5, 0.05f).starts);
}

void test_gentle_wobble_below_threshold_does_not_trigger() {
  Result r = run(shake(1.0f, 4.0f, 4.0f, 0.6f, {1, 0, 0}), 5.f);
  TEST_ASSERT_EQUAL_INT(0, r.starts);
}

void test_reset_clears_everything() {
  ShakeDetector det;
  for (uint32_t t = 0; t < 1500; t += 22)
    det.feed(t, quantise(1.8f * sinf(2.f * kPi * 4.f * t * 0.001f)), 0.55f, 0.83f);
  TEST_ASSERT_TRUE(det.shaking());
  det.reset();
  TEST_ASSERT_FALSE(det.shaking());
  TEST_ASSERT_EQUAL_FLOAT(0.f, det.intensity());
  TEST_ASSERT_EQUAL_UINT32(0, det.reversals());
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_side_to_side_shake_triggers_once);
  RUN_TEST(test_shake_along_every_axis_triggers);
  RUN_TEST(test_slow_and_fast_shakes_trigger);
  RUN_TEST(test_stirring_circular_motion_triggers);
  RUN_TEST(test_violent_shake_clipped_at_2g_triggers);
  RUN_TEST(test_long_shake_does_not_flap);
  RUN_TEST(test_shake_survives_a_short_sample_gap);
  RUN_TEST(test_millis_wraparound);
  RUN_TEST(test_shake_ends_when_samples_stop);
  RUN_TEST(test_intensity_tracks_vigour);
  RUN_TEST(test_irregular_human_shakes_trigger);
  RUN_TEST(test_alternating_gaps_do_not_chain);
  RUN_TEST(test_walking_never_triggers);
  RUN_TEST(test_running_never_triggers);
  RUN_TEST(test_single_jolts_never_trigger);
  RUN_TEST(test_two_knocks_never_trigger);
  RUN_TEST(test_wrist_turns_never_trigger);
  RUN_TEST(test_tremor_and_typing_never_trigger);
  RUN_TEST(test_gentle_wobble_below_threshold_does_not_trigger);
  RUN_TEST(test_reset_clears_everything);
  return UNITY_END();
}
