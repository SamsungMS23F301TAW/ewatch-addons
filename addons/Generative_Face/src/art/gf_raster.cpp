#include "gf_raster.h"

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

namespace gf {

// Box-filtered coverage of a band of half-width hw at distance d (all Q8).
static inline int32_t bandCoverage(int32_t hw, int32_t d) {
  int32_t peak = imin(2 * hw, 256);
  int32_t cov = imin(peak, hw + 128 - d);
  return cov < 0 ? 0 : cov;
}

void gradientRows(Canvas &c, int32_t y0, int32_t y1, uint32_t top, uint32_t bottom,
                  uint32_t grainSeed, int32_t grain) {
  y0 = imax(y0, 0); y1 = imin(y1, c.h);
  int32_t span = imax(c.h - 1, 1);
  for (int32_t y = y0; y < y1; y++) {
    uint32_t t = (uint32_t)((y * 256) / span);
    uint32_t base = blend(top, bottom, t);
    int32_t br = (int32_t)chR(base), bg = (int32_t)chG(base), bb = (int32_t)chB(base);
    uint32_t *row = c.px + y * c.w;
    for (int32_t x = 0; x < c.w; x++) {
      if (grain > 0) {
        uint32_t n = hash2(x, y, grainSeed) & 0xFF;
        int32_t o = (int32_t)((n * (uint32_t)(2 * grain + 1)) >> 8) - grain;
        row[x] = rgb((uint32_t)iclamp(br + o, 0, 255), (uint32_t)iclamp(bg + o, 0, 255),
                     (uint32_t)iclamp(bb + o, 0, 255));
      } else {
        row[x] = base;
      }
    }
  }
}

void segment(Canvas &c, int32_t ax, int32_t ay, int32_t bx, int32_t by,
             int32_t hwA, int32_t hwB, uint32_t color, uint32_t alpha,
             bool capStart, bool capEnd) {
  if (alpha == 0) return;
  if (hwA < 0) hwA = 0;
  if (hwB < 0) hwB = 0;
  int32_t dx = bx - ax, dy = by - ay;
  int64_t len2 = (int64_t)dx * dx + (int64_t)dy * dy;
  int32_t len = (int32_t)isqrt64((uint64_t)len2);
  int32_t hwMax = imax(hwA, hwB);
  if (len < 16) {
    if (capStart || capEnd) disc(c, ax, ay, hwMax, color, alpha);
    return;
  }
  int32_t ux = (int32_t)(((int64_t)dx * 16384) / len);
  int32_t uy = (int32_t)(((int64_t)dy * 16384) / len);
  int32_t dhw = (int32_t)(((int64_t)(hwB - hwA) * 65536) / len);
  int32_t pad = hwMax + 256;
  int32_t x0 = imax(floorShift(imin(ax, bx) - pad, 8), 0);
  int32_t x1 = imin((imax(ax, bx) + pad) >> 8, c.w - 1);
  int32_t y0 = imax(floorShift(imin(ay, by) - pad, 8), 0);
  int32_t y1 = imin((imax(ay, by) + pad) >> 8, c.h - 1);
  if (x0 > x1 || y0 > y1) return;
  // Projection (t) and signed normal distance (n) are advanced incrementally
  // along each row in Q22 (Q8 position x Q14 direction).
  const int64_t stepT = (int64_t)ux * 256, stepN = (int64_t)uy * 256;
  for (int32_t y = y0; y <= y1; y++) {
    int32_t py = y * 256 + 128 - ay;
    int32_t px0 = x0 * 256 + 128 - ax;
    int64_t accT = (int64_t)px0 * ux + (int64_t)py * uy;
    int64_t accN = (int64_t)px0 * uy - (int64_t)py * ux;
    uint32_t *row = c.px + y * c.w;
    for (int32_t x = x0; x <= x1; x++, accT += stepT, accN += stepN) {
      int32_t t = (int32_t)(accT >> 14);
      int32_t d, hw;
      if (t <= 0) {
        if (!capStart) continue;
        int32_t px = x * 256 + 128 - ax;
        d = (int32_t)isqrt64((uint64_t)((int64_t)px * px + (int64_t)py * py));
        hw = hwA;
      } else if (t > len) {
        if (!capEnd) continue;
        int32_t qx = x * 256 + 128 - ax - dx, qy = py - dy;
        d = (int32_t)isqrt64((uint64_t)((int64_t)qx * qx + (int64_t)qy * qy));
        hw = hwB;
      } else {
        int32_t nn = (int32_t)(accN >> 14);
        d = nn < 0 ? -nn : nn;
        hw = hwA + (int32_t)(((int64_t)dhw * t) >> 16);
      }
      int32_t cov = bandCoverage(hw, d);
      if (cov <= 0) continue;
      row[x] = blend(row[x], color, (alpha * (uint32_t)cov) >> 8);
    }
  }
}

void polyline(Canvas &c, const int32_t *xy, int32_t n, const int32_t *hw,
              int32_t hwConst, uint32_t color, uint32_t alpha) {
  if (n < 2) {
    if (n == 1) disc(c, xy[0], xy[1], hw ? hw[0] : hwConst, color, alpha);
    return;
  }
  for (int32_t i = 0; i + 1 < n; i++) {
    int32_t ha = hw ? hw[i] : hwConst;
    int32_t hb = hw ? hw[i + 1] : hwConst;
    segment(c, xy[2 * i], xy[2 * i + 1], xy[2 * i + 2], xy[2 * i + 3], ha, hb,
            color, alpha, i == 0, i + 2 == n);
  }
}

void disc(Canvas &c, int32_t cx, int32_t cy, int32_t r, uint32_t color, uint32_t alpha) {
  if (r <= 0 || alpha == 0) return;
  int32_t pad = r + 256;
  int32_t x0 = imax(floorShift(cx - pad, 8), 0), x1 = imin((cx + pad) >> 8, c.w - 1);
  int32_t y0 = imax(floorShift(cy - pad, 8), 0), y1 = imin((cy + pad) >> 8, c.h - 1);
  int32_t peak = imin(2 * r, 256);
  int64_t inner = r - 128;
  int64_t inner2 = inner > 0 ? inner * inner : -1;
  int64_t outer = r + 128;
  int64_t outer2 = outer * outer;
  for (int32_t y = y0; y <= y1; y++) {
    int64_t py = y * 256 + 128 - cy;
    uint32_t *row = c.px + y * c.w;
    for (int32_t x = x0; x <= x1; x++) {
      int64_t px = x * 256 + 128 - cx;
      int64_t d2 = px * px + py * py;
      if (d2 >= outer2) continue;
      int32_t cov;
      if (d2 <= inner2) cov = peak;
      else cov = bandCoverage(r, (int32_t)isqrt64((uint64_t)d2));
      if (cov <= 0) continue;
      row[x] = blend(row[x], color, (alpha * (uint32_t)cov) >> 8);
    }
  }
}

// Is angle `a` inside the clockwise sweep starting at a0?
static inline bool angleIn(uint16_t a, uint16_t a0, uint32_t sweep) {
  return (uint32_t)(uint16_t)(a - a0) <= sweep;
}

void arc(Canvas &c, int32_t cx, int32_t cy, int32_t r, int32_t hw,
         uint16_t a0, uint32_t sweep, uint32_t color, uint32_t alpha) {
  if (r <= 0 || hw <= 0 || alpha == 0 || sweep == 0) return;
  bool full = sweep >= 65536;
  if (full) sweep = 65536;
  uint16_t a1 = (uint16_t)(a0 + sweep);
  int32_t sx = icos(a0), sy = isin(a0);
  int32_t ex = icos(a1), ey = isin(a1);
  bool big = sweep > 32768;

  // Bounding box of the arc: its end points plus any axis extreme it spans.
  int32_t rOut = r + hw + 256;
  int32_t bx0, bx1, by0, by1;
  if (full) {
    bx0 = cx - rOut; bx1 = cx + rOut; by0 = cy - rOut; by1 = cy + rOut;
  } else {
    int32_t px0 = cx + (int32_t)(((int64_t)sx * r) >> 14);
    int32_t py0 = cy + (int32_t)(((int64_t)sy * r) >> 14);
    int32_t px1 = cx + (int32_t)(((int64_t)ex * r) >> 14);
    int32_t py1 = cy + (int32_t)(((int64_t)ey * r) >> 14);
    bx0 = imin(px0, px1); bx1 = imax(px0, px1);
    by0 = imin(py0, py1); by1 = imax(py0, py1);
    if (angleIn(0, a0, sweep))     bx1 = cx + r;
    if (angleIn(16384, a0, sweep)) by1 = cy + r;
    if (angleIn(32768, a0, sweep)) bx0 = cx - r;
    if (angleIn(49152, a0, sweep)) by0 = cy - r;
    int32_t m = hw + 384;
    bx0 -= m; bx1 += m; by0 -= m; by1 += m;
  }
  int32_t x0 = imax(floorShift(bx0, 8), 0), x1 = imin(bx1 >> 8, c.w - 1);
  int32_t y0 = imax(floorShift(by0, 8), 0), y1 = imin(by1 >> 8, c.h - 1);
  if (x0 > x1 || y0 > y1) return;

  int64_t ro = r + hw + 128;
  int64_t ri = r - hw - 128;
  int64_t ro2 = ro * ro;
  int64_t ri2 = ri > 0 ? ri * ri : -1;
  for (int32_t y = y0; y <= y1; y++) {
    int64_t py = y * 256 + 128 - cy;
    int64_t py2 = py * py;
    if (py2 >= ro2) continue;
    // Horizontal extent of the band on this row.
    int32_t xo = (int32_t)isqrt64((uint64_t)(ro2 - py2));
    int32_t xi = (ri2 > py2) ? (int32_t)isqrt64((uint64_t)(ri2 - py2)) : -1;
    uint32_t *row = c.px + y * c.w;
    for (int side = 0; side < 2; side++) {
      int32_t lo, hi;                              // Q8 offsets from cx
      if (xi < 0) { if (side) break; lo = -xo; hi = xo; }
      else if (side == 0) { lo = -xo; hi = -xi; }
      else { lo = xi; hi = xo; }
      int32_t xa = imax(floorShift(cx + lo - 128, 8), x0);
      int32_t xb = imin((cx + hi + 128) >> 8, x1);
      for (int32_t x = xa; x <= xb; x++) {
        int64_t px = x * 256 + 128 - cx;
        int64_t d2 = px * px + py2;
        if (d2 >= ro2 || d2 <= ri2) continue;
        int32_t d = (int32_t)isqrt64((uint64_t)d2);
        int32_t cov = bandCoverage(hw, iabs(d - r));
        if (cov <= 0) continue;
        if (!full) {
          int32_t cs = (int32_t)((sx * px - sy * py) >> 14);   // cross(s, p)
          int32_t ce = (int32_t)((px * ey - py * ex) >> 14);   // cross(p, e)
          int32_t ang;
          if (!big) {
            ang = imin(iclamp(cs + 128, 0, 256), iclamp(ce + 128, 0, 256));
          } else {
            // Outside wedge runs clockwise from e back to s (< 180 deg).
            int32_t oe = (int32_t)((ex * py - ey * px) >> 14); // cross(e, p)
            int32_t os = (int32_t)((px * sy - py * sx) >> 14); // cross(p, s)
            int32_t out = imin(iclamp(oe + 128, 0, 256), iclamp(os + 128, 0, 256));
            ang = 256 - out;
          }
          cov = (cov * ang) >> 8;
          if (cov <= 0) continue;
        }
        row[x] = blend(row[x], color, (alpha * (uint32_t)cov) >> 8);
      }
    }
  }
}

void glow(Canvas &c, int32_t cx, int32_t cy, int32_t r, uint32_t color, uint32_t alpha) {
  if (r <= 0 || alpha == 0) return;
  int32_t x0 = imax(floorShift(cx - r, 8), 0), x1 = imin((cx + r) >> 8, c.w - 1);
  int32_t y0 = imax(floorShift(cy - r, 8), 0), y1 = imin((cy + r) >> 8, c.h - 1);
  int64_t r2 = (int64_t)r * r;
  for (int32_t y = y0; y <= y1; y++) {
    int64_t py = y * 256 + 128 - cy;
    uint32_t *row = c.px + y * c.w;
    for (int32_t x = x0; x <= x1; x++) {
      int64_t px = x * 256 + 128 - cx;
      int64_t d2 = px * px + py * py;
      if (d2 >= r2) continue;
      int32_t u = (int32_t)((d2 * 65536) / r2);           // Q16, 0..65536
      int32_t f = 65536 - u;
      int32_t a = (int32_t)(((int64_t)f * f >> 16) * (int64_t)alpha >> 16);
      if (a <= 0) continue;
      row[x] = blend(row[x], color, (uint32_t)a);
    }
  }
}

void columnFill(Canvas &c, int32_t x, int32_t topQ8, int32_t bottomRow,
                uint32_t color, uint32_t alpha) {
  if ((uint32_t)x >= (uint32_t)c.w || alpha == 0) return;
  bottomRow = imin(bottomRow, c.h);
  int32_t ty = floorShift(topQ8, 8);
  int32_t frac = topQ8 - ty * 256;
  if (ty >= bottomRow) return;
  if (ty >= 0) {
    uint32_t a = (alpha * (uint32_t)(256 - frac)) >> 8;
    c.mix(x, ty, color, a);
  }
  for (int32_t y = imax(ty + 1, 0); y < bottomRow; y++) {
    uint32_t &d = c.px[y * c.w + x];
    d = blend(d, color, alpha);
  }
}

void glint(Canvas &c, int32_t cx, int32_t cy, int32_t size, uint32_t color, uint32_t alpha) {
  int32_t hw = imax(size / 7, 70);
  int32_t xy[6] = { cx - size, cy, cx, cy, cx + size, cy };
  int32_t ws[3] = { 0, hw, 0 };
  polyline(c, xy, 3, ws, 0, color, alpha);
  int32_t xy2[6] = { cx, cy - size, cx, cy, cx, cy + size };
  polyline(c, xy2, 3, ws, 0, color, alpha);
  glow(c, cx, cy, size * 2 / 3, color, alpha / 2);
}

}  // namespace gf
