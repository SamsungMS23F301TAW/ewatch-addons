// Shared pair animations.
//
// Every frame is a pure function of (kind, seed, the two ids, t). The seed
// and palette come from both watch ids, order-independent, so two watches
// that agree on the start instant draw identical shapes at the same moment.
// Only the caption differs (each watch shows the other person's name).
// Every pair of mates therefore gets its own "signature" hello.
#pragma once
#include "fr_gfx.h"
#include "fr_types.h"

namespace fr {

constexpr uint32_t kHelloDurMs     = 3600;   // keep in sync with HandshakeConfig
constexpr uint32_t kCelebrateDurMs = 3000;

struct PairPalette { uint16_t a, b, mix; };
PairPalette pairPalette(uint32_t idA, uint32_t idB);

struct PairAnimSpec {
  AnimKind kind = AnimKind::None;
  uint32_t seed = 0;
  uint32_t idA = 0, idB = 0;      // either order
  char     name[kMaxName + 1] = {0};   // the other person, for the caption
};

uint32_t animDurationMs(AnimKind k);

// Draws over whatever is already in `c` (normally the radar frame).
// t is milliseconds since the shared t = 0; values outside [0, duration)
// draw nothing.
void drawPairAnim(Canvas &c, const PairAnimSpec &spec, int32_t tMs);

}  // namespace fr
