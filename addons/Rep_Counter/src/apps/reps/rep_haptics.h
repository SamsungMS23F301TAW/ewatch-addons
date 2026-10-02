// Rep Counter: haptic patterns and a tiny non-blocking sequencer.
//
// BaseOS's hapticBuzz(intensity, ms) is fire-and-forget with a 4-deep queue
// and no notion of gaps, so patterns are played by calling service() every
// frame; it issues one buzz per pulse when that pulse is due. Pure C++: the
// view passes a function that calls hapticBuzz. (Timing granularity is the
// render loop, ~20 ms, which is plenty for these.)
#pragma once
#include <stdint.h>

namespace reps {

struct Pulse { uint8_t intensity; uint16_t onMs; uint16_t gapMs; };

// Light, short: one per counted rep. Felt, but easy to ignore mid-set.
static const Pulse kPatRep[] = { {110, 22, 0} };
// Set confirmed: the first two reps land together, so two quick ticks.
static const Pulse kPatSetStart[] = { {110, 22, 90}, {110, 22, 0} };
// Set done: long-short-long, unmistakable.
static const Pulse kPatSetDone[] = { {230, 140, 90}, {170, 60, 90}, {255, 320, 0} };
// Rest target reached: two firm, even pulses.
static const Pulse kPatRestGoal[] = { {200, 90, 140}, {200, 90, 0} };
// Rest is running long: three softer pulses.
static const Pulse kPatRestLong[] = { {150, 60, 120}, {150, 60, 120}, {150, 60, 0} };
// Start / resume: rising. Pause: falling.
static const Pulse kPatStart[] = { {110, 50, 70}, {210, 80, 0} };
static const Pulse kPatPause[] = { {210, 80, 70}, {110, 50, 0} };

struct Pattern {
  const Pulse *p;
  uint8_t n;
  uint8_t priority;     // a busy sequencer only takes equal or higher priority
};

#define REPS_PATTERN(arr, prio) \
  ::reps::Pattern{arr, (uint8_t)(sizeof(arr) / sizeof(arr[0])), (uint8_t)(prio)}

class HapticSeq {
public:
  typedef void (*BuzzFn)(uint8_t intensity, uint16_t ms);
  void play(const Pattern &pat, uint32_t now) {
    if (busy(now) && pat.priority < cur_.priority) return;
    cur_ = pat;
    idx_ = 0;
    nextAt_ = now;
    active_ = true;
  }
  // Returns the time (ms) the motor will be running until, for callers that
  // want to ignore the accelerometer while it buzzes.
  uint32_t service(uint32_t now, BuzzFn fn) {
    if (!active_) return 0;
    if ((int32_t)(now - nextAt_) < 0) return 0;
    const Pulse &p = cur_.p[idx_];
    if (fn) fn(p.intensity, p.onMs);
    uint32_t until = now + p.onMs;
    nextAt_ = now + p.onMs + p.gapMs;
    if (++idx_ >= cur_.n) active_ = false;
    return until;
  }
  bool busy(uint32_t now) const { return active_ || (int32_t)(now - nextAt_) < 0; }
  void stop() { active_ = false; }
private:
  Pattern cur_ = {nullptr, 0, 0};
  uint8_t idx_ = 0;
  uint32_t nextAt_ = 0;
  bool active_ = false;
};

}  // namespace reps
