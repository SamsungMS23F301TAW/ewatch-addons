// Rep Counter: the rep / set detector.
//
// Feed it every accelerometer sample at a fixed rate (100 Hz on the watch,
// from the MMA8451 FIFO). It runs two counting channels side by side:
//
//   Rotation channel: the angle between the current gravity direction (in the
//     watch frame) and a "home" direction captured at the start of each rep.
//     A curl swings the forearm through 100-150°, a raise through ~70-100°.
//   Linear channel: |acceleration| minus its slow baseline is the vertical
//     linear acceleration (independent of how the watch sits on the wrist).
//     It is integrated to a displacement-like signal in metres. A press or a
//     row moves the hand 25-50 cm while the forearm hardly rotates.
//
// Each channel turns its signal into "humps": leave home, reach a peak, come
// back. A rep is counted as soon as 60 % of the swing has been undone, so the
// tick arrives on the way down instead of at the start of the next rep. Reps
// are then filtered by an arbiter:
//
//   * duration 0.5-5 s, minimum swing, and no locomotion (walking/running
//     produces sharp heel-strike energy that a lift never has);
//   * the first rep of a set is tentative and only becomes real when a
//     second, similar rep follows within a few seconds (rejects one-off
//     motions such as checking the watch or picking up a dumbbell);
//   * later reps must look like the set's earlier reps;
//   * a set ends when no rep has finished for `setEndSec` (default 4 s) and
//     nothing is in flight.
//
// In Auto mode the exercise label comes from the first reps' features: which
// channel fired, the swing size, whether the back of the wrist faces up at the
// top (raise vs curl) and whether the hand is above or below the elbow (press
// vs row; needs the wrist setting). The label never changes the count, only
// the channel does, so a mislabel is cosmetic.
//
// Pure C++, no allocation, no Arduino: unit-tested on the host (test/) and
// used by the CSV replay tool (tools/replay).
#pragma once
#include <stdint.h>
#include "rep_types.h"
#include "rep_dsp.h"

namespace reps {

// Every threshold in one place so recordings can be replayed with different
// values (tools/replay accepts --set name=value for each of these).
struct Tuning {
  // Pre-filter on each axis (Hz). Reps live below ~2 Hz; tremor and strap
  // buzz above ~5 Hz.
  float lpfHz          = 4.0f;
  float rotLpfHz       = 1.5f;   // extra smoothing for the gravity direction

  // Rotation channel (degrees).
  float rotArmDeg      = 10.f;   // leaving home by this much may start a rep
  float rotDepartDeg   = 35.f;   // ... and it's a real movement past this
  float rotMinRepDeg   = 50.f;   // smallest peak swing that can be a rep
  float homeTauSec     = 0.8f;   // home direction follows slow drift

  // Linear channel (metres, seconds).
  float linArmM        = 0.03f;
  float linDepartM     = 0.07f;
  float linMinRepM     = 0.15f;
  float baseTauSec     = 8.0f;   // baseline of |a| creep while not rotating
  float velTauSec      = 4.0f;   // leak of the velocity integrator
  float velHpSec       = 2.0f;   // high-pass on velocity (removes drift)
  float steadyG        = 0.006f; // |a| std over 0.25 s below this = steady
  float settleSec      = 2.0f;   // after a posture change, steady => re-learn
  float zuptSec        = 0.f;    // steady this long => velocity zeroed (0 = off)
  float rotGateDps     = 75.f;   // linear channel frozen while rotating faster
  float rotGateDeg     = 25.f;   // ...and having turned this far in ~0.5 s
  float linMaxRotDeg   = 45.f;   // a press/row rep can't rotate more than this

  // Shared rep shape and timing.
  float returnFrac     = 0.6f;   // fraction of the swing undone => rep counted
  float minRepSec      = 0.4f;   // start -> counted point
  float maxRepSec      = 8.0f;   // (includes any pause at the top)
  float holdRebaseSec  = 10.0f;  // stuck "away" this long => posture change

  // Set logic.
  float confirmGapSec  = 4.0f;   // rep 2 must start this soon after rep 1
  float endPeriodK     = 1.2f;   // slow sets: end after max(setEnd, k*period)
  float ampRatioMin    = 0.55f;  // rep 2 vs rep 1 swing
  float ampRatioMax    = 1.8f;
  float durRatioMin    = 0.4f;
  float durRatioMax    = 2.5f;
  float setAmpMin      = 0.40f;  // later reps vs the set's median swing
  float setAmpMax      = 2.5f;

  // Locomotion / impact gate.
  float impactHz       = 8.0f;   // impacts = |a| content above this
  float impactG        = 0.12f;  // spike in that band that counts as a footfall
  float hfGateG        = 0.10f;  // sustained RMS in that band (running)
  float hfHoldSec      = 1.5f;

  // Plausibility: what a real lift looks like. Manual modes apply the same
  // checks (they only pin the label and the counting channel).
  float curlMinElev    = 0.9f;   // curl: forearm tilt change (gravity along X)
  float curlHomeElev   = 0.5f;   // curl home: forearm hanging (or upright)
  float raiseMinElev   = 0.6f;   // raise: forearm goes from hanging to ~level
  float raiseMinDeg    = 55.f;
  float raiseHomeElev  = 0.8f;   // raise starts with the forearm hanging
  float raiseMinSec    = 1.0f;
  float maxPath        = 1.45f;  // wiggly paths aren't reps
  float maxHoldFrac    = 0.5f;   // mostly parked at the top: drinking, looking
  float parkDps        = 20.f;   // "parked" = turning slower than this
  float linMinRepSec   = 1.0f;   // press/row reps are never this quick
  float linMaxPath     = 1.3f;
  float manualMinElev  = 0.75f;  // manual Curl: smaller tilt change accepted

  // Classification.
  float raiseMaxDeg    = 112.f;  // raises swing less than curls...
  float raiseMinUp     = 0.55f;  // ...and finish with the back of the wrist up
  float handUpG        = 0.45f;  // press if the hand is this far "up" (in g)
};

// Look up a Tuning field by name (replay tool / bench: --set name=value).
// Returns nullptr for an unknown name. tuningName(i) iterates the table.
float *tuningField(Tuning &t, const char *name);
const char *tuningName(int i);

struct Config {
  float  fs          = 100.f;    // sample rate, Hz
  float  countsPerG  = 2048.f;   // raw units (MMA8451 at ±4 g, 14-bit)
  Mode   mode        = Mode::Auto;
  Wrist  wrist       = Wrist::Left;
  float  setEndSec   = 4.f;
  // Sign of the watch's +X axis relative to "towards the hand" when worn on
  // the LEFT wrist (+1: X points at the fingers). Only the press/row label
  // depends on it. See README "hardware test checklist".
  int8_t handAxisSignLeft = +1;
  // Learned hand direction along X (+1/-1, 0 = not learned yet). When set it
  // overrides wrist x handAxisSignLeft. The detector learns it from curl and
  // raise sets (they start with the hand hanging below the elbow); the app
  // persists it and passes it back here.
  int8_t learnedHandSign = 0;
  Tuning tune;
};

enum class EventType : uint8_t {
  None = 0,
  RepTentative,   // first rep of a possible set (not yet confirmed)
  SetStart,       // second consistent rep: the set is real (reps = 2)
  Rep,            // another rep in a confirmed set (reps = new count)
  SetEnd,         // set finished (reps = final count)
  SetDiscarded,   // the tentative rep expired without a partner
};

struct Event {
  EventType type = EventType::None;
  uint32_t  tMs = 0;           // detector clock (samples * 1000 / fs)
  uint8_t   reps = 0;          // count in the current set after this event
  Exercise  exercise = Exercise::None;
  uint8_t   confidence = 0;    // 0..3
  float     repSec = 0.f;      // Rep*/SetStart: this rep, start -> counted
  float     tempoSec = 0.f;    // seconds per rep so far (start-to-start)
  float     setSec = 0.f;      // SetEnd: first rep start -> last rep counted
  uint32_t  setStartMs = 0;    // SetStart/Rep/SetEnd: when the set's first rep began
};

// Snapshot for the UI (cheap to copy every frame).
struct Live {
  float    progress = 0.f;     // 0..~1: how far through the current swing
  bool     moving = false;     // a channel is mid-swing
  uint8_t  setState = 0;       // 0 idle, 1 tentative, 2 active set
  uint8_t  reps = 0;
  Exercise exercise = Exercise::None;
  uint8_t  confidence = 0;
  float    tempoSec = 0.f;
  bool     gated = false;      // locomotion gate currently active
  float    rotDeg = 0.f;       // debug: rotation-channel excursion
  float    linM = 0.f;         // debug: linear-channel excursion
};

// Internal signals for the replay tool and debugging plots.
struct Debug {
  float theta = 0.f;     // rotation channel: angle from home, deg
  uint8_t rotPh = 0;     // 0 home, 1 rising, 2 falling
  float linExc = 0.f;    // linear channel: excursion from home, m
  uint8_t linPh = 0;
  float vel = 0.f;       // linear channel velocity estimate, m/s
  float accV = 0.f;      // vertical linear acceleration estimate, g
  float rate = 0.f;      // smoothed angular rate, deg/s
  float hfRms = 0.f;     // impact energy, g
  bool gated = false;
  bool frozen = false;   // linear channel frozen by rotation
};

// One candidate rep produced by a channel (internal, exposed for tests).
struct Candidate {
  Channel  ch = Channel::None;
  uint32_t nStart = 0, nPeak = 0, nEnd = 0;   // sample indices
  float    amp = 0.f;          // degrees (rotation) or metres (linear)
  float    upAtPeak = 0.f;     // rotation: back-of-wrist-up component at peak
  float    rotDeg = 0.f;       // linear: max orientation change during rep
  float    handG = 0.f;        // gravity along the hand direction (+ = hand up)
  V3       axis;               // rotation: unit axis of the swing (home x peak)
  int8_t   sign = 0;           // linear: +1 the rep went up first, -1 down
  float    elevHome = 0.f;     // rotation: gravity along the forearm (watch X)
  float    elevPeak = 0.f;     //   at home and at the peak; |difference| is
                               //   how much the forearm's tilt changed
  float    path = 0.f;         // distance travelled / (1.6 x amp); ~1 = smooth
  float    holdFrac = 0.f;     // share of the rep spent near the peak
};

class Detector {
public:
  Detector() { reset(Config()); }
  void reset(const Config &cfg);
  const Config &config() const { return cfg_; }

  // Live settings changes. A mode change while a set is running ends it.
  void setMode(Mode m);
  void setWrist(Wrist w) { cfg_.wrist = w; }
  // Restore (from storage) or forget (0) the learned hand direction.
  void setLearnedHandSign(int8_t s) {
    cfg_.learnedHandSign = s > 0 ? 1 : (s < 0 ? -1 : 0);
    handVotes_ = cfg_.learnedHandSign ? 2 : 0;
  }
  void setSetEndSec(float s) { cfg_.setEndSec = s; }

  // Feed one sample: raw counts (uses cfg.countsPerG) or g units.
  void push(int16_t x, int16_t y, int16_t z) {
    float k = 1.f / cfg_.countsPerG;
    pushG(x * k, y * k, z * k);
  }
  void pushG(float gx, float gy, float gz);

  // The stream had a hole (FIFO overflow, pause/resume). In-flight reps are
  // dropped and filters restart from the next sample.
  void markGap();

  // Ignore the impact gate for a while (our own haptic buzz shakes the
  // accelerometer). Duration in ms, from now.
  void suppressGate(uint32_t ms);

  // Force the current set to end now (e.g. the user paused).
  void endSetNow();

  bool pollEvent(Event &out);
  const Live &live() const { return live_; }
  // Current learned hand direction (+1/-1, 0 unknown) and its evidence.
  int8_t learnedHandSign() const { return cfg_.learnedHandSign; }
  const Debug &debug() const { return dbg_; }
  uint32_t nowMs() const { return toMs(n_); }
  uint32_t samples() const { return n_; }

  // Last rejection reason (debug / replay tool); static string.
  const char *lastReject() const { return lastReject_; }
  // Called for every candidate a channel produces (debug / tests).
  typedef void (*CandidateHook)(const Candidate &, bool accepted,
                                const char *why, void *ctx);
  void setCandidateHook(CandidateHook h, void *ctx) { hook_ = h; hookCtx_ = ctx; }

private:
  // ---- rotation channel ----
  struct Rot {
    enum Phase : uint8_t { Home, Rising, Falling } ph = Home;
    V3 h, uPeak, uMin;
    float theta = 0.f, thMax = 0.f, thMin = 0.f;
    uint32_t nQuiet = 0, nStart = 0, nPeak = 0, nMin = 0;
    bool departed = false;
    float path = 0.f;            // angle travelled since the rep started
    uint32_t nearPeak = 0;       // samples parked near the peak
    float holdRef = 0.f;
    V3 uPrev;
  } rot_;
  // ---- linear channel ----
  struct Lin {
    enum Phase : uint8_t { Home, Rising, Falling } ph = Home;
    float v = 0.f, vRaw = 0.f, vMean = 0.f;
    float d = 0.f, dHome = 0.f, dPeak = 0.f, dMin = 0.f;
    int8_t sign = 0;             // +1 away = up, -1 away = down
    float exc = 0.f, excMax = 0.f;
    uint32_t nQuiet = 0, nStart = 0, nPeak = 0, nMin = 0;
    uint32_t quietCount = 0;
    uint32_t rotHoldUntil = 0;
    uint32_t settleUntil = 0;
    bool settleArmed = false;
    bool departed = false;
    V3 uStart;
    float rotMax = 0.f;
    float path = 0.f;            // |displacement| travelled since the start
    uint32_t nearPeak = 0;
    float holdRef = 0.f;
  } lin_;

  // ---- set / arbiter ----
  struct Rec {
    Channel ch = Channel::None;
    Exercise ex = Exercise::None;
    uint32_t nStart = 0, nEnd = 0;
    float amp = 0.f, dur = 0.f, margin = 0.f;
    V3 axis;
    int8_t sign = 0;
    float elevHome = 0.f;
  };
  enum SetState : uint8_t { Idle = 0, Tentative = 1, Active = 2 };
  SetState setState_ = Idle;
  Rec tent_;                       // tentative first rep
  Rec near_;                       // last near-miss (see handleCandidate)
  bool nearValid_ = false;
  Channel setCh_ = Channel::None;
  uint8_t reps_ = 0;
  uint32_t setFirstStart_ = 0, setLastEnd_ = 0, setLastStart_ = 0;
  static const int kHist = 16;
  float amps_[kHist];
  float periods_[kHist];
  int nAmps_ = 0, nPeriods_ = 0;
  uint8_t votes_[kExerciseCount];
  float marginSum_ = 0.f;
  V3 setAxis_;
  int8_t setSign_ = 0;
  float setElevHome_ = 0.f;        // first rotation rep's home elevation
  int8_t handVotes_ = 0;           // evidence for the hand direction

  // ---- preprocessing ----
  Config cfg_;
  float dt_ = 0.01f;
  uint32_t n_ = 0;
  bool primed_ = false;
  Biquad lp_[3];
  Biquad rlp_[3];                  // rotation channel's extra low-pass
  Biquad hfHp_[2];                 // 4th-order high-pass of |a| (impact band)
  OnePole hfE_;                    // HF energy
  OnePole base_;                   // linear-channel baseline of |a|
  OnePole gSlow_[3];               // slow gravity estimate (classification)
  OnePole rate_;                   // smoothed angular rate, deg/s
  V3 u_;                           // gravity direction, rotation channel
  V3 uFast_;                       // gravity direction, less smoothing
  static const int kUHist = 21;    // 0.2 s at 100 Hz
  V3 uHist_[kUHist];
  int uHead_ = 0;
  static const int kTurnHist = 11; // 0.5 s, one entry per 5 samples
  V3 turnHist_[kTurnHist];
  int turnHead_ = 0, turnDiv_ = 0;
  float turn_ = 0.f;
  float rateNow_ = 0.f;            // smoothed angular rate, deg/s
  uint32_t gateUntil_ = 0;
  uint32_t suppressUntil_ = 0;
  uint32_t impacts_[4];            // recent footfall-like spikes (ring)
  uint32_t nImpacts_ = 0;
  static const int kWin = 25;      // 0.25 s steadiness window
  float win_[kWin];
  int winHead_ = 0;
  float winSum_ = 0.f, winSum2_ = 0.f;
  uint32_t steadyN_ = 0;
  float homeK_ = 0.f, velLeak_ = 1.f, fastBaseK_ = 1.f, velHpK_ = 0.f, settleK_ = 1.f;

  // ---- output ----
  static const int kQ = 8;
  Event q_[kQ];
  int qHead_ = 0, qCount_ = 0;
  Live live_;
  Debug dbg_;
  const char *lastReject_ = "";
  CandidateHook hook_ = nullptr;
  void *hookCtx_ = nullptr;

  uint32_t toMs(uint32_t n) const { return (uint32_t)((double)n * 1000.0 / cfg_.fs); }
  uint32_t secToN(float s) const { return (uint32_t)(s * cfg_.fs + 0.5f); }
  void prime(const V3 &g);
  bool stepRot(Candidate &out);
  bool stepLin(float m, float meanM, bool steady, float rateDps, Candidate &out);
  void handleCandidate(const Candidate &c);
  bool gatedAt(uint32_t nFrom, uint32_t nTo) const;
  Exercise classify(const Candidate &c, float &margin, const char *&why) const;
  bool channelBusy(Channel ch) const;
  void emit(EventType t, float repSec);
  void startTentative(const Candidate &c, Exercise ex, float dur, float margin);
  void endSet(bool discarded);
  void rememberNear(const Candidate &c, Exercise ex, float dur);
  void flipPhase(const Candidate &c);
  float handSign() const;
  uint8_t confidence() const;
  Exercise votedExercise() const;
  float tempo() const;
  void reject(const Candidate &c, const char *why);
  void updateLive();
};

}  // namespace reps
