// Rep Counter: detector tests on synthetic signals.
//
// Each lifting category draws 25 randomised scenarios (tempo, range of
// motion, grip, pauses, sticking points, wrist, sensor offsets/noise...) from
// test/support/rep_scenarios.h and checks the counts against ground truth.
// Negative categories (walking, running, fidgeting, everyday gestures) must
// not produce sets. Run: pio test -e native
#include <unity.h>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "rep_detector.h"
#include "rep_harness.h"
#include "rep_scenarios.h"

static const int kSeeds = 25;

struct CatStats {
  int sets = 0, exact = 0, within1 = 0, label = 0, falseSets = 0, detected = 0;
  double endMin = 1e9, endMax = -1e9;
};

static int catIndex(const std::string &cat) {
  for (int c = 0; c < scen::kNumCategories; c++)
    if (cat == scen::kCategories[c]) return c;
  return 0;
}

static bool gPolled = false;        // simulate the REPS_IMU_FIFO=0 fallback

static CatStats runCat(const std::string &cat, int seeds = kSeeds,
                       void (*tweak)(reps::Config &) = nullptr) {
  CatStats st;
  int c = catIndex(cat);
  for (int s = 1; s <= seeds; s++) {
    scen::Built b = scen::build(cat, (uint32_t)(s * 7919 + c * 104729));
    if (tweak) tweak(b.cfg);
    if (gPolled) {
      b.xyz = harness::polledTo50Hz(b.xyz, (uint32_t)s);
      b.cfg.fs = 50.f;
    }
    harness::RunResult res = harness::run(b.xyz, b.cfg);
    harness::Score sc = harness::score(b.truth, res);
    st.sets += (int)b.truth.size();
    st.exact += sc.exact;
    st.label += sc.labelOk;
    st.falseSets += (int)sc.extra.size();
    st.detected += (int)res.sets.size();
    for (auto &m : sc.matches) {
      int got = m.det ? m.det->reps : 0;
      if (std::abs(got - m.truth->reps) <= 1) st.within1++;
      if (m.det) {
        double e = m.det->tEnd - m.truth->tLastEnd;
        if (e < st.endMin) st.endMin = e;
        if (e > st.endMax) st.endMax = e;
      }
    }
  }
  std::printf("  %-11s sets=%3d exact=%3.0f%% +-1=%3.0f%% label=%3.0f%% false=%d end=[%.1f,%.1f]s\n",
              cat.c_str(), st.sets, st.sets ? 100.0 * st.exact / st.sets : 0.0,
              st.sets ? 100.0 * st.within1 / st.sets : 0.0,
              st.sets ? 100.0 * st.label / st.sets : 0.0, st.falseSets,
              st.endMin < 1e8 ? st.endMin : 0.0, st.endMax > -1e8 ? st.endMax : 0.0);
  return st;
}

static void expectRates(const CatStats &st, double exactMin, double within1Min, double labelMin) {
  TEST_ASSERT_TRUE(st.sets > 0);
  TEST_ASSERT_TRUE_MESSAGE((double)st.exact / st.sets >= exactMin, "exact-count rate too low");
  TEST_ASSERT_TRUE_MESSAGE((double)st.within1 / st.sets >= within1Min, "within-one rate too low");
  TEST_ASSERT_TRUE_MESSAGE((double)st.label / st.sets >= labelMin, "label accuracy too low");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, st.falseSets, "spurious sets during a lifting scenario");
}

// ---- counting accuracy -----------------------------------------------------
static void test_curls() { expectRates(runCat("curl"), 0.92, 1.0, 0.90); }
static void test_hammer_curls() { expectRates(runCat("hammer"), 0.92, 1.0, 0.95); }
static void test_slow_curls() { expectRates(runCat("curl_slow"), 0.88, 0.96, 0.88); }
static void test_explosive_curls() { expectRates(runCat("curl_fast"), 0.70, 0.88, 0.90); }
static void test_presses() { expectRates(runCat("press"), 0.92, 1.0, 0.95); }
static void test_slow_presses_sticking() { expectRates(runCat("press_slow"), 0.92, 1.0, 0.95); }
static void test_bench_press_from_lockout() { expectRates(runCat("bench"), 0.92, 1.0, 0.95); }
static void test_raises() { expectRates(runCat("raise"), 0.92, 1.0, 0.95); }
static void test_rows() { expectRates(runCat("row"), 0.88, 0.96, 0.95); }

static void test_workout_sessions() {
  // Three sets of mixed exercises separated by rests and walking.
  CatStats st = runCat("session", 20);
  expectRates(st, 0.92, 0.98, 0.95);
}

// ---- rejection -------------------------------------------------------------
static void test_walking_makes_no_sets() {
  CatStats st = runCat("walk");
  TEST_ASSERT_EQUAL_INT(0, st.falseSets);
}

static void test_running_makes_no_sets() {
  CatStats st = runCat("run");
  TEST_ASSERT_EQUAL_INT(0, st.falseSets);
}

static void test_fidgeting_rarely_makes_sets() {
  // Continuous random wrist motion for 1.5-2.5 minutes per run: at most one
  // spurious set across all runs (that's > 50 minutes of fidgeting).
  CatStats st = runCat("fidget");
  TEST_ASSERT_LESS_OR_EQUAL_INT(1, st.falseSets);
}

static void test_gestures_rarely_make_sets() {
  // Checking the watch, drinking, picking things up, short walks.
  CatStats st = runCat("gestures");
  TEST_ASSERT_LESS_OR_EQUAL_INT(1, st.falseSets);
}

// ---- set end ---------------------------------------------------------------
static void test_set_end_timing() {
  // Default 4 s: the set ends a few seconds after the weight comes to rest
  // (rep counted ~60 % of the way down, then 4 s, longer for slow tempos).
  CatStats st = runCat("curl");
  TEST_ASSERT_TRUE(st.endMin > 1.5);
  TEST_ASSERT_TRUE(st.endMax < 6.5);
}

static void setEnd7(reps::Config &c) { c.setEndSec = 7.f; }
static void test_set_end_is_configurable() {
  CatStats a = runCat("press", 10);
  CatStats b = runCat("press", 10, setEnd7);
  TEST_ASSERT_EQUAL_INT(a.within1, b.within1);
  TEST_ASSERT_TRUE(b.endMin > a.endMin + 2.0);
}

// ---- modes -----------------------------------------------------------------
static void forcePress(reps::Config &c) { c.mode = reps::Mode::Press; }
static void forceCurl(reps::Config &c) { c.mode = reps::Mode::Curl; }

static void test_manual_mode_only_counts_its_channel() {
  // Curls while "Press" is selected (and vice versa) must not count.
  CatStats a = runCat("curl", 10, forcePress);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, a.detected, "curls counted while Press was selected");
  CatStats b = runCat("press", 10, forceCurl);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, b.detected, "presses counted while Curl was selected");
}

static void test_manual_curl_mode_counts_curls() {
  CatStats st = runCat("curl", 15, forceCurl);
  TEST_ASSERT_TRUE((double)st.within1 / st.sets >= 1.0);
  TEST_ASSERT_TRUE((double)st.label / st.sets >= 1.0);
}

static void test_manual_modes_still_reject_noise() {
  // A pinned exercise only fixes the label and the counting channel; walking
  // and fidgeting between sets must still not become sets.
  CatStats a = runCat("walk", 15, forceCurl);
  CatStats b = runCat("fidget", 15, forceCurl);
  CatStats c = runCat("walk", 15, forcePress);
  CatStats d = runCat("gestures", 15, forcePress);
  TEST_ASSERT_EQUAL_INT(0, a.falseSets);
  TEST_ASSERT_LESS_OR_EQUAL_INT(1, b.falseSets);
  TEST_ASSERT_EQUAL_INT(0, c.falseSets);
  TEST_ASSERT_LESS_OR_EQUAL_INT(1, d.falseSets);
}

// ---- robustness ------------------------------------------------------------
static void test_determinism() {
  scen::Built b = scen::build("session", 1234);
  harness::RunResult r1 = harness::run(b.xyz, b.cfg);
  harness::RunResult r2 = harness::run(b.xyz, b.cfg);
  TEST_ASSERT_EQUAL_INT((int)r1.events.size(), (int)r2.events.size());
  for (size_t i = 0; i < r1.events.size(); i++) {
    TEST_ASSERT_EQUAL_INT((int)r1.events[i].type, (int)r2.events[i].type);
    TEST_ASSERT_EQUAL_UINT32(r1.events[i].tMs, r2.events[i].tMs);
    TEST_ASSERT_EQUAL_INT(r1.events[i].reps, r2.events[i].reps);
  }
}

static void test_gap_inside_a_set() {
  // A hole in the stream (FIFO overflow) mid-set loses at most a rep or two.
  scen::Built b = scen::build("curl", 77);
  std::vector<size_t> gaps;
  size_t mid = (size_t)((b.truth[0].tFirstStart + b.truth[0].tLastEnd) / 2 * 100);
  std::vector<int16_t> cut(b.xyz.begin(), b.xyz.begin() + mid * 3);
  cut.insert(cut.end(), b.xyz.begin() + (mid + 30) * 3, b.xyz.end());   // drop 0.3 s
  gaps.push_back(mid);
  reps::Detector d;
  harness::RunResult r = harness::run(cut, b.cfg, &d, &gaps);
  TEST_ASSERT_TRUE(r.sets.size() >= 1);
  int total = 0;
  for (auto &s : r.sets) total += s.reps;
  TEST_ASSERT_TRUE(std::abs(total - b.truth[0].reps) <= 2);
}

static void test_hand_direction_is_learned() {
  // Mounting constant deliberately wrong: rows would read as presses. After
  // a curl set the detector knows which way the hand points.
  synth::Mount mt;
  synth::SensorParams sp;
  synth::Scenario sc(mt, sp, 5);
  sc.add(synth::rest(3, mt, 1));
  synth::CurlParams cp;
  cp.plan.reps = 8;
  cp.plan.seed = 3;
  sc.add(synth::curlSet(cp, mt));
  sc.add(synth::rest(8, mt, 2));
  synth::RowParams rp;
  rp.plan.reps = 8;
  rp.plan.seed = 4;
  sc.add(synth::rowSet(rp, mt), 1.5);
  sc.add(synth::rest(8, mt, 3), 1.5);
  std::vector<int16_t> xyz = sc.render();
  reps::Config cfg;
  cfg.handAxisSignLeft = -1;            // wrong on purpose
  reps::Detector d;
  harness::RunResult r = harness::run(xyz, cfg, &d);
  TEST_ASSERT_EQUAL_INT(2, (int)r.sets.size());
  TEST_ASSERT_EQUAL_INT((int)reps::Exercise::Curl, (int)r.sets[0].ex);
  TEST_ASSERT_EQUAL_INT((int)reps::Exercise::Row, (int)r.sets[1].ex);
  TEST_ASSERT_EQUAL_INT(1, d.learnedHandSign());
}

static void test_long_idle_is_quiet_and_finite() {
  // Three hours on a desk: no events, nothing blows up numerically.
  reps::Config cfg;
  reps::Detector d;
  d.reset(cfg);
  uint32_t lcg = 12345;
  for (long i = 0; i < 3L * 3600 * 100; i++) {
    lcg = lcg * 1664525u + 1013904223u;
    int16_t nz = (int16_t)((lcg >> 16) % 9) - 4;
    d.push((int16_t)(10 + nz), (int16_t)(-6 - nz), (int16_t)(2048 + nz));
  }
  reps::Event e;
  TEST_ASSERT_FALSE(d.pollEvent(e));
  const reps::Debug &g = d.debug();
  TEST_ASSERT_TRUE(std::isfinite(g.theta) && std::isfinite(g.vel) && std::isfinite(g.linExc));
  TEST_ASSERT_TRUE(std::fabs(g.linExc) < 1.f);
}

static void test_extreme_inputs() {
  reps::Detector d;
  d.reset(reps::Config());
  const int16_t vals[] = {8191, -8192, 0, 32767, -32768, 1};
  for (int i = 0; i < 20000; i++) {
    d.push(vals[i % 6], vals[(i / 3) % 6], vals[(i / 7) % 6]);
    reps::Event e;
    while (d.pollEvent(e)) {}
  }
  for (int i = 0; i < 1000; i++) d.push(0, 0, 0);      // free fall / dead sensor
  const reps::Live &l = d.live();
  TEST_ASSERT_TRUE(std::isfinite(l.progress));
  TEST_ASSERT_TRUE(std::isfinite(d.debug().theta));
}

static void test_event_queue_survives_not_polling() {
  // A caller that stops polling must not corrupt anything; newest events win.
  scen::Built b = scen::build("press", 31);
  reps::Detector d;
  d.reset(b.cfg);
  for (size_t i = 0; i < b.xyz.size() / 3; i++) d.push(b.xyz[3 * i], b.xyz[3 * i + 1], b.xyz[3 * i + 2]);
  for (int i = 0; i < 1500; i++) d.push(0, 0, 2048);
  reps::Event e, last;
  int n = 0;
  while (d.pollEvent(e)) { last = e; n++; }
  TEST_ASSERT_TRUE(n > 0 && n <= 8);
  TEST_ASSERT_EQUAL_INT((int)reps::EventType::SetEnd, (int)last.type);
}

// ---- fallback sampling (REPS_IMU_FIFO=0) ---------------------------------------
static void test_polled_fallback_still_counts() {
  // Jittery ~45 Hz polls at +/-2 g, resampled to 50 Hz.
  gPolled = true;
  CatStats c = runCat("curl", 15);
  CatStats p = runCat("press", 15);
  CatStats w = runCat("walk", 15);
  CatStats r = runCat("run", 15);
  gPolled = false;
  TEST_ASSERT_TRUE((double)c.within1 / c.sets >= 0.95);
  TEST_ASSERT_TRUE((double)p.within1 / p.sets >= 0.95);
  // Blunted footfalls (polls + +/-2 g clipping) make the locomotion gate a
  // little weaker in this fallback; the FIFO path above is the default.
  TEST_ASSERT_LESS_OR_EQUAL_INT(1, w.falseSets);
  TEST_ASSERT_LESS_OR_EQUAL_INT(2, r.falseSets);
}

static void test_resampler_grid() {
  reps::Resampler rs(50.f, 250);
  std::vector<float> xs;
  int gaps = 0;
  float g[3] = {0, 0, 1};
  uint32_t t = 1000;
  for (int i = 0; i < 100; i++) {
    g[0] = (float)t;                         // x = time, so interpolation is exact
    rs.push(t, g, false, [&](float x, float, float, bool gap) { xs.push_back(x); gaps += gap; });
    t += (i % 3 == 0) ? 27 : 19;
  }
  TEST_ASSERT_EQUAL_INT(1, gaps);
  for (size_t i = 1; i < xs.size(); i++) TEST_ASSERT_FLOAT_WITHIN(0.01f, 20.f, xs[i] - xs[i - 1]);
  // A long hole restarts the grid.
  g[0] = (float)(t + 1000);
  rs.push(t + 1000, g, false, [&](float, float, float, bool gap) { gaps += gap; });
  TEST_ASSERT_EQUAL_INT(2, gaps);
}

// ---- DSP building blocks -----------------------------------------------------
static void test_dsp_basics() {
  reps::Biquad lp = reps::Biquad::lowpass(4.f, 100.f);
  lp.prime(1.5f);
  for (int i = 0; i < 50; i++) TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.5f, lp.step(1.5f));
  reps::Biquad hp = reps::Biquad::highpass(8.f, 100.f);
  hp.prime(1.0f);
  for (int i = 0; i < 50; i++) TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.f, hp.step(1.0f));
  reps::V3 a(1, 0, 0), b(0, 1, 0), c(1, 1e-4f, 0);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 90.f, reps::angleDeg(a, b));
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0057f, reps::angleDeg(a, reps::normalized(c)));
  float v[5] = {5, 1, 4, 2, 3};
  TEST_ASSERT_EQUAL_FLOAT(3.f, reps::smallMedian(v, 5));
  float *f = nullptr;
  reps::Tuning t;
  f = reps::tuningField(t, "returnFrac");
  TEST_ASSERT_NOT_NULL(f);
  TEST_ASSERT_NULL(reps::tuningField(t, "nope"));
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_dsp_basics);
  RUN_TEST(test_curls);
  RUN_TEST(test_hammer_curls);
  RUN_TEST(test_slow_curls);
  RUN_TEST(test_explosive_curls);
  RUN_TEST(test_presses);
  RUN_TEST(test_slow_presses_sticking);
  RUN_TEST(test_bench_press_from_lockout);
  RUN_TEST(test_raises);
  RUN_TEST(test_rows);
  RUN_TEST(test_workout_sessions);
  RUN_TEST(test_walking_makes_no_sets);
  RUN_TEST(test_running_makes_no_sets);
  RUN_TEST(test_fidgeting_rarely_makes_sets);
  RUN_TEST(test_gestures_rarely_make_sets);
  RUN_TEST(test_set_end_timing);
  RUN_TEST(test_set_end_is_configurable);
  RUN_TEST(test_manual_mode_only_counts_its_channel);
  RUN_TEST(test_manual_curl_mode_counts_curls);
  RUN_TEST(test_manual_modes_still_reject_noise);
  RUN_TEST(test_determinism);
  RUN_TEST(test_gap_inside_a_set);
  RUN_TEST(test_hand_direction_is_learned);
  RUN_TEST(test_long_idle_is_quiet_and_finite);
  RUN_TEST(test_extreme_inputs);
  RUN_TEST(test_event_queue_survives_not_polling);
  RUN_TEST(test_resampler_grid);
  RUN_TEST(test_polled_fallback_still_counts);
  return UNITY_END();
}
