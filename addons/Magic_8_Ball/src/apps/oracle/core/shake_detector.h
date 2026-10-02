// Deliberate-shake detector. Portable C++, unit-tested on the host.
//
// Feed it timestamped accelerometer samples in g. It removes gravity with a
// time-constant-correct moving average, then looks for "lobes": stretches of
// strong dynamic acceleration (|a - g| >= peakG) in one direction. A lobe in
// roughly the opposite direction of the previous lobe is a reversal.
//
//   Started  when >= reversalsToStart reversals land inside windowMs, every
//            one following the previous within [minHalfMs, maxHalfMs] and in
//            rhythm with it (a shake is periodic).
//   Stopped  when no reversal has happened for quietMs.
//
// Why this rejects the usual false triggers:
//   * a single jolt rings out in well under 3 strong reversals, and ringing
//     faster than minHalfMs is ignored;
//   * walking and running swing the arm at 1-1.7 Hz, so half-swings take
//     longer than maxHalfMs (and are mostly below peakG anyway);
//   * a foot strike and its rebound alternate short and long gaps, which
//     breaks the rhythm test;
//   * tilting the wrist only moves gravity, which the average tracks;
//   * for warmupMs after the filter (re)starts, mid-motion priming errors
//     settle out before any lobe can count.
#pragma once
#include <stdint.h>

namespace oracle {

struct ShakeConfig {
  float    gravityTauS      = 0.30f;  // gravity-estimate time constant
  float    peakG            = 0.90f;  // dynamic magnitude that makes a lobe
  float    reverseCos       = -0.35f; // cos(angle) below this is "opposite"
  uint32_t minHalfMs        = 60;     // faster reversals are ringing
  uint32_t maxHalfMs        = 260;    // slower half-swings break the chain
  float    rhythmMin        = 0.55f;  // each half-swing must last 0.55x..1.8x
  float    rhythmMax        = 1.80f;  // the previous one, or the chain restarts
  uint32_t windowMs         = 1000;   // reversals must fit in this window
  uint8_t  reversalsToStart = 3;
  uint32_t quietMs          = 420;    // stillness that ends a shake
  uint32_t resetGapMs       = 250;    // sample gap that restarts the filter
  uint32_t warmupMs         = 600;    // after a (re)start: settle, don't detect
  float    warmupTauS       = 0.10f;  // faster gravity filter while settling
};

class ShakeDetector {
 public:
  enum class Event : uint8_t { None, Started, Stopped };

  explicit ShakeDetector(const ShakeConfig &cfg = ShakeConfig()) : cfg_(cfg) { reset(); }

  void  reset();
  // One sample: timestamp in ms (monotonic, wraps safely) and acceleration
  // in g. Returns the edge event this sample caused, if any.
  Event feed(uint32_t ms, float ax, float ay, float az);
  // Call when no samples arrive (e.g. the IMU stalled) so a shake still ends.
  Event idle(uint32_t ms);

  bool  shaking() const { return shaking_; }
  float intensity() const { return intensity_; }    // 0..1, smoothed
  float dynX() const { return d_[0]; }               // gravity removed, g
  float dynY() const { return d_[1]; }
  float dynZ() const { return d_[2]; }
  float gravX() const { return g_[0]; }              // gravity estimate, g
  float gravY() const { return g_[1]; }
  float gravZ() const { return g_[2]; }
  uint32_t reversals() const { return totalReversals_; }
  const ShakeConfig &config() const { return cfg_; }

 private:
  static constexpr int kHist = 8;

  ShakeConfig cfg_;
  bool     primed_ = false;
  uint32_t lastMs_ = 0;
  uint32_t primedMs_ = 0;   // when the filter (re)started
  float    g_[3] = {0, 0, 0};
  float    d_[3] = {0, 0, 0};

  bool     lobeActive_ = false;
  float    lobeDir_[3] = {0, 0, 0};
  float    lobePeak_ = 0;
  uint32_t lobeStartMs_ = 0;
  uint32_t lastStrongMs_ = 0;

  uint32_t chain_[kHist] = {0};   // timestamps of chained reversals
  int      chainLen_ = 0;
  uint32_t lastHalfMs_ = 0;       // half-swing that led to the last reversal
  uint32_t lastReversalMs_ = 0;
  uint32_t totalReversals_ = 0;

  bool     shaking_ = false;
  float    intensity_ = 0;

  void pushChain(uint32_t ms);
  int  chainInWindow(uint32_t ms) const;
};

}  // namespace oracle
