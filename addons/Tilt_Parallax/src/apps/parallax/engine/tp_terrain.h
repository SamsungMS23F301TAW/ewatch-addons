// Tilt Parallax — reusable landscape generators.
#pragma once
#include <stdint.h>
#include "tp_paint.h"

namespace tp {

// A mountain range drawn as lit and shaded facets: every peak sends a spur
// down the face, splitting it into a left-facing and a right-facing side, so
// the morning sun lights one side and the evening sun the other. Optional
// snow caps that grow on the highest peaks and streak down gullies.
struct RangeParams {
  float    baseY = 140;      // mean ridge height (screen y)
  float    amp = 24;         // ridge relief in px
  float    rough = 0.52f;    // midpoint-displacement roughness
  float    detail = 2.0f;    // fine jaggedness in px
  float    snow = 0;         // 0 = none, 1 = generous
  float    spurDrift = 6.0f; // wobble of the light/shadow split line
  int      matRockL = 0, matRockR = 1, matSnowL = 2, matSnowR = 3;
  float    peakBias = 0;     // >0 raises massifs near the centre, <0 the sides
  uint32_t seed = 1;
};
void genRange(Painter &P, const RangeParams &p);

// Rolling ground whose top edge is a smooth ridge. Facing (left/right) follows
// the local slope near the crest; deeper down it flattens out.
struct HillParams {
  float    baseY = 190;
  float    amp = 10;
  float    freq = 0.012f;    // ridge frequency (per px)
  int      matL = 0, matR = 1;
  float    levelTop = 0.55f; // steepness level at the crest
  float    levelDeep = 0.15f;
  float    relief = 0.12f;   // light/shade contrast of the undulations below the crest
  float    texture = 0.10f;  // fine texture amplitude
  float    texFreq = 0.11f;  // fine texture frequency (higher = smaller blobs)
  uint32_t seed = 2;
};
// Fills the hill and writes the ridge (screen y per layer column) to ridgeOut.
void genHill(Painter &P, const HillParams &p, float *ridgeOut);

// A crowded line of pines whose bases sit along ridge[] (per layer column).
struct ForestParams {
  float    minH = 8, maxH = 20;   // tree heights
  float    widthRatio = 0.42f;    // width / height
  float    spacing = 4.5f;        // mean gap between trunks
  float    sink = 3.0f;           // how far bases sit below the ridge
  float    density = 0.85f;       // 0..1 chance a slot gets a tree
  int      matL = 2, matR = 3;
  float    level = 0.55f;
  uint32_t seed = 3;
};
void genForest(Painter &P, const float *ridge, const ForestParams &p);

// Grass blades with flowers along the bottom of the screen.
struct GrassParams {
  float    groundY = 268;         // top of the solid ground strip
  float    minH = 6, maxH = 24;
  float    edgeBoost = 14;        // taller blades toward the screen edges
  int      blades = 150;
  int      matGrass = 0;
  int      matFlowerA = 1, matFlowerB = 3;
  float    flowers = 0.10f;       // chance per blade of a flower
  uint32_t seed = 4;
};
void genGrass(Painter &P, const GrassParams &p);

}  // namespace tp
