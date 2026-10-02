// Pixel Pet scenes — full-screen layouts for the watch face and the pet app,
// drawn with px into a 240x280 RGB565 buffer. Pure C++: the watch passes its
// frame canvas, tools/preview passes a host buffer.
#pragma once
#include <stdint.h>
#include "px.h"
#include "petart.h"

namespace scenes {

constexpr int W = 240, H = 280;

// Horizontal bands of the face, so the watch can flush only what changed.
enum class Band : uint8_t { Top, Pet, Stats };
void bandRows(Band b, int &y0, int &h);

struct FaceData {
  // clock
  bool     rtcOk = true;
  bool     clockUnset = false;   // RTC year looks unset: show a hint
  uint8_t  hour = 12, minute = 0, second = 0;
  uint8_t  weekday = 0, day = 1, month = 1;
  bool     colonOn = true;
  // battery
  bool     batOk = false;
  uint8_t  batPct = 0;
  // steps + food
  uint32_t stepsToday = 0;
  uint16_t goal = 6000;
  uint16_t toSnack = 400;        // steps to the next snack (or to hatching)
  uint8_t  snackPct = 0;         // progress to the next snack / hatch
  uint8_t  pantry = 0;           // snacks in the bowl
  bool     walking = false;
  // the pet
  petart::Pose pose;
  const char *name = "BIX";
  // transient caption (e.g. "+1 SNACK!"), shown above the pet while set
  const char *toast = nullptr;
  uint32_t toastAgeMs = 0;
};

// Draw one band (or all of them) of the watch face.
void drawFace(px::Canvas &c, const FaceData &d, uint32_t nowMs, Band band);
void drawFaceAll(px::Canvas &c, const FaceData &d, uint32_t nowMs);
// Hit areas on the face.
bool faceHitPet(int x, int y);
bool faceHitStats(int x, int y);

// ---------------------------------------------------------------------------
// Pet app (launcher). Four pages, swiped vertically.
// ---------------------------------------------------------------------------
enum class Page : uint8_t { Pet = 0, Today, Week, Options, Count };

struct AppData {
  FaceData face;                 // shared bits (pose, steps, food, clock)
  const char *stageName = "KID";
  const char *moodName = "HAPPY";
  float    fullness = 70;        // 0..100 belly meter
  uint16_t snacksToday = 0;
  uint16_t streak = 0, bestStreak = 0;
  uint32_t lifetimeSteps = 0;
  uint8_t  stagePct = 0;         // progress to the next stage
  uint32_t toNextStage = 0;      // steps (0 = max stage)
  const char *nextStageName = "ADULT";
  uint32_t ageDays = 0;
  uint32_t week[7] = {0};        // oldest .. today
  uint8_t  weekDay0 = 0;         // weekday (0=Sun) of week[0]
  const char *accessoryName = "NONE";
  // options
  bool optBgSteps = true;
  bool optPetFace = true;
  bool optSnackBuzz = true;
  bool optNudges = true;
  uint8_t resetArmPct = 0;       // hold-to-reset progress 0..100
  bool bgAvailable = true;       // background stepping compiled in
};

void drawApp(px::Canvas &c, Page page, const AppData &d, uint32_t nowMs);

// Options page rows (hit testing): returns -1 or the row index.
enum class OptRow : uint8_t { BgSteps = 0, PetFace, SnackBuzz, Nudges, GoalMinus, GoalPlus, Rename, Reset, Count };
int  appOptionAt(int x, int y);
bool appHitPet(int x, int y);
bool appHitBowl(int x, int y);

}  // namespace scenes
