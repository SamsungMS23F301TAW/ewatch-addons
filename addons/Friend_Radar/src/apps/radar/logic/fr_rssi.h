// Per-device RSSI smoothing.
//
// Wrist-worn BLE RSSI is noisy: +/-5-10 dB of multipath and body-shadow jitter
// plus occasional deep fades. Two stages:
//   1. sliding median of the last 5 raw samples (kills single-packet spikes
//      and dropouts without lagging a genuine step by more than 2 samples),
//   2. a time-constant EMA on the median (alpha = dt / (tau + dt)), so the
//      smoothing is the same whether packets arrive at 10 Hz in the foreground
//      or in short bursts during a background window.
// A gap longer than gapResetMs restarts the filter from the next sample, so a
// friend who walks back into range is not dragged by stale history.
#pragma once
#include "fr_types.h"

namespace fr {

class RssiFilter {
public:
  static constexpr int kWindow = 5;

  explicit RssiFilter(float tauMs = 1000.f, uint32_t gapResetMs = 6000)
      : tauMs_(tauMs), gapResetMs_(gapResetMs) { reset(); }

  void reset();

  // Feed one raw sample (dBm). Values >= 0 or < -110 are ignored (127 is the
  // HCI "not available" marker). nowMs must be monotonic.
  void add(int rssi, uint32_t nowMs);

  bool     valid()   const { return valid_; }
  float    value()   const { return ema_; }       // filtered dBm
  int      lastRaw() const { return lastRaw_; }
  uint32_t lastMs()  const { return lastMs_; }
  uint16_t count()   const { return count_; }     // samples since reset (saturates)
  float    median()  const;                       // current window median

private:
  float    tauMs_;
  uint32_t gapResetMs_;
  int8_t   win_[kWindow];
  uint8_t  n_    = 0;
  uint8_t  head_ = 0;
  bool     valid_ = false;
  float    ema_  = 0.f;
  int      lastRaw_ = 0;
  uint32_t lastMs_  = 0;
  uint16_t count_   = 0;
};

}  // namespace fr
