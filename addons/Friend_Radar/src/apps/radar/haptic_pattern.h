// Non-blocking haptic patterns on top of hapticBuzz().
//
// hapticBuzz() queues back-to-back pulses with no gaps (and a queue of 4), so
// rhythmic patterns are scheduled here with an esp_timer instead: each step
// fires hapticBuzz() at its own absolute time. Safe to call from any task.
// Starting a new pattern replaces the one in progress.
#pragma once
#include <stdint.h>

struct HapticStep {
  uint16_t atMs;       // offset from the pattern's t = 0
  uint8_t  intensity;  // 0..255 (scaled by the user's haptic strength)
  uint16_t durMs;
};

// Play `n` steps (n <= 8) with t = 0 at millis() value `t0Ms`. Steps already
// in the past are skipped, except that the last step always plays, so a late
// join still feels the pattern's accent.
void hapticPatternPlay(const HapticStep *steps, uint8_t n, uint32_t t0Ms);
void hapticPatternStop();

// The two Friend Radar patterns, timed to their animations.
extern const HapticStep kHapticMateNear[];      // soft tick, then da-da-DUM at the bloom
extern const uint8_t    kHapticMateNearN;
extern const HapticStep kHapticCelebrate[];     // rapid triple + long
extern const uint8_t    kHapticCelebrateN;
