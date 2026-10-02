// OracleApp / Scene / HapticPlanner tests: the whole app driven the way the
// watch drives it (IMU samples at ~45 Hz, frames at the app's own cadence),
// checked for behaviour, haptic pacing, pixel bounds and robustness.
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include <vector>

#include "FreeSansBold24pt7b.h"
#include "oracle_app.h"
#include "oracle_config.h"

using namespace oracle;

namespace {

constexpr float kPi = 3.14159265f;
std::vector<uint16_t> gFb(kScreenW * kScreenH);

void *hostAlloc(size_t n, bool) { return calloc(1, n); }

int16_t counts(float g) {
  float c = roundf(g * kCountsPerG);
  if (c > 8191) c = 8191;
  if (c < -8192) c = -8192;
  return (int16_t)c;
}

// A model of the haptic driver: one buzz playing plus a 4-deep queue.
struct MotorModel {
  std::vector<uint32_t> ends;   // finish times of the playing + queued buzzes
  int  maxQueued = 0;
  int  posted = 0;
  bool overflow = false;
  void post(uint32_t now, const Buzz &b) {
    while (!ends.empty() && ends.front() <= now) ends.erase(ends.begin());
    if (ends.size() >= 5) overflow = true;   // 1 playing + 4 waiting is the limit
    uint32_t start = ends.empty() ? now : ends.back();
    ends.push_back(start + b.ms);
    if ((int)ends.size() > maxQueued) maxQueued = (int)ends.size();
    posted++;
  }
};

struct Run {
  OracleApp app;
  uint32_t  t = 0, nextSample = 0;
  MotorModel motor;
  int landed = 0, settledAt = -1, phaseChanges = 0;
  Phase lastPhase = Phase::Idle;
  std::vector<std::pair<uint32_t, Buzz>> buzzes;

  explicit Run(uint64_t seed = 0x1234) {
    TEST_ASSERT_TRUE(app.begin(&FreeSansBold24pt7b, hostAlloc, seed));
    app.setImuOk(true);
    app.enter(gFb.data(), 0);
  }
  // Advance to `until` ms, feeding the accelerometer from `accel`.
  template <typename F>
  void advance(uint32_t until, F accel) {
    while (t < until) {
      while (nextSample <= t) {
        float ax, ay, az;
        accel(nextSample * 0.001f, ax, ay, az);
        app.imuSample(nextSample, counts(ax), counts(ay), counts(az));
        nextSample += 22;
      }
      FrameOut out;
      app.frame(gFb.data(), t, out);
      if (out.landed) landed++;
      for (int i = 0; i < out.buzzCount; i++) {
        motor.post(t, out.buzz[i]);
        buzzes.push_back({t, out.buzz[i]});
      }
      if (app.scene().phase() != lastPhase) {
        lastPhase = app.scene().phase();
        phaseChanges++;
        if (lastPhase == Phase::Showing && settledAt < 0) settledAt = (int)t;
      }
      t += app.framePeriodMs();
    }
  }
};

void still(float, float &ax, float &ay, float &az) { ax = 0; ay = 0.55f; az = 0.83f; }

// Rest, then a 4 Hz shake between `from` and `to` seconds.
struct ShakeBetween {
  float from, to;
  void operator()(float t, float &ax, float &ay, float &az) const {
    still(t, ax, ay, az);
    if (t >= from && t < to) ax += 1.8f * sinf(2.f * kPi * 4.f * (t - from));
  }
};

}  // namespace

void setUp() {}
void tearDown() {}

void test_shake_then_answer_full_cycle() {
  Run r;
  r.advance(1500, still);
  TEST_ASSERT_EQUAL(Phase::Idle, r.app.scene().phase());
  TEST_ASSERT_TRUE(r.app.scene().frame().promptAlpha > 0.9f);   // prompt is up
  r.advance(3000, ShakeBetween{1.5f, 3.0f});
  TEST_ASSERT_EQUAL(Phase::Churning, r.app.scene().phase());
  TEST_ASSERT_TRUE(r.app.scene().frame().promptAlpha < 0.1f);
  TEST_ASSERT_TRUE(r.app.scene().frame().dieTextAlpha < 0.1f);  // blank while churning
  r.advance(7000, still);
  TEST_ASSERT_EQUAL(Phase::Showing, r.app.scene().phase());
  TEST_ASSERT_EQUAL_INT(1, r.landed);                           // one thunk
  TEST_ASSERT_TRUE(r.settledAt > 3000 && r.settledAt < 3000 + 3500);
  TEST_ASSERT_TRUE(strlen(r.app.answer()) > 0);
  const FrameState &f = r.app.scene().frame();
  TEST_ASSERT_FLOAT_WITHIN(0.02f, 0.f, f.dieFog);               // against the glass
  TEST_ASSERT_FLOAT_WITHIN(0.03f, 1.f, f.dieScale);
  TEST_ASSERT_FLOAT_WITHIN(0.08f, 0.f, f.dieAngle);             // upright
  TEST_ASSERT_TRUE(f.dieSquash > 0.97f);                        // facing the glass
}

void test_answer_comes_from_the_current_pack() {
  for (int p = 0; p < packCount(); p++) {
    Run r(77 + p);
    r.app.swipePack(p);
    r.advance(1500, still);
    TEST_ASSERT_EQUAL_INT(p, r.app.pack());
    r.advance(6000, ShakeBetween{1.5f, 3.0f});
    const Pack &pk = packAt(p);
    bool found = strcmp(r.app.answer(), pk.golden) == 0;
    for (int i = 0; i < pk.count; i++) found = found || strcmp(r.app.answer(), pk.answers[i].text) == 0;
    TEST_ASSERT_TRUE_MESSAGE(found, r.app.answer());
  }
}

void test_haptics_rumble_then_thunk_without_flooding() {
  Run r;
  r.advance(1500, still);
  TEST_ASSERT_EQUAL_INT(0, r.motor.posted);                     // quiet while idle
  r.advance(3000, ShakeBetween{1.5f, 3.0f});
  int rumbles = r.motor.posted;
  TEST_ASSERT_TRUE(rumbles >= 8);                               // ~ every 72-84 ms
  r.advance(7000, still);
  TEST_ASSERT_FALSE(r.motor.overflow);
  TEST_ASSERT_TRUE(r.motor.maxQueued <= 2);
  // Rumble pulses are soft, never back to back.
  uint32_t prev = 0;
  bool sawThunk = false;
  for (auto &e : r.buzzes) {
    if (e.second.intensity == kThunkHit && e.second.ms == kThunkHitMs) sawThunk = true;
    if (!sawThunk) {
      TEST_ASSERT_TRUE(e.second.intensity <= kRumbleMax + 14);
      if (prev) TEST_ASSERT_TRUE(e.first - prev >= kRumblePulseMs + kRumbleGapMs);
      prev = e.first;
    }
  }
  TEST_ASSERT_TRUE(sawThunk);
  // The thunk is followed by its softer rebound, and then silence.
  TEST_ASSERT_EQUAL_UINT8(kThunkTail, r.buzzes.back().second.intensity);
}

void test_rumble_stops_promptly_when_shaking_stops() {
  Run r;
  r.advance(7000, ShakeBetween{1.5f, 3.0f});
  uint32_t lastRumble = 0;
  for (auto &e : r.buzzes) {
    if (e.second.intensity == kThunkHit && e.second.ms == kThunkHitMs) break;
    lastRumble = e.first;
  }
  TEST_ASSERT_TRUE(lastRumble > 2500);
  // The shake ends at 3.0 s; its last pulse starts within 200 ms of that,
  // though the churn (and the answer) waits for the detector's 420 ms.
  TEST_ASSERT_TRUE_MESSAGE(lastRumble <= 3200, "rumble outlasted the shake");
}

void test_golden_answer_twinkles() {
  Run r;
  r.app.forceNextAnswer(packAt(0).golden, true);
  r.advance(7000, ShakeBetween{1.5f, 3.0f});
  TEST_ASSERT_TRUE(r.app.answerGolden());
  TEST_ASSERT_TRUE(r.app.scene().frame().dieGold);
  // Knock, rebound, and two twinkle notes.
  int afterThunk = 0;
  bool sawThunk = false;
  for (auto &e : r.buzzes) {
    if (e.second.intensity == kThunkHit) sawThunk = true;
    else if (sawThunk) afterThunk++;
  }
  TEST_ASSERT_EQUAL_INT(3, afterThunk);
}

void test_tap_sinks_the_answer_and_the_prompt_returns() {
  Run r;
  r.advance(7000, ShakeBetween{1.5f, 3.0f});
  TEST_ASSERT_EQUAL(Phase::Showing, r.app.scene().phase());
  r.app.tap(120, 140);
  r.advance(7100, still);
  TEST_ASSERT_EQUAL(Phase::Sinking, r.app.scene().phase());
  r.advance(10000, still);
  TEST_ASSERT_EQUAL(Phase::Idle, r.app.scene().phase());
  TEST_ASSERT_TRUE(r.app.scene().frame().promptAlpha > 0.9f);
  TEST_ASSERT_TRUE(r.app.scene().frame().dieFog > 0.85f);       // back in the deep
}

void test_tap_while_idle_blows_bubbles() {
  Run r;
  r.advance(1500, still);
  int before = r.app.scene().frame().bubbleCount;
  r.app.tap(60, 90);
  r.advance(1600, still);
  TEST_ASSERT_TRUE(r.app.scene().frame().bubbleCount >= before + 4);
  TEST_ASSERT_EQUAL(Phase::Idle, r.app.scene().phase());
}

void test_pack_swipe_wraps_and_reprints_the_label() {
  Run r;
  r.advance(500, still);
  r.app.swipePack(-1);
  FrameOut out;
  r.app.frame(gFb.data(), r.t, out);
  TEST_ASSERT_EQUAL_INT(packCount() - 1, r.app.pack());
  TEST_ASSERT_TRUE(out.packChanged);
  TEST_ASSERT_TRUE(out.labelFlush);
  TEST_ASSERT_TRUE(out.labelY0 >= kWinBottom);                  // never overlaps the window
  TEST_ASSERT_TRUE(out.labelY0 + out.labelRows <= kScreenH);
  r.app.swipePack(+1);
  r.app.frame(gFb.data(), r.t + 30, out);
  TEST_ASSERT_EQUAL_INT(0, r.app.pack());
}

void test_frames_only_touch_the_window_rows() {
  Run r;
  std::vector<uint16_t> ball = gFb;   // what enter() painted
  const uint16_t kSentinel = 0xF81F;
  for (int y = 0; y < kScreenH; y++)
    if (y < kWinTop || y >= kWinBottom)
      for (int x = 0; x < kScreenW; x++) gFb[y * kScreenW + x] = kSentinel;
  r.advance(7000, ShakeBetween{1.0f, 2.5f});
  for (int y = 0; y < kScreenH; y++) {
    for (int x = 0; x < kScreenW; x++) {
      uint16_t v = gFb[y * kScreenW + x];
      if (y < kWinTop || y >= kWinBottom) {
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(kSentinel, v, "row outside the window");
        continue;
      }
      // Inside the window rows, the ball around the glass is never repainted.
      float dx = x + 0.5f - kBallCX, dy = y + 0.5f - kBallCY;
      if (dx * dx + dy * dy > (kWinR + 1.5f) * (kWinR + 1.5f))
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(ball[y * kScreenW + x], v, "ball outside the glass");
    }
  }
}

void test_same_seed_same_pixels() {
  auto runOnce = [](uint64_t seed) {
    Run r(seed);
    r.advance(6000, ShakeBetween{1.0f, 2.4f});
    uint32_t h = 2166136261u;
    for (uint16_t v : gFb) { h ^= v; h *= 16777619u; }
    return h;
  };
  TEST_ASSERT_EQUAL_HEX32(runOnce(99), runOnce(99));
}

void test_frame_rate_hint_follows_the_action() {
  Run r;
  r.advance(4000, still);                                       // settled idle
  TEST_ASSERT_EQUAL_UINT16(kFramePeriodCalm, r.app.framePeriodMs());
  r.advance(5500, ShakeBetween{4.0f, 6.0f});                    // shaking
  TEST_ASSERT_EQUAL_UINT16(kFramePeriodBusy, r.app.framePeriodMs());
  r.advance(12000, still);                                      // answer at rest
  TEST_ASSERT_EQUAL(Phase::Showing, r.app.scene().phase());
  TEST_ASSERT_EQUAL_UINT16(kFramePeriodCalm, r.app.framePeriodMs());
}

void test_hold_to_churn_without_any_imu() {
  // Press-and-hold (and the serial console) drive the same path: works even
  // when the accelerometer is missing and no samples ever arrive.
  OracleApp app;
  TEST_ASSERT_TRUE(app.begin(&FreeSansBold24pt7b, hostAlloc, 3));
  app.setImuOk(false);
  app.enter(gFb.data(), 0);
  FrameOut out;
  uint32_t t = 0;
  for (; t < 1000; t += 30) app.frame(gFb.data(), t, out);
  app.simulateShake(true);
  int rumbles = 0, landed = 0;
  for (; t < 1900; t += 28) {
    app.frame(gFb.data(), t, out);
    rumbles += out.buzzCount;
  }
  TEST_ASSERT_EQUAL(Phase::Churning, app.scene().phase());
  TEST_ASSERT_TRUE(rumbles >= 5);
  app.simulateShake(false);
  for (; t < 5500; t += 28) {
    app.frame(gFb.data(), t, out);
    if (out.landed) landed++;
  }
  TEST_ASSERT_EQUAL(Phase::Showing, app.scene().phase());
  TEST_ASSERT_EQUAL_INT(1, landed);
}

void test_imu_dropout_still_ends_a_shake() {
  Run r;
  r.advance(2600, ShakeBetween{1.0f, 99.f});                    // shaking hard
  TEST_ASSERT_EQUAL(Phase::Churning, r.app.scene().phase());
  // The accelerometer stops answering: the view calls imuIdle() each frame.
  for (uint32_t t = r.t; t < r.t + 1200; t += 30) {
    r.app.imuIdle(t);
    FrameOut out;
    r.app.frame(gFb.data(), t, out);
  }
  TEST_ASSERT_TRUE(r.app.scene().phase() == Phase::Rising || r.app.scene().phase() == Phase::Showing);
}

void test_random_abuse_stays_finite_and_in_bounds() {
  Run r(5);
  Pcg32 rng(2024);
  uint32_t t = 0;
  for (int i = 0; i < 20000; i++) {
    t += rng.below(121);                                        // 0..120 ms frames
    int n = (int)rng.below(6);
    for (int k = 0; k < n; k++) {
      int16_t big = (int16_t)((int)rng.below(16384) - 8192);
      r.app.imuSample(t, big, (int16_t)((int)rng.below(16384) - 8192), (int16_t)((int)rng.below(16384) - 8192));
    }
    switch (rng.below(60)) {
      case 0: r.app.tap((float)rng.below(240), (float)rng.below(280)); break;
      case 1: r.app.swipePack(rng.below(2) ? 1 : -1); break;
      case 2: r.app.simulateShake(true); break;
      case 3: r.app.simulateShake(false); break;
      case 4: r.app.setImuOk(rng.below(2) != 0); break;
      default: break;
    }
    FrameOut out;
    r.app.frame(gFb.data(), t, out);
    const FrameState &f = r.app.scene().frame();
    TEST_ASSERT_TRUE(f.dieX == f.dieX && f.dieY == f.dieY && f.dieAngle == f.dieAngle);
    TEST_ASSERT_TRUE(f.dieScale > 0.4f && f.dieScale < 1.1f);
    TEST_ASSERT_TRUE(f.dieFog >= 0.f && f.dieFog <= 1.f);
    TEST_ASSERT_TRUE(fabsf(f.dieX - kBallCX) <= 26.f && fabsf(f.dieY - kBallCY - kDieRestDy) <= 26.f);
    TEST_ASSERT_TRUE(f.bubbleCount >= 0 && f.bubbleCount <= Scene::kMaxBubbles);
    TEST_ASSERT_TRUE(r.app.pack() >= 0 && r.app.pack() < packCount());
    uint16_t p = r.app.framePeriodMs();
    TEST_ASSERT_TRUE(p == kFramePeriodBusy || p == kFramePeriodCalm);
  }
}

void test_planner_never_floods_the_driver() {
  HapticPlanner hp;
  hp.reset();
  MotorModel motor;
  Buzz out[HapticPlanner::kMax];
  Pcg32 rng(8);
  for (uint32_t t = 0; t < 60000; t += 10 + rng.below(40)) {
    bool thunk = rng.below(40) == 0;
    int n = hp.plan(t, rng.unit(), thunk, rng.below(2) != 0, rng.below(10) == 0, out);
    TEST_ASSERT_TRUE(n >= 0 && n <= HapticPlanner::kMax);
    for (int i = 0; i < n; i++) {
      TEST_ASSERT_TRUE(out[i].intensity > 0);   // pauses are timed, never sent
      motor.post(t, out[i]);
    }
  }
  TEST_ASSERT_FALSE(motor.overflow);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_shake_then_answer_full_cycle);
  RUN_TEST(test_answer_comes_from_the_current_pack);
  RUN_TEST(test_haptics_rumble_then_thunk_without_flooding);
  RUN_TEST(test_rumble_stops_promptly_when_shaking_stops);
  RUN_TEST(test_golden_answer_twinkles);
  RUN_TEST(test_tap_sinks_the_answer_and_the_prompt_returns);
  RUN_TEST(test_tap_while_idle_blows_bubbles);
  RUN_TEST(test_pack_swipe_wraps_and_reprints_the_label);
  RUN_TEST(test_frames_only_touch_the_window_rows);
  RUN_TEST(test_same_seed_same_pixels);
  RUN_TEST(test_frame_rate_hint_follows_the_action);
  RUN_TEST(test_hold_to_churn_without_any_imu);
  RUN_TEST(test_imu_dropout_still_ends_a_shake);
  RUN_TEST(test_random_abuse_stays_finite_and_in_bounds);
  RUN_TEST(test_planner_never_floods_the_driver);
  return UNITY_END();
}
