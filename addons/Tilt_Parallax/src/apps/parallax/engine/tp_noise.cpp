#include "tp_noise.h"

namespace tp {

static inline float quintic(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

float noise1(float x, uint32_t seed) {
  float fl = floorf(x);
  int32_t i = (int32_t)fl;
  float t = quintic(x - fl);
  float a = hashUnit(hash2(i, 0, seed)) * 2.0f - 1.0f;
  float b = hashUnit(hash2(i + 1, 0, seed)) * 2.0f - 1.0f;
  return a + (b - a) * t;
}

float noise2(float x, float y, uint32_t seed) {
  float fx = floorf(x), fy = floorf(y);
  int32_t ix = (int32_t)fx, iy = (int32_t)fy;
  float tx = quintic(x - fx), ty = quintic(y - fy);
  float a = hashUnit(hash2(ix,     iy,     seed));
  float b = hashUnit(hash2(ix + 1, iy,     seed));
  float c = hashUnit(hash2(ix,     iy + 1, seed));
  float d = hashUnit(hash2(ix + 1, iy + 1, seed));
  float top = a + (b - a) * tx;
  float bot = c + (d - c) * tx;
  return (top + (bot - top) * ty) * 2.0f - 1.0f;
}

float fbm1(float x, uint32_t seed, int oct, float gain) {
  float sum = 0, amp = 1, norm = 0;
  for (int o = 0; o < oct; o++) {
    sum += noise1(x, seed + (uint32_t)o * 1013u) * amp;
    norm += amp;
    amp *= gain;
    x *= 2.0f;
  }
  return sum / norm;
}

float fbm2(float x, float y, uint32_t seed, int oct, float gain) {
  float sum = 0, amp = 1, norm = 0;
  for (int o = 0; o < oct; o++) {
    sum += noise2(x, y, seed + (uint32_t)o * 7919u) * amp;
    norm += amp;
    amp *= gain;
    x *= 2.0f; y *= 2.0f;
  }
  return sum / norm;
}

float ridged1(float x, uint32_t seed, int oct, float gain) {
  float sum = 0, amp = 1, norm = 0;
  for (int o = 0; o < oct; o++) {
    float n = 1.0f - fabsf(noise1(x, seed + (uint32_t)o * 4099u));
    sum += n * n * amp;
    norm += amp;
    amp *= gain;
    x *= 2.0f;
  }
  return sum / norm;
}

// Midpoint displacement over an arbitrary-length array: split [lo,hi] at its
// midpoint, offset the midpoint by a random amount, recurse with the
// amplitude scaled by `rough`. Depth is log2(n) (about 10), so plain
// recursion is fine on any task stack and keeps this reentrant.
static void mpdRec(float *out, int lo, int hi, float a, float rough, Rng &rng) {
  if (hi - lo < 2) return;
  int mid = (lo + hi) >> 1;
  out[mid] = 0.5f * (out[lo] + out[hi]) + rng.range(-a, a);
  mpdRec(out, lo, mid, a * rough, rough, rng);
  mpdRec(out, mid, hi, a * rough, rough, rng);
}

void midpointRidge(float *out, int n, Rng &rng, float rough, float amp) {
  if (n <= 0) return;
  if (n == 1) { out[0] = rng.range(-amp, amp); return; }
  out[0] = rng.range(-amp, amp) * 0.5f;
  out[n - 1] = rng.range(-amp, amp) * 0.5f;
  mpdRec(out, 0, n - 1, amp, rough, rng);
}

}  // namespace tp
