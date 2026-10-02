// Host tests for the step detector + day book (src/core/steps.cpp).
//
// The detector is fed synthetic wrist-accelerometer signals that are built in
// continuous time, rotated by an arm-swing model, point-sampled at the FIFO
// ODR (no anti-alias filter: pessimistic), quantised to the MMA8451's 14-bit
// counts and clipped at ±2 g — the same shape the watch hands the detector.
#include <unity.h>
#include <math.h>
#include <stdio.h>
#include <random>
#include <vector>
#include "steps.h"

using steps::Detector;
using steps::DetectorConfig;
using steps::StepBook;

namespace {

const double PI = 3.14159265358979323846;

struct V3 { double x, y, z; };

V3 add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 mul(V3 a, double k) { return {a.x * k, a.y * k, a.z * k}; }

// Rotate v about the X axis by a (rad), then about Z by b.
V3 rot(V3 v, double a, double b) {
  double ca = cos(a), sa = sin(a);
  V3 r1 = {v.x, ca * v.y - sa * v.z, sa * v.y + ca * v.z};
  double cb = cos(b), sb = sin(b);
  return {cb * r1.x - sb * r1.y, sb * r1.x + cb * r1.y, r1.z};
}

double gauss(double t, double c, double w) {
  double u = (t - c) / w;
  return exp(-0.5 * u * u);
}

// A continuous-time signal model: returns specific force in the sensor frame (g).
struct Model {
  virtual ~Model() {}
  virtual V3 at(double t) = 0;
};

// ---- Walking / running ---------------------------------------------------
struct Walk : Model {
  std::vector<double> times;   // step instants
  double bounce, heel, swing, swingDeg, tiltDeg;
  double noise;
  std::mt19937 rng;
  std::normal_distribution<double> nd{0.0, 1.0};

  Walk(int nSteps, double t0, double cadenceHz, double jitter, double bounceG,
       double heelG, double swingG, uint32_t seed, double noiseG = 0.01)
      : bounce(bounceG), heel(heelG), swing(swingG), swingDeg(25), tiltDeg(35),
        noise(noiseG), rng(seed) {
    std::normal_distribution<double> j(0.0, jitter);
    double t = t0;
    for (int i = 0; i < nSteps; i++) {
      times.push_back(t);
      double T = (1.0 / cadenceHz) * (1.0 + j(rng));
      if (T < 0.2) T = 0.2;
      t += T;
    }
  }
  double end() const { return times.empty() ? 0 : times.back() + 0.6; }

  // Index of the step whose interval contains t, and the phase within it.
  bool phase(double t, int &k, double &ph) const {
    if (times.size() < 2 || t < times.front() || t >= times.back()) return false;
    // binary search
    size_t lo = 0, hi = times.size() - 1;
    while (hi - lo > 1) {
      size_t mid = (lo + hi) / 2;
      if (times[mid] <= t) lo = mid; else hi = mid;
    }
    k = (int)lo;
    ph = (t - times[lo]) / (times[lo + 1] - times[lo]);
    return true;
  }

  V3 at(double t) override {
    V3 lin = {0, 0, 0};
    double swingAng = 0;
    int k; double ph;
    if (phase(t, k, ph)) {
      double strideAng = PI * (k + ph);                // one cycle per 2 steps
      double vert = bounce * cos(2 * PI * ph)          // body bounce
                  + heel * gauss(ph, 0.06, 0.05);      // heel-strike spike
      double fwd = swing * sin(2 * PI * ph + 0.4)      // centripetal-ish (step freq)
                 + 0.6 * swing * sin(strideAng);       // swing (stride freq)
      lin = {0.3 * swing * sin(strideAng + 1.0), fwd, vert};
      swingAng = (swingDeg * PI / 180.0) * sin(strideAng);
    }
    // Specific force in world frame: gravity reaction (+1 g up) + linear accel.
    V3 f = add({0, 0, 1.0}, lin);
    V3 s = rot(f, tiltDeg * PI / 180.0 + swingAng, 0.3);
    s = add(s, {noise * nd(rng), noise * nd(rng), noise * nd(rng)});
    return s;
  }
};

// ---- Still / fidgeting -----------------------------------------------------
struct Still : Model {
  std::mt19937 rng;
  std::normal_distribution<double> nd{0.0, 1.0};
  double noise;
  std::vector<std::pair<double, double>> fidgets;   // (time, amplitude)
  Still(uint32_t seed, double dur, double noiseG, int nFidgets, double fidgetG)
      : rng(seed), noise(noiseG) {
    std::uniform_real_distribution<double> u(0, dur);
    for (int i = 0; i < nFidgets; i++) fidgets.push_back({u(rng), fidgetG});
  }
  V3 at(double t) override {
    double a = 0.4 + 0.2 * sin(t * 0.05), b = 0.1 * sin(t * 0.031);
    double f = 0;
    for (auto &p : fidgets) f += p.second * gauss(t, p.first, 0.12);
    V3 s = rot({0, 0.3 * f, 1.0 + f}, a, b);
    return add(s, {noise * nd(rng), noise * nd(rng), noise * nd(rng)});
  }
};

// ---- Typing: fast irregular small taps --------------------------------------
struct Typing : Model {
  std::vector<std::pair<double, V3>> taps;
  std::mt19937 rng;
  std::normal_distribution<double> nd{0.0, 1.0};
  Typing(uint32_t seed, double dur) : rng(seed) {
    std::exponential_distribution<double> gapd(1.0 / 0.16);   // ~6 taps/s
    std::uniform_real_distribution<double> amp(0.08, 0.35), dir(-1, 1);
    double t = 0.2;
    while (t < dur) {
      // occasional pauses (thinking)
      if (std::uniform_real_distribution<double>(0, 1)(rng) < 0.03) t += 1.5;
      V3 d = {dir(rng), dir(rng), dir(rng)};
      double n = sqrt(d.x * d.x + d.y * d.y + d.z * d.z) + 1e-9;
      taps.push_back({t, mul(d, amp(rng) / n)});
      t += 0.05 + gapd(rng);
    }
  }
  V3 at(double t) override {
    V3 lin = {0, 0, 0};
    for (auto &p : taps) {
      if (fabs(t - p.first) > 0.1) continue;
      lin = add(lin, mul(p.second, gauss(t, p.first, 0.015)));
    }
    V3 s = rot(add({0, 0, 1.0}, lin), 0.6 + 0.05 * sin(t * 0.7), 0.2);
    return add(s, {0.008 * nd(rng), 0.008 * nd(rng), 0.008 * nd(rng)});
  }
};

// ---- Car ride ------------------------------------------------------------------
struct Car : Model {
  std::mt19937 rng;
  std::normal_distribution<double> nd{0.0, 1.0};
  std::vector<std::pair<double, double>> bumps;   // (time, amplitude)
  double phases[6];
  Car(uint32_t seed, double dur) : rng(seed) {
    std::exponential_distribution<double> gapd(1.0 / 6.0);
    std::uniform_real_distribution<double> amp(0.15, 0.6);
    double t = 1.0;
    while (t < dur) { bumps.push_back({t, amp(rng)}); t += 1.0 + gapd(rng); }
    std::uniform_real_distribution<double> ph(0, 2 * PI);
    for (double &p : phases) p = ph(rng);
  }
  V3 at(double t) override {
    // body bounce: a few incommensurate low frequencies (narrowband 1-2 Hz)
    double v = 0.03 * sin(2 * PI * 1.13 * t + phases[0]) +
               0.025 * sin(2 * PI * 1.57 * t + phases[1]) +
               0.02 * sin(2 * PI * 1.91 * t + phases[2]);
    // engine / road vibration (aliases at 12.5 Hz when point-sampled)
    v += 0.04 * sin(2 * PI * 27.3 * t + phases[3]) + 0.03 * sin(2 * PI * 14.1 * t + phases[4]);
    // bumps: damped 1.6 Hz oscillation after each hit
    for (auto &b : bumps) {
      double dt = t - b.first;
      if (dt < 0 || dt > 2.0) continue;
      v += b.second * exp(-dt * 3.0) * sin(2 * PI * 1.6 * dt);
    }
    // gentle turns / braking
    double lat = 0.15 * sin(2 * PI * 0.03 * t + phases[5]);
    double lon = 0.1 * sin(2 * PI * 0.021 * t);
    V3 s = rot(add({0, 0, 1.0}, {lat, lon, v}), 0.5, 0.1);
    return add(s, {0.01 * nd(rng), 0.01 * nd(rng), 0.01 * nd(rng)});
  }
};

// ---- Gesturing: big, smooth, irregular arm movements ----------------------------
struct Gesture : Model {
  std::vector<std::pair<double, V3>> moves;
  std::mt19937 rng;
  std::normal_distribution<double> nd{0.0, 1.0};
  Gesture(uint32_t seed, double dur) : rng(seed) {
    std::uniform_real_distribution<double> gap(0.25, 1.8), amp(0.3, 0.9), dir(-1, 1);
    double t = 0.5;
    while (t < dur) {
      V3 d = {dir(rng), dir(rng), dir(rng)};
      double n = sqrt(d.x * d.x + d.y * d.y + d.z * d.z) + 1e-9;
      moves.push_back({t, mul(d, amp(rng) / n)});
      t += gap(rng);
    }
  }
  V3 at(double t) override {
    V3 lin = {0, 0, 0};
    double ang = 0;
    for (auto &m : moves) {
      double dt = t - m.first;
      if (dt < -0.6 || dt > 0.6) continue;
      // accelerate then decelerate (derivative of a gaussian)
      lin = add(lin, mul(m.second, -dt / 0.12 * gauss(t, m.first, 0.12)));
      ang += 0.5 * m.second.x * gauss(t, m.first, 0.25);
    }
    V3 s = rot(add({0, 0, 1.0}, lin), 0.5 + ang, 0.2 + ang);
    return add(s, {0.01 * nd(rng), 0.01 * nd(rng), 0.01 * nd(rng)});
  }
};

// ---- Isolated jolts -------------------------------------------------------------
struct Jolts : Model {
  std::vector<double> times;
  std::mt19937 rng;
  std::normal_distribution<double> nd{0.0, 1.0};
  Jolts(uint32_t seed, int n) : rng(seed) {
    std::uniform_real_distribution<double> gap(3.0, 10.0);
    double t = 1.0;
    for (int i = 0; i < n; i++) { times.push_back(t); t += gap(rng); }
  }
  double end() const { return times.back() + 3.0; }
  V3 at(double t) override {
    double v = 0;
    for (double j : times) {
      double dt = t - j;
      if (dt < 0 || dt > 1.5) continue;
      v += 1.1 * exp(-dt * 6.0) * cos(2 * PI * 2.2 * dt);   // knock + ring-down
    }
    V3 s = rot({0.2 * v, 0.1 * v, 1.0 + v}, 0.4, 0.1);
    return add(s, {0.01 * nd(rng), 0.01 * nd(rng), 0.01 * nd(rng)});
  }
};

// ---- Brushing teeth: fast regular strokes (~4.5 Hz) -------------------------
struct Brushing : Model {
  std::mt19937 rng;
  std::normal_distribution<double> nd{0.0, 1.0};
  double f, amp;
  Brushing(uint32_t seed, double hz, double ampG) : rng(seed), f(hz), amp(ampG) {}
  V3 at(double t) override {
    double a = amp * sin(2 * PI * f * t) * (1.0 + 0.2 * sin(2 * PI * 0.3 * t));
    V3 s = rot(add({0, 0, 1.0}, {a, 0.3 * a, 0.2 * a}), 0.9, 0.4);
    return add(s, {0.01 * nd(rng), 0.01 * nd(rng), 0.01 * nd(rng)});
  }
};

// Two models back to back (stop-and-go etc.).
struct Concat : Model {
  Model &a, &b; double switchT;
  Concat(Model &a_, Model &b_, double t) : a(a_), b(b_), switchT(t) {}
  V3 at(double t) override { return t < switchT ? a.at(t) : b.at(t); }
};

// Sample a model the way the watch does: point samples at fs, 14-bit counts,
// clipped to ±2 g, then converted back to g for the detector.
uint32_t run(Model &m, double seconds, Detector &d, double fs = 12.5) {
  uint32_t total = 0;
  int n = (int)(seconds * fs);
  for (int i = 0; i < n; i++) {
    V3 v = m.at(i / fs);
    auto q = [](double g) {
      long c = lround(g * 4096.0);
      if (c > 8191) c = 8191;
      if (c < -8192) c = -8192;
      return (float)c / 4096.0f;
    };
    total += d.push(q(v.x), q(v.y), q(v.z));
  }
  return total;
}

void expectRange(const char *what, uint32_t got, uint32_t lo, uint32_t hi) {
  char msg[128];
  snprintf(msg, sizeof msg, "%s: counted %u, expected %u..%u", what, got, lo, hi);
  printf("  %s\n", msg);
  TEST_ASSERT_TRUE_MESSAGE(got >= lo && got <= hi, msg);
}

}  // namespace

// ===========================================================================
// Walking-type signals: count within tolerance
// ===========================================================================
void test_normal_walk() {
  Detector d;
  Walk w(200, 1.0, 1.85, 0.04, 0.12, 0.30, 0.18, 11);
  uint32_t n = run(w, w.end() + 2.0, d);
  expectRange("normal walk 200 @1.85Hz", n, 192, 201);
}

void test_slow_walk() {
  Detector d;
  Walk w(120, 1.0, 1.3, 0.06, 0.07, 0.16, 0.10, 12);
  uint32_t n = run(w, w.end() + 2.0, d);
  expectRange("slow walk 120 @1.3Hz", n, 108, 121);
}

void test_brisk_walk() {
  Detector d;
  Walk w(200, 1.0, 2.15, 0.04, 0.18, 0.40, 0.25, 13);
  uint32_t n = run(w, w.end() + 2.0, d);
  expectRange("brisk walk 200 @2.15Hz", n, 192, 201);
}

void test_run() {
  Detector d;
  Walk w(300, 1.0, 2.8, 0.03, 0.6, 1.1, 0.5, 14);
  uint32_t n = run(w, w.end() + 2.0, d);
  expectRange("run 300 @2.8Hz (clipping)", n, 276, 301);
}

void test_irregular_walk() {
  // A wandering walk: 10% cadence jitter, e.g. shopping / indoors.
  Detector d;
  Walk w(150, 1.0, 1.7, 0.10, 0.10, 0.25, 0.15, 15);
  uint32_t n = run(w, w.end() + 2.0, d);
  expectRange("irregular walk 150 (10% jitter)", n, 125, 151);
}

void test_stop_and_go() {
  Detector d;
  Walk a(40, 1.0, 1.8, 0.04, 0.12, 0.30, 0.18, 16);
  Walk b(40, a.end() + 4.0, 1.8, 0.04, 0.12, 0.30, 0.18, 17);
  Concat c(a, b, a.end() + 2.0);
  uint32_t n = run(c, b.end() + 2.0, d);
  expectRange("stop-and-go 40+40", n, 74, 81);
}

void test_strong_then_gentle_walk() {
  // A walk whose arm swing fades (hands into pockets, carrying shopping):
  // 60 normal steps, then 140 at ~40% of the amplitude. The detector must
  // keep counting through the gentle part (lower floor once confirmed,
  // missed-peak recovery).
  Detector d;
  Walk a(60, 1.0, 1.75, 0.05, 0.12, 0.30, 0.18, 50);
  Walk b(140, a.end() - 0.03, 1.75, 0.05, 0.05, 0.12, 0.07, 51);
  Concat c(a, b, a.end() - 0.3);
  uint32_t n = run(c, b.end() + 2.0, d);
  expectRange("strong-then-gentle walk 60+140", n, 185, 201);
}

void test_walk_at_50hz() {
  DetectorConfig cfg;
  cfg.sampleHz = 50.0f;
  Detector d(cfg);
  Walk w(200, 1.0, 1.85, 0.04, 0.12, 0.30, 0.18, 18);
  uint32_t n = run(w, w.end() + 2.0, d, 50.0);
  expectRange("normal walk @50Hz ODR", n, 192, 201);
}

void test_walking_flag_and_cadence() {
  Detector d;
  Walk w(60, 1.0, 1.8, 0.02, 0.12, 0.30, 0.18, 19);
  double fs = 12.5;
  bool sawWalking = false;
  float cad = 0;
  for (int i = 0; i < (int)((w.end() - 0.5) * fs); i++) {
    V3 v = w.at(i / fs);
    d.push((float)v.x, (float)v.y, (float)v.z);
    if (d.walking()) { sawWalking = true; cad = d.cadenceSpm(); }
  }
  TEST_ASSERT_TRUE(sawWalking);
  printf("  cadence estimate %.1f spm (true 108)\n", cad);
  TEST_ASSERT_FLOAT_WITHIN(12.0f, 108.0f, cad);
  // after standing still for a few seconds the walk ends
  Still s(5, 10, 0.005, 0, 0);
  for (int i = 0; i < (int)(5 * fs); i++) {
    V3 v = s.at(i / fs);
    d.push((float)v.x, (float)v.y, (float)v.z);
  }
  TEST_ASSERT_FALSE(d.walking());
}

// ===========================================================================
// Non-walking signals: must (nearly) never count
// ===========================================================================
void test_still_with_fidgets() {
  Detector d;
  Still s(21, 600, 0.01, 40, 0.12);
  uint32_t n = run(s, 600, d);
  expectRange("10 min desk + 40 fidgets", n, 0, 2);
}

void test_typing() {
  Detector d;
  Typing ty(22, 300);
  uint32_t n = run(ty, 300, d);
  expectRange("5 min typing", n, 0, 6);
}

void test_car_ride() {
  Detector d;
  Car c(23, 600);
  uint32_t n = run(c, 600, d);
  expectRange("10 min car ride", n, 0, 10);
}

void test_gesturing() {
  Detector d;
  Gesture g(24, 120);
  uint32_t n = run(g, 120, d);
  expectRange("2 min gesturing", n, 0, 5);
}

void test_brushing_teeth() {
  Detector d;
  Brushing b(29, 4.5, 0.45);
  uint32_t n = run(b, 120, d);
  expectRange("2 min brushing teeth @4.5Hz", n, 0, 5);
}

void test_isolated_jolts() {
  Detector d;
  Jolts j(25, 25);
  uint32_t n = run(j, j.end(), d);
  expectRange("25 isolated jolts", n, 0, 0);
}

void test_short_bursts_below_gate() {
  // Repeated 3-step shuffles (kitchen, desk to printer...) never confirm a walk.
  Detector d;
  uint32_t total = 0;
  for (int i = 0; i < 10; i++) {
    Walk w(3, 1.0, 1.8, 0.03, 0.12, 0.30, 0.18, 100 + i);
    Detector *dd = &d;
    // run each burst followed by stillness, on one continuous detector
    Still s(200 + i, 8, 0.008, 0, 0);
    Concat c(w, s, w.end());
    int n = (int)(8.0 * 12.5);
    for (int k = 0; k < n; k++) {
      V3 v = c.at(k / 12.5);
      total += dd->push((float)v.x, (float)v.y, (float)v.z);
    }
  }
  expectRange("10 x 3-step shuffles", total, 0, 3);
}

// ===========================================================================
// Robustness
// ===========================================================================
void test_gap_breaks_chain_but_recovers() {
  Detector d;
  Walk w(100, 1.0, 1.8, 0.03, 0.12, 0.30, 0.18, 26);
  double fs = 12.5;
  uint32_t total = 0;
  int n = (int)((w.end() + 2) * fs);
  for (int i = 0; i < n; i++) {
    if (i == n / 2) d.gap();
    V3 v = w.at(i / fs);
    total += d.push((float)v.x, (float)v.y, (float)v.z);
  }
  expectRange("walk with a FIFO overflow gap", total, 92, 101);
}

void test_reset_clears() {
  Detector d;
  Walk w(30, 1.0, 1.8, 0.03, 0.12, 0.30, 0.18, 27);
  run(w, w.end(), d);
  TEST_ASSERT_TRUE(d.total() > 0);
  d.reset();
  TEST_ASSERT_EQUAL_UINT32(0, d.total());
  TEST_ASSERT_FALSE(d.walking());
}

void test_deterministic() {
  Detector a, b;
  Walk w1(80, 1.0, 1.9, 0.05, 0.12, 0.30, 0.18, 28);
  Walk w2(80, 1.0, 1.9, 0.05, 0.12, 0.30, 0.18, 28);
  TEST_ASSERT_EQUAL_UINT32(run(w1, w1.end(), a), run(w2, w2.end(), b));
}

// ===========================================================================
// StepBook
// ===========================================================================
void test_book_rollover_and_history() {
  StepBook b;
  b.init();
  b.add(500, 0);                 // clock not set yet
  TEST_ASSERT_EQUAL_UINT32(500, b.today);
  b.add(100, 9000);              // clock becomes valid: adopt the day, keep steps
  TEST_ASSERT_EQUAL_UINT32(9000, b.day);
  TEST_ASSERT_EQUAL_UINT32(600, b.today);
  b.add(50, 9001);               // midnight
  TEST_ASSERT_EQUAL_UINT32(50, b.today);
  TEST_ASSERT_EQUAL_UINT32(600, b.stepsOn(9000));
  TEST_ASSERT_EQUAL_UINT32(650, b.lifetime);
  TEST_ASSERT_TRUE(b.rollTo(9005));                       // skipped days
  TEST_ASSERT_EQUAL_UINT32(0, b.today);
  TEST_ASSERT_EQUAL_UINT32(50, b.stepsOn(9001));
  TEST_ASSERT_EQUAL_UINT32(0, b.stepsOn(9003));
  TEST_ASSERT_FALSE(b.rollTo(0));                         // invalid clock: no-op
  TEST_ASSERT_FALSE(b.rollTo(9005));                      // same day: no-op
}

void test_book_history_capacity() {
  StepBook b;
  b.init();
  for (uint32_t d = 1; d <= 40; d++) b.add(d * 10, 8000 + d);
  b.rollTo(8100);
  TEST_ASSERT_EQUAL_UINT32(400, b.stepsOn(8040));
  TEST_ASSERT_EQUAL_UINT32(110, b.stepsOn(8011));   // 30 most recent kept
  TEST_ASSERT_EQUAL_UINT32(0, b.stepsOn(8010));     // fell off the end
}

void test_book_clock_backwards_resumes_day() {
  StepBook b;
  b.init();
  b.add(1000, 9000);
  b.add(200, 9001);      // next day
  b.add(5, 9000);        // user sets the date back a day
  TEST_ASSERT_EQUAL_UINT32(9000, b.day);
  TEST_ASSERT_EQUAL_UINT32(1005, b.today);   // resumed, not lost
  TEST_ASSERT_EQUAL_UINT32(200, b.stepsOn(9001));
  TEST_ASSERT_EQUAL_UINT32(1205, b.lifetime);
}

void test_book_stale_day_reads_zero() {
  // Right after midnight the book may not have rolled yet. Anything asking
  // about the new day (the week chart, the goal check) must get 0, never
  // yesterday's total.
  StepBook b;
  b.init();
  b.add(7000, 9000);
  TEST_ASSERT_EQUAL_UINT32(7000, b.stepsOn(9000));
  TEST_ASSERT_EQUAL_UINT32(0, b.stepsOn(9001));      // not rolled yet
  b.rollTo(9001);
  TEST_ASSERT_EQUAL_UINT32(0, b.today);
  TEST_ASSERT_EQUAL_UINT32(7000, b.stepsOn(9000));   // now in history
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_normal_walk);
  RUN_TEST(test_slow_walk);
  RUN_TEST(test_brisk_walk);
  RUN_TEST(test_run);
  RUN_TEST(test_irregular_walk);
  RUN_TEST(test_stop_and_go);
  RUN_TEST(test_strong_then_gentle_walk);
  RUN_TEST(test_walk_at_50hz);
  RUN_TEST(test_walking_flag_and_cadence);
  RUN_TEST(test_still_with_fidgets);
  RUN_TEST(test_typing);
  RUN_TEST(test_car_ride);
  RUN_TEST(test_gesturing);
  RUN_TEST(test_brushing_teeth);
  RUN_TEST(test_isolated_jolts);
  RUN_TEST(test_short_bursts_below_gate);
  RUN_TEST(test_gap_breaks_chain_but_recovers);
  RUN_TEST(test_reset_clears);
  RUN_TEST(test_deterministic);
  RUN_TEST(test_book_rollover_and_history);
  RUN_TEST(test_book_history_capacity);
  RUN_TEST(test_book_clock_backwards_resumes_day);
  RUN_TEST(test_book_stale_day_reads_zero);
  return UNITY_END();
}
