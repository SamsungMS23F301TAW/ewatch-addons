// CURRENTS — strokes traced through a smooth noise flow field, optionally
// bent by a couple of vortices. Every element is one stroke; walking adds
// more of them. Variants: SILK (long hairlines), BRUSH (short tapered
// brushwork), DRIFT (strokes made of dots).
#include <string.h>
#include "gf_family.h"

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

namespace gf {

namespace {
struct St {
  uint8_t  variant, seeding, colorMode, nVort;
  uint32_t noiseSeed, colorSeed;
  int32_t  scaleQ16;          // lattice units per pixel, Q16
  int32_t  colorScaleQ16;
  int32_t  turnsQ8;           // field swing in turns, Q8
  uint16_t baseAngle;
  int32_t  stepQ8;
  int32_t  lenMin, lenMax;
  int32_t  hwMin, hwMax;      // Q8
  int32_t  alphaMin, alphaMax;
  int32_t  n0, n1;
  int32_t  ringCx, ringCy, ringR;
  int32_t  vx[3], vy[3], vr[3], vdir[3];
};
static const int32_t kMaxPts = 300;

inline St &S(void *p) { return *static_cast<St *>(p); }
inline const St &S(const void *p) { return *static_cast<const St *>(p); }

// Unit flow direction (Q14) at Q8 position (x, y).
void fieldDir(const St &s, int32_t x, int32_t y, int32_t &ux, int32_t &uy) {
  int32_t nx = (int32_t)(((int64_t)x * s.scaleQ16) >> 8);
  int32_t ny = (int32_t)(((int64_t)y * s.scaleQ16) >> 8);
  int32_t n = fbm2(nx, ny, s.noiseSeed, 2);
  uint16_t a = (uint16_t)(s.baseAngle + (int32_t)(((int64_t)n * s.turnsQ8) >> 8));
  int32_t fx = icos(a), fy = isin(a);
  for (int32_t v = 0; v < s.nVort; v++) {
    int32_t dx = (x - s.vx[v]) >> 4, dy = (y - s.vy[v]) >> 4;      // Q4
    int64_t d2 = (int64_t)dx * dx + (int64_t)dy * dy;
    int64_t r = s.vr[v] >> 4;
    int64_t r2 = r * r;
    if (d2 > 9 * r2) continue;
    int32_t d = (int32_t)isqrt64((uint64_t)d2);
    if (d < 1) continue;
    int32_t tx = (int32_t)(((int64_t)-dy * 16384) / d) * s.vdir[v];
    int32_t ty = (int32_t)(((int64_t)dx * 16384) / d) * s.vdir[v];
    int32_t w = (int32_t)((r2 * 65536) / (d2 + r2));              // Q16 weight
    fx = (int32_t)(((int64_t)fx * (65536 - w) + (int64_t)tx * w) >> 16);
    fy = (int32_t)(((int64_t)fy * (65536 - w) + (int64_t)ty * w) >> 16);
  }
  int32_t len = (int32_t)isqrt64((uint64_t)((int64_t)fx * fx + (int64_t)fy * fy));
  if (len < 64) { ux = 16384; uy = 0; return; }
  ux = (int32_t)(((int64_t)fx * 16384) / len);
  uy = (int32_t)(((int64_t)fy * 16384) / len);
}
}  // namespace

void currentsInit(void *st, JobCtx &ctx) {
  St &s = S(st);
  Rng &r = *ctx.famRng;
  s.variant = ctx.spec->variant;
  s.noiseSeed = r.next();
  s.colorSeed = r.next();
  int32_t feature = r.range(70, 150);               // px per noise unit
  s.scaleQ16 = 65536 / feature;
  s.colorScaleQ16 = 65536 / r.range(80, 200);
  s.turnsQ8 = r.range(150, 420);
  s.baseAngle = r.angle();
  s.seeding = (uint8_t)r.below(4);
  s.colorMode = (uint8_t)r.below(3);
  s.ringCx = r.range(60, 180) * 256;
  s.ringCy = r.range(80, 200) * 256;
  s.ringR = r.range(40, 100) * 256;
  s.nVort = (uint8_t)r.below(3);
  for (int32_t v = 0; v < 3; v++) {
    s.vx[v] = r.range(20, 220) * 256;
    s.vy[v] = r.range(30, 250) * 256;
    s.vr[v] = r.range(30, 70) * 256;
    s.vdir[v] = r.chance(500) ? 1 : -1;
  }
  switch (s.variant) {
    case 0:   // SILK
      s.stepQ8 = 420; s.lenMin = 80; s.lenMax = 220;
      s.hwMin = 80; s.hwMax = 170; s.alphaMin = 120; s.alphaMax = 215;
      s.n0 = 60; s.n1 = 760;
      break;
    case 1:   // BRUSH
      s.stepQ8 = 512; s.lenMin = 18; s.lenMax = 60;
      s.hwMin = 330; s.hwMax = 900; s.alphaMin = 215; s.alphaMax = 250;
      s.n0 = 30; s.n1 = 380;
      break;
    default:  // DRIFT
      s.stepQ8 = 450; s.lenMin = 50; s.lenMax = 150;
      s.hwMin = 140; s.hwMax = 420; s.alphaMin = 150; s.alphaMax = 235;
      s.n0 = 45; s.n1 = 620;
      break;
  }
}

int32_t currentsPrepCount(const void *) { return kH / kBgRowsPerOp; }

void currentsPrep(void *, JobCtx &ctx, int32_t i) {
  backgroundRows(ctx, i * kBgRowsPerOp, (i + 1) * kBgRowsPerOp, 3);
}

int32_t currentsElemCount(const void *st, int32_t g) {
  const St &s = S(st);
  return growCount(s.n0, s.n1, g);
}

void currentsElem(void *st, JobCtx &ctx, int32_t) {
  St &s = S(st);
  Rng &r = *ctx.elemRng;
  const Palette &p = ctx.spec->pal;

  // Start point.
  int32_t x, y;
  switch (s.seeding) {
    case 1: {   // ring
      uint16_t a = r.angle();
      int32_t jr = r.range(-24, 24) * 256;
      x = s.ringCx + (int32_t)(((int64_t)icos(a) * (s.ringR + jr)) >> 14);
      y = s.ringCy + (int32_t)(((int64_t)isin(a) * (s.ringR + jr)) >> 14);
      break;
    }
    case 2: {   // two soft clusters
      int32_t which = (int32_t)r.below(2);
      int32_t ox = r.tri(90);
      int32_t oy = r.tri(110);
      x = (which ? 70 : 170) * 256 + ox * 256;
      y = (which ? 90 : 190) * 256 + oy * 256;
      break;
    }
    default: {  // uniform, starting a little outside the frame too
      int32_t rx = r.range(-12, kW + 12);
      int32_t ry = r.range(-12, kH + 12);
      x = rx * 256; y = ry * 256;
      break;
    }
  }
  int32_t len = r.range(s.lenMin, s.lenMax);
  int32_t hw = r.range(s.hwMin, s.hwMax);
  int32_t alpha = r.range(s.alphaMin, s.alphaMax);
  uint32_t roll = r.below(1000);
  uint32_t randInk = r.below(5);
  int32_t dir = r.chance(500) ? 1 : -1;
  if (len > kMaxPts) len = kMaxPts;

  // Colour.
  uint32_t color;
  if (roll < 25) {
    color = p.accent;
  } else if (s.colorMode == 0) {
    int32_t cx = (int32_t)(((int64_t)x * s.colorScaleQ16) >> 8);
    int32_t cy = (int32_t)(((int64_t)y * s.colorScaleQ16) >> 8);
    int32_t n = noise2(cx, cy, s.colorSeed);
    int32_t k = iclamp((n + 65536) * 5 / 131073, 0, 4);
    color = p.ink[k];
  } else if (s.colorMode == 1) {
    int32_t k = iclamp(((y >> 8) + 20) * 5 / (kH + 40), 0, 4);
    color = p.ink[k];
  } else {
    color = p.ink[randInk];
  }

  // Trace the stroke through the field.
  int32_t *xy = reinterpret_cast<int32_t *>(ctx.mem);
  int32_t *ws = xy + 2 * kMaxPts;
  int32_t n = 0;
  int32_t lo = -16 * 256, hiX = (kW + 16) * 256, hiY = (kH + 16) * 256;
  for (int32_t k = 0; k < len; k++) {
    xy[2 * n] = x; xy[2 * n + 1] = y; n++;
    int32_t ux, uy;
    fieldDir(s, x, y, ux, uy);
    x += (int32_t)(((int64_t)ux * s.stepQ8 * dir) >> 14);
    y += (int32_t)(((int64_t)uy * s.stepQ8 * dir) >> 14);
    if (x < lo || y < lo || x > hiX || y > hiY) break;
  }
  if (n < 2) return;

  // Width profile: spindle taper for BRUSH, gentle for the others.
  for (int32_t k = 0; k < n; k++) {
    uint16_t a = (uint16_t)(((k * 2 + 1) * 32768) / (2 * n));    // 0..pi
    int32_t sn = isin(a);                                        // Q14 0..1
    int32_t f = (s.variant == 1) ? (3000 + ((sn * 13384) >> 14))
                                 : (9000 + ((sn * 7384) >> 14));
    ws[k] = (hw * f) >> 14;
  }

  if (s.variant == 2) {
    for (int32_t k = 0; k < n; k += 3) {
      disc(*ctx.cv, xy[2 * k], xy[2 * k + 1], ws[k], color, (uint32_t)alpha);
    }
  } else {
    polyline(*ctx.cv, xy, n, ws, 0, color, (uint32_t)alpha);
  }
}

}  // namespace gf
