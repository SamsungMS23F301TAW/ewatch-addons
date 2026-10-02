#include "fr_rssi.h"

namespace fr {

void RssiFilter::reset() {
  n_ = 0; head_ = 0; valid_ = false; ema_ = 0.f;
  lastRaw_ = 0; lastMs_ = 0; count_ = 0;
  for (int i = 0; i < kWindow; ++i) win_[i] = 0;
}

float RssiFilter::median() const {
  if (n_ == 0) return 0.f;
  int8_t s[kWindow];
  for (uint8_t i = 0; i < n_; ++i) s[i] = win_[i];
  for (uint8_t i = 1; i < n_; ++i) {             // insertion sort, n <= 5
    int8_t v = s[i];
    int j = (int)i - 1;
    while (j >= 0 && s[j] > v) { s[j + 1] = s[j]; --j; }
    s[j + 1] = v;
  }
  if (n_ & 1) return (float)s[n_ / 2];
  return 0.5f * ((float)s[n_ / 2 - 1] + (float)s[n_ / 2]);
}

void RssiFilter::add(int rssi, uint32_t nowMs) {
  if (rssi >= 0 || rssi < -110) return;
  if (valid_ && (uint32_t)(nowMs - lastMs_) > gapResetMs_) reset();

  win_[head_] = (int8_t)rssi;
  head_ = (uint8_t)((head_ + 1) % kWindow);
  if (n_ < kWindow) ++n_;
  float med = median();

  if (!valid_ || n_ < kWindow) {
    // Until the window is full, report the running median itself: seeding
    // the EMA from the first packet alone would let one outlier steer the
    // first few seconds.
    ema_ = med;
    valid_ = true;
  } else {
    float dt = (float)(uint32_t)(nowMs - lastMs_);
    float a = dt / (tauMs_ + dt);
    if (a < 0.05f) a = 0.05f;   // bursts with equal timestamps still converge
    ema_ += a * (med - ema_);
  }
  lastRaw_ = rssi;
  lastMs_  = nowMs;
  if (count_ < 0xFFFF) ++count_;
}

}  // namespace fr
