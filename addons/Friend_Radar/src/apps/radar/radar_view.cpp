#include "radar_view.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <stdio.h>
#include <string.h>
#include "controller.h"
#include "display.h"
#include "haptic.h"
#include "model.h"
#include "fr_anim.h"
#include "fr_motion.h"
#include "fr_radar.h"
#include "fr_screens.h"
#include "radar_service.h"

// Set to 1 to print render/flush timing to Serial every 5 s.
#ifndef FR_PERF_LOG
#define FR_PERF_LOG 0
#endif

using namespace fr;

namespace {

enum class Page : uint8_t { Radar, Menu, Mates, MateDetail, Keyboard, Calibrate, Help };
enum class KbTarget : uint8_t { MyName, MateNick };
enum MenuItem : uint8_t { MiMates, MiName, MiCalibrate, MiAlerts, MiBackground, MiPeriod, MiHelp, MiNewId };

const uint32_t kRadarFrameMs   = 50;      // 20 fps dial
const uint32_t kMotionFrameMs  = 16;      // as fast as the panel takes while things move
const uint32_t kAnimFrameMs    = 33;      // 30 fps during shared animations
const uint32_t kBootMs         = 900;     // the scope's power-on sequence
const uint32_t kDimAfterMs     = 120000;  // dim the backlight after 2 min untouched
const uint32_t kLeaveAfterMs   = 900000;  // leave the app after 15 min untouched
const uint32_t kAlertLingerMs  = 25000;   // alert session: sleep this long after the animation
const uint32_t kConfirmMs      = 3000;    // "Sure?" confirmations expire
const uint16_t kCalTarget      = 60;      // calibration readings
const uint32_t kCalTimeoutMs   = 15000;

struct State {
  // Rendering
  Canvas      cv;
  RadarScene  scene;
  bool        sceneOk = false;
  uint16_t   *bgBuf = nullptr, *polarBuf = nullptr, *overlayBuf = nullptr, *cleanBuf = nullptr;
  uint16_t   *backdropBuf = nullptr, *underBuf = nullptr;
  bool        backdropOk = false;
  BlipAnimator anim;
  Blip        blips[kMaxPeers];
  int         nBlips = 0;
  PeerView    peers[kMaxPeers];
  int         nPeers = 0;
  uint32_t    lastFrameMs = 0;
  uint32_t    lastStepMs = 0;
  uint32_t    lastPollMs = 0;
  uint32_t    bootStartMs = 0;
  bool        dirty = true;
  RadioState  lastRadio = RadioState::Off;
  // Motion
  Spring      card{14.f, 0.74f};          // 0 hidden .. 1 open; also the camera lock-on
  Spring      focusX{12.f, 0.9f}, focusY{12.f, 0.9f};   // overview point the camera locks onto
  Spring      page{15.f, 0.86f};          // page slide 0 .. 1
  bool        pageForward = true;
  bool        pageMoving = false;
  Spring      toastT{16.f, 0.7f};
  Spring      knobAlerts{20.f, 0.7f}, knobBg{20.f, 0.7f};
  uint32_t    focusId = 0;                // blip the camera is (or was) locked onto
  CardInfo    lastCard;                   // kept while the card slides away
  // Navigation
  Page        pg = Page::Radar;
  Page        kbReturn = Page::Menu;
  Page        detailReturn = Page::Mates;
  // Radar card
  uint32_t    selectedId = 0;
  uint8_t     cardPressed = 0;
  bool        cardConfirm = false;
  uint32_t    cardConfirmMs = 0;
  bool        menuPressed = false;
  // Lists
  ListView    menu, mates;
  int         helpScroll = 0;
  MenuRow     menuRows[9];
  uint8_t     menuIds[9];
  int         nMenu = 0;
  bool        newIdConfirm = false;
  uint32_t    newIdConfirmMs = 0;
  MateRowView mateRows[kMaxMates];
  uint32_t    mateIds[kMaxMates];
  int         nMates = 0;
  // Mate detail
  uint32_t    detailId = 0;
  MateDetailView detail;
  uint32_t    detailConfirmMs = 0;
  // Keyboard
  KeyboardView kb;
  KbTarget    kbTarget = KbTarget::MyName;
  uint32_t    kbMateId = 0;
  // Calibration
  CalibrateView cal;
  uint32_t    calStartMs = 0;
  // Shared animation + toast
  PairAnimSpec animSpec;
  bool        animOn = false;
  uint32_t    animStartMs = 0;
  char        toast[40] = {0};
  uint32_t    toastUntil = 0;
  // Touch
  bool        down = false, dragging = false;
  int         downX = 0, downY = 0, lastY = 0;
  uint32_t    downMs = 0;
  // Idle / power
  uint32_t    lastTouchMs = 0;
  bool        dimmed = false;
  bool        alertSession = false;
  uint32_t    alertDeadline = 0;
#if FR_PERF_LOG
  uint32_t    perfDrawUs = 0, perfFlushUs = 0, perfFrames = 0, perfSinceMs = 0;
#endif
};

State S;
bool gAlertSessionNext = false;

void showToast(const char *msg, uint32_t ms = 1800) {
  snprintf(S.toast, sizeof S.toast, "%s", msg);
  S.toastUntil = millis() + ms;
  S.toastT.target = 1.f;
  S.dirty = true;
}

void restoreBrightness() {
  if (!S.dimmed) return;
  uint8_t br;
  { ModelLock lk; br = model.brightness; }
  backlightSet(br);
  S.dimmed = false;
}

void noteInteraction() {
  S.lastTouchMs = millis();
  restoreBrightness();
  if (S.alertSession) S.alertSession = false;      // the user took over
}

// Switch pages. The frame canvas still holds the last frame we showed: keep
// it as the page underneath and slide the new one over it (or the old one off).
void go(Page p, bool forward) {
  if (p != S.pg && S.underBuf && S.cv.px) {
    memcpy(S.underBuf, S.cv.px, dial::kBgPixels * sizeof(uint16_t));
    S.page.snap(0.f);
    S.page.target = 1.f;
    S.pageForward = forward;
    S.pageMoving = true;
  }
  S.pg = p;
  S.dirty = true;
  S.down = false;
  S.dragging = false;
}

// ---------------------------------------------------------------------------
// Data refresh
// ---------------------------------------------------------------------------
const PeerView *findPeer(uint32_t id) {
  for (int i = 0; i < S.nPeers; ++i) if (S.peers[i].id == id) return &S.peers[i];
  return nullptr;
}

void deselect() {
  S.selectedId = 0;
  S.cardConfirm = false;
  S.card.target = 0.f;
  S.dirty = true;
}

void refreshPeers(uint32_t now) {
  S.nPeers = radar::snapshot(S.peers, kMaxPeers);
  S.anim.update(S.peers, S.nPeers, now);
  if (S.selectedId && !findPeer(S.selectedId)) deselect();   // forgotten: close the card
}

void buildMenu() {
  RadarSettings s = radar::settings();
  int n = 0;
  auto row = [&](uint8_t id, MenuRow::Kind k, MenuRow::Icon ic) -> MenuRow & {
    S.menuIds[n] = id;
    MenuRow &r = S.menuRows[n++];
    r = MenuRow();
    r.kind = k;
    r.icon = ic;
    return r;
  };
  static Mate tmp[kMaxMates];            // render task only; keep it off the stack
  int mates = radar::mates(tmp, kMaxMates);
  { MenuRow &r = row(MiMates, MenuRow::Nav, MenuRow::IcPeople); snprintf(r.title, sizeof r.title, "Mates");
    snprintf(r.value, sizeof r.value, "%d", mates); }
  { MenuRow &r = row(MiName, MenuRow::Nav, MenuRow::IcTag); snprintf(r.title, sizeof r.title, "My name");
    snprintf(r.value, sizeof r.value, "%s", s.name); }
  { MenuRow &r = row(MiCalibrate, MenuRow::Nav, MenuRow::IcTarget); snprintf(r.title, sizeof r.title, "Calibrate distance");
    if (s.calibrated) snprintf(r.sub, sizeof r.sub, "%d dBm at 1 m", (int)s.ref1m);
    else snprintf(r.sub, sizeof r.sub, "Using the default"); }
  { MenuRow &r = row(MiAlerts, MenuRow::Toggle, MenuRow::IcBell); snprintf(r.title, sizeof r.title, "Mate alerts");
    snprintf(r.sub, sizeof r.sub, "Buzz + animation"); r.on = s.alerts; r.knob = S.knobAlerts.x; }
#if FR_BACKGROUND_ALERTS
  { MenuRow &r = row(MiBackground, MenuRow::Toggle, MenuRow::IcMoon); snprintf(r.title, sizeof r.title, "Background alerts");
    if (s.bgAlerts) snprintf(r.sub, sizeof r.sub, mates ? "On " FR_MIDDOT " uses extra battery" : "On " FR_MIDDOT " add a mate first");
    else snprintf(r.sub, sizeof r.sub, "Off " FR_MIDDOT " only while open");
    r.on = s.bgAlerts; r.knob = S.knobBg.x; }
  if (s.bgAlerts) {
    MenuRow &r = row(MiPeriod, MenuRow::Nav, MenuRow::IcClock);
    snprintf(r.title, sizeof r.title, "Check every");
    snprintf(r.value, sizeof r.value, s.bgPeriodSec >= 120 ? "2 min" : "%u s", (unsigned)s.bgPeriodSec);
  }
#endif
  { MenuRow &r = row(MiHelp, MenuRow::Nav, MenuRow::IcQuestion); snprintf(r.title, sizeof r.title, "How it works"); }
  { MenuRow &r = row(MiNewId, MenuRow::Danger, MenuRow::IcRefresh); snprintf(r.title, sizeof r.title, "New radar ID");
    snprintf(r.sub, sizeof r.sub, S.newIdConfirm ? "Tap again to confirm" : "Mates must re-add you"); }
  S.nMenu = n;
  listClampScroll(S.menu, n);
}

void buildMates() {
  static Mate m[kMaxMates];               // render task only; keep it off the stack
  int n = radar::mates(m, kMaxMates);
  uint32_t nowS = radar::nowSec();
  for (int i = 0; i < n; ++i) {
    MateRowView &r = S.mateRows[i];
    r = MateRowView();
    S.mateIds[i] = m[i].id;
    snprintf(r.name, sizeof r.name, "%s", m[i].nick);
    r.color = mateColor(m[i].id);
    const PeerView *p = findPeer(m[i].id);
    if (p && p->zone != Zone::Lost) {
      r.live = true;
      r.zone = p->zone;
      r.signal = BlipAnimator::signalFor(p->plDb);
      char d[12];
      formatApproxMeters(p->distM, d, sizeof d);
      snprintf(r.status, sizeof r.status, "%s now " FR_MIDDOT " %s", zoneName(p->zone), d);
    } else if (m[i].lastSeenSec) {
      char ago[20];
      formatAgo(nowS > m[i].lastSeenSec ? nowS - m[i].lastSeenSec : 0, ago, sizeof ago);
      snprintf(r.status, sizeof r.status, "Seen %s", ago);
    } else {
      snprintf(r.status, sizeof r.status, "Not seen yet");
    }
  }
  S.nMates = n;
  listClampScroll(S.mates, n);
}

bool buildDetail() {
  static Mate m;
  if (!radar::mateById(S.detailId, m)) return false;
  MateDetailView &d = S.detail;
  bool confirm = d.confirmRemove;
  uint8_t pressed = d.pressed;
  d = MateDetailView();
  d.confirmRemove = confirm;
  d.pressed = pressed;
  snprintf(d.name, sizeof d.name, "%s", m.nick);
  d.color = mateColor(m.id);
  const PeerView *p = findPeer(m.id);
  uint32_t nowS = radar::nowSec();
  if (p && p->zone != Zone::Lost) {
    d.live = true; d.zone = p->zone; d.distM = p->distM;
    d.signal = BlipAnimator::signalFor(p->plDb);
  } else if (m.lastSeenSec) {
    char ago[20];
    formatAgo(nowS > m.lastSeenSec ? nowS - m.lastSeenSec : 0, ago, sizeof ago);
    snprintf(d.status, sizeof d.status, "Last seen %s", ago);
  } else {
    snprintf(d.status, sizeof d.status, "Not seen since added");
  }
  d.nLog = m.logN;
  for (int i = 0; i < m.logN && i < 6; ++i) {
    formatWhen(m.log[i].startSec, nowS, d.when[i], sizeof d.when[i]);
    formatMinutes(m.log[i].minutes, d.dur[i], sizeof d.dur[i]);
    d.closest[i] = (Zone)m.log[i].closest;
  }
  return true;
}

void startKeyboard(KbTarget target, const char *title, const char *initial, uint32_t mateId) {
  S.kbReturn = S.pg;
  S.kbTarget = target;
  S.kbMateId = mateId;
  S.kb = KeyboardView();
  snprintf(S.kb.title, sizeof S.kb.title, "%s", title);
  snprintf(S.kb.text, sizeof S.kb.text, "%s", initial);
  S.kb.shift = S.kb.text[0] == '\0';
  go(Page::Keyboard, true);
}

void startCalibrationIntro(bool forward) {
  S.cal = CalibrateView();
  RadarSettings s = radar::settings();
  S.cal.currentRef = s.ref1m;
  S.cal.currentCalibrated = s.calibrated;
  S.cal.target = kCalTarget;
  if (S.pg == Page::Calibrate) S.dirty = true;
  else go(Page::Calibrate, forward);
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
void footerText(char *out, size_t cap) {
  RadioState rs = radar::radioState();
  if (rs == RadioState::Error) { snprintf(out, cap, "Bluetooth is unavailable right now"); return; }
  if (rs != RadioState::Live) { snprintf(out, cap, "Starting the radio" FR_ELLIPSIS); return; }
  int live = 0;
  const PeerView *best = nullptr;
  for (int i = 0; i < S.nPeers; ++i) {
    const PeerView &p = S.peers[i];
    if (p.zone == Zone::Lost) continue;
    ++live;
    if (p.mate && (!best || (int)p.zone < (int)best->zone)) best = &p;
  }
  if (!live) { snprintf(out, cap, "Looking for EWatches nearby" FR_ELLIPSIS); return; }
  if (best) {
    static const char *kIs[4] = {"right here", "near", "around", "far away"};
    snprintf(out, cap, "%d nearby " FR_MIDDOT " %s is %s", live, best->label, kIs[(int)best->zone]);
  } else {
    snprintf(out, cap, "%d nearby " FR_MIDDOT " tap a dot to add a mate", live);
  }
}

int barsFor(float plDb) {
  if (plDb < -3.f) return 5;
  if (plDb < 4.f) return 4;
  if (plDb < 12.f) return 3;
  if (plDb < 20.f) return 2;
  if (plDb < 30.f) return 1;
  return 0;
}

PageChrome chrome(uint32_t now) {
  PageChrome pc;
  pc.backdrop = S.backdropOk ? S.backdropBuf : nullptr;
  pc.onAir = radar::radioState() == RadioState::Live;
  pc.tMs = now;
  return pc;
}

void drawRadarPage(uint32_t now) {
  if (!S.sceneOk) {
    PageChrome pc = chrome(now);
    drawBackdrop(S.cv, pc);
    textWrapped(S.cv, kFontM, 20, 130, 200, "Not enough memory to draw the radar.", pal::text, 22, 255, true);
    drawTitleBar(S.cv, pc, "Friend Radar");
    return;
  }
  S.anim.step(now);
  S.nBlips = S.anim.blips(S.blips, kMaxPeers, S.selectedId);
  RadarFrame f;
  f.tMs = now;
  f.beamDeg = (float)(now % 4000) * (360.f / 4000.f);
  f.blips = S.blips;
  f.nBlips = S.nBlips;
  f.hud.radio = radar::radioState();
  RadarSettings s = radar::settings();
  snprintf(f.hud.myName, sizeof f.hud.myName, "%s", s.name);
  f.hud.bgAlerts = s.bgAlerts;
  footerText(f.hud.footer, sizeof f.hud.footer);
  f.boot = S.bootStartMs ? easeOutCubicf((float)(now - S.bootStartMs) / (float)kBootMs) : 1.f;
  f.menuPressed = S.menuPressed;

  // The card and the camera: keep the last card while it slides away.
  if (S.selectedId) {
    const PeerView *p = findPeer(S.selectedId);
    if (p) {
      CardInfo &card = S.lastCard;
      card = CardInfo();
      card.show = true;
      card.id = p->id;
      snprintf(card.name, sizeof card.name, "%s", p->label);
      snprintf(card.advertised, sizeof card.advertised, "%s", p->name);
      card.mate = p->mate;
      card.zone = p->zone;
      card.distM = p->distM;
      card.agoSec = p->ageMs / 1000;
      card.bars = p->zone == Zone::Lost ? 0 : barsFor(p->plDb);
      card.asleep = (p->flags & kFlagBackground) != 0;
      card.color = p->mate ? mateColor(p->id) : pal::stranger;
      card.signal = BlipAnimator::signalFor(p->plDb);
    }
  }
  S.lastCard.confirmRemove = S.cardConfirm;
  S.lastCard.pressed = S.cardPressed;
  float u = S.card.x;
  if (u > 0.0005f && S.focusId) {
    Blip b;
    if (S.anim.find(S.focusId, b)) {
      float bx, by;
      RadarScene::blipOverviewXY(b, bx, by);
      S.focusX.target = bx;
      S.focusY.target = by;
    }
    f.cam = RadarCamera::lockOn(S.focusX.x, S.focusY.x, u);
    f.focusId = S.focusId;
    f.card = S.lastCard;
    f.cardY = (float)layout::H - ((float)layout::H - (float)layout::cardTop) * u;
  }
  if (S.animOn) {
    f.anim = &S.animSpec;
    f.animT = (int32_t)(now - S.animStartMs);
  }
  S.scene.drawFrame(S.cv, f);
}

// A shared animation always plays in the scope: from another page the scope
// slides in for the moment and slides away again afterwards.
void slideScope(bool in) {
  if (S.pg == Page::Radar || !S.underBuf || !S.cv.px) return;
  memcpy(S.underBuf, S.cv.px, dial::kBgPixels * sizeof(uint16_t));
  S.page.snap(0.f);
  S.page.target = 1.f;
  S.pageForward = in;
  S.pageMoving = true;
}

void drawPage(uint32_t now) {
  PageChrome pc = chrome(now);
  switch (S.animOn ? Page::Radar : S.pg) {
    case Page::Radar:      drawRadarPage(now); break;
    case Page::Menu:       buildMenu(); drawMenu(S.cv, pc, "Friend Radar", S.menuRows, S.nMenu, S.menu); break;
    case Page::Mates:      buildMates(); drawMates(S.cv, pc, S.mateRows, S.nMates, S.mates); break;
    case Page::MateDetail:
      if (!buildDetail()) { S.pg = Page::Mates; buildMates(); drawMates(S.cv, pc, S.mateRows, S.nMates, S.mates); break; }
      drawMateDetail(S.cv, pc, S.detail);
      break;
    case Page::Keyboard:   S.kb.tMs = now; drawKeyboard(S.cv, pc, S.kb); break;
    case Page::Calibrate:  S.cal.tMs = now; drawCalibrate(S.cv, pc, S.cal); break;
    case Page::Help:       drawHelp(S.cv, pc, S.helpScroll); break;
  }
  if (S.pageMoving) composeSlide(S.cv, S.underBuf, S.page.x, S.pageForward);
  if (S.toastT.x > 0.01f) drawToast(S.cv, S.toast, S.toastT.x);
}

void stepMotion(uint32_t now) {
  float dt = S.lastStepMs ? (float)(now - S.lastStepMs) * 0.001f : 0.f;
  if (dt > 0.1f) dt = 0.1f;
  S.lastStepMs = now;
  S.card.step(dt);
  S.focusX.step(dt);
  S.focusY.step(dt);
  if (S.card.x <= 0.0005f && S.card.target == 0.f) { S.card.snap(0.f); S.focusId = 0; }
  if (S.pageMoving) {
    S.page.step(dt);
    if (S.page.settled(0.002f) && S.page.target == 1.f) { S.page.snap(1.f); S.pageMoving = false; }
  }
  S.toastT.target = (int32_t)(S.toastUntil - now) > 0 ? 1.f : 0.f;
  S.toastT.step(dt);
  RadarSettings s = radar::settings();
  S.knobAlerts.target = s.alerts ? 1.f : 0.f;
  S.knobBg.target = s.bgAlerts ? 1.f : 0.f;
  S.knobAlerts.step(dt);
  S.knobBg.step(dt);
  if (S.bootStartMs && now - S.bootStartMs >= kBootMs) S.bootStartMs = 0;
}

bool moving() {
  return S.pageMoving || !S.card.settled() || !S.toastT.settled() || !S.knobAlerts.settled() ||
         !S.knobBg.settled() || S.bootStartMs != 0;
}

bool animating(uint32_t now) {
  return S.pg == Page::Radar || S.animOn || moving() || (int32_t)(S.toastUntil - now) > 0 ||
         S.pg == Page::Keyboard || S.pg == Page::MateDetail || S.pg == Page::Mates ||
         (S.pg == Page::Calibrate && S.cal.phase == CalibrateView::Measuring);
}

uint32_t frameInterval() {
  if (moving()) return kMotionFrameMs;
  if (S.animOn) return kAnimFrameMs;
  if (S.pg == Page::Radar) return kRadarFrameMs;
  if (S.pg == Page::Keyboard) return 250;         // cursor blink
  if (S.pg == Page::Calibrate) return 100;
  if (S.pg == Page::Mates || S.pg == Page::MateDetail) return 150;   // breathing lights
  return 1000;
}

// ---------------------------------------------------------------------------
// Periodic logic (runs every render() call, cheap)
// ---------------------------------------------------------------------------
void poll(uint32_t now) {
  // Pending shared animation from the radar service.
  PlayCmd cmd;
  char name[kMaxName + 1];
  if (radar::takePlay(cmd, name)) {
    bool same = S.animOn && S.animSpec.seed == cmd.seed && S.animSpec.kind == cmd.kind;
    if (!(cmd.resync && !same)) {
      S.animSpec.kind = cmd.kind;
      S.animSpec.seed = cmd.seed;
      S.animSpec.idA = radar::myId();
      S.animSpec.idB = cmd.peerId;
      snprintf(S.animSpec.name, sizeof S.animSpec.name, "%s", name);
      S.animStartMs = cmd.startMs;
      if (!S.animOn) slideScope(true);
      S.animOn = true;
      S.dirty = true;
      if (S.pg == Page::Radar && S.selectedId) deselect();   // give the moment the whole scope
      if (!S.alertSession) S.lastTouchMs = now;     // an alert counts as activity
      restoreBrightness();
    }
  }
  if (S.animOn && (int32_t)(now - S.animStartMs) >= (int32_t)animDurationMs(S.animSpec.kind)) {
    S.animOn = false;
    slideScope(false);
    S.dirty = true;
    if (S.alertSession) S.alertDeadline = now + kAlertLingerMs;
  }
  // Radio state drives the on-air lamp on every page.
  RadioState rs = radar::radioState();
  if (rs != S.lastRadio) { S.lastRadio = rs; S.dirty = true; }
  // Peers for the radar, mate lists and calibration.
  uint32_t pollEvery = S.pg == Page::Radar ? 0 : 250;
  if (now - S.lastPollMs >= pollEvery) {
    S.lastPollMs = now;
    refreshPeers(now);
    if (S.pg == Page::Mates || S.pg == Page::MateDetail) S.dirty = true;
  }
  // Calibration progress.
  if (S.pg == Page::Calibrate) {
    if (S.cal.phase == CalibrateView::Intro) {
      const PeerView *best = nullptr;
      for (int i = 0; i < S.nPeers; ++i)
        if (S.peers[i].ageMs < 2500 && (!best || S.peers[i].rssi > best->rssi)) best = &S.peers[i];
      bool avail = best != nullptr;
      if (avail != S.cal.peerAvailable || (best && strcmp(best->label, S.cal.peer) != 0)) {
        S.cal.peerAvailable = avail;
        if (best) snprintf(S.cal.peer, sizeof S.cal.peer, "%s", best->label);
        S.dirty = true;
      }
    } else if (S.cal.phase == CalibrateView::Measuring) {
      S.cal.samples = radar::calCount();
      S.cal.liveRssi = radar::calLastRssi();
      if (S.cal.samples >= kCalTarget || now - S.calStartMs > kCalTimeoutMs) {
        CalResult r = radar::calResult();
        radar::calStop();
        S.cal.result = r.ref1m;
        S.cal.spread = r.spreadDb;
        S.cal.samples = r.samples;
        S.cal.failReason = (uint8_t)r.reason;
        S.cal.phase = r.ok ? CalibrateView::Result : CalibrateView::Failed;
        hapticBuzz(r.ok ? 160 : 90, r.ok ? 90 : 60);
        S.dirty = true;
      }
    }
  }
  // Expiring confirmations.
  if (S.cardConfirm && now - S.cardConfirmMs > kConfirmMs) { S.cardConfirm = false; S.dirty = true; }
  if (S.newIdConfirm && now - S.newIdConfirmMs > kConfirmMs) { S.newIdConfirm = false; S.dirty = true; }
  if (S.detail.confirmRemove && now - S.detailConfirmMs > kConfirmMs) { S.detail.confirmRemove = false; S.dirty = true; }

  // Power: dim when untouched, leave after a long while; alert sessions
  // return to sleep on their own.
  if (S.alertSession) {
    if (!S.animOn && S.alertDeadline && (int32_t)(now - S.alertDeadline) >= 0) {
      radar::close();
      backlightOff();
      enterDeepSleep();                  // background alerts resume from here
    }
    return;
  }
  uint32_t idle = now - S.lastTouchMs;
  if (!S.dimmed && idle > kDimAfterMs && !S.animOn) {
    uint8_t br;
    { ModelLock lk; br = model.brightness; }
    backlightSet(br / 4 < 12 ? 12 : br / 4);
    S.dimmed = true;
  }
  if (idle > kLeaveAfterMs && !S.animOn) {
    switchTo(Screen::Watch);             // closes the radar; the watch then sleeps as usual
  }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
void back() {
  hapticBuzz(40, 30);
  switch (S.pg) {
    case Page::Radar:
      if (S.selectedId) { deselect(); return; }
      switchTo(Screen::AppList);
      return;
    case Page::Menu:       go(Page::Radar, false); return;
    case Page::Mates:      go(Page::Menu, false); return;
    case Page::MateDetail: S.detail.confirmRemove = false; go(S.detailReturn, false); return;
    case Page::Keyboard:   go(S.kbReturn, false); return;
    case Page::Calibrate:
      if (S.cal.phase == CalibrateView::Measuring) radar::calStop();
      go(Page::Menu, false);
      return;
    case Page::Help:       go(Page::Menu, false); return;
  }
}

void selectBlip(uint32_t id) {
  Blip b;
  if (!S.anim.find(id, b)) return;
  float bx, by;
  RadarScene::blipOverviewXY(b, bx, by);
  if (S.card.x < 0.05f) { S.focusX.snap(bx); S.focusY.snap(by); }   // from overview: lock straight on
  S.focusId = id;
  S.selectedId = id;
  S.cardConfirm = false;
  S.card.target = 1.f;
  S.dirty = true;
}

void tapRadar(int x, int y) {
  if (layout::menuHit.contains(x, y)) {
    hapticBuzz(50, 35);
    if (S.selectedId) deselect();
    S.card.snap(0.f);
    S.focusId = 0;
    S.menu = ListView();
    S.newIdConfirm = false;
    go(Page::Menu, true);
    return;
  }
  const RadarCamera cam = S.card.x > 0.0005f && S.focusId
                              ? RadarCamera::lockOn(S.focusX.x, S.focusY.x, S.card.x)
                              : RadarCamera::overview();
  if (S.selectedId && S.card.x > 0.5f) {
    const PeerView *p = findPeer(S.selectedId);
    if (p && y >= layout::cardTop) {
      if (p->mate) {
        if (layout::cardBtnL.grow(3).contains(x, y)) {
          hapticBuzz(60, 40);
          startKeyboard(KbTarget::MateNick, "Nickname", p->label, p->id);
          return;
        }
        if (layout::cardBtnR.grow(3).contains(x, y)) {
          if (!S.cardConfirm) {
            S.cardConfirm = true; S.cardConfirmMs = millis();
            hapticBuzz(70, 40);
          } else {
            char msg[40];
            snprintf(msg, sizeof msg, "Removed %s", p->label);
            radar::removeMate(p->id);
            S.cardConfirm = false;
            hapticBuzz(120, 60);
            showToast(msg);
          }
          S.dirty = true;
          return;
        }
      } else if (layout::cardBtnWide.grow(3).contains(x, y)) {
        char msg[40];
        if (radar::addMate(p->id)) {
          snprintf(msg, sizeof msg, "%s is now a mate", p->label);
          hapticBuzz(150, 70);
        } else {
          snprintf(msg, sizeof msg, "Mate list is full (%d)", kMaxMates);
          hapticBuzz(60, 40);
        }
        showToast(msg);
        S.dirty = true;
        return;
      }
      if (p->mate) {                          // card body: open the mate's page
        hapticBuzz(50, 35);
        S.detailId = p->id;
        S.detailReturn = Page::Radar;
        S.detail = MateDetailView();
        go(Page::MateDetail, true);
      }
      return;
    }
  }
  int hit = RadarScene::hitBlip(S.blips, S.nBlips, cam, x, y);
  if (hit >= 0 && S.blips[hit].id != S.selectedId) {
    hapticBuzz(60, 35);
    selectBlip(S.blips[hit].id);
  } else if (S.selectedId) {
    deselect();
  }
  S.dirty = true;
}

void tapMenu(int row) {
  if (row < 0 || row >= S.nMenu) return;
  RadarSettings s = radar::settings();
  hapticBuzz(50, 35);
  switch (S.menuIds[row]) {
    case MiMates:     S.mates = ListView(); go(Page::Mates, true); break;
    case MiName:      startKeyboard(KbTarget::MyName, "Your name", s.name, 0); break;
    case MiCalibrate: startCalibrationIntro(true); break;
    case MiAlerts:    radar::setAlerts(!s.alerts); break;
    case MiBackground:
      radar::setBackground(!s.bgAlerts, s.bgPeriodSec);
      showToast(!s.bgAlerts ? "Background alerts on" : "Background alerts off");
      break;
    case MiPeriod: {
      uint8_t next = s.bgPeriodSec == 30 ? 60 : (s.bgPeriodSec == 60 ? 120 : 30);
      radar::setBackground(s.bgAlerts, next);
      break;
    }
    case MiHelp:      S.helpScroll = 0; go(Page::Help, true); break;
    case MiNewId:
      if (!S.newIdConfirm) {
        S.newIdConfirm = true;
        S.newIdConfirmMs = millis();
      } else {
        S.newIdConfirm = false;
        radar::resetId();
        showToast("New radar ID");
        hapticBuzz(150, 70);
      }
      break;
  }
  S.dirty = true;
}

void tapDetail(int x, int y) {
  if (mateDetailRenameBtn().grow(3).contains(x, y)) {
    hapticBuzz(60, 40);
    startKeyboard(KbTarget::MateNick, "Nickname", S.detail.name, S.detailId);
    return;
  }
  if (mateDetailRemoveBtn().grow(3).contains(x, y)) {
    if (!S.detail.confirmRemove) {
      S.detail.confirmRemove = true;
      S.detailConfirmMs = millis();
      hapticBuzz(70, 40);
    } else {
      char msg[40];
      snprintf(msg, sizeof msg, "Removed %s", S.detail.name);
      radar::removeMate(S.detailId);
      S.detail.confirmRemove = false;
      hapticBuzz(120, 60);
      showToast(msg);
      if (S.detailReturn == Page::Radar) deselect();
      go(S.detailReturn == Page::MateDetail ? Page::Mates : S.detailReturn, false);
    }
    S.dirty = true;
  }
}

void keyPress(int code) {
  KeyboardView &k = S.kb;
  size_t len = strlen(k.text);
  if (code == kKeyDone) {
    hapticBuzz(120, 50);
    if (S.kbTarget == KbTarget::MyName) {
      radar::setName(k.text);
      showToast("Name saved");
    } else {
      if (k.text[0]) {
        radar::renameMate(S.kbMateId, k.text);
        showToast("Nickname saved");
      }
    }
    go(S.kbReturn, false);
    return;
  }
  hapticBuzz(35, 18);
  switch (code) {
    case kKeyShift:     if (!k.symbols) k.shift = !k.shift; break;
    case kKeyMode:      k.symbols = !k.symbols; break;
    case kKeySpace:
      if (len > 0 && len < kMaxName && k.text[len - 1] != ' ') { k.text[len] = ' '; k.text[len + 1] = 0; k.shift = true; }
      break;
    case kKeyBackspace:
      if (len) k.text[len - 1] = 0;
      if (strlen(k.text) == 0) k.shift = true;
      break;
    default:
      if (code > 0 && len < kMaxName) {
        k.text[len] = (char)code;
        k.text[len + 1] = 0;
        if (!k.symbols) k.shift = false;
      }
      break;
  }
  S.dirty = true;
}

void tapCalibrate(int x, int y) {
  const Rect &prim = calibratePrimaryBtn(S.cal);
  switch (S.cal.phase) {
    case CalibrateView::Intro:
      if (prim.grow(3).contains(x, y) && S.cal.peerAvailable) {
        char name[kMaxName + 1];
        if (radar::calStart(name)) {
          hapticBuzz(80, 40);
          snprintf(S.cal.peer, sizeof S.cal.peer, "%s", name);
          S.cal.phase = CalibrateView::Measuring;
          S.cal.samples = 0;
          S.calStartMs = millis();
        }
      }
      break;
    case CalibrateView::Result:
      if (prim.grow(3).contains(x, y)) {
        radar::setCalibration(S.cal.result, true);
        hapticBuzz(150, 70);
        showToast("Calibrated");
        go(Page::Menu, false);
        return;
      }
      if (calibrateSecondaryBtn().grow(3).contains(x, y)) { hapticBuzz(50, 30); startCalibrationIntro(false); }
      break;
    case CalibrateView::Failed:
      if (prim.grow(3).contains(x, y)) { hapticBuzz(50, 30); startCalibrationIntro(false); }
      break;
    default:
      break;
  }
  S.dirty = true;
}

bool scrollable() { return S.pg == Page::Menu || S.pg == Page::Mates || S.pg == Page::Help; }

void scrollBy(int dy) {
  switch (S.pg) {
    case Page::Menu:  S.menu.scroll -= dy; listClampScroll(S.menu, S.nMenu); break;
    case Page::Mates: S.mates.scroll -= dy; listClampScroll(S.mates, S.nMates); break;
    case Page::Help: {
      int maxS = helpContentHeight() - kListViewH;
      S.helpScroll -= dy;
      if (S.helpScroll > maxS) S.helpScroll = maxS;
      if (S.helpScroll < 0) S.helpScroll = 0;
      break;
    }
    default: break;
  }
  S.dirty = true;
}

void setPressedFor(int x, int y) {
  switch (S.pg) {
    case Page::Menu:  S.menu.pressed = listRowAt(y, S.menu, S.nMenu); break;
    case Page::Mates: S.mates.pressed = listRowAt(y, S.mates, S.nMates); break;
    case Page::Radar:
      S.cardPressed = 0;
      S.menuPressed = layout::menuHit.contains(x, y);
      if (S.selectedId && S.card.x > 0.5f && y >= layout::cardTop) {
        const PeerView *p = findPeer(S.selectedId);
        if (p && p->mate) {
          if (layout::cardBtnL.contains(x, y)) S.cardPressed = 1;
          else if (layout::cardBtnR.contains(x, y)) S.cardPressed = 2;
        } else if (layout::cardBtnWide.contains(x, y)) {
          S.cardPressed = 1;
        }
      }
      break;
    case Page::MateDetail:
      S.detail.pressed = mateDetailRenameBtn().contains(x, y) ? 1 : (mateDetailRemoveBtn().contains(x, y) ? 2 : 0);
      break;
    case Page::Calibrate:
      S.cal.pressed = calibratePrimaryBtn(S.cal).contains(x, y) ? 1
                    : (S.cal.phase == CalibrateView::Result && calibrateSecondaryBtn().contains(x, y) ? 2 : 0);
      break;
    default: break;
  }
  S.dirty = true;
}

void clearPressed() {
  S.menu.pressed = -1;
  S.mates.pressed = -1;
  S.cardPressed = 0;
  S.menuPressed = false;
  S.detail.pressed = 0;
  S.cal.pressed = 0;
  S.kb.pressedKey = kKeyNone;
  S.dirty = true;
}

uint16_t *psramAlloc(size_t pixels) {
  return (uint16_t *)heap_caps_malloc(pixels * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

}  // namespace

void radarViewBeginAlertSession() { gAlertSessionNext = true; }

// ---------------------------------------------------------------------------
// View contract
// ---------------------------------------------------------------------------
void FriendRadarView::onEnter() {
  Arduino_Canvas *fc = frameCanvas();
  if (fc) S.cv.attach(fc->getFramebuffer(), 240, 280);
  // The instrument's static layers live in PSRAM for the life of the boot
  // (static scope 131 KB; sweep, glass and bare-glass tables 67 KB each; page
  // backdrop 131 KB; transition snapshot 131 KB: ~600 KB of the 2 MB). Retry
  // whichever is missing on each open; the app degrades gracefully without them.
  if (!S.bgBuf) S.bgBuf = psramAlloc(dial::kBgPixels);
  if (!S.polarBuf) S.polarBuf = psramAlloc(dial::kPolarEntries);
  if (!S.overlayBuf) S.overlayBuf = psramAlloc(dial::kPolarEntries);
  if (!S.cleanBuf) S.cleanBuf = psramAlloc(dial::kPolarEntries);
  if (!S.backdropBuf) S.backdropBuf = psramAlloc(dial::kBgPixels);
  if (!S.underBuf) S.underBuf = psramAlloc(dial::kBgPixels);
  if (S.bgBuf && S.polarBuf && S.overlayBuf && S.cleanBuf && !S.scene.ready()) {
    S.scene.attach(S.bgBuf, S.polarBuf, S.overlayBuf, S.cleanBuf);
    S.scene.build();
  }
  if (S.backdropBuf && !S.backdropOk) {
    Canvas b(S.backdropBuf, layout::W, layout::H);
    buildPageBackground(b);
    S.backdropOk = true;
  }
  S.sceneOk = fc && S.scene.ready();
  S.anim.reset();
  S.pg = Page::Radar;
  S.selectedId = 0;
  S.focusId = 0;
  S.card.snap(0.f);
  S.page.snap(1.f);
  S.pageMoving = false;
  S.toastT.snap(0.f);
  RadarSettings s = radar::settings();
  S.knobAlerts.snap(s.alerts ? 1.f : 0.f);
  S.knobBg.snap(s.bgAlerts ? 1.f : 0.f);
  S.cardConfirm = false;
  S.menuPressed = false;
  S.animOn = false;
  S.toastUntil = millis();
  S.down = S.dragging = false;
  S.dimmed = false;
  S.lastTouchMs = millis();
  S.alertSession = gAlertSessionNext;
  S.alertDeadline = gAlertSessionNext ? millis() + kAlertLingerMs : 0;
  gAlertSessionNext = false;
  S.dirty = true;
  S.lastFrameMs = 0;
  S.lastStepMs = 0;
  S.bootStartMs = millis() | 1;
  radar::open();
  refreshPeers(millis());
  // First run: nudge the wearer to pick a name others will see.
  char def[kMaxName + 1];
  defaultName(s.id, def);
  if (!S.alertSession && strcmp(s.name, def) == 0) showToast("Set your name in the menu", 3500);
}

void FriendRadarView::onExit() {
  radar::calStop();
  radar::close();
  restoreBrightness();
  S.animOn = false;
  S.alertSession = false;
}

int32_t FriendRadarView::nextFrameInMs() {
  uint32_t now = millis();
  if (S.dirty) return 0;
  if (!animating(now)) return 100;      // still poll for alerts ~10x a second
  int32_t left = (int32_t)frameInterval() - (int32_t)(now - S.lastFrameMs);
  return left < 0 ? 0 : left;
}

void FriendRadarView::render() {
  if (!S.cv.px) {
    Arduino_Canvas *fc = frameCanvas();
    if (!fc) return;
    S.cv.attach(fc->getFramebuffer(), 240, 280);
  }
  uint32_t now = millis();
  poll(now);
  if (currentView != this) return;          // poll() may have left the app
  // Input and state changes draw immediately; animated pages also draw on
  // their frame clock.
  bool due = S.dirty || (animating(now) && now - S.lastFrameMs >= frameInterval());
  if (!due) return;
#if FR_PERF_LOG
  uint32_t t0 = micros();
#endif
  stepMotion(now);
  drawPage(now);
#if FR_PERF_LOG
  uint32_t t1 = micros();
#endif
  Arduino_Canvas *fc = frameCanvas();
  if (fc) fc->flush();
#if FR_PERF_LOG
  uint32_t t2 = micros();
  S.perfDrawUs += t1 - t0;
  S.perfFlushUs += t2 - t1;
  if (++S.perfFrames >= 1 && now - S.perfSinceMs >= 5000) {
    Serial.printf("fr perf: page %d  draw %.1f ms  flush %.1f ms  %.1f fps\n", (int)S.pg,
                  S.perfDrawUs / 1000.f / S.perfFrames, S.perfFlushUs / 1000.f / S.perfFrames,
                  S.perfFrames * 1000.f / (float)(now - S.perfSinceMs));
    S.perfDrawUs = S.perfFlushUs = S.perfFrames = 0;
    S.perfSinceMs = now;
  }
#endif
  S.lastFrameMs = now;
  S.dirty = false;
}

void FriendRadarView::onEvent(const Event &e) {
  switch (e.type) {
    case EventType::ButtonShort:
      noteInteraction();
      back();
      return;
    case EventType::ImuMotion:
      radar::localShake();
      return;
    case EventType::Gesture:
      noteInteraction();
      return;
    case EventType::Touch: {
      noteInteraction();
      if (S.animOn && S.pg != Page::Radar) {   // the page is hidden under the scope: end the moment here
        S.animOn = false;
        slideScope(false);
        S.down = false;
        S.dirty = true;
        return;
      }
      if (layout::backHit.contains(e.x, e.y)) { back(); return; }
      S.down = true;
      S.dragging = false;
      S.downX = e.x; S.downY = e.y; S.lastY = e.y;
      S.downMs = millis();
      if (S.pg == Page::Keyboard) {
        int code = keyboardHit(S.kb, e.x, e.y);
        S.kb.pressedKey = code;
        if (code != kKeyNone) keyPress(code);
        return;
      }
      setPressedFor(e.x, e.y);
      return;
    }
    case EventType::TouchHold: {
      if (!S.down) return;
      int dy = (int)e.y - S.lastY;
      if (!S.dragging && scrollable() && abs((int)e.y - S.downY) > 8) {
        S.dragging = true;
        S.menu.pressed = S.mates.pressed = -1;
      }
      if (S.dragging) scrollBy(dy);
      S.lastY = e.y;
      return;
    }
    case EventType::TouchUp: {
      if (!S.down) return;
      S.down = false;
      bool wasDrag = S.dragging;
      S.dragging = false;
      clearPressed();
      if (wasDrag || S.pg == Page::Keyboard) return;
      int x = S.downX, y = S.downY;
      if (abs((int)e.x - x) > 14 || abs((int)e.y - y) > 14) return;   // a swipe, not a tap
      switch (S.pg) {
        case Page::Radar: tapRadar(x, y); break;
        case Page::Menu:  tapMenu(listRowAt(y, S.menu, S.nMenu)); break;
        case Page::Mates: {
          int row = listRowAt(y, S.mates, S.nMates);
          if (row >= 0) {
            hapticBuzz(50, 35);
            S.detailId = S.mateIds[row];
            S.detailReturn = Page::Mates;
            S.detail = MateDetailView();
            go(Page::MateDetail, true);
          }
          break;
        }
        case Page::MateDetail: tapDetail(x, y); break;
        case Page::Calibrate:  tapCalibrate(x, y); break;
        default: break;
      }
      return;
    }
    default:
      return;
  }
}
