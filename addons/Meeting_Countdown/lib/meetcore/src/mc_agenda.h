// Meeting Countdown — the decision logic, free of hardware so the host tests
// can pin it down: which event the face shows, how full the ring is, which
// alert fires next, when the watch must wake, and when to sync.
//
// All times are UTC instants (int64 Unix seconds) except all-day events,
// whose start/end are wall-clock midnights (see mc_event.h).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "mc_event.h"
#include "mc_time.h"

namespace mc {

// ---------------------------------------------------------------------------
// Face
// ---------------------------------------------------------------------------
enum class FaceMode : uint8_t { Clear, Upcoming, InMeeting };

struct FaceInfo {
  FaceMode mode = FaceMode::Clear;
  int      cur = -1;          // in-progress timed event (latest started), or -1
  int      next = -1;         // next timed event starting after now, or -1
  int      later = -1;        // the one after `next`, or -1
  int      sameStart = 0;     // other events starting together with `next`
  int64_t  deadline = kNever; // what the ring counts down to
  int64_t  remaining = 0;     // seconds to the deadline (or to cur's end)
  bool     leave = false;     // deadline is next's "leave now" time
  bool     leaveNow = false;  // the leave time has passed, event not started
  float    ring = 1.0f;       // filled fraction 0..1
  bool     finalMinute = false;  // deadline < 60 s away: the ring "zooms" to seconds
  float    minuteRing = 0.0f;    // remaining / 60 s while finalMinute
  int      allDay[3] = {-1, -1, -1};
  int      nAllDay = 0;       // all-day events covering today (local)
};

// leadMin: the countdown window. The ring is full while the next deadline is
// leadMin or more away and drains linearly to empty at the deadline. While in
// a meeting it fills with the meeting's elapsed fraction instead.
FaceInfo computeFace(const Event *ev, int n, int64_t now, int tzOffsetMin, int leadMin);

// Ring colour stage for an Upcoming countdown: 0 calm .. 1 amber .. 2 red,
// fractional values blend between neighbours.
float urgency(int64_t remainingSec);

// ---------------------------------------------------------------------------
// Alerts
// ---------------------------------------------------------------------------
// Selectable alert offsets (minutes before start); bit i <-> kAlertOffsets[i].
constexpr int     kNumAlertOffsets = 7;
constexpr uint8_t kAlertOffsets[kNumAlertOffsets] = {0, 1, 2, 5, 10, 15, 30};
constexpr uint8_t kAlertDefaultMask = 1u << 3;       // 5 minutes
constexpr int64_t kAlertGraceSec = 5 * 60;           // still alert up to 5 min late

struct AlertSettings {
  bool    enabled = true;
  uint8_t offsetMask = kAlertDefaultMask;
};

struct Snooze {
  bool     active = false;
  uint32_t key = 0;           // Event::key of the snoozed instance
  int64_t  until = 0;
};

enum class AlertKind : uint8_t { None = 0, Before, Leave, Snooze };

struct AlertHit {
  int64_t   at = kNever;
  int       idx = -1;         // index into the event array
  AlertKind kind = AlertKind::None;
  uint8_t   offsetMin = 0;
  uint32_t  key = 0;
};

// Earliest alert instant strictly after `watermark`. Returns false if none.
bool nextAlert(const Event *ev, int n, const AlertSettings &s, const Snooze &sn,
               int64_t watermark, AlertHit &out);

// Alerts due at `now` (watermark < at <= now) that are still relevant, best
// first (soonest-starting event first). Returns the count written.
int dueAlerts(const Event *ev, int n, const AlertSettings &s, const Snooze &sn,
              int64_t watermark, int64_t now, AlertHit *out, int cap);

// ---------------------------------------------------------------------------
// Sleep / wake planning
// ---------------------------------------------------------------------------
enum class WakeReason : uint8_t { None = 0, Alert, Sync, Prewake, Heartbeat };

struct WakePlan {
  int64_t    wakeAt = kNever;   // when to arm the RTC timer
  int64_t    target = kNever;   // the real deadline behind it
  WakeReason reason = WakeReason::None;
};

// The ESP32-S3 sleep timer runs from a ~136 kHz RC oscillator. It is
// calibrated at every boot but still drifts a few percent with temperature
// (wrist to nightstand), so a long sleep before an alert ends early by
// kDriftPct (at least 20 s, a "prewake") and re-plans from the accurate
// RV-3028 time; each hop shrinks the error. Sleeps of kExactSleepSec or less
// are taken as-is (<= 2.7 s off at 6%). The margin costs one or two extra
// dark wakes per alert (~0.5 s each); a tighter one risks a late alert.
constexpr int64_t kHeartbeatSec = 4 * 3600;
constexpr int64_t kExactSleepSec = 45;
constexpr int64_t kDriftPct = 6;
int64_t driftMarginSec(int64_t sleepSec);
WakePlan planWake(int64_t now, int64_t nextAlertAt, int64_t nextSyncAt);

// The two decisions the watch makes around deep sleep (pass kNever when there
// is no alert / no background sync allowed):
//  * at a timer wake: Boot the UI when an alert is due within 3 s; Sync when a
//    sync is due (within 120 s: the RC timer may wake early) unless an alert
//    comes within 90 s; otherwise go back to sleep.
//  * before sleeping: keep a sync out of the 90 s before an alert (it runs just
//    after instead), then planWake().
enum class WakeAction : uint8_t { Boot, Sync, Resleep };
constexpr int64_t kAlertGuardSec = 90;
WakeAction decideWake(int64_t now, int64_t nextAlertAt, int64_t nextSyncAt);
WakePlan   planSleep(int64_t now, int64_t nextAlertAt, int64_t nextSyncAt);

// ---------------------------------------------------------------------------
// Background sync schedule
// ---------------------------------------------------------------------------
struct SyncState {
  int64_t lastAttempt = 0;    // UTC, 0 = never
  int64_t lastSuccess = 0;    // UTC, 0 = never
  uint8_t failures = 0;       // consecutive failures (exponential backoff)
};
// intervalMin 0 = manual only. Night pause holds background syncs between
// 23:00 and 06:00 local. Returns kNever when no sync is scheduled.
int64_t nextSyncTime(const SyncState &s, int intervalMin, bool nightPause,
                     int tzOffsetMin, int64_t now);

// ---------------------------------------------------------------------------
// Sources
// ---------------------------------------------------------------------------
// Merge per-source lists into one sorted list, dropping duplicates (same
// instance key, or same start/end/title) and events that ended before
// `pruneBefore`. Returns the number written (<= cap).
int mergeEvents(const Event *const *lists, const int *counts, int nLists,
                int64_t pruneBefore, int tzOffsetMin, Event *out, int cap);

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------
void fmtCountdown(int64_t secs, char *buf, size_t n);  // "in 23 min", "in 1 h 5 min", "in 45 s"
void fmtDuration(int64_t secs, char *buf, size_t n);   // "23 min", "1 h 5 min", "45 s"
void fmtDurShort(int64_t secs, char *buf, size_t n);   // "23m", "1h 5m", "45s", "2d"
void fmtAgo(int64_t secs, char *buf, size_t n);        // "just now", "5 min ago", "2 h ago"
// "09:30", or "Tue 09:30" when the instant is not on the local `now` day.
void fmtWhen(int64_t utc, int64_t now, int tzOffsetMin, char *buf, size_t n);

}  // namespace mc
