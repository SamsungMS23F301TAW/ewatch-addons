// Dayprint art core: deterministic integer maths shared by every generator.
//
// DETERMINISM RULES (read before touching anything under src/art/):
//   * No float or double anywhere in the art path. Geometry is fixed point:
//     positions Q8 (1/256 px), unit vectors and sin/cos Q14, fractions Q16.
//   * Fixed-width types only. `long` is 32 bits on the ESP32 and 64 on the
//     host, so it never appears here.
//   * Never draw two random numbers inside one expression or one argument
//     list: C++ leaves that evaluation order unspecified, and gcc and clang
//     really do differ. Draw into named locals, one statement each.
//   * No std::sort or other library algorithms whose tie order may differ
//     between libstdc++ and libc++.
//   * Products that can exceed 31 bits go through int64_t.
// The output of ALGO_VERSION 1 is frozen by golden hashes in test/test_art.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "gf_sintab.h"

namespace gf {

static const int16_t kW = 240;     // canvas width  (watch display)
static const int16_t kH = 280;     // canvas height

// ---------------------------------------------------------------- utilities
static inline int32_t imin(int32_t a, int32_t b) { return a < b ? a : b; }
static inline int32_t imax(int32_t a, int32_t b) { return a > b ? a : b; }
static inline int32_t iclamp(int32_t v, int32_t lo, int32_t hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}
static inline int32_t iabs(int32_t v) { return v < 0 ? -v : v; }

// Floor division by a power of two that is well defined for negatives.
static inline int32_t floorShift(int32_t v, int s) {
  return (v >= 0) ? (v >> s) : -(int32_t)(((uint32_t)(-v) + (1u << s) - 1) >> s);
}

// floor(sqrt(x)) for 32- and 64-bit inputs (bit-by-bit, exact).
static inline uint32_t isqrt32(uint32_t x) {
  uint32_t r = 0, b = 1u << 30;
  while (b > x) b >>= 2;
  while (b) {
    if (x >= r + b) { x -= r + b; r = (r >> 1) + b; }
    else            { r >>= 1; }
    b >>= 2;
  }
  return r;
}
static inline uint32_t isqrt64(uint64_t x) {
  if (x < 0xFFFFFFFFull) return isqrt32((uint32_t)x);
  uint64_t r = 0, b = 1ull << 62;
  while (b > x) b >>= 2;
  while (b) {
    if (x >= r + b) { x -= r + b; r = (r >> 1) + b; }
    else            { r >>= 1; }
    b >>= 2;
  }
  return (uint32_t)r;
}

// Angles are uint16: 65536 = one full turn. Screen convention: 0 points
// right (+x) and angles grow clockwise because +y points down.
static inline int32_t isin(uint16_t a) {
  uint32_t i = a >> 6, f = a & 63;
  int32_t s0 = kSinTab[i], s1 = kSinTab[i + 1];
  return s0 + (((s1 - s0) * (int32_t)f) >> 6);         // Q14
}
static inline int32_t icos(uint16_t a) { return isin((uint16_t)(a + 16384)); }

// Signed shortest difference a - b between two angles, in [-32768, 32767].
static inline int32_t angDiff(uint16_t a, uint16_t b) {
  int32_t d = (int32_t)(uint16_t)(a - b);
  return d >= 32768 ? d - 65536 : d;
}

// Degrees to the uint16 angle convention (exact for integer degrees).
static inline uint16_t deg(int32_t d) {
  int32_t m = d % 360; if (m < 0) m += 360;
  return (uint16_t)((m * 65536) / 360);
}

// Q16 smoothstep / quintic fade on t in [0, 65536].
static inline int32_t fadeQ16(int32_t t) {
  int64_t x = t;
  int64_t x3 = (x * x >> 16) * x >> 16;                 // t^3
  int64_t inner = ((x * (6 * x - 15 * 65536)) >> 16) + 10 * 65536;   // t(6t-15)+10
  return (int32_t)((x3 * inner) >> 16);
}
static inline int32_t smoothQ16(int32_t t) {          // 3t^2 - 2t^3
  if (t <= 0) return 0;
  if (t >= 65536) return 65536;
  int64_t x = t;
  int64_t x2 = x * x >> 16;
  return (int32_t)((x2 * (3 * 65536 - 2 * x)) >> 16);
}

// ---------------------------------------------------------------- hashing
static inline uint32_t mix32(uint32_t x) {            // "lowbias32"
  x ^= x >> 16; x *= 0x7feb352dU;
  x ^= x >> 15; x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}
static inline uint32_t hash2(int32_t x, int32_t y, uint32_t seed) {
  uint32_t h = (uint32_t)x * 0x9E3779B1U + (uint32_t)y * 0x85EBCA77U + seed;
  return mix32(h ^ (seed >> 7));
}
static inline uint32_t hash1(int32_t x, uint32_t seed) {
  return mix32((uint32_t)x * 0x9E3779B1U + seed);
}

// ---------------------------------------------------------------- PRNG
// SplitMix64 expands a seed; PCG32 (XSH-RR) is the workhorse generator.
static inline uint64_t splitmix64(uint64_t &s) {
  uint64_t z = (s += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

struct Rng {
  uint64_t state = 0, inc = 1;

  void seed(uint64_t seedVal, uint64_t stream) {
    uint64_t s = seedVal ^ (stream * 0xD1B54A32D192ED03ULL);
    uint64_t a = splitmix64(s);
    uint64_t b = splitmix64(s);
    state = 0;
    inc = (b << 1) | 1u;
    next();
    state += a;
    next();
  }
  uint32_t next() {
    uint64_t old = state;
    state = old * 6364136223846793005ULL + inc;
    uint32_t xs = (uint32_t)(((old >> 18) ^ old) >> 27);
    uint32_t rot = (uint32_t)(old >> 59);
    return (xs >> rot) | (xs << ((32 - rot) & 31));
  }
  // Uniform in [0, n) via multiply-shift (tiny bias, fully deterministic).
  uint32_t below(uint32_t n) {
    return (uint32_t)(((uint64_t)next() * n) >> 32);
  }
  // Uniform integer in [lo, hi] inclusive.
  int32_t range(int32_t lo, int32_t hi) {
    if (hi <= lo) return lo;
    return lo + (int32_t)below((uint32_t)(hi - lo + 1));
  }
  bool chance(uint32_t permille) { return below(1000) < permille; }
  uint16_t angle() { return (uint16_t)(next() >> 16); }
  // Roughly bell-shaped integer in [-span, span] (sum of two uniforms).
  int32_t tri(int32_t span) {
    int32_t a = range(0, span);
    int32_t b = range(0, span);
    return a - b;
  }
};

// Fowler–Noll–Vo, used for test hashes and the SNAP/ARTHASH commands.
static inline uint32_t fnv1a(const void *data, size_t n, uint32_t h = 2166136261u) {
  const uint8_t *p = (const uint8_t *)data;
  for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
  return h;
}

}  // namespace gf
