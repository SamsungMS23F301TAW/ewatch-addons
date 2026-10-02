// Rep Counter: turn irregular, timestamped samples into an even stream.
//
// Only used by the REPS_IMU_FIFO=0 fallback, where taskIO polls the
// accelerometer once per cycle (~45 Hz with jitter) instead of draining the
// 100 Hz FIFO. Linear interpolation onto a fixed grid; a hole longer than
// maxGapMs restarts the grid and reports a gap. Header-only, no Arduino.
#pragma once
#include <stdint.h>
#include <string.h>

namespace reps {

class Resampler {
public:
  explicit Resampler(float outHz = 50.f, uint32_t maxGapMs = 250)
      : stepMs_(1000.f / outHz), maxGapMs_(maxGapMs) {}
  void reset() { have_ = false; }
  // Feed one sample (t in ms, g units). Calls out(x, y, z, gap) for each grid
  // point up to t; `gap` is true on the first point after a restart.
  template <typename F>
  void push(uint32_t t, const float g[3], bool forceGap, F out) {
    if (forceGap || !have_ || (int32_t)(t - prevT_) < 0 || t - prevT_ > maxGapMs_) {
      have_ = true;
      prevT_ = t;
      next_ = (float)t;
      memcpy(prev_, g, sizeof prev_);
      pendingGap_ = true;
    }
    while (next_ <= (float)t + 1e-3f) {
      float span = (float)(t - prevT_);
      float a = span > 0.f ? (next_ - (float)prevT_) / span : 1.f;
      out(prev_[0] + (g[0] - prev_[0]) * a, prev_[1] + (g[1] - prev_[1]) * a,
          prev_[2] + (g[2] - prev_[2]) * a, pendingGap_);
      pendingGap_ = false;
      next_ += stepMs_;
    }
    prevT_ = t;
    memcpy(prev_, g, sizeof prev_);
  }
private:
  float stepMs_;
  uint32_t maxGapMs_;
  bool have_ = false, pendingGap_ = false;
  uint32_t prevT_ = 0;
  float next_ = 0.f;
  float prev_[3] = {0, 0, 0};
};

}  // namespace reps
