// Rep Counter: the app screen. See rep_view.h.
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <string.h>
#include "rep_view.h"
#include "display.h"
#include "haptic.h"
#include "model.h"
#include "imu_stream.h"
#include "rep_console.h"
#include "rep_detector.h"
#include "rep_gfx.h"
#include "rep_haptics.h"
#include "rep_resample.h"
#include "rep_session.h"
#include "rep_store.h"
#include "rep_ui.h"

#ifndef REPS_IMU_FIFO
#define REPS_IMU_FIFO 1
#endif

namespace {

using reps::Phase;

const uint32_t kDimAfterMs = 20000;    // rest/ready with no touch: dim the panel
const uint32_t kBackGuardMs = 2500;    // second back press within this exits
const uint32_t kClearHoldMs = 1500;    // hold "Clear today" this long
const float kStreamFs = REPS_IMU_FIFO ? 100.f : 50.f;

reps::Detector gDet;
reps::Session gSes;
reps::HapticSeq gHap;
repui::UiState gUi;
bool gActive = false;

// Sample stream reader (+ resampler for polled samples: the REPS_IMU_FIFO=0
// fallback, or a FIFO build whose mode switch hasn't succeeded yet).
uint32_t gCursor = 0, gLost = 0;
reps::Resampler gResample(kStreamFs, 250);
uint32_t gEnterMs = 0, gSensorWarnMs = 0;

// Frame state.
uint32_t gRowHash[repui::H];
uint32_t gLastSig = 0, gLastFrameMs = 0, gLastStatusMs = 0, gLastDayCheckMs = 0;
bool gCanvasFailed = false;

// Input / UI state.
bool gPress = false, gMoved = false;
uint16_t gPressX = 0, gPressY = 0;
uint32_t gPressMs = 0;
repui::Hit gPressHit = repui::Hit::None;
uint32_t gBackArmedUntil = 0;
uint32_t gToastUntil = 0;
const char *gToast = nullptr;
uint32_t gLastTouchMs = 0;
bool gDimmed = false;
uint32_t gClearStart = 0;
bool gClearing = false;
bool gCleared = false;
uint8_t gShownReps = 0;
uint32_t gPopMs = 0;

inline bool before(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }

bool imuOk() {
  ModelLock lk;
  return model.imuOk;
}

reps::Date nowDate(uint8_t *brightness = nullptr) {
  reps::Date d;
  ModelLock lk;
  d.year = model.year;
  d.month = model.month;
  d.day = model.day;
  d.hour = model.hour;
  d.minute = model.minute;
  d.second = model.second;
  d.valid = model.rtcOk && model.year >= 2024;
  if (brightness) *brightness = model.brightness;
  return d;
}

void buzz(uint8_t intensity, uint16_t ms) { hapticBuzz(intensity, ms); }

void toast(const char *t, uint32_t ms) {
  gToast = t;
  gToastUntil = millis() + ms;
}

void resetDetector() {
  const reps::Settings &st = gSes.settingsRO();
  reps::Config c;
  c.fs = kStreamFs;
  c.countsPerG = 2048.f;            // unused: samples are fed in g
  c.mode = (reps::Mode)st.mode;
  c.wrist = (reps::Wrist)st.wrist;
  c.setEndSec = st.setEndSec;
  c.learnedHandSign = st.handSign;
  gDet.reset(c);
  gResample.reset();
}

void undim() {
  if (!gDimmed) return;
  uint8_t br;
  nowDate(&br);
  backlightSet(br);
  gDimmed = false;
}

// ---- detector events -> session, console, haptics ---------------------------------
const char *evName(reps::EventType t) {
  switch (t) {
    case reps::EventType::RepTentative: return "rep?";
    case reps::EventType::SetStart: return "set_start";
    case reps::EventType::Rep: return "rep";
    case reps::EventType::SetEnd: return "set_end";
    case reps::EventType::SetDiscarded: return "discard";
    default: return "?";
  }
}

void pumpEvents(uint32_t now) {
  reps::Event e;
  reps::Date d = nowDate();
  while (gDet.pollEvent(e)) {
    // Forgot to tap start? The first confirmed set starts the workout.
    if (e.type == reps::EventType::SetStart && gSes.phase() == Phase::Idle) gSes.start(now);
    gSes.onEvent(e, now, d);
    char ln[72];
    snprintf(ln, sizeof ln, "t_ms=%lu type=%s reps=%u ex=%s conf=%u", (unsigned long)now,
             evName(e.type), e.reps, reps::exerciseName(e.exercise), e.confidence);
    repsConsoleEvent(ln);
    if (e.type == reps::EventType::RepTentative || e.type == reps::EventType::SetEnd) undim();
  }
  // The detector learns which way the hand points from curl/raise sets.
  int8_t hs = gDet.learnedHandSign();
  if (hs != gSes.settingsRO().handSign) gSes.settings().handSign = hs;
}

// End any set in flight (logged if it had 2+ reps) before pausing/leaving.
void closeSet(uint32_t now) {
  gDet.endSetNow();
  pumpEvents(now);
}

void playCues(uint32_t now) {
  uint16_t c = gSes.takeCues();
  if (!c) return;
  if (c & reps::CueSetDone) gHap.play(REPS_PATTERN(reps::kPatSetDone, 3), now);
  else if (c & reps::CueRestGoal) gHap.play(REPS_PATTERN(reps::kPatRestGoal, 2), now);
  else if (c & reps::CueRestLong) gHap.play(REPS_PATTERN(reps::kPatRestLong, 2), now);
  else if (c & reps::CueStart) gHap.play(REPS_PATTERN(reps::kPatStart, 2), now);
  else if (c & reps::CuePause) gHap.play(REPS_PATTERN(reps::kPatPause, 2), now);
  else if (c & reps::CueSetStart) gHap.play(REPS_PATTERN(reps::kPatSetStart, 1), now);
  else if (c & reps::CueRep) gHap.play(REPS_PATTERN(reps::kPatRep, 1), now);
  if (c & (reps::CueRestGoal | reps::CueRestLong | reps::CueSetDone)) undim();
}

void persist() {
  if (gSes.takeLogDirty()) repstore::saveLog(gSes.log());
  if (gSes.takeHistDirty()) repstore::saveHist(gSes.history());
  if (gSes.takeSettingsDirty()) repstore::saveSettings(gSes.settingsRO());
}

// ---- samples -----------------------------------------------------------------------
void pumpSamples() {
  ImuSample s;
  uint32_t lostBefore = gLost;
  int budget = 400;                       // ~4 s of data per call at most
  while (budget-- > 0 && imuStreamRead(gCursor, s, gLost)) {
    float k = 1.f / imuCountsPerG(s.flags);
    float g[3] = {s.x * k, s.y * k, s.z * k};
    bool gap = (s.flags & IMU_FLAG_GAP) || gLost != lostBefore;
    lostBefore = gLost;
    if (s.flags & IMU_FLAG_FIFO) {
      if (gap) gDet.markGap();
      gDet.pushG(g[0], g[1], g[2]);
      continue;
    }
    // Polled samples (~45 Hz, jittery): resample to the detector's rate.
    gResample.push(s.tMs, g, gap, [](float x, float y, float z, bool restart) {
      if (restart) gDet.markGap();
      gDet.pushG(x, y, z);
    });
  }
}

// ---- drawing -------------------------------------------------------------------------
uint32_t mix(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }

uint32_t signature(const repui::UiState &u) {
  uint32_t h = 2166136261u;
  h = mix(h, (uint32_t)u.page);
  h = mix(h, (uint32_t)u.phase | (u.autoPaused << 8) | (u.tentative << 9) | (u.moving << 10) |
                 (u.summary << 11) | (u.restGoalReached << 12) | (u.recording << 13) |
                 (u.handLearned << 14) | (u.clockValid << 15));
  h = mix(h, u.reps | ((uint32_t)u.exercise << 8) | ((uint32_t)u.mode << 16) | ((uint32_t)u.confidence << 24));
  h = mix(h, u.setNo | ((uint32_t)u.todaySets << 16));
  h = mix(h, u.todayReps | ((uint32_t)u.restGoalSec << 16));
  h = mix(h, u.restSec);
  h = mix(h, (uint32_t)(u.tempoSec * 10.f));
  h = mix(h, (uint32_t)(u.progress * 200.f));
  h = mix(h, (uint32_t)(u.pop * 16.f));
  h = mix(h, (uint32_t)(u.summaryAge < 0.08f ? u.summaryAge * 400.f : 99.f));
  h = mix(h, u.hour | (u.minute << 8) | ((uint32_t)u.lastExercise << 16));
  h = mix(h, (uint32_t)(uintptr_t)u.toast);
  h = mix(h, (uint32_t)u.histScroll | ((uint32_t)(u.pressedRow + 1) << 8));
  h = mix(h, (uint32_t)(u.clearHold * 40.f));
  h = mix(h, u.lastSet.reps | ((uint32_t)u.lastSetNo << 8));
  h = mix(h, u.settings.mode | (u.settings.wrist << 4) | (u.settings.setEndSec << 8) |
                 (u.settings.repTick << 16) | ((uint32_t)u.settings.restGoalSec << 20));
  h = mix(h, u.theme.bg | ((uint32_t)u.theme.fg << 16));
  h = mix(h, u.theme.accent | ((uint32_t)u.theme.line << 16));
  h = mix(h, u.log ? u.log->count : 0);
  return h;
}

void flushChanged(uint16_t *fb) {
  // Push only bands of rows whose content changed since the last flush.
  int bandStart = -1;
  for (int y = 0; y <= repui::H; y++) {
    bool dirty = false;
    if (y < repui::H) {
      const uint16_t *row = fb + y * repui::W;
      uint32_t hsh = 2166136261u;
      for (int x = 0; x < repui::W; x += 2)
        hsh = (hsh ^ ((uint32_t)row[x] | ((uint32_t)row[x + 1] << 16))) * 16777619u;
      dirty = hsh != gRowHash[y];
      gRowHash[y] = hsh;
    }
    if (dirty && bandStart < 0) bandStart = y;
    if (!dirty && bandStart >= 0) {
      gfx->draw16bitRGBBitmap(0, bandStart, fb + bandStart * repui::W, repui::W, y - bandStart);
      bandStart = -1;
    }
  }
}

void buildUi(uint32_t now) {
  repui::UiState &u = gUi;
  ThemeColors t = theme();
  u.theme.bg = t.bg;
  u.theme.fg = t.fg;
  u.theme.accent = t.accent;
  u.theme.line = t.line;
  reps::Date d = nowDate();
  u.clockValid = d.valid;
  u.hour = d.hour;
  u.minute = d.minute;
  const reps::Live &lv = gDet.live();
  u.phase = gSes.phase();
  u.autoPaused = gSes.autoPaused();
  u.reps = gSes.reps();
  u.tentative = gSes.tentative();
  u.exercise = gSes.exercise();
  u.lastExercise = gSes.lastExercise();
  u.mode = (reps::Mode)gSes.settingsRO().mode;
  u.confidence = gSes.confidence();
  u.setNo = gSes.currentSetNo();
  u.tempoSec = gSes.tempoSec();
  u.restSec = gSes.restSec(now);
  u.restGoalSec = gSes.settingsRO().restGoalSec;
  u.restGoalReached = gSes.restGoalReached();
  bool counting = u.phase == Phase::Ready || u.phase == Phase::Lifting;
  u.progress = counting ? lv.progress : 0.f;
  u.moving = counting && lv.moving;
  // Colour pop on each counted rep.
  if (u.phase == Phase::Lifting && !u.tentative && u.reps > gShownReps) gPopMs = now;
  gShownReps = u.reps;
  float pa = (float)(now - gPopMs) / 350.f;
  u.pop = (gPopMs && pa < 1.f) ? 1.f - pa : 0.f;
  u.todaySets = gSes.setsToday();
  u.todayReps = gSes.repsToday();
  u.recording = repsRecActive();
  u.summary = gSes.summaryVisible(now);
  u.summaryAge = gSes.summaryAge(now);
  u.lastSet = gSes.lastSet();
  u.lastSetNo = gSes.lastSetNo();
  u.toast = (gToast && before(now, gToastUntil)) ? gToast : nullptr;
  u.log = &gSes.log();
  u.hist = &gSes.history();
  u.settings = gSes.settingsRO();
  u.handLearned = gSes.settingsRO().handSign != 0;
  u.clearHold = gClearing ? (float)(now - gClearStart) / kClearHoldMs : 0.f;
  if (u.clearHold > 1.f) u.clearHold = 1.f;
}

void drawFrame(uint32_t now) {
  if (!gfx) return;
  Arduino_Canvas *cv = frameCanvas();
  if (!cv) {
    if (!gCanvasFailed) {
      gCanvasFailed = true;
      ThemeColors t = theme();
      gfx->fillScreen(t.bg);
      gfx->setTextColor(t.fg, t.bg);
      gfx->setTextSize(2);
      gfx->setCursor(12, 120);
      gfx->print("Out of memory");
      Serial.println("Rep Counter: frame canvas allocation failed");
    }
    return;
  }
  uint32_t sig = signature(gUi);
  if (sig == gLastSig && now - gLastFrameMs < 2000) return;
  if (now - gLastFrameMs < 33) return;               // ~30 fps is plenty
  rgfx::Surface s(cv->getFramebuffer(), repui::W, repui::H);
  repui::render(s, gUi);
  flushChanged(cv->getFramebuffer());
  gLastSig = sig;
  gLastFrameMs = now;
}

// ---- input -----------------------------------------------------------------------------
void startStopTap(uint32_t now) {
  switch (gSes.phase()) {
    case Phase::Idle:
      resetDetector();
      gSes.start(now);
      break;
    case Phase::Paused:
      closeSet(now);                     // drop anything seen while paused
      gSes.resume(now);
      break;
    default:
      closeSet(now);
      gSes.pause(now);
      break;
  }
}

void cycleMode() {
  reps::Settings &st = gSes.settings();
  st.mode = (uint8_t)((st.mode + 1) % reps::kModeCount);
  gDet.setMode((reps::Mode)st.mode);
  pumpEvents(millis());                 // a set in flight ends with the mode change
}

template <typename T, size_t N>
T nextChoice(const T (&arr)[N], T cur) {
  for (size_t i = 0; i < N; i++)
    if (arr[i] == cur) return arr[(i + 1) % N];
  return arr[0];
}

void settingsTap(int row, uint32_t now) {
  reps::Settings &st = gSes.settings();
  switch (row) {
    case repui::RowExercise: cycleMode(); break;
    case repui::RowWrist:
      st.wrist = st.wrist ? 0 : 1;
      st.handSign = 0;                   // re-learn for the new wrist
      gDet.setWrist((reps::Wrist)st.wrist);
      gDet.setLearnedHandSign(0);
      break;
    case repui::RowSetEnd:
      st.setEndSec = nextChoice(reps::kSetEndChoices, st.setEndSec);
      gDet.setSetEndSec(st.setEndSec);
      break;
    case repui::RowTick:
      st.repTick = st.repTick ? 0 : 1;
      if (st.repTick) gHap.play(REPS_PATTERN(reps::kPatRep, 1), now);
      break;
    case repui::RowRest:
      st.restGoalSec = nextChoice(reps::kRestGoalChoices, st.restGoalSec);
      break;
    default: break;
  }
}

void goBack(uint32_t now) {
  if (gUi.page != repui::Page::Main) {
    gUi.page = repui::Page::Main;
    return;
  }
  // An accidental press of the side button mid-set (wrist flexed against it)
  // must not throw the workout away: while one is running, ask twice.
  if (gSes.blocksSleep() && !before(now, gBackArmedUntil)) {
    gBackArmedUntil = now + kBackGuardMs;
    toast("Back again to finish", kBackGuardMs);
    buzz(60, 40);
    return;
  }
  switchTo(Screen::AppList);
}

void nextPage() {
  gUi.page = (repui::Page)(((int)gUi.page + 1) % repui::kPages);
  gUi.histScroll = 0;
  buzz(40, 35);
}

}  // namespace

// ---- View --------------------------------------------------------------------------------
bool repsBlockSleep() {
  return (gActive && gSes.blocksSleep()) || repsRecActive();
}

void RepView::onEnter() {
  reps::Settings st;
  static reps::DayLog log;          // static: keeps ~0.5 KB off the render stack
  static reps::History hist;
  repstore::load(st, log, hist);
  gSes.init(st, log, hist);
  gSes.setToday(nowDate());
  resetDetector();
  gCursor = imuStreamHead();
  gLost = 0;
  imuStreamWant(IMU_CLIENT_APP, true);
  gUi = repui::UiState();
  gUi.page = repui::Page::Main;
  for (int y = 0; y < repui::H; y++) gRowHash[y] = 0xFFFFFFFFu;
  gLastSig = 0;
  gLastFrameMs = 0;
  gCanvasFailed = false;
  gPress = gMoved = false;
  gBackArmedUntil = 0;
  gToast = nullptr;
  gLastTouchMs = millis();
  gLastDayCheckMs = millis();
  gDimmed = false;
  gClearing = gCleared = false;
  gShownReps = 0;
  gPopMs = 0;
  gEnterMs = millis();
  gSensorWarnMs = 0;
  gActive = true;
  persist();
}

void RepView::onExit() {
  uint32_t now = millis();
  closeSet(now);
  persist();
  imuStreamWant(IMU_CLIENT_APP, false);
  gHap.stop();
  undim();
  gActive = false;
  repsConsoleSetStatus("app not open");
}

void RepView::render() {
  uint32_t now = millis();
  pumpSamples();
  pumpEvents(now);
  gSes.tick(now);
  if (now - gLastDayCheckMs > 60000) {
    gLastDayCheckMs = now;
    gSes.setToday(nowDate());
  }
  playCues(now);
  uint32_t until = gHap.service(now, buzz);
  if (until) gDet.suppressGate(until - now + 40);   // our own buzz shakes the sensor
  persist();

  // Long rest with nobody touching the watch: dim instead of sleeping.
  Phase ph = gSes.phase();
  if (!gDimmed && (ph == Phase::Resting || ph == Phase::Ready) &&
      now - gLastTouchMs > kDimAfterMs && !gSes.summaryVisible(now)) {
    uint8_t br;
    nowDate(&br);
    uint8_t dim = br / 4 < 16 ? 16 : br / 4;
    if (dim < br) { backlightSet(dim); gDimmed = true; }
  }
  if (gDimmed && ph == Phase::Lifting) undim();

  // Hold-to-clear on the settings page.
  if (gClearing && !gCleared && now - gClearStart >= kClearHoldMs) {
    gCleared = true;
    gSes.forgetToday();
    persist();
    gHap.play(REPS_PATTERN(reps::kPatPause, 2), now);
    toast("Today's log cleared", 2000);
  }

  // No accelerometer (or it stopped answering): say so rather than sit there.
  if (now - gEnterMs > 2000 && now - gSensorWarnMs > 1400 && !imuOk()) {
    gSensorWarnMs = now;
    toast("Motion sensor not responding", 1500);
  }

  buildUi(now);
  drawFrame(now);

  if (now - gLastStatusMs > 1000) {
    gLastStatusMs = now;
    static const char *const kPh[] = {"idle", "ready", "lifting", "resting", "paused"};
    char st[96];
    snprintf(st, sizeof st, "%s reps=%u ex=%s set=%u mode=%s today=%u/%u", kPh[(int)ph], gSes.reps(),
             reps::exerciseName(gSes.exercise()), gSes.currentSetNo(),
             reps::modeName((reps::Mode)gSes.settingsRO().mode), gSes.setsToday(), gSes.repsToday());
    repsConsoleSetStatus(st);
  }
}

void RepView::onEvent(const Event &e) {
  uint32_t now = millis();
  switch (e.type) {
    case EventType::ButtonShort:
      gLastTouchMs = now;
      undim();
      gSes.touch(now);
      goBack(now);
      return;
    case EventType::Touch: {
      gLastTouchMs = now;
      gSes.touch(now);
      if (gDimmed) { undim(); gPress = false; return; }   // first touch only wakes
      gPress = true;
      gMoved = false;
      gPressX = e.x;
      gPressY = e.y;
      gPressMs = now;
      gPressHit = repui::hitTest(gUi, e.x, e.y);
      if (gUi.page == repui::Page::Settings && gPressHit >= repui::Hit::Row0) {
        gUi.pressedRow = (int)gPressHit - (int)repui::Hit::Row0;
        if (gUi.pressedRow == repui::RowClear) { gClearing = true; gCleared = false; gClearStart = now; }
      }
      return;
    }
    case EventType::TouchHold:
      if (gPress) {
        int dx = (int)e.x - gPressX, dy = (int)e.y - gPressY;
        if (dx * dx + dy * dy > 18 * 18) {
          gMoved = true;
          gUi.pressedRow = -1;
          gClearing = false;
        }
      }
      return;
    case EventType::TouchUp: {
      bool tap = gPress && !gMoved && now - gPressMs < 700;
      gPress = false;
      gUi.pressedRow = -1;
      bool wasClearing = gClearing;
      gClearing = false;
      if (!tap) return;
      if (wasClearing) {
        if (!gCleared) toast("Hold to clear", 1500);
        return;
      }
      if (gPressHit == repui::Hit::Back) { buzz(50, 40); goBack(now); return; }
      if (gUi.page == repui::Page::Main) {
        if (gPressHit == repui::Hit::ModeChip) { buzz(60, 40); cycleMode(); }
        else if (gPressHit == repui::Hit::Center && gSes.summaryVisible(now)) gSes.dismissSummary();
        else if (gPressHit == repui::Hit::Center) startStopTap(now);
        else if (gPressHit == repui::Hit::PageDots) nextPage();
      } else if (gUi.page == repui::Page::Settings) {
        if (gPressHit >= repui::Hit::Row0 && gPressHit <= repui::Hit::Row4) {
          buzz(60, 40);
          settingsTap((int)gPressHit - (int)repui::Hit::Row0, now);
        } else if (gPressHit == repui::Hit::PageDots) {
          nextPage();
        }
      } else if (gPressHit == repui::Hit::PageDots) {
        nextPage();
      }
      return;
    }
    case EventType::Gesture:
      gLastTouchMs = now;
      gSes.touch(now);
      if (e.gesture == Gesture::SwipeLeft) { gPress = false; nextPage(); }
      if (gUi.page == repui::Page::History) {
        int n = gSes.log().count, vis = repui::historyVisibleRows();
        if (e.gesture == Gesture::SwipeUp && gUi.histScroll + vis < n) gUi.histScroll++;
        if (e.gesture == Gesture::SwipeDown && gUi.histScroll > 0) gUi.histScroll--;
      }
      return;
    default:
      return;
  }
}
