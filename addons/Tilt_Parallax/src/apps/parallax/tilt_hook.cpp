#include "tilt_hook.h"
#include <atomic>

// 64 samples is ~1.4 s of taskIO cycles: far more than a frame ever lags.
static tp::SpscRing<64> sRing;
static std::atomic<bool> sEnabled{ false };

void tiltHookPush(int16_t ax, int16_t ay, int16_t az, uint32_t tMs) {
  if (!sEnabled.load(std::memory_order_relaxed)) return;
  tp::ImuSample s;
  s.x = ax; s.y = ay; s.z = az; s.tMs = tMs;
  sRing.push(s);
}

void tiltHookEnable(bool on) { sEnabled.store(on, std::memory_order_relaxed); }
bool tiltHookPop(tp::ImuSample &out) { return sRing.pop(out); }
void tiltHookClear() { sRing.clear(); }
uint32_t tiltHookDropped() { return sRing.dropped(); }
