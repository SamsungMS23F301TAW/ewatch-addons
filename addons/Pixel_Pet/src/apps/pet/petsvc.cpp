#include <Arduino.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <type_traits>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "petsvc.h"
#include "steps.h"
#include "addonstore.h"
#include "haptic.h"
#include "model.h"
#include "apptimer.h"

namespace {

pet::Model        s_pet;
SemaphoreHandle_t s_lock = nullptr;
PetOptions        s_opts;
uint32_t          s_lastSaveMs = 0;
bool              s_dirty = false;
bool              s_saveNow = false;

PetUiEvent s_ui[8];
uint8_t    s_uiHead = 0, s_uiCount = 0;

struct Lock {
  Lock()  { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
  ~Lock() { if (s_lock) xSemaphoreGive(s_lock); }
};

struct OptsBlob {
  uint32_t   magic;
  PetOptions o;
};
const uint32_t kOptsMagic = 0x4F505431;   // "OPT1"

// Haptic vocabulary (original patterns). Intensities are pre-scaler.
const HapticStep kNom[]     = {{110, 35, 70}, {110, 35, 70}, {175, 90, 0}};             // nom-nom-yum
const HapticStep kParty[]   = {{90, 40, 50}, {130, 40, 50}, {175, 50, 60}, {235, 150, 0}};
const HapticStep kHatch[]   = {{80, 50, 110}, {80, 50, 110}, {120, 60, 80}, {170, 60, 60}, {255, 220, 0}};
const HapticStep kGrumble[] = {{70, 130, 90}, {85, 110, 90}, {125, 240, 0}};          // low rumble
const HapticStep kPurr[]    = {{55, 25, 45}, {65, 25, 0}};
const HapticStep kHmph[]    = {{135, 70, 80}, {75, 45, 0}};

template <size_t N> void play(const HapticStep (&p)[N]) { hapticPattern(p, (uint8_t)N); }

void pushUi(PetUi k, uint8_t count) {
  PetUiEvent e;
  e.kind = k;
  e.count = count;
  uint8_t idx = (uint8_t)((s_uiHead + s_uiCount) % 8);
  if (s_uiCount == 8) { s_uiHead = (uint8_t)((s_uiHead + 1) % 8); s_uiCount--; }
  s_ui[idx] = e;
  s_uiCount++;
}

}  // namespace

// RTC slow-memory mirror of the pet, refreshed on every change. RTC_NOINIT:
// unlike RTC_DATA_ATTR it is not re-initialised by the bootloader after a
// panic or watchdog reset, so a crash loses nothing; after a power cycle it
// is garbage and the magic + checksum reject it. Must stay a plain struct
// (a constructor would wipe it at boot).
static_assert(std::is_trivially_default_constructible<pet::State>::value,
              "the RTC mirror must stay a plain struct");
RTC_NOINIT_ATTR static pet::State s_rtcState;
RTC_NOINIT_ATTR static uint32_t   s_rtcCrc;

static void mirror() {
  s_rtcState = s_pet.state();
  s_rtcCrc = addonstore::fnv1a(&s_rtcState, sizeof(s_rtcState));
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
PetTime petTimeFrom(uint8_t h, uint8_t m, uint8_t s, uint8_t wd, uint8_t d, uint8_t mo,
                    uint16_t y, bool rtcOk) {
  PetTime t;
  t.hour = h; t.minute = m; t.second = s;
  t.weekday = wd; t.mday = d; t.month = mo; t.year = y;
  // An RV-3028 that was never set reads 2000-01-01: treat years before 2024
  // as "clock not set" so the pet neither starves nor counts fake days.
  t.ok = rtcOk && y >= 2024 && y <= 2099 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31 &&
         h < 24 && m < 60 && s < 60;
  t.epoch = rtcEpochSec(y, mo, d, h, m, s);
  t.day = t.ok ? t.epoch / 86400u : 0;
  return t;
}

PetTime petTimeFromModel() {
  uint8_t h, m, s, wd, d, mo;
  uint16_t y;
  bool ok;
  { ModelLock lk;
    h = model.hour; m = model.minute; s = model.second; wd = model.weekday;
    d = model.day; mo = model.month; y = model.year; ok = model.rtcOk; }
  return petTimeFrom(h, m, s, wd, d, mo, y, ok);
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------
static void saveLocked() {
  pet::State st = s_pet.state();
  s_dirty = false;
  s_saveNow = false;
  s_lastSaveMs = millis();
  addonstore::putBlob("pet", &st, sizeof(st));
}

void petSvcSave(bool force) {
  Lock lk;
  if (force || s_saveNow || (s_dirty && millis() - s_lastSaveMs >= 10UL * 60UL * 1000UL)) {
    saveLocked();
  }
}

void petSvcInit(bool trustRtcMirror) {
  if (!s_lock) s_lock = xSemaphoreCreateMutex();
  OptsBlob ob;
  if (addonstore::getBlob("opts", &ob, sizeof(ob)) && ob.magic == kOptsMagic) s_opts = ob.o;

  pet::State nv;
  const bool nvOk = addonstore::getBlob("pet", &nv, sizeof(nv)) && nv.magic == pet::Model::kMagic;
  const bool rtcOk = trustRtcMirror && s_rtcState.magic == pet::Model::kMagic &&
                     addonstore::fnv1a(&s_rtcState, sizeof(s_rtcState)) == s_rtcCrc;
  Lock lk;
  bool loaded = false;
  if (rtcOk && (!nvOk || s_rtcState.lastEpoch >= nv.lastEpoch)) loaded = s_pet.load(s_rtcState);
  if (!loaded && nvOk) loaded = s_pet.load(nv);
  if (!loaded) {
    StepsSnapshot ss = stepsSvcSnapshot();
    s_pet.newPet(0, ss.lifetime, (uint8_t)(esp_random() % pet::Model::kNameCount));
    saveLocked();
    Serial.printf("pet: a new egg (%s)\n", s_pet.name());
  } else {
    Serial.printf("pet: %s the %s, fullness %.1f\n", s_pet.name(),
                  pet::Model::stageName(s_pet.stage()), s_pet.fullness());
  }
  mirror();
}

// ---------------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------------
void petSvcUpdate(const PetTime &t, bool screenOn) {
  // Roll the step book to the current day first: right after midnight it may
  // not have seen a sample yet, and yesterday's total must never count
  // towards today's goal (that would fake a streak day).
  if (t.ok) stepsSvcRollTo(t.day);
  StepsSnapshot ss = stepsSvcSnapshot();
  const uint32_t todaySteps = (ss.day == t.day) ? ss.today : 0;
  Lock lk;
  pet::Events ev = s_pet.update(t.ok ? t.epoch : 0, ss.lifetime, todaySteps, t.ok ? t.day : 0);
  const bool asleep = t.ok && s_pet.asleep(t.epoch);
  const uint8_t fed = (uint8_t)(ev.snacksEaten + ev.snacksStored);

  // One buzz per update, the most important event wins. Nothing buzzes while
  // the pet sleeps (its night is the wearer's night).
  bool buzzed = false;
  if (ev.hatched) {
    pushUi(PetUi::Hatched, 1);
    if (!asleep) { play(kHatch); buzzed = true; }
    s_saveNow = true;
  } else if (ev.evolved) {
    pushUi(PetUi::Evolved, 1);
    if (!asleep) { play(kParty); buzzed = true; }
    s_saveNow = true;
  }
  if (ev.goalReached) {
    pushUi(PetUi::Goal, (uint8_t)(ev.streakMilestone ? 2 : 1));
    if (!asleep && !buzzed) { play(kParty); buzzed = true; }
    s_saveNow = true;
  }
  if (ev.snacksEarned) {
    if (fed && screenOn) pushUi(PetUi::Snack, fed);
    if (fed && s_opts.snackBuzz && !asleep && !buzzed) { play(kNom); buzzed = true; }
    s_saveNow = true;
  }
  if (ev.pantryEaten || ev.pantryShared) s_dirty = true;

  // Grumpy nudges: rules live in the model (daytime only, rate limited,
  // never soon after walking).
  if (s_opts.nudges && t.ok && s_pet.nudgeDue(t.epoch)) {
    s_pet.markNudged(t.epoch);
    pushUi(PetUi::Nudge, 1);
    if (!buzzed) play(kGrumble);
    s_dirty = true;
  }
  if (ev.moodChanged) s_dirty = true;
  mirror();
  if (s_saveNow) saveLocked();
}

bool petSvcPopUi(PetUiEvent &e) {
  Lock lk;
  if (!s_uiCount) return false;
  e = s_ui[s_uiHead];
  s_uiHead = (uint8_t)((s_uiHead + 1) % 8);
  s_uiCount--;
  return true;
}

PetView petSvcView(const PetTime &t) {
  Lock lk;
  PetView v;
  const pet::State &st = s_pet.state();
  v.stage = s_pet.stage();
  v.mood = s_pet.mood();
  v.acc = s_pet.accessory();
  v.egg = s_pet.isEgg();
  v.asleep = t.ok && s_pet.asleep(t.epoch);
  v.sulking = t.ok && s_pet.sulking(t.epoch);
  v.fullness = s_pet.fullness();
  v.pantry = s_pet.pantry();
  v.snackPct = s_pet.snackProgressPct();
  v.eggPct = v.egg ? s_pet.snackProgressPct() : 100;
  v.stagePct = s_pet.stageProgressPct();
  v.toSnack = s_pet.stepsToNextSnack();
  v.goal = st.goal;
  v.streak = s_pet.currentStreak(t.day);
  v.bestStreak = st.bestStreak;
  v.snacksToday = (st.snackDay == t.day) ? st.snacksToday : 0;
  v.walked = st.walked;
  uint32_t need = s_pet.nextStageSteps();
  v.toNextStage = (need > st.walked) ? need - st.walked : 0;
  if (s_pet.stage() == pet::Stage::Elder) v.toNextStage = 0;
  v.ageDays = t.ok ? s_pet.ageDays(t.epoch) : 0;
  v.name = s_pet.name();
  return v;
}

// ---------------------------------------------------------------------------
// Interactions
// ---------------------------------------------------------------------------
pet::Mood petSvcPetted(const PetTime &t) {
  Lock lk;
  pet::Mood m = s_pet.mood();
  if (t.ok && s_pet.asleep(t.epoch)) { hapticBuzz(40, 20); return m; }
  if (m <= pet::Mood::Peckish) play(kPurr);
  else play(kHmph);
  return m;
}

bool petSvcServe(const PetTime &t) {
  Lock lk;
  if (!s_pet.serveFromPantry(t.ok ? t.epoch : s_pet.state().lastEpoch)) return false;
  play(kNom);
  pushUi(PetUi::Snack, 1);
  saveLocked();
  mirror();
  return true;
}

void petSvcGoalStep(int dir) {
  Lock lk;
  pet::State st = s_pet.state();
  int g = (int)st.goal + (dir > 0 ? 500 : -500);
  if (g < 2000) g = 2000;
  if (g > 20000) g = 20000;
  st.goal = (uint16_t)g;
  s_pet.load(st);
  saveLocked();
  mirror();
}

void petSvcRename() {
  Lock lk;
  pet::State st = s_pet.state();
  st.nameIdx = (uint8_t)((st.nameIdx + 1) % pet::Model::kNameCount);
  s_pet.load(st);
  saveLocked();
  mirror();
}

void petSvcNewEgg(const PetTime &t) {
  StepsSnapshot ss = stepsSvcSnapshot();
  Lock lk;
  const uint16_t goal = s_pet.state().goal;
  s_pet.newPet(t.ok ? t.epoch : 0, ss.lifetime, (uint8_t)(esp_random() % pet::Model::kNameCount));
  pet::State st = s_pet.state();
  st.goal = goal;
  s_pet.load(st);
  s_uiCount = 0;
  saveLocked();
  mirror();
}

PetOptions petSvcOptions() {
  Lock lk;
  return s_opts;
}

void petSvcSetOptions(const PetOptions &o) {
  Lock lk;
  s_opts = o;
  OptsBlob ob;
  ob.magic = kOptsMagic;
  ob.o = o;
  addonstore::putBlob("opts", &ob, sizeof(ob));
}

bool petSvcFaceEnabled() {
  Lock lk;
  return s_opts.petFace;
}

uint32_t petSvcNextNudge(const PetTime &t) {
  Lock lk;
  if (!s_opts.nudges || !t.ok) return 0;
  return s_pet.nextNudgeTime(t.epoch);
}

void petSvcDebugFullness(float f) {
  Lock lk;
  s_pet.debugSetFullness(f);
  s_dirty = true;
  mirror();
}

void petSvcPrint() {
  Lock lk;
  const pet::State &st = s_pet.state();
  Serial.printf("pet %s stage=%s mood=%s fullness=%.1f bowl=%u bank=%lu/%u walked=%lu\n",
                s_pet.name(), pet::Model::stageName(s_pet.stage()),
                pet::Model::moodName(s_pet.mood()), s_pet.fullness(), s_pet.pantry(),
                (unsigned long)st.stepBank, s_pet.tuning().stepsPerSnack, (unsigned long)st.walked);
  Serial.printf("    goal=%u streak=%u best=%u snacksToday=%u lastEpoch=%lu nudges=%u\n",
                st.goal, st.streak, st.bestStreak, st.snacksToday, (unsigned long)st.lastEpoch,
                st.nudgesToday);
  Serial.printf("    opts bg=%d face=%d snackBuzz=%d nudges=%d\n", s_opts.bgSteps, s_opts.petFace,
                s_opts.snackBuzz, s_opts.nudges);
}
