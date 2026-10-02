// Friend Radar motion: damped springs and easing curves (pure C++).
//
// Every moving part of the UI (the detail card, the camera that locks onto a
// mate, page pushes, switch knobs, toasts) is a Spring stepped with real
// elapsed time, so motion feels physical and frame-rate independent.
#pragma once
#include <math.h>

namespace fr {

struct Spring {
  float x = 0.f, v = 0.f, target = 0.f;
  float omega = 16.f;    // natural frequency, rad/s (higher = snappier)
  float zeta = 0.78f;    // damping ratio (< 1 overshoots a little)

  Spring() = default;
  Spring(float omega_, float zeta_) : omega(omega_), zeta(zeta_) {}

  void snap(float to) { x = target = to; v = 0.f; }

  // Semi-implicit Euler in <= 4 ms substeps: stable for any frame time.
  void step(float dtSec) {
    if (dtSec <= 0.f) return;
    if (dtSec > 0.25f) dtSec = 0.25f;
    int n = (int)(dtSec / 0.004f) + 1;
    float h = dtSec / (float)n;
    for (int i = 0; i < n; ++i) {
      float a = -omega * omega * (x - target) - 2.f * zeta * omega * v;
      v += a * h;
      x += v * h;
    }
    if (settled()) { x = target; v = 0.f; }
  }

  bool settled(float eps = 0.0015f) const {
    return fabsf(x - target) < eps && fabsf(v) < eps * 20.f;
  }
};

static inline float clamp01f(float t) { return t < 0.f ? 0.f : (t > 1.f ? 1.f : t); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float easeOutCubicf(float t) { t = clamp01f(t); float u = 1.f - t; return 1.f - u * u * u; }
static inline float easeInOutCubicf(float t) {
  t = clamp01f(t);
  return t < 0.5f ? 4.f * t * t * t : 1.f - powf(-2.f * t + 2.f, 3.f) * 0.5f;
}
static inline float smoothstepf(float e0, float e1, float x) {
  float t = clamp01f((x - e0) / (e1 - e0));
  return t * t * (3.f - 2.f * t);
}

}  // namespace fr
