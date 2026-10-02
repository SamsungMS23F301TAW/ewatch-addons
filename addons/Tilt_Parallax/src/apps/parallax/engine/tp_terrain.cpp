#include "tp_terrain.h"
#include "tp_platform.h"
#include "tp_color.h"
#include <math.h>
#include <string.h>

namespace tp {

// ---------------------------------------------------------------------------
// Faceted mountain range
//
// The skyline is the lower envelope of "tent" massifs (one apex each, with
// different left/right flank slopes) plus a little midpoint-displacement
// roughness. Every visible apex sends a spur ridge down the face, and spurs
// branch into smaller side spurs. Each pixel belongs to the spur whose ridge
// line is "highest" there (a max of V-shaped height fields), and faces left
// or right depending on which side of that line it sits: flat-shaded facets
// that the sun lights from the east in the morning and the west at dusk.
// ---------------------------------------------------------------------------
namespace {
struct Spur {
  float x0, y0;      // start (screen)
  float slant;       // dx per dy
  float drift;       // wobble amplitude
  float offset;      // lower ridges lose to higher ones
  uint32_t seed;
};

struct RangeCtx {
  const RangeParams *p;
  Painter *P;
  const float *ridge;
  int n;
  float minR, maxR;
  int nSpurs;
  static const int kMaxSpurs = 64;
  Spur spurs[kMaxSpurs];
  // per-row cache of each spur's line x (filled lazily, one row at a time)
  int cacheRow;
  float lineX[kMaxSpurs];
};

void cacheRow(RangeCtx &c, int row, float sy) {
  if (c.cacheRow == row) return;
  c.cacheRow = row;
  for (int i = 0; i < c.nSpurs; i++) {
    const Spur &s = c.spurs[i];
    float dy = sy - s.y0;
    c.lineX[i] = s.x0 + s.slant * dy + s.drift * fbm1(dy * 0.05f, s.seed, 2);
  }
}

uint8_t rangeShade(void *vctx, int col, float sy, float below) {
  RangeCtx &c = *(RangeCtx *)vctx;
  const RangeParams &p = *c.p;
  float x = c.P->sx(col) + 0.5f;
  cacheRow(c, c.P->ly(sy), sy);
  float best = -1e9f;
  int win = -1;
  for (int i = 0; i < c.nSpurs; i++) {
    const Spur &s = c.spurs[i];
    if (sy < s.y0 - 1.0f) continue;
    float h = -fabsf(x - c.lineX[i]) - 0.45f * (sy - s.y0) - s.offset;
    if (h > best) { best = h; win = i; }
  }
  // A ~1.5 px ordered-dither band softens the facet edge at 1x.
  float thr = ((bayer4((int)x, (int)sy) + 0.5f) / 16.0f - 0.5f) * 1.5f;
  bool left = win >= 0 ? (x - c.lineX[win] < thr) : true;
  float dist = win >= 0 ? fabsf(x - c.lineX[win]) : 0.0f;
  // Steeper (brighter on the lit side) close to the ridge line, gentler
  // further out and lower down; a little striation texture on top.
  float steep = 0.86f - 0.30f * sat(dist / 30.0f) - 0.28f * sat(below / 90.0f) +
                0.10f * fbm2(x * 0.20f, sy * 0.06f, p.seed + 5u, 2);
  steep = sat(steep);
  bool snow = false;
  if (p.snow > 0 && below < p.snow * 48.0f) {   // max depth + max streak
    float peakness = (c.maxR - c.ridge[col]) / (c.maxR - c.minR + 1e-3f);  // 1 = highest
    float depth = p.snow * (2.0f + 22.0f * peakness * peakness);
    if (below < depth) {
      snow = true;
    } else {
      float streak = 20.0f * p.snow * peakness *
                     fmaxf(0.0f, fbm2(x * 0.16f, sy * 0.035f, p.seed + 21u, 3) + 0.15f);
      snow = below < depth + streak;
    }
  }
  int mat = snow ? (left ? p.matSnowL : p.matSnowR) : (left ? p.matRockL : p.matRockR);
  return packIdxF(mat, steep);
}
}  // namespace

void genRange(Painter &P, const RangeParams &p) {
  Layer &L = P.L;
  const int n = L.w;
  float *ridge = (float *)allocBig(sizeof(float) * n);
  float *rough = (float *)allocBig(sizeof(float) * n);
  RangeCtx *c = (RangeCtx *)allocBig(sizeof(RangeCtx));
  if (!ridge || !rough || !c) {
    if (ridge) freeMem(ridge);
    if (rough) freeMem(rough);
    if (c) freeMem(c);
    return;
  }
  Rng rng(p.seed);
  // ---- massifs: big tents spaced across the layer, plus smaller sub-peaks
  struct Tent { float x, y, sl, sr; };
  Tent tents[24];
  int nt = 0;
  const float xa = P.sx(0), xb = P.sx(n - 1);
  int big = 3 + (int)(rng.unit() * 2.0f);
  for (int i = 0; i < big && nt < 24; i++) {
    float u = (i + rng.range(0.25f, 0.75f)) / big;
    float x = xa + u * (xb - xa);
    float centre = 1.0f - fabsf(x - 120.0f) / 160.0f;
    float hgt = p.amp * (0.55f + 0.45f * rng.unit()) + p.peakBias * p.amp * centre;
    tents[nt++] = Tent{ x, p.baseY - hgt, rng.range(0.55f, 1.15f), rng.range(0.55f, 1.15f) };
  }
  int small = 4 + (int)(rng.unit() * 4.0f);
  for (int i = 0; i < small && nt < 24; i++) {
    float x = rng.range(xa, xb);
    float hgt = p.amp * rng.range(0.05f, 0.55f);
    tents[nt++] = Tent{ x, p.baseY - hgt, rng.range(0.6f, 1.3f), rng.range(0.6f, 1.3f) };
  }
  midpointRidge(rough, n, rng, p.rough, 1.0f);
  for (int i = 0; i < n; i++) {
    float x = P.sx(i) + 0.5f;
    float y = 1e9f;
    for (int t = 0; t < nt; t++) {
      float d = x - tents[t].x;
      float ty = tents[t].y + (d < 0 ? -d * tents[t].sl : d * tents[t].sr);
      if (ty < y) y = ty;
    }
    // roughness grows away from the apexes so summits stay crisp
    ridge[i] = y + p.detail * (rough[i] * 1.6f + fbm1(x * 0.18f, p.seed + 33u, 3));
  }
  RangeCtx &C = *c;
  C.p = &p; C.P = &P; C.ridge = ridge; C.n = n; C.cacheRow = -100000;
  C.minR = 1e9f; C.maxR = -1e9f;
  for (int i = 0; i < n; i++) { C.minR = fminf(C.minR, ridge[i]); C.maxR = fmaxf(C.maxR, ridge[i]); }
  // ---- spurs from every apex that actually shows on the skyline
  C.nSpurs = 0;
  for (int t = 0; t < nt && C.nSpurs < RangeCtx::kMaxSpurs; t++) {
    int col = (int)(tents[t].x - L.x0);
    if (col < 0 || col >= n) continue;
    if (ridge[col] > tents[t].y + p.detail * 3.0f + 3.0f) continue;   // buried
    float lean = (tents[t].sr - tents[t].sl) * 0.6f;                   // toward the gentler flank
    Spur s{ tents[t].x, ridge[col], lean + rng.range(-0.18f, 0.18f), p.spurDrift, 0.0f,
            p.seed + 1000u + (uint32_t)t };
    C.spurs[C.nSpurs++] = s;
    // side spurs branching outward lower down
    int branches = 1 + (int)(rng.unit() * 3.0f);
    for (int b = 0; b < branches && C.nSpurs < RangeCtx::kMaxSpurs; b++) {
      float dy = rng.range(10.0f, 46.0f);
      float side = rng.chance(0.5f) ? -1.0f : 1.0f;
      Spur q;
      q.y0 = s.y0 + dy;
      q.x0 = s.x0 + s.slant * dy;
      q.slant = s.slant + side * rng.range(0.55f, 1.25f);
      q.drift = p.spurDrift * 0.6f;
      q.offset = rng.range(2.0f, 7.0f);
      q.seed = p.seed + 2000u + (uint32_t)(t * 8 + b);
      C.spurs[C.nSpurs++] = q;
    }
  }
  if (C.nSpurs == 0) {
    int best = 0;
    for (int i = 1; i < n; i++) if (ridge[i] < ridge[best]) best = i;
    C.spurs[0] = Spur{ P.sx(best) + 0.5f, ridge[best], 0.0f, p.spurDrift, 0.0f, p.seed };
    C.nSpurs = 1;
  }
  fillBelowRidge(P, ridge, rangeShade, &C);
  freeMem(ridge);
  freeMem(rough);
  freeMem(c);
}

// ---------------------------------------------------------------------------
// Rolling hill
// ---------------------------------------------------------------------------
namespace {
struct HillCtx {
  const HillParams *p;
  Painter *P;
  const float *ridge;
  int n;
};
uint8_t hillShade(void *vctx, int col, float sy, float below) {
  HillCtx &c = *(HillCtx *)vctx;
  const HillParams &p = *c.p;
  int a = col > 2 ? col - 3 : 0, b = col < c.n - 3 ? col + 3 : c.n - 1;
  float slope = (c.ridge[b] - c.ridge[a]) / (float)(b - a > 0 ? b - a : 1);  // dy/dx
  float x = c.P->sx(col) + 0.5f;
  float t = sat(below / 26.0f);
  // Continuous facing: the crest's slope near the top, a smooth undulation
  // field (x-derivative of a soft height map) deeper down. Left/right is
  // picked against an ordered-dither threshold so the light/shade boundary
  // is a soft stipple rather than a hard seam.
  float g1 = noise2((x + 2.0f) * 0.018f, sy * 0.05f, p.seed + 13u);
  float g0 = noise2((x - 2.0f) * 0.018f, sy * 0.05f, p.seed + 13u);
  float f = lerpf(clampf(slope * 3.0f, -1.0f, 1.0f), clampf((g0 - g1) * 9.0f, -1.0f, 1.0f), t);
  float thr = ((bayer4((int)x, (int)sy) + 0.5f) / 16.0f - 0.5f) * 0.7f;
  bool right = f > thr;
  float steepCrest = sat(fabsf(slope) * 1.8f);
  float lv = lerpf(p.levelTop * (0.45f + 0.55f * steepCrest), p.levelDeep, t) +
             p.relief * fabsf(f) +
             p.texture * fbm2(x * p.texFreq, sy * p.texFreq * 1.6f, p.seed + 7u, 2);
  return packIdxF(right ? p.matR : p.matL, sat(lv));
}
}  // namespace

void genHill(Painter &P, const HillParams &p, float *ridge) {
  Layer &L = P.L;
  const int n = L.w;
  for (int i = 0; i < n; i++) {
    float sx = P.sx(i) + 0.5f;
    ridge[i] = p.baseY - p.amp * fbm1(sx * p.freq + 3.1f, p.seed, 3, 0.45f) -
               0.25f * p.amp * fbm1(sx * p.freq * 4.0f, p.seed + 1u, 2);
  }
  HillCtx c{ &p, &P, ridge, n };
  fillBelowRidge(P, ridge, hillShade, &c);
}

// ---------------------------------------------------------------------------
// Forest line
// ---------------------------------------------------------------------------
void genForest(Painter &P, const float *ridge, const ForestParams &p) {
  Layer &L = P.L;
  Rng rng(p.seed);
  float x = P.sx(0) - 4;
  float xEnd = P.sx(L.w - 1) + 4;
  while (x < xEnd) {
    x += p.spacing * rng.range(0.55f, 1.45f);
    if (!rng.chance(p.density)) continue;
    // clump heights with low-frequency noise so the canopy undulates
    float clump = 0.5f + 0.5f * fbm1(x * 0.035f, p.seed + 9u, 2);
    float h = lerpf(p.minH, p.maxH, sat(clump * 0.75f + rng.unit() * 0.45f));
    float col = x - L.x0;
    float base = sampleCol(ridge, L.w, col) + p.sink * rng.range(0.6f, 1.4f);
    drawPine(P, x, base, h, h * p.widthRatio * rng.range(0.85f, 1.15f), p.matL, p.matR,
             p.level + rng.range(-0.08f, 0.08f), p.seed + (uint32_t)(x * 13.0f));
  }
}

// ---------------------------------------------------------------------------
// Grass foreground
// ---------------------------------------------------------------------------
void genGrass(Painter &P, const GrassParams &p) {
  Layer &L = P.L;
  Rng rng(p.seed);
  // solid ground strip with a soft undulating top
  float *ridge = (float *)allocBig(sizeof(float) * L.w);
  if (!ridge) return;
  for (int i = 0; i < L.w; i++) {
    float sx = P.sx(i) + 0.5f;
    ridge[i] = p.groundY + 3.0f * fbm1(sx * 0.03f, p.seed + 1u, 2);
  }
  struct G { const GrassParams *p; } gctx{ &p };
  fillBelowRidge(P, ridge, [](void *v, int, float sy, float below) -> uint8_t {
    const GrassParams &gp = *((G *)v)->p;
    (void)sy;
    return packIdxF(gp.matGrass, sat(0.22f - below * 0.02f));
  }, &gctx);
  // blades, back to front by height so taller ones read in front
  const float x0 = P.sx(0), x1 = P.sx(L.w - 1);
  for (int i = 0; i < p.blades; i++) {
    float bx = rng.range(x0, x1);
    float edge = fabsf(bx - 120.0f) / 120.0f;              // 0 centre .. 1 edge
    float h = rng.range(p.minH, p.maxH) * (0.55f + 0.45f * rng.unit()) +
              p.edgeBoost * edge * edge * rng.unit();
    float by = sampleCol(ridge, L.w, bx - L.x0) + 2.0f;
    float bend = rng.range(-0.45f, 0.45f) * h + (bx < 120 ? -1.0f : 1.0f) * edge * 3.0f;
    float w = rng.range(1.5f, 2.8f);
    drawBlade(P, bx, by, h, w, bend, p.matGrass, rng.range(0.15f, 0.30f), rng.range(0.55f, 0.95f));
    if (rng.chance(p.flowers)) {
      float fx = bx + bend, fy = by - h;
      int mat = rng.chance(0.5f) ? p.matFlowerA : p.matFlowerB;
      float r = rng.range(1.3f, 2.2f);
      drawDisc(P, fx, fy, r, packIdxF(mat, rng.range(0.55f, 1.0f)));
      drawDisc(P, fx - 0.3f, fy - 0.4f, r * 0.45f, packIdxF(mat, 1.0f));
    }
  }
  freeMem(ridge);
}

}  // namespace tp
