// Tilt Parallax — drawing primitives for the procedural scenes.
//
// Generators work in *screen* coordinates (where a pixel sits at zero
// parallax); the Painter converts to layer coordinates, which include the
// horizontal overscan. Coverage is accumulated "over" (painter's algorithm):
// the pixel keeps the index of whatever covers more than half of it, and the
// alpha plane holds total coverage, so only silhouette edges against the
// layers behind are anti-aliased.
#pragma once
#include <stdint.h>
#include "tp_layer.h"
#include "tp_noise.h"
#include "tp_shade.h"

namespace tp {

struct Painter {
  Layer &L;
  explicit Painter(Layer &l) : L(l) {}

  int   lx(float sx) const { return (int)floorf(sx) - L.x0; }
  int   ly(float sy) const { return (int)floorf(sy) - L.y0; }
  float sx(int lx) const { return (float)(lx + L.x0); }
  float sy(int ly) const { return (float)(ly + L.y0); }
  bool  in(int x, int y) const { return x >= 0 && y >= 0 && x < L.w && y < L.h; }

  // Layer-space pixel write with coverage 0..1.
  void put(int x, int y, uint8_t ix, float cov) {
    if (cov <= 0.0f || !in(x, y)) return;
    size_t i = (size_t)y * L.w + x;
    if (!L.alpha) {
      if (cov >= 0.5f) L.idx[i] = ix;
      return;
    }
    float old = L.alpha[i] * (1.0f / 255.0f);
    float na = cov + old * (1.0f - cov);
    if (cov >= 0.5f || L.idx[i] == 0) L.idx[i] = ix;
    L.alpha[i] = (uint8_t)(na * 255.0f + 0.5f);
  }
  // Screen-space convenience.
  void putS(float sxp, float syp, uint8_t ix, float cov) { put(lx(sxp), ly(syp), ix, cov); }
  uint8_t idxAt(int x, int y) const { return in(x, y) ? L.idx[(size_t)y * L.w + x] : 0; }
};

// Linear interpolation into a per-column array (layer columns).
static inline float sampleCol(const float *a, int n, float x) {
  if (x <= 0) return a[0];
  if (x >= n - 1) return a[n - 1];
  int i = (int)x;
  float f = x - i;
  return a[i] + (a[i + 1] - a[i]) * f;
}

// Callback choosing the palette index of a ground pixel. `col` is the layer
// column, `sy` the screen y, `below` = pixels below the ridge line.
typedef uint8_t (*GroundShader)(void *ctx, int col, float sy, float below);

// Fills everything below ridge[] (screen y per layer column) down to the
// bottom of the layer, anti-aliasing the ridge line with 4 sub-columns.
void fillBelowRidge(Painter &P, const float *ridge, GroundShader shade, void *ctx);

// Pine tree silhouette. Left half uses matL, right half matR (slope
// materials, so the sun side lights up). `level` 0..1 shifts the shade.
void drawPine(Painter &P, float cx, float baseY, float height, float width,
              int matL, int matR, float level, uint32_t seed);

// Rounded deciduous / bush crown made of overlapping discs.
void drawBush(Painter &P, float cx, float baseY, float radius, int matL, int matR,
              float level, uint32_t seed);

// Grass blade rising from (bx, by), tapering, bending sideways by `bend` px.
void drawBlade(Painter &P, float bx, float by, float h, float w, float bend,
               int mat, float levelBase, float levelTip);

// Boulder: a squashed, lumpy ellipse lit from the top-left/right.
void drawBoulder(Painter &P, float cx, float baseY, float rw, float rh, int matL, int matR,
                 uint32_t seed);

// Filled anti-aliased disc in one index.
void drawDisc(Painter &P, float cx, float cy, float r, uint8_t ix);

// Axis-aligned filled rectangle (screen space), optional anti-aliased edges.
void fillRect(Painter &P, float x0, float y0, float x1, float y1, uint8_t ix);

// Anti-aliased line with round caps (radius r), screen space.
void drawCapsule(Painter &P, float x0, float y0, float x1, float y1, float r, uint8_t ix);

// Filled polygon (any shape, even-odd), 4x4 supersampled edges. `ix` may be
// chosen per pixel through `shade` (pass nullptr to use `ix` everywhere).
typedef uint8_t (*PolyShader)(void *ctx, float sx, float sy);
void fillPolygon(Painter &P, const float *xy, int n, uint8_t ix,
                 PolyShader shade = nullptr, void *ctx = nullptr);

}  // namespace tp
