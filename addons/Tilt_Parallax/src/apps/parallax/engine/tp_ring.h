// Tilt Parallax — lock-free single-producer / single-consumer ring.
//
// taskIO (core 0) pushes every accelerometer sample; the face (render task,
// core 1) drains them all each frame. No locks, no blocking: when the ring
// is full the newest sample is dropped and counted.
#pragma once
#include <stdint.h>
#include <atomic>

namespace tp {

struct ImuSample {
  int16_t  x, y, z;
  uint32_t tMs;
};

template <uint32_t N>
class SpscRing {
  static_assert((N & (N - 1)) == 0, "N must be a power of two");
public:
  // Producer side.
  bool push(const ImuSample &s) {
    uint32_t h = head_.load(std::memory_order_relaxed);
    uint32_t t = tail_.load(std::memory_order_acquire);
    if (h - t >= N) { dropped_.fetch_add(1, std::memory_order_relaxed); return false; }
    buf_[h & (N - 1)] = s;
    head_.store(h + 1, std::memory_order_release);
    return true;
  }
  // Consumer side.
  bool pop(ImuSample &out) {
    uint32_t t = tail_.load(std::memory_order_relaxed);
    uint32_t h = head_.load(std::memory_order_acquire);
    if (t == h) return false;
    out = buf_[t & (N - 1)];
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }
  // Consumer side: discard everything queued.
  void clear() { tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release); }
  uint32_t size() const {
    return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
  }
  uint32_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
  ImuSample buf_[N];
  std::atomic<uint32_t> head_{ 0 };
  std::atomic<uint32_t> tail_{ 0 };
  std::atomic<uint32_t> dropped_{ 0 };
};

}  // namespace tp
