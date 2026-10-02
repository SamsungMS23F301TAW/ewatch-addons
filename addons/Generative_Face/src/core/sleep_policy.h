// The screen-off decision logic of power_bg, free of hardware so it can be
// simulated on the host (test/test_sleep). power_bg.cpp owns the hardware:
// it asks plan() what to arm and how long to sleep, sleeps, then reports
// what woke it to onWake(), which says whether to keep sleeping, hand the
// watch back to the user, or drop to deep sleep.
#pragma once
#include <stdint.h>

namespace gf {

enum class WakeCause : uint8_t { None, Button, Touch, Fifo, Motion, Timer };

struct SleepPolicy {
  // Tuning (milliseconds).
  uint32_t movingHoldMs  = 90UL * 1000;     // tier 1 hold after the screen goes dark
  uint32_t joltHoldMs    = 25UL * 1000;     // tier 1 hold after a motion wake
  uint32_t stillToDeepMs = 15UL * 60 * 1000;
  uint32_t housekeepMs   = 30UL * 1000;
  uint32_t maxSleepMs    = 15UL * 1000;     // stays inside the 20 s watchdog
  uint32_t motionStuckMax = 3;              // stuck motion wakes before INT1 is muted

  // What to arm for the next sleep.
  bool fifoArmed = false, motionArmed = false, touchArmed = true;

  enum Plan : uint8_t { kSleep, kDeepSleep };
  enum Outcome : uint8_t { kContinue, kUserWake, kDeepSleepNow };

  void begin(uint32_t now, bool accelPresent, uint32_t steps);
  // Top of each loop: housekeeping due? tier changes; how long to sleep.
  bool housekeepingDue(uint32_t now) const;
  void housekeepingDone(uint32_t now) { lastHouse_ = now; houseOnce_ = true; }
  Plan plan(uint32_t now, uint32_t steps, uint32_t &horizonMs);
  // After a wake. drainOk is the FIFO read result when the FIFO was armed;
  // touchReal says whether a touch wake carried a real finger or gesture;
  // motionStuck says the motion line was still high right after its latch
  // was cleared (a stuck line, not a fresh movement).
  Outcome onWake(uint32_t now, WakeCause w, bool fifoWasArmed, bool drainOk, bool touchReal,
                 bool motionStuck = false);

  // Diagnostics.
  uint32_t storms = 0, stillEntries = 0, spuriousTouch = 0, spuriousNone = 0;
  bool motionMuted() const { return motionMuted_; }

 private:
  bool     accel_ = false;
  uint32_t tier1Since_ = 0, tier1Hold_ = 0;
  uint32_t lastStepMs_ = 0, stillSince_ = 0, lastSteps_ = 0;
  uint32_t lastHouse_ = 0;
  bool     houseOnce_ = false;
  uint32_t stormWindow_ = 0, stormFifo_ = 0, stormNone_ = 0, badDrains_ = 0;
  uint32_t touchWindow_ = 0, touchStorm_ = 0, touchMutedUntil_ = 0;
  bool     touchMuted_ = false;
  uint32_t motionStuck_ = 0;                // consecutive stuck motion wakes
  bool     motionMuted_ = false;            // INT1 not armed for this screen-off
  bool     stuckSpell_ = false;             // this tier-1 spell began with a stuck wake
};

}  // namespace gf
