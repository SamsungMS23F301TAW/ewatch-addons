// Meeting Countdown — streaming iCalendar (RFC 5545) parser.
//
// Feeds of several megabytes are normal (Google's secret address exports the
// whole history), so nothing is ever buffered whole: bytes go through a
// one-character-lookahead line unfolder, a property lexer that keeps only the
// properties we use (huge DESCRIPTION lines are dropped as they stream), and
// a component state machine. Only events overlapping a small window
// [windowStart, windowEnd) are kept, capped and sorted.
//
// Supported:
//   * line folding (CRLF / LF / CR, space or tab continuation), UTF-8 BOM
//   * DTSTART/DTEND/DURATION in UTC ("Z"), floating, TZID=... and VALUE=DATE
//   * VTIMEZONE blocks (STANDARD/DAYLIGHT with yearly RRULE or RDATE), so
//     TZID times convert with real DST rules; unknown TZIDs and floating
//     times fall back to the watch's fixed offset
//   * all-day events (kept, flagged EF_ALLDAY, never alerted)
//   * RRULE FREQ=DAILY/WEEKLY/MONTHLY/YEARLY with INTERVAL, COUNT, UNTIL,
//     BYDAY (with ordinals for monthly/yearly), BYMONTHDAY, BYMONTH,
//     BYSETPOS, WKST; RDATE; EXDATE (date-time and date forms)
//   * RECURRENCE-ID overrides (moved/cancelled single instances)
//   * STATUS:CANCELLED, X-APPLE-TRAVEL-DURATION (-> leave-now buffer),
//     X-MICROSOFT-CDO-ALLDAYEVENT, X-WR-CALNAME, X-WR-TIMEZONE
// Not supported (documented): BYWEEKNO/BYYEARDAY/BYHOUR..., HOURLY or finer
// frequencies (such a series shows only its first instance), RANGE=
// THISANDFUTURE overrides (treated as single-instance), VTIMEZONEs that
// appear after the events that use them.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "mc_event.h"
#include "mc_time.h"

namespace mc {

struct IcsOptions {
  int64_t windowStart = 0;         // UTC: keep events ending after this...
  int64_t windowEnd   = 0;         // ...and starting before this
  int     fallbackOffsetMin = 0;   // floating times and unknown TZIDs
  uint8_t source    = (uint8_t)Source::Feed;
  uint8_t feedIndex = 0;
  int     maxEvents = 16;          // final cap (<= IcsParser::kMaxOut)
  int     maxAllDay = 4;           // of which at most this many all-day
};

struct IcsStats {
  uint32_t bytes = 0, lines = 0, events = 0, recurring = 0, instances = 0;
  uint32_t overrides = 0, cancelled = 0, unsupportedRules = 0, unknownTz = 0;
  uint32_t badDates = 0, truncated = 0, dropped = 0, zones = 0;
  bool     sawCalendar = false;    // BEGIN:VCALENDAR seen (sanity check)
  char     calName[40] = "";       // X-WR-CALNAME (ASCII-folded)
  char     calTz[48]   = "";       // X-WR-TIMEZONE
  bool     calTzResolved = false;  // a VTIMEZONE for calTz was parsed
  int      calTzOffsetMin = 0;     // ...and its UTC offset at windowStart
};

// Parsed recurrence rule (also used for VTIMEZONE observances).
struct RRule {
  uint8_t  freq = 0;               // 0 none, 1 daily, 2 weekly, 3 monthly, 4 yearly
  int      interval = 1;
  int      count = 0;              // 0 = unbounded
  bool     hasUntil = false;
  IcsDateTime until;
  uint8_t  byDayMask = 0;          // weekday bits (bit 0 = Sunday), no ordinal
  int8_t   byDayOrd[8] = {0};      // ordinal entries ("2TU", "-1FR")
  int8_t   byDayOrdWd[8] = {0};
  uint8_t  nByDayOrd = 0;
  int8_t   byMonthDay[8] = {0};
  uint8_t  nByMonthDay = 0;
  uint16_t byMonthMask = 0;        // bit m for month m (1..12)
  int16_t  bySetPos[4] = {0};
  uint8_t  nBySetPos = 0;
  uint8_t  wkst = 1;               // Monday
  bool     unsupported = false;
};
bool parseRRule(const char *s, RRule &r);

class IcsParser {
public:
  static constexpr int kMaxCand  = 48;   // candidates before overrides apply
  static constexpr int kMaxOut   = 24;
  static constexpr int kMaxZones = 8;
  static constexpr int kMaxObs   = 6;
  static constexpr int kMaxOvr   = 64;

  void begin(const IcsOptions &opt);
  void feed(const char *data, size_t n);
  void finish();

  int          count() const { return nOut_; }
  const Event &event(int i) const { return out_[i]; }
  const IcsStats &stats() const { return stats_; }

  // UTC offset (minutes) of a parsed VTIMEZONE at a UTC instant.
  bool zoneOffsetAt(const char *tzid, int64_t utc, int &offMin) const;

  // Exposed for tests: wall time in a parsed zone -> UTC instant.
  bool zoneWallToUtc(const char *tzid, int64_t wall, int64_t &utc) const;

private:
  // ---- time zones ----
  struct TzObs {
    int64_t dtstartWall;
    int16_t offFrom, offTo;        // minutes
    bool    hasRule;
    int8_t  month, ord, wday;      // yearly rule: BYMONTH + BYDAY
    int8_t  mdays[7];
    uint8_t nMdays;
    int64_t untilUtc;
    int64_t rdates[4];             // extra onsets (wall)
    uint8_t nRdates;
  };
  struct TzZone {
    char    name[48];
    TzObs   obs[kMaxObs];
    uint8_t nObs;
  };
  int  findZone(const char *name) const;
  int  zoneOffsetAtUtc(const TzZone &z, int64_t utc) const;
  int64_t zoneWallToUtcIdx(int zi, int64_t wall) const;
  int64_t toUtc(const IcsDateTime &dt, const char *tzid, bool countUnknown);

  // ---- streaming lexer ----
  enum LexState : uint8_t { L_NAME, L_PNAME, L_PVAL, L_VALUE };
  enum Prop : uint8_t {
    P_IGNORE, P_BEGIN, P_END,
    P_SUMMARY, P_LOCATION, P_DTSTART, P_DTEND, P_DURATION, P_RRULE, P_EXDATE,
    P_RDATE, P_RECURID, P_UID, P_STATUS, P_TRAVEL, P_MSALLDAY,
    P_TZID, P_OBS_DTSTART, P_OBS_FROM, P_OBS_TO, P_OBS_RRULE, P_OBS_RDATE,
    P_CALNAME, P_CALTZ,
  };
  enum Comp : uint8_t { C_NONE, C_CAL, C_EVENT, C_TZ, C_TZOBS, C_OTHER };

  void lexChar(char c);
  void endLine();
  void identify();
  void commitParam();
  void dispatch();
  void listItem();
  void beginComp(const char *name);
  void endComp();

  // ---- events ----
  struct Cur {
    IcsDateTime dtstart, dtend, recurId;
    char     tzStart[48], tzEnd[48], tzRecur[48];
    bool     hasEnd, hasDuration, hasRecurId, hasRrule, hasUid;
    int64_t  duration;
    char     rrule[200];
    uint32_t uid;
    char     title[kTitleMax];
    char     location[kLocMax];
    bool     cancelled, tentative, allDayX;
    uint8_t  leaveMin;
    int64_t  exUtc[40];   uint8_t nExUtc;
    int64_t  exDay[16];   uint8_t nExDay;
    int64_t  exFloat[16]; uint8_t nExFloat;
    int64_t  rdUtc[12];   uint8_t nRdUtc;
    int64_t  rdFloat[8];  uint8_t nRdFloat;
    int64_t  rdDay[8];    uint8_t nRdDay;
  };
  void resetCur();
  void finalizeEvent();
  void emitInstance(int64_t occWall, int64_t occUtc, int64_t durSec, bool allDay,
                    bool recurring, int64_t origKey);
  bool excluded(int64_t occWall, int64_t occUtc, bool allDay) const;
  void expandRule(const RRule &r, int64_t dtWall, int64_t durSec, bool allDay,
                  int zoneIdx, bool utcBased);
  int64_t occToUtc(int64_t wall, bool allDay, int zoneIdx, bool utcBased) const;
  void addOverride(uint32_t uid, int64_t orig);

  struct Cand { Event ev; int64_t orig; bool isOverride; };
  void addCandidate(const Event &ev, int64_t orig, bool isOverride);

  IcsOptions opt_;
  IcsStats   stats_;

  // lexer
  LexState lst_ = L_NAME;
  Prop     prop_ = P_IGNORE;
  bool     nlPending_ = false, crSeen_ = false, inQuote_ = false;
  uint8_t  bomSkip_ = 0;
  char     name_[40];   uint8_t nameLen_ = 0;
  char     pname_[24];  uint8_t pnameLen_ = 0;
  char     pval_[64];   uint8_t pvalLen_ = 0;
  char     tzParam_[48];
  char     valueParam_[12];
  char     val_[320];   uint16_t valLen_ = 0;
  bool     valOverflow_ = false;
  bool     lineHasContent_ = false;

  // components
  Comp     comp_[8];
  uint8_t  depth_ = 0;
  uint16_t deepOverflow_ = 0;      // BEGINs beyond the nesting limit

  Cur      cur_;
  TzZone   zones_[kMaxZones];
  uint8_t  nZones_ = 0;
  TzZone   zcur_;                  // zone being parsed
  TzObs    ocur_;                  // observance being parsed
  bool     ocurValid_ = false;
  bool     obsHaveFrom_ = false, obsHaveTo_ = false;
  char     ocurRrule_[120];

  Cand     cand_[kMaxCand];
  int      nCand_ = 0;
  struct Ovr { uint32_t uid; int64_t orig; };
  Ovr      ovr_[kMaxOvr];
  int      nOvr_ = 0;

  Event    out_[kMaxOut];
  int      nOut_ = 0;
};

}  // namespace mc
