// Step detector implementation. See steps.h for the pipeline.
#include "steps.h"
#include <math.h>

namespace gf {

namespace {
const float kMinPeakG     = 0.038f;   // threshold floor after filtering
const float kThreshFrac   = 0.12f;    // of the recent peak-to-valley amplitude
const float kZeroHystFrac = 0.03f;    // zero-crossing hysteresis
const float kZeroHystMinG = 0.008f;
const float kRegularity   = 0.38f;    // max relative interval deviation
const float kMinIntervalS = 0.25f;
const float kMaxIntervalS = 2.0f;
const float kMaxStepS     = 1.1f;     // slowest cadence a bout may qualify at
const float kCountsPerG   = 4096.0f;
}  // namespace

StepDetector::StepDetector(float sampleHz) : fs_(sampleHz) {
  const float pi = 3.14159265f;
  baseA_ = 1.0f - expf(-2.0f * pi * 0.5f / fs_);          // ~0.5 Hz baseline
  // RBJ biquad low-pass, Butterworth (Q = 1/sqrt 2), fc = 3 Hz.
  float w0 = 2.0f * pi * 3.0f / fs_;
  float cw = cosf(w0), alpha = sinf(w0) / (2.0f * 0.70710678f);
  float a0 = 1.0f + alpha;
  b0_ = (1.0f - cw) * 0.5f / a0;
  b1_ = (1.0f - cw) / a0;
  b2_ = b0_;
  a1_ = -2.0f * cw / a0;
  a2_ = (1.0f - alpha) / a0;
  reset();
}

void StepDetector::reset() {
  gap();
  ampAvg_ = 0.0f;
  havePeak_ = false;
  run_ = 0;
  pending_ = 0;
  avgInterval_ = 0.0f;
  misses_ = 0;
  walking_ = false;
  lastPeakAmp_ = 0.0f;
  total_ = 0;
}

void StepDetector::gap() {
  x1_ = x2_ = y1_ = y2_ = 0.0f;
  base_ = -1.0f;                         // re-seeded by the next sample
  settle_ = (uint32_t)(fs_ * 0.8f);      // let the filters settle
  positive_ = false;
  waveMax_ = 0.0f;
  waveMin_ = 0.0f;
  waveMaxT_ = 0;
  // A gap breaks the rhythm; an ongoing bout must re-qualify.
  havePeak_ = false;
  run_ = 0;
  pending_ = 0;
  walking_ = false;
  misses_ = 0;
}

float StepDetector::threshold() const {
  float t = kThreshFrac * ampAvg_;
  return t > kMinPeakG ? t : kMinPeakG;
}

float StepDetector::cadenceHz() const {
  return (walking_ && avgInterval_ > 0.0f) ? fs_ / avgInterval_ : 0.0f;
}

uint32_t StepDetector::push(int16_t x, int16_t y, int16_t z) {
  float fx = (float)x, fy = (float)y, fz = (float)z;
  float m = sqrtf(fx * fx + fy * fy + fz * fz) / kCountsPerG;
  if (base_ < 0.0f) base_ = m;
  base_ += (m - base_) * baseA_;
  float hp = m - base_;
  float v = b0_ * hp + b1_ * x1_ + b2_ * x2_ - a1_ * y1_ - a2_ * y2_;
  x2_ = x1_; x1_ = hp; y2_ = y1_; y1_ = v;
  n_++;
  ampAvg_ *= 0.9966f;                     // halves in ~4 s without peaks
  if (settle_ > 0) { settle_--; return 0; }

  uint32_t credited = 0;
  uint32_t maxIv = (uint32_t)(kMaxIntervalS * fs_);
  if (havePeak_ && (n_ - lastPeakT_) > maxIv) {
    // The rhythm stopped: end the bout.
    havePeak_ = false;
    walking_ = false;
    run_ = 0;
    pending_ = 0;
    misses_ = 0;
  }

  // Half-wave detection: a step candidate is one positive excursion of the
  // band-passed signal (between hysteresis zero crossings); its timing is the
  // excursion's maximum. An arm-swing hump riding on a footfall stays inside
  // the same excursion, so it can't become a second step.
  float hz = kZeroHystFrac * ampAvg_;
  if (hz < kZeroHystMinG) hz = kZeroHystMinG;
  if (positive_) {
    if (v > waveMax_) { waveMax_ = v; waveMaxT_ = n_; }
    if (v < -hz) {
      positive_ = false;
      if (waveMax_ > threshold()) onPeak(waveMaxT_, waveMax_ - waveMin_, credited);
      waveMin_ = v;
    }
  } else {
    if (v < waveMin_) waveMin_ = v;
    if (v > hz) {
      positive_ = true;
      waveMax_ = v;
      waveMaxT_ = n_;
    }
  }
  total_ += credited;
  return credited;
}

void StepDetector::onPeak(uint32_t t, float amp, uint32_t &credited) {
  peaks_++;
  lastPeakAmp_ = amp;
  ampAvg_ = (ampAvg_ <= 0.0f) ? amp : ampAvg_ * 0.75f + amp * 0.25f;
  if (!havePeak_) {
    havePeak_ = true;
    lastPeakT_ = t;
    stepAmp_ = amp;
    runAmp_ = amp;
    run_ = 1;
    pending_ = 1;
    return;
  }
  float iv = (float)(t - lastPeakT_);
  if (iv < kMinIntervalS * fs_) {
    // Two peaks inside one footfall: keep the stronger as the reference.
    if (amp > stepAmp_ * 1.25f) { lastPeakT_ = t; stepAmp_ = amp; }
    return;
  }
  // Inside an established bout, a clearly weaker peak well before the next
  // step is due is an arm-swing hump or an echo, not a footfall.
  if (walking_ && iv < 0.6f * avgInterval_ && amp < 0.7f * stepAmp_) return;
  float prevAmp = stepAmp_;
  lastPeakT_ = t;
  stepAmp_ = amp;
  if (run_ <= 1) {
    avgInterval_ = iv;
    runAmp_ = 0.5f * (prevAmp + amp);
    run_ = 2;
    pending_ = 2;
    return;
  }

  float ar = amp / (runAmp_ > 1e-4f ? runAmp_ : 1e-4f);
  bool ampOk = ar > 0.22f && ar < 4.5f;      // steps in a bout look roughly alike
  float r = iv / avgInterval_;
  // How many steps this interval spans: one, or two when the other foot's
  // peak was hidden (arm swing often swamps every other footfall at the
  // wrist, leaving one strong peak per stride).
  int mult = 0;
  if (fabsf(r - 1.0f) <= kRegularity) mult = 1;
  else if (fabsf(r - 2.0f) <= 2.0f * kRegularity * 0.6f && ar > 0.5f && ar < 2.0f) mult = 2;
  if (!walking_ && mult == 0 && fabsf(r - 0.5f) <= 0.5f * kRegularity * 0.6f && ampOk) {
    // The run had locked onto strides; this interval is a single step.
    // Re-base on the step interval: each earlier interval was two steps.
    pending_ = 1 + 2 * (pending_ - 1) + 1;
    avgInterval_ = iv;
    run_++;
    runAmp_ = runAmp_ * 0.75f + amp * 0.25f;
    return;
  }
  if (mult > 0 && ampOk) {
    float base = iv / (float)mult;
    avgInterval_ = avgInterval_ * 0.75f + base * 0.25f;
    runAmp_ = runAmp_ * 0.75f + amp * 0.25f;
    run_++;
    misses_ = 0;
    if (walking_) {
      credited += (uint32_t)mult;
    } else {
      pending_ += mult;
      // Qualify only at a plausible stepping cadence: slower rhythms are
      // strides (which re-base once the hidden footfalls show) or bumps.
      if (run_ >= kStartRun && avgInterval_ <= kMaxStepS * fs_) {
        walking_ = true;
        credited += (uint32_t)pending_;    // credit the whole qualifying run
        pending_ = 0;
      }
    }
    return;
  }
  if (walking_ && ++misses_ < 2) {
    // Tolerate a single odd step (a stumble, a turn) and keep the bout.
    credited += 1;
    return;
  }
  // Irregular: these two peaks become the start of a new candidate run.
  walking_ = false;
  misses_ = 0;
  run_ = 2;
  pending_ = 2;
  avgInterval_ = iv;
  runAmp_ = 0.5f * (prevAmp + amp);
}

}  // namespace gf
