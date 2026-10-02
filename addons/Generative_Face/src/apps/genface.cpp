#include <Arduino_GFX_Library.h>
#include <esp_attr.h>
#include "genface.h"
#include "display.h"
#include "haptic.h"
#include "model.h"
#include "artslots.h"
#include "steps_hw.h"
#include "gf_settings.h"
#include "gf_face.h"
#include "gf_date.h"
#include "gallery.h"

// The clock's text tone for today, remembered across deep-sleep reboots so
// a fresh bloom doesn't flip the clock colour when it completes.
RTC_DATA_ATTR static uint16_t gToneDay = 0xFFFF;
RTC_DATA_ATTR static uint8_t  gTones = 0;

// Render-task budget per call: art work is sliced so that a call which also
// composes and flushes a frame (~30 ms on the watch) stays under ~40 ms.
static const uint32_t kSliceUs = 25000;      // art work in a call that doesn't compose
static const uint32_t kSliceComposeUs = 8000;// art work in a call that composes
static const uint32_t kBloomFrameMs = 90;    // composite cadence while blooming
static uint32_t sPendingTapMs = 0;           // tap resolves after swipe check

namespace {
struct Snap {
  uint8_t  h, m;
  bool     rtcOk;
  uint16_t y;
  uint8_t  mo, d;
  uint8_t  batPct;
  bool     batOk;
};
Snap snapModel() {
  Snap s;
  ModelLock lk;
  s.h = model.hour; s.m = model.minute; s.rtcOk = model.rtcOk;
  s.y = model.year; s.mo = model.month; s.d = model.day;
  s.batPct = model.batPct; s.batOk = model.batOk;
  return s;
}
}  // namespace

void GenFaceView::onEnter() {
  mode_ = gf::kModeClock;
  needCompose_ = true;
  enterMs_ = millis();
  pressActive_ = false;
  sPendingTapMs = 0;
  ArtSlot *slot = artSlotFace();
  if (slot) slot->layer.valid = false;      // the frame canvas was someone else's
}

void GenFaceView::onWake() {
  wakeMs_ = millis();
  mode_ = gf::kModeClock;
  pressActive_ = false;
  sPendingTapMs = 0;
  frame(true);
}

void GenFaceView::render() {
  if (sPendingTapMs && (int32_t)(millis() - sPendingTapMs) >= 0) {
    sPendingTapMs = 0;
    mode_ = (mode_ == gf::kModeClock) ? gf::kModeArt : gf::kModeClock;
    artTonesStale_ = true;
    hapticBuzz(45, 35);
  }
  frame(false);
}

void GenFaceView::frame(bool forceCompose) {
  ArtSlot *slot = artSlotFace();
  Arduino_Canvas *cv = frameCanvas();
  if (!slot || !cv) { fallback(); return; }
  Snap s = snapModel();
  GfSettings set = gfSettings();

  bool plausible = gf::plausibleDate(s.y, s.mo, s.d);
  bool dateOk = s.rtcOk && plausible && s.y >= 2024;
  uint16_t today = plausible ? gf::dayIndex(s.y, s.mo, s.d) : 0;
  uint32_t steps = (!dateOk || stepsDay() == today) ? stepsToday() : 0;
  uint16_t bucket = gf::bucketForSteps(steps);

  // Keep the art job in step with the day and the step bucket.
  gf::ArtJob &job = slot->job;
  if (!job.active() || job.day() != today || bucket < job.bucket()) {
    job.begin(&slot->canvas, slot->scratch, today, bucket);
    forceCompose = true;
  } else if (bucket > job.bucket() && !job.extendTo(bucket)) {
    job.begin(&slot->canvas, slot->scratch, today, bucket);
    forceCompose = true;
  }
  bool artMoved = false, justFinished = false;
  if (!job.done()) {
    artMoved = true;
    bool composeSoon = forceCompose || needCompose_ || millis() - lastComposeMs_ >= kBloomFrameMs;
    justFinished = artRunFor(job, composeSoon ? kSliceComposeUs : kSliceUs);
  }
  const gf::ArtSpec &spec = job.spec();

  // Text tones: cached per day while a piece blooms, measured once done.
  uint8_t tones = (gToneDay == today) ? gTones : (spec.pal.darkBg ? 0 : 3);
  if (job.done() && (justFinished || gToneDay != today)) {
    tones = gf::chooseTones(slot->canvas, spec, gf::kModeClock, tones);
    gToneDay = today;
    gTones = tones;
  }
  if (mode_ != gf::kModeClock) {
    // The art view's caption sits at the bottom: measure once per change.
    if (artTonesStale_ || justFinished) {
      artTones_ = gf::chooseTones(slot->canvas, spec, mode_, artTones_);
      artTonesStale_ = false;
    }
    tones = artTones_;
  }

  gf::FaceInputs in;
  in.day = today;
  in.hour = s.h;
  in.minute = s.m;
  in.rtcOk = s.rtcOk;
  in.timeUnset = !dateOk;
  in.steps = steps;
  in.goal = set.goal;
  in.mode = mode_;
  in.batteryLow = s.batOk && s.batPct <= 10;
  in.ambient = set.ambient;
  uint32_t now = millis();
  bool animating = set.animate && mode_ == gf::kModeClock;
  if (animating) in.lightPhase = (uint16_t)((now * 5u) | 1u);

  bool layerChanged = false;
  uint32_t key = gf::faceKey(spec, in, tones);
  if (!slot->layer.valid || slot->layer.key != key) {
    gf::buildFaceLayer(spec, in, tones, slot->layer);
    layerChanged = true;
  }

  bool due = forceCompose || layerChanged || needCompose_ || justFinished ||
             (artMoved && now - lastComposeMs_ >= kBloomFrameMs) ||
             (animating && now - lastComposeMs_ >= kBloomFrameMs);
  if (!due) return;
  gf::composeFrame(slot->canvas, spec, in, slot->layer, cv->getFramebuffer(), 0, gf::kH);
  cv->flush();
  lastComposeMs_ = now;
  needCompose_ = false;
}

// Without PSRAM there is no art: a plain, readable clock instead.
void GenFaceView::fallback() {
  static uint8_t lastM = 99;
  Snap s = snapModel();
  if (!gfx || (s.m == lastM && !needCompose_)) return;
  lastM = s.m;
  needCompose_ = false;
  gfx->fillScreen(BLACK);
  gfx->setTextColor(WHITE, BLACK);
  gfx->setTextSize(6);
  char buf[24];
  snprintf(buf, sizeof(buf), "%02u:%02u", s.h, s.m);
  gfx->setCursor(30, 100);
  gfx->print(buf);
  gfx->setTextSize(2);
  snprintf(buf, sizeof(buf), "%lu steps", (unsigned long)stepsToday());
  gfx->setCursor(40, 200);
  gfx->print(buf);
}

void GenFaceView::onEvent(const Event &e) {
  bool touchy = e.type == EventType::Touch || e.type == EventType::TouchHold ||
                e.type == EventType::TouchUp || e.type == EventType::Gesture;
  if (touchy && millis() - wakeMs_ < 450) return;      // the tap that woke us
  if (e.type == EventType::Gesture) {
    pressActive_ = false;
    sPendingTapMs = 0;
    if (e.gesture == Gesture::SwipeLeft || e.gesture == Gesture::SwipeUp) {
      hapticBuzz(60, 70);
      switchTo(Screen::AppList);
    } else if (e.gesture == Gesture::SwipeDown) {
      hapticBuzz(60, 70);
      GalleryView::openFromFace(true);
      switchTo(Screen::Gallery);
    } else if (e.gesture == Gesture::LongPress) {
      hapticBuzz(80, 60);
      switchTo(Screen::FaceSettings);
    }
    return;
  }
  if (e.type == EventType::ButtonShort) {
    if (mode_ != gf::kModeClock) { mode_ = gf::kModeClock; needCompose_ = true; }
    return;
  }
  if (e.type == EventType::Touch) {
    pressActive_ = true;
    pressMoved_ = false;
    pressX_ = e.x; pressY_ = e.y;
    pressMs_ = millis();
    return;
  }
  if (e.type == EventType::TouchHold && pressActive_) {
    int dx = (int)e.x - pressX_, dy = (int)e.y - pressY_;
    if (dx * dx + dy * dy > 16 * 16) pressMoved_ = true;
    return;
  }
  if (e.type == EventType::TouchUp) {
    if (pressActive_ && !pressMoved_ && millis() - pressMs_ < 600) {
      // Resolve shortly after release so a swipe's gesture event, which can
      // arrive after the finger lifts, cancels it.
      sPendingTapMs = millis() + 140;
    }
    pressActive_ = false;
  }
}
