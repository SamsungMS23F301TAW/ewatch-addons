// Coast: low islands on the horizon, a headland with a lighthouse whose lamp
// glows after dusk, sea stacks standing in the surf, marram-grass dunes, and
// beach grass with a weathered rope fence at the glass. The sea itself is
// part of the sky layer, so its glitter path follows the real sun and moon.
#include "tp_scene.h"
#include "tp_terrain.h"
#include "tp_platform.h"
#include "tp_color.h"
#include <math.h>

namespace tp {

namespace {

const float kHorizon = 164.0f;

// ---- islands on the horizon ----
struct IslandCtx { Painter *P; const float *ridge; };
uint8_t islandShade(void *v, int col, float sy, float below) {
  IslandCtx &c = *(IslandCtx *)v;
  int a = col > 1 ? col - 2 : 0, b = col < c.P->L.w - 2 ? col + 2 : c.P->L.w - 1;
  float slope = c.ridge[b] - c.ridge[a];
  float lv = 0.35f + 0.25f * sat(fabsf(slope) * 0.6f) - 0.2f * sat(below / 8.0f);
  (void)sy;
  return packIdxF(slope < 0 ? 0 : 1, sat(lv));
}

// ---- headland ----
struct HeadCtx { Painter *P; const float *ridge; float waterY; uint32_t seed; };
uint8_t headShade(void *v, int col, float sy, float below) {
  HeadCtx &c = *(HeadCtx *)v;
  float x = c.P->sx(col) + 0.5f;
  // foam where rock meets the sea: ragged top edge, varying thickness, and
  // a second broken wave line just below
  float foamTop = c.waterY - 1.0f - 2.2f * (0.5f + 0.5f * noise1(x * 0.21f, c.seed + 3u));
  if (sy > c.waterY + 3.5f) return 0;                       // under water: hole
  if (sy > c.waterY + 1.0f) {
    float w = noise1(x * 0.12f + 7.0f, c.seed + 4u);
    return w > 0.15f ? packIdxF(3, sat(0.5f + 0.4f * w)) : 0;
  }
  if (sy > foamTop) return packIdxF(3, sat(0.65f + 0.35f * noise1(x * 0.5f, c.seed + 6u)));
  // grassy cap with a ragged lower edge
  float cap = 4.0f + 2.5f * noise1(x * 0.21f, c.seed + 5u);
  if (below < cap) return packIdxF(2, sat(0.75f - below * 0.08f));
  // rock face: horizontal strata and vertical cracks
  float strata = fbm1(sy * 0.32f + 5.0f, c.seed + 7u, 2);
  float crack = fbm2(x * 0.30f, sy * 0.05f, c.seed + 9u, 2);
  int a = col > 2 ? col - 3 : 0, b = col < c.P->L.w - 3 ? col + 3 : c.P->L.w - 1;
  float slope = c.ridge[b] - c.ridge[a];
  bool right = slope > 2.5f || crack > 0.25f;               // the seaward flank faces right
  float lv = 0.55f + 0.18f * strata + 0.12f * crack - 0.15f * sat(below / 50.0f);
  return packIdxF(right ? 1 : 0, sat(lv));
}

// ---- dunes ----
struct DuneCtx { Painter *P; const float *ridge; uint32_t seed; };
uint8_t duneShade(void *v, int col, float sy, float below) {
  DuneCtx &c = *(DuneCtx *)v;
  float x = c.P->sx(col) + 0.5f;
  int a = col > 2 ? col - 3 : 0, b = col < c.P->L.w - 3 ? col + 3 : c.P->L.w - 1;
  float slope = (c.ridge[b] - c.ridge[a]) / (float)(b - a);
  float t = sat(below / 30.0f);
  // wind ripples
  float rip = sinf((x * 0.9f + sy * 2.4f) + 3.0f * noise1(x * 0.05f, c.seed + 1u));
  float lv = lerpf(0.55f + 0.3f * sat(fabsf(slope) * 2.0f), 0.18f, t) + 0.06f * rip;
  float thr = ((bayer4((int)x, (int)sy) + 0.5f) / 16.0f - 0.5f) * 0.12f;
  bool right = lerpf(slope * 3.0f, fbm2(x * 0.02f, sy * 0.05f, c.seed + 4u, 2) * 2.0f, t) > thr;
  return packIdxF(right ? 1 : 0, sat(lv));
}

}  // namespace

bool buildCoast(Scene &s) {
  s.sky.horizonY = (int16_t)kHorizon;
  s.sky.sea = true;
  s.sky.seaDeep = rgb(20, 62, 92);
  s.sky.starDensity = 1.0f;
  s.sky.seed = 0xC0A57u;
  s.skyBottom = 240;
  s.clouds = true;
  int n = 0;

  // ---- 1. islands sitting on the horizon (move with the sea) ------------
  {
    Layer &L = s.layers[n];
    LayerLook &k = s.looks[n];
    if (!setupTerrainLayer(L, 1.0f, 140, (int)kHorizon + 1, true, 1)) return false;
    L.clampBottom = false;
    k.mats[0] = Material(MAT_SLOPE_L, rgb(110, 128, 120));
    k.mats[1] = Material(MAT_SLOPE_R, rgb(110, 128, 120));
    k.haze = 0.58f; k.nightFloor = 0.25f;
    Painter P(L);
    float *ridge = (float *)allocBig(sizeof(float) * L.w);
    if (!ridge) return false;
    struct Isl { float cx, hw, ht; } isl[] = { { 186, 46, 9 }, { 236, 18, 5 }, { 44, 22, 4.5f } };
    for (int i = 0; i < L.w; i++) {
      float x = P.sx(i) + 0.5f;
      float y = kHorizon + 2.0f;                 // below the bottom = nothing
      for (const Isl &q : isl) {
        float u = (x - q.cx) / q.hw;
        if (fabsf(u) >= 1) continue;
        float h = q.ht * powf(1.0f - u * u, 0.55f) * (0.85f + 0.3f * fbm1(x * 0.08f, 71u, 2));
        y = fminf(y, kHorizon - h);
      }
      ridge[i] = y;
    }
    IslandCtx c{ &P, ridge };
    fillBelowRidge(P, ridge, islandShade, &c);
    freeMem(ridge);
    n++;
  }
  // ---- 2. headland --------------------------------------------------------
  const float waterY = 201.0f;
  float *headRidge = nullptr;
  {
    Layer &L = s.layers[n];
    LayerLook &k = s.looks[n];
    if (!setupTerrainLayer(L, 0.60f, 124, (int)waterY + 5, true, 1)) return false;
    L.clampBottom = false;
    k.mats[0] = Material(MAT_SLOPE_L, rgb(160, 138, 116));
    k.mats[1] = Material(MAT_SLOPE_R, rgb(160, 138, 116));
    k.mats[2] = Material(MAT_RAMP, rgb(132, 168, 86), rgb(52, 84, 40));
    k.mats[3] = Material(MAT_RAMP, rgb(250, 252, 255), rgb(170, 190, 200));
    k.haze = 0.16f; k.nightFloor = 0.2f;
    Painter P(L);
    headRidge = (float *)allocBig(sizeof(float) * L.w);
    if (!headRidge) return false;
    for (int i = 0; i < L.w; i++) {
      float x = P.sx(i) + 0.5f;
      float y;
      if (x < 118) y = 134.0f + 0.17f * (x + 40.0f) + 2.5f * fbm1(x * 0.05f, 31u, 3);
      else y = 134.0f + 0.17f * 158.0f + (x - 118.0f) * 2.4f + 2.0f * noise1(x * 0.3f, 37u);
      headRidge[i] = fminf(y, waterY + 6.0f);
    }
    HeadCtx c{ &P, headRidge, waterY, 0xBEAC4u };
    fillBelowRidge(P, headRidge, headShade, &c);
    n++;
  }
  // ---- 3. lighthouse and keeper's cottage (same depth as the headland) ----
  {
    Layer &L = s.layers[n];
    LayerLook &k = s.looks[n];
    if (!setupTerrainLayer(L, 0.60f, 98, 160, true, 1)) {
      if (headRidge) freeMem(headRidge);
      return false;
    }
    L.clampBottom = false;
    k.mats[0] = Material(MAT_SLOPE_L, rgb(246, 242, 234));
    k.mats[1] = Material(MAT_SLOPE_R, rgb(246, 242, 234));
    k.mats[2] = Material(MAT_RAMP, rgb(214, 70, 58), rgb(110, 30, 26));
    k.mats[3] = Material(MAT_LAMP, rgb(150, 160, 170));
    k.haze = 0.16f; k.nightFloor = 0.2f;
    Painter P(L);
    const float hx = 52.0f;
    const Layer &HL = s.layers[n - 1];
    float base = sampleCol(headRidge, HL.w, hx - HL.x0) + 3.0f;
    const float th = 38.0f, bw = 6.0f, tw = 4.4f;
    float top = base - th;
    // tower body (tapered), shaded as a cylinder: left half / right half
    for (int py = (int)top; py < (int)base; py++) {
      float u = (base - (py + 0.5f)) / th;
      float hw = lerpf(bw, tw, u);
      bool band = u > 0.42f && u < 0.60f;
      for (int px = (int)floorf(hx - hw - 1); px <= (int)ceilf(hx + hw + 1); px++) {
        float cov = sat(fminf(px + 1.0f, hx + hw) - fmaxf((float)px, hx - hw));
        if (cov <= 0) continue;
        float rel = (px + 0.5f - hx) / hw;                // -1..1 across the tower
        uint8_t ix = band ? packIdxF(2, sat(0.75f - 0.4f * fabsf(rel)))
                          : packIdxF(rel < 0 ? 0 : 1, sat(0.25f + 0.6f * fabsf(rel)));
        P.put(px - L.x0, py - L.y0, ix, cov);
      }
    }
    // gallery, lantern room, cap
    fillRect(P, hx - tw - 2.0f, top - 1.5f, hx + tw + 2.0f, top + 0.5f, packIdxF(2, 0.25f));
    fillRect(P, hx - 3.2f, top - 7.5f, hx + 3.2f, top - 1.5f, packIdxF(3, 0.9f));
    float cap[] = { hx - 4.6f, top - 7.5f, hx + 4.6f, top - 7.5f, hx, top - 12.5f };
    fillPolygon(P, cap, 3, packIdxF(2, 0.15f));
    drawCapsule(P, hx, top - 12.5f, hx, top - 14.5f, 0.6f, packIdxF(2, 0.1f));
    // cottage to the right of the tower
    const float cx0 = 62.0f, cx1 = 80.0f;
    float cb = sampleCol(headRidge, HL.w, (cx0 + cx1) * 0.5f - HL.x0) + 2.5f;
    fillRect(P, cx0, cb - 9.0f, cx1, cb, packIdxF(1, 0.30f));
    float roof[] = { cx0 - 1.5f, cb - 9.0f, cx1 + 1.5f, cb - 9.0f, cx1 - 3.0f, cb - 15.0f,
                     cx0 + 3.0f, cb - 15.0f };
    fillPolygon(P, roof, 4, packIdxF(2, 0.45f));
    fillRect(P, cx0 + 4.0f, cb - 6.5f, cx0 + 7.0f, cb - 3.5f, packIdxF(3, 0.3f));   // window
    fillRect(P, cx1 - 7.0f, cb - 6.5f, cx1 - 4.0f, cb - 3.5f, packIdxF(3, 0.5f));
    s.glowDepth = 0.60f;
    s.addGlow(hx, top - 4.5f, 16.0f);                     // lantern
    s.addGlow(cx0 + 5.5f, cb - 5.0f, 5.0f);               // cottage windows
    s.addGlow(cx1 - 5.5f, cb - 5.0f, 5.0f);
    freeMem(headRidge);
    headRidge = nullptr;
    n++;
  }
  // ---- 4. sea stacks in the surf -----------------------------------------
  {
    Layer &L = s.layers[n];
    LayerLook &k = s.looks[n];
    if (!setupTerrainLayer(L, 0.42f, 132, 214, true, 1)) return false;
    L.clampBottom = false;
    k.mats[0] = Material(MAT_SLOPE_L, rgb(138, 120, 104));
    k.mats[1] = Material(MAT_SLOPE_R, rgb(138, 120, 104));
    k.mats[2] = Material(MAT_RAMP, rgb(120, 160, 80), rgb(48, 80, 40));
    k.mats[3] = Material(MAT_RAMP, rgb(250, 252, 255), rgb(170, 190, 200));
    k.haze = 0.08f; k.nightFloor = 0.18f;
    Painter P(L);
    struct Stack { float x, w, h; } st[] = { { 158, 9, 44 }, { 186, 6, 26 }, { 214, 12, 66 } };
    const float base = 209.0f;
    for (const Stack &q : st) {
      for (int py = (int)(base - q.h - 2); py < (int)base + 2; py++) {
        float u = (base - (py + 0.5f)) / q.h;               // 0 base .. 1 top
        if (u > 1.0f) continue;
        float round = u > 0.82f ? sqrtf(sat(1.0f - (u - 0.82f) / 0.18f)) : 1.0f;
        float hw = q.w * (1.0f - 0.18f * u) * round + 0.8f * noise1(py * 0.4f, (uint32_t)q.x);
        for (int px = (int)(q.x - q.w - 2); px <= (int)(q.x + q.w + 2); px++) {
          float cov = sat(fminf(px + 1.0f, q.x + hw) - fmaxf((float)px, q.x - hw));
          if (cov <= 0) continue;
          float rel = (px + 0.5f - q.x) / (hw + 0.01f);
          uint8_t ix;
          if (py > base - 2.5f) ix = packIdxF(3, 0.8f);                       // foam
          else if (u > 0.9f) ix = packIdxF(2, 0.6f);                          // grass cap
          else {
            float strata = fbm1(py * 0.35f, (uint32_t)q.x + 9u, 2);
            ix = packIdxF(rel < -0.1f ? 0 : 1, sat(0.35f + 0.45f * fabsf(rel) + 0.15f * strata));
          }
          P.put(px - L.x0, py - L.y0, ix, cov);
        }
      }
      // surf around the base: a flattened, ragged ellipse of foam
      for (int py = (int)base - 3; py <= (int)base + 2; py++)
        for (int px = (int)(q.x - q.w - 6); px <= (int)(q.x + q.w + 6); px++) {
          float ex = (px + 0.5f - q.x) / (q.w + 4.0f), ey = (py + 0.5f - (base - 0.3f)) / 2.2f;
          float rr = 1.0f + 0.25f * noise1(px * 0.4f, (uint32_t)q.x + 21u);
          float d = ex * ex + ey * ey;
          if (d < rr) {
            float cov = sat((rr - d) * 3.0f);
            P.put(px - L.x0, py - L.y0, packIdxF(3, sat(0.75f + 0.25f * noise1(px * 0.6f, 5u))), cov);
          }
        }
    }
    n++;
  }
  // ---- 5. dunes with marram grass ----------------------------------------
  {
    Layer &L = s.layers[n];
    LayerLook &k = s.looks[n];
    if (!setupTerrainLayer(L, 0.26f, 188, 268, true, 16)) return false;
    k.mats[0] = Material(MAT_SLOPE_L, rgb(232, 206, 158));
    k.mats[1] = Material(MAT_SLOPE_R, rgb(232, 206, 158));
    k.mats[2] = Material(MAT_RAMP, rgb(168, 178, 96), rgb(70, 86, 44));
    k.mats[3] = Material(MAT_RAMP, rgb(200, 190, 120), rgb(96, 92, 56));
    k.haze = 0.03f; k.mist = 0.08f; k.nightFloor = 0.15f;
    Painter P(L);
    float *ridge = (float *)allocBig(sizeof(float) * L.w);
    if (!ridge) return false;
    for (int i = 0; i < L.w; i++) {
      float x = P.sx(i) + 0.5f;
      ridge[i] = 222.0f - 9.0f * fbm1(x * 0.011f + 2.0f, 0xD00Eu, 3, 0.45f) -
                 5.0f * sinf(x * 0.021f + 1.3f);
    }
    DuneCtx c{ &P, ridge, 0xD00Eu };
    fillBelowRidge(P, ridge, duneShade, &c);
    Rng rng(0x7A11u);
    for (int i = 0; i < 120; i++) {
      float x = rng.range(P.sx(0), P.sx(L.w - 1));
      float base = sampleCol(ridge, L.w, x - L.x0) + rng.range(1.0f, 6.0f);
      float h = rng.range(5.0f, 15.0f);
      drawBlade(P, x, base, h, rng.range(1.2f, 2.0f), rng.range(-0.5f, 0.5f) * h,
                rng.chance(0.6f) ? 2 : 3, 0.2f, rng.range(0.6f, 1.0f));
    }
    freeMem(ridge);
    n++;
  }
  // ---- 6. beach grass and a rope fence at the glass ----------------------
  {
    Layer &L = s.layers[n];
    LayerLook &k = s.looks[n];
    if (!setupTerrainLayer(L, 0.0f, 214, 280, true, 1)) return false;
    k.mats[0] = Material(MAT_RAMP, rgb(206, 196, 120), rgb(58, 62, 34));
    k.mats[1] = Material(MAT_RAMP, rgb(150, 122, 92), rgb(52, 40, 30));
    k.mats[2] = Material(MAT_RAMP, rgb(196, 178, 142), rgb(80, 70, 56));
    k.mats[3] = Material(MAT_ACCENT, rgb(250, 190, 214), rgb(200, 90, 140));
    k.nightFloor = 0.10f;
    Painter P(L);
    // fence posts with a sagging rope, receding to the right
    const float postX[] = { 150, 186, 222, 258 };
    const float postTop[] = { 232, 236, 240, 244 };
    for (int i = 0; i < 4; i++) {
      float w = 2.6f - 0.25f * i;
      fillRect(P, postX[i] - w, postTop[i], postX[i] + w, 282.0f, packIdxF(1, 0.55f - 0.1f * i));
      fillRect(P, postX[i] - w, postTop[i], postX[i] - w * 0.2f, 282.0f, packIdxF(1, 0.8f));
    }
    for (int i = 0; i < 3; i++) {
      float x0 = postX[i], x1 = postX[i + 1], y0 = postTop[i] + 3, y1 = postTop[i + 1] + 3;
      float px = x0, py = y0;
      for (int k2 = 1; k2 <= 12; k2++) {
        float u = k2 / 12.0f;
        float x = lerpf(x0, x1, u), y = lerpf(y0, y1, u) + 5.0f * sinf(u * 3.14159f);
        drawCapsule(P, px, py, x, y, 0.65f, packIdxF(2, 0.6f));
        px = x; py = y;
      }
    }
    GrassParams gp;
    gp.groundY = 270; gp.minH = 8; gp.maxH = 26; gp.edgeBoost = 22; gp.blades = 210;
    gp.matGrass = 0; gp.matFlowerA = 3; gp.matFlowerB = 3; gp.flowers = 0.06f;
    gp.seed = 0x6E55u;
    genGrass(P, gp);
    n++;
  }
  s.count = n;
  return true;
}

}  // namespace tp
