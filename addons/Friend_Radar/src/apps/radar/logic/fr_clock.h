// Time helpers for background rendezvous, plus distance calibration maths.
//
// Background mate alerts wake each watch for a short radio window at the same
// wall-clock instants ("slots"), so two sleeping watches listen at the same
// time. Slots are multiples of the period in *radar time*:
//     radar time = RTC local time + a per-watch offset (whole seconds)
// The offset lets mates whose RTCs disagree converge on one schedule: each
// watch follows any mate with a lower id (see shouldAdoptClock), so a group
// settles on the clock of its lowest-id member.
//
// The RV-3028 only exposes whole seconds, so RtcPhase keeps a sub-second
// estimate of RTC time against a free-running microsecond clock that survives
// deep sleep (gettimeofday on the watch). Each RTC reading bounds the true
// time to [S, S+1); a reading taken right after a second rollover bounds it
// much tighter. The estimate is clamped into every new bound, which also
// absorbs slow drift of the sleep clock.
#pragma once
#include "fr_types.h"

namespace fr {

class RtcPhase {
public:
  void reset() { valid_ = false; offsetUs_ = 0; }
  bool valid() const { return valid_; }

  // The RTC read `rtcSec` at local time `localUs` (any time within that second).
  void observe(uint32_t rtcSec, int64_t localUs) { bound(rtcSec, localUs, 1000000); }
  // The RTC was seen to roll over to `rtcSec` at most `lagUs` before `localUs`.
  void observeRollover(uint32_t rtcSec, int64_t localUs, int64_t lagUs) { bound(rtcSec, localUs, lagUs); }

  // Estimated RTC time in milliseconds at `localUs` (0 if not valid).
  uint64_t rtcMs(int64_t localUs) const;

  int64_t offsetUs() const { return offsetUs_; }
  void restore(int64_t offsetUs) { offsetUs_ = offsetUs; valid_ = true; }

private:
  void bound(uint32_t rtcSec, int64_t localUs, int64_t widthUs);
  bool    valid_ = false;
  int64_t offsetUs_ = 0;   // rtcUs ~= localUs + offsetUs_
};

struct RendezvousConfig {
  uint16_t periodSec = 60;    // one window per period
  uint16_t windowMs  = 3500;  // radio on for this long
  uint16_t earlyMs   = 1500;  // window opens this long before the slot boundary
  uint16_t bootMs    = 450;   // timer wake -> radio on (bootloader + init + BLE)
  uint16_t minSleepMs = 2000; // never schedule a wake sooner than this
};

// Milliseconds to sleep from radar time `radarNowMs` so the radio window of
// the next reachable slot opens on time.
uint32_t msUntilNextWake(uint64_t radarNowMs, const RendezvousConfig &c);

// Radar clock field for the beacon: whole radar seconds mod 120.
static inline uint8_t clkField(uint64_t radarMs) { return (uint8_t)((radarMs / 1000u) % 120u); }

// Signed difference theirs - mine, wrapped into [-60, 59] seconds.
int clkDelta(uint8_t mine, uint8_t theirs);

// Follow a mate's radar clock if they have a lower id and we differ by 2 s or more.
static inline bool shouldAdoptClock(uint32_t myId, uint32_t theirId, bool isMate, int delta) {
  return isMate && theirId < myId && (delta >= 2 || delta <= -2);
}

// ---- calibration -----------------------------------------------------------
struct CalResult {
  bool    ok = false;
  int8_t  ref1m = 0;      // median RSSI at 1 m
  uint8_t spreadDb = 0;   // interquartile range of the samples
  uint16_t samples = 0;
  enum Reason : uint8_t { Ok, TooFew, TooNoisy, OutOfRange } reason = TooFew;
};
constexpr uint16_t kCalMinSamples = 15;
constexpr uint8_t  kCalMaxIqrDb   = 14;

// Robust 1 m reference from raw RSSI samples collected at 1 m.
CalResult calibrateFromSamples(const int8_t *samples, uint16_t n);

}  // namespace fr
