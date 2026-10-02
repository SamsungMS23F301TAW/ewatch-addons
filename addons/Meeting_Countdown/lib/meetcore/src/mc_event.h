// Meeting Countdown — the event record shared by every source (calendar feed,
// manual web entry, USB push) and by the NVS store. Pure C++.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace mc {

constexpr int kTitleMax = 48;          // bytes incl. NUL (ASCII-folded)
constexpr int kLocMax   = 32;

enum : uint8_t {
  EF_ALLDAY    = 0x01,   // start/end are wall-clock midnights, not instants
  EF_RECURRING = 0x02,   // generated from an RRULE/RDATE series
  EF_OVERRIDE  = 0x04,   // a VEVENT that carried RECURRENCE-ID
  EF_TENTATIVE = 0x08,   // STATUS:TENTATIVE
};

enum class Source : uint8_t { Feed = 0, Manual = 1, Pushed = 2 };

struct Event {
  int64_t  start;        // UTC instant (timed) or wall-clock midnight (all-day)
  int64_t  end;          // exclusive
  uint32_t uid;          // FNV-1a of the iCalendar UID (or a synthetic id)
  uint32_t key;          // instance key = hash(uid, original start)
  uint8_t  flags;        // EF_*
  uint8_t  source;       // Source
  uint8_t  leaveMin;     // "leave now" buffer in minutes, 0 = none
  uint8_t  feed;         // feed slot (0..2) for Source::Feed
  char     title[kTitleMax];
  char     location[kLocMax];
};

inline bool isAllDay(const Event &e) { return (e.flags & EF_ALLDAY) != 0; }

// Instance key: stable across syncs for the same occurrence of the same
// series, so snoozes and alert bookkeeping survive a re-sync.
uint32_t instanceKey(uint32_t uid, int64_t originalStart);

// Sort by start, then end, then title (deterministic order everywhere).
bool eventLess(const Event &a, const Event &b);
void sortEvents(Event *ev, int n);

}  // namespace mc
