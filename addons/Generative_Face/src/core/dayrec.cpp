#include "dayrec.h"
#include <string.h>

namespace gf {

uint32_t crc32(const uint8_t *p, size_t n, uint32_t crc) {
  crc = ~crc;
  for (size_t i = 0; i < n; i++) {
    crc ^= p[i];
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

int32_t DayLog::find(uint16_t day) const {
  int32_t lo = 0, hi = n_ - 1;
  while (lo <= hi) {
    int32_t mid = (lo + hi) / 2;
    if (recs_[mid].day == day) return mid;
    if (recs_[mid].day < day) lo = mid + 1; else hi = mid - 1;
  }
  return -1;
}

int32_t DayLog::floorIndex(uint16_t day) const {
  int32_t lo = 0, hi = n_ - 1, best = -1;
  while (lo <= hi) {
    int32_t mid = (lo + hi) / 2;
    if (recs_[mid].day <= day) { best = mid; lo = mid + 1; } else { hi = mid - 1; }
  }
  return best;
}

void DayLog::upsert(const DayRecord &r) {
  int32_t i = find(r.day);
  if (i >= 0) {
    if (r.steps >= recs_[i].steps) recs_[i] = r;
    return;
  }
  // Insertion point.
  int32_t pos = floorIndex(r.day) + 1;
  if (n_ == kDayLogMax) {
    if (pos == 0) return;                       // older than everything kept
    memmove(&recs_[0], &recs_[1], sizeof(DayRecord) * (size_t)(pos - 1));
    recs_[pos - 1] = r;
    return;
  }
  memmove(&recs_[pos + 1], &recs_[pos], sizeof(DayRecord) * (size_t)(n_ - pos));
  recs_[pos] = r;
  n_++;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

size_t DayLog::serialize(uint8_t *out, size_t cap) const {
  size_t need = serializedSize();
  if (cap < need) return 0;
  uint8_t *r = out + kDayFileHeader;
  for (int32_t i = 0; i < n_; i++) {
    put16(r + i * 8, recs_[i].day);
    r[i * 8 + 2] = recs_[i].algo;
    r[i * 8 + 3] = recs_[i].flags;
    put32(r + i * 8 + 4, recs_[i].steps);
  }
  memcpy(out, "DAYP", 4);
  out[4] = 1;
  out[5] = 8;
  put16(out + 6, (uint16_t)n_);
  put32(out + 8, crc32(r, (size_t)n_ * 8));
  return need;
}

static bool v1Header(const uint8_t *in, size_t len) {
  return in && len >= kDayFileHeader && memcmp(in, "DAYP", 4) == 0 && in[4] == 1 && in[5] == 8;
}

bool DayLog::intact(const uint8_t *in, size_t len) {
  if (!v1Header(in, len)) return false;
  uint16_t count = get16(in + 6);
  if (count > kDayLogMax || len < kDayFileHeader + (size_t)count * 8) return false;
  return crc32(in + kDayFileHeader, (size_t)count * 8) == get32(in + 8);
}

int32_t DayLog::merge(const uint8_t *in, size_t len) {
  if (!v1Header(in, len)) return 0;
  // Trust the count only as far as the data goes.
  size_t avail = (len - kDayFileHeader) / 8;
  size_t count = get16(in + 6);
  if (count > avail) count = avail;
  if (count > (size_t)kDayLogMax) count = (size_t)kDayLogMax;
  const uint8_t *r = in + kDayFileHeader;
  int32_t merged = 0;
  for (size_t i = 0; i < count; i++) {
    DayRecord d;
    d.day = get16(r + i * 8);
    d.algo = r[i * 8 + 2];
    d.flags = r[i * 8 + 3];
    d.steps = get32(r + i * 8 + 4);
    // Plausible: 2024 to about 2120, a real algorithm number, a human day.
    bool ok = d.day >= kFirstValidDay && d.day < kFirstValidDay + 35000 &&
              d.algo >= 1 && d.algo <= 63 && d.steps <= 300000;
    if (!ok) continue;
    upsert(d);
    merged++;
  }
  return merged;
}

bool DayLog::deserialize(const uint8_t *in, size_t len) {
  if (!intact(in, len)) return false;
  // Rebuild through upsert so a hand-edited or reordered file still ends up
  // sorted and unique.
  n_ = 0;
  uint16_t count = get16(in + 6);
  const uint8_t *r = in + kDayFileHeader;
  for (uint16_t i = 0; i < count; i++) {
    DayRecord d;
    d.day = get16(r + i * 8);
    d.algo = r[i * 8 + 2];
    d.flags = r[i * 8 + 3];
    d.steps = get32(r + i * 8 + 4);
    upsert(d);
  }
  return true;
}

bool rolloverDay(LiveDay &live, uint16_t today, uint8_t algo, DayRecord &finished) {
  if (live.day == today) return false;
  bool liveValid = live.day >= kFirstValidDay && live.day <= today;
  bool todayValid = today >= kFirstValidDay;
  if (!todayValid) {
    // The clock itself is unset: keep counting under whatever we have.
    return false;
  }
  if (!liveValid) {
    // Steps were counted before the clock was set (or under a date in the
    // future that has now been corrected): they belong to today.
    live.day = today;
    return false;
  }
  finished.day = live.day;
  finished.algo = algo;
  finished.flags = 0;
  finished.steps = live.steps;
  live.day = today;
  live.steps = 0;
  return true;
}

}  // namespace gf
