#include "mc_console.h"
#include <Arduino.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <new>
#include <string.h>
#include <ctype.h>
#include "mc_app.h"
#include "mc_sync.h"
#include "mc_proto.h"
#include "mc_text.h"
#include "ics_parser.h"
#include "http_parse.h"
#include "event.h"
#include "model.h"

using namespace mc;

#ifndef MC_VERSION
#define MC_VERSION "1.0.0"
#endif

namespace mcconsole {

static char   gLine[512];
static size_t gLen = 0;
static bool   gOverflow = false;
static bool   gLastCr = false;
static bool   gIcsMode = false;
static int    gIcsLines = 0;
static IcsParser *gIcs = nullptr;

void begin() {
  Serial.setTimeout(10);
}

static bool eqi(const char *a, const char *b) {
  while (*a && *b) {
    if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return false;
    a++; b++;
  }
  return *a == *b;
}

// Splits off the first word; returns the rest (spaces skipped).
static char *word(char *s, char *out, size_t cap) {
  while (*s == ' ' || *s == '\t') s++;
  size_t n = 0;
  while (*s && *s != ' ' && *s != '\t') { if (n + 1 < cap) out[n++] = *s; s++; }
  out[n] = '\0';
  while (*s == ' ' || *s == '\t') s++;
  return s;
}

static const char *srcName(uint8_t s) {
  return s == (uint8_t)Source::Feed ? "feed" : s == (uint8_t)Source::Manual ? "web" : "usb";
}

static void printEvent(const mc::Event &e, int tz) {
  char a[32], b[32];
  if (isAllDay(e)) {
    Civil c1 = secondsToCivil(e.start), c2 = secondsToCivil(e.end);
    snprintf(a, sizeof(a), "%04d-%02d-%02d", c1.year, c1.month, c1.day);
    snprintf(b, sizeof(b), "%04d-%02d-%02d", c2.year, c2.month, c2.day);
  } else {
    formatIsoLocal(e.start, tz, a, sizeof(a));
    formatIsoLocal(e.end, tz, b, sizeof(b));
  }
  Serial.printf("EV %08lx %s %s %s %s%s%s\n", (unsigned long)e.key, a, b, srcName(e.source), e.title,
                e.location[0] ? " @ " : "", e.location);
}

static bool onOff(const char *v, bool &out) {
  if (eqi(v, "on") || eqi(v, "1") || eqi(v, "true") || eqi(v, "yes")) { out = true; return true; }
  if (eqi(v, "off") || eqi(v, "0") || eqi(v, "false") || eqi(v, "no")) { out = false; return true; }
  return false;
}

static void cmdSet(char *args) {
  char key[16], val[64];
  char *rest = word(args, key, sizeof(key));
  word(rest, val, sizeof(val));
  mcapp::Settings s = mcapp::settings();
  bool b;
  int v = atoi(val);
  if (eqi(key, "alerts") && onOff(val, b)) s.alertsOn = b;
  else if (eqi(key, "night") && onOff(val, b)) s.nightPause = b;
  else if (eqi(key, "autotz") && onOff(val, b)) s.autoTz = b;
  else if (eqi(key, "insecure") && onOff(val, b)) s.insecureTls = b;
  else if (eqi(key, "lead") && v >= 15 && v <= 180) s.leadMin = (uint8_t)v;
  else if (eqi(key, "sync") && v >= 0 && v <= 240 && isdigit((unsigned char)val[0])) s.syncMin = (uint8_t)v;
  else if (eqi(key, "snooze") && v >= 1 && v <= 30) s.snoozeMin = (uint8_t)v;
  else if (eqi(key, "face") && (eqi(val, "meeting") || eqi(val, "classic"))) s.faceClassic = eqi(val, "classic");
  else if (eqi(key, "offsets")) {
    uint8_t mask = 0;
    char *p = val;
    while (*p) {
      int m = (int)strtol(p, &p, 10);
      bool found = false;
      for (int i = 0; i < kNumAlertOffsets; i++) if (kAlertOffsets[i] == m) { mask |= (uint8_t)(1u << i); found = true; }
      if (!found) { Serial.printf("ERR offsets must be from 0,1,2,5,10,15,30\n"); return; }
      while (*p == ',' || *p == ' ') p++;
    }
    if (!mask) { Serial.println("ERR no offsets"); return; }
    s.alertMask = mask;
  } else {
    Serial.println("ERR usage: SET alerts|night|autotz|insecure on|off, lead|sync|snooze <min>, "
                   "face meeting|classic, offsets 5,1");
    return;
  }
  mcapp::setSettings(s);
  { ModelLock lk; model.revision++; }
  Serial.println("OK");
}

static void cmdStatus() {
  int64_t now = 0;
  bool haveNow = mcapp::nowUtc(now);
  int tz = mcapp::tzOffsetMin();
  char t[40];
  Serial.printf("KV version %s\n", MC_VERSION);
  if (haveNow) { formatIsoLocal(now, tz, t, sizeof(t)); Serial.printf("KV time %s\n", t); }
  else Serial.println("KV time unknown");
  Serial.printf("KV tz %+d\n", tz);
  mcapp::SyncInfo si = mcapp::syncInfo();
  for (int i = 0; i < mcapp::kMaxFeeds; i++) {
    char lbl[96];
    mcapp::feedLabel(i, lbl, sizeof(lbl));
    if (!lbl[0]) continue;
    Serial.printf("KV feed%d %s %s events=%u bytes=%lu\n", i, lbl,
                  mcapp::syncResultText((mcapp::SyncResult)si.feed[i].result), si.feed[i].events,
                  (unsigned long)si.feed[i].bytes);
  }
  Serial.printf("KV events feed=%d web=%d usb=%d\n", mcapp::countSource(Source::Feed),
                mcapp::countSource(Source::Manual), mcapp::countSource(Source::Pushed));
  char line[64];
  bool warn;
  mcapp::statusLine(now, line, sizeof(line), warn);
  Serial.printf("KV sync %s (last result: %s)\n", line[0] ? line : "-", mcapp::syncResultText(si.last));
  if (si.calTz[0]) Serial.printf("KV calendar %s tz=%s offset=%+d\n", si.calName, si.calTz, si.calTzOffset);
  AlertHit h;
  mc::Event e;
  if (mcapp::nextAlertInfo(h, e)) {
    formatIsoLocal(h.at, tz, t, sizeof(t));
    Serial.printf("KV alert %s %s %s\n", t, h.kind == AlertKind::Leave ? "leave" : h.kind == AlertKind::Snooze ? "snooze" : "before", e.title);
  }
  mcapp::Settings s = mcapp::settings();
  Serial.printf("KV settings alerts=%s mask=0x%02x lead=%u sync=%u night=%s autotz=%s snooze=%u face=%s\n",
                s.alertsOn ? "on" : "off", s.alertMask, s.leadMin, s.syncMin, s.nightPause ? "on" : "off",
                s.autoTz ? "on" : "off", s.snoozeMin, s.faceClassic ? "classic" : "meeting");
  Serial.printf("KV heap internal=%u psram=%u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  Serial.println("OK");
}

static void cmdSync() {
  if (!mcsync::startAsync() && !mcsync::running()) { Serial.println("ERR could not start"); return; }
  char last[48] = "";
  uint32_t t0 = millis();
  while (mcsync::running() && millis() - t0 < 100000) {
    mcsync::Progress p = mcsync::progress();
    if (strcmp(p.step, last) != 0) {
      Serial.printf("... %s\n", p.step);
      copyStr(last, sizeof(last), p.step);
    }
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  mcapp::SyncInfo si = mcapp::syncInfo();
  bool ok = si.last == mcapp::SyncResult::Ok || si.last == mcapp::SyncResult::Partial;
  Serial.printf("%s %s\n", ok ? "OK" : "ERR", mcapp::syncResultText(si.last));
}

static void handleLine(char *line) {
  char cmd[12];
  char *args = word(line, cmd, sizeof(cmd));
  int tz = mcapp::tzOffsetMin();
  if (!cmd[0]) return;
  if (eqi(cmd, "HELLO")) {
    Serial.printf("OK meeting-countdown %s proto=1 tz=%+d\n", MC_VERSION, tz);
  } else if (eqi(cmd, "HELP")) {
    Serial.println("HELLO | TIME | EVENT <start> <end|+dur> [leave=N] [loc=X] <title> | DEL <id> | LIST |");
    Serial.println("CLEAR [ALL] | ICS BEGIN ... ICS END | FEED [n url|n CLEAR] | SYNC | STATUS |");
    Serial.println("SET <key> <value> | ALERT TEST");
    Serial.println("OK");
  } else if (eqi(cmd, "TIME")) {
    int64_t now;
    if (!mcapp::nowUtc(now)) { Serial.println("ERR clock not set"); return; }
    char t[40];
    formatIsoLocal(now, tz, t, sizeof(t));
    Serial.printf("OK %s\n", t);
  } else if (eqi(cmd, "EVENT")) {
    mc::Event e;
    const char *err = "";
    if (!parseEventArgs(args, tz, Source::Pushed, e, &err)) { Serial.printf("ERR %s\n", err); return; }
    if (!mcapp::addEvent(e, &err)) { Serial.printf("ERR %s\n", err); return; }
    { ModelLock lk; model.revision++; }
    Serial.printf("OK id=%08lx\n", (unsigned long)e.key);
  } else if (eqi(cmd, "DEL")) {
    uint32_t key = (uint32_t)strtoul(args, nullptr, 16);
    if (!mcapp::deleteEvent(key)) { Serial.println("ERR no such event (feed events cannot be deleted)"); return; }
    { ModelLock lk; model.revision++; }
    Serial.println("OK");
  } else if (eqi(cmd, "LIST")) {
    static mc::Event *list = nullptr;
    if (!list) list = mcapp::allocEvents(mcapp::kMaxMerged);
    int n = list ? mcapp::snapshot(list, mcapp::kMaxMerged) : 0;
    for (int i = 0; i < n; i++) printEvent(list[i], tz);
    Serial.printf("OK n=%d\n", n);
  } else if (eqi(cmd, "CLEAR")) {
    int n = mcapp::clearSource(Source::Pushed);
    if (eqi(args, "ALL")) {
      n += mcapp::clearSource(Source::Manual);
      n += mcapp::clearSource(Source::Feed);
    }
    { ModelLock lk; model.revision++; }
    Serial.printf("OK cleared=%d\n", n);
  } else if (eqi(cmd, "ICS")) {
    if (!eqi(args, "BEGIN")) { Serial.println("ERR use ICS BEGIN"); return; }
    if (!gIcs) {
      void *mem = heap_caps_malloc(sizeof(IcsParser), MALLOC_CAP_SPIRAM);
      if (!mem) mem = malloc(sizeof(IcsParser));
      if (!mem) { Serial.println("ERR out of memory"); return; }
      gIcs = new (mem) IcsParser();
    }
    int64_t now = 0;
    mcapp::nowUtc(now);
    IcsOptions o;
    o.windowStart = now;
    o.windowEnd = now + 7 * 86400;
    o.fallbackOffsetMin = tz;
    o.source = (uint8_t)Source::Pushed;
    o.maxEvents = mcapp::kMaxPushed;
    o.maxAllDay = 4;
    gIcs->begin(o);
    gIcsMode = true;
    gIcsLines = 0;
    Serial.println("OK send lines, finish with ICS END");
  } else if (eqi(cmd, "FEED")) {
    char nb[8];
    char *rest = word(args, nb, sizeof(nb));
    if (!nb[0]) {
      for (int i = 0; i < mcapp::kMaxFeeds; i++) {
        char lbl[96];
        mcapp::feedLabel(i, lbl, sizeof(lbl));
        Serial.printf("FEED %d %s\n", i + 1, lbl[0] ? lbl : "-");
      }
      Serial.println("OK");
      return;
    }
    int idx = atoi(nb) - 1;
    if (idx < 0 || idx >= mcapp::kMaxFeeds || !rest[0]) { Serial.println("ERR usage: FEED <1-3> <url>|CLEAR"); return; }
    if (eqi(rest, "CLEAR")) { mcapp::setFeedUrl(idx, ""); Serial.println("OK"); return; }
    Url u;
    if (strlen(rest) >= (size_t)mcapp::kUrlMax || !parseUrl(rest, u)) { Serial.println("ERR bad URL"); return; }
    mcapp::setFeedUrl(idx, rest);
    Serial.println("OK (syncs on SYNC or at the next sleep)");
  } else if (eqi(cmd, "SYNC")) {
    cmdSync();
  } else if (eqi(cmd, "STATUS")) {
    cmdStatus();
  } else if (eqi(cmd, "SET")) {
    cmdSet(args);
  } else if (eqi(cmd, "ALERT")) {
    mcapp::startTestAlert();
    ::Event ev = ::makeEvent(EventType::TimerExpired);
    postEvent(ev);
    Serial.println("OK");
  } else {
    Serial.printf("ERR unknown command %s (try HELP)\n", cmd);
  }
}

static void icsLine(char *line, size_t len, bool complete) {
  if (complete && len == 7 && eqi(line, "ICS END")) {
    gIcsMode = false;
    gIcs->finish();
    if (!gIcs->stats().sawCalendar) { Serial.println("ERR no BEGIN:VCALENDAR seen"); return; }
    int n = gIcs->count() > mcapp::kMaxPushed ? mcapp::kMaxPushed : gIcs->count();
    mcapp::replaceSource(Source::Pushed, n ? &gIcs->event(0) : nullptr, n);
    { ModelLock lk; model.revision++; }
    Serial.printf("OK events=%d vevents=%u\n", n, gIcs->stats().events);
    return;
  }
  gIcs->feed(line, len);
  if (complete) {
    gIcs->feed("\r\n", 2);
    if (++gIcsLines % 32 == 0) Serial.println("ACK");
  }
}

void poll() {
  int budget = 2048;                         // bytes per call, keeps loop() responsive
  while (budget-- > 0 && Serial.available() > 0) {
    int c = Serial.read();
    if (c < 0) break;
    mcapp::keepAwakeFor(90000);
    if (c == '\n' && gLastCr) { gLastCr = false; continue; }
    gLastCr = c == '\r';
    if (c == '\r' || c == '\n') {
      gLine[gLen] = '\0';
      if (gIcsMode) {
        icsLine(gLine, gLen, true);
      } else if (gOverflow) {
        Serial.println("ERR line too long");
      } else {
        handleLine(gLine);
      }
      gLen = 0;
      gOverflow = false;
      continue;
    }
    if (gLen < sizeof(gLine) - 1) {
      gLine[gLen++] = (char)c;
    } else if (gIcsMode) {
      icsLine(gLine, gLen, false);           // very long ICS line: pass it through in pieces
      gLen = 0;
      gLine[gLen++] = (char)c;
    } else {
      gOverflow = true;
    }
  }
  mcapp::flushPending();
}

}  // namespace mcconsole
