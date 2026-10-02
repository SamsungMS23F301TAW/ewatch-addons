// Meeting Countdown — argument parsing shared by the USB serial protocol and
// the web form, so both build events exactly the same way. Pure C++.
//
//   EVENT <start> <end> [leave=<min>] [loc=<word>|loc="<words>"] <title...>
//
// <start>/<end> are ISO-8601 (see parseIso8601): local times use the watch's
// UTC offset, "Z"/"+01:00" suffixes are honoured. <end> may also be a
// duration such as +30m, 1h30m or PT45M. Two date-only values make an all-day
// event ("EVENT 2026-10-02 2026-10-03 Offsite").
#pragma once
#include <stdint.h>
#include "mc_event.h"

namespace mc {

bool parseEventArgs(const char *args, int tzOffsetMin, Source src, Event &out,
                    const char **err);

// Fill uid/key/flags for a hand-made event. `salt` keeps two otherwise
// identical manual entries distinct.
void finishManualEvent(Event &e, Source src, uint32_t salt);

// Builds an event from separate fields (web form). Times are local wall
// values ("2026-10-01", "09:30"); endTime may be empty when durMin > 0.
bool makeEvent(const char *title, const char *date, const char *startTime,
               const char *endTime, int durMin, int leaveMin, const char *location,
               int tzOffsetMin, Source src, uint32_t salt, Event &out, const char **err);

}  // namespace mc
