#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <string.h>
#include "mc_views.h"
#include "mc_ui.h"
#include "mc_haptic.h"
#include "mc_sync.h"
#include "mc_text.h"
#include "display.h"
#include "haptic.h"
#include "model.h"
#include "storage.h"

using namespace mc;

// Render-task scratch for snapshots (PSRAM, allocated on first use).
static mc::Event *evBuf() {
  static mc::Event *p = nullptr;
  if (!p) p = mcapp::allocEvents(mcapp::kMaxMerged);
  return p;
}
static int snap(mc::Event *&ev) {
  ev = evBuf();
  return ev ? mcapp::snapshot(ev, mcapp::kMaxMerged) : 0;
}
static volatile bool gFirstFrame = false;
static MeetingAlertView *gAlertView = nullptr;
static bool gSyncOnAgendaEnter = false;

bool mcViewsFirstFrameDone() { return gFirstFrame; }

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
static bool canvas(Canvas &out) {
  Arduino_Canvas *cv = frameCanvas();
  if (!cv) return false;
  out.px = cv->getFramebuffer();
  out.w = 240;
  out.h = 280;
  return out.px != nullptr;
}

static void present() {
  Arduino_Canvas *cv = frameCanvas();
  if (cv) cv->flush();
  gFirstFrame = true;
}

// Without the PSRAM canvas, at least show the time and the next event.
static void fallback(const char *a, const char *b) {
  if (!gfx) return;
  gfx->fillScreen(BLACK);
  gfx->setTextColor(WHITE, BLACK);
  gfx->setTextSize(3);
  gfx->setCursor(30, 100);
  gfx->print(a);
  gfx->setTextSize(2);
  gfx->setCursor(10, 160);
  gfx->print(b);
  gFirstFrame = true;
}

static inline uint32_t mix(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }
static uint32_t mixStr(uint32_t h, const char *s) {
  while (*s) h = mix(h, (uint8_t)*s++);
  return h;
}

static bool inRect(const UiRect &r, const ::Event &e, int pad = 6) { return r.hit(e.x, e.y, pad); }

// ---------------------------------------------------------------------------
// Face
// ---------------------------------------------------------------------------
void MeetingFaceView::onEnter() {
  lastSig = 0;
  lastDraw = 0;
  pressActive = false;
}

void MeetingFaceView::render() {
  int64_t now = 0;
  bool ok = mcapp::nowUtc(now);
  int tz = mcapp::tzOffsetMin();
  mcapp::Settings set = mcapp::settings();
  mc::Event *gEv;
  int n = snap(gEv);

  FaceModel m;
  m.now = now;
  m.tz = tz;
  m.rtcOk = ok;
  m.leadMin = set.leadMin;
  m.ev = gEv;
  m.n = n;
  m.fi = computeFace(gEv, n, now, tz, set.leadMin);
  m.alertsOn = set.alertsOn != 0;
  mcapp::statusLine(now, m.status.text, sizeof(m.status.text), m.status.warn);
  if (mcsync::running()) {
    mcsync::Progress p = mcsync::progress();
    copyStr(m.status.text, sizeof(m.status.text), p.step);
    m.status.warn = false;
  } else if (!m.status.text[0] && n == 0 && mcapp::feedCount() == 0) {
    copyStr(m.status.text, sizeof(m.status.text), "tap to add your calendar");   // first run
  }
  uint32_t ms = millis();
  m.phase = (float)(ms % 1000) / 1000.0f;

  // Redraw only when something visible changed.
  uint32_t h = 2166136261u;
  h = mix(h, (uint32_t)((now + (int64_t)tz * 60) / 60));
  h = mix(h, ok);
  h = mix(h, (uint32_t)m.fi.mode);
  h = mix(h, m.fi.cur >= 0 ? gEv[m.fi.cur].key : 0);
  h = mix(h, m.fi.next >= 0 ? gEv[m.fi.next].key : 0);
  h = mix(h, m.fi.later >= 0 ? gEv[m.fi.later].key : 0);
  h = mix(h, (uint32_t)(m.fi.ring * 2000.0f));
  h = mix(h, m.fi.remaining < 60 ? (uint32_t)m.fi.remaining : (uint32_t)((m.fi.remaining + 59) / 60));
  h = mix(h, (uint32_t)m.fi.nAllDay);
  h = mixStr(h, m.status.text);
  h = mix(h, m.status.warn);
  h = mix(h, m.alertsOn);
  if (m.fi.finalMinute) h = mix(h, ms / 90);       // pulse animation
  if (h == lastSig && ms - lastDraw < 60000) return;

  Canvas c;
  if (!canvas(c)) {
    char t[8];
    Civil cv = secondsToCivil(now + (int64_t)tz * 60);
    snprintf(t, sizeof(t), "%02d:%02d", cv.hour, cv.minute);
    fallback(t, m.fi.next >= 0 ? gEv[m.fi.next].title : "");
    lastSig = h;
    lastDraw = ms;
    return;
  }
  drawFace(c, m);
  present();
  lastSig = h;
  lastDraw = ms;
}

void MeetingFaceView::onEvent(const ::Event &e) {
  if (e.type == EventType::Gesture) {
    if (e.gesture == Gesture::SwipeUp || e.gesture == Gesture::SwipeLeft) {
      pressActive = false;
      hapticBuzz(60, 70);
      switchTo(Screen::AppList);
    } else if (e.gesture == Gesture::SwipeDown) {
      pressActive = false;
      hapticBuzz(50, 50);
      switchTo(Screen::MeetingAgenda);
    }
    return;
  }
  if (e.type == EventType::Touch) {
    pressActive = true;
    pressMoved = false;
    pressX = e.x;
    pressY = e.y;
    pressMs = millis();
    return;
  }
  if (e.type == EventType::TouchHold && pressActive) {
    int dx = (int)e.x - pressX, dy = (int)e.y - pressY;
    if (dx * dx + dy * dy > 16 * 16) pressMoved = true;
    return;
  }
  if (e.type == EventType::TouchUp) {
    bool tap = pressActive && !pressMoved && millis() - pressMs < 600;
    pressActive = false;
    if (tap) {
      hapticBuzz(50, 45);
      switchTo(Screen::MeetingAgenda);
    }
  }
}

// ---------------------------------------------------------------------------
// Alert
// ---------------------------------------------------------------------------
static const uint32_t kAlertRingMs = 45000;

void MeetingAlertView::onEnter() {
  gAlertView = this;
  startMs = millis();
  lastDraw = 0;
  pressed = -1;
  done = false;
  mcapp::ActiveAlert a = mcapp::activeAlert();
  if (!a.active) { done = true; return; }
  int64_t now = 0;
  mcapp::nowUtc(now);
  mchaptic::Pattern p = a.kind == AlertKind::Leave ? mchaptic::Pattern::Leave
                        : (a.ev.start - now <= 30 ? mchaptic::Pattern::Start : mchaptic::Pattern::Alert);
  mchaptic::play(p, 3, 2600);
}

void MeetingAlertView::onExit() {
  mchaptic::stop();
  done = true;
}

bool MeetingAlertView::holding() const { return !done && millis() - startMs < kAlertRingMs; }

void MeetingAlertView::finish(bool snooze) {
  mchaptic::stop();
  hapticBuzz(80, 60);
  if (snooze) mcapp::snoozeActive();
  else mcapp::dismissActive();
  done = true;
  switchTo(Screen::Watch);
}

static bool alertCanSnooze(const mcapp::ActiveAlert &a, int64_t now, int snoozeMin) {
  return a.test || now + (int64_t)snoozeMin * 60 < a.ev.end;
}

void MeetingAlertView::render() {
  if (done) { switchTo(Screen::Watch); return; }
  uint32_t ms = millis();
  if (ms - startMs > kAlertRingMs) {
    // Nobody looked: stop ringing, leave the alert handled.
    mchaptic::stop();
    mcapp::dismissActive();
    done = true;
    switchTo(Screen::Watch);
    return;
  }
  if (lastDraw && ms - lastDraw < 66) return;
  mcapp::ActiveAlert a = mcapp::activeAlert();
  if (!a.active) { done = true; switchTo(Screen::Watch); return; }
  int64_t now = 0;
  mcapp::nowUtc(now);
  mcapp::Settings set = mcapp::settings();
  AlertModel m;
  m.now = now;
  m.tz = mcapp::tzOffsetMin();
  m.ev = a.ev;
  m.kind = a.kind;
  m.more = a.more;
  m.phase = (float)(ms % 1400) / 1400.0f;
  m.snoozeMin = set.snoozeMin;
  m.pressed = pressed;
  m.canSnooze = alertCanSnooze(a, now, set.snoozeMin);
  Canvas c;
  if (!canvas(c)) { fallback("MEETING", a.ev.title); lastDraw = ms; return; }
  drawAlert(c, m);
  present();
  lastDraw = ms;
}

void MeetingAlertView::onEvent(const ::Event &e) {
  if (done) return;
  if (e.type == EventType::ButtonShort) { finish(false); return; }
  if (e.type == EventType::Touch) {
    mchaptic::stop();                              // they've noticed
    mcapp::ActiveAlert a = mcapp::activeAlert();
    int64_t now = 0;
    mcapp::nowUtc(now);
    bool canSnooze = alertCanSnooze(a, now, mcapp::settings().snoozeMin);
    if (canSnooze && inRect(alertSnoozeRect(), e)) pressed = 0;
    else if (inRect(alertDismissRect(), e) || (!canSnooze && e.y >= alertDismissRect().y - 6)) pressed = 1;
    else pressed = -1;
    lastDraw = 0;
    return;
  }
  if (e.type == EventType::TouchUp) {
    int8_t p = pressed;
    pressed = -1;
    if (p == 0) finish(true);
    else if (p == 1) finish(false);
  }
}

// ---------------------------------------------------------------------------
// Agenda
// ---------------------------------------------------------------------------
static void agendaNote(char *note, uint32_t &until, const char *text) {
  copyStr(note, 48, text);
  until = millis() + 4000;
}

static void startSync(char *note, uint32_t &until) {
  if (mcapp::feedCount() == 0) { agendaNote(note, until, "add a calendar feed first (Settings)"); return; }
  if (Storage::knownCount() == 0) { agendaNote(note, until, "save a WiFi network first"); return; }
  if (!mcsync::startAsync() && !mcsync::running()) agendaNote(note, until, "could not start sync");
}

void MeetingAgendaView::onEnter() {
  lastSig = 0;
  first = 0;
  pressed = -1;
  note[0] = '\0';
  if (gSyncOnAgendaEnter) {
    gSyncOnAgendaEnter = false;
    startSync(note, noteUntil);
  }
}

void MeetingAgendaView::render() {
  int64_t now = 0;
  mcapp::nowUtc(now);
  int tz = mcapp::tzOffsetMin();
  mc::Event *gEv;
  int n = snap(gEv);
  int rows = agendaRowsVisible();
  if (first > n - rows) first = n - rows;
  if (first < 0) first = 0;
  uint32_t ms = millis();

  AgendaModel m;
  m.now = now;
  m.tz = tz;
  m.ev = gEv;
  m.n = n;
  m.first = first;
  ThemeColors t = theme();
  m.bg = rgb888(t.bg);
  m.fg = rgb888(t.fg);
  m.accent = rgb888(t.accent);
  m.syncing = mcsync::running();
  mcsync::Progress pr = mcsync::progress();
  m.syncStep = pr.step;
  m.phase = (float)(ms % 1000) / 1000.0f;
  if (note[0] && (int32_t)(noteUntil - ms) > 0) {
    copyStr(m.status.text, sizeof(m.status.text), note);
    m.status.warn = true;
  } else {
    note[0] = '\0';
    mcapp::statusLine(now, m.status.text, sizeof(m.status.text), m.status.warn);
    if (!m.status.text[0] && mcapp::feedCount() == 0) copyStr(m.status.text, sizeof(m.status.text), "no calendar feed yet");
  }
  m.pressed = (ms - pressedAt < 160) ? pressed : -1;

  uint32_t h = 2166136261u;
  h = mix(h, (uint32_t)n);
  h = mix(h, (uint32_t)first);
  for (int i = first; i < first + rows && i < n; i++) h = mix(h, gEv[i].key);
  h = mix(h, (uint32_t)((now + (int64_t)tz * 60) / 60));
  h = mix(h, m.syncing);
  h = mixStr(h, m.syncStep);
  h = mixStr(h, m.status.text);
  h = mix(h, (uint32_t)(m.pressed + 1));
  h = mix(h, t.bg ^ (t.fg << 16));
  h = mix(h, t.accent);
  if (m.syncing) h = mix(h, ms / 110);
  if (h == lastSig && ms - lastDraw < 30000) return;
  Canvas c;
  if (!canvas(c)) { fallback("Up next", n ? gEv[first].title : "nothing"); lastSig = h; return; }
  drawAgenda(c, m);
  present();
  lastSig = h;
  lastDraw = ms;
}

void MeetingAgendaView::onEvent(const ::Event &e) {
  if (e.type == EventType::ButtonShort) { switchTo(Screen::Watch); return; }
  if (e.type == EventType::Gesture) {
    int n = mcapp::countSource(Source::Feed) + mcapp::countSource(Source::Manual) + mcapp::countSource(Source::Pushed);
    if (e.gesture == Gesture::SwipeUp && first + agendaRowsVisible() < n) { first++; hapticBuzz(30, 30); }
    else if (e.gesture == Gesture::SwipeDown && first > 0) { first--; hapticBuzz(30, 30); }
    return;
  }
  if (e.type != EventType::Touch) return;
  if (inRect(agendaBackRect(), e, 0)) { hapticBuzz(60, 50); switchTo(Screen::Watch); return; }
  if (inRect(agendaSyncRect(), e, 0)) {
    hapticBuzz(60, 50);
    pressed = 1;
    pressedAt = millis();
    startSync(note, noteUntil);
    return;
  }
  if (inRect(agendaSettingsRect(), e, 4)) {
    hapticBuzz(60, 50);
    switchTo(Screen::MeetingSettings);
    return;
  }
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
static const uint8_t kLeads[] = {30, 45, 60, 90, 120};
static const uint8_t kSyncs[] = {0, 15, 30, 60, 120};
enum : int { R_ALERT, R_LEAD, R_SYNC, R_FACE, R_SYNCNOW, R_TEST, R_COUNT };

static int alertIndex(const mcapp::Settings &s) {
  if (!s.alertsOn) return -1;
  for (int i = 0; i < kNumAlertOffsets; i++) if (s.alertMask & (1u << i)) return i;
  return -1;
}

static void alertText(const mcapp::Settings &s, char *out, size_t n) {
  if (!s.alertsOn || !s.alertMask) { copyStr(out, n, "off"); return; }
  int bits = 0;
  for (int i = 0; i < kNumAlertOffsets; i++) if (s.alertMask & (1u << i)) bits++;
  if (bits > 1) {
    char tmp[20] = "";                 // "30,15,10,5,2,1,0" at most
    for (int i = kNumAlertOffsets - 1; i >= 0; i--) {
      if (!(s.alertMask & (1u << i))) continue;
      char part[8];
      snprintf(part, sizeof(part), tmp[0] ? ",%u" : "%u", kAlertOffsets[i]);
      strncat(tmp, part, sizeof(tmp) - strlen(tmp) - 1);
    }
    snprintf(out, n, "%s min", tmp);
    return;
  }
  int i = alertIndex(s);
  if (kAlertOffsets[i] == 0) copyStr(out, n, "at start");
  else snprintf(out, n, "%u min before", kAlertOffsets[i]);
}

static int indexOf(const uint8_t *arr, int n, uint8_t v) {
  for (int i = 0; i < n; i++) if (arr[i] == v) return i;
  return -1;
}

void MeetingSettingsView::onEnter() {
  lastSig = 0;
  pressed = -1;
}

void MeetingSettingsView::render() {
  mcapp::Settings s = mcapp::settings();
  SettingsModel m;
  m.title = "Meetings";
  m.n = R_COUNT;
  m.rows[R_ALERT].label = "Alert";
  alertText(s, m.rows[R_ALERT].value, sizeof(m.rows[R_ALERT].value));
  m.rows[R_LEAD].label = "Ring window";
  snprintf(m.rows[R_LEAD].value, sizeof(m.rows[R_LEAD].value), "%u min", s.leadMin);
  m.rows[R_SYNC].label = "Auto sync";
#if defined(MC_BACKGROUND_SYNC) && MC_BACKGROUND_SYNC
  if (s.syncMin == 0) copyStr(m.rows[R_SYNC].value, sizeof(m.rows[R_SYNC].value), "off");
  else if (s.syncMin % 60 == 0) snprintf(m.rows[R_SYNC].value, sizeof(m.rows[R_SYNC].value), "every %u h", s.syncMin / 60);
  else snprintf(m.rows[R_SYNC].value, sizeof(m.rows[R_SYNC].value), "every %u min", s.syncMin);
#else
  copyStr(m.rows[R_SYNC].value, sizeof(m.rows[R_SYNC].value), "manual");
#endif
  m.rows[R_FACE].label = "Watch face";
  copyStr(m.rows[R_FACE].value, sizeof(m.rows[R_FACE].value), s.faceClassic ? "Classic" : "Meeting");
  m.rows[R_SYNCNOW].label = "Sync now";
  m.rows[R_SYNCNOW].action = true;
  m.rows[R_TEST].label = "Test alert";
  m.rows[R_TEST].action = true;

  bool en, conn;
  WifiMode mode;
  uint32_t ip;
  { ModelLock lk; en = model.wifiEnabled; mode = model.wifiMode; conn = model.wifiConnected; ip = model.wifiIpV4; }
  static char f2[40];
#if defined(EWATCH_ENABLE_WIFI) && EWATCH_ENABLE_WIFI
  if (!en) {
    m.footer = "Add calendars from a browser:";
    m.footer2 = "Settings > WiFi, turn it on";
  } else if (mode == WifiMode::AP) {
    m.footer = "Join WiFi EWATCH_SETUP, then open";
    m.footer2 = "http://192.168.4.1/meet";
  } else if (conn && ip) {
    snprintf(f2, sizeof(f2), "http://%u.%u.%u.%u/meet", (unsigned)(ip & 0xFF), (unsigned)((ip >> 8) & 0xFF),
             (unsigned)((ip >> 16) & 0xFF), (unsigned)((ip >> 24) & 0xFF));
    m.footer = "Calendars & events in a browser:";
    m.footer2 = f2;
  } else {
    m.footer = "Joining your WiFi...";
    m.footer2 = "then http://ewatch.local/meet";
  }
#else
  (void)en; (void)conn; (void)mode; (void)ip; (void)f2;
  m.footer = "Push events over USB:";
  m.footer2 = "tools/push_events.py";
#endif
  ThemeColors t = theme();
  m.bg = rgb888(t.bg);
  m.fg = rgb888(t.fg);
  m.accent = rgb888(t.accent);
  uint32_t ms = millis();
  m.pressed = (ms - pressedAt < 160) ? pressed : -1;

  uint32_t h = 2166136261u;
  for (int i = 0; i < m.n; i++) h = mixStr(h, m.rows[i].value);
  h = mixStr(h, m.footer);
  h = mixStr(h, m.footer2);
  h = mix(h, (uint32_t)(m.pressed + 2));
  h = mix(h, t.bg ^ (t.fg << 16));
  h = mix(h, t.accent);
  if (h == lastSig) return;
  Canvas c;
  if (!canvas(c)) { fallback("Meetings", m.footer2); lastSig = h; return; }
  drawSettings(c, m);
  present();
  lastSig = h;
}

void MeetingSettingsView::onEvent(const ::Event &e) {
  if (e.type == EventType::ButtonShort) { switchTo(Screen::MeetingAgenda); return; }
  if (e.type != EventType::Touch) return;
  if (inRect(settingsBackRect(), e, 0)) { hapticBuzz(60, 50); switchTo(Screen::MeetingAgenda); return; }
  int row = -1;
  for (int i = 0; i < R_COUNT; i++) if (inRect(settingsRowRect(i), e, 1)) { row = i; break; }
  if (row < 0) return;
  hapticBuzz(50, 45);
  pressed = (int8_t)row;
  pressedAt = millis();
  mcapp::Settings s = mcapp::settings();
  switch (row) {
    case R_ALERT: {
      int i = alertIndex(s);
      i = (i + 1 >= kNumAlertOffsets) ? -1 : i + 1;
      if (i < 0) s.alertsOn = 0;
      else { s.alertsOn = 1; s.alertMask = (uint8_t)(1u << i); }
      break;
    }
    case R_LEAD: {
      int i = indexOf(kLeads, 5, s.leadMin);
      s.leadMin = kLeads[(i + 1) % 5];
      break;
    }
    case R_SYNC: {
#if defined(MC_BACKGROUND_SYNC) && MC_BACKGROUND_SYNC
      int i = indexOf(kSyncs, 5, s.syncMin);
      s.syncMin = kSyncs[(i + 1) % 5];
#endif
      break;
    }
    case R_FACE:
      s.faceClassic = !s.faceClassic;
      break;
    case R_SYNCNOW:
      gSyncOnAgendaEnter = true;
      switchTo(Screen::MeetingAgenda);
      return;
    case R_TEST:
      mcapp::startTestAlert();
      switchTo(Screen::MeetingAlert);
      return;
  }
  mcapp::setSettings(s);
  { ModelLock lk; model.revision++; }
}

// ---------------------------------------------------------------------------
// shared
// ---------------------------------------------------------------------------
bool mcClassicFaceLine(char *buf, size_t n) {
  int64_t now;
  if (!mcapp::nowUtc(now)) return false;
  int tz = mcapp::tzOffsetMin();
  mc::Event *gEv;
  int cnt = snap(gEv);
  FaceInfo fi = computeFace(gEv, cnt, now, tz, mcapp::settings().leadMin);
  // The classic face prints this in the 12 px bitmap font: <= 20 characters.
  char d[16], t[24];
  if (fi.mode == FaceMode::InMeeting) {
    fmtDurShort(fi.remaining, d, sizeof(d));
    copyStr(t, sizeof(t), gEv[fi.cur].title);
    t[10] = '\0';
    snprintf(buf, n, "%s %s left", t, d);
    return true;
  }
  if (fi.next >= 0) {
    fmtDurShort(fi.remaining, d, sizeof(d));
    copyStr(t, sizeof(t), gEv[fi.next].title);
    t[12] = '\0';
    snprintf(buf, n, "%s in %s", t, d);
    return true;
  }
  return false;
}

bool mcViewsHoldAwake() {
  Screen s;
  { ModelLock lk; s = model.screen; }
  return s == Screen::MeetingAlert && gAlertView && gAlertView->holding();
}
