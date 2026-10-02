// Tilt Parallax — deterministic randomness for the procedural art.
//
// Everything is integer-hashed so a given seed draws the same landscape on the
// watch and in the host preview tool (floating-point rounding can differ in
// the last bit between the two compilers, which only ever moves an edge pixel).
#pragma once
#include <stdint.h>
#include <math.h>

namespace tp {

static inline uint32_t hash32(uint32_t x) {
  x ^= x >> 16; x *= 0x7FEB352Du;
  x ^= x >> 15; x *= 0x846CA68Bu;
  x ^= x >> 16;
  return x;
}
static inline uint32_t hash2(int32_t x, int32_t y, uint32_t seed) {
  return hash32((uint32_t)x * 0x9E3779B1u ^ hash32((uint32_t)y * 0x85EBCA77u ^ seed));
}
// Uniform float in [0,1) from a hash.
static inline float hashUnit(uint32_t h) { return (h >> 8) * (1.0f / 16777216.0f); }

// Small, fast PRNG (PCG-XSH-RR 32).
class Rng {
public:
  explicit Rng(uint32_t seed = 1) { reseed(seed); }
  void reseed(uint32_t seed) {
    state_ = 0; next(); state_ += 0x853C49E6748FEA9Bull ^ (uint64_t)seed * 0x9E3779B97F4A7C15ull; next();
  }
  uint32_t next() {
    uint64_t old = state_;
    state_ = old * 6364136223846793005ull + 1442695040888963407ull;
    uint32_t xs = (uint32_t)(((old >> 18u) ^ old) >> 27u);
    uint32_t rot = (uint32_t)(old >> 59u);
    return (xs >> rot) | (xs << ((32u - rot) & 31u));
  }
  float unit() { return (next() >> 8) * (1.0f / 16777216.0f); }        // [0,1)
  float range(float a, float b) { return a + (b - a) * unit(); }
  int   irange(int lo, int hiIncl) {                                      // [lo,hi]
    uint32_t span = (uint32_t)(hiIncl - lo + 1);
    return lo + (int)(next() % span);
  }
  bool  chance(float p) { return unit() < p; }
private:
  uint64_t state_ = 0;
};

// 1D value noise, smooth (quintic) interpolation, output in [-1,1].
float noise1(float x, uint32_t seed);
// 2D value noise, output in [-1,1].
float noise2(float x, float y, uint32_t seed);
// Fractal sums. `oct` octaves, each at double frequency and `gain` amplitude.
float fbm1(float x, uint32_t seed, int oct, float gain = 0.5f);
float fbm2(float x, float y, uint32_t seed, int oct, float gain = 0.5f);
// Ridged fBm (sharp crests), output roughly [0,1].
float ridged1(float x, uint32_t seed, int oct, float gain = 0.5f);

// Midpoint-displacement ridgeline. Fills out[0..n-1] (n >= 2) with heights in
// roughly [-1,1]. `rough` in (0,1): displacement scale multiplier per level
// (0.5 = classic fractal mountains, lower = smoother). Endpoints are random.
void midpointRidge(float *out, int n, Rng &rng, float rough, float amp = 1.0f);

}  // namespace tp
