// Rep Counter: workout session, log and persistence. See rep_session.h.
#include "rep_session.h"
#include <string.h>

namespace reps {

static inline bool before(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }

uint16_t DayLog::totalReps() const {
  uint16_t t = 0;
  for (int i = 0; i < count && i < kMaxSets; i++) t = (uint16_t)(t + sets[i].reps);
  return t;
}

void Settings::sanitize() {
  if (mode >= (uint8_t)kModeCount) mode = (uint8_t)Mode::Auto;
  if (wrist > 1) wrist = (uint8_t)Wrist::Left;
  if (setEndSec < 2 || setEndSec > 15) setEndSec = 4;
  repTick = repTick ? 1 : 0;
  if (restGoalSec > 600) restGoalSec = 90;
  if (handSign > 1 || handSign < -1) handSign = 0;
}

// ---- blobs -------------------------------------------------------------------
size_t packBlob(BlobKind kind, const void *payload, size_t size, uint8_t *out, size_t cap) {
  if (cap < kBlobHeader + size || size > 0xFFFF) return 0;
  uint32_t magic = (uint32_t)kind;
  uint16_t ver = kBlobVersion, sz = (uint16_t)size;
  memcpy(out, &magic, 4);
  memcpy(out + 4, &ver, 2);
  memcpy(out + 6, &sz, 2);
  memcpy(out + kBlobHeader, payload, size);
  return kBlobHeader + size;
}

bool unpackBlob(BlobKind kind, const uint8_t *in, size_t len, void *payload, size_t size) {
  if (!in || len != kBlobHeader + size) return false;
  uint32_t magic;
  uint16_t ver, sz;
  memcpy(&magic, in, 4);
  memcpy(&ver, in + 4, 2);
  memcpy(&sz, in + 6, 2);
  if (magic != (uint32_t)kind || ver != kBlobVersion || sz != size) return false;
  memcpy(payload, in + kBlobHeader, size);
  return true;
}

// ---- session -------------------------------------------------------------------
void Session::init(const Settings &s, const DayLog &log, const History &hist) {
  settings_ = s;
  settings_.sanitize();
  log_ = log;
  if (log_.count > kMaxSets) log_.count = 0;
  hist_ = hist;
  if (hist_.n > kHistDays) hist_.n = 0;
  phase_ = Phase::Idle;
  autoPaused_ = false;
  reps_ = 0;
  tentative_ = false;
  exercise_ = Exercise::None;
  lastExercise_ = log_.count ? (Exercise)log_.sets[log_.count - 1].exercise : Exercise::None;
  confidence_ = 0;
  tempo_ = 0.f;
  haveRest_ = false;
  restGoalHit_ = restLongHit_ = false;
  summaryOn_ = false;
  lastSetNo_ = 0;
  cues_ = 0;
  logDirty_ = histDirty_ = settingsDirty_ = false;
}

void Session::rollDay(uint16_t y, uint8_t m, uint8_t d) {
  if (log_.count > 0 && log_.year != 0) {
    // Move yesterday's totals into the week history (newest first).
    DaySummary sum;
    sum.year = log_.year;
    sum.month = log_.month;
    sum.day = log_.day;
    sum.sets = log_.count;
    sum.reps = log_.totalReps();
    int n = hist_.n < kHistDays ? hist_.n : kHistDays - 1;
    for (int i = n; i > 0; i--) hist_.days[i] = hist_.days[i - 1];
    hist_.days[0] = sum;
    if (hist_.n < kHistDays) hist_.n++;
    histDirty_ = true;
  }
  log_ = DayLog();
  log_.year = y;
  log_.month = m;
  log_.day = d;
  logDirty_ = true;
  lastSetNo_ = 0;
  lastExercise_ = Exercise::None;
}

void Session::setToday(const Date &d) {
  if (!d.valid || d.year < 2024) return;
  if (log_.year == 0) {                 // first ever run: just stamp the log
    log_.year = d.year;
    log_.month = d.month;
    log_.day = d.day;
    logDirty_ = true;
    return;
  }
  if (!d.sameDay(log_.year, log_.month, log_.day) && phase_ != Phase::Lifting)
    rollDay(d.year, d.month, d.day);
}

void Session::forgetToday() {
  uint16_t y = log_.year;
  uint8_t m = log_.month, d = log_.day;
  log_ = DayLog();
  log_.year = y;
  log_.month = m;
  log_.day = d;
  logDirty_ = true;
  lastSetNo_ = 0;
  haveRest_ = false;
  summaryOn_ = false;
  if (phase_ == Phase::Resting) phase_ = Phase::Ready;
}

void Session::start(uint32_t ms) {
  if (phase_ != Phase::Idle) return;
  phase_ = Phase::Ready;
  autoPaused_ = false;
  reps_ = 0;
  tentative_ = false;
  lastActivityMs_ = ms;
  cues_ |= CueStart;
}

void Session::pause(uint32_t ms, bool automatic) {
  if (phase_ == Phase::Idle || phase_ == Phase::Paused) return;
  // The view ends the detector's set first (Detector::endSetNow), so any set
  // in progress has already been logged by the time we get here.
  phase_ = Phase::Paused;
  autoPaused_ = automatic;
  reps_ = 0;
  tentative_ = false;
  lastActivityMs_ = ms;
  if (!automatic) cues_ |= CuePause;
}

void Session::resume(uint32_t ms) {
  if (phase_ != Phase::Paused) return;
  phase_ = haveRest_ ? Phase::Resting : Phase::Ready;
  autoPaused_ = false;
  lastActivityMs_ = ms;
  cues_ |= CueStart;
}

void Session::onEvent(const Event &e, uint32_t ms, const Date &now) {
  if (phase_ == Phase::Idle || phase_ == Phase::Paused) {
    // Not counting. A SetEnd can still arrive right after pausing (the view
    // ends the set first) - log it, but stay paused.
    if (e.type == EventType::SetEnd && e.reps >= 2) {
      logSet(e, ms);
      haveRest_ = true;
      restStartMs_ = ms;
      restGoalHit_ = restLongHit_ = false;
    }
    return;
  }
  lastActivityMs_ = ms;
  switch (e.type) {
    case EventType::RepTentative:
      if (phase_ != Phase::Lifting) {
        restBeforeSetS_ = haveRest_ ? (ms - restStartMs_) / 1000 : 0xFFFF;
        setStartMs_ = ms;
        setStartMin_ = now.valid ? (uint16_t)(now.hour * 60 + now.minute) : 0xFFFF;
      }
      phase_ = Phase::Lifting;
      reps_ = e.reps;
      tentative_ = true;
      exercise_ = e.exercise;
      confidence_ = e.confidence;
      tempo_ = e.tempoSec;
      summaryOn_ = false;
      break;
    case EventType::SetStart:
      if (phase_ != Phase::Lifting) {
        restBeforeSetS_ = haveRest_ ? (ms - restStartMs_) / 1000 : 0xFFFF;
        setStartMs_ = ms;
        setStartMin_ = now.valid ? (uint16_t)(now.hour * 60 + now.minute) : 0xFFFF;
      }
      phase_ = Phase::Lifting;
      reps_ = e.reps;
      tentative_ = false;
      exercise_ = e.exercise;
      confidence_ = e.confidence;
      tempo_ = e.tempoSec;
      if (settings_.repTick) cues_ |= CueSetStart;
      break;
    case EventType::Rep:
      reps_ = e.reps;
      exercise_ = e.exercise;
      confidence_ = e.confidence;
      tempo_ = e.tempoSec;
      if (settings_.repTick) cues_ |= CueRep;
      break;
    case EventType::SetEnd:
      logSet(e, ms);
      phase_ = Phase::Resting;
      haveRest_ = true;
      restStartMs_ = ms;
      restGoalHit_ = restLongHit_ = false;
      reps_ = 0;
      tentative_ = false;
      summaryOn_ = true;
      summaryFromMs_ = ms;
      cues_ |= CueSetDone;
      break;
    case EventType::SetDiscarded:
      phase_ = haveRest_ ? Phase::Resting : Phase::Ready;
      reps_ = 0;
      tentative_ = false;
      break;
    default:
      break;
  }
  (void)now;
}

void Session::logSet(const Event &e, uint32_t ms) {
  (void)ms;
  if (e.reps < 2) return;
  SetRecord r;
  r.exercise = (uint8_t)e.exercise;
  r.reps = e.reps;
  r.confidence = e.confidence;
  r.flags = (uint8_t)((settings_.mode != (uint8_t)Mode::Auto ? kSetManual : 0) |
                      (setStartMin_ == 0xFFFF ? kSetNoClock : 0));
  r.startMin = setStartMin_;
  float d = e.setSec + 0.5f;
  r.durationS = (uint16_t)(d < 0.f ? 0.f : (d > 65000.f ? 65000.f : d));
  r.restS = restBeforeSetS_ > 0xFFFE ? 0xFFFF : (uint16_t)restBeforeSetS_;
  float t = e.tempoSec * 100.f + 0.5f;
  r.tempoCs = (uint16_t)(t < 0.f ? 0.f : (t > 65000.f ? 65000.f : t));
  if (log_.count >= kMaxSets) {
    memmove(&log_.sets[0], &log_.sets[1], sizeof(SetRecord) * (kMaxSets - 1));
    log_.count = kMaxSets - 1;
  }
  log_.sets[log_.count++] = r;
  lastSet_ = r;
  lastSetNo_ = log_.count;
  lastExercise_ = e.exercise;
  logDirty_ = true;
}

void Session::tick(uint32_t ms) {
  if (summaryOn_ && !before(ms, summaryFromMs_ + kSummaryMs)) summaryOn_ = false;
  if (phase_ == Phase::Resting) {
    uint32_t rs = restSec(ms);
    uint16_t goal = settings_.restGoalSec;
    if (goal && !restGoalHit_ && rs >= goal) { restGoalHit_ = true; cues_ |= CueRestGoal; }
    uint32_t longS = goal ? (uint32_t)goal * 2 : kRestLongMinS;
    if (longS < kRestLongMinS) longS = kRestLongMinS;
    if (!restLongHit_ && rs >= longS) { restLongHit_ = true; cues_ |= CueRestLong; }
  }
  if ((phase_ == Phase::Ready || phase_ == Phase::Resting) &&
      !before(ms, lastActivityMs_ + kAutoPauseMs))
    pause(ms, true);
}

uint32_t Session::restSec(uint32_t ms) const {
  if (!haveRest_) return 0;
  return (ms - restStartMs_) / 1000;
}

bool Session::summaryVisible(uint32_t ms) const {
  return summaryOn_ && before(ms, summaryFromMs_ + kSummaryMs);
}

float Session::summaryAge(uint32_t ms) const {
  if (!summaryOn_) return 1.f;
  float a = (float)(ms - summaryFromMs_) / (float)kSummaryMs;
  return a < 0.f ? 0.f : (a > 1.f ? 1.f : a);
}

bool Session::blocksSleep() const {
  return phase_ == Phase::Ready || phase_ == Phase::Lifting || phase_ == Phase::Resting;
}

}  // namespace reps
