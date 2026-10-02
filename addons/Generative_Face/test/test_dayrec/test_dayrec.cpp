// Day-record collection: ordering, merging, capacity, the file codec and
// the midnight rollover rules.
#include <string.h>
#include <vector>
#include <unity.h>
#include "dayrec.h"

using namespace gf;

static DayLog gLog;   // ~9 KB: keep it off the stack

static DayRecord rec(uint16_t day, uint32_t steps, uint8_t algo = 1) {
  DayRecord r;
  r.day = day; r.steps = steps; r.algo = algo;
  return r;
}

void test_upsert_keeps_days_sorted_and_unique() {
  gLog.clear();
  gLog.upsert(rec(9500, 100));
  gLog.upsert(rec(9400, 200));
  gLog.upsert(rec(9600, 300));
  gLog.upsert(rec(9450, 400));
  gLog.upsert(rec(9500, 50));          // smaller count for an existing day: ignored
  TEST_ASSERT_EQUAL_INT32(4, gLog.count());
  TEST_ASSERT_EQUAL_UINT16(9400, gLog.at(0).day);
  TEST_ASSERT_EQUAL_UINT16(9450, gLog.at(1).day);
  TEST_ASSERT_EQUAL_UINT16(9500, gLog.at(2).day);
  TEST_ASSERT_EQUAL_UINT16(9600, gLog.at(3).day);
  TEST_ASSERT_EQUAL_UINT32(100, gLog.at(2).steps);
  gLog.upsert(rec(9500, 8000));        // larger count replaces
  TEST_ASSERT_EQUAL_UINT32(8000, gLog.at(gLog.find(9500)).steps);
  TEST_ASSERT_EQUAL_INT32(-1, gLog.find(9401));
  TEST_ASSERT_EQUAL_INT32(1, gLog.floorIndex(9499));
  TEST_ASSERT_EQUAL_INT32(-1, gLog.floorIndex(9000));
}

void test_capacity_drops_oldest() {
  gLog.clear();
  for (int i = 0; i < kDayLogMax + 100; i++) gLog.upsert(rec((uint16_t)(9000 + i), (uint32_t)i));
  TEST_ASSERT_EQUAL_INT32(kDayLogMax, gLog.count());
  TEST_ASSERT_EQUAL_UINT16(9100, gLog.at(0).day);
  TEST_ASSERT_EQUAL_UINT16((uint16_t)(9000 + kDayLogMax + 99), gLog.at(kDayLogMax - 1).day);
  gLog.upsert(rec(8000, 1));           // older than anything kept: ignored when full
  TEST_ASSERT_EQUAL_UINT16(9100, gLog.at(0).day);
}

void test_file_round_trip() {
  gLog.clear();
  for (int i = 0; i < 400; i++) gLog.upsert(rec((uint16_t)(9200 + i * 2), (uint32_t)(i * 37 + 5), 1));
  std::vector<uint8_t> buf(gLog.serializedSize());
  TEST_ASSERT_EQUAL_UINT32(buf.size(), gLog.serialize(buf.data(), buf.size()));
  static DayLog other;
  TEST_ASSERT_TRUE(other.deserialize(buf.data(), buf.size()));
  TEST_ASSERT_EQUAL_INT32(gLog.count(), other.count());
  for (int32_t i = 0; i < gLog.count(); i++) {
    TEST_ASSERT_EQUAL_UINT16(gLog.at(i).day, other.at(i).day);
    TEST_ASSERT_EQUAL_UINT32(gLog.at(i).steps, other.at(i).steps);
    TEST_ASSERT_EQUAL_UINT8(gLog.at(i).algo, other.at(i).algo);
  }
  // Known header layout.
  TEST_ASSERT_EQUAL_MEMORY("DAYP", buf.data(), 4);
  TEST_ASSERT_EQUAL_UINT8(1, buf[4]);
  TEST_ASSERT_EQUAL_UINT8(8, buf[5]);
  TEST_ASSERT_EQUAL_UINT8(400 & 0xFF, buf[6]);
}

void test_corrupt_files_are_rejected() {
  gLog.clear();
  for (int i = 0; i < 10; i++) gLog.upsert(rec((uint16_t)(9300 + i), 1000));
  std::vector<uint8_t> buf(gLog.serializedSize());
  gLog.serialize(buf.data(), buf.size());
  static DayLog other;
  std::vector<uint8_t> bad = buf;
  bad[20] ^= 0x40;                                   // flipped bit in a record
  TEST_ASSERT_FALSE(other.deserialize(bad.data(), bad.size()));
  bad = buf; bad[0] = 'X';                           // wrong magic
  TEST_ASSERT_FALSE(other.deserialize(bad.data(), bad.size()));
  bad = buf; bad[4] = 2;                             // unknown version
  TEST_ASSERT_FALSE(other.deserialize(bad.data(), bad.size()));
  TEST_ASSERT_FALSE(other.deserialize(buf.data(), buf.size() - 1));   // truncated
  TEST_ASSERT_FALSE(other.deserialize(buf.data(), 5));
  TEST_ASSERT_TRUE(other.deserialize(buf.data(), buf.size()));
}

void test_damaged_files_are_salvaged_record_by_record() {
  gLog.clear();
  for (int i = 0; i < 300; i++) gLog.upsert(rec((uint16_t)(9300 + i), (uint32_t)(1000 + i)));
  std::vector<uint8_t> buf(gLog.serializedSize());
  gLog.serialize(buf.data(), buf.size());
  static DayLog out;

  // Intact file: every record merges, on top of what's already there.
  TEST_ASSERT_TRUE(DayLog::intact(buf.data(), buf.size()));
  out.clear();
  out.upsert(rec(9000, 5));
  TEST_ASSERT_EQUAL_INT32(300, out.merge(buf.data(), buf.size()));
  TEST_ASSERT_EQUAL_INT32(301, out.count());

  // A flipped bit in one record's step count: the CRC fails, all 300 come
  // back (one with a wrong count), still sorted and unique.
  std::vector<uint8_t> bad = buf;
  bad[12 + 8 * 150 + 5] ^= 0x01;
  TEST_ASSERT_FALSE(DayLog::intact(bad.data(), bad.size()));
  out.clear();
  TEST_ASSERT_EQUAL_INT32(300, out.merge(bad.data(), bad.size()));
  for (int32_t i = 1; i < out.count(); i++) TEST_ASSERT_TRUE(out.at(i - 1).day < out.at(i).day);

  // Truncated mid-record: the complete records before the cut survive.
  out.clear();
  TEST_ASSERT_EQUAL_INT32(100, out.merge(buf.data(), 12 + 8 * 100 + 5));
  TEST_ASSERT_EQUAL_UINT16(9300, out.at(0).day);
  TEST_ASSERT_EQUAL_UINT16(9399, out.at(99).day);

  // A damaged count is trusted only as far as the data goes.
  bad = buf; bad[6] = 0xFF; bad[7] = 0xFF;
  out.clear();
  TEST_ASSERT_EQUAL_INT32(300, out.merge(bad.data(), bad.size()));

  // Implausible records are skipped (a pre-2024 day, algo 0, a silly count).
  bad = buf;
  bad[12 + 0] = 0x10; bad[12 + 1] = 0x00;               // day 16 (2000-01-17)
  bad[12 + 8 + 2] = 0;                                  // algo 0
  bad[12 + 16 + 7] = 0x7F;                              // ~2 billion steps
  out.clear();
  TEST_ASSERT_EQUAL_INT32(297, out.merge(bad.data(), bad.size()));

  // Not a v1 day log at all: nothing is invented.
  bad = buf; bad[0] = 'X';
  TEST_ASSERT_EQUAL_INT32(0, out.merge(bad.data(), bad.size()));
  bad = buf; bad[4] = 2;
  TEST_ASSERT_EQUAL_INT32(0, out.merge(bad.data(), bad.size()));
  TEST_ASSERT_EQUAL_INT32(0, out.merge(buf.data(), 5));
  TEST_ASSERT_EQUAL_INT32(0, out.merge(nullptr, 0));
}

void test_crc32_reference_value() {
  // The classic check value for CRC-32 (IEEE) of "123456789".
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, crc32((const uint8_t *)"123456789", 9));
}

void test_rollover_at_midnight() {
  LiveDay live; live.day = 9770; live.steps = 8123;
  DayRecord done;
  TEST_ASSERT_FALSE(rolloverDay(live, 9770, 1, done));     // same day: nothing
  TEST_ASSERT_TRUE(rolloverDay(live, 9771, 1, done));
  TEST_ASSERT_EQUAL_UINT16(9770, done.day);
  TEST_ASSERT_EQUAL_UINT32(8123, done.steps);
  TEST_ASSERT_EQUAL_UINT8(1, done.algo);
  TEST_ASSERT_EQUAL_UINT16(9771, live.day);
  TEST_ASSERT_EQUAL_UINT32(0, live.steps);
}

void test_rollover_after_days_off_files_the_old_day() {
  LiveDay live; live.day = 9700; live.steps = 4000;
  DayRecord done;
  TEST_ASSERT_TRUE(rolloverDay(live, 9790, 1, done));
  TEST_ASSERT_EQUAL_UINT16(9700, done.day);
  TEST_ASSERT_EQUAL_UINT16(9790, live.day);
}

void test_rollover_with_unset_clock_keeps_counting() {
  LiveDay live; live.day = 0; live.steps = 120;            // RTC was at 2000-01-01
  DayRecord done;
  TEST_ASSERT_FALSE(rolloverDay(live, 1, 1, done));         // still unset
  TEST_ASSERT_FALSE(rolloverDay(live, 9800, 1, done));      // clock set: carry over
  TEST_ASSERT_EQUAL_UINT16(9800, live.day);
  TEST_ASSERT_EQUAL_UINT32(120, live.steps);
}

void test_rollover_backwards_carries_steps() {
  LiveDay live; live.day = 9900; live.steps = 300;          // a date set in the future
  DayRecord done;
  TEST_ASSERT_FALSE(rolloverDay(live, 9850, 1, done));      // corrected backwards
  TEST_ASSERT_EQUAL_UINT16(9850, live.day);
  TEST_ASSERT_EQUAL_UINT32(300, live.steps);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_upsert_keeps_days_sorted_and_unique);
  RUN_TEST(test_capacity_drops_oldest);
  RUN_TEST(test_file_round_trip);
  RUN_TEST(test_corrupt_files_are_rejected);
  RUN_TEST(test_damaged_files_are_salvaged_record_by_record);
  RUN_TEST(test_crc32_reference_value);
  RUN_TEST(test_rollover_at_midnight);
  RUN_TEST(test_rollover_after_days_off_files_the_old_day);
  RUN_TEST(test_rollover_with_unset_clock_keeps_counting);
  RUN_TEST(test_rollover_backwards_carries_steps);
  return UNITY_END();
}
