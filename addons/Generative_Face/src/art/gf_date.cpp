#include "gf_date.h"

namespace gf {

bool isLeapYear(uint16_t y) {
  return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

uint8_t daysInMonth(uint16_t y, uint8_t m) {
  static const uint8_t kDays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  if (m < 1 || m > 12) return 31;
  return (m == 2 && isLeapYear(y)) ? 29 : kDays[m - 1];
}

bool plausibleDate(uint16_t y, uint8_t m, uint8_t d) {
  return y >= 2000 && y <= 2178 && m >= 1 && m <= 12 && d >= 1 && d <= daysInMonth(y, m);
}

uint16_t dayIndex(uint16_t y, uint8_t m, uint8_t d) {
  if (y < 2000) y = 2000;
  if (m < 1) m = 1;
  if (m > 12) m = 12;
  if (d < 1) d = 1;
  uint32_t days = 0;
  for (uint16_t yy = 2000; yy < y; yy++) days += isLeapYear(yy) ? 366u : 365u;
  for (uint8_t mm = 1; mm < m; mm++) days += daysInMonth(y, mm);
  days += (uint32_t)(d - 1);
  return (uint16_t)(days > 65535u ? 65535u : days);
}

void dayToDate(uint16_t idx, uint16_t &y, uint8_t &m, uint8_t &d) {
  uint32_t rem = idx;
  y = 2000;
  for (;;) {
    uint32_t n = isLeapYear(y) ? 366u : 365u;
    if (rem < n) break;
    rem -= n;
    y++;
  }
  m = 1;
  for (;;) {
    uint32_t n = daysInMonth(y, m);
    if (rem < n) break;
    rem -= n;
    m++;
  }
  d = (uint8_t)(rem + 1);
}

uint8_t weekdayOf(uint16_t idx) {
  return (uint8_t)((idx + 6u) % 7u);         // 2000-01-01 was a Saturday
}

uint16_t dayOfYear(uint16_t idx) {
  uint16_t y; uint8_t m, d;
  dayToDate(idx, y, m, d);
  return (uint16_t)(idx - dayIndex(y, 1, 1) + 1);
}

const char *weekdayShort(uint8_t wd) {
  static const char *k[7] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
  return wd < 7 ? k[wd] : "---";
}

const char *monthShort(uint8_t m) {
  static const char *k[12] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                               "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
  return (m >= 1 && m <= 12) ? k[m - 1] : "---";
}

// Equinox / solstice dates (UTC) for 2020..2099, from Meeus' algorithm
// (Astronomical Algorithms ch. 27) and frozen. Per year one byte:
// bits 0-1 = March day - 19, 2-3 = June day - 19, 4-5 = Sept day - 21,
// 6-7 = Dec day - 20.
static const uint8_t kSeasons[80] = {
  0x55, 0x59, 0x69, 0xA9, 0x55, 0x59, 0x69, 0xA9, 0x55, 0x59, 0x59, 0xA9, 0x55, 0x59, 0x59, 0xA9,
  0x55, 0x59, 0x59, 0xA9, 0x55, 0x55, 0x59, 0xA9, 0x54, 0x55, 0x59, 0x69, 0x54, 0x55, 0x59, 0x69,
  0x54, 0x55, 0x59, 0x69, 0x54, 0x55, 0x59, 0x69, 0x54, 0x55, 0x59, 0x59, 0x54, 0x55, 0x59, 0x59,
  0x54, 0x55, 0x55, 0x59, 0x54, 0x55, 0x55, 0x59, 0x54, 0x54, 0x55, 0x59, 0x14, 0x54, 0x55, 0x59,
  0x14, 0x54, 0x55, 0x59, 0x14, 0x54, 0x55, 0x59, 0x04, 0x54, 0x55, 0x59, 0x04, 0x54, 0x55, 0x55,
};

SpecialDay specialDayOf(uint16_t idx) {
  uint16_t y; uint8_t m, d;
  dayToDate(idx, y, m, d);
  if (m == 1 && d == 1) return kNewYear;
  if (y < 2020 || y > 2099) return kOrdinary;
  uint8_t b = kSeasons[y - 2020];
  if (m == 3  && d == 19 + (b & 3))        return kMarchEquinox;
  if (m == 6  && d == 19 + ((b >> 2) & 3)) return kJuneSolstice;
  if (m == 9  && d == 21 + ((b >> 4) & 3)) return kSeptemberEquinox;
  if (m == 12 && d == 20 + ((b >> 6) & 3)) return kDecemberSolstice;
  return kOrdinary;
}

const char *specialDayName(SpecialDay s) {
  switch (s) {
    case kNewYear:          return "NEW YEAR";
    case kMarchEquinox:     return "EQUINOX";
    case kJuneSolstice:     return "SOLSTICE";
    case kSeptemberEquinox: return "EQUINOX";
    case kDecemberSolstice: return "SOLSTICE";
    default:                return "";
  }
}

static bool digits(const char *s, int n, int &out) {
  out = 0;
  for (int i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') return false;
    out = out * 10 + (s[i] - '0');
  }
  return true;
}

bool parseIsoDate(const char *s, uint16_t &idx) {
  int y, m, d;
  if (!s || !digits(s, 4, y) || s[4] != '-' || !digits(s + 5, 2, m) ||
      s[7] != '-' || !digits(s + 8, 2, d)) return false;
  if (s[10] != '\0' && s[10] != ' ' && s[10] != '\r' && s[10] != '\n') return false;
  if (!plausibleDate((uint16_t)y, (uint8_t)m, (uint8_t)d)) return false;
  idx = dayIndex((uint16_t)y, (uint8_t)m, (uint8_t)d);
  return true;
}

void formatIsoDate(uint16_t idx, char *out) {
  uint16_t y; uint8_t m, d;
  dayToDate(idx, y, m, d);
  out[0] = (char)('0' + (y / 1000) % 10); out[1] = (char)('0' + (y / 100) % 10);
  out[2] = (char)('0' + (y / 10) % 10);   out[3] = (char)('0' + y % 10);
  out[4] = '-';
  out[5] = (char)('0' + m / 10); out[6] = (char)('0' + m % 10);
  out[7] = '-';
  out[8] = (char)('0' + d / 10); out[9] = (char)('0' + d % 10);
  out[10] = '\0';
}

}  // namespace gf
