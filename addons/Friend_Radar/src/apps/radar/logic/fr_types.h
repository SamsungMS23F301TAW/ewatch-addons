// Friend Radar — shared types for the Arduino-free radar core.
//
// Everything under src/apps/radar/logic/ is plain C++17 with no Arduino,
// FreeRTOS or ESP-IDF dependencies, so it compiles on the host for the Unity
// tests (`pio test -e native`) and the PNG preview tool. Time is always passed
// in explicitly: `nowMs` is a monotonic millisecond clock (millis() on the
// watch), `nowSec` / `epoch` is RTC wall-clock seconds since 2000-01-01.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace fr {

// Distance zones, closest first. Lost means "not heard recently".
enum class Zone : uint8_t {
  RightHere = 0,
  Near      = 1,
  Around    = 2,
  Far       = 3,
  Lost      = 4,
};
constexpr int kZoneCount = 5;

const char *zoneName(Zone z);         // "Right here", "Near", ...
const char *zoneShort(Zone z);        // "here", "near", ...

// Events carried in the beacon (see docs/PROTOCOL.md).
enum class EventKind : uint8_t {
  None      = 0,
  Wave      = 1,   // "I just noticed you nearby" (targeted) -> shared hello animation
  Shake     = 2,   // "my wearer just shook their wrist" (untargeted)
  Celebrate = 3,   // "we both shook while right here" (targeted) -> celebration
};

// Shared animations both watches play in sync.
enum class AnimKind : uint8_t { None = 0, Hello = 1, Celebrate = 2 };

constexpr size_t kMaxName  = 12;   // display name / nickname, printable ASCII
constexpr int    kMaxPeers = 16;   // live EWatches tracked at once
constexpr int    kMaxMates = 12;   // saved mates

// 32-bit finaliser (MurmurHash3 fmix32, public domain). Used for decorative
// blip angles and animation seeds; must be identical on every watch.
static inline uint32_t mix32(uint32_t h) {
  h ^= h >> 16; h *= 0x85ebca6bu;
  h ^= h >> 13; h *= 0xc2b2ae35u;
  h ^= h >> 16;
  return h;
}

// Order-independent seed for a pair of watches: A and B compute the same value.
static inline uint32_t pairSeed(uint32_t a, uint32_t b, uint32_t salt) {
  uint32_t lo = a < b ? a : b, hi = a < b ? b : a;
  return mix32(lo ^ mix32(hi + 0x9e3779b9u) ^ mix32(salt * 0x27d4eb2fu + 1u));
}

// 16-bit tag used to target an event at a watch (a hash of the 32-bit id, so
// structured ids do not collide). Never 0: 0 means "untargeted" on the air.
static inline uint16_t targetTag(uint32_t id) {
  uint16_t t = (uint16_t)(mix32(id ^ 0x74616721u) >> 16);
  return t ? t : 1;
}

// Stable decorative angle for a watch on the radar dial, in degrees [0, 360)
// (0 = up, clockwise). RSSI carries no direction, so this is deliberately
// just a hash of the id. The sector within kBlipKeepOutHalfDeg of straight
// down is never used: the zone names are engraved there.
constexpr uint16_t kBlipKeepOutHalfDeg = 42;
static inline uint16_t blipAngleDeg(uint32_t id) {
  const uint32_t span = 360u - 2u * kBlipKeepOutHalfDeg;
  return (uint16_t)((180u + kBlipKeepOutHalfDeg + mix32(id ^ 0x5bd1e995u) % span) % 360u);
}

}  // namespace fr
