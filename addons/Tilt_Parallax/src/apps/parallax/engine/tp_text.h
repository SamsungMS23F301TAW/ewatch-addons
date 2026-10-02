// Tilt Parallax — anti-aliased type for the clock and the status line.
//
// The big time uses a geometric monoline digit set drawn from signed distance
// fields (line segments and arcs with round caps), so it's crisp at any size
// and needs no font data. The small date / battery line supersamples the
// project's FreeSansBold 24pt glyphs down ~3x, which gives smooth 11 px caps.
// Both render coverage into ALPHA layers; shadows are blurred copies.
#pragma once
#include <stdint.h>
#include "tp_layer.h"

namespace tp {

struct ClockMetrics {
  float digitH = 66;       // digit height in px
  float digitW = 40;       // digit advance box width
  float stroke = 7.5f;     // stroke width in px
  float gap = 9;           // space between glyph boxes
  float colonW = 12;       // colon box width
};

// Width in pixels of `text` (digits, ':' and ' ') at the given metrics.
float clockTextWidth(const char *text, const ClockMetrics &m);
// Draws coverage for `text` into an ALPHA layer with its top-left at (ox, oy)
// in layer pixels. Existing coverage is max-combined.
void drawClockText(Layer &L, float ox, float oy, const char *text, const ClockMetrics &m);

// Small UI text. `pxHeight` is the target cap height in pixels.
int  smallTextWidth(const char *text, float capPx);
void drawSmallText(Layer &L, float ox, float baselineY, const char *text, float capPx);

// Battery glyph (outline, nub, level bar), top-left at (ox, oy), body w x h.
void drawBatteryGlyph(Layer &L, float ox, float oy, float w, float h, int pct);

// dst = blur(dilate(src, spread), radius) * gain, same size as src. Both are
// ALPHA layers of identical dimensions. Used for soft shadows and halos.
void blurAlphaInto(const Layer &src, Layer &dst, int spread, int radius, float gain);

}  // namespace tp
