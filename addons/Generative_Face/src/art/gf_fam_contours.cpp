// CONTOURS — a topographic map of an imaginary terrain: stepped elevation
// tints with hillshade, marching-squares contour lines (every fourth one
// heavier), and a dashed trail. The map is fixed for the day; the trail is
// the growth: every 250 steps extend your walk across the map, with a small
// waypoint every so often. Variants: ISLANDS (sea and land), TERRACE (bold
// stepped bands), NIGHT MAP (glowing contour lines on dark).
#include "gf_family.h"

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

namespace gf {

namespace {
static const int32_t kG = 3;                         // grid step, px
static const int32_t kGW = kW / kG + 1;              // 81
static const int32_t kGH = kH / kG + 2;              // 95

struct St {
  uint8_t  variant;
  uint32_t hSeed, trailSeed;
  int32_t  scaleQ16, octaves;
  int32_t  hMin, hMax;
  int32_t  levels;
  int32_t  sea;                 // permille of the height range
  int32_t  relief;
  uint32_t ramp[6];
  uint32_t water0, water1;
  uint32_t lineColor;
  int32_t  lineAlpha;
  int32_t  tx, ty, tCount;
  uint16_t tAng;
  uint32_t trailColor, trailShadow;
  int32_t  n0, n1;
};
inline St &S(void *p) { return *static_cast<St *>(p); }
inline const St &S(const void *p) { return *static_cast<const St *>(p); }
inline int32_t *grid(JobCtx &c) { return reinterpret_cast<int32_t *>(c.mem); }

static const int32_t kGridRowsPerOp = 8;
static const int32_t kGridOps = (kGH + kGridRowsPerOp - 1) / kGridRowsPerOp;  // 12
static const int32_t kFillRowsPerOp = 14;
static const int32_t kFillOps = kH / kFillRowsPerOp;                           // 20
static const int32_t kLineRowsPerOp = 8;
static const int32_t kLineOps = (kGH - 1 + kLineRowsPerOp - 1) / kLineRowsPerOp; // 12

// Elevation in permille of the day's range at Q8 pixel position.
int32_t heightAt(const St &s, const int32_t *g, int32_t x, int32_t y, int32_t &gx, int32_t &gy) {
  int32_t fx = x / kG, fy = y / kG;                  // pixel -> cell
  int32_t i = iclamp(fx, 0, kGW - 2), j = iclamp(fy, 0, kGH - 2);
  int32_t tx = ((x - i * kG) * 65536) / kG, ty = ((y - j * kG) * 65536) / kG;
  int32_t h00 = g[j * kGW + i], h10 = g[j * kGW + i + 1];
  int32_t h01 = g[(j + 1) * kGW + i], h11 = g[(j + 1) * kGW + i + 1];
  int32_t a = h00 + (int32_t)(((int64_t)(h10 - h00) * tx) >> 16);
  int32_t b = h01 + (int32_t)(((int64_t)(h11 - h01) * tx) >> 16);
  int32_t h = a + (int32_t)(((int64_t)(b - a) * ty) >> 16);
  gx = ((h10 - h00) + (h11 - h01)) / 2;
  gy = ((h01 - h00) + (h11 - h10)) / 2;
  int32_t range = imax(s.hMax - s.hMin, 1);
  return (int32_t)(((int64_t)(h - s.hMin) * 1000) / range);
}
}  // namespace

void contoursInit(void *st, JobCtx &ctx) {
  St &s = S(st);
  Rng &r = *ctx.famRng;
  const Palette &p = ctx.spec->pal;
  s.variant = ctx.spec->variant;
  s.hSeed = r.next();
  s.trailSeed = r.next();
  s.scaleQ16 = 65536 / r.range(90, 170);
  s.octaves = r.range(4, 5);
  s.hMin = 0x7FFFFFFF; s.hMax = -0x7FFFFFFF;
  s.levels = (s.variant == 1) ? r.range(9, 13) : r.range(12, 18);
  s.sea = r.range(330, 470);
  s.relief = r.range(10, 16);
  // Elevation ramp: one hue drifting to a second, stepping in lightness
  // (light maps darken uphill, dark maps brighten uphill).
  int32_t hLow = p.baseHue + r.range(-10, 10);
  int32_t hHigh = hLow + ((p.scheme == kAnalogous || p.scheme == kMonochrome) ? 40 : 120);
  int32_t sat = r.range(300, 600);
  for (int32_t k = 0; k < 6; k++) {
    int32_t h = hLow + (hHigh - hLow) * k / 5;
    int32_t l = p.darkBg ? 120 + k * 110 : 880 - k * 95;
    s.ramp[k] = hsl(h, sat + k * 40, l);
  }
  int32_t wHue = hLow + 180 + r.range(-30, 30);
  s.water0 = p.darkBg ? hsl(wHue, 500, 90) : hsl(wHue, 450, 520);
  s.water1 = p.darkBg ? hsl(wHue, 450, 220) : hsl(wHue, 380, 760);
  s.lineColor = p.darkBg ? hsl(hLow, 300, 820) : hsl(hHigh, 450, 220);
  s.lineAlpha = (s.variant == 2) ? 230 : 110;
  s.trailColor = p.darkBg ? hsl(p.baseHue + 180, 900, 660) : hsl(p.baseHue + 180, 850, 290);
  s.trailShadow = p.darkBg ? hsl(hLow, 400, 60) : hsl(hLow, 300, 975);
  s.tx = r.range(70, 170) * 256;
  s.ty = r.range(90, 190) * 256;
  s.tAng = r.angle();
  s.tCount = 0;
  s.n0 = 4;
  s.n1 = 96;
}

int32_t contoursPrepCount(const void *) { return kGridOps + kFillOps + kLineOps; }

void contoursPrep(void *st, JobCtx &ctx, int32_t i) {
  St &s = S(st);
  const Palette &p = ctx.spec->pal;
  Canvas &cv = *ctx.cv;
  int32_t *g = grid(ctx);
  if (i < kGridOps) {
    for (int32_t j = i * kGridRowsPerOp; j < imin((i + 1) * kGridRowsPerOp, kGH); j++) {
      for (int32_t k = 0; k < kGW; k++) {
        int32_t nx = (int32_t)(((int64_t)(k * kG) * s.scaleQ16));
        int32_t ny = (int32_t)(((int64_t)(j * kG) * s.scaleQ16));
        int32_t h = fbm2(nx, ny, s.hSeed, s.octaves);
        g[j * kGW + k] = h;
        if (h < s.hMin) s.hMin = h;
        if (h > s.hMax) s.hMax = h;
      }
    }
    return;
  }
  i -= kGridOps;
  if (i < kFillOps) {
    int32_t y0 = i * kFillRowsPerOp, y1 = y0 + kFillRowsPerOp;
    if (s.variant == 2) { backgroundRows(ctx, y0, y1, 2); return; }
    uint32_t grainSeed = (uint32_t)(ctx.spec->seed >> 20);
    for (int32_t y = y0; y < y1; y++) {
      uint32_t *row = cv.px + y * cv.w;
      for (int32_t x = 0; x < kW; x++) {
        int32_t gx, gy;
        int32_t h = heightAt(s, g, x, y, gx, gy);
        uint32_t c;
        int32_t shadeOff = (int32_t)(((int64_t)gx + gy) * s.relief >> 11);   // light from the top left
        if (s.variant == 0 && h < s.sea) {
          int32_t depth = (h * 256) / imax(s.sea, 1);             // 0 deep .. 256 shore
          c = blend(s.water0, s.water1, (uint32_t)iclamp(depth, 0, 256));
          shadeOff /= 4;
        } else {
          int32_t lo = (s.variant == 0) ? s.sea : 0;
          int32_t t = ((h - lo) * 1000) / imax(1000 - lo, 1);      // 0..1000 over land
          int32_t band = iclamp(t * s.levels / 1000, 0, s.levels - 1);
          int32_t pos = band * 5 * 256 / imax(s.levels - 1, 1);   // ramp position Q8
          int32_t k = iclamp(pos >> 8, 0, 4);
          c = blend(s.ramp[k], s.ramp[k + 1], (uint32_t)(pos & 255));
        }
        int32_t f = iclamp(256 + shadeOff, 170, 330);
        c = scaleRGB(c, f);
        uint32_t n = hash2(x, y, grainSeed) & 3;
        if (n == 0) c = scaleRGB(c, 250);
        row[x] = c;
      }
    }
    return;
  }
  i -= kFillOps;
  // Contour lines by marching squares over grid rows [j0, j1).
  int32_t j0 = i * kLineRowsPerOp, j1 = imin(j0 + kLineRowsPerOp, kGH - 1);
  int32_t range = imax(s.hMax - s.hMin, 1);
  for (int32_t j = j0; j < j1; j++) {
    for (int32_t k = 0; k < kGW - 1; k++) {
      int32_t v[4] = { g[j * kGW + k], g[j * kGW + k + 1], g[(j + 1) * kGW + k + 1],
                       g[(j + 1) * kGW + k] };              // tl, tr, br, bl
      int32_t lo = imin(imin(v[0], v[1]), imin(v[2], v[3]));
      int32_t hi = imax(imax(v[0], v[1]), imax(v[2], v[3]));
      int32_t l0 = (int32_t)(((int64_t)(lo - s.hMin) * s.levels) / range) + 1;
      int32_t l1 = (int32_t)(((int64_t)(hi - s.hMin) * s.levels) / range);
      for (int32_t L = imax(l0, 1); L <= imin(l1, s.levels - 1); L++) {
        int32_t iso = s.hMin + (int32_t)(((int64_t)range * L) / s.levels);
        if (s.variant == 0) {
          int32_t seaH = s.hMin + (int32_t)(((int64_t)range * s.sea) / 1000);
          if (iso < seaH - range / 40) continue;           // no lines under water
        }
        int32_t code = (v[0] > iso ? 8 : 0) | (v[1] > iso ? 4 : 0) |
                       (v[2] > iso ? 2 : 0) | (v[3] > iso ? 1 : 0);
        if (code == 0 || code == 15) continue;
        // Edge crossing points (Q8 px): 0 top, 1 right, 2 bottom, 3 left.
        int32_t ex[4], ey[4];
        int32_t X = k * kG * 256, Y = j * kG * 256, G8 = kG * 256;
        auto lerpT = [&](int32_t a, int32_t b) -> int32_t {
          int32_t den = b - a;
          if (den == 0) return 128;
          return iclamp((int32_t)(((int64_t)(iso - a) * 256) / den), 0, 256);
        };
        ex[0] = X + lerpT(v[0], v[1]) * G8 / 256; ey[0] = Y;
        ex[1] = X + G8; ey[1] = Y + lerpT(v[1], v[2]) * G8 / 256;
        ex[2] = X + lerpT(v[3], v[2]) * G8 / 256; ey[2] = Y + G8;
        ex[3] = X; ey[3] = Y + lerpT(v[0], v[3]) * G8 / 256;
        int32_t segs[4], nseg = 0;
        switch (code) {
          case 1: case 14: segs[0] = 3; segs[1] = 2; nseg = 1; break;
          case 2: case 13: segs[0] = 2; segs[1] = 1; nseg = 1; break;
          case 3: case 12: segs[0] = 3; segs[1] = 1; nseg = 1; break;
          case 4: case 11: segs[0] = 0; segs[1] = 1; nseg = 1; break;
          case 6: case 9:  segs[0] = 0; segs[1] = 2; nseg = 1; break;
          case 7: case 8:  segs[0] = 3; segs[1] = 0; nseg = 1; break;
          case 5: case 10: {
            int32_t centre = (v[0] + v[1] + v[2] + v[3]) / 4;
            bool cHigh = centre > iso;
            if ((code == 5) == cHigh) { segs[0] = 3; segs[1] = 0; segs[2] = 2; segs[3] = 1; }
            else                      { segs[0] = 0; segs[1] = 1; segs[2] = 3; segs[3] = 2; }
            nseg = 2;
            break;
          }
          default: break;
        }
        bool major = (L % 4) == 0;
        int32_t hw = major ? 200 : 105;
        uint32_t color = s.lineColor;
        int32_t alpha = s.lineAlpha;
        if (s.variant == 2) {
          int32_t pos = L * 5 * 256 / s.levels;
          int32_t kk = iclamp(pos >> 8, 0, 4);
          color = ctx.spec->pal.ink[4 - kk];
          if (major) alpha = 255;
        } else if (major) {
          alpha = imin(alpha + 70, 255);
        }
        for (int32_t q = 0; q < nseg; q++) {
          int32_t a = segs[2 * q], b = segs[2 * q + 1];
          if (s.variant == 2 && major)
            segment(cv, ex[a], ey[a], ex[b], ey[b], 520, 520, color, 40, false, false);
          segment(cv, ex[a], ey[a], ex[b], ey[b], hw, hw, color, (uint32_t)alpha, false, false);
        }
      }
    }
  }
  (void)p;
}

int32_t contoursElemCount(const void *st, int32_t g) {
  const St &s = S(st);
  return growCount(s.n0, s.n1, g);
}

void contoursElem(void *st, JobCtx &ctx, int32_t i) {
  St &s = S(st);
  Canvas &cv = *ctx.cv;
  const int32_t *g = grid(ctx);
  // One dash of the trail: 4 drawn sub-steps then a 3-step gap. The path
  // wanders gently, prefers lower ground (so it threads along valleys like
  // a footpath) and turns back from the edges.
  int32_t xy[10];
  int32_t n = 0;
  for (int32_t k = 0; k < 7; k++) {
    if (k < 5) { xy[2 * n] = s.tx; xy[2 * n + 1] = s.ty; n++; }
    int32_t wv = noise1(s.tCount * 1700, s.trailSeed);
    s.tAng = (uint16_t)(s.tAng + (wv >> 7));
    // Downhill preference from the height grid.
    int32_t gx, gy;
    int32_t px = iclamp(s.tx >> 8, 0, kW - 1), py = iclamp(s.ty >> 8, 0, kH - 1);
    heightAt(s, g, px, py, gx, gy);
    int64_t cross = (int64_t)icos(s.tAng) * (-gy) - (int64_t)isin(s.tAng) * (-gx);
    int32_t turn = (int32_t)iclamp((int32_t)(cross >> 18), -260, 260);
    s.tAng = (uint16_t)(s.tAng + turn);
    int32_t mx = 30 * 256;
    if (s.tx < mx || s.ty < mx || s.tx > kW * 256 - mx || s.ty > kH * 256 - mx) {
      int32_t cx = kW * 128 - s.tx, cy = kH * 128 - s.ty;
      int64_t cr = (int64_t)icos(s.tAng) * cy - (int64_t)isin(s.tAng) * cx;
      s.tAng = (uint16_t)(s.tAng + (cr > 0 ? 1000 : -1000));
    }
    s.tx += (int32_t)(((int64_t)icos(s.tAng) * 540) >> 14);
    s.ty += (int32_t)(((int64_t)isin(s.tAng) * 540) >> 14);
    s.tCount++;
  }
  polyline(cv, xy, n, nullptr, 560, s.trailShadow, 150);
  polyline(cv, xy, n, nullptr, 250, s.trailColor, 255);
  if (i == 0) {
    arc(cv, xy[0], xy[1], 4 * 256, 180, 0, 65536, s.trailColor, 255);
  } else if (i % 24 == 0) {
    disc(cv, xy[0], xy[1], 520, s.trailShadow, 160);
    disc(cv, xy[0], xy[1], 380, s.trailColor, 255);
  }
}

}  // namespace gf
