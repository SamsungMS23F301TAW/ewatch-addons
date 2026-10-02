// Meeting Countdown — device-side state: the event store (feed cache, manual
// and USB-pushed events), settings, NVS persistence, the alert/wake planner
// glue and the sleep hooks. Thread-safe: the render task, the WiFi/web task,
// the sync task and the serial console all call in here.
//
// Time base: the RV-3028 keeps local time; mcapp converts with
// model.tzOffsetMin so all planning happens in UTC (see lib/meetcore).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "mc_event.h"
#include "mc_agenda.h"

namespace mcapp {

constexpr int kMaxFeeds    = 3;
constexpr int kMaxFeedEv   = 16;   // cached feed events (all feeds together)
constexpr int kMaxManual   = 8;    // added on the web page
constexpr int kMaxPushed   = 16;   // pushed over USB serial / imported .ics
constexpr int kMaxMerged   = kMaxFeedEv + kMaxManual + kMaxPushed;
constexpr int kUrlMax      = 400;

struct Settings {
  uint8_t version     = 3;
  uint8_t alertsOn    = 1;
  uint8_t alertMask   = mc::kAlertDefaultMask;
  uint8_t leadMin     = 60;     // ring window: 30/45/60/90/120
  uint8_t syncMin     = 30;     // 0 = manual only
  uint8_t nightPause  = 1;      // no background sync 23:00-06:00
  uint8_t faceClassic = 0;      // 1 = stock BaseOS face as Screen::Watch
  uint8_t autoTz      = 1;      // follow the calendar's time zone (DST)
  uint8_t insecureTls = 0;      // skip certificate checks (self-hosted feeds)
  uint8_t snoozeMin   = 3;
  uint8_t reserved[6] = {0};
};

enum class SyncResult : uint8_t {
  Never = 0, Ok, NoFeed, NoWifi, WifiFailed, DnsFailed, ConnectFailed, TlsFailed,
  HttpError, BadData, Timeout, LowMemory, Aborted, LowBattery, Partial
};
const char *syncResultText(SyncResult r);

struct FeedStatus {
  uint8_t  result;        // SyncResult
  int16_t  http;          // last HTTP status (0 = none)
  uint16_t events;        // instances kept from this feed
  uint32_t bytes;
};

struct SyncInfo {
  int64_t    lastAttempt = 0, lastSuccess = 0;   // UTC
  uint8_t    failures = 0;
  SyncResult last = SyncResult::Never;
  FeedStatus feed[kMaxFeeds] = {};
  char       calName[32] = "";
  char       calTz[40] = "";
  int16_t    calTzOffset = 0;
  bool       calTzResolved = false;
  int16_t    tzChangedFrom = 0x7FFF;            // last auto-tz change (0x7FFF = none)
};

// Lifecycle ------------------------------------------------------------------
void init();                       // after Storage::load(); loads NVS, recovers RTC state
// Zeroed event array from PSRAM (internal RAM as a fallback); scratch space
// for snapshots, kept out of internal RAM which TLS needs. Never freed.
mc::Event *allocEvents(int n);
void noteResetReason(bool crashed);

// Time -----------------------------------------------------------------------
bool nowUtc(int64_t &utc);         // false while the RTC is unreadable
int  tzOffsetMin();

// Settings / feeds ------------------------------------------------------------
Settings settings();
void     setSettings(const Settings &s);           // persists + replans
int      feedCount();                               // configured feeds
bool     feedUrl(int i, char *out, size_t cap);    // empty -> false
void     setFeedUrl(int i, const char *url);       // "" clears; persists; requests a sync
void     feedLabel(int i, char *out, size_t cap);  // redacted for display

// Events -----------------------------------------------------------------------
// Merged, sorted view of every source, pruned of finished events.
int  snapshot(mc::Event *out, int cap);
int  countSource(mc::Source s);
bool addEvent(const mc::Event &e, const char **err);          // Manual or Pushed
bool deleteEvent(uint32_t key);                                // Manual/Pushed by key
int  clearSource(mc::Source s);
void replaceSource(mc::Source s, const mc::Event *ev, int n);  // Pushed import
// Called by the sync with fresh feed instances; feeds that failed keep
// their previously cached events.
void applyFeedResults(const mc::Event *ev, int n, const bool feedOk[kMaxFeeds]);

// Sync status --------------------------------------------------------------------
SyncInfo syncInfo();
void     recordSync(const SyncInfo &info);              // persists
void     requestSync();                                  // sync at the next opportunity
bool     syncRequested();
void     clearSyncRequest();
bool     backgroundSyncAllowed(int64_t now, int batPct);
int64_t  nextSyncAt(int64_t now);
void     statusLine(int64_t now, char *buf, size_t n, bool &warn);
// Fast-reconnect hint for the headless sync (RTC memory).
void     rememberAp(const char *ssid, const uint8_t *bssid, uint8_t channel);
bool     lastAp(char *ssid, uint8_t *bssid, uint8_t &channel);
void     markSyncActive(bool active);                    // crash guard

// Alerts ------------------------------------------------------------------------
struct ActiveAlert {
  bool          active = false;
  mc::Event     ev;
  mc::AlertKind kind = mc::AlertKind::Before;
  int           more = 0;
  bool          test = false;
};
void armAwakeTimer();             // after any change while awake
bool onTimerExpired();            // TimerExpired: true -> show the alert screen
ActiveAlert activeAlert();
void snoozeActive();
void dismissActive();
void startTestAlert();
bool nextAlertInfo(mc::AlertHit &hit, mc::Event &ev);

// Sleep / wake --------------------------------------------------------------------
// enterDeepSleep() hook: flushes NVS and arms the RTC timer for the next
// alert / sync / drift-correcting pre-wake. Returns false when nothing needs
// a timer (the stock auto-power-off then applies).
bool prepareSleep(uint32_t nowRtcLocal, bool rtcOk, uint32_t &sleepSec);
using mc::WakeAction;              // Boot / Sync / Resleep (see mc_agenda.h)
WakeAction decideWake(int64_t nowUtc, int batPct);

// Keep the watch awake (web page, serial session, sync in progress).
void keepAwakeFor(uint32_t ms);
bool holdAwake();
// Writes debounced changes (USB-pushed events) once input has gone quiet.
void flushPending();

}  // namespace mcapp
