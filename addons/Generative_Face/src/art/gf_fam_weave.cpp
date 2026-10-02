// WEAVE — Truchet tiles: each square holds two quarter-circle bands joining
// its edge midpoints, so the bands link up into long meanders and closed
// loops. Each connected path gets one colour (union-find over the edge
// midpoints). Walking reveals tiles outward from a seed point, so the weave
// spreads across the face during the day. Variants: RIBBON (outlined bands
// with a sheen), RAIL (twin hairlines), KNOT (shaded tubes).
#include "gf_family.h"

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

namespace gf {

namespace {
struct St {
  uint8_t  variant;
  int32_t  s;              // tile size, px
  int32_t  cols, rows, nTiles;
  int32_t  hw;             // band half-width, Q8
  int32_t  outline;        // extra outline width, Q8
  int32_t  n0permille;
  uint32_t outlineColor;
};
inline St &S(void *p) { return *static_cast<St *>(p); }
inline const St &S(const void *p) { return *static_cast<const St *>(p); }

// Scratch layout (offsets into ctx.mem).
static const int32_t kMaxTiles = 400;
struct Mem {
  uint8_t  orient[kMaxTiles];
  uint32_t order[kMaxTiles];          // sorted keys; low 10 bits = tile index
  uint32_t colA[kMaxTiles], colB[kMaxTiles];
  int16_t  parent[1024];
};
inline Mem &M(JobCtx &c) { return *reinterpret_cast<Mem *>(c.mem); }

int32_t ufFind(int16_t *par, int32_t a) {
  while (par[a] != a) { par[a] = par[par[a]]; a = par[a]; }
  return a;
}
void ufUnion(int16_t *par, int32_t a, int32_t b) {
  a = ufFind(par, a); b = ufFind(par, b);
  if (a == b) return;
  if (a < b) par[b] = (int16_t)a; else par[a] = (int16_t)b;   // deterministic
}
}  // namespace

void weaveInit(void *st, JobCtx &ctx) {
  St &s = S(st);
  Mem &m = M(ctx);
  Rng &r = *ctx.famRng;
  const Palette &p = ctx.spec->pal;
  s.variant = ctx.spec->variant;
  static const int32_t kSizes[3][3] = { { 30, 34, 40 }, { 20, 24, 28 }, { 34, 40, 48 } };
  s.s = kSizes[s.variant][r.below(3)];
  s.cols = (kW + s.s - 1) / s.s;
  s.rows = (kH + s.s - 1) / s.s;
  s.nTiles = s.cols * s.rows;
  if (s.variant == 0)      s.hw = s.s * r.range(38, 50);        // ~0.15..0.2 s, Q8
  else if (s.variant == 1) s.hw = r.range(150, 210);
  else                     s.hw = s.s * r.range(52, 66);
  s.outline = s.variant == 1 ? 0 : r.range(220, 380);
  s.n0permille = r.range(150, 220);
  s.outlineColor = p.darkBg ? p.dark : scaleRGB(p.ink[2], 120);

  for (int32_t t = 0; t < s.nTiles; t++) m.orient[t] = (uint8_t)r.below(2);

  // Union-find over edge midpoints: horizontal edges first, then vertical.
  int32_t nH = (s.rows + 1) * s.cols;
  int32_t nV = s.rows * (s.cols + 1);
  for (int32_t i = 0; i < nH + nV; i++) m.parent[i] = (int16_t)i;
  for (int32_t rr = 0; rr < s.rows; rr++) {
    for (int32_t c = 0; c < s.cols; c++) {
      int32_t t = rr * s.cols + c;
      int32_t N = rr * s.cols + c, So = (rr + 1) * s.cols + c;
      int32_t Wp = nH + rr * (s.cols + 1) + c, E = Wp + 1;
      if (m.orient[t] == 0) { ufUnion(m.parent, N, Wp); ufUnion(m.parent, So, E); }
      else                  { ufUnion(m.parent, N, E);  ufUnion(m.parent, So, Wp); }
    }
  }
  uint32_t cseed = r.next();
  for (int32_t rr = 0; rr < s.rows; rr++) {
    for (int32_t c = 0; c < s.cols; c++) {
      int32_t t = rr * s.cols + c;
      int32_t N = rr * s.cols + c, So = (rr + 1) * s.cols + c;
      int32_t ra = ufFind(m.parent, N), rb = ufFind(m.parent, So);
      uint32_t ha = hash1(ra, cseed), hb = hash1(rb, cseed);
      m.colA[t] = (ha % 23 == 0) ? p.accent : p.ink[ha % 5];
      m.colB[t] = (hb % 23 == 0) ? p.accent : p.ink[hb % 5];
    }
  }

  // Growth order: distance from a seed point, roughened by noise.
  int32_t sx = r.range(70, 170);
  int32_t sy = r.range(110, 210);
  uint32_t nseed = r.next();
  for (int32_t rr = 0; rr < s.rows; rr++) {
    for (int32_t c = 0; c < s.cols; c++) {
      int32_t t = rr * s.cols + c;
      int32_t dx = c * s.s + s.s / 2 - sx, dy = rr * s.s + s.s / 2 - sy;
      int32_t d = (int32_t)isqrt32((uint32_t)(dx * dx + dy * dy));
      int32_t jitter = (int32_t)(hash1(t, nseed) % (uint32_t)(s.s * 2));
      m.order[t] = ((uint32_t)(d + jitter) << 10) | (uint32_t)t;
    }
  }
  sortKeys(m.order, s.nTiles);
}

int32_t weavePrepCount(const void *) { return kH / kBgRowsPerOp; }

void weavePrep(void *, JobCtx &ctx, int32_t i) {
  backgroundRows(ctx, i * kBgRowsPerOp, (i + 1) * kBgRowsPerOp, 2);
}

int32_t weaveElemCount(const void *st, int32_t g) {
  const St &s = S(st);
  int32_t n0 = s.nTiles * s.n0permille / 1000;
  return growCount(n0, s.nTiles, g);
}

void weaveElem(void *st, JobCtx &ctx, int32_t i) {
  St &s = S(st);
  Mem &m = M(ctx);
  if (i >= s.nTiles) return;
  const Palette &p = ctx.spec->pal;
  int32_t t = (int32_t)(m.order[i] & 1023u);
  int32_t c = t % s.cols, rr = t / s.cols;
  int32_t x0 = c * s.s, y0 = rr * s.s;
  // Arc centres (Q8, tile-relative) for the two bands.
  int32_t S8 = s.s * 256;
  int32_t ax, ay, bx, by;
  if (m.orient[t] == 0) { ax = 0;  ay = 0; bx = S8; by = S8; }
  else                  { ax = S8; ay = 0; bx = 0;  by = S8; }
  int32_t rad = S8 / 2;
  Canvas &cv = *ctx.cv;
  uint32_t light = p.light;
  for (int32_t y = 0; y < s.s; y++) {
    int32_t py = y0 + y;
    if (py >= cv.h) break;
    uint32_t *row = cv.px + py * cv.w;
    for (int32_t x = 0; x < s.s; x++) {
      int32_t px = x0 + x;
      if (px >= cv.w) break;
      uint32_t col = row[px];
      int32_t qx = x * 256 + 128, qy = y * 256 + 128;
      for (int32_t band = 0; band < 2; band++) {
        int32_t cx = band ? bx : ax, cy = band ? by : ay;
        int32_t dx = qx - cx, dy = qy - cy;
        int32_t d = (int32_t)isqrt64((uint64_t)((int64_t)dx * dx + (int64_t)dy * dy));
        int32_t off = d - rad;
        int32_t dist = iabs(off);
        uint32_t ink = band ? m.colB[t] : m.colA[t];
        if (s.variant == 1) {
          // Twin rails at +-s/7 around the centre line.
          int32_t railOff = S8 / 7;
          int32_t c1 = imin(imin(2 * s.hw, 256), s.hw + 128 - iabs(off - railOff));
          int32_t c2 = imin(imin(2 * s.hw, 256), s.hw + 128 - iabs(off + railOff));
          int32_t cov = imax(imax(c1, c2), 0);
          if (cov > 0) col = blend(col, ink, (uint32_t)cov);
          continue;
        }
        int32_t covO = imin(256, s.hw + s.outline + 128 - dist);
        if (covO > 0) col = blend(col, s.outlineColor, (uint32_t)(covO * 230 >> 8));
        int32_t cov = imin(256, s.hw + 128 - dist);
        if (cov <= 0) continue;
        uint32_t fill = ink;
        if (s.variant == 2) {
          // Tube shading: bright crest, darker flanks.
          int32_t f = 256 - (int32_t)(((int64_t)dist * 150) / imax(s.hw, 1));
          fill = scaleRGB(ink, iclamp(f + 40, 90, 300));
        } else {
          // Ribbon sheen along the centre line.
          int32_t sheen = imax(0, s.hw / 3 + 128 - iabs(off + s.hw / 4));
          if (sheen > 0) fill = blend(fill, light, (uint32_t)iclamp(sheen * 90 / 256, 0, 90));
        }
        col = blend(col, fill, (uint32_t)cov);
      }
      row[px] = col;
    }
  }
}

}  // namespace gf
