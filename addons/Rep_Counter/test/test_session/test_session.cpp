// Rep Counter: session / log / persistence tests (pure C++).
#include <unity.h>
#include <cstring>
#include "rep_haptics.h"
#include "rep_harness.h"
#include "rep_scenarios.h"
#include "rep_session.h"

using namespace reps;

static Date day(uint16_t y, uint8_t m, uint8_t d, uint8_t h = 10, uint8_t mi = 0) {
  Date x;
  x.year = y; x.month = m; x.day = d; x.hour = h; x.minute = mi; x.valid = true;
  return x;
}

static Event ev(EventType t, uint8_t reps, Exercise ex = Exercise::Curl, float tempo = 2.5f,
                float setSec = 0.f) {
  Event e;
  e.type = t;
  e.reps = reps;
  e.exercise = ex;
  e.confidence = 3;
  e.tempoSec = tempo;
  e.setSec = setSec;
  return e;
}

static Session fresh(Settings st = Settings()) {
  Session s;
  s.init(st, DayLog(), History());
  s.setToday(day(2026, 10, 1));
  return s;
}

static void test_full_set_cycle() {
  Session s = fresh();
  TEST_ASSERT_EQUAL_INT((int)Phase::Idle, (int)s.phase());
  TEST_ASSERT_FALSE(s.blocksSleep());
  s.start(1000);
  TEST_ASSERT_EQUAL_INT((int)Phase::Ready, (int)s.phase());
  TEST_ASSERT_TRUE(s.takeCues() & CueStart);
  TEST_ASSERT_TRUE(s.blocksSleep());

  Date now = day(2026, 10, 1, 18, 30);
  s.onEvent(ev(EventType::RepTentative, 1), 2000, now);
  TEST_ASSERT_EQUAL_INT((int)Phase::Lifting, (int)s.phase());
  TEST_ASSERT_TRUE(s.tentative());
  TEST_ASSERT_EQUAL_UINT16(0, s.takeCues());          // no tick for a tentative rep
  s.onEvent(ev(EventType::SetStart, 2), 4500, now);
  TEST_ASSERT_FALSE(s.tentative());
  TEST_ASSERT_TRUE(s.takeCues() & CueSetStart);
  s.onEvent(ev(EventType::Rep, 3), 7000, now);
  TEST_ASSERT_EQUAL_UINT8(3, s.reps());
  TEST_ASSERT_TRUE(s.takeCues() & CueRep);
  s.onEvent(ev(EventType::SetEnd, 3, Exercise::Curl, 2.5f, 6.2f), 11000, now);
  TEST_ASSERT_EQUAL_INT((int)Phase::Resting, (int)s.phase());
  TEST_ASSERT_TRUE(s.takeCues() & CueSetDone);
  TEST_ASSERT_TRUE(s.summaryVisible(11500));
  TEST_ASSERT_FALSE(s.summaryVisible(11000 + kSummaryMs + 10));
  TEST_ASSERT_TRUE(s.takeLogDirty());
  TEST_ASSERT_EQUAL_UINT16(1, s.setsToday());
  TEST_ASSERT_EQUAL_UINT16(3, s.repsToday());
  const SetRecord &r = s.log().sets[0];
  TEST_ASSERT_EQUAL_UINT8(3, r.reps);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Exercise::Curl, r.exercise);
  TEST_ASSERT_EQUAL_UINT16(18 * 60 + 30, r.startMin);
  TEST_ASSERT_EQUAL_UINT16(250, r.tempoCs);
  TEST_ASSERT_EQUAL_UINT16(6, r.durationS);
  TEST_ASSERT_EQUAL_UINT16(0xFFFF, r.restS);           // first set of the day
  TEST_ASSERT_EQUAL_UINT16(2, s.currentSetNo());

  // Rest timer, goal and "running long" nudges.
  s.tick(11000 + 89000);
  TEST_ASSERT_EQUAL_UINT32(89, s.restSec(11000 + 89000));
  TEST_ASSERT_EQUAL_UINT16(0, s.takeCues() & CueRestGoal);
  s.tick(11000 + 90000);
  TEST_ASSERT_TRUE(s.takeCues() & CueRestGoal);
  TEST_ASSERT_TRUE(s.restGoalReached());
  s.touch(11000 + 150000);
  s.tick(11000 + 180000);
  TEST_ASSERT_TRUE(s.takeCues() & CueRestLong);

  // Next set records the rest before it.
  s.onEvent(ev(EventType::RepTentative, 1), 11000 + 200000, now);
  s.onEvent(ev(EventType::SetStart, 2), 11000 + 202000, now);
  s.onEvent(ev(EventType::SetEnd, 2), 11000 + 206000, now);
  TEST_ASSERT_EQUAL_UINT16(200, s.log().sets[1].restS);
}

static void test_tick_setting_respected() {
  Settings st;
  st.repTick = 0;
  Session s = fresh(st);
  s.start(0);
  s.takeCues();
  Date now = day(2026, 10, 1);
  s.onEvent(ev(EventType::RepTentative, 1), 100, now);
  s.onEvent(ev(EventType::SetStart, 2), 200, now);
  s.onEvent(ev(EventType::Rep, 3), 300, now);
  uint16_t c = s.takeCues();
  TEST_ASSERT_EQUAL_UINT16(0, c & (CueRep | CueSetStart));
  s.onEvent(ev(EventType::SetEnd, 3), 400, now);
  TEST_ASSERT_TRUE(s.takeCues() & CueSetDone);          // set-done always plays
}

static void test_discarded_rep_leaves_no_trace() {
  Session s = fresh();
  s.start(0);
  Date now = day(2026, 10, 1);
  s.onEvent(ev(EventType::RepTentative, 1), 100, now);
  s.onEvent(ev(EventType::SetDiscarded, 1), 5000, now);
  TEST_ASSERT_EQUAL_INT((int)Phase::Ready, (int)s.phase());
  TEST_ASSERT_EQUAL_UINT16(0, s.setsToday());
}

static void test_pause_and_resume() {
  Session s = fresh();
  s.start(0);
  Date now = day(2026, 10, 1);
  s.onEvent(ev(EventType::RepTentative, 1), 100, now);
  s.onEvent(ev(EventType::SetStart, 2), 2000, now);
  // The view ends the detector's set before pausing; that SetEnd is logged.
  s.pause(3000);
  TEST_ASSERT_EQUAL_INT((int)Phase::Paused, (int)s.phase());
  TEST_ASSERT_FALSE(s.blocksSleep());
  s.onEvent(ev(EventType::SetEnd, 2), 3001, now);
  TEST_ASSERT_EQUAL_UINT16(1, s.setsToday());
  // Reps seen while paused don't count.
  s.onEvent(ev(EventType::RepTentative, 1), 4000, now);
  TEST_ASSERT_EQUAL_INT((int)Phase::Paused, (int)s.phase());
  s.resume(9000);
  TEST_ASSERT_EQUAL_INT((int)Phase::Resting, (int)s.phase());
  TEST_ASSERT_EQUAL_UINT32(6, s.restSec(9000 + 1000));   // rest began at 3001 ms
}

static void test_auto_pause_after_long_idle() {
  Session s = fresh();
  s.start(0);
  s.takeCues();
  s.tick(kAutoPauseMs - 1000);
  TEST_ASSERT_EQUAL_INT((int)Phase::Ready, (int)s.phase());
  s.tick(kAutoPauseMs + 1);
  TEST_ASSERT_EQUAL_INT((int)Phase::Paused, (int)s.phase());
  TEST_ASSERT_TRUE(s.autoPaused());
  TEST_ASSERT_EQUAL_UINT16(0, s.takeCues() & CuePause);    // silent auto-pause
}

static void test_day_rollover_and_week_history() {
  Session s = fresh();
  s.start(0);
  for (int d = 1; d <= 9; d++) {
    Date now = day(2026, 10, (uint8_t)d);
    s.setToday(now);
    s.onEvent(ev(EventType::RepTentative, 1), d * 100000, now);
    s.onEvent(ev(EventType::SetStart, 2), d * 100000 + 10, now);
    s.onEvent(ev(EventType::SetEnd, (uint8_t)(10 + d)), d * 100000 + 20, now);
  }
  TEST_ASSERT_EQUAL_UINT16(1, s.setsToday());
  TEST_ASSERT_EQUAL_UINT8(9, s.log().day);
  const History &h = s.history();
  TEST_ASSERT_EQUAL_UINT8(7, h.n);                       // capped at a week
  TEST_ASSERT_EQUAL_UINT8(8, h.days[0].day);             // newest first
  TEST_ASSERT_EQUAL_UINT16(18, h.days[0].reps);
  TEST_ASSERT_EQUAL_UINT8(2, h.days[6].day);
  TEST_ASSERT_TRUE(s.takeHistDirty());
  // An invalid clock never rolls the day.
  Date bad;
  s.setToday(bad);
  TEST_ASSERT_EQUAL_UINT8(9, s.log().day);
}

static void test_log_keeps_newest_sets_when_full() {
  Session s = fresh();
  s.start(0);
  Date now = day(2026, 10, 1);
  for (int i = 0; i < kMaxSets + 5; i++) {
    s.onEvent(ev(EventType::RepTentative, 1), i * 1000, now);
    s.onEvent(ev(EventType::SetStart, 2), i * 1000 + 10, now);
    s.onEvent(ev(EventType::SetEnd, (uint8_t)(2 + i % 50)), i * 1000 + 20, now);
  }
  TEST_ASSERT_EQUAL_UINT8(kMaxSets, s.log().count);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)(2 + (kMaxSets + 4) % 50), s.log().sets[kMaxSets - 1].reps);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)(2 + 5 % 50), s.log().sets[0].reps);
}

static void test_blobs_roundtrip_and_reject_garbage() {
  Session s = fresh();
  s.start(0);
  Date now = day(2026, 10, 1);
  s.onEvent(ev(EventType::RepTentative, 1), 0, now);
  s.onEvent(ev(EventType::SetStart, 2), 10, now);
  s.onEvent(ev(EventType::SetEnd, 12, Exercise::Press, 3.1f, 30.f), 20, now);
  uint8_t buf[kBlobHeader + sizeof(DayLog)];
  size_t n = packBlob(kBlobLog, &s.log(), sizeof(DayLog), buf, sizeof buf);
  TEST_ASSERT_EQUAL_UINT32(kBlobHeader + sizeof(DayLog), n);
  DayLog back;
  TEST_ASSERT_TRUE(unpackBlob(kBlobLog, buf, n, &back, sizeof back));
  TEST_ASSERT_EQUAL_MEMORY(&s.log(), &back, sizeof(DayLog));
  // Wrong kind, wrong length, wrong version: rejected.
  TEST_ASSERT_FALSE(unpackBlob(kBlobHist, buf, n, &back, sizeof back));
  TEST_ASSERT_FALSE(unpackBlob(kBlobLog, buf, n - 1, &back, sizeof back));
  buf[4] ^= 0xFF;
  TEST_ASSERT_FALSE(unpackBlob(kBlobLog, buf, n, &back, sizeof back));
  // Layout is part of the stored format.
  TEST_ASSERT_EQUAL_UINT32(12, sizeof(SetRecord));
  TEST_ASSERT_EQUAL_UINT32(8 + 12 * kMaxSets, sizeof(DayLog));
  TEST_ASSERT_EQUAL_UINT32(4 + 8 * kHistDays, sizeof(History));
  TEST_ASSERT_EQUAL_UINT32(8, sizeof(Settings));
}

static void test_settings_sanitize() {
  Settings st;
  st.mode = 77; st.wrist = 9; st.setEndSec = 0; st.repTick = 5; st.restGoalSec = 9999; st.handSign = 7;
  st.sanitize();
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Mode::Auto, st.mode);
  TEST_ASSERT_EQUAL_UINT8(0, st.wrist);
  TEST_ASSERT_EQUAL_UINT8(4, st.setEndSec);
  TEST_ASSERT_EQUAL_UINT8(1, st.repTick);
  TEST_ASSERT_EQUAL_UINT16(90, st.restGoalSec);
  TEST_ASSERT_EQUAL_INT8(0, st.handSign);
}

static void test_forget_today() {
  Session s = fresh();
  s.start(0);
  Date now = day(2026, 10, 1);
  s.onEvent(ev(EventType::RepTentative, 1), 0, now);
  s.onEvent(ev(EventType::SetStart, 2), 10, now);
  s.onEvent(ev(EventType::SetEnd, 5), 20, now);
  s.takeLogDirty();
  s.forgetToday();
  TEST_ASSERT_EQUAL_UINT16(0, s.setsToday());
  TEST_ASSERT_TRUE(s.takeLogDirty());
  TEST_ASSERT_EQUAL_UINT8(1, s.log().day);
}

static void test_haptic_sequencer() {
  HapticSeq h;
  static int calls = 0;
  static uint16_t lastMs = 0;
  calls = 0;
  auto fn = [](uint8_t, uint16_t ms) { calls++; lastMs = ms; };
  h.play(REPS_PATTERN(kPatSetDone, 3), 1000);
  TEST_ASSERT_TRUE(h.service(1000, fn) > 0);
  TEST_ASSERT_EQUAL_INT(1, calls);
  TEST_ASSERT_EQUAL_UINT32(0, h.service(1100, fn));      // still in the first pulse + gap
  h.service(1000 + 140 + 90, fn);
  TEST_ASSERT_EQUAL_INT(2, calls);
  // A low-priority tick can't interrupt the set-done pattern...
  h.play(REPS_PATTERN(kPatRep, 1), 1240);
  h.service(1000 + 140 + 90 + 60 + 90, fn);
  TEST_ASSERT_EQUAL_INT(3, calls);
  TEST_ASSERT_EQUAL_UINT16(320, lastMs);
  // ...but plays once it's done.
  h.play(REPS_PATTERN(kPatRep, 1), 3000);
  h.service(3000, fn);
  TEST_ASSERT_EQUAL_UINT16(22, lastMs);
}

static void test_detector_to_session_end_to_end() {
  // A synthetic workout through the real detector and the session.
  scen::Built b = scen::build("session", 2024);
  Detector d;
  d.reset(b.cfg);
  Session s = fresh();
  s.start(0);
  Date now = day(2026, 10, 1, 9, 0);
  size_t n = b.xyz.size() / 3;
  for (size_t i = 0; i < n + 1500; i++) {
    size_t k = i < n ? i : n - 1;
    d.push(b.xyz[3 * k], b.xyz[3 * k + 1], b.xyz[3 * k + 2]);
    Event e;
    uint32_t ms = (uint32_t)(i * 10);
    while (d.pollEvent(e)) s.onEvent(e, ms, now);
    s.touch(ms);
    s.tick(ms);
  }
  TEST_ASSERT_EQUAL_UINT16((uint16_t)b.truth.size(), s.setsToday());
  for (size_t i = 0; i < b.truth.size(); i++)
    TEST_ASSERT_INT_WITHIN(1, b.truth[i].reps, s.log().sets[i].reps);
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_full_set_cycle);
  RUN_TEST(test_tick_setting_respected);
  RUN_TEST(test_discarded_rep_leaves_no_trace);
  RUN_TEST(test_pause_and_resume);
  RUN_TEST(test_auto_pause_after_long_idle);
  RUN_TEST(test_day_rollover_and_week_history);
  RUN_TEST(test_log_keeps_newest_sets_when_full);
  RUN_TEST(test_blobs_roundtrip_and_reject_garbage);
  RUN_TEST(test_settings_sanitize);
  RUN_TEST(test_forget_today);
  RUN_TEST(test_haptic_sequencer);
  RUN_TEST(test_detector_to_session_end_to_end);
  return UNITY_END();
}
