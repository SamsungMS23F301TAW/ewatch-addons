// Tilt Parallax — scenes: a back-to-front stack of procedurally drawn layers.
//
// buildScene() is the expensive part (it draws all the art). It's pure
// computation into PSRAM buffers, so on the watch it runs on core 0 while
// setup() is still waiting on hardware delays, and again in the background
// when the user taps to change scene.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "tp_layer.h"
#include "tp_shade.h"
#include "tp_sky.h"

namespace tp {

// A light that blooms after dusk (lamp, lit window, bulb). Screen space at
// zero parallax; drawn as a soft halo in a sprite that moves with `depth`.
struct GlowSpot {
  float x, y, r;
};

struct Scene {
  static const int kMaxLayers = 7;
  static const int kMaxGlows = 24;
  int       id = -1;
  int       count = 0;               // terrain layers, far -> near
  Layer     layers[kMaxLayers];
  LayerLook looks[kMaxLayers];
  SkyStyle  sky;
  int16_t   skyBottom = 220;         // screen row the sky must reach
  bool      clouds = true;
  uint32_t  buildMs = 0;             // generation time, for diagnostics
  // Night glow halos (optional): all at one depth, drawn just in front of
  // the terrain layer at that depth.
  GlowSpot  glows[kMaxGlows];
  int       nGlows = 0;
  float     glowDepth = 0;
  void addGlow(float x, float y, float r) {
    if (nGlows < kMaxGlows) { glows[nGlows].x = x; glows[nGlows].y = y; glows[nGlows].r = r; nGlows++; }
  }

  void   release();
  size_t bytes() const;
};

int         sceneCount();
const char *sceneName(int id);

// Builds scene `id` into `out` (which must be empty/released). Returns false
// on allocation failure; `out` is then released.
bool buildScene(int id, Scene &out);

// Helpers shared by the scene files.
bool setupTerrainLayer(Layer &L, float depth, int top, int bottom, bool alpha, uint8_t bands);

// Individual scene builders (tp_scene_*.cpp).
bool buildAlpine(Scene &s);
bool buildCoast(Scene &s);
bool buildCity(Scene &s);
bool buildDesert(Scene &s);

}  // namespace tp
