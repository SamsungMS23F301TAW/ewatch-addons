// Host tests for the decision logic: next-event selection, ring fraction and
// colour stage, alert computation (offsets, leave-now, snooze, watermark,
// midnight, back-to-back), wake planning with drift margins, the background
// sync schedule, source merging, text formatting and the EVENT protocol.
#include <unity.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <vector>
#include "mc_agenda.h"
#include "mc_proto.h"
#include "mc_text.h"

using namespace mc;

static const int64_t NOW = 1790841600;          // 2026-10-01T08:00:00Z (Thursday)
static const int64_t M = 60, H = 3600;
static const int TZ = 60;                        // watch at UTC+1

static Event mk(const char *title, int64_t start, int64_t end, uint8_t leave = 0) {
  Event e;
  memset((void *)&e, 0, sizeof(e));
  copyStr(e.title, sizeof(e.title), title);
  e.start = start;
  e.end = end;
  e.leaveMin = leave;
  finishManualEvent(e, Source::Manual, (uint32_t)start);
  return e;
}

static Event mkAllDay(const char *title, int y, int mo, int d, int days = 1) {
  Event e = mk(title, civilToSeconds(y, mo, d, 0, 0, 0), civilToSeconds(y, mo, d, 0, 0, 0) + days * kDay);
  e.flags |= EF_ALLDAY;
  return e;
}

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------
void test_face_far_off() {
  Event ev[] = {mk("Lunch", NOW + 3 * H, NOW + 4 * H)};
  FaceInfo f = computeFace(ev, 1, NOW, TZ, 60);
  TEST_ASSERT_EQUAL_INT((int)FaceMode::Upcoming, (int)f.mode);
  TEST_ASSERT_EQUAL_INT(0, f.next);
  TEST_ASSERT_EQUAL_INT64(3 * H, f.remaining);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, f.ring);                // capped full
  TEST_ASSERT_EQUAL_FLOAT(0.0f, urgency(f.remaining));  // calm
}

void test_face_ring_drains_linearly() {
  Event ev[] = {mk("Review", NOW + 60 * M, NOW + 90 * M)};
  // 60-minute lead: full at T-60, half at T-30, empty at T-0.
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, computeFace(ev, 1, NOW, TZ, 60).ring);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, computeFace(ev, 1, NOW + 30 * M, TZ, 60).ring);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 23.0f / 60.0f, computeFace(ev, 1, NOW + 37 * M, TZ, 60).ring);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f / 3600.0f, computeFace(ev, 1, NOW + 60 * M - 1, TZ, 60).ring);
  // At the start time the event becomes current: the ring now shows progress.
  FaceInfo atStart = computeFace(ev, 1, NOW + 60 * M, TZ, 60);
  TEST_ASSERT_EQUAL_INT((int)FaceMode::InMeeting, (int)atStart.mode);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, atStart.ring);
  // A 30-minute lead window drains twice as fast.
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, computeFace(ev, 1, NOW + 30 * M, TZ, 30).ring);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, computeFace(ev, 1, NOW + 45 * M, TZ, 30).ring);
}

void test_face_urgency_stages() {
  TEST_ASSERT_EQUAL_FLOAT(0.0f, urgency(25 * M));
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, urgency(15 * M));   // calm -> amber
  TEST_ASSERT_EQUAL_FLOAT(1.0f, urgency(8 * M));            // amber
  TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f + 1.0f / 3.0f, urgency(4 * M));
  TEST_ASSERT_EQUAL_FLOAT(2.0f, urgency(90));                // red
  TEST_ASSERT_EQUAL_FLOAT(2.0f, urgency(0));
  // Monotonic: never gets calmer as the event approaches.
  float prev = 0;
  for (int64_t s = 40 * M; s >= 0; s -= 7) {
    float u = urgency(s);
    TEST_ASSERT_TRUE(u >= prev - 1e-6f);
    prev = u;
  }
}

void test_face_in_meeting_and_next() {
  Event ev[] = {mk("Workshop", NOW - 30 * M, NOW + 30 * M), mk("1:1", NOW + 30 * M, NOW + 60 * M)};
  FaceInfo f = computeFace(ev, 2, NOW, TZ, 60);
  TEST_ASSERT_EQUAL_INT((int)FaceMode::InMeeting, (int)f.mode);
  TEST_ASSERT_EQUAL_INT(0, f.cur);
  TEST_ASSERT_EQUAL_INT(1, f.next);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, f.ring);             // half way through
  TEST_ASSERT_EQUAL_INT64(30 * M, f.remaining);
}

void test_face_back_to_back() {
  // A ends exactly when B starts: in A until 10:00:00, then B is current.
  Event ev[] = {mk("A", NOW, NOW + H), mk("B", NOW + H, NOW + 2 * H)};
  FaceInfo f = computeFace(ev, 2, NOW + H - 1, TZ, 60);
  TEST_ASSERT_EQUAL_INT((int)FaceMode::InMeeting, (int)f.mode);
  TEST_ASSERT_EQUAL_INT(0, f.cur);
  TEST_ASSERT_EQUAL_INT(1, f.next);
  f = computeFace(ev, 2, NOW + H, TZ, 60);
  TEST_ASSERT_EQUAL_INT((int)FaceMode::InMeeting, (int)f.mode);
  TEST_ASSERT_EQUAL_INT(1, f.cur);
  TEST_ASSERT_EQUAL_INT(-1, f.next);
}

void test_face_short_meeting_inside_long_one() {
  // In an all-afternoon workshop, a 1:1 starting in 10 min takes over.
  Event ev[] = {mk("Workshop", NOW - H, NOW + 3 * H), mk("1:1", NOW + 10 * M, NOW + 40 * M)};
  FaceInfo f = computeFace(ev, 2, NOW, TZ, 60);
  TEST_ASSERT_EQUAL_INT((int)FaceMode::Upcoming, (int)f.mode);
  TEST_ASSERT_EQUAL_INT(1, f.next);
  TEST_ASSERT_EQUAL_INT(0, f.cur);
  // ...but not while it is still beyond the lead window.
  Event ev2[] = {mk("Workshop", NOW - H, NOW + 3 * H), mk("1:1", NOW + 2 * H, NOW + 150 * M)};
  f = computeFace(ev2, 2, NOW, TZ, 60);
  TEST_ASSERT_EQUAL_INT((int)FaceMode::InMeeting, (int)f.mode);
}

void test_face_overlapping_picks_latest_started() {
  Event ev[] = {mk("Long", NOW - 2 * H, NOW + 2 * H), mk("Short", NOW - 10 * M, NOW + 20 * M)};
  FaceInfo f = computeFace(ev, 2, NOW, TZ, 60);
  TEST_ASSERT_EQUAL_INT(1, f.cur);
}

void test_face_clear_and_all_day() {
  FaceInfo f = computeFace(nullptr, 0, NOW, TZ, 60);
  TEST_ASSERT_EQUAL_INT((int)FaceMode::Clear, (int)f.mode);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, f.ring);
  // All-day events never drive the ring, but are listed for today.
  Event ev[] = {mkAllDay("Holiday", 2026, 10, 1), mkAllDay("Tomorrow thing", 2026, 10, 2),
                mk("Past", NOW - 2 * H, NOW - H)};
  f = computeFace(ev, 3, NOW, TZ, 60);
  TEST_ASSERT_EQUAL_INT((int)FaceMode::Clear, (int)f.mode);
  TEST_ASSERT_EQUAL_INT(1, f.nAllDay);
  TEST_ASSERT_EQUAL_INT(0, f.allDay[0]);
  // Local midnight matters, not UTC: at 23:30Z on Oct 1 it is already Oct 2
  // at UTC+1.
  f = computeFace(ev, 3, civilToSeconds(2026, 10, 1, 23, 30, 0), TZ, 60);
  TEST_ASSERT_EQUAL_INT(1, f.nAllDay);
  TEST_ASSERT_EQUAL_INT(1, f.allDay[0]);
}

void test_face_leave_buffer() {
  Event ev[] = {mk("Dentist", NOW + 40 * M, NOW + 70 * M, 15)};
  FaceInfo f = computeFace(ev, 1, NOW, TZ, 60);
  TEST_ASSERT_TRUE(f.leave);
  TEST_ASSERT_EQUAL_INT64(NOW + 25 * M, f.deadline);
  TEST_ASSERT_EQUAL_INT64(25 * M, f.remaining);
  f = computeFace(ev, 1, NOW + 30 * M, TZ, 60);           // past the leave time
  TEST_ASSERT_FALSE(f.leave);
  TEST_ASSERT_TRUE(f.leaveNow);
  TEST_ASSERT_EQUAL_INT64(NOW + 40 * M, f.deadline);
}

void test_face_same_start() {
  Event ev[] = {mk("A", NOW + H, NOW + 2 * H), mk("B", NOW + H, NOW + 90 * M), mk("C", NOW + 3 * H, NOW + 4 * H)};
  sortEvents(ev, 3);
  FaceInfo f = computeFace(ev, 3, NOW, TZ, 60);
  TEST_ASSERT_EQUAL_STRING("B", ev[f.next].title);        // ends first
  TEST_ASSERT_EQUAL_INT(1, f.sameStart);
  TEST_ASSERT_EQUAL_STRING("A", ev[f.later].title);
}

// ---------------------------------------------------------------------------
void test_alert_default_five_minutes() {
  Event ev[] = {mk("Standup", NOW + 30 * M, NOW + 45 * M)};
  AlertSettings s;
  Snooze sn;
  AlertHit h;
  TEST_ASSERT_TRUE(nextAlert(ev, 1, s, sn, NOW, h));
  TEST_ASSERT_EQUAL_INT64(NOW + 25 * M, h.at);
  TEST_ASSERT_EQUAL_INT((int)AlertKind::Before, (int)h.kind);
  TEST_ASSERT_EQUAL_UINT8(5, h.offsetMin);
  // Once handled (watermark moved), nothing else for this event.
  TEST_ASSERT_FALSE(nextAlert(ev, 1, s, sn, h.at, h));
  // Disabled alerts produce nothing.
  s.enabled = false;
  TEST_ASSERT_FALSE(nextAlert(ev, 1, s, sn, NOW, h));
}

void test_alert_multiple_offsets_and_leave() {
  Event ev[] = {mk("Offsite", NOW + 60 * M, NOW + 120 * M, 20)};
  AlertSettings s;
  s.offsetMask = (1u << 0) | (1u << 3) | (1u << 6);       // at start, 5, 30
  Snooze sn;
  AlertHit h;
  int64_t w = NOW;
  int64_t expect[] = {NOW + 30 * M, NOW + 40 * M, NOW + 55 * M, NOW + 60 * M};
  AlertKind kinds[] = {AlertKind::Before, AlertKind::Leave, AlertKind::Before, AlertKind::Before};
  for (int i = 0; i < 4; i++) {
    TEST_ASSERT_TRUE(nextAlert(ev, 1, s, sn, w, h));
    TEST_ASSERT_EQUAL_INT64(expect[i], h.at);
    TEST_ASSERT_EQUAL_INT((int)kinds[i], (int)h.kind);
    w = h.at;
  }
  TEST_ASSERT_FALSE(nextAlert(ev, 1, s, sn, w, h));
}

void test_alert_leave_replaces_same_time_offset() {
  Event ev[] = {mk("Gym", NOW + 60 * M, NOW + 120 * M, 5)};
  AlertSettings s;                                         // default: 5 min
  Snooze sn;
  AlertHit h;
  TEST_ASSERT_TRUE(nextAlert(ev, 1, s, sn, NOW, h));
  TEST_ASSERT_EQUAL_INT((int)AlertKind::Leave, (int)h.kind);
  TEST_ASSERT_FALSE(nextAlert(ev, 1, s, sn, h.at, h));
}

void test_alert_across_midnight() {
  // 00:02 local (UTC+1) on Oct 2 = 23:02Z Oct 1: the T-5 alert is at 23:57
  // local on the previous day.
  int64_t start = civilToSeconds(2026, 10, 1, 23, 2, 0);
  Event ev[] = {mk("Night launch", start, start + 30 * M)};
  AlertSettings s;
  Snooze sn;
  AlertHit h;
  TEST_ASSERT_TRUE(nextAlert(ev, 1, s, sn, NOW, h));
  TEST_ASSERT_EQUAL_INT64(start - 5 * M, h.at);
  Civil local = secondsToCivil(h.at + TZ * 60);
  TEST_ASSERT_EQUAL_INT(1, local.day);
  TEST_ASSERT_EQUAL_INT(23, local.hour);
  TEST_ASSERT_EQUAL_INT(57, local.minute);
  AlertHit due[4];
  TEST_ASSERT_EQUAL_INT(1, dueAlerts(ev, 1, s, sn, NOW, h.at, due, 4));
}

void test_alert_back_to_back_and_simultaneous() {
  Event ev[] = {mk("A", NOW + 10 * M, NOW + 40 * M), mk("B", NOW + 40 * M, NOW + 70 * M),
                mk("C", NOW + 40 * M, NOW + 50 * M)};
  sortEvents(ev, 3);
  AlertSettings s;
  Snooze sn;
  AlertHit h;
  TEST_ASSERT_TRUE(nextAlert(ev, 3, s, sn, NOW, h));
  TEST_ASSERT_EQUAL_INT64(NOW + 5 * M, h.at);
  TEST_ASSERT_EQUAL_STRING("A", ev[h.idx].title);
  TEST_ASSERT_TRUE(nextAlert(ev, 3, s, sn, h.at, h));
  TEST_ASSERT_EQUAL_INT64(NOW + 35 * M, h.at);             // B and C together, during A
  AlertHit due[4];
  int n = dueAlerts(ev, 3, s, sn, NOW + 5 * M, NOW + 35 * M, due, 4);
  TEST_ASSERT_EQUAL_INT(2, n);
  TEST_ASSERT_EQUAL_STRING("C", ev[due[0].idx].title);      // same start: shorter first (sort)
  TEST_ASSERT_TRUE(nextAlert(ev, 3, s, sn, NOW + 35 * M, h) == false);
}

void test_alert_due_window_and_lateness() {
  Event ev[] = {mk("Call", NOW + 10 * M, NOW + 40 * M)};
  AlertSettings s;
  s.offsetMask = (1u << 3) | (1u << 4);                    // 5 and 10 min
  Snooze sn;
  AlertHit due[4];
  // Woke late: both offsets passed; only the most recent one is reported.
  int n = dueAlerts(ev, 1, s, sn, NOW - H, NOW + 6 * M, due, 4);
  TEST_ASSERT_EQUAL_INT(1, n);
  TEST_ASSERT_EQUAL_UINT8(5, due[0].offsetMin);
  // Still alerted up to 5 minutes after the start...
  n = dueAlerts(ev, 1, s, sn, NOW - H, NOW + 14 * M, due, 4);
  TEST_ASSERT_EQUAL_INT(1, n);
  // ...but not later than that.
  n = dueAlerts(ev, 1, s, sn, NOW - H, NOW + 16 * M, due, 4);
  TEST_ASSERT_EQUAL_INT(0, n);
  // Nothing due a second before the T-10 alert; due exactly at it.
  n = dueAlerts(ev, 1, s, sn, NOW - H, NOW - 1, due, 4);
  TEST_ASSERT_EQUAL_INT(0, n);
  n = dueAlerts(ev, 1, s, sn, NOW - H, NOW, due, 4);
  TEST_ASSERT_EQUAL_INT(1, n);
  TEST_ASSERT_EQUAL_UINT8(10, due[0].offsetMin);
}

void test_snooze() {
  Event ev[] = {mk("Review", NOW + 5 * M, NOW + 35 * M)};
  AlertSettings s;
  Snooze sn;
  sn.active = true;
  sn.key = ev[0].key;
  sn.until = NOW + 3 * M;
  AlertHit h;
  TEST_ASSERT_TRUE(nextAlert(ev, 1, s, sn, NOW, h));
  TEST_ASSERT_EQUAL_INT((int)AlertKind::Snooze, (int)h.kind);
  TEST_ASSERT_EQUAL_INT64(NOW + 3 * M, h.at);
  // Snoozing past the end of the event is pointless and ignored.
  sn.until = NOW + 40 * M;
  TEST_ASSERT_FALSE(nextAlert(ev, 1, s, sn, NOW, h));
  // A snooze for an unknown key (event deleted by a sync) is ignored.
  sn.until = NOW + 3 * M;
  sn.key ^= 0xFFFF;
  TEST_ASSERT_FALSE(nextAlert(ev, 1, s, sn, NOW, h));
}

void test_all_day_never_alerts() {
  Event ev[] = {mkAllDay("Holiday", 2026, 10, 2)};
  AlertSettings s;
  s.offsetMask = 0x7F;
  Snooze sn;
  AlertHit h;
  TEST_ASSERT_FALSE(nextAlert(ev, 1, s, sn, NOW, h));
}

// ---------------------------------------------------------------------------
void test_wake_plan_alert_and_prewake() {
  // Alert in 45 s: wake exactly then.
  WakePlan p = planWake(NOW, NOW + 45, kNever);
  TEST_ASSERT_EQUAL_INT((int)WakeReason::Alert, (int)p.reason);
  TEST_ASSERT_EQUAL_INT64(NOW + 45, p.wakeAt);
  // Alert in 80 s: 20 s early, then a short exact hop.
  p = planWake(NOW, NOW + 80, kNever);
  TEST_ASSERT_EQUAL_INT((int)WakeReason::Prewake, (int)p.reason);
  TEST_ASSERT_EQUAL_INT64(NOW + 60, p.wakeAt);
  // Alert in 2 h: wake 6% early, then re-plan from the RTC.
  p = planWake(NOW, NOW + 2 * H, kNever);
  TEST_ASSERT_EQUAL_INT((int)WakeReason::Prewake, (int)p.reason);
  TEST_ASSERT_EQUAL_INT64(NOW + 2 * H - 432, p.wakeAt);
  TEST_ASSERT_EQUAL_INT64(NOW + 2 * H, p.target);
  // Successive hops converge: each gap is <= 6% of the previous one.
  int64_t now = NOW, target = NOW + 2 * H;
  int hops = 0;
  while (true) {
    WakePlan q = planWake(now, target, kNever);
    hops++;
    if (q.reason == WakeReason::Alert) break;
    TEST_ASSERT_TRUE(q.wakeAt < target);
    now = q.wakeAt;
    TEST_ASSERT_TRUE(hops < 6);
  }
  TEST_ASSERT_TRUE(hops <= 4);
  // Minimum margin of 20 s for medium sleeps.
  p = planWake(NOW, NOW + 200, kNever);
  TEST_ASSERT_EQUAL_INT64(NOW + 180, p.wakeAt);
}

void test_wake_plan_sync_heartbeat_none() {
  WakePlan p = planWake(NOW, NOW + 3 * H, NOW + 30 * M);
  TEST_ASSERT_EQUAL_INT((int)WakeReason::Sync, (int)p.reason);
  TEST_ASSERT_EQUAL_INT64(NOW + 30 * M, p.wakeAt);
  p = planWake(NOW, NOW + 30 * H, kNever);
  TEST_ASSERT_EQUAL_INT((int)WakeReason::Heartbeat, (int)p.reason);
  TEST_ASSERT_EQUAL_INT64(NOW + kHeartbeatSec, p.wakeAt);
  p = planWake(NOW, kNever, kNever);
  TEST_ASSERT_EQUAL_INT((int)WakeReason::None, (int)p.reason);
  TEST_ASSERT_EQUAL_INT64(kNever, p.wakeAt);
  // Overdue targets fire after one second, never in the past.
  p = planWake(NOW, NOW - 50, kNever);
  TEST_ASSERT_EQUAL_INT64(NOW + 1, p.wakeAt);
  // A sync due 4 s before an alert 30 min away must not drag the wake past
  // the alert's drift margin (6% of 1800 s = 108 s early).
  p = planWake(NOW, NOW + 30 * M, NOW + 30 * M - 4);
  TEST_ASSERT_EQUAL_INT((int)WakeReason::Prewake, (int)p.reason);
  TEST_ASSERT_EQUAL_INT64(NOW + 30 * M - 108, p.wakeAt);
  TEST_ASSERT_EQUAL_INT64(NOW + 30 * M, p.target);
  // A sync that is genuinely earlier than the prewake still goes first.
  p = planWake(NOW, NOW + 30 * M, NOW + 20 * M);
  TEST_ASSERT_EQUAL_INT((int)WakeReason::Sync, (int)p.reason);
  TEST_ASSERT_EQUAL_INT64(NOW + 20 * M, p.wakeAt);
}

void test_sync_schedule() {
  SyncState st;
  TEST_ASSERT_EQUAL_INT64(NOW, nextSyncTime(st, 30, true, TZ, NOW));            // never synced
  TEST_ASSERT_EQUAL_INT64(kNever, nextSyncTime(st, 0, true, TZ, NOW));          // manual only
  st.lastAttempt = NOW;
  TEST_ASSERT_EQUAL_INT64(NOW + 30 * M, nextSyncTime(st, 30, true, TZ, NOW));
  // Backoff doubles per failure, capped at 8x / 6 h.
  st.failures = 1;
  TEST_ASSERT_EQUAL_INT64(NOW + 60 * M, nextSyncTime(st, 30, false, TZ, NOW));
  st.failures = 9;
  TEST_ASSERT_EQUAL_INT64(NOW + 240 * M, nextSyncTime(st, 30, false, TZ, NOW));
  st.failures = 9;
  TEST_ASSERT_EQUAL_INT64(NOW + 6 * H, nextSyncTime(st, 120, false, TZ, NOW));
  // Night pause: 22:50 local + 30 min lands at 23:20 -> moved to 06:00.
  st.failures = 0;
  int64_t t2250 = civilToSeconds(2026, 10, 1, 21, 50, 0);                       // 22:50 at +1
  st.lastAttempt = t2250;
  int64_t expect = civilToSeconds(2026, 10, 2, 5, 0, 0);                        // 06:00 local
  TEST_ASSERT_EQUAL_INT64(expect, nextSyncTime(st, 30, true, TZ, t2250));
  TEST_ASSERT_EQUAL_INT64(t2250 + 30 * M, nextSyncTime(st, 30, false, TZ, t2250));
  // 03:00 local is inside the pause too.
  int64_t t0300 = civilToSeconds(2026, 10, 2, 2, 0, 0);
  st.lastAttempt = t0300 - H;
  TEST_ASSERT_EQUAL_INT64(expect, nextSyncTime(st, 30, true, TZ, t0300));
  // A clock that jumped backwards does not push the sync into the far future.
  st.lastAttempt = NOW + 10 * H;
  TEST_ASSERT_EQUAL_INT64(NOW + 30 * M, nextSyncTime(st, 30, false, TZ, NOW));
}

// ---------------------------------------------------------------------------
void test_merge_dedupe_prune() {
  Event manual[] = {mk("Design review", NOW + H, NOW + 2 * H)};
  Event pushed[] = {mk("Gym", NOW + 5 * H, NOW + 6 * H), mk("Old", NOW - 3 * H, NOW - 2 * H)};
  Event feed[] = {mk("Design review", NOW + H, NOW + 2 * H), mk("Standup", NOW + 30 * M, NOW + 45 * M)};
  feed[0].source = (uint8_t)Source::Feed;
  const Event *lists[3] = {manual, pushed, feed};
  int counts[3] = {1, 2, 2};
  Event out[8];
  int n = mergeEvents(lists, counts, 3, NOW, TZ, out, 8);
  TEST_ASSERT_EQUAL_INT(3, n);
  TEST_ASSERT_EQUAL_STRING("Standup", out[0].title);
  TEST_ASSERT_EQUAL_STRING("Design review", out[1].title);
  TEST_ASSERT_EQUAL_INT((int)Source::Manual, out[1].source);   // first list wins
  TEST_ASSERT_EQUAL_STRING("Gym", out[2].title);
  // Capacity keeps the earliest.
  n = mergeEvents(lists, counts, 3, NOW, TZ, out, 2);
  TEST_ASSERT_EQUAL_INT(2, n);
  TEST_ASSERT_EQUAL_STRING("Standup", out[0].title);
  TEST_ASSERT_EQUAL_STRING("Design review", out[1].title);
}

void test_text_formatting() {
  char b[32];
  fmtCountdown(23 * M, b, sizeof(b));        TEST_ASSERT_EQUAL_STRING("in 23 min", b);
  fmtCountdown(22 * M + 1, b, sizeof(b));    TEST_ASSERT_EQUAL_STRING("in 23 min", b);
  fmtCountdown(65 * M, b, sizeof(b));        TEST_ASSERT_EQUAL_STRING("in 1 h 5 min", b);
  fmtCountdown(2 * H, b, sizeof(b));         TEST_ASSERT_EQUAL_STRING("in 2 h", b);
  fmtCountdown(45, b, sizeof(b));            TEST_ASSERT_EQUAL_STRING("in 45 s", b);
  fmtCountdown(0, b, sizeof(b));             TEST_ASSERT_EQUAL_STRING("now", b);
  fmtCountdown(26 * H, b, sizeof(b));        TEST_ASSERT_EQUAL_STRING("in 1 d 2 h", b);
  fmtAgo(30, b, sizeof(b));                  TEST_ASSERT_EQUAL_STRING("just now", b);
  fmtAgo(12 * M, b, sizeof(b));              TEST_ASSERT_EQUAL_STRING("12 min ago", b);
  fmtAgo(2 * H + 5, b, sizeof(b));           TEST_ASSERT_EQUAL_STRING("2 h ago", b);
  fmtAgo(3 * kDay, b, sizeof(b));            TEST_ASSERT_EQUAL_STRING("3 d ago", b);
  fmtWhen(NOW + H, NOW, TZ, b, sizeof(b));   TEST_ASSERT_EQUAL_STRING("10:00", b);
  fmtWhen(NOW + 24 * H, NOW, TZ, b, sizeof(b)); TEST_ASSERT_EQUAL_STRING("Fri 09:00", b);
}

void test_iso_and_durations() {
  int64_t t;
  bool dateOnly;
  TEST_ASSERT_TRUE(parseIso8601("2026-10-01T09:00", TZ, t, &dateOnly));
  TEST_ASSERT_EQUAL_INT64(NOW, t);
  TEST_ASSERT_FALSE(dateOnly);
  TEST_ASSERT_TRUE(parseIso8601("2026-10-01T08:00:00Z", TZ, t));      TEST_ASSERT_EQUAL_INT64(NOW, t);
  TEST_ASSERT_TRUE(parseIso8601("2026-10-01T04:00-04:00", TZ, t));    TEST_ASSERT_EQUAL_INT64(NOW, t);
  TEST_ASSERT_TRUE(parseIso8601("20261001T080000Z", TZ, t));          TEST_ASSERT_EQUAL_INT64(NOW, t);
  TEST_ASSERT_TRUE(parseIso8601("2026-10-01T08:00:00.123Z", TZ, t));  TEST_ASSERT_EQUAL_INT64(NOW, t);
  TEST_ASSERT_TRUE(parseIso8601("2026-10-01", TZ, t, &dateOnly));
  TEST_ASSERT_TRUE(dateOnly);
  TEST_ASSERT_FALSE(parseIso8601("2026-13-01T09:00", TZ, t));
  TEST_ASSERT_FALSE(parseIso8601("tomorrow", TZ, t));
  int64_t d;
  TEST_ASSERT_TRUE(parseHumanDuration("+30m", d));    TEST_ASSERT_EQUAL_INT64(30 * M, d);
  TEST_ASSERT_TRUE(parseHumanDuration("1h30m", d));   TEST_ASSERT_EQUAL_INT64(90 * M, d);
  TEST_ASSERT_TRUE(parseHumanDuration("45", d));      TEST_ASSERT_EQUAL_INT64(45 * M, d);
  TEST_ASSERT_TRUE(parseHumanDuration("PT45M", d));   TEST_ASSERT_EQUAL_INT64(45 * M, d);
  TEST_ASSERT_FALSE(parseHumanDuration("m30", d));
  TEST_ASSERT_FALSE(parseHumanDuration("", d));
}

void test_event_protocol() {
  Event e;
  const char *err;
  TEST_ASSERT_TRUE(parseEventArgs("2026-10-01T10:00 2026-10-01T10:30 Design review", TZ, Source::Pushed, e, &err));
  TEST_ASSERT_EQUAL_INT64(NOW + H, e.start);
  TEST_ASSERT_EQUAL_INT64(NOW + 90 * M, e.end);
  TEST_ASSERT_EQUAL_STRING("Design review", e.title);
  TEST_ASSERT_EQUAL_INT((int)Source::Pushed, e.source);
  TEST_ASSERT_NOT_EQUAL(0, e.key);

  TEST_ASSERT_TRUE(parseEventArgs("2026-10-01T09:00:00Z +45m leave=10 loc=\"Room 4.12\" Caf\xC3\xA9 sync  ",
                                  TZ, Source::Pushed, e, &err));
  TEST_ASSERT_EQUAL_INT64(NOW + H, e.start);
  TEST_ASSERT_EQUAL_INT64(45 * M, e.end - e.start);
  TEST_ASSERT_EQUAL_UINT8(10, e.leaveMin);
  TEST_ASSERT_EQUAL_STRING("Room 4.12", e.location);
  TEST_ASSERT_EQUAL_STRING("Cafe sync", e.title);

  TEST_ASSERT_TRUE(parseEventArgs("2026-10-02 2026-10-04 Offsite", TZ, Source::Pushed, e, &err));
  TEST_ASSERT_TRUE(isAllDay(e));
  TEST_ASSERT_EQUAL_INT64(civilToSeconds(2026, 10, 2, 0, 0, 0), e.start);
  TEST_ASSERT_EQUAL_INT64(2 * kDay, e.end - e.start);

  TEST_ASSERT_FALSE(parseEventArgs("2026-10-01T10:00 2026-10-01T09:00 Backwards", TZ, Source::Pushed, e, &err));
  TEST_ASSERT_EQUAL_STRING("end is before start", err);
  TEST_ASSERT_FALSE(parseEventArgs("2026-10-01T10:00 +30m", TZ, Source::Pushed, e, &err));
  TEST_ASSERT_FALSE(parseEventArgs("soon later Thing", TZ, Source::Pushed, e, &err));
  TEST_ASSERT_FALSE(parseEventArgs("2026-10-01T10:00 +30m leave=999 X", TZ, Source::Pushed, e, &err));

  // Web form path.
  TEST_ASSERT_TRUE(makeEvent("Pick up kids", "2026-10-01", "15:30", "", 20, 10, "School", TZ,
                             Source::Manual, 7, e, &err));
  TEST_ASSERT_EQUAL_INT64(civilToSeconds(2026, 10, 1, 14, 30, 0), e.start);
  TEST_ASSERT_EQUAL_INT64(20 * M, e.end - e.start);
  TEST_ASSERT_EQUAL_UINT8(10, e.leaveMin);
  TEST_ASSERT_TRUE(makeEvent("Late show", "2026-10-01", "23:30", "00:30", 0, 0, "", TZ,
                             Source::Manual, 8, e, &err));
  TEST_ASSERT_EQUAL_INT64(H, e.end - e.start);                // crosses midnight
  TEST_ASSERT_FALSE(makeEvent("", "2026-10-01", "10:00", "", 30, 0, "", TZ, Source::Manual, 9, e, &err));
}

// A day in the life of the sleep planner: meetings (incl. back-to-back, two
// at once, one just after midnight), 5-min and at-start alerts, a 30-min
// background sync with the night pause, syncs that take 20 s, and a sleep
// timer that runs 3% fast, exact, or 3% slow. Every alert must land within a
// few seconds, syncs must never crowd an alert, and there must be no wake loops.
void test_sleep_wake_simulation() {
  const int64_t day = civilToSeconds(2026, 10, 1, 7, 0, 0);     // 08:00 local
  struct Mt { int h, m, dur; } plan[] = {{9, 30, 15}, {10, 0, 60}, {11, 0, 30}, {11, 0, 45}, {11, 30, 30},
                                          {13, 7, 23}, {16, 45, 60}, {23, 58, 30}, {24, 5, 20}};
  const int nEv = sizeof(plan) / sizeof(plan[0]);
  Event ev[16];
  for (int i = 0; i < nEv; i++) {
    int64_t st = day + (int64_t)(plan[i].h - 8) * H + plan[i].m * M;
    char t[16];
    snprintf(t, sizeof(t), "M%d", i);
    ev[i] = mk(t, st, st + plan[i].dur * M);
  }
  sortEvents(ev, nEv);
  AlertSettings s;
  s.offsetMask = (1u << 3) | (1u << 0);                          // 5 min before + at start
  Snooze sn;
  std::vector<int64_t> expected;
  for (int i = 0; i < nEv; i++) {
    for (int64_t a : {ev[i].start - 5 * M, ev[i].start}) {
      bool dup = false;
      for (int64_t e : expected) dup |= e == a;
      if (!dup) expected.push_back(a);
    }
  }
  for (double drift : {0.94, 0.97, 1.0, 1.03, 1.06}) {
    int64_t t = day, end = day + 20 * H, W = t;
    SyncState st;
    st.lastAttempt = t;
    int wakes = 0, syncs = 0;
    std::vector<int64_t> firedAt, firedFor, syncAt;
    while (t < end) {
      AlertHit h;
      int64_t aAt = nextAlert(ev, nEv, s, sn, W, h) ? h.at : kNever;
      WakePlan p = planSleep(t, aAt, nextSyncTime(st, 30, true, TZ, t));
      TEST_ASSERT_TRUE(p.reason != WakeReason::None);
      int64_t sleepFor = p.wakeAt - t;
      TEST_ASSERT_TRUE(sleepFor >= 1);
      t += (int64_t)llround((double)sleepFor * drift);         // the RC timer's idea of it
      if (t >= end) break;
      wakes++;
      aAt = nextAlert(ev, nEv, s, sn, W, h) ? h.at : kNever;
      WakeAction act = decideWake(t, aAt, nextSyncTime(st, 30, true, TZ, t));
      if (act == WakeAction::Boot) {
        int64_t fire = t + 1 > aAt ? t + 1 : aAt;                // UI up; TimerExpired at the deadline
        AlertHit due[8];
        int k = dueAlerts(ev, nEv, s, sn, W, fire, due, 8);
        TEST_ASSERT_TRUE(k > 0);
        for (int i = 0; i < k; i++) { firedAt.push_back(fire); firedFor.push_back(due[i].at); }
        W = fire;
        t = fire + 30;                                           // glance, then sleep again
      } else if (act == WakeAction::Sync) {
        syncs++;
        syncAt.push_back(t);
        st.lastAttempt = t;
        t += 20;
      }
    }
    // Every alert fired, on time.
    for (int64_t a : expected) {
      if (a <= day || a >= end) continue;
      bool ok = false;
      for (size_t i = 0; i < firedFor.size(); i++)
        if (firedFor[i] == a && firedAt[i] >= a - 3 && firedAt[i] <= a + 4) ok = true;
      if (!ok) {
        char msg[96];
        snprintf(msg, sizeof(msg), "alert at +%lld s missed or late (drift %.2f)", (long long)(a - day), drift);
        TEST_FAIL_MESSAGE(msg);
      }
    }
    // No sync in the 90 s before an alert, none during the night pause.
    for (int64_t sAt : syncAt) {
      for (int64_t a : expected) TEST_ASSERT_FALSE(sAt > a - 90 && sAt < a);
      Civil c = secondsToCivil(sAt + TZ * 60);
      TEST_ASSERT_TRUE(c.hour >= 6 && c.hour < 23);
    }
    // Roughly every 30 min from 08:00 to 23:00, and no wake storms.
    TEST_ASSERT_TRUE(syncs >= 26 && syncs <= 32);
    TEST_ASSERT_TRUE(wakes <= syncs + 4 * (int)expected.size() + 8);
  }
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_face_far_off);
  RUN_TEST(test_face_ring_drains_linearly);
  RUN_TEST(test_face_urgency_stages);
  RUN_TEST(test_face_in_meeting_and_next);
  RUN_TEST(test_face_back_to_back);
  RUN_TEST(test_face_short_meeting_inside_long_one);
  RUN_TEST(test_face_overlapping_picks_latest_started);
  RUN_TEST(test_face_clear_and_all_day);
  RUN_TEST(test_face_leave_buffer);
  RUN_TEST(test_face_same_start);
  RUN_TEST(test_alert_default_five_minutes);
  RUN_TEST(test_alert_multiple_offsets_and_leave);
  RUN_TEST(test_alert_leave_replaces_same_time_offset);
  RUN_TEST(test_alert_across_midnight);
  RUN_TEST(test_alert_back_to_back_and_simultaneous);
  RUN_TEST(test_alert_due_window_and_lateness);
  RUN_TEST(test_snooze);
  RUN_TEST(test_all_day_never_alerts);
  RUN_TEST(test_wake_plan_alert_and_prewake);
  RUN_TEST(test_wake_plan_sync_heartbeat_none);
  RUN_TEST(test_sync_schedule);
  RUN_TEST(test_sleep_wake_simulation);
  RUN_TEST(test_merge_dedupe_prune);
  RUN_TEST(test_text_formatting);
  RUN_TEST(test_iso_and_durations);
  RUN_TEST(test_event_protocol);
  return UNITY_END();
}
