#include "mc_proto.h"
#include "mc_text.h"
#include "mc_time.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

namespace mc {

static const char *skipSpaces(const char *p) {
  while (*p == ' ' || *p == '\t') p++;
  return p;
}

// Copies the next whitespace-delimited token into buf; returns the rest.
static const char *nextToken(const char *p, char *buf, size_t cap) {
  p = skipSpaces(p);
  size_t n = 0;
  while (*p && *p != ' ' && *p != '\t') {
    if (n + 1 < cap) buf[n++] = *p;
    p++;
  }
  buf[n] = '\0';
  return p;
}

void finishManualEvent(Event &e, Source src, uint32_t salt) {
  e.source = (uint8_t)src;
  e.feed = 0;
  uint32_t h = fnv1a(e.title, strlen(e.title));
  h = fnv1aByte((uint8_t)src, h);
  for (int i = 0; i < 4; i++) h = fnv1aByte((uint8_t)(salt >> (i * 8)), h);
  e.uid = h ? h : 1;
  e.key = instanceKey(e.uid, e.start);
}

bool parseEventArgs(const char *args, int tzOffsetMin, Source src, Event &out, const char **err) {
  static const char *kNoErr = "";
  const char *dummy;
  if (!err) err = &dummy;
  *err = kNoErr;
  memset((void *)&out, 0, sizeof(out));
  if (!args) { *err = "missing arguments"; return false; }

  char t1[40], t2[40];
  const char *p = nextToken(args, t1, sizeof(t1));
  p = nextToken(p, t2, sizeof(t2));
  if (!t1[0] || !t2[0]) { *err = "usage: EVENT <start> <end|+dur> [leave=N] [loc=X] <title>"; return false; }

  int64_t start;
  bool startDate = false;
  if (!parseIso8601(t1, tzOffsetMin, start, &startDate)) { *err = "bad start time"; return false; }

  int64_t end = 0;
  bool endDate = false;
  int64_t dur;
  if (parseIso8601(t2, tzOffsetMin, end, &endDate)) {
    // absolute end
  } else if (parseHumanDuration(t2, dur)) {
    end = start + dur;
    endDate = startDate && dur % kDay == 0;
  } else {
    *err = "bad end time or duration";
    return false;
  }

  // Options, then the title (the rest of the line).
  for (;;) {
    p = skipSpaces(p);
    if (strncmp(p, "leave=", 6) == 0) {
      char tok[16];
      p = nextToken(p + 6, tok, sizeof(tok));
      int64_t secs;
      if (!parseHumanDuration(tok, secs) || secs < 0 || secs > 240 * 60) { *err = "bad leave= minutes"; return false; }
      out.leaveMin = (uint8_t)(secs / 60);
      continue;
    }
    if (strncmp(p, "loc=", 4) == 0) {
      p += 4;
      char loc[96];
      size_t n = 0;
      if (*p == '"') {
        p++;
        while (*p && *p != '"') { if (n + 1 < sizeof(loc)) loc[n++] = *p; p++; }
        if (*p == '"') p++;
      } else {
        while (*p && *p != ' ' && *p != '\t') { if (n + 1 < sizeof(loc)) loc[n++] = *p; p++; }
      }
      loc[n] = '\0';
      foldToAscii(loc, n, out.location, sizeof(out.location));
      continue;
    }
    break;
  }
  size_t tl = strlen(p);
  while (tl && (p[tl - 1] == ' ' || p[tl - 1] == '\r' || p[tl - 1] == '\n' || p[tl - 1] == '\t')) tl--;
  foldToAscii(p, tl, out.title, sizeof(out.title));
  if (!out.title[0]) { *err = "missing title"; return false; }

  if (startDate && endDate) {
    // All-day: store wall-clock midnights.
    int64_t off = (int64_t)tzOffsetMin * 60;
    out.start = start + off;
    out.end = end + off;
    if (out.end <= out.start) out.end = out.start + kDay;
    out.flags = EF_ALLDAY;
    out.leaveMin = 0;
  } else {
    out.start = start;
    out.end = end;
    if (out.end < out.start) { *err = "end is before start"; return false; }
    if (out.end - out.start > 31 * kDay) { *err = "event longer than 31 days"; return false; }
  }
  finishManualEvent(out, src, 0);
  return true;
}

bool makeEvent(const char *title, const char *date, const char *startTime, const char *endTime,
               int durMin, int leaveMin, const char *location, int tzOffsetMin, Source src,
               uint32_t salt, Event &out, const char **err) {
  static const char *kNoErr = "";
  const char *dummy;
  if (!err) err = &dummy;
  *err = kNoErr;
  memset((void *)&out, 0, sizeof(out));
  if (!title || !date) { *err = "missing fields"; return false; }
  foldToAscii(title, strlen(title), out.title, sizeof(out.title));
  if (!out.title[0]) { *err = "please enter a title"; return false; }
  if (location) foldToAscii(location, strlen(location), out.location, sizeof(out.location));

  char iso[40];
  bool allDay = !startTime || !startTime[0];
  if (allDay) {
    int64_t s;
    bool dateOnly;
    if (!parseIso8601(date, tzOffsetMin, s, &dateOnly) || !dateOnly) { *err = "bad date"; return false; }
    out.start = s + (int64_t)tzOffsetMin * 60;
    int days = durMin >= 24 * 60 ? durMin / (24 * 60) : 1;
    out.end = out.start + (int64_t)days * kDay;
    out.flags = EF_ALLDAY;
    finishManualEvent(out, src, salt);
    return true;
  }
  snprintf(iso, sizeof(iso), "%sT%s", date, startTime);
  if (!parseIso8601(iso, tzOffsetMin, out.start, nullptr)) { *err = "bad date or start time"; return false; }
  if (endTime && endTime[0]) {
    snprintf(iso, sizeof(iso), "%sT%s", date, endTime);
    if (!parseIso8601(iso, tzOffsetMin, out.end, nullptr)) { *err = "bad end time"; return false; }
    if (out.end <= out.start) out.end += kDay;       // ends after midnight
  } else if (durMin > 0) {
    out.end = out.start + (int64_t)durMin * 60;
  } else {
    out.end = out.start + 30 * 60;
  }
  if (out.end - out.start > 31 * kDay) { *err = "event too long"; return false; }
  if (leaveMin < 0) leaveMin = 0;
  if (leaveMin > 240) leaveMin = 240;
  out.leaveMin = (uint8_t)leaveMin;
  finishManualEvent(out, src, salt);
  return true;
}

}  // namespace mc
