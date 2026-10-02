// IMU sample stream (added for Rep Counter). See imu_stream.h.
#include "imu_stream.h"

static ImuSample gRing[IMU_RING_SIZE];
static std::atomic<uint32_t> gHead{0};        // samples ever pushed
static std::atomic<uint8_t> gWant{0};         // bit mask of ImuClient
static bool gPendingGap = true;               // taskIO only
static uint32_t gSeq = 0;                     // taskIO only

void imuStreamWant(ImuClient who, bool on) {
  if (on) gWant.fetch_or((uint8_t)who);
  else    gWant.fetch_and((uint8_t)~who);
}

bool imuStreamWanted() { return gWant.load() != 0; }

void imuStreamMarkGap() { gPendingGap = true; }

void imuStreamPush(int16_t x, int16_t y, int16_t z, uint32_t tMs, uint8_t flags) {
  uint32_t h = gHead.load(std::memory_order_relaxed);
  ImuSample &s = gRing[h % IMU_RING_SIZE];
  s.seq = gSeq++;
  s.tMs = tMs;
  s.x = x;
  s.y = y;
  s.z = z;
  s.flags = (uint8_t)(flags | (gPendingGap ? IMU_FLAG_GAP : 0));
  s.pad = 0;
  gPendingGap = false;
  gHead.store(h + 1, std::memory_order_release);
}

uint32_t imuStreamHead() { return gHead.load(std::memory_order_acquire); }

bool imuStreamRead(uint32_t &cursor, ImuSample &out, uint32_t &lost) {
  uint32_t h = gHead.load(std::memory_order_acquire);
  if (cursor == h) return false;
  if (h - cursor > IMU_RING_SIZE - 16) {
    // Fell behind: skip to fresh data, leaving a margin so the producer
    // can't lap us while we copy.
    uint32_t to = h - (IMU_RING_SIZE / 2);
    lost += to - cursor;
    cursor = to;
  }
  out = gRing[cursor % IMU_RING_SIZE];
  // If the producer lapped this slot during the copy, the data is torn.
  uint32_t h2 = gHead.load(std::memory_order_acquire);
  if (h2 - cursor >= IMU_RING_SIZE) {
    lost += 1;
    cursor++;
    return false;
  }
  cursor++;
  return true;
}

float imuCountsPerG(uint8_t flags) { return (flags & IMU_FLAG_4G) ? 2048.f : 4096.f; }
