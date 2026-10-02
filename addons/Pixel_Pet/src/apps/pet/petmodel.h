// Pixel Pet — the rules of the creature. Pure C++ (no Arduino, no FreeRTOS)
// so every rule is unit-tested on the host (test/test_pet).
//
// THE LOOP
//   Walking is the only food. Every `stepsPerSnack` steps the pet earns a
//   snack. It eats immediately if its belly has room; otherwise the snack goes
//   into a small pantry (the bowl) and is eaten automatically as soon as there
//   is room again. At bedtime the pet has a "bedtime feast": it eats what is
//   left in the bowl up to full and shares the rest with the neighbours, so
//   the bowl spreads a walk across the day but can't bank food for tomorrow.
//   When belly and bowl are both full, extra snacks are shared too — walking
//   more still grows the pet (evolution is driven by lifetime steps).
//
// TIME BASE
//   Fullness drains over real time anchored to the RTC epoch (seconds since
//   2000-01-01, *local* time — the RV-3028 keeps local time). The state stores
//   the epoch at which it was last true; update(now) replays the elapsed time
//   exactly (piecewise: slower at night, pantry meals at the instant there is
//   room), so one big update after a deep sleep equals many small ones.
//
// MOOD = f(fullness)
//   happy >= 70 > content >= 50 > peckish >= 32 > grumpy >= 15 > furious.
//   Ecstatic is a reward state: >= 90 AND fed within 45 minutes (so a good
//   walker's resting mood is happy, and a walk makes it light up). Furious for
//   2 h straight turns into sulking (the pet turns its back). The pet sleeps
//   in its night window and never nags then.
#pragma once
#include <stdint.h>

namespace pet {

enum class Mood : uint8_t { Ecstatic = 0, Happy, Content, Peckish, Grumpy, Furious, Count };
enum class Stage : uint8_t { Egg = 0, Baby, Kid, Adult, Elder, Count };
enum class Accessory : uint8_t { None = 0, Bow, PartyHat, FlowerCrown, Crown, Count };

struct Tuning {
  uint16_t stepsPerSnack     = 400;
  float    snackFill         = 8.5f;    // fullness points per snack
  float    drainDayPerHour   = 7.0f;    // full -> grumpy in ~10 h awake
  float    drainNightPerHour = 1.5f;    // slow metabolism while asleep
  uint8_t  pantryMax         = 3;       // snacks the bowl can hold
  uint8_t  bedHour           = 22;      // pet's night window [bed, wake)
  uint8_t  wakeHour          = 7;
  float    hatchFullness     = 72.0f;
  uint32_t hatchSteps        = 300;     // first walk hatches the egg
  uint32_t kidSteps          = 25000;   // lifetime steps since birth
  uint32_t adultSteps        = 120000;
  uint32_t elderSteps        = 500000;
  float    evolveMinFull     = 32.0f;   // won't grow while grumpy or worse
  float    moodLevels[5]     = {90.0f, 70.0f, 50.0f, 32.0f, 15.0f};
  uint32_t ecstaticSec       = 2700;    // ecstatic = full AND fed within this

  uint32_t sulkAfterSec      = 2 * 3600;
  // nudges
  uint8_t  nudgeFromHour     = 9;
  uint8_t  nudgeToHour       = 20;
  uint32_t nudgeGapSec       = 2 * 3600;
  uint8_t  nudgesPerDay      = 3;
  uint32_t nudgeQuietSec     = 30 * 60; // no nudge within 30 min of walking
  // default goal
  uint16_t defaultGoal       = 6000;
  uint32_t maxReplaySec      = 14u * 86400u;
};

// Persisted state. POD; versioned by `magic`. Keep fields append-only.
struct State {
  uint32_t magic;
  uint32_t bornEpoch;        // 0 until the first valid clock
  uint32_t lastEpoch;        // fields below are true as of this epoch (0 = unanchored)
  float    fullness;         // 0..100
  uint8_t  pantry;
  uint8_t  stage;            // Stage
  uint8_t  nameIdx;
  uint8_t  nudgesToday;
  uint32_t stepsSeen;        // step-service lifetime count already consumed
  uint32_t stepBank;         // steps toward the next snack
  uint32_t walked;           // steps walked since birth (drives evolution/hatch)
  uint32_t totalSnacks;
  uint16_t snacksToday;
  uint16_t goal;             // daily step goal
  uint32_t snackDay;         // day snacksToday belongs to
  uint32_t goalDay;          // last day the goal was reached
  uint16_t streak;           // consecutive goal days ending at goalDay
  uint16_t bestStreak;
  uint32_t furiousSince;     // epoch fullness fell below the furious line (0 = not)
  uint32_t lastNudge;
  uint32_t nudgeDay;
  uint32_t lastStepEpoch;
  uint32_t lastAteEpoch;
  uint32_t hatchEpoch;
};

struct Events {
  uint8_t snacksEarned = 0;   // new snacks from walking (all destinations)
  uint8_t snacksEaten  = 0;   // eaten immediately on earning
  uint8_t snacksStored = 0;   // went into the bowl
  uint8_t snacksWasted = 0;   // belly + bowl full
  uint8_t pantryEaten  = 0;   // eaten from the bowl during the replay
  uint8_t pantryShared = 0;   // bowl leftovers given away at bedtime
  bool    hatched      = false;
  bool    evolved      = false;
  bool    goalReached  = false;
  bool    streakMilestone = false;   // streak just hit 3/7/14/30
  bool    moodChanged  = false;
  Mood    moodBefore   = Mood::Content;
  Mood    moodAfter    = Mood::Content;
};

class Model {
public:
  static constexpr uint32_t kMagic = 0x50455432;   // "PET2"
  static constexpr int kNameCount = 16;

  explicit Model(const Tuning &t = Tuning());

  // A fresh egg. `stepsNow` is the step service's lifetime count (steps from
  // before the egg existed don't count). `now` may be 0 if the clock is invalid.
  void newPet(uint32_t now, uint32_t stepsNow, uint8_t nameIdx);
  bool load(const State &s);                 // false if magic mismatches
  const State &state() const { return s_; }
  const Tuning &tuning() const { return t_; }

  // Advance to `now` (0 = clock invalid: steps are still consumed, no drift)
  // and consume steps up to `lifetimeSteps`. `todaySteps`/`day` come from
  // the step book and drive the daily goal + streak.
  Events update(uint32_t now, uint32_t lifetimeSteps, uint32_t todaySteps, uint32_t day);

  // Tap to serve a snack from the bowl (only if there is room for it).
  bool serveFromPantry(uint32_t now);

  // --- queries ---
  Stage     stage()     const { return (Stage)s_.stage; }
  bool      isEgg()     const { return s_.stage == (uint8_t)Stage::Egg; }
  Mood      mood()      const;
  float     fullness()  const { return s_.fullness; }
  uint8_t   pantry()    const { return s_.pantry; }
  bool      asleep(uint32_t now) const;          // night window
  bool      sulking(uint32_t now) const;         // furious for sulkAfterSec
  uint16_t  stepsToNextSnack() const;
  uint8_t   snackProgressPct() const;            // 0..100
  uint32_t  stepsToHatch() const;
  uint32_t  nextStageSteps() const;              // 0 at max stage
  uint8_t   stageProgressPct() const;
  uint16_t  currentStreak(uint32_t day) const;   // 0 if the streak is broken
  Accessory accessory() const;                   // best unlocked by bestStreak
  const char *name() const;
  uint32_t  ageDays(uint32_t now) const;

  // Nudges: a grumpy pet buzzes the wearer at most nudgesPerDay times, never
  // at night, never soon after walking, at least nudgeGapSec apart.
  bool      nudgeDue(uint32_t now) const;
  void      markNudged(uint32_t now);
  // Earliest epoch (within 24 h) at which a nudge would become due if no
  // steps are walked meanwhile; 0 = none. Used to schedule a deep-sleep wake.
  uint32_t  nextNudgeTime(uint32_t now) const;

  // Mood for an arbitrary fullness (shared with the art + tests).
  Mood      moodFor(float fullness) const;

  // Debug / console.
  void      debugSetFullness(float f) { s_.fullness = (f >= 0.0f) ? (f > 100.0f ? 100.0f : f) : 0.0f; }   // NaN -> 0

  static const char *moodName(Mood m);
  static const char *stageName(Stage st);
  static const char *accessoryName(Accessory a);
  static uint32_t    hourOf(uint32_t epoch) { return (epoch % 86400u) / 3600u; }

private:
  void   drift(uint32_t from, uint32_t to);
  float  drainRate(uint32_t epoch) const;
  uint32_t nextBoundary(uint32_t epoch) const;   // next bed/wake hour boundary
  void   earnSnack(uint32_t now, Events &ev);
  void   touchDay(uint32_t day);
  void   trackFurious(uint32_t at);

  Tuning t_;
  State  s_;
  uint32_t shared_ = 0;   // bowl snacks given away at bedtime during a drift
};

}  // namespace pet
