// Host tests for the pet rules (src/apps/pet/petmodel.cpp), driven over
// simulated days together with the real step book (src/core/steps.cpp), the
// way the watch drives them: frequent small updates with the current RTC
// epoch, the step service's lifetime counter and today's count.
#include <unity.h>
#include <math.h>
#include <stdio.h>
#include <vector>
#include "petmodel.h"
#include "steps.h"

using pet::Model;
using pet::Mood;
using pet::Stage;
using steps::StepBook;

namespace {

const uint32_t kDay0 = 9400;   // an arbitrary local day number (~2025)

uint32_t at(uint32_t day, int h, int m = 0) {
  return day * 86400u + (uint32_t)h * 3600u + (uint32_t)m * 60u;
}

// A walking schedule: (startMinuteOfDay, endMinuteOfDay, steps) blocks.
struct Block { int from, to; uint32_t steps; };

std::vector<Block> normalDay(uint32_t total) {
  // ~7k shape scaled to `total`: commute, lunch walk, commute, ambient.
  double k = total / 7000.0;
  return {
    {7 * 60 + 30, 8 * 60,       (uint32_t)(1800 * k)},
    {8 * 60,      12 * 60,      (uint32_t)(600 * k)},
    {12 * 60 + 15, 12 * 60 + 35, (uint32_t)(1500 * k)},
    {12 * 60 + 35, 17 * 60 + 30, (uint32_t)(800 * k)},
    {17 * 60 + 30, 18 * 60,     (uint32_t)(1800 * k)},
    {18 * 60,     22 * 60,      (uint32_t)(500 * k)},
  };
}

std::vector<Block> lazyDay(uint32_t total) {
  return {{8 * 60, 22 * 60, total}};      // ambient shuffling only
}

struct Sim {
  Model pet;
  StepBook book;
  uint32_t now;
  pet::Events lastEv;
  uint32_t eaten = 0, stored = 0, wasted = 0, earned = 0;

  Sim() {
    book.init();
    now = at(kDay0, 6, 0);
    pet.newPet(now, 0, 3);
  }

  void hatchAt(uint32_t day, int h, float fullness) {
    advanceTo(at(day, h, 0));
    addSteps(300);
    pet.debugSetFullness(fullness);
  }

  void addSteps(uint32_t n) {
    book.add(n, steps::dayOf(now));
    lastEv = pet.update(now, book.lifetime, book.today, book.day);
    earned += lastEv.snacksEarned; eaten += lastEv.snacksEaten;
    stored += lastEv.snacksStored; wasted += lastEv.snacksWasted;
  }

  void advanceTo(uint32_t t) {
    // Minute ticks, like the watch's frequent updates.
    while (now < t) {
      now += 60;
      if (now > t) now = t;
      book.rollTo(steps::dayOf(now));
      lastEv = pet.update(now, book.lifetime, book.today, book.day);
    }
  }

  // Walk one day following `blocks`, sampling the mood at the given hours.
  void runDay(uint32_t day, const std::vector<Block> &blocks, const int *probeH, Mood *out, int nProbe) {
    int probe = 0;
    for (int minute = 0; minute < 24 * 60; minute++) {
      uint32_t t = at(day, 0, minute);
      advanceTo(t);
      for (const Block &b : blocks) {
        if (minute >= b.from && minute < b.to) {
          uint32_t len = (uint32_t)(b.to - b.from);
          uint32_t idx = (uint32_t)(minute - b.from);
          uint32_t per = b.steps / len + ((idx < b.steps % len) ? 1u : 0u);
          if (per) addSteps(per);
        }
      }
      while (probe < nProbe && minute == probeH[probe] * 60) {
        out[probe] = pet.mood();
        probe++;
      }
    }
    advanceTo(at(day + 1, 0, 0));
  }
};

const char *mn(Mood m) { return Model::moodName(m); }

}  // namespace

// ===========================================================================
// The brief: 0 steps -> grumpy within N hours
// ===========================================================================
void test_zero_steps_grumpy_within_hours() {
  Sim s;
  s.hatchAt(kDay0, 7, 85.0f);                  // a well-fed morning
  TEST_ASSERT_EQUAL(Mood::Happy, s.pet.mood());
  int grumpyAtH = -1, furiousAtH = -1;
  for (int h = 8; h <= 21; h++) {
    s.advanceTo(at(kDay0, h, 0));
    Mood m = s.pet.mood();
    printf("  %02d:00 fullness %5.1f %s\n", h, s.pet.fullness(), mn(m));
    if (grumpyAtH < 0 && m >= Mood::Grumpy) grumpyAtH = h;
    if (furiousAtH < 0 && m == Mood::Furious) furiousAtH = h;
  }
  printf("  grumpy after %d h, furious after %d h\n", grumpyAtH - 7, furiousAtH - 7);
  TEST_ASSERT_TRUE(grumpyAtH >= 7 + 6 && grumpyAtH <= 7 + 9);    // mid-afternoon
  TEST_ASSERT_TRUE(furiousAtH >= 7 + 8 && furiousAtH <= 7 + 11);  // by early evening
}

void test_zero_steps_from_morning_steady_state() {
  // The usual morning (after a normal day and a night) is "content": skipping
  // the morning walk makes it grumpy by lunchtime-ish.
  Sim s;
  s.hatchAt(kDay0, 7, 56.0f);
  s.advanceTo(at(kDay0, 11, 0));
  TEST_ASSERT_TRUE(s.pet.mood() >= Mood::Peckish);
  s.advanceTo(at(kDay0, 12, 0));
  TEST_ASSERT_TRUE(s.pet.mood() >= Mood::Grumpy);
}

// ===========================================================================
// The brief: a normal walking day keeps it happy, a lazy day -> grumpy by evening
// ===========================================================================
void test_normal_days_keep_it_happy() {
  for (uint32_t total : {6000u, 7000u, 8000u}) {
    Sim s;
    s.hatchAt(kDay0, 21, 72.0f);
    const int probes[] = {9, 13, 16, 19, 21};
    Mood m[5];
    for (uint32_t d = 0; d < 7; d++) {
      s.runDay(kDay0 + 1 + d, normalDay(total), probes, m, 5);
      printf("  %5u steps day %u: 09 %-8s 13 %-8s 16 %-8s 19 %-8s 21 %-8s bowl %u\n",
             total, d + 1, mn(m[0]), mn(m[1]), mn(m[2]), mn(m[3]), mn(m[4]), s.pet.pantry());
      if (d >= 1) {                       // after the first day
        for (int i = 0; i < 5; i++) TEST_ASSERT_TRUE(m[i] <= Mood::Content);
        TEST_ASSERT_TRUE(m[3] <= Mood::Happy);      // happy after the walk home
      }
    }
  }
}

void test_lazy_day_grumpy_by_evening() {
  Sim s;
  s.hatchAt(kDay0, 21, 72.0f);
  const int probes[] = {9, 13, 16, 19, 21};
  Mood m[5];
  for (uint32_t d = 0; d < 3; d++) s.runDay(kDay0 + 1 + d, normalDay(7000), probes, m, 5);
  s.runDay(kDay0 + 4, lazyDay(1500), probes, m, 5);
  printf("  lazy 1500: 09 %s 13 %s 16 %s 19 %s 21 %s\n", mn(m[0]), mn(m[1]), mn(m[2]), mn(m[3]), mn(m[4]));
  TEST_ASSERT_TRUE(m[0] <= Mood::Happy);            // the morning is still fine
  TEST_ASSERT_TRUE(m[3] >= Mood::Grumpy);           // grumpy (or worse) by 19:00
  // and the next morning it starts out unhappy
  s.runDay(kDay0 + 5, lazyDay(0), probes, m, 5);
  TEST_ASSERT_TRUE(m[0] >= Mood::Grumpy);
}

void test_walking_recovers_visibly() {
  Sim s;
  s.hatchAt(kDay0, 7, 85.0f);
  s.advanceTo(at(kDay0, 19, 30));
  TEST_ASSERT_EQUAL(Mood::Furious, s.pet.mood());
  Mood prev = s.pet.mood();
  int improvements = 0;
  for (int i = 0; i < 40; i++) {          // a ~27-minute, 3,200-step walk
    s.advanceTo(s.now + 40);
    s.addSteps(80);
    if (s.pet.mood() < prev) improvements++;
    prev = s.pet.mood();
  }
  printf("  after the walk: fullness %.1f %s, mood went up %d times\n",
         s.pet.fullness(), mn(s.pet.mood()), improvements);
  TEST_ASSERT_TRUE(s.pet.mood() <= Mood::Content);
  TEST_ASSERT_TRUE(improvements >= 3);    // furious -> grumpy -> peckish -> content
}

// ===========================================================================
// Night
// ===========================================================================
void test_overnight_sleep_and_slow_drain() {
  Sim s;
  s.hatchAt(kDay0, 6, 70.0f);
  s.advanceTo(at(kDay0, 22, 0));
  s.pet.debugSetFullness(70.0f);
  TEST_ASSERT_TRUE(s.pet.asleep(at(kDay0, 22, 0)));
  TEST_ASSERT_TRUE(s.pet.asleep(at(kDay0 + 1, 3, 0)));
  TEST_ASSERT_FALSE(s.pet.asleep(at(kDay0 + 1, 7, 0)));
  TEST_ASSERT_FALSE(s.pet.asleep(at(kDay0, 21, 59)));
  s.advanceTo(at(kDay0 + 1, 7, 0));
  // 9 h at 1.5/h
  TEST_ASSERT_FLOAT_WITHIN(0.2f, 70.0f - 13.5f, s.pet.fullness());
  // no nudges at night even when furious
  s.pet.debugSetFullness(5.0f);
  TEST_ASSERT_FALSE(s.pet.nudgeDue(at(kDay0 + 1, 23, 30)));
  TEST_ASSERT_FALSE(s.pet.nudgeDue(at(kDay0 + 1, 3, 0)));
}

void test_midnight_rollover() {
  Sim s;
  s.hatchAt(kDay0, 6, 72.0f);
  s.advanceTo(at(kDay0, 23, 50));
  s.addSteps(900);                        // late walk: 2 snacks today
  uint16_t before = s.pet.state().snacksToday;
  TEST_ASSERT_TRUE(before >= 2);
  s.advanceTo(at(kDay0 + 1, 0, 10));
  TEST_ASSERT_EQUAL_UINT32(0, s.book.today);                    // steps reset
  TEST_ASSERT_TRUE(s.book.stepsOn(kDay0) >= 1200);              // kept in history
  s.addSteps(400);
  TEST_ASSERT_EQUAL_UINT16(1, s.pet.state().snacksToday);       // new day count
}

void test_goal_and_streak() {
  Sim s;
  s.hatchAt(kDay0, 6, 72.0f);
  const int probes[] = {12};
  Mood m[1];
  s.runDay(kDay0 + 1, normalDay(7000), probes, m, 1);
  TEST_ASSERT_EQUAL_UINT16(1, s.pet.currentStreak(kDay0 + 1));
  s.runDay(kDay0 + 2, normalDay(7000), probes, m, 1);
  s.runDay(kDay0 + 3, normalDay(7000), probes, m, 1);
  TEST_ASSERT_EQUAL_UINT16(3, s.pet.currentStreak(kDay0 + 3));
  TEST_ASSERT_EQUAL_UINT16(3, s.pet.currentStreak(kDay0 + 4));  // still alive the next day
  TEST_ASSERT_EQUAL(pet::Accessory::Bow, s.pet.accessory());
  s.runDay(kDay0 + 4, lazyDay(1000), probes, m, 1);              // missed
  TEST_ASSERT_EQUAL_UINT16(0, s.pet.currentStreak(kDay0 + 5));
  s.runDay(kDay0 + 5, normalDay(7000), probes, m, 1);
  TEST_ASSERT_EQUAL_UINT16(1, s.pet.currentStreak(kDay0 + 5));
  TEST_ASSERT_EQUAL_UINT16(3, s.pet.state().bestStreak);
  TEST_ASSERT_EQUAL(pet::Accessory::Bow, s.pet.accessory());     // unlocks are kept
}

// ===========================================================================
// Mechanics
// ===========================================================================
void test_replay_is_deterministic() {
  // One update across hours (what a deep sleep produces) must equal hundreds
  // of small ones (what light-sleep sampling produces) — including bowl
  // meals at the instant there is room and the bedtime feast.
  Model a, b;
  uint32_t steps = 300;
  a.newPet(at(kDay0, 7), 0, 1);
  b.newPet(at(kDay0, 7), 0, 1);
  a.update(at(kDay0, 7), steps, steps, kDay0);       // hatch both
  b.update(at(kDay0, 7), steps, steps, kDay0);
  steps += 400 * 12;                                 // big walk: full belly + full bowl
  a.update(at(kDay0, 7, 1), steps, 0, kDay0);
  b.update(at(kDay0, 7, 1), steps, 0, kDay0);
  TEST_ASSERT_EQUAL_UINT8(3, a.pantry());

  uint32_t end = at(kDay0, 13, 17);
  a.update(end, steps, 0, 0);
  for (uint32_t t = at(kDay0, 7, 1); t < end; t += 37) b.update(t, steps, 0, 0);
  b.update(end, steps, 0, 0);
  printf("  day:   one-shot %.3f bowl %u | stepped %.3f bowl %u\n", a.fullness(), a.pantry(), b.fullness(), b.pantry());
  TEST_ASSERT_TRUE(a.fullness() > 40.0f && a.fullness() < 100.0f);   // a meaningful comparison
  TEST_ASSERT_FLOAT_WITHIN(0.05f, a.fullness(), b.fullness());
  TEST_ASSERT_EQUAL_UINT8(a.pantry(), b.pantry());

  steps += 400 * 12;                                 // evening walk: bowl full again
  a.update(at(kDay0, 20), steps, 0, kDay0);
  b.update(at(kDay0, 20), steps, 0, kDay0);
  end = at(kDay0 + 1, 8, 30);                        // across the bedtime feast + night
  pet::Events ea = a.update(end, steps, 0, 0);
  uint32_t sharedB = 0;
  for (uint32_t t = at(kDay0, 20); t < end; t += 53) sharedB += b.update(t, steps, 0, 0).pantryShared;
  sharedB += b.update(end, steps, 0, 0).pantryShared;
  printf("  night: one-shot %.3f bowl %u shared %u | stepped %.3f bowl %u shared %u\n",
         a.fullness(), a.pantry(), ea.pantryShared, b.fullness(), b.pantry(), sharedB);
  TEST_ASSERT_FLOAT_WITHIN(0.05f, a.fullness(), b.fullness());
  TEST_ASSERT_EQUAL_UINT8(a.pantry(), b.pantry());
  TEST_ASSERT_EQUAL_UINT32(ea.pantryShared, sharedB);
  TEST_ASSERT_TRUE(ea.pantryShared > 0);
  TEST_ASSERT_EQUAL(a.mood(), b.mood());
}

void test_bowl_fills_then_feeds() {
  Sim s;
  s.hatchAt(kDay0, 8, 95.0f);
  s.addSteps(400 * 10);                     // 10 snacks while (nearly) full
  printf("  earned %u eaten %u stored %u wasted %u\n", s.earned, s.eaten, s.stored, s.wasted);
  TEST_ASSERT_EQUAL_UINT8(3, s.pet.pantry());
  TEST_ASSERT_TRUE(s.wasted >= 5);
  // the bowl keeps it fed into the afternoon
  s.advanceTo(at(kDay0, 13, 0));
  TEST_ASSERT_TRUE(s.pet.mood() <= Mood::Happy);
  TEST_ASSERT_TRUE(s.pet.pantry() < 3);
  // and leftovers don't survive bedtime
  s.addSteps(400 * 8);
  TEST_ASSERT_TRUE(s.pet.pantry() > 0);
  s.advanceTo(at(kDay0 + 1, 7, 0));
  TEST_ASSERT_EQUAL_UINT8(0, s.pet.pantry());
}

void test_serve_from_bowl() {
  Sim s;
  s.hatchAt(kDay0, 8, 99.0f);
  s.addSteps(800);                          // 2 snacks -> bowl
  TEST_ASSERT_EQUAL_UINT8(2, s.pet.pantry());
  TEST_ASSERT_FALSE(s.pet.serveFromPantry(s.now));      // too full
  s.pet.debugSetFullness(80.0f);
  TEST_ASSERT_TRUE(s.pet.serveFromPantry(s.now));
  TEST_ASSERT_EQUAL_UINT8(1, s.pet.pantry());
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 88.5f, s.pet.fullness());
}

void test_egg_hatches_on_first_walk() {
  Sim s;
  TEST_ASSERT_TRUE(s.pet.isEgg());
  s.advanceTo(at(kDay0, 18, 0));            // an egg doesn't get hungry
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 72.0f, s.pet.fullness());
  s.addSteps(250);
  TEST_ASSERT_TRUE(s.pet.isEgg());
  TEST_ASSERT_EQUAL_UINT32(50, s.pet.stepsToHatch());
  s.addSteps(60);
  TEST_ASSERT_TRUE(s.lastEv.hatched);
  TEST_ASSERT_EQUAL(Stage::Baby, s.pet.stage());
  TEST_ASSERT_EQUAL_UINT16(390, s.pet.stepsToNextSnack());   // 10 spare steps banked
}

void test_evolution_waits_for_a_fed_pet() {
  Sim s;
  s.hatchAt(kDay0, 8, 20.0f);               // grumpy baby
  s.addSteps(24000);                        // crosses 25k lifetime... almost
  TEST_ASSERT_EQUAL(Stage::Baby, s.pet.stage());
  s.pet.debugSetFullness(20.0f);
  s.addSteps(1000);                         // 25,300: but snacks fed it
  // The 1,000 steps fed 2 snacks (20 -> 37): fed enough, so it evolves.
  TEST_ASSERT_EQUAL(Stage::Kid, s.pet.stage());
  Sim g;
  g.hatchAt(kDay0, 8, 5.0f);
  g.addSteps(24500);                         // walked 24,800: not yet
  TEST_ASSERT_EQUAL(Stage::Baby, g.pet.stage());
  g.pet.debugSetFullness(5.0f);
  g.addSteps(399);                           // past 25k, one snack: still furious
  TEST_ASSERT_EQUAL(Stage::Baby, g.pet.stage());
  g.pet.debugSetFullness(60.0f);
  g.addSteps(1);
  TEST_ASSERT_TRUE(g.lastEv.evolved);
  TEST_ASSERT_EQUAL(Stage::Kid, g.pet.stage());
}

void test_sulks_after_two_furious_hours() {
  Sim s;
  s.hatchAt(kDay0, 8, 16.0f);
  s.advanceTo(at(kDay0, 9, 0));
  TEST_ASSERT_EQUAL(Mood::Furious, s.pet.mood());
  TEST_ASSERT_FALSE(s.pet.sulking(s.now));
  s.advanceTo(at(kDay0, 11, 0));
  TEST_ASSERT_TRUE(s.pet.sulking(s.now));
  s.addSteps(1200);                         // fed: no more sulking
  TEST_ASSERT_FALSE(s.pet.sulking(s.now));
}

void test_nudge_rules() {
  Sim s;
  s.hatchAt(kDay0, 7, 40.0f);
  s.advanceTo(at(kDay0, 9, 0));             // grumpy-ish by now
  s.pet.debugSetFullness(25.0f);
  TEST_ASSERT_EQUAL(Mood::Grumpy, s.pet.mood());
  // walked at 07:00 (hatch); 2 h ago: due
  TEST_ASSERT_TRUE(s.pet.nudgeDue(s.now));
  s.pet.markNudged(s.now);
  s.advanceTo(at(kDay0, 10, 0));
  s.pet.debugSetFullness(25.0f);
  TEST_ASSERT_FALSE(s.pet.nudgeDue(s.now));          // < 2 h since the last
  s.advanceTo(at(kDay0, 11, 5));
  s.pet.debugSetFullness(25.0f);
  TEST_ASSERT_TRUE(s.pet.nudgeDue(s.now));
  s.pet.markNudged(s.now);
  s.addSteps(10);                                    // walking resets the quiet timer
  s.advanceTo(at(kDay0, 13, 20));
  s.pet.debugSetFullness(25.0f);
  TEST_ASSERT_TRUE(s.pet.nudgeDue(s.now));
  s.pet.markNudged(s.now);                           // 3rd of the day
  s.advanceTo(at(kDay0, 16, 0));
  s.pet.debugSetFullness(25.0f);
  TEST_ASSERT_FALSE(s.pet.nudgeDue(s.now));          // daily cap
  TEST_ASSERT_FALSE(s.pet.nudgeDue(at(kDay0, 20, 30)));   // evening: never
  // a content pet never nudges
  s.pet.debugSetFullness(60.0f);
  TEST_ASSERT_FALSE(s.pet.nudgeDue(at(kDay0 + 1, 12, 0)));
}

void test_next_nudge_time_prediction() {
  Sim s;
  s.hatchAt(kDay0, 7, 60.0f);
  uint32_t t = s.pet.nextNudgeTime(s.now);
  // 60 -> 32 at 6.5/h is ~4.3 h; quiet 30 min after the hatch walk is long over.
  TEST_ASSERT_TRUE(t > at(kDay0, 11, 0) && t <= at(kDay0, 11, 40));
  s.advanceTo(at(kDay0, 15, 0));
  s.pet.debugSetFullness(100.0f);
  s.advanceTo(at(kDay0, 19, 0));
  // full at 15:00 -> not grumpy before the 20:00 cut-off; after the night
  // (74 at 19:00 -> ~41 at 07:00) it is grumpy by 09:00 tomorrow
  uint32_t t2 = s.pet.nextNudgeTime(s.now);
  TEST_ASSERT_EQUAL_UINT32(at(kDay0 + 1, 9, 0), t2 - (t2 % 600));
}

void test_clock_backwards_and_long_absence() {
  Sim s;
  s.hatchAt(kDay0, 9, 80.0f);
  float f = s.pet.fullness();
  s.pet.update(at(kDay0, 8, 0), s.book.lifetime, s.book.today, kDay0);   // clock set back 1 h
  TEST_ASSERT_FLOAT_WITHIN(0.01f, f, s.pet.fullness());                  // no drift, no crash
  s.pet.update(at(kDay0 + 40, 12, 0), s.book.lifetime, 0, kDay0 + 40);   // 40 days away
  TEST_ASSERT_EQUAL(Mood::Furious, s.pet.mood());
  TEST_ASSERT_TRUE(s.pet.fullness() >= 0.0f);
}

void test_invalid_clock_still_feeds() {
  Model m;
  m.newPet(0, 0, 0);                         // no clock at all
  pet::Events ev = m.update(0, 300, 0, 0);
  TEST_ASSERT_TRUE(ev.hatched);
  ev = m.update(0, 300 + 800, 0, 0);
  TEST_ASSERT_EQUAL_UINT8(2, ev.snacksEarned);
  ev = m.update(at(kDay0, 10), 1100, 0, kDay0);   // clock arrives: anchors, no drift
  TEST_ASSERT_TRUE(m.fullness() > 72.0f);
}

void test_load_rejects_garbage() {
  Model m;
  pet::State bad;
  for (size_t i = 0; i < sizeof(bad); i++) ((uint8_t *)&bad)[i] = 0xA5;
  TEST_ASSERT_FALSE(m.load(bad));
  pet::State weird = m.state();
  weird.fullness = NAN;
  weird.stage = 77;
  weird.pantry = 200;
  TEST_ASSERT_TRUE(m.load(weird));
  TEST_ASSERT_TRUE(m.fullness() >= 0.0f && m.fullness() <= 100.0f);
  TEST_ASSERT_TRUE(m.stage() < Stage::Count);
  TEST_ASSERT_TRUE(m.pantry() <= m.tuning().pantryMax);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_zero_steps_grumpy_within_hours);
  RUN_TEST(test_zero_steps_from_morning_steady_state);
  RUN_TEST(test_normal_days_keep_it_happy);
  RUN_TEST(test_lazy_day_grumpy_by_evening);
  RUN_TEST(test_walking_recovers_visibly);
  RUN_TEST(test_overnight_sleep_and_slow_drain);
  RUN_TEST(test_midnight_rollover);
  RUN_TEST(test_goal_and_streak);
  RUN_TEST(test_replay_is_deterministic);
  RUN_TEST(test_bowl_fills_then_feeds);
  RUN_TEST(test_serve_from_bowl);
  RUN_TEST(test_egg_hatches_on_first_walk);
  RUN_TEST(test_evolution_waits_for_a_fed_pet);
  RUN_TEST(test_sulks_after_two_furious_hours);
  RUN_TEST(test_nudge_rules);
  RUN_TEST(test_next_nudge_time_prediction);
  RUN_TEST(test_clock_backwards_and_long_absence);
  RUN_TEST(test_invalid_clock_still_feeds);
  RUN_TEST(test_load_rejects_garbage);
  return UNITY_END();
}
