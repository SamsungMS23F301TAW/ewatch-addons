// Rep Counter: the workout session around the detector.
//
// Owns what the user sees and what gets saved: whether a workout is running,
// the current set, the rest timer, today's log and a week of daily totals,
// and the settings. It turns detector events into log entries and haptic
// cue requests. Pure C++ (no Arduino); the view feeds it events and the time,
// persists the blobs it marks dirty, and plays the cues.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "rep_types.h"
#include "rep_detector.h"

namespace reps {

// Wall-clock date/time from the RTC (local time).
struct Date {
  uint16_t year = 0;
  uint8_t month = 0, day = 0, hour = 0, minute = 0, second = 0;
  bool valid = false;
  bool sameDay(uint16_t y, uint8_t m, uint8_t d) const { return year == y && month == m && day == d; }
};

// One finished set. 12 bytes; persisted, so the layout is versioned.
struct SetRecord {
  uint8_t  exercise = 0;       // Exercise
  uint8_t  reps = 0;
  uint8_t  flags = 0;          // kSetManual, kSetNoClock
  uint8_t  confidence = 0;     // 0..3
  uint16_t startMin = 0;       // minute of the day the set began (local)
  uint16_t durationS = 0;      // first rep -> last rep
  uint16_t restS = 0xFFFF;     // rest before it (0xFFFF: first set of the day)
  uint16_t tempoCs = 0;        // seconds per rep x 100
};
enum : uint8_t { kSetManual = 1, kSetNoClock = 2 };

static const int kMaxSets = 40;
struct DayLog {
  uint16_t year = 0;
  uint8_t month = 0, day = 0;
  uint8_t count = 0;
  uint8_t pad[3] = {0, 0, 0};
  SetRecord sets[kMaxSets];
  uint16_t totalReps() const;
};

struct DaySummary {
  uint16_t year = 0;
  uint8_t month = 0, day = 0;
  uint16_t sets = 0, reps = 0;
};
static const int kHistDays = 7;
struct History {                 // newest first
  uint8_t n = 0;
  uint8_t pad[3] = {0, 0, 0};
  DaySummary days[kHistDays];
};

struct Settings {
  uint8_t  mode = (uint8_t)Mode::Auto;
  uint8_t  wrist = (uint8_t)Wrist::Left;
  uint8_t  setEndSec = 4;      // 3..10
  uint8_t  repTick = 1;
  uint16_t restGoalSec = 90;   // 0 = off
  int8_t   handSign = 0;       // learned hand direction (detector)
  uint8_t  pad = 0;
  void sanitize();
};

// Choices offered on the settings page.
static const uint8_t kSetEndChoices[] = {3, 4, 5, 6, 8};
static const uint16_t kRestGoalChoices[] = {0, 60, 90, 120, 180, 240};

// ---- persistence blobs --------------------------------------------------------
// [magic u32][version u16][payload size u16][payload]. A blob with the wrong
// magic, version or size is rejected and the caller keeps its defaults.
enum BlobKind : uint32_t { kBlobLog = 0x52434C47, kBlobHist = 0x52434849, kBlobSettings = 0x52435354 };
static const uint16_t kBlobVersion = 1;
static const size_t kBlobHeader = 8;
size_t packBlob(BlobKind kind, const void *payload, size_t size, uint8_t *out, size_t cap);
bool   unpackBlob(BlobKind kind, const uint8_t *in, size_t len, void *payload, size_t size);

// ---- session -----------------------------------------------------------------
enum class Phase : uint8_t {
  Idle,      // app open, workout not started
  Ready,     // started, no set yet
  Lifting,   // a set is in progress (tentative or confirmed)
  Resting,   // between sets: rest timer running
  Paused,    // workout paused by the user (or after a long idle stretch)
};

// Haptic cues the view should play (bit mask from takeCues()).
enum Cue : uint16_t {
  CueRep      = 1 << 0,    // light tick per rep (if enabled)
  CueSetStart = 1 << 1,    // set confirmed (the first two reps at once)
  CueSetDone  = 1 << 2,    // the distinct "set finished" pattern
  CueRestGoal = 1 << 3,    // rest target reached
  CueRestLong = 1 << 4,    // rest is running long
  CueStart    = 1 << 5,
  CuePause    = 1 << 6,
};

static const uint32_t kSummaryMs = 5000;            // set-done card on screen
static const uint32_t kAutoPauseMs = 10UL * 60000;  // idle this long => pause
static const uint32_t kRestLongMinS = 180;          // nudge after max(2x goal, 3 min)

class Session {
public:
  void init(const Settings &s, const DayLog &log, const History &hist);
  // Start a new day's log if the date moved on (call when opening the app,
  // and now and then while it's open). Ignored while the clock isn't valid.
  void setToday(const Date &d);

  // User actions.
  void start(uint32_t ms);
  void pause(uint32_t ms, bool automatic = false);
  void resume(uint32_t ms);
  void touch(uint32_t ms) { lastActivityMs_ = ms; }   // any interaction
  void dismissSummary() { summaryOn_ = false; }

  // Detector output and the passage of time.
  void onEvent(const Event &e, uint32_t ms, const Date &now);
  void tick(uint32_t ms);

  // Settings edits (mark them dirty for saving).
  Settings &settings() { settingsDirty_ = true; return settings_; }
  const Settings &settingsRO() const { return settings_; }
  void forgetToday();              // clear today's log (settings page)

  // State for the UI.
  Phase phase() const { return phase_; }
  bool autoPaused() const { return autoPaused_; }
  uint8_t reps() const { return reps_; }
  bool tentative() const { return tentative_; }
  Exercise exercise() const { return exercise_; }
  uint8_t confidence() const { return confidence_; }
  float tempoSec() const { return tempo_; }
  uint32_t restSec(uint32_t ms) const;
  bool restGoalReached() const { return restGoalHit_; }
  bool summaryVisible(uint32_t ms) const;
  float summaryAge(uint32_t ms) const;            // 0..1 over its lifetime
  const SetRecord &lastSet() const { return lastSet_; }
  uint16_t lastSetNo() const { return lastSetNo_; }
  uint16_t setsToday() const { return log_.count; }
  uint16_t repsToday() const { return log_.totalReps(); }
  uint16_t currentSetNo() const { return (uint16_t)(log_.count + 1); }
  Exercise lastExercise() const { return lastExercise_; }
  bool blocksSleep() const;
  const DayLog &log() const { return log_; }
  const History &history() const { return hist_; }

  // Cues and dirty flags (cleared when read).
  uint16_t takeCues() { uint16_t c = cues_; cues_ = 0; return c; }
  bool takeLogDirty() { bool d = logDirty_; logDirty_ = false; return d; }
  bool takeHistDirty() { bool d = histDirty_; histDirty_ = false; return d; }
  bool takeSettingsDirty() { bool d = settingsDirty_; settingsDirty_ = false; return d; }

private:
  Settings settings_;
  DayLog log_;
  History hist_;
  Phase phase_ = Phase::Idle;
  bool autoPaused_ = false;
  uint8_t reps_ = 0;
  bool tentative_ = false;
  Exercise exercise_ = Exercise::None;
  Exercise lastExercise_ = Exercise::None;
  uint8_t confidence_ = 0;
  float tempo_ = 0.f;
  uint32_t restStartMs_ = 0;
  bool haveRest_ = false;
  bool restGoalHit_ = false, restLongHit_ = false;
  uint32_t setStartMs_ = 0;
  uint16_t setStartMin_ = 0xFFFF;
  uint32_t restBeforeSetS_ = 0xFFFF;
  uint32_t summaryFromMs_ = 0;
  bool summaryOn_ = false;
  SetRecord lastSet_;
  uint16_t lastSetNo_ = 0;
  uint32_t lastActivityMs_ = 0;
  uint16_t cues_ = 0;
  bool logDirty_ = false, histDirty_ = false, settingsDirty_ = false;

  void logSet(const Event &e, uint32_t ms);
  void rollDay(uint16_t y, uint8_t m, uint8_t d);
};

}  // namespace reps
