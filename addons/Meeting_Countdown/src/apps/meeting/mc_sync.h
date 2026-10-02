// Meeting Countdown — calendar feed sync over WiFi.
//
// One engine, two ways in:
//   * startAsync(): while the watch is awake (Sync now, web page). Runs in its
//     own task; borrows the radio from wifi_svc with a client-mode lease and
//     gives it back afterwards (an AP session comes back by itself).
//   * runHeadless(): from setup() on a timer wake, before any UI task exists.
//     Owns the radio directly, the screen stays off, a hard time budget
//     applies, and pressing the button or touching the screen aborts it so
//     the normal UI can boot.
// Each feed is fetched over HTTPS (Mozilla root bundle from the ESP-IDF SDK)
// and streamed through the ICS parser: nothing is buffered whole.
#pragma once
#include <stdint.h>

namespace mcsync {

enum class Phase : uint8_t { Idle, Connecting, Clock, Fetching, Saving, Done };

struct Progress {
  bool     running = false;
  Phase    phase = Phase::Idle;
  uint8_t  feed = 0, feeds = 0;
  uint32_t bytes = 0;
  char     step[48] = "";
};

bool     startAsync();             // false if a sync is already running
bool     running();
Progress progress();

// Blocking; for the headless timer-wake path only. Returns false when the
// user interrupted it (the caller should then boot the UI).
bool     runHeadless(uint32_t budgetMs);

}  // namespace mcsync
