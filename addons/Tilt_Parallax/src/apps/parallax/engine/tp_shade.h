// Tilt Parallax — materials and palette (LUT) building.
//
// Indexed layers store, per pixel, a material (0..3) and a 63-step level:
//     idx = 1 + mat * 63 + level        (idx 0 = transparent)
// What "level" means depends on the material kind (surface steepness for
// slopes, brightness for ramps, a random class for windows...). buildLuts()
// turns that into RGB565 for the current sky: lighting from the sun or moon,
// atmospheric haze that pales distant layers, valley mist toward the bottom of
// a layer (via the row bands), and emissive windows and lamps at night.
#pragma once
#include <stdint.h>
#include "tp_color.h"
#include "tp_layer.h"
#include "tp_sky.h"

namespace tp {

enum MatKind : uint8_t {
  MAT_UNUSED = 0,
  MAT_SLOPE_L,   // faces left (lit by a morning sun); level = steepness 0..1
  MAT_SLOPE_R,   // faces right (lit by an evening sun)
  MAT_RAMP,      // pre-shaded surface; level = albedoDark..albedo
  MAT_WINDOWS,   // building windows; level = random class (who lights first)
  MAT_LAMP,      // emissive at night (lighthouse, street lamp)
  MAT_WATER,     // reflects the sky; level = reflection 0..1
  MAT_ACCENT,    // flowers etc.: ramp that keeps saturation at dusk
};

struct Material {
  MatKind kind;
  RGB albedo;        // lit / light end
  RGB albedoDark;    // ramp dark end, window glass at night
  float sheen;       // sky reflection (snow, wet rock)
  Material() : kind(MAT_UNUSED), albedo(RGB{ 128, 128, 128 }), albedoDark(RGB{ 50, 50, 50 }),
               sheen(0) {}
  Material(MatKind k, RGB a, RGB d = RGB{ 0, 0, 0 }, float sh = 0)
      : kind(k), albedo(a), albedoDark(d), sheen(sh) {}
};

struct LayerLook {
  Material mats[4];
  float haze = 0;        // haze at the top of the layer (0..1)
  float mist = 0;        // additional haze at the bottom of the layer
  float exposure = 1;    // overall light multiplier
  float nightFloor = 0;  // keeps silhouettes from vanishing at night (0..1)
};

static inline uint8_t packIdx(int mat, int level) {
  if (level < 0) level = 0;
  if (level > 62) level = 62;
  return (uint8_t)(1 + mat * 63 + level);
}
static inline uint8_t packIdxF(int mat, float s) {
  return packIdx(mat, (int)(s * 62.0f + 0.5f));
}

// Shades one material level for a given haze amount. Exposed for tests.
RGB shadeMaterial(const Material &m, float level, float haze, const LayerLook &look,
                  const SkyState &sky);

void buildLuts(Layer &L, const LayerLook &look, const SkyState &sky);

}  // namespace tp
