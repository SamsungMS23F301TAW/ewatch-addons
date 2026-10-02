#include "gf_noise.h"

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

namespace gf {

// Eight gradient directions, Q16 (diagonals are 1/sqrt(2)).
static const int32_t kGx[8] = { 65536, -65536, 0, 0, 46341, -46341, 46341, -46341 };
static const int32_t kGy[8] = { 0, 0, 65536, -65536, 46341, 46341, -46341, -46341 };

static inline int32_t gdot(uint32_t h, int32_t dx, int32_t dy) {
  uint32_t i = h & 7;
  return (int32_t)(((int64_t)kGx[i] * dx + (int64_t)kGy[i] * dy) >> 16);
}

int32_t noise2(int32_t x, int32_t y, uint32_t seed) {
  int32_t xi = floorShift(x, 16), yi = floorShift(y, 16);
  int32_t xf = x - xi * 65536, yf = y - yi * 65536;
  int32_t n00 = gdot(hash2(xi, yi, seed), xf, yf);
  int32_t n10 = gdot(hash2(xi + 1, yi, seed), xf - 65536, yf);
  int32_t n01 = gdot(hash2(xi, yi + 1, seed), xf, yf - 65536);
  int32_t n11 = gdot(hash2(xi + 1, yi + 1, seed), xf - 65536, yf - 65536);
  int32_t u = fadeQ16(xf), v = fadeQ16(yf);
  int32_t a = n00 + (int32_t)(((int64_t)(n10 - n00) * u) >> 16);
  int32_t b = n01 + (int32_t)(((int64_t)(n11 - n01) * u) >> 16);
  int32_t n = a + (int32_t)(((int64_t)(b - a) * v) >> 16);
  return (int32_t)(((int64_t)n * 92682) >> 16);       // scale ~1/0.707 to +-1
}

int32_t fbm2(int32_t x, int32_t y, uint32_t seed, int32_t octaves) {
  int64_t sum = 0;
  int32_t amp = 65536, norm = 0;
  for (int32_t o = 0; o < octaves; o++) {
    uint32_t s = mix32(seed + (uint32_t)o * 0x632BE5ABU);
    // Per-octave offset breaks lattice alignment between layers.
    int32_t ox = (int32_t)(s & 0xFFFF) * 7, oy = (int32_t)(s >> 16) * 7;
    sum += (int64_t)noise2(x + ox, y + oy, s) * amp >> 16;
    norm += amp;
    amp >>= 1;
    x *= 2; y *= 2;
  }
  return (int32_t)(sum * 65536 / norm);
}

int32_t noise1(int32_t x, uint32_t seed) {
  int32_t xi = floorShift(x, 16);
  int32_t xf = x - xi * 65536;
  // Random slopes in [-1, 1] (Q16).
  int32_t g0 = (int32_t)(hash1(xi, seed) & 0x1FFFF) - 65536;
  int32_t g1 = (int32_t)(hash1(xi + 1, seed) & 0x1FFFF) - 65536;
  int32_t n0 = (int32_t)(((int64_t)g0 * xf) >> 16);
  int32_t n1 = (int32_t)(((int64_t)g1 * (xf - 65536)) >> 16);
  int32_t u = fadeQ16(xf);
  int32_t n = n0 + (int32_t)(((int64_t)(n1 - n0) * u) >> 16);
  return n * 2;
}

int32_t fbm1(int32_t x, uint32_t seed, int32_t octaves) {
  int64_t sum = 0;
  int32_t amp = 65536, norm = 0;
  for (int32_t o = 0; o < octaves; o++) {
    uint32_t s = mix32(seed + (uint32_t)o * 0x2C1B3C6DU);
    sum += (int64_t)noise1(x + (int32_t)(s & 0xFFFF), s) * amp >> 16;
    norm += amp;
    amp >>= 1;
    x *= 2;
  }
  return (int32_t)(sum * 65536 / norm);
}

}  // namespace gf
