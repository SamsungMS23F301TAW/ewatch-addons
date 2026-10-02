#include "fr_clock.h"
#include "fr_beacon.h"

namespace fr {

void RtcPhase::bound(uint32_t rtcSec, int64_t localUs, int64_t widthUs) {
  if (widthUs < 1) widthUs = 1;
  if (widthUs > 1000000) widthUs = 1000000;
  const int64_t lo = (int64_t)rtcSec * 1000000 - localUs;    // offset >= lo
  const int64_t hi = lo + widthUs - 1;                        // offset <= hi
  if (!valid_) {
    offsetUs_ = lo + widthUs / 2;
    valid_ = true;
    return;
  }
  // A jump of more than a couple of seconds means the RTC or the local clock
  // was set: start again from the middle of the new bound.
  if (offsetUs_ < lo - 2000000 || offsetUs_ > hi + 2000000) {
    offsetUs_ = lo + widthUs / 2;
    return;
  }
  if (offsetUs_ < lo) offsetUs_ = lo;
  else if (offsetUs_ > hi) offsetUs_ = hi;
}

uint64_t RtcPhase::rtcMs(int64_t localUs) const {
  if (!valid_) return 0;
  int64_t us = localUs + offsetUs_;
  return us > 0 ? (uint64_t)(us / 1000) : 0;
}

uint32_t msUntilNextWake(uint64_t now, const RendezvousConfig &c) {
  const uint64_t period = (uint64_t)(c.periodSec ? c.periodSec : 60) * 1000u;
  const uint64_t lead = (uint64_t)c.earlyMs + c.bootMs;
  // First slot boundary b with (b - lead) >= now + minSleep, i.e. the radio
  // window for b can still open on time if we wake at b - lead.
  const uint64_t x = now + c.minSleepMs + lead;
  const uint64_t b = ((x + period - 1) / period) * period;
  return (uint32_t)(b - lead - now);
}

int clkDelta(uint8_t mine, uint8_t theirs) {
  int d = ((int)theirs - (int)mine) % 120;
  if (d < -60) d += 120;
  if (d >= 60) d -= 120;
  return d;
}

CalResult calibrateFromSamples(const int8_t *samples, uint16_t n) {
  CalResult r;
  r.samples = n;
  if (!samples || n < kCalMinSamples) { r.reason = CalResult::TooFew; return r; }
  // Sort a bounded copy (n is at most a few hundred).
  static const uint16_t kMax = 256;
  int8_t s[kMax];
  uint16_t m = n > kMax ? kMax : n;
  for (uint16_t i = 0; i < m; ++i) s[i] = samples[n - m + i];   // keep the newest
  for (uint16_t i = 1; i < m; ++i) {
    int8_t v = s[i];
    int j = (int)i - 1;
    while (j >= 0 && s[j] > v) { s[j + 1] = s[j]; --j; }
    s[j + 1] = v;
  }
  int med;
  if (m & 1) {
    med = s[m / 2];
  } else {
    int sum = s[m / 2 - 1] + s[m / 2];
    med = (sum - 1) / 2;          // floor of the mean for negative dBm values
  }
  int iqr = s[(3 * m) / 4] - s[m / 4];
  r.ref1m = (int8_t)med;
  r.spreadDb = (uint8_t)(iqr < 0 ? 0 : iqr);
  if (med < kMinRef1m || med > kMaxRef1m) { r.reason = CalResult::OutOfRange; return r; }
  if (iqr > kCalMaxIqrDb) { r.reason = CalResult::TooNoisy; return r; }
  r.ok = true;
  r.reason = CalResult::Ok;
  return r;
}

}  // namespace fr
