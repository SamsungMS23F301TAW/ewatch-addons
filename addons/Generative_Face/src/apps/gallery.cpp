#include <Arduino_GFX_Library.h>
#include <stdio.h>
#include "gallery.h"
#include "display.h"
#include "haptic.h"
#include "model.h"
#include "artslots.h"
#include "daystore.h"
#include "steps_hw.h"
#include "gf_settings.h"
#include "gf_face.h"
#include "gf_date.h"

static bool sFromFace = false;
static char sRemixTitle[24];
static char sRemixLine[40];

void GalleryView::openFromFace(bool fromFace) { sFromFace = fromFace; }

// Today's date from the RTC (0 if the clock is unset).
static uint16_t todayIndex() {
  uint16_t y; uint8_t m, d; bool ok;
  { ModelLock lk; y = model.year; m = model.month; d = model.day; ok = model.rtcOk; }
  return (ok && gf::plausibleDate(y, m, d)) ? gf::dayIndex(y, m, d) : 0;
}

// Page idx (0 = today) -> the day it shows. Records are newest first and a
// record for today's date (possible after a clock change) is hidden behind
// the live page.
static bool pageDay(int32_t idx, uint16_t today, gf::DayRecord &out) {
  if (idx == 0) {
    out.day = today;
    out.steps = (stepsDay() == today) ? stepsToday() : 0;
    out.algo = gf::ALGO_VERSION;
    return true;
  }
  int32_t n = daystoreCount();
  int32_t k = idx;
  for (int32_t i = n - 1; i >= 0; i--) {
    gf::DayRecord r;
    if (!daystoreAt(i, r) || r.day == today) continue;
    if (--k == 0) { out = r; return true; }
  }
  return false;
}

static int32_t pageCount(uint16_t today) {
  gf::DayRecord r;
  int32_t n = daystoreCount();
  return n + 1 - (daystoreFind(today, r) ? 1 : 0);
}

void GalleryView::onEnter() {
  if (!daystoreAttempted() && gfx) {
    // First use ever: mounting may format the flash partition (a few
    // seconds, once). Say so instead of looking frozen.
    gfx->fillScreen(BLACK);
    gfx->setTextColor(WHITE, BLACK);
    gfx->setTextSize(2);
    gfx->setCursor(30, 120);
    gfx->print("Preparing the");
    gfx->setCursor(30, 144);
    gfx->print("gallery...");
  }
  idx_ = 0;
  remix_ = false;
  caption_ = true;
  count_ = pageCount(todayIndex());
  select(0);
}

void GalleryView::onExit() {
  ArtSlot *slot = artSlotShared();
  if (slot && artSharedLock(50)) {
    slot->owner = kOwnerNone;       // nobody resumes a half-drawn gallery page
    artSharedUnlock();
  }
}

void GalleryView::select(int32_t idx) {
  uint16_t today = todayIndex();
  gf::DayRecord r;
  if (!pageDay(idx, today, r)) return;
  idx_ = idx;
  day_ = r.day;
  steps_ = r.steps;
  algo_ = (r.algo == 0 || r.algo > gf::ALGO_VERSION) ? gf::ALGO_VERSION : r.algo;
  remix_ = false;
  restart_ = true;
  needCompose_ = true;
}

void GalleryView::render() {
  if (pendingTapMs_ && (int32_t)(millis() - pendingTapMs_) >= 0) {
    pendingTapMs_ = 0;
    caption_ = !caption_;
    needCompose_ = true;
    hapticBuzz(40, 30);
  }
  ArtSlot *slot = artSlotShared();
  Arduino_Canvas *cv = frameCanvas();
  if (!slot || !cv) {
    static bool told = false;
    if (!told && gfx) {
      told = true;
      gfx->fillScreen(BLACK);
      gfx->setTextColor(WHITE, BLACK);
      gfx->setTextSize(2);
      gfx->setCursor(24, 124);
      gfx->print("Not enough memory");
      gfx->setCursor(24, 148);
      gfx->print("for the gallery");
    }
    return;
  }
  if (!artSharedLock(0)) return;            // a serial SNAP is rendering; try later
  if (slot->owner != kOwnerGallery) restart_ = true;
  if (restart_) {
    if (remix_) {
      // This week's remix: gather the Monday-based week around day_.
      uint16_t monday = (uint16_t)(day_ - (uint16_t)((gf::weekdayOf(day_) + 6) % 7));
      uint16_t days[7]; uint32_t steps[7]; int32_t n = 0;
      uint16_t today = todayIndex();
      for (int32_t k = 0; k < 7; k++) {
        uint16_t d = (uint16_t)(monday + k);
        gf::DayRecord r;
        if (d == today) { days[n] = d; steps[n] = stepsToday(); n++; }
        else if (daystoreFind(d, r)) { days[n] = d; steps[n] = r.steps; n++; }
      }
      gf::ArtSpec spec;
      uint16_t bucket;
      gf::describeWeekRemix(monday, days, steps, n, spec, bucket);
      slot->job.beginSpec(&slot->canvas, slot->scratch, spec, bucket);
      uint32_t total = 0;
      for (int32_t k = 0; k < n; k++) total += steps[k];
      uint16_t yy; uint8_t mm, dd;
      gf::dayToDate(monday, yy, mm, dd);
      snprintf(sRemixTitle, sizeof(sRemixTitle), "WEEK OF %u %s", dd, gf::monthShort(mm));
      char num[16];
      gf::formatSteps(total, num);
      snprintf(sRemixLine, sizeof(sRemixLine), "REMIX %c %c %s %c %s", gf::kGlyphDot,
               gf::kGlyphSteps, num, gf::kGlyphDot, gf::familyName(spec.family));
    } else {
      slot->job.begin(&slot->canvas, slot->scratch, day_, gf::bucketForSteps(steps_), algo_);
    }
    slot->owner = kOwnerGallery;
    slot->layer.valid = false;
    restart_ = false;
    needCompose_ = true;
    tones_ = slot->job.spec().pal.darkBg ? 0 : 2;
  }
  bool artMoved = false, finished = false;
  if (!slot->job.done()) {
    artMoved = true;
    // Short slices in calls that also compose a frame (see genface.cpp).
    bool composeSoon = needCompose_ || millis() - lastComposeMs_ >= 90;
    finished = artRunFor(slot->job, composeSoon ? 8000 : 25000);
  }
  const gf::ArtSpec &spec = slot->job.spec();
  if (finished) tones_ = gf::chooseTones(slot->canvas, spec, gf::kModeGallery, tones_);

  gf::FaceInputs in;
  in.day = day_;
  in.steps = steps_;
  in.mode = caption_ ? gf::kModeGallery : gf::kModePlain;
  in.isToday = (idx_ == 0) && !remix_;
  if (remix_) {
    in.caption1 = sRemixTitle;
    in.caption2 = sRemixLine;
  } else if (idx_ == 0 && caption_) {
    if (daystoreAttempted() && !daystoreAvailable()) in.note = "COLLECTION UNAVAILABLE";
    else in.note = count_ > 1 ? "SWIPE UP FOR EARLIER DAYS" : "A NEW PIECE IS KEPT EACH NIGHT";
  }
  uint32_t key = gf::faceKey(spec, in, tones_);
  if (!slot->layer.valid || slot->layer.key != key) {
    gf::buildFaceLayer(spec, in, tones_, slot->layer);
    needCompose_ = true;
  }
  uint32_t now = millis();
  if (needCompose_ || finished || (artMoved && now - lastComposeMs_ >= 90)) {
    gf::composeFrame(slot->canvas, spec, in, slot->layer, cv->getFramebuffer(), 0, gf::kH);
    cv->flush();
    lastComposeMs_ = now;
    needCompose_ = false;
  }
  artSharedUnlock();
}

void GalleryView::onEvent(const Event &e) {
  if (e.type == EventType::ButtonShort) {
    bool fromFace = sFromFace;
    sFromFace = false;
    switchTo(fromFace ? Screen::Watch : Screen::AppList);
    return;
  }
  if (e.type == EventType::Gesture) {
    pressActive_ = false;
    pendingTapMs_ = 0;
    if (e.gesture == Gesture::SwipeUp) {
      if (remix_) { remix_ = false; restart_ = true; hapticBuzz(50, 40); return; }
      if (idx_ + 1 < count_) { select(idx_ + 1); hapticBuzz(50, 40); }
      else { hapticBuzz(30, 25); hapticBuzz(0, 60); hapticBuzz(30, 25); }   // the end
    } else if (e.gesture == Gesture::SwipeDown) {
      if (remix_) { remix_ = false; restart_ = true; hapticBuzz(50, 40); return; }
      if (idx_ > 0) { select(idx_ - 1); hapticBuzz(50, 40); }
    } else if (e.gesture == Gesture::LongPress) {
      remix_ = !remix_;
      restart_ = true;
      caption_ = true;
      hapticBuzz(90, 60);
    }
    return;
  }
  if (e.type == EventType::Touch) {
    pressActive_ = true; pressMoved_ = false;
    pressX_ = e.x; pressY_ = e.y; pressMs_ = millis();
    return;
  }
  if (e.type == EventType::TouchHold && pressActive_) {
    int dx = (int)e.x - pressX_, dy = (int)e.y - pressY_;
    if (dx * dx + dy * dy > 16 * 16) pressMoved_ = true;
    return;
  }
  if (e.type == EventType::TouchUp) {
    if (pressActive_ && !pressMoved_ && millis() - pressMs_ < 600) pendingTapMs_ = millis() + 140;
    pressActive_ = false;
  }
}
