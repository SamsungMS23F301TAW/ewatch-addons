#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <string.h>
#include <stdio.h>
#include "petviews.h"
#include "petsvc.h"
#include "steps.h"
#include "display.h"
#include "haptic.h"
#include "model.h"
#include "controller.h"
#include "bgpower.h"

using petart::Act;

// ---------------------------------------------------------------------------
// Shared animation state
// ---------------------------------------------------------------------------
void PetStageAnim::say(const char *s, uint32_t now, uint32_t ms) {
  strncpy(toast, s, sizeof(toast) - 1);
  toast[sizeof(toast) - 1] = 0;
  toastStart = now;
  toastMs = ms;
}

void PetStageAnim::expire(uint32_t now) {
  if (act != Act::Idle && act != Act::Walk && now - actStart >= petart::actDuration(act)) {
    act = Act::Idle;
  }
  // toastStart may lie in the future (a caption delayed behind an animation)
  if (toastMs && (int32_t)(now - toastStart) >= 0 && now - toastStart >= toastMs) {
    toastMs = 0; toast[0] = 0;
  }
}

void PetStageAnim::consumeServiceEvents(uint32_t now, const char *petName, bool asleep) {
  PetUiEvent e;
  char buf[24];
  while (petSvcPopUi(e)) {
    switch (e.kind) {
      case PetUi::Snack:
        if (!asleep) play(Act::Eat, now);
        if (e.count > 1) snprintf(buf, sizeof buf, "+%u SNACKS!", e.count);
        else snprintf(buf, sizeof buf, "+1 SNACK!");
        say(buf, now);
        break;
      case PetUi::Hatched:
        play(Act::Hatch, now);
        snprintf(buf, sizeof buf, "HI! I'M %s", petName);
        say(buf, now + 1200, 2600);
        toastStart = now + 1400;
        break;
      case PetUi::Evolved:
        play(Act::Celebrate, now);
        snprintf(buf, sizeof buf, "%s GREW!", petName);
        say(buf, now, 2400);
        break;
      case PetUi::Goal:
        play(Act::Celebrate, now);
        say(e.count > 1 ? "STREAK PRIZE!" : "GOAL MET!", now, 2400);
        break;
      case PetUi::Nudge:
        play(Act::Nudge, now);
        say("WALK ME!", now, 2000);
        break;
      default:
        break;
    }
  }
}

static const char *pettedLine(pet::Mood m, bool asleep, bool egg) {
  if (egg) return "WALK TO HATCH!";
  if (asleep) return "ZZZ...";
  switch (m) {
    case pet::Mood::Ecstatic: return "^ BEST DAY ^";
    case pet::Mood::Happy:    return "HEE HEE!";
    case pet::Mood::Content:  return "HELLO!";
    case pet::Mood::Peckish:  return "SNACK?";
    case pet::Mood::Grumpy:   return "HMPH.";
    default:                  return "GO WALK!";
  }
}

// Build the scene data shared by the face and the app.
static void fillFace(scenes::FaceData &d, const PetTime &t, const PetView &v,
                     const StepsSnapshot &ss, const PetStageAnim &anim, uint32_t now) {
  uint8_t bat; bool batOk;
  { ModelLock lk; bat = model.batPct; batOk = model.batOk; }
  d.rtcOk = t.ok;
  d.clockUnset = !t.ok;
  d.hour = t.hour; d.minute = t.minute; d.second = t.second;
  d.weekday = t.weekday; d.day = t.mday; d.month = t.month;
  d.colonOn = (t.second % 2) == 0 || !t.ok;
  d.batOk = batOk; d.batPct = bat;
  d.stepsToday = (!t.ok || ss.day == t.day) ? ss.today : 0;   // stale until midnight rolls
  d.goal = v.goal;
  d.toSnack = v.toSnack;
  d.snackPct = v.snackPct;
  d.pantry = v.pantry;
  const bool walking = ss.walking || (ss.lastStepMs && now - ss.lastStepMs < 2500);
  d.walking = walking;
  d.pose.stage = v.stage;
  d.pose.mood = v.mood;
  d.pose.acc = v.acc;
  d.pose.asleep = v.asleep;
  d.pose.sulking = v.sulking;
  d.pose.eggPct = v.eggPct;
  d.pose.clockMs = now;
  Act act = anim.act;
  if (act == Act::Idle && walking) act = Act::Walk;
  d.pose.act = act;
  d.pose.animMs = now - anim.actStart;
  if (act == Act::Hatch && now < anim.actStart) d.pose.animMs = 0;
  d.name = v.name;
  d.toast = (anim.toastMs && (int32_t)(now - anim.toastStart) >= 0 && anim.toast[0]) ? anim.toast : nullptr;
  d.toastAgeMs = now - anim.toastStart;
}

// ---------------------------------------------------------------------------
// Watch face
// ---------------------------------------------------------------------------
void PetFaceView::onEnter() {
  full = true;
  topKey = statsKey = 0xFFFFFFFF;
  pressActive = false;
  anim = PetStageAnim();
  eggHintMs = 0;
}

void PetFaceView::onExit() {}

void PetFaceView::render() {
  if (!gfx) return;
  Arduino_Canvas *cv = frameCanvas();
  if (!cv) {
    if (full) { gfx->fillScreen(BLACK); gfx->setTextColor(WHITE); gfx->setCursor(10, 130); gfx->print("Stepmunk: no memory"); full = false; }
    return;
  }
  const uint32_t now = millis();
  const PetTime t = petTimeFromModel();
  PetView v = petSvcView(t);
  anim.consumeServiceEvents(now, v.name, v.asleep);
  anim.expire(now);
  // First-run onboarding: tell a new owner what the egg wants.
  if (v.egg && !anim.toastMs && (eggHintMs == 0 || now - eggHintMs > 30000)) {
    anim.say("WALK TO HATCH!", now, 3000);
    eggHintMs = now;
  }
  const StepsSnapshot ss = stepsSvcSnapshot();
  scenes::FaceData d;
  fillFace(d, t, v, ss, anim, now);

  px::Canvas c(cv->getFramebuffer(), 240, 280);
  int y0, h;
  const uint32_t tk = ((uint32_t)t.hour << 24) ^ ((uint32_t)t.minute << 16) ^
                      ((uint32_t)d.colonOn << 8) ^ (d.batOk ? d.batPct : 255u) ^ (t.ok ? 0x80000000u : 0u);
  if (full || tk != topKey) {
    scenes::drawFace(c, d, now, scenes::Band::Top);
    scenes::bandRows(scenes::Band::Top, y0, h);
    displayFlushRows((int16_t)y0, (int16_t)h);
    topKey = tk;
  }
  const uint32_t petEvery = (v.asleep && anim.act == Act::Idle) ? 250 : 100;   // calm at night
  if (full || now - lastPetMs >= petEvery) {
    scenes::drawFace(c, d, now, scenes::Band::Pet);
    scenes::bandRows(scenes::Band::Pet, y0, h);
    displayFlushRows((int16_t)y0, (int16_t)h);
    lastPetMs = now;
  }
  const uint32_t sk = d.stepsToday * 31u + d.toSnack * 7u + d.snackPct + (d.walking ? (1u + (now / 250) % 3) : 0u) * 100003u +
                      (uint32_t)d.pose.stage * 7919u;
  if (full || sk != statsKey) {
    scenes::drawFace(c, d, now, scenes::Band::Stats);
    scenes::bandRows(scenes::Band::Stats, y0, h);
    displayFlushRows((int16_t)y0, (int16_t)h);
    statsKey = sk;
  }
  full = false;
}

void PetFaceView::onEvent(const Event &e) {
  const uint32_t now = millis();
  if (e.type == EventType::ButtonShort) {        // button (or right-swipe): screen off
    requestScreenOff();
    return;
  }
  if (e.type == EventType::Gesture) {
    if (e.gesture == Gesture::SwipeLeft || e.gesture == Gesture::SwipeUp) {
      hapticBuzz(60, 60);
      pressActive = false;
      switchTo(Screen::AppList);
      return;
    }
    if (e.gesture == Gesture::SwipeDown) {
      hapticBuzz(60, 60);
      pressActive = false;
      switchTo(Screen::PetApp);
      return;
    }
    return;
  }
  if (e.type == EventType::Touch) {
    pressActive = true; pressMoved = false;
    pressX = e.x; pressY = e.y; pressMs = now;
    return;
  }
  if (e.type == EventType::TouchHold && pressActive) {
    int dx = (int)e.x - pressX, dy = (int)e.y - pressY;
    if (dx * dx + dy * dy > 18 * 18) pressMoved = true;
    return;
  }
  if (e.type == EventType::TouchUp) {
    const bool tap = pressActive && !pressMoved && now - pressMs < 600;
    pressActive = false;
    if (!tap) return;
    const PetTime t = petTimeFromModel();
    if (scenes::faceHitStats(pressX, pressY)) {
      hapticBuzz(60, 50);
      switchTo(Screen::PetApp);
      return;
    }
    if (pressX >= 186 && pressY >= 170 && pressY < 226) {     // the bowl
      if (petSvcServe(t)) return;
    }
    if (scenes::faceHitPet(pressX, pressY)) {
      PetView v = petSvcView(t);
      pet::Mood m = petSvcPetted(t);
      if (!v.egg && !v.asleep) anim.play(Act::Petted, now);
      anim.say(pettedLine(m, v.asleep, v.egg), now, 1400);
    }
  }
}

// ---------------------------------------------------------------------------
// Pet app (launcher)
// ---------------------------------------------------------------------------
void PetAppView::onEnter() {
  page = scenes::Page::Pet;
  anim = PetStageAnim();
  pressActive = false;
  resetHeld = false;
  resetDone = false;
}

void PetAppView::onExit() { petSvcSave(false); }

static const char *nextStageName(pet::Stage s) {
  switch (s) {
    case pet::Stage::Egg:   return "BABY";
    case pet::Stage::Baby:  return "KID";
    case pet::Stage::Kid:   return "ADULT";
    case pet::Stage::Adult: return "ELDER";
    default:                return "";
  }
}

void PetAppView::render() {
  if (!gfx) return;
  Arduino_Canvas *cv = frameCanvas();
  if (!cv) return;
  const uint32_t now = millis();
  const PetTime t = petTimeFromModel();
  PetView v = petSvcView(t);
  anim.consumeServiceEvents(now, v.name, v.asleep);
  anim.expire(now);
  const StepsSnapshot ss = stepsSvcSnapshot();

  scenes::AppData d;
  fillFace(d.face, t, v, ss, anim, now);
  d.stageName = pet::Model::stageName(v.stage);
  d.moodName = v.egg ? "WALK TO HATCH" : (v.asleep ? "ASLEEP" : (v.sulking ? "SULKING" : pet::Model::moodName(v.mood)));
  d.fullness = v.fullness;
  d.snacksToday = v.snacksToday;
  d.streak = v.streak;
  d.bestStreak = v.bestStreak;
  d.lifetimeSteps = ss.lifetime;
  d.stagePct = v.stagePct;
  d.toNextStage = v.toNextStage;
  d.nextStageName = nextStageName(v.stage);
  d.ageDays = v.ageDays;
  d.accessoryName = pet::Model::accessoryName(v.acc);
  if (page == scenes::Page::Week) {
    for (int i = 0; i < 7; i++) {
      uint32_t day = t.ok ? t.day - (uint32_t)(6 - i) : 0;
      d.week[i] = t.ok ? stepsSvcOnDay(day) : (i == 6 ? ss.today : 0);
    }
    // 2000-01-01 was a Saturday (6): weekday of day N = (N + 6) % 7
    d.weekDay0 = t.ok ? (uint8_t)((t.day - 6 + 6) % 7) : 0;
  }
  PetOptions o = petSvcOptions();
  d.optBgSteps = o.bgSteps;
  d.optPetFace = o.petFace;
  d.optSnackBuzz = o.snackBuzz;
  d.optNudges = o.nudges;
  d.bgAvailable = bgCompiled() && !bgSafeMode();
  d.resetArmPct = 0;
  if (resetHeld && !resetDone) {
    uint32_t held = now - resetStart;
    d.resetArmPct = (uint8_t)(held >= 2000 ? 100 : held * 100 / 2000);
    if (held >= 2000) {
      resetDone = true;
      petSvcNewEgg(t);
      hapticBuzz(200, 160);
      page = scenes::Page::Pet;
      anim.say("A NEW EGG!", now, 2000);
    }
  }

  px::Canvas c(cv->getFramebuffer(), 240, 280);
  scenes::drawApp(c, page, d, now);
  cv->flush();
}

void PetAppView::tap(int x, int y) {
  const uint32_t now = millis();
  const PetTime t = petTimeFromModel();
  if (tappedBack((uint16_t)x, (uint16_t)y)) { hapticBuzz(60, 50); switchTo(Screen::AppList); return; }
  if (page == scenes::Page::Pet) {
    if (scenes::appHitBowl(x, y)) { if (petSvcServe(t)) return; }
    if (scenes::appHitPet(x, y)) {
      PetView v = petSvcView(t);
      pet::Mood m = petSvcPetted(t);
      if (!v.egg && !v.asleep) anim.play(Act::Petted, now);
      anim.say(pettedLine(m, v.asleep, v.egg), now, 1400);
    }
    return;
  }
  if (page != scenes::Page::Options) return;
  PetOptions o = petSvcOptions();
  switch ((scenes::OptRow)scenes::appOptionAt(x, y)) {
    case scenes::OptRow::BgSteps:
      if (!bgCompiled()) { hapticBuzz(40, 120); return; }
      o.bgSteps = !o.bgSteps; break;
    case scenes::OptRow::PetFace:   o.petFace = !o.petFace; break;
    case scenes::OptRow::SnackBuzz: o.snackBuzz = !o.snackBuzz; break;
    case scenes::OptRow::Nudges:    o.nudges = !o.nudges; break;
    case scenes::OptRow::GoalMinus: petSvcGoalStep(-1); hapticBuzz(40, 40); return;
    case scenes::OptRow::GoalPlus:  petSvcGoalStep(+1); hapticBuzz(40, 40); return;
    case scenes::OptRow::Rename:    petSvcRename(); hapticBuzz(50, 50); return;
    default: return;
  }
  petSvcSetOptions(o);
  hapticBuzz(50, 50);
}

void PetAppView::onEvent(const Event &e) {
  const uint32_t now = millis();
  if (e.type == EventType::ButtonShort) { switchTo(Screen::AppList); return; }
  if (e.type == EventType::Gesture) {
    int p = (int)page;
    if (e.gesture == Gesture::SwipeUp && p + 1 < (int)scenes::Page::Count) { page = (scenes::Page)(p + 1); hapticBuzz(40, 40); }
    if (e.gesture == Gesture::SwipeDown && p > 0) { page = (scenes::Page)(p - 1); hapticBuzz(40, 40); }
    pressActive = false;
    resetHeld = false;
    return;
  }
  if (e.type == EventType::Touch) {
    pressActive = true; pressMoved = false;
    pressX = e.x; pressY = e.y; pressMs = now;
    if (page == scenes::Page::Options &&
        scenes::appOptionAt(e.x, e.y) == (int)scenes::OptRow::Reset) {
      resetHeld = true; resetStart = now; resetDone = false;
    }
    return;
  }
  if (e.type == EventType::TouchHold && pressActive) {
    int dx = (int)e.x - pressX, dy = (int)e.y - pressY;
    if (dx * dx + dy * dy > 18 * 18) { pressMoved = true; resetHeld = false; }
    return;
  }
  if (e.type == EventType::TouchUp) {
    const bool tapped = pressActive && !pressMoved && now - pressMs < 600;
    pressActive = false;
    const bool wasReset = resetHeld;
    resetHeld = false;
    if (tapped && !wasReset) tap(pressX, pressY);
  }
}

// ---------------------------------------------------------------------------
// Launcher tile art
// ---------------------------------------------------------------------------
void petDrawLauncherIcon(uint16_t *fb, int cx, int top, uint32_t nowMs) {
  if (!fb) return;
  const PetTime t = petTimeFromModel();
  const PetView v = petSvcView(t);
  petart::Pose p;
  p.stage = v.stage;
  p.mood = v.mood;
  p.acc = v.acc;
  p.asleep = v.asleep;
  p.sulking = v.sulking;
  p.eggPct = v.eggPct;
  p.clockMs = nowMs;
  px::Canvas c(fb, 240, 280);
  const int scale = 3;
  petart::draw(c, p, cx - petart::kW * scale / 2, top, scale);
}
