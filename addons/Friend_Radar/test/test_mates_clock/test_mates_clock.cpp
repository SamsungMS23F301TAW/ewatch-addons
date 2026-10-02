// Host tests: mate book + persistence blob, encounter log, rendezvous clock,
// slot scheduling and calibration maths.
#include <unity.h>
#include <string.h>
#include "fr_mates.h"
#include "fr_clock.h"

using namespace fr;

void setUp() {}
void tearDown() {}

static const uint32_t T0 = 800000000u;

// ---------------------------------------------------------------------------
// MateBook
// ---------------------------------------------------------------------------
static void test_add_find_rename_remove() {
  MateBook mb;
  TEST_ASSERT_TRUE(mb.add(0x11, "  Ewan ", T0));
  TEST_ASSERT_EQUAL(MateBook::Structure, mb.dirty());
  TEST_ASSERT_EQUAL(1, mb.count());
  TEST_ASSERT_EQUAL_STRING("Ewan", mb.nickOf(0x11));
  TEST_ASSERT_TRUE(mb.add(0x11, "Other", T0));            // already a mate: no-op
  TEST_ASSERT_EQUAL(1, mb.count());
  TEST_ASSERT_TRUE(mb.rename(0x11, "Ewan W"));
  TEST_ASSERT_EQUAL_STRING("Ewan W", mb.at(0).nick);
  TEST_ASSERT_FALSE(mb.rename(0x11, "   "));              // empty nick refused
  TEST_ASSERT_FALSE(mb.rename(0x99, "x"));
  TEST_ASSERT_TRUE(mb.remove(0x11));
  TEST_ASSERT_FALSE(mb.remove(0x11));
  TEST_ASSERT_EQUAL(0, mb.count());
  TEST_ASSERT_NULL(mb.nickOf(0x11));
}

static void test_add_rejects_bad_ids_and_fills_default_nick() {
  MateBook mb;
  TEST_ASSERT_FALSE(mb.add(0, "x", T0));
  TEST_ASSERT_FALSE(mb.add(0xFFFFFFFF, "x", T0));
  TEST_ASSERT_TRUE(mb.add(0x12345678, "", T0));
  TEST_ASSERT_EQUAL_STRING("EWatch 5678", mb.at(0).nick);
}

static void test_capacity() {
  MateBook mb;
  for (int i = 0; i < kMaxMates; ++i) TEST_ASSERT_TRUE(mb.add(100 + i, "m", T0));
  TEST_ASSERT_FALSE(mb.add(999, "overflow", T0));
  TEST_ASSERT_EQUAL(kMaxMates, mb.count());
  TEST_ASSERT_TRUE(mb.remove(105));
  TEST_ASSERT_EQUAL(-1, mb.find(105));
  TEST_ASSERT_EQUAL(5, mb.find(106));                      // order kept
  TEST_ASSERT_TRUE(mb.add(999, "fits", T0));
}

static void test_encounter_log() {
  MateBook mb;
  mb.add(0x21, "Sam", T0);
  mb.clearDirty();
  mb.noteSeen(0x21, Zone::Lost, T0);                       // ignored
  TEST_ASSERT_EQUAL(0, mb.at(0).logN);
  mb.noteSeen(0x21, Zone::Around, T0 + 10);
  TEST_ASSERT_EQUAL(MateBook::Sightings, mb.dirty());
  mb.noteSeen(0x21, Zone::Near, T0 + 200);
  mb.noteSeen(0x21, Zone::Far, T0 + 400);                  // gaps < 5 min: same encounter
  const Mate &m = mb.at(0);
  TEST_ASSERT_EQUAL(1, m.logN);
  TEST_ASSERT_EQUAL_UINT32(T0 + 10, m.log[0].startSec);
  TEST_ASSERT_EQUAL_UINT16(6, m.log[0].minutes);           // 390 s
  TEST_ASSERT_EQUAL((int)Zone::Near, (int)m.log[0].closest);
  TEST_ASSERT_EQUAL((int)Zone::Far, (int)m.lastZone);
  // Back the next day: a new encounter, newest first.
  mb.noteSeen(0x21, Zone::RightHere, T0 + 86400);
  TEST_ASSERT_EQUAL(2, mb.at(0).logN);
  TEST_ASSERT_EQUAL((int)Zone::RightHere, (int)mb.at(0).log[0].closest);
  TEST_ASSERT_EQUAL_UINT32(T0 + 10, mb.at(0).log[1].startSec);
  // The log keeps only the newest kLogLen encounters.
  for (int i = 0; i < 10; ++i) mb.noteSeen(0x21, Zone::Around, T0 + 200000 + i * 1000);
  TEST_ASSERT_EQUAL(kLogLen, mb.at(0).logN);
  TEST_ASSERT_EQUAL_UINT32(T0 + 209000, mb.at(0).log[0].startSec);
  // Non-mates are not logged.
  mb.noteSeen(0x77, Zone::Near, T0);
  TEST_ASSERT_EQUAL(1, mb.count());
}

static void test_blob_roundtrip() {
  MateBook a;
  a.add(0xAAAA0001, "Ewan", T0);
  a.add(0xAAAA0002, "Kyle", T0 + 5);
  a.noteSeen(0xAAAA0001, Zone::Near, T0 + 100);
  a.noteSeen(0xAAAA0001, Zone::RightHere, T0 + 160);
  uint8_t blob[2048];
  size_t n = a.serialize(blob, sizeof(blob));
  TEST_ASSERT_TRUE(n > 0 && n <= MateBook::maxBlobSize());
  MateBook b;
  TEST_ASSERT_TRUE(b.deserialize(blob, n));
  TEST_ASSERT_EQUAL(MateBook::Clean, b.dirty());
  TEST_ASSERT_EQUAL(2, b.count());
  TEST_ASSERT_EQUAL_STRING("Ewan", b.at(0).nick);
  TEST_ASSERT_EQUAL_STRING("Kyle", b.at(1).nick);
  TEST_ASSERT_EQUAL_UINT32(T0 + 160, b.at(0).lastSeenSec);
  TEST_ASSERT_EQUAL(1, b.at(0).logN);
  TEST_ASSERT_EQUAL((int)Zone::RightHere, (int)b.at(0).log[0].closest);
  TEST_ASSERT_EQUAL_UINT16(1, b.at(0).log[0].minutes);
  TEST_ASSERT_EQUAL_UINT32(T0 + 5, b.at(1).addedSec);
}

static void test_blob_rejects_corruption() {
  MateBook a;
  a.add(0x31, "A", T0);
  uint8_t blob[2048];
  size_t n = a.serialize(blob, sizeof(blob));
  MateBook b;
  b.add(0x55, "keep", T0);
  TEST_ASSERT_FALSE(b.deserialize(blob, n - 1));          // wrong length
  uint8_t bad[2048];
  memcpy(bad, blob, n); bad[0] = 9;                        // unknown version
  TEST_ASSERT_FALSE(b.deserialize(bad, n));
  memcpy(bad, blob, n); bad[1] = 200;                      // absurd count
  TEST_ASSERT_FALSE(b.deserialize(bad, n));
  TEST_ASSERT_EQUAL(1, b.count());                         // untouched on failure
  TEST_ASSERT_EQUAL_STRING("keep", b.at(0).nick);
  // Records with a corrupt id are dropped, the rest kept.
  MateBook c;
  c.add(0x41, "One", T0);
  c.add(0x42, "Two", T0);
  n = c.serialize(blob, sizeof(blob));
  blob[2] = blob[3] = blob[4] = blob[5] = 0;               // first record id -> 0
  MateBook d;
  TEST_ASSERT_TRUE(d.deserialize(blob, n));
  TEST_ASSERT_EQUAL(1, d.count());
  TEST_ASSERT_EQUAL_STRING("Two", d.at(0).nick);
  TEST_ASSERT_FALSE(c.serialize(blob, 10) != 0);           // too small a buffer
}

static void test_empty_book_blob() {
  MateBook a, b;
  uint8_t blob[8];
  size_t n = a.serialize(blob, sizeof(blob));
  TEST_ASSERT_EQUAL_UINT32(2, n);
  b.add(0x9, "x", T0);
  TEST_ASSERT_TRUE(b.deserialize(blob, n));
  TEST_ASSERT_EQUAL(0, b.count());
}

// ---------------------------------------------------------------------------
// RtcPhase (sub-second RTC estimate)
// ---------------------------------------------------------------------------
static void test_rtc_phase_converges_from_rollovers() {
  // True RTC: second boundaries at local time 0.37 s + k (local clock in us).
  RtcPhase ph;
  const int64_t phaseUs = 370000;
  const uint32_t base = 1000;
  ph.observe(base, 100000);                     // a mid-second reading: coarse
  TEST_ASSERT_TRUE(ph.valid());
  // Rollovers detected by a reader polling every ~85 ms.
  for (int k = 1; k < 20; ++k) {
    int64_t boundary = phaseUs + (int64_t)k * 1000000;
    int64_t seenAt = boundary + (int64_t)((k * 37) % 85) * 1000;   // detected 0..85 ms late
    ph.observeRollover(base + (uint32_t)k, seenAt, 85000);
  }
  // Estimate RTC ms at local time 30.87 s: should be base*1000 + 30500 ms.
  int64_t probe = 30870000;
  int64_t truthMs = (int64_t)base * 1000 + (probe - phaseUs) / 1000;
  int64_t err = (int64_t)ph.rtcMs(probe) - truthMs;
  TEST_ASSERT_TRUE(err >= -90 && err <= 90);
}

static void test_rtc_phase_jump_recentres() {
  RtcPhase ph;
  ph.observe(5000, 0);
  ph.observe(9000, 1000000);                     // RTC set 4000 s ahead
  int64_t ms = (int64_t)ph.rtcMs(1000000);
  TEST_ASSERT_TRUE(ms >= 9000000 && ms < 9001000);
}

static void test_rtc_phase_tracks_slow_drift() {
  // The local (sleep) clock runs 0.5 % fast. Periodic whole-second readings
  // keep the estimate within the true second.
  RtcPhase ph;
  int64_t local = 0;
  uint32_t trueSecAtLocal0 = 2000;
  for (int i = 0; i < 200; ++i) {                // 200 wakes, 60 s apart
    double trueSec = trueSecAtLocal0 + (double)local / 1.005e6;
    ph.observe((uint32_t)trueSec, local);
    int64_t err = (int64_t)ph.rtcMs(local) - (int64_t)(trueSec * 1000.0);
    TEST_ASSERT_TRUE(err > -1000 && err < 1000);
    local += 60300000;                            // 60.3 s of fast local clock
  }
}

// ---------------------------------------------------------------------------
// Rendezvous slots
// ---------------------------------------------------------------------------
static void test_next_wake_lands_window_on_slot() {
  RendezvousConfig c;                            // 60 s, early 1500, boot 450
  const uint64_t lead = c.earlyMs + c.bootMs;
  for (uint64_t now = 600000000; now < 600000000 + 130000; now += 777) {
    uint32_t sleep = msUntilNextWake(now, c);
    uint64_t wake = now + sleep;
    TEST_ASSERT_TRUE(sleep >= c.minSleepMs);
    TEST_ASSERT_TRUE(sleep <= c.minSleepMs + 60000);
    TEST_ASSERT_EQUAL_UINT64(0, (wake + lead) % 60000);   // window opens 1.5 s before a slot
  }
}

static void test_two_watches_share_slots_with_any_period() {
  // Periods 30/60/120 s all divide 120, so a 120 s watch wakes on a subset of
  // a 30 s watch's slots.
  RendezvousConfig c30, c120;
  c30.periodSec = 30; c120.periodSec = 120;
  uint64_t now = 900000123;
  uint64_t w120 = now + msUntilNextWake(now, c120) + c120.earlyMs + c120.bootMs;
  TEST_ASSERT_EQUAL_UINT64(0, w120 % 30000);
}

static void test_clk_field_and_delta() {
  TEST_ASSERT_EQUAL_UINT8(0, clkField(120000));
  TEST_ASSERT_EQUAL_UINT8(119, clkField(119999));
  TEST_ASSERT_EQUAL(3, clkDelta(10, 13));
  TEST_ASSERT_EQUAL(-3, clkDelta(13, 10));
  TEST_ASSERT_EQUAL(2, clkDelta(119, 1));        // wraps
  TEST_ASSERT_EQUAL(-2, clkDelta(1, 119));
  TEST_ASSERT_EQUAL(-60, clkDelta(0, 60));
  TEST_ASSERT_TRUE(shouldAdoptClock(50, 10, true, 5));
  TEST_ASSERT_FALSE(shouldAdoptClock(10, 50, true, 5));   // they follow us
  TEST_ASSERT_FALSE(shouldAdoptClock(50, 10, false, 5));  // strangers ignored
  TEST_ASSERT_FALSE(shouldAdoptClock(50, 10, true, 1));   // within quantisation
}

// ---------------------------------------------------------------------------
// Calibration
// ---------------------------------------------------------------------------
static void test_calibration_median_and_validation() {
  int8_t s[40];
  for (int i = 0; i < 40; ++i) s[i] = (int8_t)(-62 + (i % 5) - 2);   // -64..-60
  s[7] = -95; s[21] = -30;                                            // outliers
  CalResult r = calibrateFromSamples(s, 40);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_INT8(-62, r.ref1m);
  TEST_ASSERT_TRUE(r.spreadDb <= 4);

  CalResult few = calibrateFromSamples(s, 10);
  TEST_ASSERT_FALSE(few.ok);
  TEST_ASSERT_EQUAL(CalResult::TooFew, few.reason);

  int8_t noisy[40];
  for (int i = 0; i < 40; ++i) noisy[i] = (int8_t)((i & 1) ? -45 : -80);
  CalResult nr = calibrateFromSamples(noisy, 40);
  TEST_ASSERT_FALSE(nr.ok);
  TEST_ASSERT_EQUAL(CalResult::TooNoisy, nr.reason);

  int8_t weak[20];
  for (int i = 0; i < 20; ++i) weak[i] = -100;
  CalResult wr = calibrateFromSamples(weak, 20);
  TEST_ASSERT_EQUAL(CalResult::OutOfRange, wr.reason);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_add_find_rename_remove);
  RUN_TEST(test_add_rejects_bad_ids_and_fills_default_nick);
  RUN_TEST(test_capacity);
  RUN_TEST(test_encounter_log);
  RUN_TEST(test_blob_roundtrip);
  RUN_TEST(test_blob_rejects_corruption);
  RUN_TEST(test_empty_book_blob);
  RUN_TEST(test_rtc_phase_converges_from_rollovers);
  RUN_TEST(test_rtc_phase_jump_recentres);
  RUN_TEST(test_rtc_phase_tracks_slow_drift);
  RUN_TEST(test_next_wake_lands_window_on_slot);
  RUN_TEST(test_two_watches_share_slots_with_any_period);
  RUN_TEST(test_clk_field_and_delta);
  RUN_TEST(test_calibration_median_and_validation);
  return UNITY_END();
}
