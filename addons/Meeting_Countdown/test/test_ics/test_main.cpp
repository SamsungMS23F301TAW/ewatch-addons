// Host tests for the streaming ICS parser: line folding, UTC/TZID/floating
// times, VTIMEZONE + DST, all-day events, RRULE (daily/weekly/monthly/yearly
// with INTERVAL/COUNT/UNTIL/BYDAY/WKST/BYSETPOS), EXDATE, RECURRENCE-ID
// overrides, malformed input and multi-megabyte streams. Every fixture is
// parsed at several chunk sizes to prove results do not depend on where the
// network splits the stream.
#include <unity.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "ics_parser.h"
#include "mc_time.h"
#include "mc_text.h"

using namespace mc;

#ifndef MC_FIXTURE_DIR
#define MC_FIXTURE_DIR "test/fixtures"
#endif

static const int64_t NOW = 1790841600;          // 2026-10-01T08:00:00Z (Thursday)
static const int64_t H = 3600;

static std::string loadFixture(const char *name) {
  std::string path = std::string(MC_FIXTURE_DIR) + "/" + name;
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) {
    std::string msg = "cannot open fixture " + path;
    TEST_FAIL_MESSAGE(msg.c_str());
  }
  std::string s;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
  fclose(f);
  return s;
}

static IcsParser *P() {
  static IcsParser *p = new IcsParser();       // ~17 KB: keep it off the stack
  return p;
}

static IcsOptions window48(int64_t now = NOW) {
  IcsOptions o;
  o.windowStart = now;
  o.windowEnd = now + 48 * H;
  o.fallbackOffsetMin = 60;
  o.maxEvents = 16;
  o.maxAllDay = 4;
  return o;
}

static void parse(const std::string &data, const IcsOptions &o, size_t chunk = 0) {
  IcsParser *p = P();
  p->begin(o);
  if (chunk == 0) chunk = data.size() ? data.size() : 1;
  for (size_t i = 0; i < data.size(); i += chunk) {
    size_t n = data.size() - i < chunk ? data.size() - i : chunk;
    p->feed(data.data() + i, n);
  }
  p->finish();
}

static const Event *findTitle(const char *title) {
  IcsParser *p = P();
  for (int i = 0; i < p->count(); i++)
    if (strcmp(p->event(i).title, title) == 0) return &p->event(i);
  return nullptr;
}

static int countTitle(const char *title) {
  int c = 0;
  IcsParser *p = P();
  for (int i = 0; i < p->count(); i++)
    if (strcmp(p->event(i).title, title) == 0) c++;
  return c;
}

static std::string dump() {
  std::string s;
  IcsParser *p = P();
  char line[160];
  for (int i = 0; i < p->count(); i++) {
    const Event &e = p->event(i);
    snprintf(line, sizeof(line), "%lld|%lld|%08x|%02x|%u|%s|%s\n", (long long)e.start,
             (long long)e.end, e.key, e.flags, e.leaveMin, e.title, e.location);
    s += line;
  }
  return s;
}

// Parse at several chunk sizes and require byte-identical results.
static void parseAllChunkings(const char *fixture, const IcsOptions &o) {
  std::string data = loadFixture(fixture);
  parse(data, o, 0);
  std::string ref = dump();
  const size_t sizes[] = {1, 2, 3, 7, 64, 1460};
  for (size_t cs : sizes) {
    parse(data, o, cs);
    std::string got = dump();
    if (got != ref) {
      char msg[96];
      snprintf(msg, sizeof(msg), "%s differs at chunk size %u", fixture, (unsigned)cs);
      TEST_FAIL_MESSAGE(msg);
    }
  }
}

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------
void test_time_basics() {
  TEST_ASSERT_EQUAL_INT(4, weekdayFromDays(daysFromCivil(2026, 10, 1)));   // Thursday
  TEST_ASSERT_EQUAL_INT64(NOW, civilToSeconds(2026, 10, 1, 8, 0, 0));
  Civil c = secondsToCivil(NOW);
  TEST_ASSERT_EQUAL_INT(2026, c.year);
  TEST_ASSERT_EQUAL_INT(10, c.month);
  TEST_ASSERT_EQUAL_INT(1, c.day);
  TEST_ASSERT_EQUAL_INT(8, c.hour);
  // Round trips across a wide range including leap days and pre-1970.
  for (int64_t d = -200000; d < 200000; d += 997) {
    int y, m, dd;
    civilFromDays(d, y, m, dd);
    TEST_ASSERT_EQUAL_INT64(d, daysFromCivil(y, m, dd));
  }
  TEST_ASSERT_EQUAL_INT(29, daysInMonth(2028, 2));
  TEST_ASSERT_EQUAL_INT(28, daysInMonth(2100, 2));
  // RTC epoch bridge: 2026-10-01 09:00 local at +60 == NOW.
  uint32_t rtc = (uint32_t)(civilToSeconds(2026, 10, 1, 9, 0, 0) - kUnix2000);
  TEST_ASSERT_EQUAL_INT64(NOW, rtcToUtc(rtc, 60));
  TEST_ASSERT_EQUAL_INT64((int64_t)rtc, utcToRtc(NOW, 60));
}

void test_ics_datetime_and_duration() {
  IcsDateTime dt;
  TEST_ASSERT_TRUE(parseIcsDateTime("20261001T090000Z", 16, dt));
  TEST_ASSERT_TRUE(dt.isUtc);
  TEST_ASSERT_FALSE(dt.isDate);
  TEST_ASSERT_EQUAL_INT64(NOW + H, dt.wall);
  TEST_ASSERT_TRUE(parseIcsDateTime("20261001", 8, dt));
  TEST_ASSERT_TRUE(dt.isDate);
  TEST_ASSERT_FALSE(parseIcsDateTime("2026-10-01", 10, dt));
  TEST_ASSERT_FALSE(parseIcsDateTime("20261301T000000", 15, dt));
  TEST_ASSERT_FALSE(parseIcsDateTime("20260230T000000", 15, dt));
  int64_t s;
  TEST_ASSERT_TRUE(parseIcsDuration("PT1H30M", 7, s));  TEST_ASSERT_EQUAL_INT64(5400, s);
  TEST_ASSERT_TRUE(parseIcsDuration("P1DT2H", 6, s));   TEST_ASSERT_EQUAL_INT64(93600, s);
  TEST_ASSERT_TRUE(parseIcsDuration("P2W", 3, s));      TEST_ASSERT_EQUAL_INT64(14 * 86400, s);
  TEST_ASSERT_TRUE(parseIcsDuration("-PT15M", 6, s));   TEST_ASSERT_EQUAL_INT64(-900, s);
  TEST_ASSERT_FALSE(parseIcsDuration("PT", 2, s));
  TEST_ASSERT_FALSE(parseIcsDuration("1H", 2, s));
  int off;
  TEST_ASSERT_TRUE(parseUtcOffset("+0100", 5, off));   TEST_ASSERT_EQUAL_INT(60, off);
  TEST_ASSERT_TRUE(parseUtcOffset("-0530", 5, off));   TEST_ASSERT_EQUAL_INT(-330, off);
  TEST_ASSERT_TRUE(parseUtcOffset("+05:45", 6, off));  TEST_ASSERT_EQUAL_INT(345, off);
  TEST_ASSERT_FALSE(parseUtcOffset("0100", 4, off));
}

void test_text_folding() {
  char out[48];
  const char *in1 = "Caf\xC3\xA9 \xE2\x80\x9C" "quoted\xE2\x80\x9D \xE2\x80\x94 Zo\xC3\xAB";
  foldToAscii(in1, strlen(in1), out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Cafe \"quoted\" - Zoe", out);
  const char *in2 = "  Stra\xC3\x9F" "e  \xF0\x9F\x8E\x89  \xC5\x81\xC3\xB3" "d\xC5\xBA  ";
  foldToAscii(in2, strlen(in2), out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Strasse Lodz", out);
  foldToAscii("\xE6\x97\xA5\xE6\x9C\xAC", 6, out, sizeof(out));         // CJK: dropped
  TEST_ASSERT_EQUAL_STRING("", out);
  char esc[] = "a\\, b\\; c\\nd\\\\e";
  size_t n = icsUnescape(esc, strlen(esc));
  esc[n] = '\0';
  TEST_ASSERT_EQUAL_STRING("a, b; c d\\e", esc);
  // Truncation never exceeds the buffer.
  char tiny[6];
  foldToAscii("abcdefghij", 10, tiny, sizeof(tiny));
  TEST_ASSERT_EQUAL_STRING("abcde", tiny);
}

// ---------------------------------------------------------------------------
void test_basic_utc_folding_and_filtering() {
  IcsOptions o = window48();
  parseAllChunkings("basic_utc.ics", o);
  IcsParser *p = P();
  TEST_ASSERT_EQUAL_INT(5, p->count());
  TEST_ASSERT_EQUAL_STRING("Work", p->stats().calName);
  TEST_ASSERT_TRUE(p->stats().sawCalendar);

  // Sorted by start; the in-progress event comes first.
  TEST_ASSERT_EQUAL_STRING("Early sync", p->event(0).title);
  const Event *d = findTitle("Design review, round 2; final");
  TEST_ASSERT_NOT_NULL(d);
  TEST_ASSERT_EQUAL_INT64(NOW + H, d->start);
  TEST_ASSERT_EQUAL_INT64(NOW + H + 1800, d->end);
  TEST_ASSERT_EQUAL_STRING("Room 4.12", d->location);

  // Folded mid-UTF-8 sequence + emoji dropped + DURATION.
  const Event *c = findTitle("Cafe catch-up with Zoe");
  TEST_ASSERT_NOT_NULL(c);
  TEST_ASSERT_EQUAL_INT64(NOW + 5 * H, c->start);
  TEST_ASSERT_EQUAL_INT64(45 * 60, c->end - c->start);

  // VALARM's SUMMARY/DESCRIPTION never leak into the event.
  TEST_ASSERT_NULL(findTitle("Alarm summary must be ignored"));
  // Cancelled, past and far-future events are filtered out.
  TEST_ASSERT_NULL(findTitle("Cancelled thing"));
  TEST_ASSERT_NULL(findTitle("Yesterday"));
  TEST_ASSERT_NULL(findTitle("Next week"));

  // No DTEND / DURATION: zero length, synthesized UID.
  const Event *r = findTitle("Reminder without end");
  TEST_ASSERT_NOT_NULL(r);
  TEST_ASSERT_EQUAL_INT64(r->start, r->end);
  TEST_ASSERT_NOT_EQUAL(0, r->uid);

  // Apple travel time becomes the leave-now buffer.
  const Event *l = findTitle("Lunch across town");
  TEST_ASSERT_NOT_NULL(l);
  TEST_ASSERT_EQUAL_UINT8(20, l->leaveMin);
}

void test_tzid_and_floating() {
  IcsOptions o = window48();
  parseAllChunkings("tzid.ics", o);
  IcsParser *p = P();
  TEST_ASSERT_EQUAL_INT(5, p->count());
  TEST_ASSERT_EQUAL_INT64(1790845200, findTitle("London ten o'clock")->start);   // 09:00Z
  TEST_ASSERT_EQUAL_INT64(1790845200 + H, findTitle("London ten o'clock")->end);
  TEST_ASSERT_EQUAL_INT64(1790859600, findTitle("New York standup")->start);     // 13:00Z
  // Floating 16:00 uses the watch offset (+60): 15:00Z.
  TEST_ASSERT_EQUAL_INT64(NOW + 7 * H, findTitle("Floating four pm")->start);
  // Unknown TZID falls back to the watch offset and is counted.
  TEST_ASSERT_EQUAL_INT64(NOW + 24 * H, findTitle("Unknown zone")->start);
  TEST_ASSERT_TRUE(p->stats().unknownTz >= 1);
  // Quoted TZID parameter.
  TEST_ASSERT_EQUAL_INT64(1790924400, findTitle("Quoted TZID")->start);
  // Calendar time zone is known and currently BST.
  TEST_ASSERT_EQUAL_STRING("Europe/London", p->stats().calTz);
  TEST_ASSERT_TRUE(p->stats().calTzResolved);
  TEST_ASSERT_EQUAL_INT(60, p->stats().calTzOffsetMin);
  TEST_ASSERT_EQUAL_INT(2, (int)p->stats().zones);
}

void test_vtimezone_dst_rules() {
  IcsOptions o = window48();
  parse(loadFixture("tzid.ics"), o);
  IcsParser *p = P();
  int off;
  // London: BST until 2026-10-25 01:00Z, GMT after.
  TEST_ASSERT_TRUE(p->zoneOffsetAt("Europe/London", civilToSeconds(2026, 10, 25, 0, 59, 59), off));
  TEST_ASSERT_EQUAL_INT(60, off);
  TEST_ASSERT_TRUE(p->zoneOffsetAt("Europe/London", civilToSeconds(2026, 10, 25, 1, 0, 0), off));
  TEST_ASSERT_EQUAL_INT(0, off);
  TEST_ASSERT_TRUE(p->zoneOffsetAt("Europe/London", civilToSeconds(2026, 1, 15, 12, 0, 0), off));
  TEST_ASSERT_EQUAL_INT(0, off);
  TEST_ASSERT_TRUE(p->zoneOffsetAt("Europe/London", civilToSeconds(2026, 3, 29, 1, 0, 0), off));
  TEST_ASSERT_EQUAL_INT(60, off);
  // New York: EDT until 2026-11-01 06:00Z.
  TEST_ASSERT_TRUE(p->zoneOffsetAt("America/New_York", civilToSeconds(2026, 11, 1, 5, 59, 0), off));
  TEST_ASSERT_EQUAL_INT(-240, off);
  TEST_ASSERT_TRUE(p->zoneOffsetAt("America/New_York", civilToSeconds(2026, 11, 1, 6, 0, 0), off));
  TEST_ASSERT_EQUAL_INT(-300, off);
  // Wall -> UTC on both sides of the London change.
  int64_t utc;
  TEST_ASSERT_TRUE(p->zoneWallToUtc("Europe/London", civilToSeconds(2026, 10, 24, 9, 0, 0), utc));
  TEST_ASSERT_EQUAL_INT64(civilToSeconds(2026, 10, 24, 8, 0, 0), utc);
  TEST_ASSERT_TRUE(p->zoneWallToUtc("Europe/London", civilToSeconds(2026, 10, 26, 9, 0, 0), utc));
  TEST_ASSERT_EQUAL_INT64(civilToSeconds(2026, 10, 26, 9, 0, 0), utc);
  TEST_ASSERT_FALSE(p->zoneOffsetAt("Nowhere/Nothing", NOW, off));
}

void test_recurrence_across_dst() {
  // Window 2026-10-18 .. 2026-11-03 spans both the UK and US changes.
  IcsOptions o;
  o.windowStart = civilToSeconds(2026, 10, 18, 0, 0, 0);
  o.windowEnd = civilToSeconds(2026, 11, 3, 0, 0, 0);
  o.fallbackOffsetMin = 60;
  o.maxEvents = 24;
  parseAllChunkings("dst.ics", o);
  IcsParser *p = P();
  int mondays = 0, ny = 0;
  bool sawOct19 = false, sawOct26 = false, sawNov2 = false, sawOct28 = false;
  for (int i = 0; i < p->count(); i++) {
    const Event &e = p->event(i);
    if (strcmp(e.title, "Monday planning") == 0) {
      mondays++;
      if (e.start == 1792396800) sawOct19 = true;   // 09:00 BST = 08:00Z
      if (e.start == 1793005200) sawOct26 = true;   // 09:00 GMT = 09:00Z
      TEST_ASSERT_TRUE((e.flags & EF_RECURRING) != 0);
    }
    if (strcmp(e.title, "NY weekly") == 0) {
      ny++;
      if (e.start == 1793192400) sawOct28 = true;   // 09:00 EDT = 13:00Z
      if (e.start == 1793628000) sawNov2 = true;    // 09:00 EST = 14:00Z
    }
  }
  TEST_ASSERT_TRUE(sawOct19);
  TEST_ASSERT_TRUE(sawOct26);
  TEST_ASSERT_TRUE(sawOct28);
  TEST_ASSERT_TRUE(sawNov2);
  TEST_ASSERT_EQUAL_INT(3, mondays);    // Oct 19, 26, Nov 2
  TEST_ASSERT_EQUAL_INT(5, ny);         // Oct 19, 21, 26, 28, Nov 2
  // The UTC-anchored daily series ignores DST: always 12:00Z.
  for (int i = 0; i < p->count(); i++)
    if (strcmp(p->event(i).title, "UTC daily") == 0)
      TEST_ASSERT_EQUAL_INT64(12 * H, floorMod(p->event(i).start, kDay));
}

void test_all_day_events() {
  IcsOptions o = window48();
  parseAllChunkings("allday.ics", o);
  IcsParser *p = P();
  TEST_ASSERT_EQUAL_INT(5, p->count());
  const Event *b = findTitle("Mum's birthday");
  TEST_ASSERT_NOT_NULL(b);
  TEST_ASSERT_TRUE(isAllDay(*b));
  TEST_ASSERT_EQUAL_INT64(civilToSeconds(2026, 10, 1, 0, 0, 0), b->start);   // wall midnight
  TEST_ASSERT_EQUAL_INT64(civilToSeconds(2026, 10, 2, 0, 0, 0), b->end);
  const Event *conf = findTitle("Conference");
  TEST_ASSERT_NOT_NULL(conf);
  TEST_ASSERT_EQUAL_INT64(3 * kDay, conf->end - conf->start);
  const Event *noEnd = findTitle("No end date all-day");
  TEST_ASSERT_NOT_NULL(noEnd);
  TEST_ASSERT_EQUAL_INT64(kDay, noEnd->end - noEnd->start);
  const Event *ms = findTitle("Outlook all-day");
  TEST_ASSERT_NOT_NULL(ms);
  TEST_ASSERT_TRUE(isAllDay(*ms));
  TEST_ASSERT_EQUAL_INT64(civilToSeconds(2026, 10, 2, 0, 0, 0), ms->start);
  TEST_ASSERT_NULL(findTitle("Last week"));
  const Event *t = findTitle("Timed among all-day");
  TEST_ASSERT_NOT_NULL(t);
  TEST_ASSERT_FALSE(isAllDay(*t));

  // The all-day cap keeps timed events: with room for 2, only 2 all-day stay.
  o.maxEvents = 3;
  o.maxAllDay = 2;
  parse(loadFixture("allday.ics"), o);
  int allDay = 0;
  for (int i = 0; i < p->count(); i++) if (isAllDay(p->event(i))) allDay++;
  TEST_ASSERT_EQUAL_INT(3, p->count());
  TEST_ASSERT_EQUAL_INT(2, allDay);
  TEST_ASSERT_NOT_NULL(findTitle("Timed among all-day"));
}

void test_rrule_daily() {
  IcsOptions o = window48();
  parseAllChunkings("rrule_daily.ics", o);
  IcsParser *p = P();
  // UNTIL (UTC) cuts the London-time series after Oct 1.
  TEST_ASSERT_EQUAL_INT(1, countTitle("Daily until Oct 1"));
  TEST_ASSERT_EQUAL_INT64(NOW + 15 * 60, findTitle("Daily until Oct 1")->start);   // 09:15 BST
  // COUNT=4 starting Sep 28 ends with Oct 1.
  TEST_ASSERT_EQUAL_INT(1, countTitle("Daily count 4"));
  TEST_ASSERT_EQUAL_INT64(NOW + 6 * H, findTitle("Daily count 4")->start);
  // INTERVAL=2 from Sep 20 lands on Oct 2.
  TEST_ASSERT_EQUAL_INT(1, countTitle("Every other day"));
  TEST_ASSERT_EQUAL_INT64(NOW + 24 * H + 9 * H, findTitle("Every other day")->start);
  // DAILY + BYDAY weekdays: Thu Oct 1 and Fri Oct 2.
  TEST_ASSERT_EQUAL_INT(2, countTitle("Weekdays only"));
  // EXDATE removes Oct 1; Oct 2 remains.
  TEST_ASSERT_EQUAL_INT(1, countTitle("Daily with exdate"));
  TEST_ASSERT_EQUAL_INT64(NOW + 27 * H, findTitle("Daily with exdate")->start);
  TEST_ASSERT_EQUAL_INT(0, countTitle("Finished series"));
  TEST_ASSERT_EQUAL_INT(6, p->count());
}

void test_rrule_weekly_exdate_overrides() {
  IcsOptions o = window48();
  parseAllChunkings("rrule_weekly.ics", o);
  IcsParser *p = P();
  TEST_ASSERT_EQUAL_INT(1, countTitle("Mon Wed Fri"));
  TEST_ASSERT_EQUAL_INT64(NOW + 25 * H, findTitle("Mon Wed Fri")->start);       // Fri 10:00 BST
  TEST_ASSERT_EQUAL_INT(1, countTitle("Fortnightly on"));
  TEST_ASSERT_EQUAL_INT64(NOW + 6 * H, findTitle("Fortnightly on")->start);     // Thu 15:00 BST
  TEST_ASSERT_EQUAL_INT(0, countTitle("Fortnightly off"));
  TEST_ASSERT_EQUAL_INT(0, countTitle("Three weeks only"));
  TEST_ASSERT_EQUAL_INT(1, countTitle("Third and last"));
  TEST_ASSERT_EQUAL_INT(0, countTitle("Ended in September"));
  // EXDATE (TZID form) drops Oct 2; the Oct 1 instance is replaced by its
  // RECURRENCE-ID override at 11:00 BST.
  TEST_ASSERT_EQUAL_INT(0, countTitle("Standup"));
  TEST_ASSERT_EQUAL_INT(1, countTitle("Standup (moved)"));
  const Event *mv = findTitle("Standup (moved)");
  TEST_ASSERT_EQUAL_INT64(NOW + 2 * H, mv->start);
  TEST_ASSERT_TRUE((mv->flags & EF_OVERRIDE) != 0);
  // Cancelled override removes that week's retro.
  TEST_ASSERT_EQUAL_INT(0, countTitle("Retro"));
  // Override that appears before its master still wins.
  TEST_ASSERT_EQUAL_INT(0, countTitle("Sync"));
  TEST_ASSERT_EQUAL_INT(1, countTitle("Sync (late start)"));
  TEST_ASSERT_EQUAL_INT64(civilToSeconds(2026, 10, 2, 13, 30, 0), findTitle("Sync (late start)")->start);
  TEST_ASSERT_EQUAL_INT(5, p->count());
  TEST_ASSERT_TRUE(p->stats().overrides >= 3);
}

// RFC 5545 §3.3.10 WKST example: same rule, different week start.
static std::string wkstCal(const char *wkst) {
  std::string s = "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:w\r\nSUMMARY:W\r\n"
                  "DTSTART:19970805T090000Z\r\nDTEND:19970805T100000Z\r\n"
                  "RRULE:FREQ=WEEKLY;INTERVAL=2;COUNT=4;BYDAY=TU,SU;WKST=";
  s += wkst;
  s += "\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
  return s;
}

void test_rrule_wkst() {
  IcsOptions o;
  o.windowStart = civilToSeconds(1997, 8, 1, 0, 0, 0);
  o.windowEnd = civilToSeconds(1997, 9, 30, 0, 0, 0);
  o.maxEvents = 16;
  parse(wkstCal("MO"), o);
  IcsParser *p = P();
  TEST_ASSERT_EQUAL_INT(4, p->count());
  const int mo[4] = {5, 10, 19, 24};
  for (int i = 0; i < 4; i++)
    TEST_ASSERT_EQUAL_INT64(civilToSeconds(1997, 8, mo[i], 9, 0, 0), p->event(i).start);
  parse(wkstCal("SU"), o);
  TEST_ASSERT_EQUAL_INT(4, p->count());
  const int su[4] = {5, 17, 19, 31};
  for (int i = 0; i < 4; i++)
    TEST_ASSERT_EQUAL_INT64(civilToSeconds(1997, 8, su[i], 9, 0, 0), p->event(i).start);
}

void test_rrule_monthly_yearly() {
  IcsOptions o;
  o.windowStart = civilToSeconds(2026, 10, 1, 0, 0, 0);
  o.windowEnd = civilToSeconds(2026, 12, 1, 0, 0, 0);
  o.fallbackOffsetMin = 60;
  o.maxEvents = 24;
  o.maxAllDay = 4;
  parseAllChunkings("monthly.ics", o);
  IcsParser *p = P();
  auto has = [&](const char *t, int64_t start) {
    for (int i = 0; i < p->count(); i++)
      if (strcmp(p->event(i).title, t) == 0 && p->event(i).start == start) return true;
    return false;
  };
  TEST_ASSERT_TRUE(has("First Monday", 1791190800));                           // Oct 5 09:00Z
  TEST_ASSERT_TRUE(has("First Monday", civilToSeconds(2026, 11, 2, 10, 0, 0)));  // GMT in Nov
  TEST_ASSERT_TRUE(has("Month end", civilToSeconds(2026, 10, 31, 17, 0, 0)));
  TEST_ASSERT_TRUE(has("Month end", civilToSeconds(2026, 11, 30, 17, 0, 0)));
  TEST_ASSERT_TRUE(has("Last working day", civilToSeconds(2026, 10, 30, 16, 0, 0)));
  TEST_ASSERT_TRUE(has("Last working day", civilToSeconds(2026, 11, 30, 16, 0, 0)));
  TEST_ASSERT_TRUE(has("Anniversary", civilToSeconds(2026, 10, 2, 0, 0, 0)));
  TEST_ASSERT_TRUE(has("On the 31st", civilToSeconds(2026, 10, 31, 12, 0, 0)));
  TEST_ASSERT_EQUAL_INT(1, countTitle("On the 31st"));                          // no Nov 31
  TEST_ASSERT_TRUE(has("Third Thursday of November", civilToSeconds(2026, 11, 19, 18, 0, 0)));
  // FREQ=HOURLY is unsupported: only its first instance is shown.
  TEST_ASSERT_EQUAL_INT(1, countTitle("Hourly (unsupported)"));
  TEST_ASSERT_TRUE(p->stats().unsupportedRules >= 1);
}

void test_outlook_style() {
  IcsOptions o = window48();
  parseAllChunkings("outlook.ics", o);
  IcsParser *p = P();
  TEST_ASSERT_EQUAL_INT(2, p->count());
  // Tab folding: exactly one whitespace char is removed.
  const Event *q = findTitle("Quarterly planning with the whole team");
  TEST_ASSERT_NOT_NULL(q);
  TEST_ASSERT_EQUAL_INT64(1790861400, q->start);                               // 14:30 BST
  TEST_ASSERT_EQUAL_INT64(90 * 60, q->end - q->start);
  TEST_ASSERT_EQUAL_STRING("Microsoft Teams Meeting", q->location);
  TEST_ASSERT_EQUAL_INT64(1790870400, findTitle("West coast sync")->start);    // 09:00 PDT
  TEST_ASSERT_EQUAL_INT(0, (int)p->stats().unknownTz);
}

void test_malformed_input_survives() {
  IcsOptions o = window48();
  parseAllChunkings("malformed.ics", o);
  IcsParser *p = P();
  TEST_ASSERT_NOT_NULL(findTitle("Survivor one"));
  TEST_ASSERT_NOT_NULL(findTitle("Survivor two"));
  const Event *s3 = nullptr;
  for (int i = 0; i < p->count(); i++)
    if (strncmp(p->event(i).title, "Survivor three", 14) == 0) s3 = &p->event(i);
  TEST_ASSERT_NOT_NULL(s3);
  TEST_ASSERT_TRUE(strlen(s3->title) < (size_t)kTitleMax);
  TEST_ASSERT_TRUE(p->stats().truncated >= 1);
  TEST_ASSERT_NULL(findTitle("Bad date format"));
  TEST_ASSERT_TRUE(p->stats().badDates >= 1);
  TEST_ASSERT_NULL(findTitle("inside nested junk"));
  // Lenient: an event after END:VCALENDAR and one cut off by EOF still count.
  TEST_ASSERT_NOT_NULL(findTitle("Event after END:VCALENDAR"));
  TEST_ASSERT_NOT_NULL(findTitle("No END line"));
}

void test_garbage_and_empty_streams() {
  IcsOptions o = window48();
  parse("", o);
  TEST_ASSERT_EQUAL_INT(0, P()->count());
  TEST_ASSERT_FALSE(P()->stats().sawCalendar);
  std::string html = "<!doctype html><html><body>Sign in</body></html>";
  parse(html, o);
  TEST_ASSERT_EQUAL_INT(0, P()->count());
  TEST_ASSERT_FALSE(P()->stats().sawCalendar);
  // Random bytes must never crash or produce events.
  std::string junk;
  uint32_t x = 12345;
  for (int i = 0; i < 200000; i++) { x = x * 1103515245u + 12345u; junk += (char)(x >> 16); }
  parse(junk, o, 333);
  TEST_ASSERT_EQUAL_INT(0, P()->count());
}

// A 3 MB feed: thousands of past events with huge descriptions, a few in the
// window, one daily series. Streams in 1460-byte "TCP segments".
void test_large_stream() {
  std::string s = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\n";
  char buf[256];
  std::string desc = "DESCRIPTION:";
  for (int i = 0; i < 30; i++) desc += "Lorem ipsum dolor sit amet, consectetur adipiscing elit. ";
  std::string folded;
  for (size_t i = 0; i < desc.size(); i += 74) {
    if (i) folded += "\r\n ";
    folded += desc.substr(i, 74);
  }
  int64_t base = civilToSeconds(2018, 1, 1, 9, 0, 0);
  int n = 0;
  while (s.size() < 3u * 1024u * 1024u) {
    int64_t st = base + (int64_t)n * 8 * H;
    Civil c = secondsToCivil(st);
    snprintf(buf, sizeof(buf),
             "BEGIN:VEVENT\r\nUID:bulk-%d\r\nDTSTART:%04d%02d%02dT%02d%02d00Z\r\n"
             "DURATION:PT30M\r\nSUMMARY:Bulk %d\r\n",
             n, c.year, c.month, c.day, c.hour, c.minute, n);
    s += buf;
    s += folded;
    s += "\r\nEND:VEVENT\r\n";
    n++;
  }
  s += "BEGIN:VEVENT\r\nUID:inwin\r\nDTSTART:20261001T120000Z\r\nDTEND:20261001T130000Z\r\n"
       "SUMMARY:Needle\r\nEND:VEVENT\r\n";
  s += "BEGIN:VEVENT\r\nUID:dly\r\nDTSTART:20190101T070000Z\r\nDTEND:20190101T071500Z\r\n"
       "RRULE:FREQ=DAILY\r\nSUMMARY:Since 2019\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
  IcsOptions o = window48();
  parse(s, o, 1460);
  IcsParser *p = P();
  TEST_ASSERT_TRUE(p->stats().bytes > 3u * 1024u * 1024u);
  TEST_ASSERT_NOT_NULL(findTitle("Needle"));
  // Oct 1 07:00-07:15Z ended before the window opened at 08:00Z; Oct 2 and
  // Oct 3 07:00Z are inside (the window closes Oct 3 08:00Z).
  TEST_ASSERT_EQUAL_INT(2, countTitle("Since 2019"));
  TEST_ASSERT_EQUAL_INT(3, p->count());
}

// The cap keeps the earliest events and drops the rest deterministically.
void test_cap_keeps_earliest() {
  std::string s = "BEGIN:VCALENDAR\r\n";
  char buf[200];
  for (int i = 59; i >= 0; i--) {     // reverse order on purpose
    int64_t st = NOW + 30 * 60 * (int64_t)i;
    Civil c = secondsToCivil(st);
    snprintf(buf, sizeof(buf),
             "BEGIN:VEVENT\r\nUID:c%d\r\nDTSTART:%04d%02d%02dT%02d%02d00Z\r\nDURATION:PT20M\r\n"
             "SUMMARY:E%02d\r\nEND:VEVENT\r\n",
             i, c.year, c.month, c.day, c.hour, c.minute, i);
    s += buf;
  }
  s += "END:VCALENDAR\r\n";
  IcsOptions o = window48();
  parse(s, o, 97);
  IcsParser *p = P();
  TEST_ASSERT_EQUAL_INT(16, p->count());
  for (int i = 0; i < 16; i++) {
    char t[8];
    snprintf(t, sizeof(t), "E%02d", i);
    TEST_ASSERT_EQUAL_STRING(t, p->event(i).title);
  }
}

void test_rrule_parser_units() {
  RRule r;
  TEST_ASSERT_TRUE(parseRRule("FREQ=WEEKLY;INTERVAL=2;BYDAY=MO,-1FR,2TU;UNTIL=20261231T235959Z;WKST=SU", r));
  TEST_ASSERT_EQUAL_INT(2, r.freq);
  TEST_ASSERT_EQUAL_INT(2, r.interval);
  TEST_ASSERT_EQUAL_INT(0, r.wkst);
  TEST_ASSERT_TRUE(r.hasUntil);
  TEST_ASSERT_TRUE(r.until.isUtc);
  // Weekly ignores ordinals: MO, FR and TU become plain days.
  TEST_ASSERT_EQUAL_HEX8((1 << 1) | (1 << 5) | (1 << 2), r.byDayMask);
  TEST_ASSERT_TRUE(parseRRule("FREQ=MONTHLY;BYDAY=-1SU;BYMONTH=3,10", r));
  TEST_ASSERT_EQUAL_INT(1, r.nByDayOrd);
  TEST_ASSERT_EQUAL_INT(-1, r.byDayOrd[0]);
  TEST_ASSERT_EQUAL_HEX16((1 << 3) | (1 << 10), r.byMonthMask);
  TEST_ASSERT_TRUE(parseRRule("FREQ=DAILY;BYHOUR=9", r));
  TEST_ASSERT_TRUE(r.unsupported);
  TEST_ASSERT_FALSE(parseRRule("INTERVAL=2", r));
}

// COUNT series that started decades ago are fast-forwarded arithmetically;
// expectations computed independently (Python datetime), incl. boundaries.
static int countSeries(const char *dtstart, const char *rrule) {
  std::string cal = "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:long\r\nSUMMARY:Long\r\nDTSTART:";
  cal += dtstart;
  cal += "\r\nDURATION:PT15M\r\nRRULE:";
  cal += rrule;
  cal += "\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
  IcsOptions o;
  o.windowStart = civilToSeconds(2026, 10, 5, 0, 0, 0);
  o.windowEnd = civilToSeconds(2026, 10, 8, 0, 0, 0);
  o.maxEvents = 16;
  parse(cal, o, 41);
  return P()->count();
}

void test_long_running_count_series() {
  TEST_ASSERT_EQUAL_INT(3, countSeries("19900101T090000Z", "FREQ=DAILY;COUNT=100000"));
  TEST_ASSERT_EQUAL_INT(0, countSeries("19900101T090000Z", "FREQ=DAILY;COUNT=13426"));   // ends Oct 4
  TEST_ASSERT_EQUAL_INT(1, countSeries("19900101T090000Z", "FREQ=DAILY;COUNT=13427"));   // ends Oct 5
  TEST_ASSERT_EQUAL_INT(1, countSeries("20100301T090000Z", "FREQ=DAILY;INTERVAL=3;COUNT=5000"));
  TEST_ASSERT_EQUAL_INT64(civilToSeconds(2026, 10, 6, 9, 0, 0), P()->event(0).start);
  TEST_ASSERT_EQUAL_INT(2, countSeries("20000103T090000Z", "FREQ=WEEKLY;BYDAY=MO,WE;COUNT=3000"));
  TEST_ASSERT_EQUAL_INT(0, countSeries("20000103T090000Z", "FREQ=WEEKLY;BYDAY=MO,WE;COUNT=2792"));
  TEST_ASSERT_EQUAL_INT(1, countSeries("20000103T090000Z", "FREQ=WEEKLY;BYDAY=MO,WE;COUNT=2793"));
  // DTSTART on a Thursday outside BYDAY still counts as the first instance.
  TEST_ASSERT_EQUAL_INT(2, countSeries("20150101T090000Z", "FREQ=WEEKLY;BYDAY=MO,WE;COUNT=5000"));
}

// Feeds come off the network: mutate every fixture (flip, insert, delete,
// truncate) and make sure the parser never misbehaves. Run under
// -fsanitize=address,undefined to catch memory errors as well.
void test_mutation_fuzz() {
  const char *files[] = {"basic_utc.ics", "tzid.ics", "dst.ics", "allday.ics", "rrule_daily.ics",
                         "rrule_weekly.ics", "outlook.ics", "monthly.ics", "malformed.ics"};
  uint32_t seed = 12345;
  auto rnd = [&]() { seed = seed * 1103515245u + 12345u; return (seed >> 8) & 0xFFFFFF; };
  static const char kInteresting[] = ":;=,\r\n \t\"\\TZID=RRULE:FREQ=WEEKLY;COUNT=1000000;BYDAY=-5MO,99XX";
  for (const char *f : files) {
    std::string base = loadFixture(f);
    for (int iter = 0; iter < 150; iter++) {
      std::string d = base;
      int muts = 1 + (int)(rnd() % 12);
      for (int m = 0; m < muts && !d.empty(); m++) {
        size_t pos = rnd() % d.size();
        switch (rnd() % 5) {
          case 0: d[pos] = (char)(rnd() & 0xFF); break;
          case 1: d.insert(pos, 1, kInteresting[rnd() % (sizeof(kInteresting) - 1)]); break;
          case 2: d.erase(pos, 1 + rnd() % 40); break;
          case 3: d.insert(pos, d.substr(rnd() % d.size(), rnd() % 200)); break;
          case 4: d.resize(pos); break;
        }
      }
      IcsOptions o = window48();
      parse(d, o, 1 + rnd() % 300);
      IcsParser *p = P();
      TEST_ASSERT_TRUE(p->count() <= o.maxEvents);
      for (int i = 0; i < p->count(); i++) {
        const Event &e = p->event(i);
        TEST_ASSERT_TRUE(strlen(e.title) < (size_t)kTitleMax);
        TEST_ASSERT_TRUE(strlen(e.location) < (size_t)kLocMax);
        TEST_ASSERT_TRUE(e.end >= e.start);
      }
    }
  }
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_time_basics);
  RUN_TEST(test_ics_datetime_and_duration);
  RUN_TEST(test_text_folding);
  RUN_TEST(test_basic_utc_folding_and_filtering);
  RUN_TEST(test_tzid_and_floating);
  RUN_TEST(test_vtimezone_dst_rules);
  RUN_TEST(test_recurrence_across_dst);
  RUN_TEST(test_all_day_events);
  RUN_TEST(test_rrule_daily);
  RUN_TEST(test_rrule_weekly_exdate_overrides);
  RUN_TEST(test_rrule_wkst);
  RUN_TEST(test_rrule_monthly_yearly);
  RUN_TEST(test_outlook_style);
  RUN_TEST(test_malformed_input_survives);
  RUN_TEST(test_garbage_and_empty_streams);
  RUN_TEST(test_large_stream);
  RUN_TEST(test_cap_keeps_earliest);
  RUN_TEST(test_rrule_parser_units);
  RUN_TEST(test_long_running_count_series);
  RUN_TEST(test_mutation_fuzz);
  return UNITY_END();
}
