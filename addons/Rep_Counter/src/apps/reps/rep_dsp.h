// Rep Counter: tiny DSP toolkit (vectors, biquads, one-pole filters).
//
// Header-only, allocation-free, single precision. Used by the detector on the
// watch and by the host tests, so it must not include Arduino headers.
#pragma once
#include <math.h>
#include <stdint.h>

namespace reps {

struct V3 {
  float x = 0.f, y = 0.f, z = 0.f;
  V3() = default;
  V3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
  V3 operator+(const V3 &o) const { return V3(x + o.x, y + o.y, z + o.z); }
  V3 operator-(const V3 &o) const { return V3(x - o.x, y - o.y, z - o.z); }
  V3 operator*(float k) const { return V3(x * k, y * k, z * k); }
};

inline float dot(const V3 &a, const V3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(const V3 &a, const V3 &b) {
  return V3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
inline float norm(const V3 &a) { return sqrtf(dot(a, a)); }
inline V3 normalized(const V3 &a, const V3 &fallback = V3(0.f, 0.f, 1.f)) {
  float n = norm(a);
  return (n > 1e-6f) ? a * (1.f / n) : fallback;
}

// Angle between two unit vectors in degrees. atan2 form stays accurate near
// 0° and 180°, where acos(dot) loses most of its precision.
inline float angleDeg(const V3 &a, const V3 &b) {
  return atan2f(norm(cross(a, b)), dot(a, b)) * 57.2957795f;
}

// Second-order section, transposed direct form II.
struct Biquad {
  float b0 = 1.f, b1 = 0.f, b2 = 0.f, a1 = 0.f, a2 = 0.f;
  float z1 = 0.f, z2 = 0.f;

  // Butterworth-style low-pass (RBJ cookbook, bilinear transform).
  static Biquad lowpass(float fc, float fs, float q = 0.70710678f) {
    Biquad f;
    float w0 = 6.2831853f * fc / fs;
    float c = cosf(w0), s = sinf(w0);
    float alpha = s / (2.f * q);
    float a0 = 1.f + alpha;
    f.b0 = (1.f - c) * 0.5f / a0;
    f.b1 = (1.f - c) / a0;
    f.b2 = f.b0;
    f.a1 = -2.f * c / a0;
    f.a2 = (1.f - alpha) / a0;
    return f;
  }

  // Butterworth-style high-pass (RBJ cookbook).
  static Biquad highpass(float fc, float fs, float q = 0.70710678f) {
    Biquad f;
    float w0 = 6.2831853f * fc / fs;
    float c = cosf(w0), s = sinf(w0);
    float alpha = s / (2.f * q);
    float a0 = 1.f + alpha;
    f.b0 = (1.f + c) * 0.5f / a0;
    f.b1 = -(1.f + c) / a0;
    f.b2 = f.b0;
    f.a1 = -2.f * c / a0;
    f.a2 = (1.f - alpha) / a0;
    return f;
  }

  float step(float x) {
    float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }

  // Put the filter in the steady state it would reach for a constant input,
  // so a fresh stream doesn't start with a big step transient.
  void prime(float x) {
    float dc = (b0 + b1 + b2) / (1.f + a1 + a2);
    float y = x * dc;
    z1 = y - b0 * x;
    z2 = b2 * x - a2 * y;
  }
};

// First-order exponential smoother with time constant tau (seconds).
struct OnePole {
  float k = 1.f;
  float y = 0.f;
  void setTau(float tauSec, float fs) {
    k = (tauSec <= 0.f) ? 1.f : (1.f - expf(-1.f / (tauSec * fs)));
  }
  float step(float x) { y += k * (x - y); return y; }
  void prime(float x) { y = x; }
};

// Median of a small array (copies, insertion sort). n <= 32.
inline float smallMedian(const float *v, int n) {
  if (n <= 0) return 0.f;
  float tmp[32];
  if (n > 32) n = 32;
  for (int i = 0; i < n; i++) {
    float x = v[i];
    int j = i - 1;
    while (j >= 0 && tmp[j] > x) { tmp[j + 1] = tmp[j]; j--; }
    tmp[j + 1] = x;
  }
  return (n & 1) ? tmp[n / 2] : 0.5f * (tmp[n / 2 - 1] + tmp[n / 2]);
}

}  // namespace reps
