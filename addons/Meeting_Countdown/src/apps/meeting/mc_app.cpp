// Meeting Countdown — device state, persistence and planning glue.
#include <Arduino.h>
#include <Preferences.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>
#include "mc_app.h"
#include "mc_text.h"
#include "http_parse.h"
#include "model.h"
#include "storage.h"
#include "apptimer.h"

using namespace mc;

namespace mcapp {

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------
static const char *NS = "mtgcount";            // own NVS namespace (<= 15 chars)
static const uint8_t kBlobVer = 1;

static SemaphoreHandle_t gMx = nullptr;         // guards everything below
static SemaphoreHandle_t gNvsMx = nullptr;      // guards gPrefs
struct Lock {
  Lock() { xSemaphoreTakeRecursive(gMx, portMAX_DELAY); }
  ~Lock() { xSemaphoreGiveRecursive(gMx); }
};

static Settings gSet;
static char     gFeeds[kMaxFeeds][kUrlMax];
static Event    gFeedEv[kMaxFeedEv];  static int gNFeed = 0;
static Event    gManual[kMaxManual];  static int gNManual = 0;
static Event    gPushed[kMaxPushed];  static int gNPushed = 0;
static SyncInfo gSync;
static ActiveAlert gAlert;
static Preferences gPrefs;
static bool     gPrefsOk = false;
static volatile uint32_t gKeepAwakeUntil = 0;
static bool     gPushedDirty = false;
static uint32_t gPushedDirtyAt = 0;

// Survives deep sleep. (The bootloader re-initialises RTC_DATA_ATTR on every
// reset that is not a deep-sleep wake, panics included; losing this state
// then is harmless: it is re-derived from NVS.)
struct RtcState {
  uint32_t magic;
  int64_t  watermark;          // alerts at or before this instant are handled
  Snooze   snooze;
  uint8_t  syncRequested;
  char     apSsid[33];
  uint8_t  apBssid[6];
  uint8_t  apChannel;
  int64_t  plannedWake;
  uint8_t  plannedReason;
};
RTC_DATA_ATTR static RtcState gRtc;
static const uint32_t kRtcMagic = 0x4D43544Eu;
static const int64_t kUnset = INT64_MIN;

// The sync crash guard must survive exactly the reset RTC_DATA_ATTR does not
// (a panic), so it lives in RTC_NOINIT memory behind its own magic.
struct CrashGuard {
  uint32_t magic;
  uint8_t  syncActive;         // set while a sync runs
  uint8_t  syncCrashes;        // consecutive crashes during a sync
  uint8_t  bgPaused;           // background sync paused after repeated crashes
  uint8_t  pad;
};
RTC_NOINIT_ATTR static CrashGuard gGuard;
static const uint32_t kGuardMagic = 0x4D434721u;
static void guardInit() {
  if (gGuard.magic != kGuardMagic) {
    memset((void *)&gGuard, 0, sizeof(gGuard));
    gGuard.magic = kGuardMagic;
  }
}

// ---------------------------------------------------------------------------
// NVS helpers
// ---------------------------------------------------------------------------
struct BlobHdr { uint8_t ver; uint8_t count; uint16_t evSize; };

static bool nvsBegin() {
  if (gPrefsOk) return true;
  gPrefsOk = gPrefs.begin(NS, false);
  if (!gPrefsOk) Serial.println("MC: NVS namespace open failed");
  return gPrefsOk;
}

static void saveEvents(const char *key, const Event *ev, int n) {
  if (xSemaphoreTake(gNvsMx, portMAX_DELAY) != pdTRUE) return;
  if (nvsBegin()) {
    size_t sz = sizeof(BlobHdr) + (size_t)n * sizeof(Event);
    uint8_t *buf = (uint8_t *)malloc(sz);
    if (buf) {
      BlobHdr h = {kBlobVer, (uint8_t)n, (uint16_t)sizeof(Event)};
      memcpy(buf, &h, sizeof(h));
      if (n) memcpy(buf + sizeof(h), ev, (size_t)n * sizeof(Event));
      if (gPrefs.putBytes(key, buf, sz) != sz) Serial.printf("MC: NVS write %s failed\n", key);
      free(buf);
    }
  }
  xSemaphoreGive(gNvsMx);
}

static int loadEvents(const char *key, Event *ev, int cap) {
  int n = 0;
  if (xSemaphoreTake(gNvsMx, portMAX_DELAY) != pdTRUE) return 0;
  if (nvsBegin()) {
    size_t sz = gPrefs.isKey(key) ? gPrefs.getBytesLength(key) : 0;
    if (sz >= sizeof(BlobHdr)) {
      uint8_t *buf = (uint8_t *)malloc(sz);
      if (buf && gPrefs.getBytes(key, buf, sz) == sz) {
        BlobHdr h;
        memcpy(&h, buf, sizeof(h));
        if (h.ver == kBlobVer && h.evSize == sizeof(Event) &&
            sz == sizeof(BlobHdr) + (size_t)h.count * sizeof(Event)) {
          n = h.count > cap ? cap : h.count;
          memcpy(ev, buf + sizeof(h), (size_t)n * sizeof(Event));
          for (int i = 0; i < n; i++) {                 // defensive: terminate strings
            ev[i].title[kTitleMax - 1] = '\0';
            ev[i].location[kLocMax - 1] = '\0';
          }
        }
      }
      free(buf);
    }
  }
  xSemaphoreGive(gNvsMx);
  return n;
}

static void saveSettings() {
  Settings s;
  { Lock lk; s = gSet; }
  if (xSemaphoreTake(gNvsMx, portMAX_DELAY) != pdTRUE) return;
  if (nvsBegin()) gPrefs.putBytes("cfg", &s, sizeof(s));
  xSemaphoreGive(gNvsMx);
}

static void saveSync() {
  SyncInfo s;
  { Lock lk; s = gSync; }
  if (xSemaphoreTake(gNvsMx, portMAX_DELAY) != pdTRUE) return;
  if (nvsBegin()) gPrefs.putBytes("sync", &s, sizeof(s));
  xSemaphoreGive(gNvsMx);
}

static void saveFeedUrl(int i) {
  char key[8];
  snprintf(key, sizeof(key), "feed%d", i);
  char url[kUrlMax];
  { Lock lk; copyStr(url, sizeof(url), gFeeds[i]); }
  if (xSemaphoreTake(gNvsMx, portMAX_DELAY) != pdTRUE) return;
  if (nvsBegin()) {
    if (url[0]) gPrefs.putString(key, url);
    else gPrefs.remove(key);
  }
  xSemaphoreGive(gNvsMx);
}

static void flushPushed() {
  // Heap copy: this runs on the render task's small stack from the sleep hook.
  Event *copy = (Event *)malloc(sizeof(Event) * kMaxPushed);
  if (!copy) return;
  int n;
  {
    Lock lk;
    if (!gPushedDirty) { free(copy); return; }
    n = gNPushed;
    memcpy(copy, gPushed, sizeof(Event) * (size_t)n);
    gPushedDirty = false;
  }
  saveEvents("evP", copy, n);
  free(copy);
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------
Event *allocEvents(int n) {
  void *p = heap_caps_calloc((size_t)n, sizeof(Event), MALLOC_CAP_SPIRAM);
  if (!p) p = calloc((size_t)n, sizeof(Event));
  return (Event *)p;
}

void init() {
  if (!gMx) gMx = xSemaphoreCreateRecursiveMutex();
  if (!gNvsMx) gNvsMx = xSemaphoreCreateMutex();
  guardInit();
  if (gRtc.magic != kRtcMagic) {
    memset((void *)&gRtc, 0, sizeof(gRtc));
    gRtc.magic = kRtcMagic;
    gRtc.watermark = kUnset;
  }
  Settings s;
  bool haveCfg = false;
  if (xSemaphoreTake(gNvsMx, portMAX_DELAY) == pdTRUE) {
    if (nvsBegin()) {
      if (gPrefs.isKey("cfg") && gPrefs.getBytesLength("cfg") == sizeof(Settings)) {
        gPrefs.getBytes("cfg", &s, sizeof(s));
        haveCfg = s.version == Settings().version;
      }
      for (int i = 0; i < kMaxFeeds; i++) {
        char key[8];
        snprintf(key, sizeof(key), "feed%d", i);
        if (gPrefs.isKey(key)) {
          String u = gPrefs.getString(key, "");
          copyStr(gFeeds[i], kUrlMax, u.c_str());
        } else {
          gFeeds[i][0] = '\0';
        }
      }
      if (gPrefs.isKey("sync") && gPrefs.getBytesLength("sync") == sizeof(SyncInfo))
        gPrefs.getBytes("sync", &gSync, sizeof(gSync));
    }
    xSemaphoreGive(gNvsMx);
  }
  if (!haveCfg) s = Settings();
  // Sanity-clamp anything that came from flash.
  if (s.leadMin < 15 || s.leadMin > 180) s.leadMin = 60;
  if (s.syncMin > 240) s.syncMin = 30;
  if (s.snoozeMin < 1 || s.snoozeMin > 30) s.snoozeMin = 3;
  gSet = s;
  gNFeed = loadEvents("evF", gFeedEv, kMaxFeedEv);
  gNManual = loadEvents("evM", gManual, kMaxManual);
  gNPushed = loadEvents("evP", gPushed, kMaxPushed);
  Serial.printf("MC: init feeds=%d cached=%d manual=%d pushed=%d lead=%u sync=%u\n", feedCount(),
                gNFeed, gNManual, gNPushed, gSet.leadMin, gSet.syncMin);
}

void noteResetReason(bool crashed) {
  guardInit();
  if (crashed && gGuard.syncActive) {
    if (gGuard.syncCrashes < 255) gGuard.syncCrashes++;
    Serial.printf("MC: crash during sync (%u in a row)\n", gGuard.syncCrashes);
    if (gGuard.syncCrashes >= 2) gGuard.bgPaused = 1;
  }
  gGuard.syncActive = 0;
}

// ---------------------------------------------------------------------------
// time
// ---------------------------------------------------------------------------
int tzOffsetMin() {
  ModelLock lk;
  return model.tzOffsetMin;
}

bool nowUtc(int64_t &utc) {
  uint16_t y; uint8_t mo, d, h, mi, s; int16_t tz; bool ok;
  { ModelLock lk;
    ok = model.rtcOk; y = model.year; mo = model.month; d = model.day;
    h = model.hour; mi = model.minute; s = model.second; tz = model.tzOffsetMin; }
  if (!ok || y < 2020) return false;
  utc = rtcToUtc(rtcEpochSec(y, mo, d, h, mi, s), tz);
  return true;
}

// ---------------------------------------------------------------------------
// settings / feeds
// ---------------------------------------------------------------------------
Settings settings() { Lock lk; return gSet; }

void setSettings(const Settings &s) {
  { Lock lk; gSet = s; gSet.version = Settings().version; }
  saveSettings();
  armAwakeTimer();
}

int feedCount() {
  Lock lk;
  int n = 0;
  for (int i = 0; i < kMaxFeeds; i++) if (gFeeds[i][0]) n++;
  return n;
}

bool feedUrl(int i, char *out, size_t cap) {
  if (i < 0 || i >= kMaxFeeds) return false;
  Lock lk;
  copyStr(out, cap, gFeeds[i]);
  return out[0] != '\0';
}

void setFeedUrl(int i, const char *url) {
  if (i < 0 || i >= kMaxFeeds) return;
  char clean[kUrlMax];
  copyStr(clean, sizeof(clean), url ? url : "");
  // trim
  size_t n = strlen(clean);
  while (n && (clean[n - 1] == ' ' || clean[n - 1] == '\r' || clean[n - 1] == '\n')) clean[--n] = '\0';
  char *p = clean;
  while (*p == ' ') p++;
  {
    Lock lk;
    copyStr(gFeeds[i], kUrlMax, p);
    if (!p[0]) {
      // Forget this feed's cached events too.
      int w = 0;
      for (int k = 0; k < gNFeed; k++) if (gFeedEv[k].feed != i) gFeedEv[w++] = gFeedEv[k];
      gNFeed = w;
      gSync.feed[i] = FeedStatus();
    }
  }
  saveFeedUrl(i);
  if (!p[0]) {
    Event copy[kMaxFeedEv];
    int nn;
    { Lock lk; nn = gNFeed; memcpy(copy, gFeedEv, sizeof(Event) * (size_t)nn); }
    saveEvents("evF", copy, nn);
    saveSync();
  } else {
    requestSync();
  }
  armAwakeTimer();
}

void feedLabel(int i, char *out, size_t cap) {
  char url[kUrlMax];
  if (!feedUrl(i, url, sizeof(url))) { copyStr(out, cap, ""); return; }
  redactUrl(url, out, cap);
}

// ---------------------------------------------------------------------------
// events
// ---------------------------------------------------------------------------
int snapshot(Event *out, int cap) {
  int64_t now;
  int64_t prune = nowUtc(now) ? now : INT64_MIN;
  int tz = tzOffsetMin();
  Lock lk;
  const Event *lists[3] = {gManual, gPushed, gFeedEv};
  int counts[3] = {gNManual, gNPushed, gNFeed};
  return mergeEvents(lists, counts, 3, prune, tz, out, cap);
}

int countSource(Source s) {
  Lock lk;
  return s == Source::Feed ? gNFeed : s == Source::Manual ? gNManual : gNPushed;
}

// Drop finished events from a stored list (keeps storage small).
static int pruneList(Event *ev, int n, int64_t now, int tz) {
  int w = 0;
  for (int i = 0; i < n; i++) {
    int64_t endUtc = isAllDay(ev[i]) ? ev[i].end - (int64_t)tz * 60 : ev[i].end;
    if (endUtc > now) ev[w++] = ev[i];
  }
  return w;
}

bool addEvent(const Event &e, const char **err) {
  static const char *kFull = "list is full (delete something first)";
  int64_t now;
  bool haveNow = nowUtc(now);
  int tz = tzOffsetMin();
  if (e.source == (uint8_t)Source::Manual) {
    Event copy[kMaxManual];
    int n;
    {
      Lock lk;
      if (haveNow) gNManual = pruneList(gManual, gNManual, now, tz);
      for (int i = 0; i < gNManual; i++) if (gManual[i].key == e.key) { if (err) *err = "already added"; return false; }
      if (gNManual >= kMaxManual) { if (err) *err = kFull; return false; }
      gManual[gNManual++] = e;
      sortEvents(gManual, gNManual);
      n = gNManual;
      memcpy(copy, gManual, sizeof(Event) * (size_t)n);
    }
    saveEvents("evM", copy, n);
  } else {
    Lock lk;
    if (haveNow) gNPushed = pruneList(gPushed, gNPushed, now, tz);
    for (int i = 0; i < gNPushed; i++) {
      if (gPushed[i].key == e.key) { gPushed[i] = e; gPushedDirty = true; gPushedDirtyAt = millis(); return true; }
    }
    if (gNPushed >= kMaxPushed) { if (err) *err = kFull; return false; }
    gPushed[gNPushed++] = e;
    sortEvents(gPushed, gNPushed);
    gPushedDirty = true;
    gPushedDirtyAt = millis();
  }
  armAwakeTimer();
  return true;
}

bool deleteEvent(uint32_t key) {
  bool hitM = false, hitP = false;
  Event copy[kMaxManual];
  int n = 0;
  {
    Lock lk;
    int w = 0;
    for (int i = 0; i < gNManual; i++) { if (gManual[i].key == key) { hitM = true; continue; } gManual[w++] = gManual[i]; }
    gNManual = w;
    w = 0;
    for (int i = 0; i < gNPushed; i++) { if (gPushed[i].key == key) { hitP = true; continue; } gPushed[w++] = gPushed[i]; }
    gNPushed = w;
    if (hitP) { gPushedDirty = true; gPushedDirtyAt = millis(); }
    n = gNManual;
    memcpy(copy, gManual, sizeof(Event) * (size_t)n);
  }
  if (hitM) saveEvents("evM", copy, n);
  if (hitP) flushPushed();
  if (hitM || hitP) armAwakeTimer();
  return hitM || hitP;
}

int clearSource(Source s) {
  int n = 0;
  {
    Lock lk;
    if (s == Source::Manual) { n = gNManual; gNManual = 0; }
    else if (s == Source::Pushed) { n = gNPushed; gNPushed = 0; gPushedDirty = true; }
    else { n = gNFeed; gNFeed = 0; }
  }
  if (s == Source::Manual) saveEvents("evM", nullptr, 0);
  else if (s == Source::Pushed) flushPushed();
  else saveEvents("evF", nullptr, 0);
  armAwakeTimer();
  return n;
}

void replaceSource(Source s, const Event *ev, int n) {
  if (s != Source::Pushed) return;
  {
    Lock lk;
    gNPushed = n > kMaxPushed ? kMaxPushed : n;
    for (int i = 0; i < gNPushed; i++) { gPushed[i] = ev[i]; gPushed[i].source = (uint8_t)Source::Pushed; }
    sortEvents(gPushed, gNPushed);
    gPushedDirty = true;
  }
  flushPushed();
  armAwakeTimer();
}

static bool sameEvent(const Event &a, const Event &b) {
  return a.start == b.start && a.end == b.end && a.uid == b.uid && a.key == b.key &&
         a.flags == b.flags && a.source == b.source && a.leaveMin == b.leaveMin &&
         a.feed == b.feed && strncmp(a.title, b.title, kTitleMax) == 0 &&
         strncmp(a.location, b.location, kLocMax) == 0;
}

void applyFeedResults(const Event *ev, int n, const bool feedOk[kMaxFeeds]) {
  int64_t now;
  bool haveNow = nowUtc(now);
  int tz = tzOffsetMin();
  int cap = n + kMaxFeedEv;
  Event *pool = (Event *)heap_caps_malloc(sizeof(Event) * (size_t)cap, MALLOC_CAP_SPIRAM);
  if (!pool) pool = (Event *)malloc(sizeof(Event) * (size_t)cap);
  if (!pool) return;
  Event copy[kMaxFeedEv];
  int nc = 0;
  bool changed = false;
  {
    Lock lk;
    int m = 0;
    for (int i = 0; i < n; i++) pool[m++] = ev[i];
    // Feeds that failed this time keep their previous events.
    for (int i = 0; i < gNFeed; i++) {
      int f = gFeedEv[i].feed;
      if (f >= 0 && f < kMaxFeeds && !feedOk[f] && gFeeds[f][0]) pool[m++] = gFeedEv[i];
    }
    const Event *lists[1] = {pool};
    int counts[1] = {m};
    int k = mergeEvents(lists, counts, 1, haveNow ? now : INT64_MIN, tz, copy, kMaxFeedEv);
    changed = k != gNFeed;
    for (int i = 0; i < k && !changed; i++) changed = !sameEvent(copy[i], gFeedEv[i]);
    if (changed) {
      memcpy(gFeedEv, copy, sizeof(Event) * (size_t)k);
      gNFeed = k;
    }
    nc = gNFeed;
    memcpy(copy, gFeedEv, sizeof(Event) * (size_t)nc);
  }
  free(pool);
  if (changed) saveEvents("evF", copy, nc);
  Serial.printf("MC: feed cache %d events (%s)\n", nc, changed ? "changed" : "unchanged");
  armAwakeTimer();
}

// ---------------------------------------------------------------------------
// sync status
// ---------------------------------------------------------------------------
const char *syncResultText(SyncResult r) {
  switch (r) {
    case SyncResult::Never:         return "never synced";
    case SyncResult::Ok:            return "ok";
    case SyncResult::NoFeed:        return "no calendar feed";
    case SyncResult::NoWifi:        return "no WiFi network saved";
    case SyncResult::WifiFailed:    return "WiFi not in range";
    case SyncResult::DnsFailed:     return "server not found";
    case SyncResult::ConnectFailed: return "server unreachable";
    case SyncResult::TlsFailed:     return "secure connection failed";
    case SyncResult::HttpError:     return "server refused";
    case SyncResult::BadData:       return "not a calendar";
    case SyncResult::Timeout:       return "timed out";
    case SyncResult::LowMemory:     return "low memory";
    case SyncResult::Aborted:       return "interrupted";
    case SyncResult::LowBattery:    return "battery low";
    case SyncResult::Partial:       return "some feeds failed";
  }
  return "?";
}

SyncInfo syncInfo() { Lock lk; return gSync; }

void recordSync(const SyncInfo &info) {
  { Lock lk; gSync = info; }
  saveSync();
}

void requestSync() { gRtc.syncRequested = 1; }
bool syncRequested() { return gRtc.syncRequested != 0; }
void clearSyncRequest() { gRtc.syncRequested = 0; }

bool backgroundSyncAllowed(int64_t now, int batPct) {
  (void)now;
  // Background sync runs only on the headless (screen-off) wake path; without
  // it a planned sync wake would boot the UI and re-plan the same sync again.
#if !defined(MC_BACKGROUND_SYNC) || !MC_BACKGROUND_SYNC || !defined(MC_HEADLESS_WAKE) || \
    !MC_HEADLESS_WAKE || !defined(EWATCH_ENABLE_WIFI) || !EWATCH_ENABLE_WIFI
  (void)batPct;
  return false;
#else
  guardInit();
  if (gGuard.bgPaused) return false;
  if (feedCount() == 0) return false;
  if (Storage::knownCount() == 0) return false;
  Settings s = settings();
  if (s.syncMin == 0 && !gRtc.syncRequested) return false;
  if (batPct >= 0 && batPct < 12) return false;
  return true;
#endif
}

int64_t nextSyncAt(int64_t now) {
  if (gRtc.syncRequested) return now;
  SyncInfo si = syncInfo();
  Settings s = settings();
  SyncState st;
  st.lastAttempt = si.lastAttempt;
  st.lastSuccess = si.lastSuccess;
  st.failures = si.failures;
  return nextSyncTime(st, s.syncMin, s.nightPause != 0, tzOffsetMin(), now);
}

void statusLine(int64_t now, char *buf, size_t n, bool &warn) {
  warn = false;
  buf[0] = '\0';
  if (feedCount() == 0) return;
  SyncInfo si = syncInfo();
  if (Storage::knownCount() == 0) {
    snprintf(buf, n, "no WiFi saved for sync");
    warn = true;
    return;
  }
  if (gGuard.magic == kGuardMagic && gGuard.bgPaused) {
    snprintf(buf, n, "auto sync paused - sync manually");
    warn = true;
    return;
  }
  if (si.lastSuccess == 0) {
    if (si.last == SyncResult::Never || si.last == SyncResult::Aborted) snprintf(buf, n, "waiting for first sync");
    else { snprintf(buf, n, "sync failed: %s", syncResultText(si.last)); warn = true; }
    return;
  }
  char ago[24];
  fmtAgo(now - si.lastSuccess, ago, sizeof(ago));
  if (si.last == SyncResult::Ok || si.last == SyncResult::Aborted) {
    snprintf(buf, n, "synced %s", ago);
  } else {
    snprintf(buf, n, "offline - synced %s", ago);
    warn = now - si.lastSuccess > 3 * 3600;
  }
}

void rememberAp(const char *ssid, const uint8_t *bssid, uint8_t channel) {
  copyStr(gRtc.apSsid, sizeof(gRtc.apSsid), ssid);
  if (bssid) memcpy(gRtc.apBssid, bssid, 6);
  gRtc.apChannel = channel;
}

bool lastAp(char *ssid, uint8_t *bssid, uint8_t &channel) {
  if (!gRtc.apSsid[0] || !gRtc.apChannel) return false;
  copyStr(ssid, 33, gRtc.apSsid);
  memcpy(bssid, gRtc.apBssid, 6);
  channel = gRtc.apChannel;
  return true;
}

void markSyncActive(bool active) {
  guardInit();
  gGuard.syncActive = active ? 1 : 0;
  if (!active) { gGuard.syncCrashes = 0; gGuard.bgPaused = 0; }
}

// ---------------------------------------------------------------------------
// alerts
// ---------------------------------------------------------------------------
static Event *gTmp = nullptr;           // scratch (kMaxMerged), only touched under Lock

static AlertSettings alertSettingsLocked() {
  AlertSettings a;
  a.enabled = gSet.alertsOn != 0;
  a.offsetMask = gSet.alertMask;
  return a;
}

static int mergedLocked(int64_t pruneBefore, int tz) {
  if (!gTmp) gTmp = allocEvents(kMaxMerged);
  if (!gTmp) return 0;
  const Event *lists[3] = {gManual, gPushed, gFeedEv};
  int counts[3] = {gNManual, gNPushed, gNFeed};
  return mergeEvents(lists, counts, 3, pruneBefore, tz, gTmp, kMaxMerged);
}

void armAwakeTimer() {
  int64_t now;
  if (!nowUtc(now)) return;
  int tz = tzOffsetMin();
  Lock lk;
  if (gRtc.watermark == kUnset) gRtc.watermark = now;
  int n = mergedLocked(INT64_MIN, tz);
  AlertHit h;
  if (nextAlert(gTmp, n, alertSettingsLocked(), gRtc.snooze, gRtc.watermark, h)) {
    int64_t d = h.at - now;
    if (d < 1) d = 1;
    timerArm((uint32_t)utcToRtc(now, tz), (uint32_t)d);
  } else {
    timerCancel();
  }
}

bool onTimerExpired() {
  int64_t now;
  if (!nowUtc(now)) return false;
  int tz = tzOffsetMin();
  bool show = false;
  {
    Lock lk;
    if (gRtc.watermark == kUnset) gRtc.watermark = now - 1;
    int n = mergedLocked(INT64_MIN, tz);
    AlertHit hits[4];
    int k = dueAlerts(gTmp, n, alertSettingsLocked(), gRtc.snooze, gRtc.watermark, now, hits, 4);
    if (gRtc.snooze.active && gRtc.snooze.until <= now) gRtc.snooze.active = false;
    gRtc.watermark = now;
    if (k > 0) {
      gAlert.active = true;
      gAlert.ev = gTmp[hits[0].idx];
      gAlert.kind = hits[0].kind;
      gAlert.more = k - 1;
      gAlert.test = false;
      show = true;
      Serial.printf("MC: alert \"%s\" kind=%d (+%d)\n", gAlert.ev.title, (int)gAlert.kind, k - 1);
    }
  }
  armAwakeTimer();
  return show;
}

ActiveAlert activeAlert() { Lock lk; return gAlert; }

void snoozeActive() {
  int64_t now;
  bool haveNow = nowUtc(now);
  {
    Lock lk;
    if (!gAlert.active) return;
    if (!gAlert.test && haveNow) {
      gRtc.snooze.active = true;
      gRtc.snooze.key = gAlert.ev.key;
      gRtc.snooze.until = now + (int64_t)gSet.snoozeMin * 60;
    }
    gAlert.active = false;
  }
  armAwakeTimer();
}

void dismissActive() {
  {
    Lock lk;
    if (gAlert.active && gRtc.snooze.active && gRtc.snooze.key == gAlert.ev.key) gRtc.snooze.active = false;
    gAlert.active = false;
  }
  armAwakeTimer();
}

void startTestAlert() {
  int64_t now;
  if (!nowUtc(now)) now = 0;
  int tz = tzOffsetMin();
  Lock lk;
  int n = mergedLocked(now, tz);
  FaceInfo fi = computeFace(gTmp, n, now, tz, gSet.leadMin);
  gAlert = ActiveAlert();
  if (fi.next >= 0) {
    gAlert.ev = gTmp[fi.next];
  } else {
    memset((void *)&gAlert.ev, 0, sizeof(gAlert.ev));
    copyStr(gAlert.ev.title, kTitleMax, "Test meeting");
    copyStr(gAlert.ev.location, kLocMax, "Right here");
    gAlert.ev.start = now + 5 * 60;
    gAlert.ev.end = now + 35 * 60;
  }
  gAlert.kind = AlertKind::Before;
  gAlert.active = true;
  gAlert.test = true;
}

bool nextAlertInfo(AlertHit &hit, Event &ev) {
  int64_t now;
  if (!nowUtc(now)) return false;
  int tz = tzOffsetMin();
  Lock lk;
  int64_t w = gRtc.watermark == kUnset ? now : gRtc.watermark;
  int n = mergedLocked(INT64_MIN, tz);
  if (!nextAlert(gTmp, n, alertSettingsLocked(), gRtc.snooze, w, hit)) return false;
  ev = gTmp[hit.idx];
  return true;
}

// ---------------------------------------------------------------------------
// sleep / wake
// ---------------------------------------------------------------------------
bool prepareSleep(uint32_t nowRtcLocal, bool rtcOk, uint32_t &sleepSec) {
  flushPushed();
  if (!rtcOk) { timerCancel(); return false; }
  int tz = tzOffsetMin();
  int64_t now = rtcToUtc(nowRtcLocal, tz);
  int batPct;
  bool batOk;
  { ModelLock lk; batPct = model.batPct; batOk = model.batOk; }
  int64_t alertAt = kNever, syncAt = kNever;
  {
    Lock lk;
    if (gRtc.watermark == kUnset) gRtc.watermark = now;
    int n = mergedLocked(INT64_MIN, tz);
    AlertHit h;
    if (nextAlert(gTmp, n, alertSettingsLocked(), gRtc.snooze, gRtc.watermark, h)) alertAt = h.at;
  }
  if (backgroundSyncAllowed(now, batOk ? batPct : -1)) syncAt = nextSyncAt(now);
  WakePlan p = planSleep(now, alertAt, syncAt);
  gRtc.plannedWake = p.wakeAt;
  gRtc.plannedReason = (uint8_t)p.reason;
  if (p.reason == WakeReason::None) {
    timerCancel();
    Serial.println("MC: sleep with no timer (nothing to alert or sync)");
    return false;
  }
  int64_t secs = p.wakeAt - now;
  if (secs < 1) secs = 1;
  timerArm(nowRtcLocal, (uint32_t)secs);
  sleepSec = (uint32_t)secs;
  static const char *kReason[] = {"none", "alert", "sync", "prewake", "heartbeat"};
  Serial.printf("MC: sleeping %lus (%s)\n", (unsigned long)secs, kReason[(int)p.reason]);
  return true;
}

WakeAction decideWake(int64_t now, int batPct) {
  int tz = tzOffsetMin();
  int64_t alertAt = kNever;
  {
    Lock lk;
    if (gRtc.watermark == kUnset) return WakeAction::Boot;
    int n = mergedLocked(INT64_MIN, tz);
    AlertHit h;
    if (nextAlert(gTmp, n, alertSettingsLocked(), gRtc.snooze, gRtc.watermark, h)) alertAt = h.at;
  }
  int64_t syncAt = backgroundSyncAllowed(now, batPct) ? nextSyncAt(now) : kNever;
  return mc::decideWake(now, alertAt, syncAt);
}

void keepAwakeFor(uint32_t ms) {
  uint32_t until = millis() + ms;
  if ((int32_t)(until - gKeepAwakeUntil) > 0) gKeepAwakeUntil = until;
}

bool holdAwake() { return (int32_t)(gKeepAwakeUntil - millis()) > 0; }

void flushPending() {
  bool due;
  { Lock lk; due = gPushedDirty && (millis() - gPushedDirtyAt > 1500); }
  if (due) flushPushed();
}

}  // namespace mcapp
