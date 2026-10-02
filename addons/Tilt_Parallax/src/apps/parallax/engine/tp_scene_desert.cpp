// Desert: stratified mesas on the horizon, a sandstone arch, sharp-crested
// dunes whose lit and shaded flanks swap between morning and evening, and
// saguaro cacti with scrub at the glass. Clear desert nights get the densest
// star field of any scene.
#include "tp_scene.h"
#include "tp_terrain.h"
#include "tp_platform.h"
#include "tp_color.h"
#include <math.h>

namespace tp {

namespace {

struct Mesa { float cx, halfTop, top, steep, talus; };

// Mesa silhouette: flat top, steep cliff, gentler talus apron at the base.
float mesaY(const Mesa &m, float x, float groundY) {
  float d = fabsf(x - m.cx) - m.halfTop;
  if (d <= 0) return m.top;
  float cliffH = (groundY - m.top) * 0.62f;
  float cliffW = cliffH / m.steep;
  if (d <= cliffW) return m.top + d * m.steep;
  return m.top + cliffH + (d - cliffW) * m.talus;
}

struct MesaCtx { Painter *P; const Mesa *mesas; int n; float groundY; uint32_t seed; };

uint8_t mesaShade(void *v, int col, float sy, float below) {
  MesaCtx &c = *(MesaCtx *)v;
  float x = c.P->sx(col) + 0.5f;
  // the mesa this pixel belongs to: the one whose silhouette is highest here
  int best = 0;
  float by = 1e9f;
  for (int i = 0; i < c.n; i++) {
    float y = mesaY(c.mesas[i], x, c.groundY);
    if (y < by) { by = y; best = i; }
  }
  const Mesa &m = c.mesas[best];
  float d = x - m.cx;
  float cliffBottom = m.top + (c.groundY - m.top) * 0.62f;
  // cap rock: a darker band just under the flat top
  if (sy < m.top + 3.0f) return packIdxF(2, 0.35f);
  bool onCliff = sy < cliffBottom + 2.0f && fabsf(d) > m.halfTop * 0.2f;
  // strata: horizontal bands that wobble slightly
  float band = sinf(sy * 0.9f + 2.0f * noise1(x * 0.04f, c.seed + 3u)) * 0.5f + 0.5f;
  float lv = 0.45f + 0.3f * band * (onCliff ? 1.0f : 0.4f) - 0.12f * sat(below / 60.0f);
  if (onCliff && band > 0.82f) return packIdxF(2, sat(0.55f + 0.2f * band));
  float thr = ((bayer4((int)x, (int)sy) + 0.5f) / 16.0f - 0.5f) * 3.0f;
  return packIdxF(d + thr < 0 ? 0 : 1, sat(lv + (onCliff ? 0.25f : 0.0f)));
}

struct DuneCtx { Painter *P; const float *ridge; uint32_t seed; };

// Sharp-crested dunes: the facing flips exactly at each crest.
uint8_t duneShade(void *v, int col, float sy, float below) {
  DuneCtx &c = *(DuneCtx *)v;
  float x = c.P->sx(col) + 0.5f;
  int a = col > 1 ? col - 2 : 0, b = col < c.P->L.w - 2 ? col + 2 : c.P->L.w - 1;
  float slope = (c.ridge[b] - c.ridge[a]) / (float)(b - a);
  float t = sat(below / 34.0f);
  float rip = sinf(x * 0.75f + sy * 2.1f + 4.0f * noise1(x * 0.03f, c.seed + 1u));
  // gentle facets: steepness follows the actual slope near the crest and
  // relaxes toward the dune floor, so lit and shaded flanks stay related
  float lv = lerpf(0.18f + 0.30f * sat(fabsf(slope) * 1.2f), 0.10f, t) + 0.04f * rip;
  float thr = ((bayer4((int)x, (int)sy) + 0.5f) / 16.0f - 0.5f) * 0.12f;
  float f = lerpf(slope * 4.0f, fbm2(x * 0.02f, sy * 0.05f, c.seed + 4u, 2) * 1.5f, t * t);
  return packIdxF(f > thr ? 1 : 0, sat(lv));
}

void drawSaguaro(Painter &P, float x, float base, float h, float w, uint32_t seed) {
  Rng rng(seed);
  auto stroke = [&](float x0, float y0, float x1, float y1, float r) {
    // ribbed, cylinder-shaded capsule: left half / right half materials
    int bx0 = (int)floorf(fminf(x0, x1) - r - 1), bx1 = (int)ceilf(fmaxf(x0, x1) + r + 1);
    int by0 = (int)floorf(fminf(y0, y1) - r - 1), by1 = (int)ceilf(fmaxf(y0, y1) + r + 1);
    float vx = x1 - x0, vy = y1 - y0, l2 = vx * vx + vy * vy;
    for (int py = by0; py < by1; py++)
      for (int px = bx0; px < bx1; px++) {
        float wx = px + 0.5f - x0, wy = py + 0.5f - y0;
        float t = l2 > 0 ? (wx * vx + wy * vy) / l2 : 0;
        t = t < 0 ? 0 : (t > 1 ? 1 : t);
        float dx = wx - vx * t, dy = wy - vy * t;
        float d = sqrtf(dx * dx + dy * dy);
        float cov = sat(r + 0.5f - d);
        if (cov <= 0) continue;
        // across-stroke coordinate for ribs (vertical strokes: dx)
        float across = fabsf(vx) > fabsf(vy) ? dy : dx;
        float rib = 0.5f + 0.5f * cosf(across * 3.2f);
        float lv = sat(0.35f + 0.35f * fabsf(across) / (r + 0.01f) + 0.15f * rib);
        P.put(px - P.L.x0, py - P.L.y0, packIdxF(across < 0 ? 0 : 1, lv), cov);
      }
  };
  float r = w * 0.5f;
  stroke(x, base + 2, x, base - h + r, r);
  int arms = 1 + (int)(rng.unit() * 2.2f);
  for (int i = 0; i < arms; i++) {
    float side = (i % 2 == 0) ? (rng.chance(0.5f) ? -1.0f : 1.0f) : 0;
    if (side == 0) side = -1.0f;
    if (i == 1) side = -side;
    float ay = base - h * rng.range(0.35f, 0.6f);
    float reach = w * rng.range(1.0f, 1.5f);
    float up = h * rng.range(0.22f, 0.38f);
    float ar = r * 0.78f;
    stroke(x, ay, x + side * reach, ay, ar);
    stroke(x + side * reach, ay, x + side * reach, ay - up, ar);
  }
}

}  // namespace

bool buildDesert(Scene &s) {
  s.sky.horizonY = 188;
  s.sky.sea = false;
  s.sky.starDensity = 1.15f;
  s.sky.seed = 0xDE5Eu;
  s.skyBottom = 214;
  s.clouds = false;                     // clear desert sky
  int n = 0;

  // ---- 1. distant mesas --------------------------------------------------
  {
    Layer &L = s.layers[n];
    LayerLook &k = s.looks[n];
    if (!setupTerrainLayer(L, 0.80f, 112, 214, true, 16)) return false;
    k.mats[0] = Material(MAT_SLOPE_L, rgb(232, 136, 88));
    k.mats[1] = Material(MAT_SLOPE_R, rgb(232, 136, 88));
    k.mats[2] = Material(MAT_RAMP, rgb(206, 110, 76), rgb(112, 54, 40));
    k.mats[3] = Material(MAT_RAMP, rgb(200, 160, 120), rgb(120, 90, 70));
    k.haze = 0.24f; k.mist = 0.34f; k.nightFloor = 0.28f;
    Painter P(L);
    const float ground = 200.0f;
    Mesa mesas[] = {
      { -6, 20, 140, 3.2f, 0.45f },
      { 70, 12, 154, 3.0f, 0.5f },
      { 150, 30, 132, 3.6f, 0.42f },
      { 232, 16, 148, 2.8f, 0.5f },
    };
    const int nm = sizeof(mesas) / sizeof(mesas[0]);
    float *ridge = (float *)allocBig(sizeof(float) * L.w);
    if (!ridge) return false;
    for (int i = 0; i < L.w; i++) {
      float x = P.sx(i) + 0.5f, y = ground + 4.0f;
      for (int m = 0; m < nm; m++) y = fminf(y, mesaY(mesas[m], x, ground));
      ridge[i] = y + 0.8f * noise1(x * 0.5f, 5u);
    }
    MesaCtx c{ &P, mesas, nm, ground, 0x3E5Au };
    fillBelowRidge(P, ridge, mesaShade, &c);
    freeMem(ridge);
    n++;
  }
  // ---- 2. sandstone arch and buttes --------------------------------------
  {
    Layer &L = s.layers[n];
    LayerLook &k = s.looks[n];
    if (!setupTerrainLayer(L, 0.56f, 128, 230, true, 16)) return false;
    k.mats[0] = Material(MAT_SLOPE_L, rgb(208, 118, 78));
    k.mats[1] = Material(MAT_SLOPE_R, rgb(208, 118, 78));
    k.mats[2] = Material(MAT_RAMP, rgb(220, 160, 110), rgb(130, 70, 50));
    k.mats[3] = Material(MAT_RAMP, rgb(160, 150, 100), rgb(60, 60, 40));
    k.haze = 0.16f; k.mist = 0.30f; k.nightFloor = 0.2f;
    Painter P(L);
    // low ground ridge
    float *ridge = (float *)allocBig(sizeof(float) * L.w);
    if (!ridge) return false;
    for (int i = 0; i < L.w; i++) {
      float x = P.sx(i) + 0.5f;
      ridge[i] = 214.0f - 6.0f * fbm1(x * 0.02f, 0xA7Cu, 3);
    }
    struct G { Painter *P; } g{ &P };
    fillBelowRidge(P, ridge, [](void *v, int col, float sy, float below) -> uint8_t {
      Painter &PP = *((G *)v)->P;
      float x = PP.sx(col) + 0.5f;
      float lv = 0.3f + 0.15f * fbm2(x * 0.08f, sy * 0.2f, 77u, 2) - 0.1f * sat(below / 20.0f);
      return packIdxF(2, sat(lv));
    }, &g);
    // the arch: a massive, squarish sandstone fin with an opening punched
    // low through it (superellipses), edges roughened
    const float acx = 178.0f, acy = 192.0f, oax = 48.0f, oay = 52.0f;
    const float icx = 180.0f, icy = 206.0f, iax = 24.0f, iay = 30.0f;
    for (int py = (int)(acy - oay - 2); py < 216; py++)
      for (int px = (int)(acx - oax - 3); px < (int)(acx + oax + 3); px++) {
        int hits = 0;
        float shadeX = 0;
        for (int s2 = 0; s2 < 9; s2++) {
          float fx = px + ((s2 % 3) + 0.5f) / 3.0f, fy = py + ((s2 / 3) + 0.5f) / 3.0f;
          float rough = 1.0f + 0.05f * noise1(atan2f(fy - acy, fx - acx) * 6.0f, 0xA2Cu);
          float ux = fabsf(fx - acx) / oax, uy = fabsf(fy - acy) / oay;
          float o = ux * ux * ux + uy * uy * uy;                  // squarish outline
          float vx = fabsf(fx - icx) / iax, vy = fabsf(fy - icy) / iay;
          float in = vx * vx + vy * vy;
          if (o < rough && in > 1.0f && fy < 216.0f) { hits++; shadeX += fx - acx; }
        }
        if (!hits) continue;
        float rel = shadeX / hits / oax;
        float fy = py + 0.5f;
        float band = 0.5f + 0.5f * sinf(fy * 0.7f + 3.0f * noise1(px * 0.05f, 9u));
        // distance to the opening, for a shaded inner rim
        float vx = (px + 0.5f - icx) / iax, vy = (fy - icy) / iay;
        float inner = sqrtf(vx * vx + vy * vy) - 1.0f;
        uint8_t ix;
        if (fabsf(rel) > 0.70f) {
          // rounded outer flanks catch the sun on one side
          float thr = ((bayer4(px, py) + 0.5f) / 16.0f - 0.5f) * 0.12f;
          ix = packIdxF(rel + thr < 0 ? 0 : 1, sat(0.45f + 0.5f * (fabsf(rel) - 0.7f) / 0.3f));
        } else {
          float lv = 0.62f + 0.16f * band - 0.25f * sat(1.0f - inner * 5.0f) -
                     0.10f * sat((fy - (acy - oay)) / (2.0f * oay));
          ix = packIdxF(2, sat(lv));
        }
        P.put(px - L.x0, py - L.y0, ix, hits / 9.0f);
      }
    // a couple of hoodoo spires on the left
    const float hx[] = { 34.0f, 52.0f }, hh[] = { 44.0f, 30.0f }, hw[] = { 6.0f, 5.0f };
    for (int i = 0; i < 2; i++) {
      for (int py = (int)(212 - hh[i]); py < 214; py++) {
        float u = (212.0f - py) / hh[i];
        float w = hw[i] * (1.0f - 0.35f * u) * (u > 0.85f ? 1.35f : 1.0f);   // capped top
        for (int px = (int)(hx[i] - w - 2); px <= (int)(hx[i] + w + 2); px++) {
          float cov = sat(fminf(px + 1.0f, hx[i] + w) - fmaxf((float)px, hx[i] - w));
          if (cov <= 0) continue;
          float rel = (px + 0.5f - hx[i]) / w;
          float band = 0.5f + 0.5f * sinf(py * 0.8f);
          P.put(px - L.x0, py - L.y0,
                packIdxF(rel < 0 ? 0 : 1, sat(0.3f + 0.45f * fabsf(rel) + 0.1f * band)), cov);
        }
      }
    }
    freeMem(ridge);
    n++;
  }
  // ---- 3. dunes ------------------------------------------------------------
  {
    Layer &L = s.layers[n];
    LayerLook &k = s.looks[n];
    if (!setupTerrainLayer(L, 0.32f, 188, 266, true, 16)) return false;
    k.mats[0] = Material(MAT_SLOPE_L, rgb(236, 184, 124));
    k.mats[1] = Material(MAT_SLOPE_R, rgb(236, 184, 124));
    k.mats[2] = Material(MAT_RAMP, rgb(150, 150, 96), rgb(60, 64, 40));
    k.mats[3] = Material(MAT_RAMP, rgb(150, 150, 96), rgb(60, 64, 40));
    k.haze = 0.04f; k.mist = 0.10f; k.nightFloor = 0.15f;
    Painter P(L);
    float *ridge = (float *)allocBig(sizeof(float) * L.w);
    if (!ridge) return false;
    for (int i = 0; i < L.w; i++) {
      float x = P.sx(i) + 0.5f;
      // asymmetric dune profile: long gentle windward slope, short steep lee
      float ph = x * 0.013f + 1.1f * fbm1(x * 0.006f, 0xD1Au, 2);
      float f = ph - floorf(ph);
      float prof = f < 0.70f ? f / 0.70f : (1.0f - f) / 0.30f;
      float amp = 10.0f + 9.0f * (0.5f + 0.5f * noise1(floorf(ph) * 1.7f, 0xD1Du));
      ridge[i] = 230.0f - amp * powf(prof, 1.4f) - 3.0f * fbm1(x * 0.03f, 0xD1Bu, 2);
    }
    DuneCtx c{ &P, ridge, 0xD1Cu };
    fillBelowRidge(P, ridge, duneShade, &c);
    freeMem(ridge);
    n++;
  }
  // ---- 4. saguaros and scrub at the glass ------------------------------
  {
    Layer &L = s.layers[n];
    LayerLook &k = s.looks[n];
    if (!setupTerrainLayer(L, 0.06f, 150, 280, true, 1)) return false;
    k.mats[0] = Material(MAT_SLOPE_L, rgb(104, 136, 96));
    k.mats[1] = Material(MAT_SLOPE_R, rgb(104, 136, 96));
    k.mats[2] = Material(MAT_RAMP, rgb(222, 178, 122), rgb(110, 80, 56));
    k.mats[3] = Material(MAT_RAMP, rgb(170, 160, 104), rgb(56, 54, 36));
    k.nightFloor = 0.10f;
    Painter P(L);
    // sandy ground strip
    float *ridge = (float *)allocBig(sizeof(float) * L.w);
    if (!ridge) return false;
    for (int i = 0; i < L.w; i++) {
      float x = P.sx(i) + 0.5f;
      ridge[i] = 266.0f - 4.0f * fbm1(x * 0.03f, 0x5A5Au, 2) + 0.0005f * (x - 120) * (x - 120);
    }
    struct G2 { Painter *P; } g2{ &P };
    fillBelowRidge(P, ridge, [](void *v, int col, float sy, float below) -> uint8_t {
      Painter &PP = *((G2 *)v)->P;
      float x = PP.sx(col) + 0.5f;
      return packIdxF(2, sat(0.45f - 0.03f * below + 0.08f * fbm2(x * 0.2f, sy * 0.3f, 3u, 2)));
    }, &g2);
    drawSaguaro(P, 26.0f, 268.0f, 104.0f, 11.0f, 0x5A61u);
    drawSaguaro(P, 214.0f, 270.0f, 78.0f, 9.0f, 0x5A62u);
    drawSaguaro(P, 168.0f, 262.0f, 30.0f, 5.0f, 0x5A63u);
    Rng rng(0x5C2Bu);
    for (int i = 0; i < 70; i++) {          // dry scrub
      float x = rng.range(P.sx(0), P.sx(L.w - 1));
      float base = sampleCol(ridge, L.w, x - L.x0) + 2.0f;
      drawBlade(P, x, base, rng.range(3.0f, 10.0f), 1.2f, rng.range(-4.0f, 4.0f), 3, 0.2f,
                rng.range(0.5f, 0.9f));
    }
    for (int i = 0; i < 5; i++) {
      float x = rng.range(50, 200);
      drawBoulder(P, x, sampleCol(ridge, L.w, x - L.x0) + 3.0f, rng.range(4.0f, 8.0f),
                  rng.range(3.0f, 5.0f), 2, 2, 0x9000u + i);
    }
    freeMem(ridge);
    n++;
  }
  s.count = n;
  return true;
}

}  // namespace tp
