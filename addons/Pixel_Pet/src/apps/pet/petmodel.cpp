// Pixel Pet rules — see petmodel.h. Pure C++; compiled into host tests.
#include "petmodel.h"
#include <math.h>
#include <string.h>

namespace pet {

namespace {
// Original names — short, friendly, pronounceable.
const char *const kNames[Model::kNameCount] = {
  "Bix", "Nib", "Tuck", "Sprig", "Toffee", "Biscuit", "Nugget", "Waffle",
  "Pickle", "Button", "Crumb", "Fig", "Kiwi", "Maple", "Noodle", "Peanut",
};

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline uint8_t sat8(int v) { return (uint8_t)(v > 255 ? 255 : (v < 0 ? 0 : v)); }
}  // namespace

Model::Model(const Tuning &t) : t_(t) {
  memset(&s_, 0, sizeof(s_));
  s_.magic = kMagic;
  s_.goal = t_.defaultGoal;
}

void Model::newPet(uint32_t now, uint32_t stepsNow, uint8_t nameIdx) {
  memset(&s_, 0, sizeof(s_));
  s_.magic = kMagic;
  s_.fullness = t_.hatchFullness;
  s_.stage = (uint8_t)Stage::Egg;
  s_.nameIdx = (uint8_t)(nameIdx % kNameCount);
  s_.stepsSeen = stepsNow;
  s_.goal = t_.defaultGoal;
  s_.bornEpoch = now;
  s_.lastEpoch = now;
}

bool Model::load(const State &s) {
  if (s.magic != kMagic) return false;
  s_ = s;
  // Defensive clamps: a corrupted record must never produce NaNs or
  // out-of-range enums on screen.
  if (!(s_.fullness >= 0.0f && s_.fullness <= 100.0f)) s_.fullness = 50.0f;
  if (s_.stage >= (uint8_t)Stage::Count) s_.stage = (uint8_t)Stage::Baby;
  if (s_.pantry > t_.pantryMax) s_.pantry = t_.pantryMax;
  if (s_.stepBank >= t_.stepsPerSnack) s_.stepBank = 0;
  if (s_.goal < 1000 || s_.goal > 30000) s_.goal = t_.defaultGoal;
  s_.nameIdx %= kNameCount;
  return true;
}

// ---------------------------------------------------------------------------
// Time helpers
// ---------------------------------------------------------------------------
bool Model::asleep(uint32_t now) const {
  if (now == 0) return false;
  const uint32_t h = hourOf(now);
  if (t_.bedHour > t_.wakeHour) return h >= t_.bedHour || h < t_.wakeHour;
  return h >= t_.bedHour && h < t_.wakeHour;
}

float Model::drainRate(uint32_t epoch) const {
  return (asleep(epoch) ? t_.drainNightPerHour : t_.drainDayPerHour) / 3600.0f;
}

uint32_t Model::nextBoundary(uint32_t epoch) const {
  const uint32_t day0 = epoch - epoch % 86400u;
  uint32_t best = 0xFFFFFFFFu;
  for (uint32_t d = 0; d < 2; d++) {
    const uint32_t a = day0 + d * 86400u + (uint32_t)t_.wakeHour * 3600u;
    const uint32_t b = day0 + d * 86400u + (uint32_t)t_.bedHour * 3600u;
    if (a > epoch && a < best) best = a;
    if (b > epoch && b < best) best = b;
  }
  return best;
}

void Model::trackFurious(uint32_t at) {
  if (s_.fullness >= t_.moodLevels[4]) s_.furiousSince = 0;
  else if (s_.furiousSince == 0) s_.furiousSince = at;
}

// Replay [from, to): linear drain at the day or night rate, split at the
// bed/wake boundaries; whenever the belly has room for a whole snack and the
// bowl isn't empty, the pet eats one at that instant.
void Model::drift(uint32_t from, uint32_t to) {
  if (to <= from) return;
  if (to - from > t_.maxReplaySec) from = to - t_.maxReplaySec;
  const float eatLevel = 100.0f - t_.snackFill;
  const float furiousLine = t_.moodLevels[4];
  uint32_t t = from;

  auto eatFromBowl = [&](uint32_t at) {
    s_.pantry--;
    s_.fullness = clampf(s_.fullness + t_.snackFill, 0.0f, 100.0f);
    s_.lastAteEpoch = at;
    trackFurious(at);
  };
  auto drainTo = [&](uint32_t t0, uint32_t t1, float r) {
    const float before = s_.fullness;
    const float after = clampf(before - r * (float)(t1 - t0), 0.0f, 100.0f);
    if (before >= furiousLine && after < furiousLine && s_.furiousSince == 0) {
      s_.furiousSince = t0 + (uint32_t)((before - furiousLine) / r);
    }
    s_.fullness = after;
  };

  while (s_.pantry > 0 && s_.fullness <= eatLevel) eatFromBowl(t);

  while (t < to) {
    // Crossing into the night: bedtime feast. Eat bowl leftovers while there
    // is room for at least half a snack, give the rest away.
    if (t != from && hourOf(t) == t_.bedHour && t % 3600u == 0 && s_.pantry > 0) {
      while (s_.pantry > 0 && s_.fullness <= 100.0f - 0.5f * t_.snackFill) eatFromBowl(t);
      shared_ += s_.pantry;
      s_.pantry = 0;
    }
    uint32_t segEnd = nextBoundary(t);
    if (segEnd > to) segEnd = to;
    const float r = drainRate(t);
    while (t < segEnd) {
      if (s_.pantry > 0) {
        const float secs = (s_.fullness - eatLevel) / r;
        const uint32_t te = t + (uint32_t)ceilf(secs > 0.0f ? secs : 0.0f);
        if (te < segEnd) {
          drainTo(t, te, r);
          t = te;
          eatFromBowl(t);
          continue;
        }
      }
      drainTo(t, segEnd, r);
      t = segEnd;
    }
  }
}

void Model::touchDay(uint32_t day) {
  if (day == 0) return;
  if (s_.snackDay != day) { s_.snackDay = day; s_.snacksToday = 0; }
}

void Model::earnSnack(uint32_t now, Events &ev) {
  ev.snacksEarned = sat8(ev.snacksEarned + 1);
  s_.totalSnacks++;
  if (s_.snacksToday < 0xFFFF) s_.snacksToday++;
  if (s_.fullness <= 100.0f - 0.5f * t_.snackFill) {
    s_.fullness = clampf(s_.fullness + t_.snackFill, 0.0f, 100.0f);
    s_.lastAteEpoch = now;
    trackFurious(now);
    ev.snacksEaten = sat8(ev.snacksEaten + 1);
  } else if (s_.pantry < t_.pantryMax) {
    s_.pantry++;
    ev.snacksStored = sat8(ev.snacksStored + 1);
  } else {
    ev.snacksWasted = sat8(ev.snacksWasted + 1);
  }
}

// ---------------------------------------------------------------------------
// update
// ---------------------------------------------------------------------------
Events Model::update(uint32_t now, uint32_t lifetimeSteps, uint32_t todaySteps, uint32_t day) {
  Events ev;
  ev.moodBefore = mood();
  const uint8_t pantryBefore = s_.pantry;

  if (now != 0) {
    if (s_.lastEpoch == 0) {
      s_.lastEpoch = now;                      // first valid clock: anchor only
      if (s_.bornEpoch == 0) s_.bornEpoch = now;
    } else if (now < s_.lastEpoch) {
      s_.lastEpoch = now;                      // clock set backwards: re-anchor
    } else {
      if (!isEgg()) drift(s_.lastEpoch, now);
      s_.lastEpoch = now;
    }
  }
  if (s_.pantry + shared_ < pantryBefore) ev.pantryEaten = (uint8_t)(pantryBefore - s_.pantry - shared_);
  ev.pantryShared = sat8((int)shared_);
  shared_ = 0;

  const uint32_t at = now ? now : s_.lastEpoch;
  touchDay(day);

  // ---- steps -> snacks ----
  if (lifetimeSteps < s_.stepsSeen) s_.stepsSeen = lifetimeSteps;   // counter reset
  uint32_t delta = lifetimeSteps - s_.stepsSeen;
  s_.stepsSeen = lifetimeSteps;
  if (delta > 200000u) delta = 200000u;        // corrupt jump guard
  if (delta > 0) {
    s_.walked += delta;
    s_.lastStepEpoch = at;
    if (isEgg()) {
      if (s_.walked >= t_.hatchSteps) {
        s_.stage = (uint8_t)Stage::Baby;
        s_.fullness = t_.hatchFullness;
        s_.hatchEpoch = at;
        s_.furiousSince = 0;
        s_.stepBank = (s_.walked - t_.hatchSteps) % t_.stepsPerSnack;
        ev.hatched = true;
      }
    } else {
      s_.stepBank += delta;
      while (s_.stepBank >= t_.stepsPerSnack) {
        s_.stepBank -= t_.stepsPerSnack;
        earnSnack(at, ev);
      }
    }
  }

  // ---- daily goal + streak ----
  if (day != 0 && todaySteps >= s_.goal && s_.goalDay != day) {
    s_.streak = (s_.goalDay != 0 && s_.goalDay + 1 == day) ? (uint16_t)(s_.streak + 1) : 1;
    s_.goalDay = day;
    if (s_.streak > s_.bestStreak) s_.bestStreak = s_.streak;
    ev.goalReached = true;
    const uint16_t st = s_.streak;
    ev.streakMilestone = (st == 3 || st == 7 || st == 14 || st == 30 || st == 50 || st == 100);
  }

  // ---- evolution (one stage per update, only when not grumpy) ----
  if (!isEgg() && s_.stage < (uint8_t)Stage::Elder && s_.fullness >= t_.evolveMinFull) {
    const uint32_t need = nextStageSteps();
    if (need != 0 && s_.walked >= need) {
      s_.stage++;
      ev.evolved = true;
    }
  }

  ev.moodAfter = mood();
  ev.moodChanged = ev.moodAfter != ev.moodBefore;
  return ev;
}

bool Model::serveFromPantry(uint32_t now) {
  if (isEgg() || s_.pantry == 0) return false;
  if (s_.fullness > 100.0f - 0.5f * t_.snackFill) return false;   // too full
  s_.pantry--;
  s_.fullness = clampf(s_.fullness + t_.snackFill, 0.0f, 100.0f);
  s_.lastAteEpoch = now;
  trackFurious(now);
  return true;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------
Mood Model::moodFor(float f) const {
  for (int i = 0; i < 5; i++) {
    if (f >= t_.moodLevels[i]) return (Mood)i;
  }
  return Mood::Furious;
}

Mood Model::mood() const {
  if (isEgg()) return Mood::Content;
  Mood m = moodFor(s_.fullness);
  if (m == Mood::Ecstatic) {
    const bool fedRecently = s_.lastAteEpoch != 0 && s_.lastEpoch >= s_.lastAteEpoch &&
                             s_.lastEpoch - s_.lastAteEpoch <= t_.ecstaticSec;
    if (!fedRecently) m = Mood::Happy;
  }
  return m;
}

bool Model::sulking(uint32_t now) const {
  return mood() == Mood::Furious && s_.furiousSince != 0 && now >= s_.furiousSince &&
         now - s_.furiousSince >= t_.sulkAfterSec;
}

uint16_t Model::stepsToNextSnack() const {
  if (isEgg()) return (uint16_t)(stepsToHatch() > 0xFFFF ? 0xFFFF : stepsToHatch());
  return (uint16_t)(t_.stepsPerSnack - s_.stepBank);
}

uint8_t Model::snackProgressPct() const {
  if (isEgg()) {
    return (uint8_t)(s_.walked >= t_.hatchSteps ? 100 : s_.walked * 100u / t_.hatchSteps);
  }
  return (uint8_t)(s_.stepBank * 100u / t_.stepsPerSnack);
}

uint32_t Model::stepsToHatch() const {
  return s_.walked >= t_.hatchSteps ? 0 : t_.hatchSteps - s_.walked;
}

uint32_t Model::nextStageSteps() const {
  switch ((Stage)s_.stage) {
    case Stage::Egg:   return t_.hatchSteps;
    case Stage::Baby:  return t_.kidSteps;
    case Stage::Kid:   return t_.adultSteps;
    case Stage::Adult: return t_.elderSteps;
    default:           return 0;
  }
}

uint8_t Model::stageProgressPct() const {
  uint32_t lo = 0, hi = nextStageSteps();
  switch ((Stage)s_.stage) {
    case Stage::Baby:  lo = t_.hatchSteps; break;
    case Stage::Kid:   lo = t_.kidSteps;   break;
    case Stage::Adult: lo = t_.adultSteps; break;
    case Stage::Elder: return 100;
    default: break;
  }
  if (hi <= lo) return 100;
  if (s_.walked <= lo) return 0;
  uint32_t p = (uint32_t)((uint64_t)(s_.walked - lo) * 100u / (hi - lo));
  return (uint8_t)(p > 100 ? 100 : p);
}

uint16_t Model::currentStreak(uint32_t day) const {
  if (s_.goalDay == 0 || day == 0) return 0;
  if (s_.goalDay == day || s_.goalDay + 1 == day) return s_.streak;
  return 0;
}

Accessory Model::accessory() const {
  const uint16_t b = s_.bestStreak;
  if (b >= 30) return Accessory::Crown;
  if (b >= 14) return Accessory::FlowerCrown;
  if (b >= 7)  return Accessory::PartyHat;
  if (b >= 3)  return Accessory::Bow;
  return Accessory::None;
}

const char *Model::name() const { return kNames[s_.nameIdx % kNameCount]; }

uint32_t Model::ageDays(uint32_t now) const {
  if (s_.bornEpoch == 0 || now <= s_.bornEpoch) return 0;
  return (now - s_.bornEpoch) / 86400u;
}

// ---------------------------------------------------------------------------
// Nudges
// ---------------------------------------------------------------------------
bool Model::nudgeDue(uint32_t now) const {
  if (now == 0 || isEgg()) return false;
  if (asleep(now)) return false;
  const uint32_t h = hourOf(now);
  if (h < t_.nudgeFromHour || h >= t_.nudgeToHour) return false;
  if (mood() < Mood::Grumpy) return false;
  if (s_.lastStepEpoch != 0 && now >= s_.lastStepEpoch &&
      now - s_.lastStepEpoch < t_.nudgeQuietSec) return false;
  const uint32_t day = now / 86400u;
  const uint8_t count = (s_.nudgeDay == day) ? s_.nudgesToday : 0;
  if (count >= t_.nudgesPerDay) return false;
  if (s_.lastNudge != 0 && now >= s_.lastNudge && now - s_.lastNudge < t_.nudgeGapSec) return false;
  return true;
}

void Model::markNudged(uint32_t now) {
  const uint32_t day = now / 86400u;
  if (s_.nudgeDay != day) { s_.nudgeDay = day; s_.nudgesToday = 0; }
  if (s_.nudgesToday < 255) s_.nudgesToday++;
  s_.lastNudge = now;
}

uint32_t Model::nextNudgeTime(uint32_t now) const {
  if (now == 0 || isEgg()) return 0;
  for (uint32_t dt = 0; dt <= 24u * 3600u; dt += 600u) {
    Model tmp = *this;
    tmp.update(now + dt, s_.stepsSeen, 0, 0);
    if (tmp.nudgeDue(now + dt)) return now + dt;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------
const char *Model::moodName(Mood m) {
  switch (m) {
    case Mood::Ecstatic: return "ECSTATIC";
    case Mood::Happy:    return "HAPPY";
    case Mood::Content:  return "CONTENT";
    case Mood::Peckish:  return "PECKISH";
    case Mood::Grumpy:   return "GRUMPY";
    case Mood::Furious:  return "FURIOUS";
    default:             return "?";
  }
}

const char *Model::stageName(Stage st) {
  switch (st) {
    case Stage::Egg:   return "EGG";
    case Stage::Baby:  return "BABY";
    case Stage::Kid:   return "KID";
    case Stage::Adult: return "ADULT";
    case Stage::Elder: return "ELDER";
    default:           return "?";
  }
}

const char *Model::accessoryName(Accessory a) {
  switch (a) {
    case Accessory::Bow:         return "BOW";
    case Accessory::PartyHat:    return "PARTY HAT";
    case Accessory::FlowerCrown: return "FLOWER CROWN";
    case Accessory::Crown:       return "CROWN";
    default:                     return "NONE";
  }
}

}  // namespace pet
