#include "sleep_policy.h"

namespace gf {

void SleepPolicy::begin(uint32_t now, bool accelPresent, uint32_t steps) {
  accel_ = accelPresent;
  fifoArmed = accelPresent;
  motionArmed = false;
  touchArmed = true;
  touchMuted_ = false;
  tier1Since_ = now;
  tier1Hold_ = movingHoldMs;
  lastStepMs_ = now;
  stillSince_ = now;
  lastSteps_ = steps;
  lastHouse_ = now;
  houseOnce_ = false;
  stormWindow_ = now;
  stormFifo_ = stormNone_ = badDrains_ = 0;
  touchWindow_ = now;
  touchStorm_ = 0;
  motionStuck_ = 0;
  motionMuted_ = false;
  stuckSpell_ = false;
}

bool SleepPolicy::housekeepingDue(uint32_t now) const {
  return !houseOnce_ || now - lastHouse_ >= housekeepMs;
}

SleepPolicy::Plan SleepPolicy::plan(uint32_t now, uint32_t steps, uint32_t &horizonMs) {
  if (steps != lastSteps_) { lastSteps_ = steps; lastStepMs_ = now; motionStuck_ = 0; }
  uint32_t calmSince = lastStepMs_ > tier1Since_ ? lastStepMs_ : tier1Since_;
  if (fifoArmed && now - calmSince > tier1Hold_) {
    fifoArmed = false;                       // tier 2: wait for motion
    motionArmed = accel_ && badDrains_ <= 20 && !motionMuted_;
    // The stillness clock restarts here, except after a tier-1 spell that a
    // stuck motion line started and that found no steps: otherwise such a
    // line would hold off deep sleep forever.
    bool idleStuckSpell = stuckSpell_ && lastStepMs_ <= tier1Since_;
    if (!idleStuckSpell) stillSince_ = now;
    stuckSpell_ = false;
    stillEntries++;
  }
  if (!fifoArmed && now - stillSince_ > stillToDeepMs) return kDeepSleep;
  if (touchMuted_ && (int32_t)(now - touchMutedUntil_) >= 0) {
    touchMuted_ = false;
    touchArmed = true;
  }
  uint32_t h = maxSleepMs;
  uint32_t sinceHouse = now - lastHouse_;
  uint32_t untilHouse = sinceHouse >= housekeepMs ? 0 : housekeepMs - sinceHouse;
  if (untilHouse < h) h = untilHouse;
  if (fifoArmed) {
    uint32_t held = now - calmSince;
    uint32_t left = held >= tier1Hold_ ? 0 : tier1Hold_ - held;
    if (left < h) h = left + 1;
  } else {
    uint32_t still = now - stillSince_;
    uint32_t left = still >= stillToDeepMs ? 0 : stillToDeepMs - still;
    if (left < h) h = left + 1;                // wake exactly at the deadline
  }
  if (h < 20) h = 20;
  horizonMs = h;
  return kSleep;
}

SleepPolicy::Outcome SleepPolicy::onWake(uint32_t now, WakeCause w, bool fifoWasArmed,
                                         bool drainOk, bool touchReal, bool motionStuck) {
  if (fifoWasArmed) {
    badDrains_ = drainOk ? 0 : badDrains_ + 1;
    if (badDrains_ > 20) {
      // The accelerometer stopped answering: don't trust INT2 (it may be
      // stuck high); fall back to timer housekeeping and the button.
      fifoArmed = false;
      motionArmed = false;
      stillSince_ = now;
      storms++;
    }
  }
  if (now - stormWindow_ > 1000) { stormWindow_ = now; stormFifo_ = 0; stormNone_ = 0; }
  if (w == WakeCause::Fifo && ++stormFifo_ > 12) {
    fifoArmed = false;                        // INT2 is chattering
    motionArmed = accel_ && !motionMuted_;
    stillSince_ = now;
    storms++;
  }
  switch (w) {
    case WakeCause::Button:
      return kUserWake;                       // always honoured
    case WakeCause::Touch:
      if (touchReal) return kUserWake;
      spuriousTouch++;
      if (now - touchWindow_ > 10000) { touchWindow_ = now; touchStorm_ = 0; }
      if (++touchStorm_ > 15) {
        touchArmed = false;                   // EMI or a stuck line: mute a minute
        touchMuted_ = true;
        touchMutedUntil_ = now + 60000;
        storms++;
      }
      return kContinue;
    case WakeCause::Motion:
      if (motionStuck) {
        // INT1 stayed high after its latch was read: a stuck line, which
        // would wake us at once every time. It doesn't restart the
        // stillness clock, and a few in a row (with no steps between) mute
        // it for the rest of this screen-off. Deep sleep re-arms it only if
        // the line reads low.
        if (++motionStuck_ >= motionStuckMax && !motionMuted_) {
          motionMuted_ = true;
          storms++;
        }
      } else {
        motionStuck_ = 0;
        stillSince_ = now;
      }
      if (accel_ && badDrains_ <= 20) {
        fifoArmed = true;                     // back to tier 1, briefly
        motionArmed = false;
        tier1Since_ = now;
        tier1Hold_ = joltHoldMs;
        stuckSpell_ = motionStuck;
      }
      return kContinue;
    case WakeCause::Fifo:
    case WakeCause::Timer:
      return kContinue;
    case WakeCause::None:
    default:
      spuriousNone++;
      if (++stormNone_ > 40) { storms++; return kDeepSleepNow; }
      return kContinue;
  }
}

}  // namespace gf
