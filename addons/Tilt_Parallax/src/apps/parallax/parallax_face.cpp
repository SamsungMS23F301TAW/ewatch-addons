#include "parallax_face.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_task_wdt.h>
#include <math.h>
#include <string.h>
#include "display.h"
#include "haptic.h"
#include "model.h"
#include "tilt_hook.h"
#include "parallax_gen.h"
#include "parallax_store.h"
#include "parallax_cmd.h"
#include "parallax_settings.h"
#include "tp_platform.h"
#include "tp_config.h"
#include "tp_transition.h"

#ifndef TP_PERF_LOG
#define TP_PERF_LOG 1
#endif
// 0 (default): compose dirty rows in a 32-row internal-SRAM strip and push
// each strip. 1: compose into the shared frameCanvas() (PSRAM) and push its
// changed rows. Same pixels; the strip avoids ~400 KB of PSRAM traffic per
// full frame. Compare both on hardware with the serial `bench` command.
#ifndef TP_COMPOSE_TO_CANVAS
#define TP_COMPOSE_TO_CANVAS 0
#endif

using namespace tp;

static const int kStripRows = 32;


static const char *const kWd[] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
static const char *const kMo[] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                   "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };

static int weekdayOf(int y, int m, int d) {          // 0 = Sunday (Sakamoto)
  static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
  if (m < 1 || m > 12) return 0;
  if (m < 3) y -= 1;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}


bool parallaxUseClassicFace() { return parallaxSettings().classicFace; }

// ---------------------------------------------------------------------------
bool ParallaxFaceView::ensureRenderer() {
  if (frReady_) return true;
  if (frFailed_) return false;
  if (!fr_.begin()) {
    Serial.println("tp: renderer allocation failed; using fallback clock");
    fr_.end();
    frFailed_ = true;
    return false;
  }
#if !TP_COMPOSE_TO_CANVAS
  strip_ = (uint16_t *)allocFast((size_t)kScreenW * kStripRows * sizeof(uint16_t));
  if (!strip_) Serial.println("tp: no internal RAM for the strip; composing via frameCanvas");
#endif
  frReady_ = true;
  return true;
}

void ParallaxFaceView::onEnter() {
  enterMs_ = millis();
  tilt_.reset();              // whatever pose the wrist is in now reads as centred
  tiltHookClear();
  tiltHookEnable(true);
  pressActive_ = false;
  moving_ = false;
  perfStart_ = 0;
  if (frReady_) fr_.markAllDirty();
  // Scene picked in settings while we were away?
  const ParallaxSettings &st = parallaxSettings();
  if (scene_ && scene_->id != st.scene && phase_ == PH_IDLE) startSceneChange(st.scene, enterMs_);
  if (!scene_ && targetScene_ < 0) targetScene_ = st.scene;
}

void ParallaxFaceView::onExit() {
  tiltHookEnable(false);
  pressActive_ = false;
  parallaxSettingsSave();
}

uint16_t ParallaxFaceView::desiredFrameMs() const {
  if (firstFramePending_) return 15;
  if (phase_ != PH_IDLE || moving_) return 15;
  if (parallaxSettings().ambient && (fr_.hasClouds() || fr_.starsVisible())) return 120;
  return 0;
}

void ParallaxFaceView::openSettings() {
  pressActive_ = false;
  hapticBuzz(70, 35);
  parallaxSettingsOpen(Screen::Watch);
}

// ---------------------------------------------------------------------------
// Scene changes
// ---------------------------------------------------------------------------
void ParallaxFaceView::startSceneChange(int targetId, uint32_t now) {
  int n = sceneCount();
  targetId = ((targetId % n) + n) % n;
  targetScene_ = targetId;
  // Remembered in RAM now; written to NVS when the face is left or the watch
  // sleeps (a flash write mid-animation would stall the frame).
  parallaxSettings().scene = (uint8_t)targetId;
  parallaxGenRequest(targetId);
  lastGenReqMs_ = now;
  if (phase_ == PH_IDLE || phase_ == PH_IN) {
    // Drop away from wherever the layers are right now (mid-rise included).
    for (int i = 0; scene_ && i < scene_->count; i++) outFrom_[i] = fr_.layerAnimDy(i);
    phase_ = scene_ ? PH_OUT : PH_WAIT;
    phaseStart_ = now;
  }
  if (parked_) { parallaxGenRecycle(parked_); parked_ = nullptr; }
}

void ParallaxFaceView::adoptScene(Scene *s, bool animateIn, uint32_t now) {
  Scene *old = scene_;
  scene_ = s;
  fr_.setScene(s);
  if (old) parallaxGenRecycle(old);
  if (animateIn) {
    for (int i = 0; i < s->count; i++)
      if (transitionAnimates(s->layers[i].depth))
        fr_.setLayerAnim(i, 0, kScreenH + 8 - s->layers[i].y0);
    phase_ = PH_IN;
    phaseStart_ = now;
  } else {
    phase_ = PH_IDLE;
  }
}

void ParallaxFaceView::updateTransition(uint32_t now) {
  // Collect a finished build.
  Scene *fresh = parallaxGenTake();
  if (fresh) {
    if (phase_ == PH_OUT) {
      if (parked_) parallaxGenRecycle(parked_);
      parked_ = fresh;
    } else {
      // A scene that's ready before the first frame appears instantly (the
      // usual wake). One that arrives later (a scene change, or a slow
      // boot where sky + clock were shown first) rises into view.
      adoptScene(fresh, !firstFramePending_, now);
    }
  }
  if (!scene_) return;
  uint32_t t = now - phaseStart_;
  int n = scene_->count;
  float depths[Scene::kMaxLayers];
  for (int i = 0; i < n; i++) depths[i] = scene_->layers[i].depth;
  int ranks = n > 0 ? transitionRank(depths, n - 1) + 1 : 0;
  switch (phase_) {
    case PH_OUT: {
      bool done = true;
      for (int i = 0; i < n; i++) {
        if (!transitionAnimates(depths[i])) continue;
        int dist = kScreenH + 8 - scene_->layers[i].y0;
        fr_.setLayerAnim(i, 0, transitionOutDy(transitionRank(depths, i), ranks, t, outFrom_[i],
                                               dist, &done));
      }
      if (done) {
        phase_ = PH_WAIT;
        phaseStart_ = now;
        if (parked_) {
          Scene *p = parked_;
          parked_ = nullptr;
          adoptScene(p, true, now);
        }
      }
      break;
    }
    case PH_IN: {
      bool done = true;
      for (int i = 0; i < n; i++) {
        if (!transitionAnimates(depths[i])) continue;
        int dist = kScreenH + 8 - scene_->layers[i].y0;
        fr_.setLayerAnim(i, 0, transitionInDy(transitionRank(depths, i), t, dist, &done));
      }
      if (done) {
        for (int i = 0; i < n; i++) fr_.setLayerAnim(i, 0, 0);
        phase_ = PH_IDLE;
      }
      break;
    }
    case PH_WAIT:
      // The new scene never came (a build failed for lack of PSRAM): after a
      // few seconds bring the previous landscape back rather than leaving an
      // empty sky.
      if (!parallaxGenBusy() && t > 3000) {
        Serial.println("tp: scene build didn't arrive; restoring the previous scene");
        targetScene_ = scene_->id;
        parallaxSettings().scene = (uint8_t)scene_->id;
        for (int i = 0; i < n; i++)
          if (transitionAnimates(depths[i]))
            fr_.setLayerAnim(i, 0, kScreenH + 8 - scene_->layers[i].y0);
        phase_ = PH_IN;
        phaseStart_ = now;
      }
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Frame output
// ---------------------------------------------------------------------------
void ParallaxFaceView::present(uint32_t now) {
  Arduino_Canvas *cv = nullptr;
  uint16_t *fb = nullptr;
  if (!strip_) {
    cv = frameCanvas();
    if (!cv) return;
    fb = cv->getFramebuffer();
  }
  uint32_t tc = 0, tpush = 0;
  int rows = 0, y0, y1;
  while (fr_.popDirtyRun(y0, y1, kStripRows)) {
    uint32_t a = micros();
    uint16_t *dst = strip_ ? strip_ : fb + (size_t)y0 * kScreenW;
    fr_.composeRows(y0, y1, dst, kScreenW);
    uint32_t b = micros();
    gfx->draw16bitRGBBitmap(0, y0, dst, kScreenW, y1 - y0);
    uint32_t c = micros();
    tc += b - a;
    tpush += c - b;
    rows += y1 - y0;
  }
  if (rows == 0) return;
  if (firstFramePending_) {
    firstFramePending_ = false;
    uiMarkFirstFrame();
    Serial.printf("tp: first frame %lu ms after enter (compose %lu us, push %lu us)\n",
                  (unsigned long)(now - enterMs_), (unsigned long)tc, (unsigned long)tpush);
  }
#if TP_PERF_LOG
  if (perfStart_ == 0) perfStart_ = now;
  frames_++;
  rows_ += rows;
  composeUs_ += tc;
  pushUs_ += tpush;
  uint32_t win = now - perfStart_;
  if (win >= 2000) {
    if (perfLog_ && frames_ > 2) {
      Serial.printf("tp: %.1f fps  compose %.2f ms  push %.2f ms  %lu rows/frame  tilt %+.2f %+.2f\n",
                    frames_ * 1000.0f / win, composeUs_ / 1000.0f / frames_,
                    pushUs_ / 1000.0f / frames_, (unsigned long)(rows_ / frames_),
                    tilt_.targetX(), tilt_.targetY());
    }
    perfStart_ = now;
    frames_ = rows_ = composeUs_ = pushUs_ = 0;
  }
#endif
}

void ParallaxFaceView::drawFallback() {
  // PSRAM exhausted: plain time so the watch is still a watch.
  static int lastMin = -1;
  uint8_t h, m; bool ok;
  { ModelLock lk; h = model.hour; m = model.minute; ok = model.rtcOk; }
  if (!ok || m == lastMin) return;
  lastMin = m;
  gfx->fillScreen(0x0000);
  gfx->setTextSize(6);
  gfx->setTextColor(0xFFFF, 0x0000);
  gfx->setCursor(30, 110);
  gfx->printf("%02u:%02u", h, m);
  if (firstFramePending_) { firstFramePending_ = false; uiMarkFirstFrame(); }
}

// ---------------------------------------------------------------------------
void ParallaxFaceView::render() {
  if (!gfx) return;
  uint32_t now = millis();
  if (!ensureRenderer()) { drawFallback(); return; }

  ParallaxCmd cmd;
  while (parallaxCmdPop(cmd)) runCommand(cmd.line, now);

  // No scene and nothing in flight (e.g. a build failed for lack of PSRAM):
  // retry, but at most every 5 s.
  if (!scene_ && targetScene_ >= 0 && !parallaxGenBusy() &&
      (lastGenReqMs_ == 0 || now - lastGenReqMs_ > 5000)) {
    parallaxGenRequest(targetScene_);
    lastGenReqMs_ = now;
  }
  updateTransition(now);

  // ---- snapshot the model (short lock, no drawing while held) ----
  uint8_t hh, mm, ss, day, mon, batPct;
  uint16_t year;
  bool rtcOk, batOk;
  { ModelLock lk;
    hh = model.hour; mm = model.minute; ss = model.second;
    day = model.day; mon = model.month; year = model.year;
    rtcOk = model.rtcOk; batPct = model.batPct; batOk = model.batOk; }

  // On a cold boot / wake, give the RTC and the prewarmed scene a moment so
  // the very first frame is the finished face (the backlight waits for it).
  if (firstFramePending_) {
    // Both waits end before setup()'s 900 ms backlight gate gives up.
    bool timeReady = rtcOk || now - enterMs_ > 400;
    bool sceneReady = scene_ != nullptr || now - enterMs_ > 750;
    if (!timeReady || !sceneReady) return;
  }

  // ---- time of day, clock, status ----
  const ParallaxSettings &st = parallaxSettings();
  LocalTime lt;
  lt.year = rtcOk ? year : 2026;
  lt.month = rtcOk ? mon : 6;
  lt.day = rtcOk ? day : 21;
  lt.hour = rtcOk ? hh : 12;
  lt.minute = rtcOk ? mm : 0;
  lt.second = rtcOk ? ss : 0;
  if (timeOverride_ >= 0) { lt.hour = timeOverride_ / 60; lt.minute = timeOverride_ % 60; }
  fr_.setTimeOfDay(lt);
  fr_.setClock(lt.hour, lt.minute, rtcOk || timeOverride_ >= 0, st.h12);
  StatusInfo si;
  if (rtcOk && mon >= 1 && mon <= 12)
    snprintf(si.date, sizeof(si.date), "%s %u %s", kWd[weekdayOf(year, mon, day)], day, kMo[mon - 1]);
  // The ADC reading wobbles by a percent or two; only move the number when it
  // really changed (2+ points, or any change after a minute).
  if (!batOk) {
    shownBattery_ = -1;
  } else if (shownBattery_ < 0 || abs((int)batPct - shownBattery_) >= 2 ||
             ((int)batPct != shownBattery_ && now - batteryShownMs_ > 60000)) {
    shownBattery_ = batPct;
    batteryShownMs_ = now;
  }
  si.battery = shownBattery_;
  fr_.setStatus(si);

  // ---- tilt: every queued sample, then the spring ----
  ImuSample smp;
  while (tiltHookPop(smp)) tilt_.addSample(smp.x, smp.y, smp.z, smp.tMs);
  float tx = 0, ty = 0;
  if (tilt_.hasData()) tilt_.step(now, tx, ty);
  float strength = kStrengthScale[st.depth < kStrengthCount ? st.depth : kStrengthDefault];
  // Only ask for fast frames when something can actually move.
  moving_ = strength > 0 && tilt_.hasData() && !tilt_.settled();
  fr_.setParallax(tx, ty, strength);

  // ---- ambient: clouds drift ~2 px/s, anchored to the RTC so they don't
  //      jump back every time the watch wakes ----
  if (st.ambient && fr_.hasClouds()) {
    if (ss != lastSec_) { lastSec_ = ss; secAnchorMs_ = now; }
    uint32_t frac = now - secAnchorMs_;
    if (frac > 999) frac = 999;
    uint32_t msOfDay = ((uint32_t)hh * 3600u + mm * 60u + ss) * 1000u + frac;
    fr_.setCloudDrift((int)((msOfDay / 500u) % (uint32_t)fr_.cloudPeriod()));
  }
  if (st.ambient && fr_.starsVisible()) fr_.twinkle(now);

  if (fr_.anyDirty()) present(now);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
void ParallaxFaceView::onEvent(const Event &e) {
  switch (e.type) {
    case EventType::Gesture:
      if (e.gesture == Gesture::SwipeUp || e.gesture == Gesture::SwipeLeft) {
        pressActive_ = false;
        hapticBuzz(60, 40);
        switchTo(Screen::AppList);
      } else if (e.gesture == Gesture::SwipeDown) {
        openSettings();
      } else if (e.gesture == Gesture::LongPress && !longFired_) {
        longFired_ = true;
        openSettings();
      }
      return;
    case EventType::Touch:
      pressActive_ = true;
      pressMoved_ = false;
      longFired_ = false;
      pressX_ = e.x;
      pressY_ = e.y;
      pressMs_ = millis();
      return;
    case EventType::TouchHold: {
      if (!pressActive_) return;
      int dx = (int)e.x - pressX_, dy = (int)e.y - pressY_;
      if (dx * dx + dy * dy > 14 * 14) pressMoved_ = true;
      if (!pressMoved_ && !longFired_ && millis() - pressMs_ > 650) {
        longFired_ = true;
        openSettings();
      }
      return;
    }
    case EventType::TouchUp: {
      if (!pressActive_) return;
      int dx = (int)e.x - pressX_, dy = (int)e.y - pressY_;
      bool tap = !pressMoved_ && !longFired_ && dx * dx + dy * dy <= 14 * 14 &&
                 millis() - pressMs_ < 450;
      pressActive_ = false;
      if (tap && frReady_) {
        hapticBuzz(90, 30);
        int cur = targetScene_ >= 0 ? targetScene_ : (scene_ ? scene_->id : 0);
        startSceneChange(cur + 1, millis());
      }
      return;
    }
    default:
      return;
  }
}

// ---------------------------------------------------------------------------
// Serial commands (see parallax_cmd.h)
// ---------------------------------------------------------------------------
void ParallaxFaceView::bench() {
  if (!frReady_ || !scene_) { Serial.println("tp: bench needs a scene"); return; }
  uint16_t *canvasFb = nullptr;
  if (!strip_) {
    Arduino_Canvas *cv = frameCanvas();
    if (!cv) return;
    canvasFb = cv->getFramebuffer();
  }
  const int frames = 90;
  uint32_t t0 = millis(), tc = 0, tpush = 0, rows = 0;
  for (int f = 0; f < frames; f++) {
    esp_task_wdt_reset();
    float a = f * 0.21f;
    fr_.setParallax(sinf(a), 0.6f * cosf(a * 0.7f), 1.0f);
    fr_.markAllDirty();
    int y0, y1;
    while (fr_.popDirtyRun(y0, y1, kStripRows)) {
      uint32_t a1 = micros();
      uint16_t *dst = strip_ ? strip_ : canvasFb + (size_t)y0 * kScreenW;
      fr_.composeRows(y0, y1, dst, kScreenW);
      uint32_t b1 = micros();
      gfx->draw16bitRGBBitmap(0, y0, dst, kScreenW, y1 - y0);
      tc += b1 - a1;
      tpush += micros() - b1;
      rows += y1 - y0;
    }
  }
  uint32_t dt = millis() - t0;
  Serial.printf("tp: bench %d full frames in %lu ms = %.1f fps; compose %.2f ms, push %.2f ms per frame\n",
                frames, (unsigned long)dt, frames * 1000.0f / dt, tc / 1000.0f / frames,
                tpush / 1000.0f / frames);
  fr_.markAllDirty();
}

void ParallaxFaceView::runCommand(const char *line, uint32_t now) {
  char cmd[16] = { 0 }, arg[24] = { 0 };
  sscanf(line, "%15s %23s", cmd, arg);
  ParallaxSettings &st = parallaxSettings();
  if (!strcmp(cmd, "help")) {
    Serial.println("tp commands: scene <n|next>, depth <0-3>, time <HH:MM|off>, ambient <on|off>,\n"
                   "  h12 <on|off>, perf <on|off>, bench, tilt, axis <x|y> (flip), info");
  } else if (!strcmp(cmd, "scene")) {
    int id = !strcmp(arg, "next") || !arg[0] ? (scene_ ? scene_->id + 1 : 0) : atoi(arg);
    startSceneChange(id, now);
  } else if (!strcmp(cmd, "depth")) {
    int d = atoi(arg);
    if (d >= 0 && d < kStrengthCount) { st.depth = (uint8_t)d; parallaxSettingsSave(); }
    Serial.printf("tp: depth %s\n", kStrengthName[st.depth]);
  } else if (!strcmp(cmd, "time")) {
    int h, m;
    if (sscanf(arg, "%d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60) timeOverride_ = h * 60 + m;
    else timeOverride_ = -1;
    Serial.printf("tp: time override %s\n", timeOverride_ >= 0 ? arg : "off");
  } else if (!strcmp(cmd, "ambient")) {
    st.ambient = strcmp(arg, "off") != 0;
    parallaxSettingsSave();
  } else if (!strcmp(cmd, "h12")) {
    st.h12 = strcmp(arg, "off") != 0;
    parallaxSettingsSave();
  } else if (!strcmp(cmd, "perf")) {
    perfLog_ = strcmp(arg, "off") != 0;
  } else if (!strcmp(cmd, "bench")) {
    bench();
  } else if (!strcmp(cmd, "tilt")) {
    float nx, ny, nz;
    tilt_.neutral(nx, ny, nz);
    Serial.printf("tp: tilt raw %+.3f %+.3f target %+.3f %+.3f neutral %+.2f %+.2f %+.2f dropped %lu\n",
                  tilt_.rawX(), tilt_.rawY(), tilt_.targetX(), tilt_.targetY(), nx, ny, nz,
                  (unsigned long)tiltHookDropped());
  } else if (!strcmp(cmd, "axis")) {
    TiltConfig c = tilt_.config();
    if (arg[0] == 'x') c.invertX = !c.invertX;
    if (arg[0] == 'y') c.invertY = !c.invertY;
    tilt_.configure(c);
    Serial.printf("tp: invertX=%d invertY=%d (session only; set defaults in tp_tilt.h)\n",
                  c.invertX, c.invertY);
  } else if (!strcmp(cmd, "info")) {
    Serial.printf("tp: scene %d (%s) %u KB built in %lu ms; renderer %u KB; strip %s; "
                  "free heap %u, free PSRAM %u\n",
                  scene_ ? scene_->id : -1, scene_ ? sceneName(scene_->id) : "-",
                  scene_ ? (unsigned)(scene_->bytes() / 1024) : 0,
                  scene_ ? (unsigned long)scene_->buildMs : 0UL, (unsigned)(fr_.bytes() / 1024),
                  strip_ ? "internal" : "canvas", (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getFreePsram());
  } else {
    Serial.printf("tp: unknown command '%s' (try help)\n", cmd);
  }
  fr_.markAllDirty();
}
