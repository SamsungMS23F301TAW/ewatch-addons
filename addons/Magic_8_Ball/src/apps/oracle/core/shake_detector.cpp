// The Arduino build compiles everything at -Os; these per-pixel and
// per-sample loops run every frame, so they get -O2 on the watch.
#if defined(ESP_PLATFORM) && defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif
#include "shake_detector.h"

#include <math.h>

namespace oracle {

namespace {
inline uint32_t elapsed(uint32_t now, uint32_t then) {
  return (uint32_t)(now - then);   // wrap-safe for intervals < 49 days
}
}  // namespace

void ShakeDetector::reset() {
  primed_ = false;
  lastMs_ = 0;
  primedMs_ = 0;
  for (int i = 0; i < 3; i++) { g_[i] = 0; d_[i] = 0; lobeDir_[i] = 0; }
  lobeActive_ = false;
  lobePeak_ = 0;
  lobeStartMs_ = 0;
  lastStrongMs_ = 0;
  chainLen_ = 0;
  lastHalfMs_ = 0;
  lastReversalMs_ = 0;
  totalReversals_ = 0;
  shaking_ = false;
  intensity_ = 0;
}

void ShakeDetector::pushChain(uint32_t ms) {
  if (chainLen_ == kHist) {
    for (int i = 1; i < kHist; i++) chain_[i - 1] = chain_[i];
    chainLen_--;
  }
  chain_[chainLen_++] = ms;
}

int ShakeDetector::chainInWindow(uint32_t ms) const {
  int n = 0;
  for (int i = 0; i < chainLen_; i++)
    if (elapsed(ms, chain_[i]) <= cfg_.windowMs) n++;
  return n;
}

ShakeDetector::Event ShakeDetector::idle(uint32_t ms) {
  if (shaking_ && elapsed(ms, lastReversalMs_) > cfg_.quietMs) {
    shaking_ = false;
    chainLen_ = 0;
    lobeActive_ = false;
    return Event::Stopped;
  }
  return Event::None;
}

ShakeDetector::Event ShakeDetector::feed(uint32_t ms, float ax, float ay, float az) {
  const float a[3] = {ax, ay, az};
  if (!(ax == ax) || !(ay == ay) || !(az == az)) return idle(ms);   // NaN guard

  // ---- gravity estimate (first-order low-pass with a real time constant)
  float dtS = 0.f;
  if (!primed_ || (int32_t)(ms - lastMs_) < 0 || elapsed(ms, lastMs_) > cfg_.resetGapMs) {
    // First sample, clock went backwards, or a long gap: restart the filter
    // from this sample. Any half-built chain is stale now.
    for (int i = 0; i < 3; i++) { g_[i] = a[i]; d_[i] = 0; }
    primed_ = true;
    primedMs_ = ms;
    lastMs_ = ms;
    lobeActive_ = false;
    chainLen_ = 0;
    return idle(ms);
  }
  dtS = elapsed(ms, lastMs_) * 0.001f;
  lastMs_ = ms;
  bool warming = elapsed(ms, primedMs_) < cfg_.warmupMs;
  float alpha = 1.f - expf(-dtS / (warming ? cfg_.warmupTauS : cfg_.gravityTauS));
  for (int i = 0; i < 3; i++) {
    g_[i] += alpha * (a[i] - g_[i]);
    d_[i] = a[i] - g_[i];
  }
  float mag = sqrtf(d_[0] * d_[0] + d_[1] * d_[1] + d_[2] * d_[2]);

  // ---- intensity: smoothed, normalised dynamic magnitude while shaking
  {
    float target = shaking_ ? fminf(mag / 2.2f, 1.f) : 0.f;
    float k = 1.f - expf(-dtS / (shaking_ ? 0.20f : 0.35f));
    intensity_ += k * (target - intensity_);
    if (intensity_ < 0.001f) intensity_ = 0.f;
  }

  Event ev = Event::None;

  // ---- lobes and reversals
  if (warming) return idle(ms);
  if (mag >= cfg_.peakG) {
    float u[3] = {d_[0] / mag, d_[1] / mag, d_[2] / mag};
    bool stale = lobeActive_ && elapsed(ms, lastStrongMs_) > cfg_.maxHalfMs;
    if (!lobeActive_ || stale) {
      // A fresh lobe after quiet: it can only start a new chain.
      lobeActive_ = true;
      for (int i = 0; i < 3; i++) lobeDir_[i] = u[i];
      lobePeak_ = mag;
      lobeStartMs_ = ms;
      if (stale) chainLen_ = 0;
    } else {
      float c = u[0] * lobeDir_[0] + u[1] * lobeDir_[1] + u[2] * lobeDir_[2];
      uint32_t half = elapsed(ms, lobeStartMs_);
      if (c <= cfg_.reverseCos && half >= cfg_.minHalfMs) {
        // Direction reversal.
        totalReversals_++;
        lastReversalMs_ = ms;
        if (half > cfg_.maxHalfMs) {
          chainLen_ = 0;                            // too slow to chain
        } else if (chainLen_ > 0 && lastHalfMs_ > 0) {
          float ratio = (float)half / (float)lastHalfMs_;
          if (ratio < cfg_.rhythmMin || ratio > cfg_.rhythmMax) chainLen_ = 0;   // off-beat
        }
        lastHalfMs_ = half;
        pushChain(ms);
        for (int i = 0; i < 3; i++) lobeDir_[i] = u[i];
        lobePeak_ = mag;
        lobeStartMs_ = ms;
        if (!shaking_ && chainInWindow(ms) >= cfg_.reversalsToStart) {
          shaking_ = true;
          ev = Event::Started;
        }
      } else if (c > 0.f && mag > lobePeak_) {
        // Same lobe, stronger: follow its peak direction.
        for (int i = 0; i < 3; i++) lobeDir_[i] = u[i];
        lobePeak_ = mag;
      }
    }
    lastStrongMs_ = ms;
  }

  if (ev == Event::None) ev = idle(ms);
  return ev;
}

}  // namespace oracle
