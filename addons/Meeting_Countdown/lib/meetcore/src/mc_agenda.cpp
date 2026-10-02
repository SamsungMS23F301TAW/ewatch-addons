#include "mc_agenda.h"
#include <stdio.h>
#include <string.h>

namespace mc {

// ===========================================================================
// Face
// ===========================================================================
FaceInfo computeFace(const Event *ev, int n, int64_t now, int tzOffsetMin, int leadMin) {
  FaceInfo f;
  int64_t today = floorDiv(now + (int64_t)tzOffsetMin * 60, kDay);
  int64_t lead = (int64_t)(leadMin > 0 ? leadMin : 60) * 60;

  for (int i = 0; i < n; i++) {
    const Event &e = ev[i];
    if (isAllDay(e)) {
      int64_t sd = floorDiv(e.start, kDay), ed = floorDiv(e.end - 1, kDay);
      if (sd <= today && today <= ed && f.nAllDay < 3) f.allDay[f.nAllDay++] = i;
      continue;
    }
    if (e.start <= now && now < e.end) {
      if (f.cur < 0 || e.start > ev[f.cur].start ||
          (e.start == ev[f.cur].start && e.end < ev[f.cur].end))
        f.cur = i;
    } else if (e.start > now) {
      if (f.next < 0 || eventLess(e, ev[f.next])) {
        f.later = f.next;
        f.next = i;
      } else if (f.later < 0 || eventLess(e, ev[f.later])) {
        f.later = i;
      }
    }
  }
  if (f.next >= 0) {
    for (int i = 0; i < n; i++)
      if (i != f.next && !isAllDay(ev[i]) && ev[i].start == ev[f.next].start) f.sameStart++;
  }

  bool upcoming = false;
  if (f.cur >= 0) {
    // Inside a long meeting, a shorter one starting soon within it takes over.
    if (f.next >= 0 && ev[f.next].start < ev[f.cur].end && ev[f.next].start - now <= lead)
      upcoming = true;
  } else if (f.next >= 0) {
    upcoming = true;
  }

  if (upcoming) {
    const Event &nx = ev[f.next];
    f.mode = FaceMode::Upcoming;
    int64_t leaveAt = nx.leaveMin ? nx.start - (int64_t)nx.leaveMin * 60 : kNever;
    if (nx.leaveMin && now < leaveAt) {
      f.deadline = leaveAt;
      f.leave = true;
    } else {
      f.deadline = nx.start;
      f.leaveNow = nx.leaveMin != 0;
    }
    f.remaining = f.deadline - now;
    float r = (float)f.remaining / (float)lead;
    f.ring = r < 0 ? 0 : (r > 1 ? 1 : r);
    if (f.remaining > 0 && f.remaining <= 60) {
      f.finalMinute = true;
      f.minuteRing = (float)f.remaining / 60.0f;
    }
  } else if (f.cur >= 0) {
    const Event &c = ev[f.cur];
    f.mode = FaceMode::InMeeting;
    f.deadline = c.end;
    f.remaining = c.end - now;
    int64_t dur = c.end - c.start;
    float r = dur > 0 ? (float)(now - c.start) / (float)dur : 1.0f;
    f.ring = r < 0 ? 0 : (r > 1 ? 1 : r);
  } else {
    f.mode = FaceMode::Clear;
    f.ring = 1.0f;
  }
  return f;
}

float urgency(int64_t remainingSec) {
  float m = (float)remainingSec / 60.0f;
  if (m >= 20.0f) return 0.0f;
  if (m > 10.0f) return (20.0f - m) / 10.0f;
  if (m > 5.0f) return 1.0f;
  if (m > 2.0f) return 1.0f + (5.0f - m) / 3.0f;
  return 2.0f;
}

// ===========================================================================
// Alerts
// ===========================================================================
template <typename F>
static void forEachAlert(const Event *ev, int n, const AlertSettings &s, const Snooze &sn, F fn) {
  if (s.enabled) {
    for (int i = 0; i < n; i++) {
      const Event &e = ev[i];
      if (isAllDay(e)) continue;
      int64_t leaveAt = e.leaveMin ? e.start - (int64_t)e.leaveMin * 60 : kNever;
      for (int b = 0; b < kNumAlertOffsets; b++) {
        if (!(s.offsetMask & (1u << b))) continue;
        int64_t at = e.start - (int64_t)kAlertOffsets[b] * 60;
        if (at == leaveAt) continue;                 // the leave alert covers it
        fn(at, i, AlertKind::Before, kAlertOffsets[b]);
      }
      if (e.leaveMin) fn(leaveAt, i, AlertKind::Leave, e.leaveMin);
    }
  }
  if (sn.active) {
    for (int i = 0; i < n; i++)
      if (ev[i].key == sn.key && ev[i].end > sn.until) { fn(sn.until, i, AlertKind::Snooze, 0); break; }
  }
}

bool nextAlert(const Event *ev, int n, const AlertSettings &s, const Snooze &sn,
               int64_t watermark, AlertHit &out) {
  out = AlertHit();
  forEachAlert(ev, n, s, sn, [&](int64_t at, int idx, AlertKind k, uint8_t off) {
    if (at <= watermark) return;
    if (out.idx < 0 || at < out.at || (at == out.at && ev[idx].start < ev[out.idx].start)) {
      out.at = at; out.idx = idx; out.kind = k; out.offsetMin = off; out.key = ev[idx].key;
    }
  });
  return out.idx >= 0;
}

static int kindRank(AlertKind k) {
  return k == AlertKind::Leave ? 0 : (k == AlertKind::Snooze ? 1 : 2);
}

int dueAlerts(const Event *ev, int n, const AlertSettings &s, const Snooze &sn,
              int64_t watermark, int64_t now, AlertHit *out, int cap) {
  int m = 0;
  forEachAlert(ev, n, s, sn, [&](int64_t at, int idx, AlertKind k, uint8_t off) {
    if (at <= watermark || at > now) return;
    const Event &e = ev[idx];
    if (now >= e.end && e.end > e.start) return;
    if (k != AlertKind::Snooze && now >= e.start + kAlertGraceSec) return;
    // One hit per event: keep the most recent one (e.g. T-5 over T-10 after
    // a late wake), preferring the leave reminder at equal times.
    for (int j = 0; j < m; j++) {
      if (out[j].idx != idx) continue;
      if (at > out[j].at || (at == out[j].at && kindRank(k) < kindRank(out[j].kind))) {
        out[j].at = at; out[j].kind = k; out[j].offsetMin = off;
      }
      return;
    }
    if (m < cap) {
      out[m].at = at; out[m].idx = idx; out[m].kind = k; out[m].offsetMin = off; out[m].key = e.key;
      m++;
    }
  });
  // Best first: soonest-starting event, then leave/snooze before plain.
  for (int i = 1; i < m; i++) {
    AlertHit t = out[i];
    int j = i - 1;
    while (j >= 0 && (ev[t.idx].start < ev[out[j].idx].start ||
                      (ev[t.idx].start == ev[out[j].idx].start && kindRank(t.kind) < kindRank(out[j].kind)))) {
      out[j + 1] = out[j];
      j--;
    }
    out[j + 1] = t;
  }
  return m;
}

// ===========================================================================
// Wake planning
// ===========================================================================
int64_t driftMarginSec(int64_t sleepSec) {
  if (sleepSec <= kExactSleepSec) return 0;
  int64_t m = sleepSec * kDriftPct / 100;
  return m < 20 ? 20 : m;
}

WakePlan planWake(int64_t now, int64_t nextAlertAt, int64_t nextSyncAt) {
  WakePlan p;
  if (nextAlertAt == kNever && nextSyncAt == kNever) return p;
  // The alert's drift margin applies even when a sync is nominally first: a
  // sync due 4 s before an alert, slept to on a slow RC clock, would wake
  // after the alert. Whichever wake time is earlier wins.
  int64_t alertWake = kNever;
  WakeReason alertReason = WakeReason::Alert;
  if (nextAlertAt != kNever) {
    int64_t d = nextAlertAt - now;
    if (d < 1) d = 1;
    int64_t margin = driftMarginSec(d);
    alertWake = now + d - margin;
    if (margin) alertReason = WakeReason::Prewake;
  }
  int64_t syncWake = kNever;
  if (nextSyncAt != kNever) {
    int64_t d = nextSyncAt - now;
    if (d < 1) d = 1;
    syncWake = now + d;
  }
  if (alertWake <= syncWake) {
    p.wakeAt = alertWake;
    p.target = nextAlertAt;
    p.reason = alertReason;
  } else {
    p.wakeAt = syncWake;
    p.target = nextSyncAt;
    p.reason = WakeReason::Sync;
  }
  if (p.wakeAt - now > kHeartbeatSec) {
    p.wakeAt = now + kHeartbeatSec;
    p.reason = WakeReason::Heartbeat;
  }
  return p;
}

WakeAction decideWake(int64_t now, int64_t nextAlertAt, int64_t nextSyncAt) {
  if (nextAlertAt != kNever && nextAlertAt <= now + 3) return WakeAction::Boot;
  if (nextAlertAt != kNever && nextAlertAt <= now + kAlertGuardSec) return WakeAction::Resleep;
  if (nextSyncAt != kNever && nextSyncAt <= now + 120) return WakeAction::Sync;
  return WakeAction::Resleep;
}

WakePlan planSleep(int64_t now, int64_t nextAlertAt, int64_t nextSyncAt) {
  if (nextSyncAt != kNever && nextAlertAt != kNever && nextAlertAt - now <= kAlertGuardSec &&
      nextSyncAt < nextAlertAt + 5)
    nextSyncAt = nextAlertAt + 5;
  return planWake(now, nextAlertAt, nextSyncAt);
}

// ===========================================================================
// Sync schedule
// ===========================================================================
int64_t nextSyncTime(const SyncState &s, int intervalMin, bool nightPause, int tzOffsetMin,
                     int64_t now) {
  if (intervalMin <= 0) return kNever;
  if (s.lastAttempt == 0) return now;            // never tried: as soon as possible
  int64_t base = s.lastAttempt > now ? now : s.lastAttempt;
  int shift = s.failures > 3 ? 3 : s.failures;
  int64_t gap = ((int64_t)intervalMin * 60) << shift;
  if (gap > 6 * 3600) gap = 6 * 3600;
  int64_t t = base + gap;
  if (t < now) t = now;
  if (nightPause) {
    int64_t off = (int64_t)tzOffsetMin * 60;
    int64_t local = t + off;
    int64_t day = floorDiv(local, kDay);
    int64_t sec = local - day * kDay;
    if (sec >= 23 * 3600) t = (day + 1) * kDay + 6 * 3600 - off;
    else if (sec < 6 * 3600) t = day * kDay + 6 * 3600 - off;
  }
  return t;
}

// ===========================================================================
// Sources
// ===========================================================================
int mergeEvents(const Event *const *lists, const int *counts, int nLists,
                int64_t pruneBefore, int tzOffsetMin, Event *out, int cap) {
  int m = 0;
  for (int l = 0; l < nLists; l++) {
    for (int i = 0; i < counts[l]; i++) {
      const Event &e = lists[l][i];
      int64_t endUtc = isAllDay(e) ? e.end - (int64_t)tzOffsetMin * 60 : e.end;
      if (endUtc <= pruneBefore) continue;
      bool dup = false;
      for (int j = 0; j < m && !dup; j++) {
        if (out[j].start != e.start) continue;
        if (out[j].key == e.key) dup = true;
        else if (out[j].end == e.end && strncmp(out[j].title, e.title, kTitleMax) == 0) dup = true;
      }
      if (dup) continue;
      if (m < cap) {
        out[m++] = e;
      } else {
        int worst = 0;
        for (int j = 1; j < m; j++) if (eventLess(out[worst], out[j])) worst = j;
        if (eventLess(e, out[worst])) out[worst] = e;
      }
    }
  }
  sortEvents(out, m);
  return m;
}

// ===========================================================================
// Text
// ===========================================================================
void fmtDuration(int64_t secs, char *buf, size_t n) {
  if (secs < 0) secs = 0;
  if (secs < 60) { snprintf(buf, n, "%d s", (int)secs); return; }
  int64_t mins = (secs + 59) / 60;
  if (mins < 60) { snprintf(buf, n, "%d min", (int)mins); return; }
  int64_t h = mins / 60, m = mins % 60;
  if (h >= 24) {
    int64_t d = h / 24, hh = h % 24;
    if (hh) snprintf(buf, n, "%d d %d h", (int)d, (int)hh);
    else snprintf(buf, n, "%d d", (int)d);
    return;
  }
  if (m) snprintf(buf, n, "%d h %d min", (int)h, (int)m);
  else snprintf(buf, n, "%d h", (int)h);
}

void fmtDurShort(int64_t secs, char *buf, size_t n) {
  if (secs < 0) secs = 0;
  if (secs < 60) { snprintf(buf, n, "%ds", (int)secs); return; }
  int64_t mins = (secs + 59) / 60;
  if (mins < 60) { snprintf(buf, n, "%dm", (int)mins); return; }
  int64_t h = mins / 60, m = mins % 60;
  if (h >= 48) { snprintf(buf, n, "%dd", (int)(h / 24)); return; }
  if (m && h < 10) snprintf(buf, n, "%dh %dm", (int)h, (int)m);
  else snprintf(buf, n, "%dh", (int)h);
}

void fmtCountdown(int64_t secs, char *buf, size_t n) {
  if (secs <= 0) { snprintf(buf, n, "now"); return; }
  char d[24];
  fmtDuration(secs, d, sizeof(d));
  snprintf(buf, n, "in %s", d);
}

void fmtAgo(int64_t secs, char *buf, size_t n) {
  if (secs < 60) { snprintf(buf, n, "just now"); return; }
  int64_t mins = secs / 60;
  if (mins < 60) { snprintf(buf, n, "%d min ago", (int)mins); return; }
  int64_t h = mins / 60;
  if (h < 48) { snprintf(buf, n, "%d h ago", (int)h); return; }
  snprintf(buf, n, "%d d ago", (int)(h / 24));
}

void fmtWhen(int64_t utc, int64_t now, int tzOffsetMin, char *buf, size_t n) {
  static const char *kWd[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  int64_t off = (int64_t)tzOffsetMin * 60;
  int64_t wall = utc + off;
  char hm[8];
  formatHHMM(wall, hm, sizeof(hm));
  int64_t d = floorDiv(wall, kDay), today = floorDiv(now + off, kDay);
  if (d == today) snprintf(buf, n, "%s", hm);
  else snprintf(buf, n, "%s %s", kWd[weekdayFromDays(d)], hm);
}

}  // namespace mc
