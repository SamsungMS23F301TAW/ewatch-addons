// Alpine: hazy far peaks, a snowy middle range, a forested ridge with valley
// mist, a meadow with tall pines, and grass and wildflowers at the glass.
#include "tp_scene.h"
#include "tp_terrain.h"
#include "tp_platform.h"

namespace tp {

bool buildAlpine(Scene &s) {
  s.sky.horizonY = 182;
  s.sky.sea = false;
  s.sky.starDensity = 1.0f;
  s.sky.seed = 0xA1B2u;
  s.skyBottom = 214;
  s.clouds = true;
  int n = 0;

  // ---- 1. far range -------------------------------------------------------
  {
    Layer &L = s.layers[n];
    LayerLook &k = s.looks[n];
    if (!setupTerrainLayer(L, 0.82f, 88, 214, true, 32)) return false;
    k.mats[0] = Material(MAT_SLOPE_L, rgb(150, 162, 190), rgb(0, 0, 0), 0);
    k.mats[1] = Material(MAT_SLOPE_R, rgb(150, 162, 190), rgb(0, 0, 0), 0);
    k.mats[2] = Material(MAT_SLOPE_L, rgb(248, 250, 255), rgb(0, 0, 0), 0.10f);
    k.mats[3] = Material(MAT_SLOPE_R, rgb(248, 250, 255), rgb(0, 0, 0), 0.10f);
    k.haze = 0.46f; k.mist = 0.36f; k.nightFloor = 0.30f;
    Painter P(L);
    RangeParams rp;
    rp.baseY = 126; rp.amp = 26; rp.rough = 0.56f; rp.detail = 2.2f;
    rp.snow = 1.0f; rp.seed = 0x51A7u; rp.peakBias = 0.15f;
    genRange(P, rp);
    n++;
  }
  // ---- 2. middle range ----------------------------------------------------
  {
    Layer &L = s.layers[n];
    LayerLook &k = s.looks[n];
    if (!setupTerrainLayer(L, 0.60f, 120, 230, true, 32)) return false;
    k.mats[0] = Material(MAT_SLOPE_L, rgb(92, 106, 122), rgb(0, 0, 0), 0);
    k.mats[1] = Material(MAT_SLOPE_R, rgb(92, 106, 122), rgb(0, 0, 0), 0);
    k.mats[2] = Material(MAT_SLOPE_L, rgb(240, 244, 252), rgb(0, 0, 0), 0.08f);
    k.mats[3] = Material(MAT_SLOPE_R, rgb(240, 244, 252), rgb(0, 0, 0), 0.08f);
    k.haze = 0.20f; k.mist = 0.46f; k.nightFloor = 0.22f;
    Painter P(L);
    RangeParams rp;
    rp.baseY = 164; rp.amp = 18; rp.rough = 0.48f; rp.detail = 1.4f;
    rp.snow = 0.35f; rp.seed = 0x77E1u; rp.peakBias = -0.25f;
    genRange(P, rp);
    n++;
  }
  // ---- 3. forested ridge --------------------------------------------------
  {
    Layer &L = s.layers[n];
    LayerLook &k = s.looks[n];
    if (!setupTerrainLayer(L, 0.38f, 158, 246, true, 24)) return false;
    k.mats[0] = Material(MAT_SLOPE_L, rgb(50, 94, 66), rgb(0, 0, 0), 0);
    k.mats[1] = Material(MAT_SLOPE_R, rgb(50, 94, 66), rgb(0, 0, 0), 0);
    k.mats[2] = Material(MAT_SLOPE_L, rgb(44, 88, 60), rgb(0, 0, 0), 0);
    k.mats[3] = Material(MAT_SLOPE_R, rgb(44, 88, 60), rgb(0, 0, 0), 0);
    k.haze = 0.10f; k.mist = 0.55f; k.nightFloor = 0.16f;
    Painter P(L);
    float *ridge = (float *)allocBig(sizeof(float) * L.w);
    if (!ridge) return false;
    HillParams hp;
    hp.baseY = 192; hp.amp = 11; hp.freq = 0.010f; hp.seed = 0x3C3Cu;
    hp.levelTop = 0.50f; hp.levelDeep = 0.10f; hp.relief = 0.18f;
    hp.texture = 0.16f; hp.texFreq = 0.42f;        // a carpet of tree crowns
    genHill(P, hp, ridge);
    ForestParams fp;
    fp.minH = 8; fp.maxH = 22; fp.spacing = 4.2f; fp.sink = 3.0f; fp.density = 0.9f;
    fp.matL = 2; fp.matR = 3; fp.level = 0.55f; fp.seed = 0xF0E5u;
    genForest(P, ridge, fp);
    freeMem(ridge);
    n++;
  }
  // ---- 4. meadow with tall pines -----------------------------------------
  {
    Layer &L = s.layers[n];
    LayerLook &k = s.looks[n];
    if (!setupTerrainLayer(L, 0.20f, 138, 264, true, 16)) return false;
    k.mats[0] = Material(MAT_SLOPE_L, rgb(104, 148, 72), rgb(0, 0, 0), 0);
    k.mats[1] = Material(MAT_SLOPE_R, rgb(104, 148, 72), rgb(0, 0, 0), 0);
    k.mats[2] = Material(MAT_SLOPE_L, rgb(38, 80, 50), rgb(0, 0, 0), 0);
    k.mats[3] = Material(MAT_SLOPE_R, rgb(38, 80, 50), rgb(0, 0, 0), 0);
    k.haze = 0.02f; k.mist = 0.10f; k.nightFloor = 0.12f;
    Painter P(L);
    float *ridge = (float *)allocBig(sizeof(float) * L.w);
    if (!ridge) return false;
    HillParams hp;
    hp.baseY = 236; hp.amp = 12; hp.freq = 0.008f; hp.seed = 0x9D11u;
    hp.levelTop = 0.45f; hp.levelDeep = 0.08f; hp.relief = 0.22f;
    hp.texture = 0.05f; hp.texFreq = 0.08f;
    genHill(P, hp, ridge);
    Rng rng(0xBEEFu);
    // a stand of tall pines on the right, a lone one on the left
    const float px[] = { 196, 214, 231, 22 };
    const float ph[] = { 84, 98, 70, 62 };
    for (int i = 0; i < 4; i++) {
      float base = sampleCol(ridge, L.w, px[i] - L.x0) + 4.0f;
      drawPine(P, px[i], base, ph[i], ph[i] * 0.40f, 2, 3, 0.58f + rng.range(-0.05f, 0.05f),
               0x1000u + i);
    }
    // a few small pines along the meadow crest
    for (int i = 0; i < 9; i++) {
      float x = rng.range(40, 170);
      float base = sampleCol(ridge, L.w, x - L.x0) + 2.0f;
      float h = rng.range(10, 20);
      drawPine(P, x, base, h, h * 0.42f, 2, 3, 0.55f, 0x2000u + i);
    }
    freeMem(ridge);
    n++;
  }
  // ---- 5. grass and flowers at the glass ---------------------------------
  {
    Layer &L = s.layers[n];
    LayerLook &k = s.looks[n];
    if (!setupTerrainLayer(L, 0.0f, 236, 280, true, 1)) return false;
    k.mats[0] = Material(MAT_RAMP, rgb(118, 166, 74), rgb(20, 46, 26), 0);
    k.mats[1] = Material(MAT_ACCENT, rgb(255, 232, 140), rgb(210, 150, 40), 0);
    k.mats[2] = Material(MAT_RAMP, rgb(150, 146, 136), rgb(46, 44, 42), 0);
    k.mats[3] = Material(MAT_ACCENT, rgb(250, 236, 250), rgb(200, 120, 190), 0);
    k.haze = 0; k.mist = 0; k.nightFloor = 0.10f;
    Painter P(L);
    GrassParams gp;
    gp.groundY = 268; gp.minH = 7; gp.maxH = 22; gp.edgeBoost = 20; gp.blades = 260;
    gp.matGrass = 0; gp.matFlowerA = 1; gp.matFlowerB = 3; gp.flowers = 0.10f;
    gp.seed = 0x6A55u;
    genGrass(P, gp);
    n++;
  }
  s.count = n;
  return true;
}

}  // namespace tp
