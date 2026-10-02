// The collection: one record per day (date, final steps, algorithm version).
// Pure C++ (host-tested in test/test_dayrec); the watch keeps the file on
// LittleFS at /generative-face/days.bin (see daystore.cpp).
//
// File format v1, little-endian:
//   0  "DAYP"            magic
//   4  u8  version (1)
//   5  u8  record size (8)
//   6  u16 record count
//   8  u32 CRC-32 of the record bytes
//   12 records: u16 day (days since 2000-01-01), u8 algo, u8 flags, u32 steps
// Records are kept sorted by day, one per day.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace gf {

struct DayRecord {
  uint16_t day = 0;
  uint8_t  algo = 0;
  uint8_t  flags = 0;       // reserved (bit0: counted only while awake)
  uint32_t steps = 0;
};

static const int32_t  kDayLogMax = 1100;          // three years of days
static const uint16_t kFirstValidDay = 8766;      // 2024-01-01
static const size_t   kDayFileHeader = 12;

uint32_t crc32(const uint8_t *p, size_t n, uint32_t crc = 0);

class DayLog {
 public:
  void clear() { n_ = 0; }
  int32_t count() const { return n_; }
  const DayRecord &at(int32_t i) const { return recs_[i]; }
  int32_t find(uint16_t day) const;                 // index or -1
  // Insert, or merge into the existing record for that day (keeps the
  // larger step count, so re-living a day never loses a richer piece).
  // Drops the oldest record when full.
  void upsert(const DayRecord &r);
  // Index of the newest record on or before `day` (-1 if none).
  int32_t floorIndex(uint16_t day) const;

  size_t serializedSize() const { return kDayFileHeader + (size_t)n_ * 8; }
  size_t serialize(uint8_t *out, size_t cap) const;     // bytes written, 0 on error
  bool   deserialize(const uint8_t *in, size_t len);    // false = corrupt / unknown

  // Header and CRC check: true for a complete, untouched v1 file.
  static bool intact(const uint8_t *in, size_t len);
  // Recovery: merges (upserts) every plausible record of a v1 file into
  // this log without clearing it, tolerating a bad CRC, a damaged count and
  // a truncated tail. Files that aren't v1 day logs yield nothing. Returns
  // the number of records merged.
  int32_t merge(const uint8_t *in, size_t len);

 private:
  DayRecord recs_[kDayLogMax];
  int32_t   n_ = 0;
};

// Today's running count, kept in RTC memory and checkpointed to NVS.
struct LiveDay {
  uint16_t day = 0;
  uint32_t steps = 0;
};

// Called whenever the clock's date is known. If `today` differs from the
// live day, the live day is closed: returns true with `finished` filled in
// when it deserves a record, and resets `live` to today. Steps counted under
// an implausible date (an unset RTC) carry over into the real day instead
// of being filed under 2000-01-01.
bool rolloverDay(LiveDay &live, uint16_t today, uint8_t algo, DayRecord &finished);

}  // namespace gf
