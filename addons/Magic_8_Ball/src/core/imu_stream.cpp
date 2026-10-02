#include "imu_stream.h"

#include <atomic>

namespace {

constexpr uint32_t kCap = 64;   // power of two
ImuSample             gRing[kCap];
std::atomic<uint32_t> gHead{0};      // written by the producer only
std::atomic<uint32_t> gTail{0};      // written by the consumer only
std::atomic<bool>     gEnabled{false};
std::atomic<uint32_t> gDropped{0};

}  // namespace

void imuStreamEnable(bool on) {
  if (on) {
    // Discard anything stale before the producer starts writing again.
    gTail.store(gHead.load(std::memory_order_acquire), std::memory_order_release);
    gDropped.store(0, std::memory_order_relaxed);
  }
  gEnabled.store(on, std::memory_order_release);
}

bool imuStreamEnabled() { return gEnabled.load(std::memory_order_acquire); }

void imuStreamPush(uint32_t ms, int16_t x, int16_t y, int16_t z) {
  if (!gEnabled.load(std::memory_order_acquire)) return;
  uint32_t h = gHead.load(std::memory_order_relaxed);
  uint32_t t = gTail.load(std::memory_order_acquire);
  if (h - t >= kCap) {   // full: drop the newest, the consumer is behind
    gDropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  ImuSample &s = gRing[h & (kCap - 1)];
  s.ms = ms;
  s.x = x;
  s.y = y;
  s.z = z;
  gHead.store(h + 1, std::memory_order_release);
}

bool imuStreamPop(ImuSample &out) {
  uint32_t t = gTail.load(std::memory_order_relaxed);
  uint32_t h = gHead.load(std::memory_order_acquire);
  if (t == h) return false;
  out = gRing[t & (kCap - 1)];
  gTail.store(t + 1, std::memory_order_release);
  return true;
}

uint32_t imuStreamDropped() { return gDropped.load(std::memory_order_relaxed); }
