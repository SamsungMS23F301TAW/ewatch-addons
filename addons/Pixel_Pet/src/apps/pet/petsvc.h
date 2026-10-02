// Pet service — the on-watch glue around the pure pet::Model:
//   * feeds it the RTC epoch + the step service's counts,
//   * turns its events into haptics (meal, goal, hatch, evolve, nudge) while
//     respecting quiet hours and the user's options,
//   * queues UI events so the face/app can play the matching animation,
//   * persists it (RTC-memory mirror for deep sleep, NVS for power-off) and
//     owns the addon options.
// Thread-safe: called from the render task (face, app, background loop) and
// the serial console.
#pragma once
#include <stdint.h>
#include "petmodel.h"

struct PetTime {
  bool     ok = false;         // RTC valid and set
  uint32_t epoch = 0;          // local seconds since 2000-01-01
  uint32_t day = 0;            // epoch / 86400 (0 when !ok)
  uint8_t  hour = 0, minute = 0, second = 0;
  uint8_t  weekday = 0, mday = 1, month = 1;
  uint16_t year = 2000;
};
PetTime petTimeFromModel();    // takes ModelLock briefly
PetTime petTimeFrom(uint8_t h, uint8_t m, uint8_t s, uint8_t wd, uint8_t d, uint8_t mo,
                    uint16_t y, bool rtcOk);

struct PetOptions {
  bool bgSteps   = true;       // background step counting (light sleep sampler)
  bool petFace   = true;       // pet watch face instead of the stock face
  bool snackBuzz = true;       // buzz when a snack is earned
  bool nudges    = true;       // grumpy nudges
};

enum class PetUi : uint8_t { None, Snack, Hatched, Evolved, Goal, Nudge };
struct PetUiEvent { PetUi kind = PetUi::None; uint8_t count = 0; };

struct PetView {
  pet::Stage stage; pet::Mood mood; pet::Accessory acc;
  bool asleep, sulking, egg;
  float fullness;
  uint8_t pantry, snackPct, eggPct, stagePct;
  uint16_t toSnack, goal, streak, bestStreak, snacksToday;
  uint32_t walked, toNextStage, ageDays;
  const char *name;
};

// Load the pet: the RTC mirror if trusted and newer, else NVS, else a new egg.
void        petSvcInit(bool trustRtcMirror);
// Advance the pet to `t` with the latest step counts. Buzzes and queues UI
// events per options. `screenOn` = someone may be looking (queue animations).
void        petSvcUpdate(const PetTime &t, bool screenOn);
bool        petSvcPopUi(PetUiEvent &e);
void        petSvcSave(bool force);
PetView     petSvcView(const PetTime &t);

// interactions (tap the pet, serve from the bowl, settings)
pet::Mood   petSvcPetted(const PetTime &t);
bool        petSvcServe(const PetTime &t);
void        petSvcGoalStep(int dir);            // +/- 500 steps, 2,000..20,000
void        petSvcRename();
void        petSvcNewEgg(const PetTime &t);

PetOptions  petSvcOptions();
void        petSvcSetOptions(const PetOptions &o);
bool        petSvcFaceEnabled();

// Earliest epoch a nudge could become due (0 = none) — deep-sleep timer.
uint32_t    petSvcNextNudge(const PetTime &t);

// console
void        petSvcDebugFullness(float f);
void        petSvcPrint();
