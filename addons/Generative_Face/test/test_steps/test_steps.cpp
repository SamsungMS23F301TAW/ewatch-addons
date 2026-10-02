// Step detector tests against synthetic wrist-accelerometer signals at the
// watch's 50 Hz FIFO rate. Every signal is generated from a fixed seed, so
// the expected counts are stable.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <vector>
#include <unity.h>
#include "steps.h"

using gf::StepDetector;

namespace {
const float kFs = 50.0f;
const float kPi = 3.14159265f;

struct Rand {
  uint64_t s;
  explicit Rand(uint64_t seed) : s(seed * 2654435761u + 1) {}
  float uni() { s = s * 6364136223846793005ULL + 1442695040888963407ULL; return (float)((s >> 40) & 0xFFFFFF) / 16777216.0f; }
  float range(float a, float b) { return a + (b - a) * uni(); }
  float gauss() { float u = uni() + 1e-7f, v = uni(); return sqrtf(-2.0f * logf(u)) * cosf(2 * kPi * v); }
};

struct Signal {
  std::vector<float> ax, ay, az;     // g
  int expectedSteps = 0;
  void resize(size_t n) { ax.assign(n, 0); ay.assign(n, 0); az.assign(n, 0); }
};

// Gravity that slowly wanders like a wrist, plus white noise.
void addGravityAndNoise(Signal &s, Rand &r, float noiseG) {
  float th = 0.4f, ph = 0.3f;
  for (size_t i = 0; i < s.ax.size(); i++) {
    float t = (float)i / kFs;
    th = 0.5f + 0.35f * sinf(0.11f * t) + 0.1f * sinf(0.37f * t + 1.0f);
    ph = 0.6f * sinf(0.07f * t + 0.5f);
    s.ax[i] += sinf(th) * cosf(ph) + noiseG * r.gauss();
    s.ay[i] += sinf(th) * sinf(ph) + noiseG * r.gauss();
    s.az[i] += cosf(th) + noiseG * r.gauss();
  }
}

// Steps: a heel-strike bump and a softer rebound along the gravity axis,
// plus an arm swing at half the cadence across it.
Signal walking(float seconds, float cadenceHz, float ampG, float swingG, float jitter,
               uint64_t seed) {
  Signal s;
  Rand r(seed);
  size_t n = (size_t)(seconds * kFs);
  s.resize(n);
  std::vector<float> vert(n, 0.0f);
  float t = 0.6f;
  int steps = 0;
  while (t < seconds - 0.3f) {
    float a = ampG * r.range(0.8f, 1.2f);
    for (size_t i = 0; i < n; i++) {
      float dt = (float)i / kFs - t;
      if (dt < -0.3f || dt > 0.5f) continue;
      vert[i] += a * expf(-(dt / 0.05f) * (dt / 0.05f)) -
                 0.45f * a * expf(-((dt - 0.13f) / 0.08f) * ((dt - 0.13f) / 0.08f));
    }
    steps++;
    t += (1.0f / cadenceHz) * r.range(1.0f - jitter, 1.0f + jitter);
  }
  for (size_t i = 0; i < n; i++) {
    float tt = (float)i / kFs;
    float swing = swingG * sinf(2 * kPi * (cadenceHz / 2) * tt);
    s.az[i] += vert[i];
    s.ax[i] += swing;
  }
  addGravityAndNoise(s, r, 0.012f);
  s.expectedSteps = steps;
  return s;
}

uint32_t run(StepDetector &d, const Signal &s) {
  uint32_t total = 0;
  for (size_t i = 0; i < s.ax.size(); i++) {
    auto c = [](float g) {
      float v = g * 4096.0f;
      if (v > 8191) v = 8191;
      if (v < -8192) v = -8192;
      return (int16_t)lrintf(v);
    };
    total += d.push(c(s.ax[i]), c(s.ay[i]), c(s.az[i]));
  }
  return total;
}

void expectNear(uint32_t got, int want, float tol, const char *what) {
  char msg[96];
  snprintf(msg, sizeof(msg), "%s: counted %u, generated %d", what, (unsigned)got, want);
  float err = fabsf((float)got - (float)want) / (float)want;
  TEST_ASSERT_TRUE_MESSAGE(err <= tol, msg);
  printf("  %s\n", msg);
}
}  // namespace

void test_walking_normal() {
  StepDetector d(kFs);
  Signal s = walking(120, 1.85f, 0.35f, 0.15f, 0.04f, 1);
  expectNear(run(d, s), s.expectedSteps, 0.05f, "walk 1.85 Hz");
}

void test_walking_slow_and_soft() {
  StepDetector d(kFs);
  Signal s = walking(90, 1.3f, 0.16f, 0.08f, 0.05f, 2);
  expectNear(run(d, s), s.expectedSteps, 0.08f, "slow walk 1.3 Hz");
}

void test_running() {
  StepDetector d(kFs);
  Signal s = walking(60, 2.9f, 1.1f, 0.4f, 0.03f, 3);
  expectNear(run(d, s), s.expectedSteps, 0.05f, "run 2.9 Hz");
}

void test_irregular_gait() {
  StepDetector d(kFs);
  Signal s = walking(120, 1.7f, 0.3f, 0.15f, 0.12f, 4);
  expectNear(run(d, s), s.expectedSteps, 0.10f, "irregular walk");
}

// Robustness: many walkers (cadence, impact, arm swing, rhythm jitter vary).
void test_many_walkers() {
  Rand pick(77);
  int worst = 0;
  for (int k = 0; k < 12; k++) {
    float cad = pick.range(1.45f, 2.3f);
    float amp = pick.range(0.18f, 0.6f);
    float swing = pick.range(0.05f, 0.35f);
    float jit = pick.range(0.02f, 0.08f);
    StepDetector d(kFs);
    Signal s = walking(90, cad, amp, swing, jit, 100 + k);
    uint32_t got = run(d, s);
    float err = fabsf((float)got - (float)s.expectedSteps) / (float)s.expectedSteps;
    int pct = (int)(err * 1000);
    if (pct > worst) worst = pct;
    char msg[128];
    snprintf(msg, sizeof(msg), "walker %d: %.2f Hz amp %.2f swing %.2f -> %u/%d", k, cad, amp,
             swing, (unsigned)got, s.expectedSteps);
    printf("  %s\n", msg);
    TEST_ASSERT_TRUE_MESSAGE(err <= 0.07f, msg);
  }
  printf("  worst walker error %.1f%%\n", worst / 10.0f);
}

void test_typing_counts_nothing() {
  StepDetector d(kFs);
  Rand r(5);
  Signal s;
  size_t n = (size_t)(180 * kFs);
  s.resize(n);
  float t = 0;
  while (t < 175) {
    float len = r.range(0.4f, 2.5f);
    float f = r.range(7.0f, 12.0f), a = r.range(0.015f, 0.04f);
    for (size_t i = (size_t)(t * kFs); i < (size_t)((t + len) * kFs) && i < n; i++) {
      float tt = (float)i / kFs;
      s.ax[i] += a * sinf(2 * kPi * f * tt) * r.range(0.5f, 1.0f);
      s.ay[i] += 0.6f * a * r.gauss();
    }
    t += len + r.range(0.3f, 3.0f);
    // An occasional slow wrist repositioning.
    if (r.uni() < 0.2f) {
      for (size_t i = (size_t)(t * kFs); i < (size_t)((t + 1.0f) * kFs) && i < n; i++) {
        float u = ((float)i / kFs - t);
        s.az[i] += 0.12f * sinf(kPi * u);
      }
    }
  }
  addGravityAndNoise(s, r, 0.01f);
  uint32_t got = run(d, s);
  printf("  typing 180 s: %u steps\n", (unsigned)got);
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(3, got);
}

void test_car_ride_counts_little() {
  StepDetector d(kFs);
  Rand r(6);
  Signal s;
  size_t n = (size_t)(300 * kFs);
  s.resize(n);
  for (size_t i = 0; i < n; i++) {
    float t = (float)i / kFs;
    s.az[i] += 0.07f * sinf(2 * kPi * 0.31f * t) + 0.05f * sinf(2 * kPi * 0.73f * t + 1.0f);
    s.ax[i] += 0.04f * sinf(2 * kPi * 0.21f * t + 2.0f);
    s.az[i] += 0.05f * sinf(2 * kPi * 18.0f * t);                  // engine
  }
  float t = 1.0f;
  while (t < 298) {                                                 // road bumps
    float a = r.range(0.2f, 0.6f);
    for (size_t i = (size_t)(t * kFs); i < (size_t)((t + 0.15f) * kFs) && i < n; i++) {
      float u = ((float)i / kFs - t) / 0.15f;
      s.az[i] += a * sinf(kPi * u);
    }
    t += r.range(0.6f, 6.0f);
  }
  addGravityAndNoise(s, r, 0.015f);
  uint32_t got = run(d, s);
  printf("  car ride 300 s: %u steps\n", (unsigned)got);
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(12, got);
}

void test_single_jolts_count_nothing() {
  StepDetector d(kFs);
  Rand r(7);
  Signal s;
  size_t n = (size_t)(120 * kFs);
  s.resize(n);
  float t = 2.0f;
  while (t < 118) {
    for (size_t i = (size_t)(t * kFs); i < (size_t)((t + 0.1f) * kFs) && i < n; i++) {
      float u = ((float)i / kFs - t) / 0.1f;
      s.ax[i] += 1.4f * sinf(kPi * u);
      s.az[i] -= 0.8f * sinf(kPi * u);
    }
    t += r.range(3.0f, 8.0f);
  }
  addGravityAndNoise(s, r, 0.01f);
  uint32_t got = run(d, s);
  printf("  jolts 120 s: %u steps\n", (unsigned)got);
  TEST_ASSERT_EQUAL_UINT32(0, got);
}

void test_arm_waving_bursts_count_little() {
  StepDetector d(kFs);
  Rand r(8);
  Signal s;
  size_t n = (size_t)(120 * kFs);
  s.resize(n);
  float t = 1.0f;
  while (t < 115) {
    float f = r.range(1.4f, 2.6f), a = r.range(0.4f, 0.9f);
    float len = r.range(1.0f, 1.8f);                       // two or three waves
    for (size_t i = (size_t)(t * kFs); i < (size_t)((t + len) * kFs) && i < n; i++) {
      float u = (float)i / kFs - t;
      s.ax[i] += a * sinf(2 * kPi * f * u);
      s.az[i] += 0.5f * a * sinf(2 * kPi * f * u + 0.8f);
    }
    t += len + r.range(2.5f, 6.0f);
  }
  addGravityAndNoise(s, r, 0.01f);
  uint32_t got = run(d, s);
  printf("  arm waving 120 s: %u steps\n", (unsigned)got);
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(6, got);
}

void test_walk_after_gap_resumes() {
  StepDetector d(kFs);
  Signal a = walking(60, 1.8f, 0.35f, 0.15f, 0.04f, 9);
  Signal b = walking(60, 1.8f, 0.35f, 0.15f, 0.04f, 10);
  uint32_t got = run(d, a);
  d.gap();
  got += run(d, b);
  expectNear(got, a.expectedSteps + b.expectedSteps, 0.06f, "walk, gap, walk");
}

void test_still_watch_counts_nothing() {
  StepDetector d(kFs);
  Rand r(11);
  Signal s;
  s.resize((size_t)(600 * kFs));
  addGravityAndNoise(s, r, 0.02f);
  uint32_t got = run(d, s);
  TEST_ASSERT_EQUAL_UINT32(0, got);
}

void test_reset_clears_total() {
  StepDetector d(kFs);
  Signal s = walking(30, 1.8f, 0.35f, 0.15f, 0.04f, 12);
  run(d, s);
  TEST_ASSERT_TRUE(d.total() > 30);
  d.reset();
  TEST_ASSERT_EQUAL_UINT32(0, d.total());
  TEST_ASSERT_FALSE(d.walking());
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_walking_normal);
  RUN_TEST(test_walking_slow_and_soft);
  RUN_TEST(test_running);
  RUN_TEST(test_irregular_gait);
  RUN_TEST(test_many_walkers);
  RUN_TEST(test_typing_counts_nothing);
  RUN_TEST(test_car_ride_counts_little);
  RUN_TEST(test_single_jolts_count_nothing);
  RUN_TEST(test_arm_waving_bursts_count_little);
  RUN_TEST(test_walk_after_gap_resumes);
  RUN_TEST(test_still_watch_counts_nothing);
  RUN_TEST(test_reset_clears_total);
  return UNITY_END();
}
