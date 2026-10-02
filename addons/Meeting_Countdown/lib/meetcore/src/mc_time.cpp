#include "mc_time.h"
#include <stdio.h>
#include <string.h>

namespace mc {

int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = (int64_t)y - era * 400;                         // [0, 399]
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; // [0, 365]
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;          // [0, 146096]
  return era * 146097 + doe - 719468;
}

void civilFromDays(int64_t z, int &y, int &m, int &d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t yy  = yoe + era * 400;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp  = (5 * doy + 2) / 153;
  d = (int)(doy - (153 * mp + 2) / 5 + 1);
  m = (int)(mp < 10 ? mp + 3 : mp - 9);
  y = (int)(yy + (m <= 2));
}

int weekdayFromDays(int64_t z) {
  // 1970-01-01 was a Thursday (4).
  return (int)floorMod(z + 4, 7);
}

bool isLeapYear(int y) { return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0); }

int daysInMonth(int y, int m) {
  static const uint8_t kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m < 1 || m > 12) return 30;
  return (m == 2 && isLeapYear(y)) ? 29 : kDays[m - 1];
}

int64_t civilToSeconds(int y, int mo, int d, int h, int mi, int s) {
  return daysFromCivil(y, mo, d) * kDay + (int64_t)h * 3600 + (int64_t)mi * 60 + s;
}

Civil secondsToCivil(int64_t t) {
  Civil c;
  int64_t days = floorDiv(t, kDay);
  int64_t rem  = t - days * kDay;
  civilFromDays(days, c.year, c.month, c.day);
  c.hour    = (int)(rem / 3600);
  c.minute  = (int)((rem % 3600) / 60);
  c.second  = (int)(rem % 60);
  c.weekday = weekdayFromDays(days);
  return c;
}

// ---------------------------------------------------------------------------
static bool digitsAt(const char *s, size_t n, size_t pos, size_t count, int &out) {
  if (pos + count > n) return false;
  int v = 0;
  for (size_t i = 0; i < count; i++) {
    char c = s[pos + i];
    if (c < '0' || c > '9') return false;
    v = v * 10 + (c - '0');
  }
  out = v;
  return true;
}

static bool validCivil(int y, int mo, int d, int h, int mi, int s) {
  if (y < 1600 || y > 2400) return false;
  if (mo < 1 || mo > 12) return false;
  if (d < 1 || d > daysInMonth(y, mo)) return false;
  if (h < 0 || h > 24 || mi < 0 || mi > 59 || s < 0 || s > 60) return false;
  return true;
}

bool parseIcsDateTime(const char *s, size_t n, IcsDateTime &out) {
  out = IcsDateTime();
  // Trim surrounding whitespace defensively.
  while (n && (s[0] == ' ' || s[0] == '\t')) { s++; n--; }
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) n--;
  int y, mo, d, h = 0, mi = 0, sec = 0;
  if (!digitsAt(s, n, 0, 4, y) || !digitsAt(s, n, 4, 2, mo) || !digitsAt(s, n, 6, 2, d))
    return false;
  if (n == 8) {
    if (!validCivil(y, mo, d, 0, 0, 0)) return false;
    out.wall = civilToSeconds(y, mo, d, 0, 0, 0);
    out.isDate = true;
    out.valid = true;
    return true;
  }
  if (n < 15 || (s[8] != 'T' && s[8] != 't')) return false;
  if (!digitsAt(s, n, 9, 2, h) || !digitsAt(s, n, 11, 2, mi) || !digitsAt(s, n, 13, 2, sec))
    return false;
  if (!validCivil(y, mo, d, h, mi, sec)) return false;
  if (sec == 60) sec = 59;                         // leap second: clamp
  bool utc = false;
  if (n >= 16) {
    if (n == 16 && (s[15] == 'Z' || s[15] == 'z')) utc = true;
    else return false;
  }
  out.wall  = civilToSeconds(y, mo, d, h, mi, sec);
  out.isUtc = utc;
  out.valid = true;
  return true;
}

bool parseIcsDuration(const char *s, size_t n, int64_t &secs) {
  size_t i = 0;
  int sign = 1;
  if (i < n && (s[i] == '+' || s[i] == '-')) { if (s[i] == '-') sign = -1; i++; }
  if (i >= n || (s[i] != 'P' && s[i] != 'p')) return false;
  i++;
  bool inTime = false, any = false;
  int64_t total = 0;
  while (i < n) {
    char c = s[i];
    if (c == 'T' || c == 't') { inTime = true; i++; continue; }
    if (c < '0' || c > '9') return false;
    int64_t v = 0;
    size_t start = i;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
      v = v * 10 + (s[i] - '0');
      if (v > 100000000LL) return false;
      i++;
    }
    if (i == start || i >= n) return false;
    char u = s[i++];
    if (u >= 'a' && u <= 'z') u = (char)(u - 32);
    if (!inTime) {
      if (u == 'W')      total += v * 7 * kDay;
      else if (u == 'D') total += v * kDay;
      else return false;
    } else {
      if (u == 'H')      total += v * 3600;
      else if (u == 'M') total += v * 60;
      else if (u == 'S') total += v;
      else return false;
    }
    any = true;
  }
  if (!any) return false;
  secs = sign * total;
  return true;
}

bool parseUtcOffset(const char *s, size_t n, int &minutes) {
  while (n && (s[0] == ' ')) { s++; n--; }
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\r')) n--;
  if (n < 3) return false;
  int sign;
  if (s[0] == '+') sign = 1; else if (s[0] == '-') sign = -1; else return false;
  char d[8]; size_t k = 0;
  for (size_t i = 1; i < n && k < sizeof(d); i++) {
    if (s[i] == ':') continue;
    if (s[i] < '0' || s[i] > '9') return false;
    d[k++] = s[i];
  }
  if (k != 2 && k != 4 && k != 6) return false;
  int hh = (d[0] - '0') * 10 + (d[1] - '0');
  int mm = (k >= 4) ? (d[2] - '0') * 10 + (d[3] - '0') : 0;
  if (hh > 23 || mm > 59) return false;
  minutes = sign * (hh * 60 + mm);
  return true;
}

// ---------------------------------------------------------------------------
bool parseIso8601(const char *s, int defaultOffsetMin, int64_t &utc, bool *dateOnly) {
  if (!s) return false;
  size_t n = strlen(s);
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\r' || s[n - 1] == '\n')) n--;
  while (n && s[0] == ' ') { s++; n--; }
  if (dateOnly) *dateOnly = false;
  int y, mo, d, h = 0, mi = 0, sec = 0;
  size_t i;
  // Basic format (iCalendar style) first: 20261001 / 20261001T093000[Z]
  if (n >= 8 && s[4] != '-') {
    IcsDateTime dt;
    if (!parseIcsDateTime(s, n, dt)) return false;
    if (dateOnly) *dateOnly = dt.isDate;
    utc = dt.isUtc ? dt.wall : dt.wall - (int64_t)defaultOffsetMin * 60;
    return true;
  }
  if (!digitsAt(s, n, 0, 4, y) || n < 10 || s[4] != '-' || !digitsAt(s, n, 5, 2, mo) ||
      s[7] != '-' || !digitsAt(s, n, 8, 2, d))
    return false;
  i = 10;
  if (i == n) {
    if (!validCivil(y, mo, d, 0, 0, 0)) return false;
    if (dateOnly) *dateOnly = true;
    utc = civilToSeconds(y, mo, d, 0, 0, 0) - (int64_t)defaultOffsetMin * 60;
    return true;
  }
  if (s[i] != 'T' && s[i] != 't' && s[i] != ' ') return false;
  i++;
  if (!digitsAt(s, n, i, 2, h) || i + 2 >= n || s[i + 2] != ':' ||
      !digitsAt(s, n, i + 3, 2, mi))
    return false;
  i += 5;
  if (i < n && s[i] == ':') {
    if (!digitsAt(s, n, i + 1, 2, sec)) return false;
    i += 3;
    // Fractional seconds are accepted and ignored.
    if (i < n && (s[i] == '.' || s[i] == ',')) {
      i++;
      while (i < n && s[i] >= '0' && s[i] <= '9') i++;
    }
  }
  if (!validCivil(y, mo, d, h, mi, sec)) return false;
  int64_t wall = civilToSeconds(y, mo, d, h, mi, sec);
  if (i == n) { utc = wall - (int64_t)defaultOffsetMin * 60; return true; }
  if ((s[i] == 'Z' || s[i] == 'z') && i + 1 == n) { utc = wall; return true; }
  int off;
  if (!parseUtcOffset(s + i, n - i, off)) return false;
  utc = wall - (int64_t)off * 60;
  return true;
}

bool parseHumanDuration(const char *s, int64_t &secs) {
  if (!s) return false;
  while (*s == ' ') s++;
  if (*s == '+') s++;
  size_t n = strlen(s);
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\r' || s[n - 1] == '\n')) n--;
  if (n == 0) return false;
  if (s[0] == 'P' || s[0] == 'p') return parseIcsDuration(s, n, secs) && secs >= 0;
  int64_t total = 0, v = 0;
  bool haveNum = false, any = false;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (c >= '0' && c <= '9') {
      v = v * 10 + (c - '0');
      if (v > 10000000) return false;
      haveNum = true;
      continue;
    }
    if (!haveNum) return false;
    if (c == 'h' || c == 'H')      total += v * 3600;
    else if (c == 'm' || c == 'M') total += v * 60;
    else if (c == 's' || c == 'S') total += v;
    else if (c == 'd' || c == 'D') total += v * kDay;
    else return false;
    v = 0; haveNum = false; any = true;
  }
  if (haveNum) { total += v * 60; any = true; }     // bare number = minutes
  if (!any) return false;
  secs = total;
  return true;
}

void formatIsoLocal(int64_t utc, int offsetMin, char *buf, size_t n) {
  Civil c = secondsToCivil(utc + (int64_t)offsetMin * 60);
  int a = offsetMin < 0 ? -offsetMin : offsetMin;
  snprintf(buf, n, "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d", c.year, c.month, c.day,
           c.hour, c.minute, c.second, offsetMin < 0 ? '-' : '+', a / 60, a % 60);
}

void formatHHMM(int64_t wall, char *buf, size_t n) {
  int64_t rem = floorMod(wall, kDay);
  snprintf(buf, n, "%02d:%02d", (int)(rem / 3600), (int)((rem % 3600) / 60));
}

}  // namespace mc
