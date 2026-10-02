// Meeting Countdown — non-blocking vibration patterns on top of hapticBuzz().
// An esp_timer steps through the pattern, so callers never wait and the
// pattern keeps its rhythm whatever the render loop is doing.
#pragma once
#include <stdint.h>

namespace mchaptic {

enum class Pattern : uint8_t {
  Alert,   // "knock-knock-knock, hum": a meeting is about to start
  Leave,   // two long pulses: time to leave
  Start,   // long + two short: starting now
};

void play(Pattern p, int repeats = 3, uint16_t gapMs = 2600);
void stop();
bool playing();

}  // namespace mchaptic
