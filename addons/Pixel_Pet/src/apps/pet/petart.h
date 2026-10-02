// Pixel Pet art — composes one frame of the creature into a 40x32 palette
// indexed buffer, then blits it with integer scaling. Pure C++ (host-previewed).
//
// The body is generated procedurally (gumdrop silhouette, 1-px outline, cel
// shading, belly, shine) so squash/stretch and the four growth stages come
// for free; faces, limbs, accessories and effects are hand-authored stamps
// (petart.cpp). Every animation is a pure function of (pose, time), so the
// watch and the previews render identical frames.
#pragma once
#include <stdint.h>
#include "px.h"
#include "petmodel.h"

namespace petart {

constexpr int kW = 40;   // stage width  (the body is ~18-24 px wide inside it)
constexpr int kH = 32;   // stage height (room above the head for clouds, Zs, hats)

// What the pet is doing right now. Idle loops depend on mood; the others are
// short one-shots driven by `animMs`.
enum class Act : uint8_t {
  Idle,       // mood idle loop
  Walk,       // marching along while the wearer walks
  Eat,        // munching a snack (one-shot, ~1.6 s)
  Petted,     // tap reaction (one-shot, ~1.4 s; depends on mood)
  Nudge,      // grumpy stomp used with the nudge buzz (one-shot)
  Celebrate,  // goal reached / evolved (one-shot sparkle jump)
  Hatch,      // egg hatching (one-shot, ~2.4 s)
  Count
};

struct Pose {
  pet::Stage     stage   = pet::Stage::Baby;
  pet::Mood      mood    = pet::Mood::Content;
  pet::Accessory acc     = pet::Accessory::None;
  bool           asleep  = false;
  bool           sulking = false;
  Act            act     = Act::Idle;
  uint32_t       animMs  = 0;      // time since `act` started
  uint32_t       clockMs = 0;      // free-running time (idle loops, blinks)
  uint8_t        eggPct  = 0;      // hatch progress for the egg (cracks)
};

// Durations of the one-shot acts.
uint32_t actDuration(Act a);

// Compose a frame into `out` (kW*kH indices) and return the palette to use.
const uint16_t *compose(const Pose &p, uint8_t out[kW * kH]);

// Compose + blit at (x, y) — top-left of the 40x32 stage — with `scale`.
void draw(px::Canvas &c, const Pose &p, int x, int y, int scale);

// Small standalone icons for UI (same palette): snack, heart, footprint...
enum class Icon : uint8_t { Snack, Heart, Foot, Flame, Star, Moon, Bowl, Count };
void drawIcon(px::Canvas &c, Icon i, int x, int y, int scale);
int  iconW(Icon i);
int  iconH(Icon i);

}  // namespace petart
