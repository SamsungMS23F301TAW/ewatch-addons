// Integer anti-aliased rasterizer over a 0x00RRGGBB canvas.
// Coordinates are Q8 (256 = one pixel); pixel centres sit at x*256+128.
// Alpha is 0..256. Coverage uses a 1-px box filter, so hairlines thinner
// than a pixel fade instead of disappearing.
#pragma once
#include <stdint.h>
#include "gf_core.h"
#include "gf_color.h"

namespace gf {

struct Canvas {
  uint32_t *px = nullptr;
  int16_t   w = kW, h = kH;

  inline void set(int32_t x, int32_t y, uint32_t c) {
    if ((uint32_t)x < (uint32_t)w && (uint32_t)y < (uint32_t)h) px[y * w + x] = c;
  }
  inline uint32_t get(int32_t x, int32_t y) const {
    if ((uint32_t)x < (uint32_t)w && (uint32_t)y < (uint32_t)h) return px[y * w + x];
    return 0;
  }
  inline void mix(int32_t x, int32_t y, uint32_t c, uint32_t a) {
    if (a == 0) return;
    if ((uint32_t)x < (uint32_t)w && (uint32_t)y < (uint32_t)h) {
      uint32_t &d = px[y * w + x];
      d = blend(d, c, a);
    }
  }
};

// Fills rows [y0, y1) with a vertical gradient spanning the whole canvas
// (top colour at y=0, bottom at y=h-1) plus deterministic film grain of
// +-grain levels. Rows are independent so callers can chunk the work.
void gradientRows(Canvas &c, int32_t y0, int32_t y1, uint32_t top, uint32_t bottom,
                  uint32_t grainSeed, int32_t grain);

// One polyline segment with linearly varying half-width (Q8). Interior
// joints are flat so translucent strokes never double-blend at vertices;
// `capStart` / `capEnd` add round caps at the polyline's true ends.
void segment(Canvas &c, int32_t ax, int32_t ay, int32_t bx, int32_t by,
             int32_t hwA, int32_t hwB, uint32_t color, uint32_t alpha,
             bool capStart, bool capEnd);

// Whole polyline (xy = Q8 pairs). hw == nullptr uses hwConst everywhere.
void polyline(Canvas &c, const int32_t *xy, int32_t n, const int32_t *hw,
              int32_t hwConst, uint32_t color, uint32_t alpha);

// Filled anti-aliased disc, radius Q8.
void disc(Canvas &c, int32_t cx, int32_t cy, int32_t r, uint32_t color, uint32_t alpha);

// Arc of an annulus: centre radius r, half-width hw (Q8), starting at angle
// a0 and sweeping clockwise by `sweep` (65536 = full ring). Flat ends are
// anti-aliased.
void arc(Canvas &c, int32_t cx, int32_t cy, int32_t r, int32_t hw,
         uint16_t a0, uint32_t sweep, uint32_t color, uint32_t alpha);

// Soft radial light: alpha falls from `alpha` at the centre to 0 at r.
void glow(Canvas &c, int32_t cx, int32_t cy, int32_t r, uint32_t color, uint32_t alpha);

// Anti-aliased horizontal-ish band fill for a column: paints rows from the
// Q8 height `topQ8` down to `bottomRow` (exclusive) in column x.
void columnFill(Canvas &c, int32_t x, int32_t topQ8, int32_t bottomRow,
                uint32_t color, uint32_t alpha);

// Four-point glint (a small cross of light) used for sparkles.
void glint(Canvas &c, int32_t cx, int32_t cy, int32_t size, uint32_t color, uint32_t alpha);

}  // namespace gf
