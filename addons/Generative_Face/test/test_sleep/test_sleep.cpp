// Simulates the screen-off sleep policy against scripted days: the watch
// must keep counting while you walk, sink to deep sleep when still, always
// answer the button, and never spin on a stuck interrupt.
#include <stdint.h>
#include <stdio.h>
#include <unity.h>
#include "sleep_policy.h"

using gf::SleepPolicy;
using gf::WakeCause;

namespace {
// The simulated world: what happens, and when.
struct World {
  uint32_t now = 0;
  uint32_t steps = 0;
  bool     walking = false;     // FIFO wakes produce steps
  uint32_t motionAt = 0;        // next motion interrupt (0 = none)
  uint32_t buttonAt = 0;        // next button press (0 = none)
  bool     fifoStuck = false;   // INT2 stuck high
  bool     motionStuck = false; // INT1 stuck high (stays high after its latch is read)
  bool     drainFails = false;  // accelerometer not answering
  uint32_t spuriousTouchEvery = 0;
};

struct Result {
  bool     deep = false, user = false;
  uint32_t at = 0, wakes = 0, fifoWakes = 0, timerWakes = 0, motionWakes = 0;
  uint32_t maxHorizon = 0, minHorizon = 0xFFFFFFFF;
  uint32_t houses = 0, lastHouse = 0, maxHouseGap = 0;
};

Result simulate(World &w, SleepPolicy &p, uint32_t untilMs, bool accel = true) {
  Result r;
  p.begin(w.now, accel, w.steps);
  uint32_t nextTouch = w.spuriousTouchEvery ? w.now + w.spuriousTouchEvery : 0;
  r.lastHouse = w.now;
  while (w.now < untilMs) {
    if (p.housekeepingDue(w.now)) {
      if (r.houses) { uint32_t gap = w.now - r.lastHouse; if (gap > r.maxHouseGap) r.maxHouseGap = gap; }
      r.houses++; r.lastHouse = w.now;
      p.housekeepingDone(w.now);
    }
    uint32_t horizon = 0;
    if (p.plan(w.now, w.steps, horizon) == SleepPolicy::kDeepSleep) { r.deep = true; r.at = w.now; return r; }
    if (horizon > r.maxHorizon) r.maxHorizon = horizon;
    if (horizon < r.minHorizon) r.minHorizon = horizon;
    // Earliest event among the armed sources.
    uint32_t t = w.now + horizon;
    WakeCause c = WakeCause::Timer;
    if (p.fifoArmed) {
      uint32_t f = w.fifoStuck ? w.now + 2 : w.now + 600;
      if (f < t) { t = f; c = WakeCause::Fifo; }
    }
    if (p.motionArmed && w.motionAt && w.motionAt >= w.now && w.motionAt < t) { t = w.motionAt; c = WakeCause::Motion; }
    if (p.motionArmed && w.motionStuck && w.now + 2 < t) { t = w.now + 2; c = WakeCause::Motion; }
    if (p.touchArmed && nextTouch && nextTouch < t) { t = nextTouch; c = WakeCause::Touch; }
    if (w.buttonAt && w.buttonAt >= w.now && w.buttonAt <= t) { t = w.buttonAt; c = WakeCause::Button; }
    w.now = t;
    r.wakes++;
    if (c == WakeCause::Fifo) { r.fifoWakes++; if (w.walking) w.steps += 1; }
    if (c == WakeCause::Timer) r.timerWakes++;
    if (c == WakeCause::Motion) { r.motionWakes++; if (!w.motionStuck) w.motionAt = 0; }
    if (c == WakeCause::Touch) nextTouch = w.now + w.spuriousTouchEvery;
    bool fifoWas = p.fifoArmed;
    bool stuck = (c == WakeCause::Motion) && w.motionStuck;
    SleepPolicy::Outcome o = p.onWake(w.now, c, fifoWas, !w.drainFails, false, stuck);
    if (o == SleepPolicy::kUserWake) { r.user = true; r.at = w.now; return r; }
    if (o == SleepPolicy::kDeepSleepNow) { r.deep = true; r.at = w.now; return r; }
  }
  r.at = w.now;
  return r;
}
const uint32_t MIN = 60000;
}  // namespace

void test_still_watch_sinks_to_deep_sleep() {
  World w; SleepPolicy p;
  Result r = simulate(w, p, 60 * MIN);
  TEST_ASSERT_TRUE(r.deep);
  // 90 s in tier 1, then 15 minutes of stillness.
  TEST_ASSERT_UINT32_WITHIN(2000, 90000 + 15 * MIN, r.at);
  TEST_ASSERT_TRUE(r.maxHorizon <= 15000);
  TEST_ASSERT_TRUE(r.minHorizon >= 20);
  // Tier 2 wakes only for timers: about one every 15 s, not every 0.6 s.
  TEST_ASSERT_TRUE(r.timerWakes < 90);
}

void test_walking_keeps_counting() {
  World w; w.walking = true; SleepPolicy p;
  Result r = simulate(w, p, 3 * 60 * MIN);
  TEST_ASSERT_FALSE(r.deep);
  TEST_ASSERT_FALSE(r.user);
  TEST_ASSERT_TRUE(p.fifoArmed);
  TEST_ASSERT_TRUE(w.steps > 15000);            // three hours of FIFO wakes
  TEST_ASSERT_TRUE(r.maxHouseGap <= 31000);     // clock + rollover keep running
}

void test_stopping_after_a_long_walk_is_not_instant_deep_sleep() {
  World w; w.walking = true; SleepPolicy p;
  simulate(w, p, 40 * MIN);
  // Stop walking: continue the same policy without re-beginning.
  w.walking = false;
  uint32_t stopAt = w.now;
  Result r;
  for (;;) {
    if (p.housekeepingDue(w.now)) p.housekeepingDone(w.now);
    uint32_t h;
    if (p.plan(w.now, w.steps, h) == SleepPolicy::kDeepSleep) { r.deep = true; r.at = w.now; break; }
    w.now += p.fifoArmed ? 600 : h;
    p.onWake(w.now, p.fifoArmed ? WakeCause::Fifo : WakeCause::Timer, p.fifoArmed, true, false);
  }
  TEST_ASSERT_TRUE(r.deep);
  TEST_ASSERT_UINT32_WITHIN(3000, 90000 + 15 * MIN, r.at - stopAt);
}

void test_motion_wake_returns_to_counting_and_restarts_stillness() {
  World w; SleepPolicy p;
  w.motionAt = 10 * MIN;                         // someone picks the watch up
  Result r = simulate(w, p, 60 * MIN);
  TEST_ASSERT_TRUE(r.deep);
  // Deep sleep 25 s (tier 1 hold) + 15 min after the motion, not 15 min
  // after the first stillness.
  TEST_ASSERT_UINT32_WITHIN(2000, 10 * MIN + 25000 + 15 * MIN, r.at);
}

void test_stuck_motion_line_still_reaches_deep_sleep() {
  World w; w.motionStuck = true; SleepPolicy p;
  Result r = simulate(w, p, 60 * MIN);
  TEST_ASSERT_TRUE(r.deep);
  // Three immediate wakes, each a short tier-1 spell, then INT1 is muted;
  // the stillness clock kept running from the first tier-2 entry.
  TEST_ASSERT_EQUAL_UINT32(3, r.motionWakes);
  TEST_ASSERT_TRUE(p.motionMuted());
  TEST_ASSERT_TRUE(p.storms >= 1);
  TEST_ASSERT_UINT32_WITHIN(2000, 90000 + 15 * MIN, r.at);
}

void test_steps_after_a_stuck_looking_wake_keep_counting() {
  // A stuck-looking wake while the wearer really is walking: the tier-1
  // spell finds steps, so counting carries on and nothing is muted.
  World w; w.motionStuck = true; SleepPolicy p;
  p.begin(0, true, 0);
  uint32_t now = 100000, h = 0;
  p.plan(now, 0, h);                               // into tier 2
  TEST_ASSERT_TRUE(p.motionArmed);
  p.onWake(now + 2, WakeCause::Motion, false, true, false, /*motionStuck=*/true);
  TEST_ASSERT_TRUE(p.fifoArmed);
  uint32_t steps = 0;
  for (int i = 0; i < 600; i++) {                  // six minutes of walking
    now += 600;
    steps += 1;
    TEST_ASSERT_EQUAL(SleepPolicy::kSleep, p.plan(now, steps, h));
    p.onWake(now, WakeCause::Fifo, true, true, false);
  }
  TEST_ASSERT_TRUE(p.fifoArmed);
  TEST_ASSERT_FALSE(p.motionMuted());
}

void test_button_always_wakes() {
  {
    World w; w.buttonAt = 5000; SleepPolicy p;
    Result r = simulate(w, p, 60 * MIN);
    TEST_ASSERT_TRUE(r.user);
    TEST_ASSERT_EQUAL_UINT32(5000, r.at);
  }
  {
    // Even while every other source misbehaves.
    World w; w.fifoStuck = true; w.drainFails = true; w.spuriousTouchEvery = 50;
    w.buttonAt = 20000;
    SleepPolicy p;
    Result r = simulate(w, p, 60 * MIN);
    TEST_ASSERT_TRUE(r.user);
    TEST_ASSERT_EQUAL_UINT32(20000, r.at);
  }
}

void test_stuck_fifo_interrupt_does_not_spin() {
  World w; w.fifoStuck = true; SleepPolicy p;
  Result r = simulate(w, p, 10 * MIN);
  TEST_ASSERT_TRUE(p.storms >= 1);
  TEST_ASSERT_FALSE(p.fifoArmed);
  // A few dozen fast wakes at most, then timer wakes only.
  TEST_ASSERT_TRUE(r.fifoWakes < 30);
}

void test_failing_accelerometer_falls_back_safely() {
  World w; w.drainFails = true; SleepPolicy p;
  Result r = simulate(w, p, 60 * MIN);
  TEST_ASSERT_TRUE(r.deep);
  TEST_ASSERT_FALSE(p.motionArmed);
  TEST_ASSERT_TRUE(r.fifoWakes <= 22);
}

void test_spurious_touch_storm_mutes_touch_for_a_minute() {
  World w; w.spuriousTouchEvery = 200; SleepPolicy p;
  p.begin(0, true, 0);
  uint32_t now = 0, mutedAt = 0;
  for (int i = 0; i < 400; i++) {
    now += 200;
    uint32_t h;
    p.plan(now, 0, h);
    if (!p.touchArmed) { if (!mutedAt) mutedAt = now; continue; }
    if (mutedAt) continue;                       // the interference has stopped
    p.onWake(now, WakeCause::Touch, false, true, false);
  }
  TEST_ASSERT_TRUE(mutedAt > 0);
  TEST_ASSERT_TRUE(mutedAt <= 16 * 200 + 200);
  TEST_ASSERT_TRUE(p.touchArmed);                // unmuted again after 60 s
  TEST_ASSERT_EQUAL(SleepPolicy::kUserWake, p.onWake(now, WakeCause::Touch, false, true, true));
}

void test_spurious_none_storm_goes_to_deep_sleep() {
  SleepPolicy p;
  p.begin(0, true, 0);
  SleepPolicy::Outcome o = SleepPolicy::kContinue;
  int n = 0;
  while (o == SleepPolicy::kContinue && n < 1000) { o = p.onWake(10 + n, WakeCause::None, false, true, false); n++; }
  TEST_ASSERT_EQUAL(SleepPolicy::kDeepSleepNow, o);
  TEST_ASSERT_TRUE(n <= 41);
}

void test_no_accelerometer_still_sleeps() {
  World w; SleepPolicy p;
  Result r = simulate(w, p, 60 * MIN, /*accel=*/false);
  TEST_ASSERT_TRUE(r.deep);
  TEST_ASSERT_FALSE(p.fifoArmed);
  TEST_ASSERT_FALSE(p.motionArmed);
  TEST_ASSERT_EQUAL_UINT32(0, r.fifoWakes);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_still_watch_sinks_to_deep_sleep);
  RUN_TEST(test_walking_keeps_counting);
  RUN_TEST(test_stopping_after_a_long_walk_is_not_instant_deep_sleep);
  RUN_TEST(test_motion_wake_returns_to_counting_and_restarts_stillness);
  RUN_TEST(test_stuck_motion_line_still_reaches_deep_sleep);
  RUN_TEST(test_steps_after_a_stuck_looking_wake_keep_counting);
  RUN_TEST(test_button_always_wakes);
  RUN_TEST(test_stuck_fifo_interrupt_does_not_spin);
  RUN_TEST(test_failing_accelerometer_falls_back_safely);
  RUN_TEST(test_spurious_touch_storm_mutes_touch_for_a_minute);
  RUN_TEST(test_spurious_none_storm_goes_to_deep_sleep);
  RUN_TEST(test_no_accelerometer_still_sleeps);
  return UNITY_END();
}
