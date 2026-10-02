// Calendar helpers for the art and the day records. A "day index" is the
// number of days since 2000-01-01 (the RV-3028's epoch), which fits a
// uint16_t until the year 2179.
#pragma once
#include <stdint.h>

namespace gf {

uint16_t dayIndex(uint16_t year, uint8_t month, uint8_t day);
void     dayToDate(uint16_t idx, uint16_t &year, uint8_t &month, uint8_t &day);
uint8_t  weekdayOf(uint16_t idx);            // 0 = Sunday .. 6 = Saturday
uint16_t dayOfYear(uint16_t idx);            // 1..366
uint8_t  daysInMonth(uint16_t year, uint8_t month);
bool     isLeapYear(uint16_t year);
bool     plausibleDate(uint16_t year, uint8_t month, uint8_t day);

const char *weekdayShort(uint8_t wd);        // "MON"
const char *monthShort(uint8_t month);       // "JAN" (1-based)

// Special days get a palette twist and a caption label.
enum SpecialDay : uint8_t {
  kOrdinary = 0, kNewYear, kMarchEquinox, kJuneSolstice, kSeptemberEquinox,
  kDecemberSolstice
};
SpecialDay specialDayOf(uint16_t idx);
const char *specialDayName(SpecialDay s);

// Parses "YYYY-MM-DD"; returns false on malformed or implausible input.
bool parseIsoDate(const char *s, uint16_t &idx);
// Writes "YYYY-MM-DD" (11 bytes including NUL).
void formatIsoDate(uint16_t idx, char *out);

}  // namespace gf
