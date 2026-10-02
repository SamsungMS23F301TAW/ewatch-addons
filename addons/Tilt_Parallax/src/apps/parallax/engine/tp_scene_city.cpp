// City: a hazy far skyline, a middle skyline with window grids that light up
// one by one after dusk (and go dark again in the small hours), tall
// buildings framing the view, and a rooftop at the glass with a railing,
// a water tank and string lights that glow at night.
#include "tp_scene.h"
#include "tp_terrain.h"
#include "tp_platform.h"
#include "tp_color.h"
#include <math.h>

namespace tp {

namespace {

// Materials used by every building layer.
const int M_SIDE_L = 0, M_SIDE_R = 1, M_FRONT = 2, M_WIN = 3;

struct BuildingStyle {
  float winW, winH, gapX, gapY;     // window grid (px)
  float margin;                     // wall around the grid
  float litBias;                    // shifts window classes (lower = lit more often)
};

// One building: a front face with a window grid, a narrow side face toward
// the screen centre (perspective), optional setback crown and antenna.
void drawBuilding(Painter &P, Rng &rng, float x, float w, float top, float base,
                  const BuildingStyle &bs, float frontLevel, bool crown, bool antenna) {
  float side = fabsf(x + w * 0.5f - 120.0f) * 0.035f;
  side = side > 4.0f ? 4.0f : side;
  bool sideOnRight = (x + w * 0.5f) < 120.0f;
  // front face
  fillRect(P, x, top, x + w, base, packIdxF(M_FRONT, frontLevel));
  // side face
  if (side >= 0.6f) {
    if (sideOnRight) fillRect(P, x + w, top + side * 0.6f, x + w + side, base, packIdxF(M_SIDE_R, 0.92f));
    else fillRect(P, x - side, top + side * 0.6f, x, base, packIdxF(M_SIDE_L, 0.92f));
  }
  // setback crown
  float roof = top;
  if (crown && w > 14) {
    float cw = w * rng.range(0.45f, 0.7f), ch = rng.range(5.0f, 12.0f);
    float cx = x + (w - cw) * 0.5f;
    fillRect(P, cx, top - ch, cx + cw, top, packIdxF(M_FRONT, sat(frontLevel - 0.08f)));
    roof = top - ch;
    if (antenna) {
      float ax = cx + cw * 0.5f, ah = rng.range(8.0f, 18.0f);
      drawCapsule(P, ax, roof, ax, roof - ah, 0.55f, packIdxF(M_FRONT, 0.2f));
    }
  } else if (antenna) {
    float ax = x + w * rng.range(0.3f, 0.7f), ah = rng.range(5.0f, 12.0f);
    drawCapsule(P, ax, roof, ax, roof - ah, 0.5f, packIdxF(M_FRONT, 0.2f));
  }
  // window grid
  float gx0 = x + bs.margin, gx1 = x + w - bs.margin;
  for (float wy = top + bs.margin; wy + bs.winH <= base - 1.0f; wy += bs.winH + bs.gapY) {
    for (float wx = gx0; wx + bs.winW <= gx1 + 0.01f; wx += bs.winW + bs.gapX) {
      float cls = sat(rng.unit() + bs.litBias);
      fillRect(P, wx, wy, wx + bs.winW, wy + bs.winH, packIdxF(M_WIN, cls));
    }
  }
}

void setBuildingLook(LayerLook &k, RGB front, RGB frontDark, float haze, float mist) {
  k.mats[M_SIDE_L] = Material(MAT_SLOPE_L, scale(front, 0.92f));
  k.mats[M_SIDE_R] = Material(MAT_SLOPE_R, scale(front, 0.92f));
  k.mats[M_FRONT] = Material(MAT_RAMP, front, frontDark);
  k.mats[M_WIN] = Material(MAT_WINDOWS, rgb(120, 140, 160), rgb(26, 30, 42));
  k.haze = haze;
  k.mist = mist;
  k.nightFloor = 0.22f;
}

}  // namespace

bool buildCity(Scene &s) {
  s.sky.horizonY = 192;
  s.sky.sea = false;
  s.sky.lightPollution = 0.75f;
  s.sky.starDensity = 0.45f;          // city lights wash out the faint stars
  s.sky.seed = 0xC17Bu;
  s.skyBottom = 214;
  s.clouds = true;
  int n = 0;

  // ---- 1. far skyline ------------------------------------------------------
  {
    Layer &L = s.layers[n];
    LayerLook &k = s.looks[n];
    if (!setupTerrainLayer(L, 0.78f, 104, 214, true, 16)) return false;
    setBuildingLook(k, rgb(170, 182, 206), rgb(120, 126, 150), 0.40f, 0.30f);
    Painter P(L);
    Rng rng(0xFA12u);
    BuildingStyle bs = { 1.0f, 1.0f, 2.0f, 2.0f, 2.0f, 0.15f };
    float x = P.sx(0) - 6;
    while (x < P.sx(L.w - 1)) {
      float w = rng.range(8.0f, 18.0f);
      float h = rng.range(26.0f, 58.0f);
      float centre = 1.0f - fabsf(x - 120.0f) / 170.0f;
      h *= 0.75f + 0.5f * centre;
      drawBuilding(P, rng, x, w, 196.0f - h, 216.0f, bs, rng.range(0.55f, 0.85f),
                   rng.chance(0.3f), rng.chance(0.25f));
      x += w + rng.range(-3.0f, 2.0f);
    }
    // solid band of low buildings along the base (the layer's bottom row is
    // repeated downward when it shifts up, so it must have no gaps)
    fillRect(P, P.sx(0), 206.0f, P.sx(L.w - 1) + 1, 214.0f, packIdxF(M_FRONT, 0.6f));
    // a slender landmark tower with a needle
    float tx = 168.0f;
    fillRect(P, tx - 3.0f, 116.0f, tx + 3.0f, 214.0f, packIdxF(M_FRONT, 0.75f));
    fillRect(P, tx - 5.0f, 112.0f, tx + 5.0f, 117.0f, packIdxF(M_FRONT, 0.65f));
    drawCapsule(P, tx, 112.0f, tx, 92.0f, 0.7f, packIdxF(M_FRONT, 0.6f));
    n++;
  }
  // ---- 2. middle skyline ---------------------------------------------------
  {
    Layer &L = s.layers[n];
    LayerLook &k = s.looks[n];
    if (!setupTerrainLayer(L, 0.52f, 124, 232, true, 16)) return false;
    setBuildingLook(k, rgb(196, 196, 204), rgb(150, 92, 76), 0.18f, 0.32f);  // concrete..brick
    Painter P(L);
    Rng rng(0x3D3Du);
    BuildingStyle bs = { 2.0f, 2.0f, 2.0f, 3.0f, 3.0f, 0.0f };
    float x = P.sx(0) - 4;
    while (x < P.sx(L.w - 1)) {
      float w = rng.range(14.0f, 30.0f);
      float h = rng.range(30.0f, 70.0f);
      if (x > 70 && x < 150) h *= 0.75f;            // keep a dip under the clock
      drawBuilding(P, rng, x, w, 214.0f - h, 234.0f, bs, rng.range(0.45f, 0.8f),
                   rng.chance(0.35f), rng.chance(0.35f));
      // rooftop water tank on some
      if (rng.chance(0.25f) && w > 16) {
        float tx = x + w * 0.3f, ty = 214.0f - h;
        fillRect(P, tx, ty - 6.0f, tx + 5.0f, ty - 1.0f, packIdxF(M_SIDE_L, 0.5f));
        drawCapsule(P, tx + 0.5f, ty - 1.0f, tx + 0.5f, ty, 0.4f, packIdxF(M_FRONT, 0.2f));
        drawCapsule(P, tx + 4.5f, ty - 1.0f, tx + 4.5f, ty, 0.4f, packIdxF(M_FRONT, 0.2f));
      }
      x += w + rng.range(1.0f, 5.0f);
    }
    fillRect(P, P.sx(0), 224.0f, P.sx(L.w - 1) + 1, 232.0f, packIdxF(M_FRONT, 0.5f));
    n++;
  }
  // ---- 3. near buildings framing the view ----------------------------------
  {
    Layer &L = s.layers[n];
    LayerLook &k = s.looks[n];
    if (!setupTerrainLayer(L, 0.30f, 118, 262, true, 16)) return false;
    setBuildingLook(k, rgb(150, 140, 136), rgb(92, 58, 50), 0.05f, 0.25f);
    Painter P(L);
    Rng rng(0x7E77u);
    BuildingStyle bs = { 3.0f, 4.0f, 3.0f, 4.0f, 4.0f, -0.05f };
    drawBuilding(P, rng, -24.0f, 52.0f, 128.0f, 262.0f, bs, 0.55f, false, true);
    drawBuilding(P, rng, 30.0f, 26.0f, 176.0f, 262.0f, bs, 0.70f, false, false);
    drawBuilding(P, rng, 196.0f, 64.0f, 138.0f, 262.0f, bs, 0.50f, true, false);
    drawBuilding(P, rng, 166.0f, 24.0f, 186.0f, 262.0f, bs, 0.68f, false, true);
    // street-level band between them (hidden behind the parapet, but keeps
    // the bottom row solid for the clamp)
    fillRect(P, P.sx(0), 250.0f, P.sx(L.w - 1) + 1, 262.0f, packIdxF(M_FRONT, 0.3f));
    n++;
  }
  // ---- 4. rooftop at the glass ----------------------------------------------
  {
    Layer &L = s.layers[n];
    LayerLook &k = s.looks[n];
    if (!setupTerrainLayer(L, 0.04f, 214, 280, true, 1)) return false;
    k.mats[0] = Material(MAT_RAMP, rgb(150, 146, 150), rgb(40, 40, 46));     // concrete
    k.mats[1] = Material(MAT_RAMP, rgb(110, 116, 124), rgb(26, 28, 34));     // metal
    k.mats[2] = Material(MAT_LAMP, rgb(120, 116, 104));                      // bulbs
    k.mats[3] = Material(MAT_RAMP, rgb(96, 140, 70), rgb(24, 46, 26));       // plants
    k.nightFloor = 0.12f;
    Painter P(L);
    // parapet wall with a coping stone
    fillRect(P, P.sx(0), 254.0f, P.sx(L.w - 1) + 1, 281.0f, packIdxF(0, 0.30f));
    fillRect(P, P.sx(0), 251.0f, P.sx(L.w - 1) + 1, 255.0f, packIdxF(0, 0.65f));
    // railing
    for (float x = P.sx(0) + 3; x < P.sx(L.w - 1); x += 11.0f)
      drawCapsule(P, x, 251.0f, x, 236.0f, 0.75f, packIdxF(1, 0.45f));
    drawCapsule(P, P.sx(0), 236.0f, P.sx(L.w - 1), 236.0f, 1.0f, packIdxF(1, 0.6f));
    // water tank on legs (left)
    fillRect(P, 4.0f, 214.0f, 30.0f, 236.0f, packIdxF(1, 0.35f));
    fillRect(P, 4.0f, 214.0f, 10.0f, 236.0f, packIdxF(1, 0.55f));
    float tankRoof[] = { 2.0f, 214.5f, 32.0f, 214.5f, 17.0f, 206.0f };
    fillPolygon(P, tankRoof, 3, packIdxF(1, 0.25f));
    // potted shrub (right)
    fillRect(P, 210.0f, 240.0f, 226.0f, 252.0f, packIdxF(0, 0.5f));
    drawBush(P, 218.0f, 241.0f, 11.0f, 3, 3, 0.55f, 0xB05Eu);
    // string lights sagging across the view
    s.glowDepth = 0.04f;
    Rng rng(0x5711u);
    float px = -14.0f, py = 222.0f;
    for (int i = 1; i <= 40; i++) {
      float u = i / 40.0f;
      float x = lerpf(-14.0f, 254.0f, u);
      float y = lerpf(222.0f, 228.0f, u) + 16.0f * sinf(u * 3.14159f);
      drawCapsule(P, px, py, x, y, 0.45f, packIdxF(1, 0.15f));
      px = x; py = y;
      if (i % 3 == 0) {
        drawCapsule(P, x, y, x, y + 1.5f, 0.4f, packIdxF(1, 0.15f));
        drawDisc(P, x, y + 2.8f, 2.0f, packIdxF(2, rng.unit()));
        s.addGlow(x, y + 2.8f, 7.0f);
      }
    }
    n++;
  }
  s.count = n;
  return true;
}

}  // namespace tp
