// Wrist step detector — pure C++, no Arduino dependencies (host-tested in
// test/test_steps). Feed it raw accelerometer samples at a fixed rate.
//
// Pipeline (per sample):
//   1. |a| in g from the raw MMA8451 counts (+-2 g range, 4096 counts/g).
//   2. Gravity removal: subtract a slow one-pole baseline (~0.5 Hz).
//   3. 2nd-order Butterworth low-pass at 3 Hz (walking and running cadences
//      sit at 1-3.5 Hz; typing tremor and road vibration sit above).
//   4. Half-wave peak picking: each positive excursion of the band-passed
//      signal (between zero crossings with a little hysteresis) yields at
//      most one candidate, timed at its maximum, and only if that maximum
//      clears an adaptive threshold (a fraction of recent step amplitudes,
//      floored at a minimum).
//   5. Interval gate: 0.25 s .. 2.0 s between steps.
//   6. Regularity gate: the first kStartRun peaks of a bout are held back;
//      only when that many arrive with consistent intervals are their steps
//      credited at once (and every step after that, while the rhythm
//      holds). Single jolts, arm flicks, typing and most car bumps never
//      build a regular run, so they never count.
//   7. Stride awareness: at the wrist, arm swing often swamps every other
//      footfall, leaving one strong peak per stride. An interval of about
//      twice the cadence counts as two steps, and a run that locked onto
//      strides re-bases when the hidden footfalls reappear.
#pragma once
#include <stdint.h>

namespace gf {

class StepDetector {
 public:
  static const int kStartRun = 5;

  explicit StepDetector(float sampleHz = 50.0f);
  void reset();                 // forget everything (new bout, new day)
  void gap();                   // samples were lost: restart the filters
  // Feeds one sample of raw counts; returns the steps credited by it.
  uint32_t push(int16_t x, int16_t y, int16_t z);

  uint32_t total() const { return total_; }
  bool     walking() const { return walking_; }
  float    sampleHz() const { return fs_; }
  // Diagnostics for the STEPS serial command.
  float    threshold() const;
  float    lastPeak() const { return lastPeakAmp_; }
  float    filtered() const { return y1_; }      // latest band-passed |a|, g
  float    cadenceHz() const;
  uint32_t peaks() const { return peaks_; }     // raw peaks seen (diagnostics)

 private:
  void onPeak(uint32_t t, float amp, uint32_t &credited);

  float fs_;
  // Filters.
  float base_ = 1.0f;            // gravity baseline (g)
  float baseA_;
  float b0_, b1_, b2_, a1_, a2_; // low-pass biquad
  float x1_ = 0, x2_ = 0, y1_ = 0, y2_ = 0;
  uint32_t settle_ = 0;          // samples to ignore after a reset/gap

  // Half-wave state machine.
  bool     positive_ = false;
  float    waveMax_ = 0.0f, waveMin_ = 0.0f;
  uint32_t waveMaxT_ = 0;
  float    ampAvg_ = 0.0f;       // recent peak-to-valley amplitude (g)

  // Rhythm.
  uint32_t n_ = 0;               // sample counter
  uint32_t lastPeakT_ = 0;
  float    stepAmp_ = 0.0f;      // amplitude of the last accepted step
  float    runAmp_ = 0.0f;       // typical step amplitude in this bout
  bool     havePeak_ = false;
  int      run_ = 0;             // peaks in the current candidate run
  int      pending_ = 0;         // steps those peaks represent, not yet credited
  float    avgInterval_ = 0.0f;  // samples
  int      misses_ = 0;
  bool     walking_ = false;
  float    lastPeakAmp_ = 0.0f;
  uint32_t total_ = 0;
  uint32_t peaks_ = 0;
};

}  // namespace gf
