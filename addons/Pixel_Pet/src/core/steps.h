// Step counting — shared design from ADDON_GUIDE.md ("Background step
// counting"). Two halves live behind this header:
//
//   1. PURE CORE (steps.cpp) — no Arduino, no FreeRTOS. Compiles on the host
//      and is unit-tested in test/test_steps:
//        * steps::Detector  — magnitude -> gravity removal -> ~3 Hz low-pass
//                             -> adaptive-threshold peaks -> 0.25..2.0 s
//                             interval gate -> regularity gate (counts a walk
//                             only after N consistent steps, then counts those
//                             and every step that follows).
//        * steps::StepBook  — today's count, lifetime count and a 30-day
//                             history with local-midnight rollover.
//
//   2. ON-WATCH SERVICE (steps_service.cpp) — the stepsSvc*() functions.
//      Owns the one Detector + StepBook, a mutex, the RTC-memory mirror that
//      survives deep sleep and the NVS checkpoints that survive power-off.
//      Fed with raw MMA8451 FIFO samples by whichever task currently owns
//      the I2C bus (taskIO while the screen is on, the background-sleep loop
//      while it is off).
#pragma once
#include <stdint.h>

namespace steps {

// ---------------------------------------------------------------------------
// Detector
// ---------------------------------------------------------------------------
struct DetectorConfig {
  float   sampleHz      = 12.5f;  // fixed accelerometer ODR (FIFO-paced)
  float   lowPassHz     = 3.0f;   // 2nd-order Butterworth on the de-gravitied magnitude
  float   baselineSec   = 1.2f;   // EMA time constant of the gravity/baseline estimate
  float   rmsSec        = 2.5f;   // EMA time constant of the signal RMS
  float   minPeakG      = 0.075f; // floor a peak must clear to help CONFIRM a walk (g)
  float   countFloorG   = 0.045f; // lower floor once a walk is confirmed (hysteresis)
  float   rmsFactor     = 0.55f;  // adaptive floor: peak must clear rmsFactor * RMS
  float   minIntervalS  = 0.25f;  // fastest plausible step (240 spm)
  float   maxIntervalS  = 2.0f;   // slowest plausible step; longer = walk ended
  uint8_t gateSteps     = 5;      // regular steps needed before anything counts
  float   gateTolerance = 0.25f;  // |dt - mean| / mean allowed while gating
  float   gateMaxPeriodS = 1.0f;  // a walk is only confirmed at >= 60 steps/min
  float   gateHeightRatio = 3.0f; // max/min peak height allowed within the chain
  float   countLo       = 0.55f;  // while counting accept dt in [lo, hi] * mean
  float   countHi       = 1.60f;
  float   missedHi      = 2.45f;  // dt in (countHi, missedHi] * mean = one step
                                  // whose peak was too weak: count 2, keep walking
  float   recentSec     = 6.0f;   // the low floor lingers this long after a walk
                                  // ends, so stop-and-go walking re-confirms
};

class Detector {
public:
  explicit Detector(const DetectorConfig &cfg = DetectorConfig());

  void reset();                     // forget everything (e.g. after deep sleep)
  void gap();                       // samples were lost: break the current chain

  // Feed one accelerometer sample, in g. Returns how many steps became
  // countable because of it (0 normally, 1 per step while walking, or the
  // whole pending chain when a walk is first confirmed).
  uint8_t push(float xg, float yg, float zg);

  uint32_t total()      const { return total_; }
  bool     walking()    const { return state_ == State::Counting; }
  float    cadenceSpm() const;      // 0 when not walking
  uint32_t samples()    const { return n_; }

  // Diagnostics (serial console / tests).
  float lastFiltered() const { return y1_; }
  float threshold()    const { return thresh_; }
  float rms()          const;

  const DetectorConfig &config() const { return cfg_; }

private:
  enum class State : uint8_t { Search, Counting };

  void    onPeak(float t, float height, uint8_t &newSteps);
  void    startChain(float t, float h);
  float   chainMean() const;

  DetectorConfig cfg_;
  // filter coefficients
  float aBase_, aRms_;
  float b0_, b1_, b2_, a1_, a2_;
  // filter state
  bool     primed_;
  float    base_;
  float    x1_, x2_, yv1_, yv2_;   // biquad history (input/output)
  float    ms_;                    // mean square of the filtered signal
  float    y0_, y1_, y2_;          // last three filtered samples (y0 newest)
  float    thresh_;
  bool     armed_;
  uint32_t n_;                     // samples seen since reset
  uint32_t lastArmN_;
  // step logic
  State    state_;
  float    lastCandT_;             // time of the last accepted candidate (s), <0 = none
  uint8_t  chainLen_;              // candidates in the pending chain
  float    chainIntervals_[8];     // intervals of the pending chain
  uint8_t  chainIntervalN_;
  float    chainMinH_, chainMaxH_;  // peak-height spread of the pending chain
  float    meanDt_;                // running step period while counting
  float    walkEndT_;              // time the last walk ended (s), <0 = never
  uint32_t total_;
};

// ---------------------------------------------------------------------------
// Day book
// ---------------------------------------------------------------------------
struct DayRecord {
  uint32_t day;     // local day number (days since 2000-01-01); 0 = empty slot
  uint32_t steps;
};

struct StepBook {
  static constexpr int kHistory = 30;
  static constexpr uint32_t kMagic = 0x53544B31;   // "STK1"

  uint32_t  magic;
  uint32_t  day;        // day `today` belongs to; 0 = clock not valid yet
  uint32_t  today;
  uint32_t  lifetime;   // every step ever counted on this watch
  DayRecord hist[kHistory];   // previous days, most recent first

  void init();
  bool valid() const { return magic == kMagic; }

  // Move to `nowDay` (0 = unknown clock: no-op). Closes the current day into
  // the history when the day changes. Returns true if a rollover happened.
  bool rollTo(uint32_t nowDay);

  // Count `n` new steps that happened on `nowDay`.
  void add(uint32_t n, uint32_t nowDay);

  // Steps recorded for `d` (today or from history; 0 if unknown).
  uint32_t stepsOn(uint32_t d) const;
};

// Local day number from an RTC epoch (seconds since 2000-01-01 local time).
static inline uint32_t dayOf(uint32_t epoch) { return epoch / 86400u; }

}  // namespace steps

// ---------------------------------------------------------------------------
// On-watch service (steps_service.cpp). Thread-safe; cheap to call.
// ---------------------------------------------------------------------------
struct StepsSnapshot {
  uint32_t today;
  uint32_t lifetime;
  uint32_t day;
  bool     walking;      // a confirmed walk is in progress right now
  float    cadenceSpm;
  uint32_t lastStepMs;   // millis() of the most recent counted step (0 = none)
};

void          stepsSvcInit(bool trustRtcMirror);  // load RTC mirror (if trusted and newer) or NVS
// Feed raw 14-bit MMA8451 samples (4096 counts/g). `overflow` = the FIFO
// reported lost samples before these. `nowDay` = current local day (0 if the
// clock is invalid). Returns steps added.
uint32_t      stepsSvcFeed(const int16_t *xyz, int count, bool overflow, uint32_t nowDay);
void          stepsSvcRollTo(uint32_t nowDay);    // midnight check without samples
StepsSnapshot stepsSvcSnapshot();
uint32_t      stepsSvcOnDay(uint32_t day);        // history lookup
void          stepsSvcResetDetector();            // after a sampling gap (deep sleep)
// Persist to NVS if enough changed (or `force`). Call from the render task only.
void          stepsSvcCheckpoint(bool force);
// Debug: inject steps as if walked now (serial console).
void          stepsSvcInject(uint32_t n, uint32_t nowDay);

// Accelerometer recorder for tuning the detector on real walks (serial
// console `rec <seconds>` then `dump`): keeps raw FIFO samples in PSRAM along
// with how many steps the detector counted meanwhile.
bool          stepsSvcRecStart(uint32_t seconds);
void          stepsSvcRecStop();
void          stepsSvcRecStatus(uint32_t &samples, uint32_t &cap, uint32_t &stepsCounted, bool &on);
uint32_t      stepsSvcRecCopy(uint32_t from, int16_t *out, uint32_t maxSamples);
