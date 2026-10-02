// Pure step detector + day book. No Arduino / FreeRTOS includes on purpose:
// this file is compiled into the host unit tests (pio test -e native).
#include "steps.h"
#include <math.h>
#include <string.h>

namespace steps {

// ---------------------------------------------------------------------------
// Detector
// ---------------------------------------------------------------------------
//
// Pipeline per sample (all in g):
//   m  = |a|                         orientation-free; gravity is ~1 g of it
//   b += aBase (m - b)               slow EMA tracks gravity + DC drift
//   d  = m - b                       dynamic part
//   y  = biquad_lowpass(d)           2nd-order Butterworth, ~3 Hz
//   rms = sqrt(EMA(y^2))             recent signal energy
//   thresh = max(minPeakG, rmsFactor * rms)
//
// A peak is a local maximum of y above `thresh` while "armed"; the detector
// re-arms once y falls back below the baseline (0). Peak times are refined
// with a 3-point parabola so the interval gate is not quantised to the
// 80 ms sample period at 12.5 Hz.
//
// Candidates feed a two-state machine:
//   Search   — build a chain of candidates whose intervals are in
//              [minInterval, maxInterval] and within gateTolerance of the
//              chain's mean. When the chain reaches gateSteps candidates the
//              whole chain is counted and we switch to Counting. Irregular
//              input (typing, gesturing, single jolts, most car bumps) never
//              forms such a chain, so it never counts.
//   Counting — every candidate whose interval is within [countLo, countHi]
//              of the running period counts as one step. Candidates that come
//              too early are ignored (double peaks inside one step); a gap
//              longer than countHi * period or maxInterval ends the walk.

Detector::Detector(const DetectorConfig &cfg) : cfg_(cfg) {
  const float fs = cfg_.sampleHz > 1.0f ? cfg_.sampleHz : 1.0f;
  aBase_ = 1.0f / (cfg_.baselineSec * fs);
  if (aBase_ > 1.0f) aBase_ = 1.0f;
  aRms_ = 1.0f / (cfg_.rmsSec * fs);
  if (aRms_ > 1.0f) aRms_ = 1.0f;

  // Butterworth low-pass, bilinear transform with pre-warping.
  float fc = cfg_.lowPassHz;
  if (fc > 0.45f * fs) fc = 0.45f * fs;
  const float k    = tanf(3.14159265f * fc / fs);
  const float k2   = k * k;
  const float sq2  = 1.41421356f;
  const float norm = 1.0f / (1.0f + sq2 * k + k2);
  b0_ = k2 * norm;
  b1_ = 2.0f * b0_;
  b2_ = b0_;
  a1_ = 2.0f * (k2 - 1.0f) * norm;
  a2_ = (1.0f - sq2 * k + k2) * norm;

  reset();
}

void Detector::reset() {
  primed_ = false;
  base_ = 1.0f;
  x1_ = x2_ = yv1_ = yv2_ = 0.0f;
  ms_ = 0.0f;
  y0_ = y1_ = y2_ = 0.0f;
  thresh_ = cfg_.minPeakG;
  armed_ = true;
  n_ = 0;
  lastArmN_ = 0;
  state_ = State::Search;
  lastCandT_ = -1.0f;
  chainLen_ = 0;
  chainIntervalN_ = 0;
  chainMinH_ = chainMaxH_ = 0.0f;
  meanDt_ = 0.0f;
  walkEndT_ = -1.0f;
  total_ = 0;
}

void Detector::gap() {
  // Lost samples: the time axis is no longer continuous, so any interval
  // measured across the gap would be meaningless. Keep the filters (they
  // re-settle within a second) but drop the chain and the walk state.
  state_ = State::Search;
  lastCandT_ = -1.0f;
  chainLen_ = 0;
  chainIntervalN_ = 0;
}

float Detector::rms() const { return sqrtf(ms_ > 0.0f ? ms_ : 0.0f); }

float Detector::cadenceSpm() const {
  if (state_ != State::Counting || meanDt_ <= 0.0f) return 0.0f;
  return 60.0f / meanDt_;
}

float Detector::chainMean() const {
  if (chainIntervalN_ == 0) return 0.0f;
  float s = 0.0f;
  for (uint8_t i = 0; i < chainIntervalN_; i++) s += chainIntervals_[i];
  return s / (float)chainIntervalN_;
}

void Detector::startChain(float t, float h) {
  chainLen_ = 1;
  chainIntervalN_ = 0;
  chainMinH_ = chainMaxH_ = h;
  lastCandT_ = t;
}

uint8_t Detector::push(float xg, float yg, float zg) {
  const float fs = cfg_.sampleHz;
  const float m = sqrtf(xg * xg + yg * yg + zg * zg);

  if (!primed_) {
    // Start the baseline at the first reading so the filters don't ring
    // through a 1 g step on boot.
    base_ = m;
    primed_ = true;
  }
  base_ += aBase_ * (m - base_);
  const float d = m - base_;

  // Direct-form-I biquad.
  const float y = b0_ * d + b1_ * x1_ + b2_ * x2_ - a1_ * yv1_ - a2_ * yv2_;
  x2_ = x1_; x1_ = d;
  yv2_ = yv1_; yv1_ = y;

  ms_ += aRms_ * (y * y - ms_);
  // Start conservatively, continue permissively: confirming a walk needs
  // peaks above minPeakG, but once walking, gentler steps (arm swing fading,
  // carrying a bag) keep counting down to countFloorG.
  const float tNow = (float)n_ / fs;            // time of this sample
  const bool lowFloor = state_ == State::Counting ||
                        (walkEndT_ >= 0.0f && tNow - walkEndT_ < cfg_.recentSec);
  const float floorG = lowFloor ? cfg_.countFloorG : cfg_.minPeakG;
  float adaptive = cfg_.rmsFactor * rms();
  thresh_ = adaptive > floorG ? adaptive : floorG;

  y2_ = y1_; y1_ = y0_; y0_ = y;
  n_++;

  uint8_t newSteps = 0;

  // Re-arm once the signal returns below the baseline. If it never does
  // (baseline lag after a posture change) re-arm after maxInterval anyway.
  if (!armed_) {
    if (y < 0.0f) {
      armed_ = true;
    } else if ((float)(n_ - lastArmN_) / fs > cfg_.maxIntervalS) {
      armed_ = true;
    }
  }

  // Local maximum at the middle sample (y1_). Needs three samples.
  if (armed_ && n_ >= 3 && y1_ > thresh_ && y1_ >= y2_ && y1_ > y0_) {
    // Parabolic refinement of the peak position, in samples relative to y1_.
    const float denom = y2_ - 2.0f * y1_ + y0_;
    float delta = 0.0f;
    if (denom < -1e-6f) {
      delta = 0.5f * (y2_ - y0_) / denom;
      if (delta > 0.5f) delta = 0.5f;
      if (delta < -0.5f) delta = -0.5f;
    }
    const float tPeak = ((float)(n_ - 2) + delta) / fs;   // y1_ is sample n_-2 (0-based)
    armed_ = false;
    lastArmN_ = n_;
    onPeak(tPeak, y1_, newSteps);
  }

  // A walk ends when no step arrives within the allowed window (one missed
  // peak is tolerated, see onPeak).
  if (state_ == State::Counting && lastCandT_ >= 0.0f) {
    const float now = (float)(n_ - 1) / fs;
    float limit = cfg_.missedHi * meanDt_;
    if (limit > cfg_.maxIntervalS) limit = cfg_.maxIntervalS;
    if (now - lastCandT_ > limit + 0.1f) {
      state_ = State::Search;
      walkEndT_ = now;
      chainLen_ = 0;
      chainIntervalN_ = 0;
      lastCandT_ = -1.0f;
    }
  }

  total_ += newSteps;
  return newSteps;
}

void Detector::onPeak(float t, float height, uint8_t &newSteps) {
  if (lastCandT_ < 0.0f) {          // first candidate of a new chain
    startChain(t, height);
    return;
  }
  const float dt = t - lastCandT_;
  if (dt < cfg_.minIntervalS) {
    // Too soon after the previous candidate: a second peak inside the same
    // step (heel strike + arm swing). Keep the earlier one.
    return;
  }

  if (state_ == State::Counting) {
    if (dt < cfg_.countLo * meanDt_) {
      return;                        // extra wiggle inside a step
    }
    if (dt <= cfg_.countHi * meanDt_ && dt <= cfg_.maxIntervalS) {
      newSteps += 1;
      meanDt_ += 0.25f * (dt - meanDt_);
      lastCandT_ = t;
      return;
    }
    if (dt <= cfg_.missedHi * meanDt_ && dt <= cfg_.maxIntervalS) {
      // One step in between had a peak too weak to see. Count it, keep the
      // rhythm (don't fold the doubled interval into the period estimate).
      newSteps += 2;
      lastCandT_ = t;
      return;
    }
    // Pause or rhythm change: stop counting, this peak may start a new walk.
    state_ = State::Search;
    walkEndT_ = t;
    startChain(t, height);
    return;
  }

  // ---- Search: building a regular chain ----
  if (dt > cfg_.maxIntervalS) {
    startChain(t, height);
    return;
  }
  bool irregular = false;
  if (chainIntervalN_ > 0) {
    const float mean = chainMean();
    irregular = fabsf(dt - mean) > cfg_.gateTolerance * mean;
  }
  float minH = height < chainMinH_ ? height : chainMinH_;
  float maxH = height > chainMaxH_ ? height : chainMaxH_;
  if (!irregular && maxH > cfg_.gateHeightRatio * minH) irregular = true;
  if (irregular) {
    // Restart the chain from the previous candidate so this interval can
    // become the first interval of a new, regular chain.
    chainLen_ = 2;
    chainIntervalN_ = 1;
    chainIntervals_[0] = dt;
    chainMinH_ = chainMaxH_ = height;
    lastCandT_ = t;
    return;
  }
  chainMinH_ = minH;
  chainMaxH_ = maxH;
  const uint8_t cap = (uint8_t)(sizeof(chainIntervals_) / sizeof(chainIntervals_[0]));
  if (chainIntervalN_ < cap) chainIntervals_[chainIntervalN_++] = dt;
  chainLen_++;
  lastCandT_ = t;

  const uint8_t gate = cfg_.gateSteps < 2 ? 2 : (cfg_.gateSteps > cap ? cap : cfg_.gateSteps);
  if (chainLen_ >= gate) {
    if (chainMean() <= cfg_.gateMaxPeriodS) {
      // Walk confirmed: count the whole chain retroactively.
      newSteps += chainLen_;
      meanDt_ = chainMean();
      state_ = State::Counting;
      chainLen_ = 0;
      chainIntervalN_ = 0;
    } else {
      // Regular but too slow to be walking (swaying, slow gestures): slide
      // the window so the chain never grows without bound.
      memmove(&chainIntervals_[0], &chainIntervals_[1],
              sizeof(float) * (size_t)(chainIntervalN_ - 1));
      chainIntervalN_--;
      chainLen_--;
    }
  }
}

// ---------------------------------------------------------------------------
// StepBook
// ---------------------------------------------------------------------------
void StepBook::init() {
  memset(this, 0, sizeof(*this));
  magic = kMagic;
}

bool StepBook::rollTo(uint32_t nowDay) {
  if (nowDay == 0 || nowDay == day) return false;
  if (day == 0) {
    // Steps counted before the clock was valid are credited to the first
    // valid day we see.
    day = nowDay;
    return false;
  }
  if (today > 0) {
    // Merge into an existing record for the same day (clock moved back and
    // forth), otherwise push a new record at the front.
    bool merged = false;
    for (int i = 0; i < kHistory; i++) {
      if (hist[i].day == day) { hist[i].steps += today; merged = true; break; }
    }
    if (!merged) {
      memmove(&hist[1], &hist[0], sizeof(DayRecord) * (kHistory - 1));
      hist[0].day = day;
      hist[0].steps = today;
    }
  }
  day = nowDay;
  // If we have travelled back onto a day that already has a record (clock
  // set backwards), resume that day's count instead of starting from zero.
  today = 0;
  for (int i = 0; i < kHistory; i++) {
    if (hist[i].day == nowDay) {
      today = hist[i].steps;
      memmove(&hist[i], &hist[i + 1], sizeof(DayRecord) * (kHistory - 1 - i));
      hist[kHistory - 1].day = 0;
      hist[kHistory - 1].steps = 0;
      break;
    }
  }
  return true;
}

void StepBook::add(uint32_t n, uint32_t nowDay) {
  rollTo(nowDay);
  today += n;
  lifetime += n;
}

uint32_t StepBook::stepsOn(uint32_t d) const {
  if (d == 0) return 0;
  if (d == day) return today;
  for (int i = 0; i < kHistory; i++) {
    if (hist[i].day == d) return hist[i].steps;
  }
  return 0;
}

}  // namespace steps
