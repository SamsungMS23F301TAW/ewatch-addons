// Dayprint's typeface: an original monoline geometric stroke font. Glyphs are
// centre-line paths (lines and elliptical arcs) on a grid where the cap
// height is 100 units, rendered as anti-aliased round-capped strokes into an
// 8-bit coverage mask at any size and weight. Uppercase only (lowercase maps
// to uppercase), digits, a little punctuation and a few icons.
#pragma once
#include <stdint.h>

namespace gf {

// Icon code points (use inside strings, e.g. "\x02 8,412").
static const char kGlyphNumero  = '\x01';   // "º" for "Nº"
static const char kGlyphSteps   = '\x02';   // footprints
static const char kGlyphSun     = '\x03';
static const char kGlyphSparkle = '\x04';
static const char kGlyphBattery = '\x05';   // low battery
static const char kGlyphDot     = '\x06';   // centred dot separator

struct Mask {
  uint8_t *px;
  int16_t  w, h;
};

// Width in Q8 pixels of `s` at cap height capQ8 with `tracking` extra font
// units between glyphs.
int32_t textWidth(const char *s, int32_t capQ8, int32_t tracking);

// Draws `s` with its left edge at x (Q8) and baseline at y (Q8). hwQ8 is
// the stroke half-width in Q8 pixels. Coverage is max-combined into the
// mask, so overlapping strokes and a halo pass never double up.
void drawText(Mask &m, int32_t x, int32_t y, const char *s, int32_t capQ8,
              int32_t hwQ8, int32_t tracking, int32_t alpha = 255);

// Same, horizontally centred on cx.
void drawTextCentered(Mask &m, int32_t cx, int32_t y, const char *s, int32_t capQ8,
                      int32_t hwQ8, int32_t tracking, int32_t alpha = 255);

// Max-combines one round-capped stroke segment into the mask (Q8 coords),
// with coverage scaled by alpha (0..255).
void maskSegment(Mask &m, int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t hwQ8,
                 int32_t alpha = 255);

}  // namespace gf
