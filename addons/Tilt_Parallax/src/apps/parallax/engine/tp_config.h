// Tilt Parallax — tunables in one place.
#pragma once
#include <stdint.h>

namespace tp {

static const int kScreenW = 240;
static const int kScreenH = 280;

// Parallax travel, in pixels, of a depth-1.0 layer (the sky) at full tilt
// with strength "Normal". Other layers move in proportion to their depth.
static const float kMaxShiftX = 18.0f;
// Vertical travel relative to horizontal. Landscapes are wide; a little less
// vertical motion keeps the horizon calm.
static const float kVertRatio = 0.65f;

// Strength presets (Settings -> Parallax -> Depth). Index stored in NVS.
static const int   kStrengthCount = 4;
static const float kStrengthScale[kStrengthCount] = { 0.0f, 0.6f, 1.0f, 1.5f };
static const char *const kStrengthName[kStrengthCount] = { "Off", "Subtle", "Normal", "Strong" };
static const int   kStrengthDefault = 2;
static const float kMaxStrength = 1.5f;

// Horizontal / vertical overscan (pixels each side) a layer needs so that it
// never exposes an edge at maximum strength and full tilt.
static inline int overscanX(float depth) {
  float d = depth < 0 ? -depth : depth;
  return (int)(d * kMaxShiftX * kMaxStrength + 0.999f) + 2;
}
static inline int overscanY(float depth) {
  float d = depth < 0 ? -depth : depth;
  return (int)(d * kMaxShiftX * kMaxStrength * kVertRatio + 0.999f) + 2;
}

// Where the clock floats: its depth factor, and its drop shadow's (a little
// deeper, so the shadow slides against the digits and the time hovers).
static const float kClockDepth  = 0.42f;
static const float kShadowDepth = 0.56f;

}  // namespace tp
