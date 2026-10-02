// Meeting Countdown — calendar/time arithmetic. Pure C++ (no Arduino), so the
// same code runs on the watch and in the host unit tests.
//
// Conventions used across meetcore:
//   * An "instant" is int64 Unix seconds (UTC).
//   * A "wall" value is local wall-clock time on the same scale: the Unix time
//     the wall clock would read if it were UTC. wall = utc + offset.
//   * Day numbers are days since 1970-01-01 (floorDiv(wall, 86400)).
//   * The watch RTC keeps local time as seconds since 2000-01-01 (apptimer's
//     rtcEpochSec); rtcToUtc()/utcToRtc() bridge the two with tzOffsetMin.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace mc {

constexpr int64_t kUnix2000 = 946684800LL;   // 2000-01-01T00:00:00Z
constexpr int64_t kDay      = 86400;
constexpr int64_t kNever    = INT64_MAX;

inline int64_t floorDiv(int64_t a, int64_t b) {
  int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
  return q;
}
inline int64_t floorMod(int64_t a, int64_t b) { return a - floorDiv(a, b) * b; }

// Proleptic Gregorian calendar (Howard Hinnant's algorithms).
int64_t daysFromCivil(int y, int m, int d);
void    civilFromDays(int64_t days, int &y, int &m, int &d);
int     weekdayFromDays(int64_t days);            // 0 = Sunday .. 6 = Saturday
int     daysInMonth(int y, int m);
bool    isLeapYear(int y);

struct Civil {
  int year, month, day, hour, minute, second;
  int weekday;                                     // 0 = Sunday
};
int64_t civilToSeconds(int y, int mo, int d, int h, int mi, int s);
Civil   secondsToCivil(int64_t t);

// Watch RTC (local seconds since 2000) <-> UTC instant.
inline int64_t rtcToUtc(uint32_t rtcEpoch2000, int tzOffsetMin) {
  return (int64_t)rtcEpoch2000 + kUnix2000 - (int64_t)tzOffsetMin * 60;
}
inline int64_t utcToRtc(int64_t utc, int tzOffsetMin) {
  return utc - kUnix2000 + (int64_t)tzOffsetMin * 60;
}

// ---- iCalendar value types ------------------------------------------------
// DATE "20261001", DATE-TIME "20261001T093000" or "20261001T093000Z".
struct IcsDateTime {
  int64_t wall  = 0;       // wall seconds (== UTC when isUtc)
  bool    isDate = false;  // VALUE=DATE (all-day)
  bool    isUtc  = false;  // trailing 'Z'
  bool    valid  = false;
};
bool parseIcsDateTime(const char *s, size_t n, IcsDateTime &out);

// DURATION: [+|-]P[nW][nD][T[nH][nM][nS]]
bool parseIcsDuration(const char *s, size_t n, int64_t &secs);

// UTC offset as used by TZOFFSETFROM/TO: "+0100", "-0530", "+013000",
// also tolerates "+01:00".
bool parseUtcOffset(const char *s, size_t n, int &minutes);

// ISO-8601 for the serial protocol and the web form:
//   2026-10-01            (date only -> local midnight)
//   2026-10-01T09:30[:00] (local, converted with defaultOffsetMin)
//   2026-10-01T08:30:00Z / 2026-10-01T09:30+01:00 / 20261001T083000Z
bool parseIso8601(const char *s, int defaultOffsetMin, int64_t &utc,
                  bool *dateOnly = nullptr);

// Durations a human types: "30m", "1h", "1h30m", "90" (minutes), "PT30M",
// "45s". An optional leading '+' is accepted.
bool parseHumanDuration(const char *s, int64_t &secs);

// "2026-10-01T09:30:00+01:00"
void formatIsoLocal(int64_t utc, int offsetMin, char *buf, size_t n);
// "09:30" from a wall value.
void formatHHMM(int64_t wall, char *buf, size_t n);

}  // namespace mc
