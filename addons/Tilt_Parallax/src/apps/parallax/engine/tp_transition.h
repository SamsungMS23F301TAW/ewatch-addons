// Tilt Parallax — the scene-change animation ("pop-up book"): the current
// landscape's layers drop away, nearest first, then the new one rises in,
// farthest first, with a little overshoot. Shared by the face and the host
// preview tool so the docs show exactly what the watch does.
#pragma once
#include <stdint.h>
#include <math.h>

namespace tp {

static const uint32_t kOutDurMs = 260, kOutStaggerMs = 35;
static const uint32_t kInDurMs = 480, kInStaggerMs = 70;

static inline float easeInCubic(float u) { return u * u * u; }
static inline float easeOutBack(float u) {
  const float c1 = 1.25f, c3 = c1 + 1.0f;
  float v = u - 1.0f;
  return 1.0f + c3 * v * v * v + c1 * v * v;
}

// Layers at (almost) the sky's depth sit on the horizon and stay put; they
// appear and disappear with the sky instead of flying through it.
static inline bool transitionAnimates(float depth) { return depth < 0.95f; }

// Depth rank of layer i: layers sharing a depth (a lighthouse and its cliff)
// get the same rank so they move as one piece. `depths` is far -> near.
static inline int transitionRank(const float *depths, int i) {
  int r = 0;
  for (int k = 1; k <= i; k++)
    if (depths[k] < depths[k - 1] - 1e-3f) r++;
  return r;
}

// A layer of depth rank `rank` (of `ranks`) dropping from offset `from` to
// `dist` (off-screen), t ms into the "out" phase. Sets *done=false while any
// layer is still moving.
static inline int transitionOutDy(int rank, int ranks, uint32_t t, int from, int dist, bool *done) {
  uint32_t delay = (uint32_t)(ranks - 1 - rank) * kOutStaggerMs;  // nearest first
  float u = t <= delay ? 0.0f : (float)(t - delay) / kOutDurMs;
  if (u < 1.0f) *done = false;
  if (u > 1.0f) u = 1.0f;
  return from + (int)(easeInCubic(u) * (dist - from));
}

// A layer of depth rank `rank` rising from `dist` to 0, t ms into "in".
static inline int transitionInDy(int rank, uint32_t t, int dist, bool *done) {
  uint32_t delay = (uint32_t)rank * kInStaggerMs;                  // farthest first
  float u = t <= delay ? 0.0f : (float)(t - delay) / kInDurMs;
  if (u < 1.0f) *done = false;
  if (u > 1.0f) u = 1.0f;
  return (int)lroundf((1.0f - easeOutBack(u)) * dist);
}

}  // namespace tp
