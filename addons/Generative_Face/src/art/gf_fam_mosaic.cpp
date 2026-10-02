// MOSAIC — a Voronoi window over jittered-grid sites. The whole window is
// laid in shadow first; each growth element lights one cell, so walking
// fills the glass with colour. Variants: CATHEDRAL (jewel glass in dark
// leading), TILEWORK (glazed tiles set in pale grout), CRACKLE (tonal glaze
// with fine bright cracks).
#include "gf_family.h"

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

namespace gf {

namespace {
static const int32_t kMaxSites = 256;

struct St {
  uint8_t  variant;
  int32_t  cell;               // px
  int32_t  gx, gy, nSites;
  int32_t  leadHw;             // Q8, 0 = none
  uint32_t leadColor;
  uint32_t binder;
  int32_t  n0permille;
  uint32_t texSeed;
};
inline St &S(void *p) { return *static_cast<St *>(p); }
inline const St &S(const void *p) { return *static_cast<const St *>(p); }

struct Mem {
  int32_t  sx[kMaxSites], sy[kMaxSites];          // Q8
  uint32_t lit[kMaxSites], unlit[kMaxSites];
  int16_t  bx0[kMaxSites], by0[kMaxSites], bx1[kMaxSites], by1[kMaxSites];
  uint32_t order[kMaxSites];
  uint8_t  owner[kW * kH];
  uint8_t  edge[kW * kH];                          // distance to leading, Q2 px
};
inline Mem &M(JobCtx &c) { return *reinterpret_cast<Mem *>(c.mem); }

static const int32_t kRowsPerOp = 14;

// Final colour of one pixel of cell k.
uint32_t shadePixel(const St &s, const Mem &m, int32_t k, int32_t x, int32_t y,
                    bool lit, int32_t edgeQ2) {
  uint32_t c = lit ? m.lit[k] : m.unlit[k];
  int32_t e = edgeQ2 * 64;                                 // Q8 px
  uint32_t h = hash2(x, y, s.texSeed);
  if (s.variant == 1) {
    // Tilework: glazed tiles with a soft sheen, set in pale grout.
    if (lit) {
      int32_t f = 232 + imin(e / 50, 40);
      c = scaleRGB(c, f);
      if ((h & 127) == 0) c = blend(c, 0xFFFFFF, 90);
    }
    int32_t gap = imin(256, 230 + 128 - e);
    if (gap > 0) c = blend(c, s.binder, (uint32_t)gap);
    if (!lit && (h & 15) == 0) c = scaleRGB(c, 240);
    return c;
  }
  if (lit) {
    // Glass: glows in the middle of the cell, dims toward the leading.
    int32_t f = 215 + imin(e / 40, 75);
    c = scaleRGB(c, f);
    if ((h & 7) == 0) c = scaleRGB(c, 240);
  }
  if (s.leadHw > 0) {
    int32_t cov = imin(256, s.leadHw + 128 - e);
    if (cov > 0) c = blend(c, s.leadColor, (uint32_t)cov);
  }
  return c;
}
}  // namespace

void mosaicInit(void *st, JobCtx &ctx) {
  St &s = S(st);
  Mem &m = M(ctx);
  Rng &r = *ctx.famRng;
  const Palette &p = ctx.spec->pal;
  s.variant = ctx.spec->variant;
  if (s.variant == 0)      s.cell = r.range(23, 32);
  else if (s.variant == 1) s.cell = r.range(19, 24);
  else                     s.cell = r.range(26, 38);
  s.gx = kW / s.cell + 3;
  s.gy = kH / s.cell + 3;
  s.nSites = s.gx * s.gy;
  if (s.nSites > kMaxSites) s.nSites = kMaxSites;
  s.texSeed = r.next();
  s.n0permille = r.range(100, 180);
  s.binder = p.darkBg ? scaleRGB(p.bg1, 160) : blend(p.bg0, p.light, 120);
  if (s.variant == 0)      { s.leadHw = r.range(260, 360); s.leadColor = scaleRGB(p.dark, 90); }
  else if (s.variant == 1) { s.leadHw = 0; s.leadColor = s.binder; }
  else                     { s.leadHw = r.range(70, 120); s.leadColor = blend(p.light, p.ink[0], 70); }

  uint32_t regionSeed = r.next();
  int32_t regionScale = 65536 / r.range(70, 140);
  int32_t jit = s.cell * 256 * 2 / 5;
  for (int32_t j = 0; j < s.gy; j++) {
    for (int32_t i = 0; i < s.gx; i++) {
      int32_t k = j * s.gx + i;
      if (k >= kMaxSites) break;
      int32_t jx = r.range(-jit, jit);
      int32_t jy = r.range(-jit, jit);
      m.sx[k] = (i - 1) * s.cell * 256 + s.cell * 128 + jx;
      m.sy[k] = (j - 1) * s.cell * 256 + s.cell * 128 + jy;
      // Colour regions follow low-frequency noise so hues cluster.
      int32_t nx = (int32_t)(((int64_t)(m.sx[k] >> 8) * regionScale));
      int32_t ny = (int32_t)(((int64_t)(m.sy[k] >> 8) * regionScale));
      int32_t n = noise2(nx, ny, regionSeed);
      int32_t idx = iclamp((n + 65536) * 5 / 131073, 0, 4);
      uint32_t roll = r.below(1000);
      int32_t vary = r.range(200, 300);
      uint32_t base = (roll < 60) ? p.accent : p.ink[idx];
      if (s.variant == 1) {
        m.lit[k] = blend(scaleRGB(base, vary), p.bg0, p.darkBg ? 40 : 110);
        m.unlit[k] = s.binder;
      } else if (s.variant == 2) {
        uint32_t tone = blend(p.ink[0], p.ink[1], (uint32_t)((n + 65536) >> 9));
        m.lit[k] = scaleRGB(tone, vary - 30);
        m.unlit[k] = scaleRGB(tone, 70);
      } else {
        m.lit[k] = scaleRGB(base, vary);
        uint32_t grey = rgb((uint32_t)luma(base), (uint32_t)luma(base), (uint32_t)luma(base));
        m.unlit[k] = scaleRGB(blend(base, grey, 140), 72);
      }
      m.bx0[k] = 32767; m.by0[k] = 32767; m.bx1[k] = -1; m.by1[k] = -1;
      m.order[k] = (hash1(k, regionSeed) & 0x3FFFFFu) << 8 | (uint32_t)k;
    }
  }
  sortKeys(m.order, s.nSites);
}

int32_t mosaicPrepCount(const void *) { return kH / kRowsPerOp; }

void mosaicPrep(void *st, JobCtx &ctx, int32_t op) {
  St &s = S(st);
  Mem &m = M(ctx);
  Canvas &cv = *ctx.cv;
  for (int32_t y = op * kRowsPerOp; y < (op + 1) * kRowsPerOp; y++) {
    int32_t py = y * 256 + 128;
    int32_t cj = (y + s.cell) / s.cell;
    for (int32_t x = 0; x < kW; x++) {
      int32_t px = x * 256 + 128;
      int32_t ci = (x + s.cell) / s.cell;
      int32_t best = -1, second = -1, third = -1;
      int64_t bd = 0x7FFFFFFFFFFFLL, sd = bd, td = bd;
      for (int32_t dj = -1; dj <= 1; dj++) {
        int32_t jj = cj + dj;
        if (jj < 0 || jj >= s.gy) continue;
        for (int32_t di = -1; di <= 1; di++) {
          int32_t ii = ci + di;
          if (ii < 0 || ii >= s.gx) continue;
          int32_t k = jj * s.gx + ii;
          if (k >= s.nSites) continue;
          int64_t dx = px - m.sx[k], dy = py - m.sy[k];
          int64_t d2 = dx * dx + dy * dy;
          if (d2 < bd)      { td = sd; third = second; sd = bd; second = best; bd = d2; best = k; }
          else if (d2 < sd) { td = sd; third = second; sd = d2; second = k; }
          else if (d2 < td) { td = d2; third = k; }
        }
      }
      if (best < 0) best = 0;
      // Distance to the nearest bisector (checked against the next two).
      int32_t edge = 255 * 64;
      int32_t cands[2] = { second, third };
      for (int32_t q = 0; q < 2; q++) {
        int32_t b = cands[q];
        if (b < 0) continue;
        int64_t ax = m.sx[best], ay = m.sy[best], bx = m.sx[b], by = m.sy[b];
        int64_t db2 = (px - bx) * (px - bx) + (py - by) * (py - by);
        int64_t num = db2 - bd;
        int64_t ab = isqrt64((uint64_t)((ax - bx) * (ax - bx) + (ay - by) * (ay - by)));
        if (ab < 1) continue;
        int32_t e = (int32_t)(num / (2 * ab));                    // Q8 px
        if (e < edge) edge = e;
      }
      int32_t eq2 = iclamp(edge / 64, 0, 255);
      int32_t idx = y * kW + x;
      m.owner[idx] = (uint8_t)best;
      m.edge[idx] = (uint8_t)eq2;
      if (x < m.bx0[best]) m.bx0[best] = (int16_t)x;
      if (x > m.bx1[best]) m.bx1[best] = (int16_t)x;
      if (y < m.by0[best]) m.by0[best] = (int16_t)y;
      if (y > m.by1[best]) m.by1[best] = (int16_t)y;
      cv.px[idx] = shadePixel(s, m, best, x, y, false, eq2);
    }
  }
}

int32_t mosaicElemCount(const void *st, int32_t g) {
  const St &s = S(st);
  return growCount(s.nSites * s.n0permille / 1000, s.nSites, g);
}

void mosaicElem(void *st, JobCtx &ctx, int32_t i) {
  St &s = S(st);
  Mem &m = M(ctx);
  if (i >= s.nSites) return;
  int32_t k = (int32_t)(m.order[i] & 0xFFu);
  if (m.bx1[k] < 0) return;                      // cell lies outside the frame
  Canvas &cv = *ctx.cv;
  for (int32_t y = m.by0[k]; y <= m.by1[k]; y++) {
    for (int32_t x = m.bx0[k]; x <= m.bx1[k]; x++) {
      int32_t idx = y * kW + x;
      if (m.owner[idx] != k) continue;
      cv.px[idx] = shadePixel(s, m, k, x, y, true, m.edge[idx]);
    }
  }
}

}  // namespace gf
