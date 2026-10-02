// ics_dump — run the watch's exact ICS parser on your computer.
//
// Build (macOS / Linux):
//   c++ -std=c++17 -O2 -Ilib/meetcore/src tools/ics_dump.cpp lib/meetcore/src/*.cpp -o ics_dump
// Use:
//   curl -s "$FEED_URL" | ./ics_dump --offset 60
//   ./ics_dump --now 2026-10-01T08:00:00Z --hours 48 --offset 60 < calendar.ics
//
// Prints the events the watch would keep (same window, same caps), with
// times shown in UTC and at the given offset, plus parser statistics.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ics_parser.h"
#include "mc_time.h"

using namespace mc;

int main(int argc, char **argv) {
  int64_t now = (int64_t)time(nullptr);
  int offset = 0, hours = 48, maxEvents = 16;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--now") && i + 1 < argc) {
      if (!parseIso8601(argv[++i], 0, now)) { fprintf(stderr, "bad --now\n"); return 2; }
    } else if (!strcmp(argv[i], "--offset") && i + 1 < argc) {
      offset = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--hours") && i + 1 < argc) {
      hours = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--max") && i + 1 < argc) {
      maxEvents = atoi(argv[++i]);
    } else {
      fprintf(stderr, "usage: ics_dump [--now ISO] [--offset MIN] [--hours H] [--max N] < feed.ics\n");
      return 2;
    }
  }
  static IcsParser p;
  IcsOptions o;
  o.windowStart = now;
  o.windowEnd = now + (int64_t)hours * 3600;
  o.fallbackOffsetMin = offset;
  o.maxEvents = maxEvents;
  p.begin(o);
  char buf[1460];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), stdin)) > 0) p.feed(buf, n);
  p.finish();

  char a[40], b[40];
  for (int i = 0; i < p.count(); i++) {
    const Event &e = p.event(i);
    if (isAllDay(e)) {
      Civil c = secondsToCivil(e.start);
      int days = (int)((e.end - e.start) / kDay);
      printf("ALL-DAY %04d-%02d-%02d (%d d)        %s%s%s\n", c.year, c.month, c.day, days,
             e.title, e.location[0] ? "  @ " : "", e.location);
    } else {
      formatIsoLocal(e.start, offset, a, sizeof(a));
      formatIsoLocal(e.end, offset, b, sizeof(b));
      printf("%s .. %s  %s%s%s%s\n", a, b + 11, e.title, e.location[0] ? "  @ " : "", e.location,
             e.leaveMin ? "  [leave buffer]" : "");
    }
  }
  const IcsStats &s = p.stats();
  printf("\n%u bytes, %u lines, %u VEVENTs (%u recurring, %u overrides, %u cancelled), "
         "%u instances in window, %d kept\n",
         s.bytes, s.lines, s.events, s.recurring, s.overrides, s.cancelled, s.instances, p.count());
  printf("zones %u, unknown TZIDs %u, unsupported rules %u, bad dates %u, truncated lines %u, "
         "dropped %u\n",
         s.zones, s.unknownTz, s.unsupportedRules, s.badDates, s.truncated, s.dropped);
  if (s.calName[0]) printf("calendar \"%s\"", s.calName);
  if (s.calTz[0]) printf("  time zone %s%s (UTC%+d min now)", s.calTz,
                         s.calTzResolved ? "" : " [no VTIMEZONE]", s.calTzOffsetMin);
  printf("\n%s\n", s.sawCalendar ? "" : "WARNING: no BEGIN:VCALENDAR - is this really an iCal feed?");
  return 0;
}
