// Colour maths for the art buffer: packed 0x00RRGGBB, integer HSL, blending,
// luma, and the daily palette generator. Deterministic (see gf_core.h rules).
#pragma once
#include <stdint.h>
#include "gf_core.h"

namespace gf {

static inline uint32_t rgb(uint32_t r, uint32_t g, uint32_t b) {
  return (r << 16) | (g << 8) | b;
}
static inline uint32_t chR(uint32_t c) { return (c >> 16) & 0xFF; }
static inline uint32_t chG(uint32_t c) { return (c >> 8) & 0xFF; }
static inline uint32_t chB(uint32_t c) { return c & 0xFF; }

// a in [0, 256]: 0 keeps d, 256 gives s.
static inline uint32_t blend(uint32_t d, uint32_t s, uint32_t a) {
  if (a >= 256) return s;
  uint32_t na = 256 - a;
  uint32_t rb = (((d & 0xFF00FFu) * na + (s & 0xFF00FFu) * a) >> 8) & 0xFF00FFu;
  uint32_t g  = (((d & 0x00FF00u) * na + (s & 0x00FF00u) * a) >> 8) & 0x00FF00u;
  return rb | g;
}
// Perceptual-ish brightness 0..255 (Rec. 709 weights, sum 256).
static inline int32_t luma(uint32_t c) {
  return (int32_t)((54 * chR(c) + 183 * chG(c) + 19 * chB(c)) >> 8);
}
// Multiply every channel by f/256 (f may exceed 256 to brighten; clamps).
static inline uint32_t scaleRGB(uint32_t c, int32_t f) {
  int32_t r = iclamp((int32_t)chR(c) * f >> 8, 0, 255);
  int32_t g = iclamp((int32_t)chG(c) * f >> 8, 0, 255);
  int32_t b = iclamp((int32_t)chB(c) * f >> 8, 0, 255);
  return rgb((uint32_t)r, (uint32_t)g, (uint32_t)b);
}

// h in degrees (any integer, wrapped), s and l in permille (0..1000).
uint32_t hsl(int32_t h, int32_t s, int32_t l);

// Harmony schemes for the daily palette.
enum Scheme : uint8_t {
  kAnalogous = 0, kComplementary, kSplitComp, kTriadic, kTetradic, kMonochrome,
  kSchemeCount
};
const char *schemeName(uint8_t s);

struct Palette {
  uint32_t bg0, bg1;      // background gradient, top -> bottom
  uint32_t ink[5];        // main colours, ink[0] is the hero colour
  uint32_t accent;        // small "pop" colour
  uint32_t light, dark;   // highlight / shadow extremes in the palette's hue
  uint32_t text;          // clock colour with strong contrast vs the art
  uint32_t halo;          // clock halo / scrim colour
  int16_t  baseHue;       // degrees
  uint8_t  scheme;
  bool     darkBg;
};

// Draws a palette from rng. `hueCenter` biases the base hue (the yearly
// colour drift); `darkPermille` is the chance of a dark background.
void makePalette(Rng &rng, int32_t hueCenter, int32_t hueSpread,
                 uint32_t darkPermille, Palette &out);

}  // namespace gf
