#include "tp_scene.h"
#include "tp_config.h"
#include "tp_platform.h"

namespace tp {

struct SceneEntry { const char *name; bool (*build)(Scene &); };
static const SceneEntry kScenes[] = {
  { "Alpine", buildAlpine },
  { "Coast",  buildCoast  },
  { "City",   buildCity   },
  { "Desert", buildDesert },
};
static const int kSceneN = sizeof(kScenes) / sizeof(kScenes[0]);

int sceneCount() { return kSceneN; }

const char *sceneName(int id) {
  if (id < 0 || id >= kSceneN) return "?";
  return kScenes[id].name;
}

void Scene::release() {
  for (int i = 0; i < kMaxLayers; i++) layers[i].release();
  count = 0;
  id = -1;
  nGlows = 0;
}

size_t Scene::bytes() const {
  size_t b = 0;
  for (int i = 0; i < count; i++) b += layers[i].bytes();
  return b;
}

bool setupTerrainLayer(Layer &L, float depth, int top, int bottom, bool alpha, uint8_t bands) {
  yieldBriefly();            // between layers: let IDLE (and its watchdog) run
  int ov = overscanX(depth);
  L.depth = depth;
  L.depthY = -1;
  L.x0 = (int16_t)(-ov);
  L.y0 = (int16_t)top;
  L.clampBottom = true;
  L.clampTop = false;
  L.wrapX = false;
  L.visible = true;
  // Mist is applied per row band; ~3 rows per band keeps the gradient smooth
  // (each band is a 512-byte LUT, so this is also the memory knob).
  int h = bottom - top;
  if (bands > 1) {
    int want = (h + 2) / 3;
    bands = (uint8_t)(want > 48 ? 48 : (want < bands ? bands : want));
  }
  return L.allocIndexed((int16_t)(kScreenW + 2 * ov), (int16_t)h, alpha, bands);
}

bool buildScene(int id, Scene &out) {
  out.release();
  if (id < 0 || id >= kSceneN) id = 0;
  out.id = id;
  bool ok = kScenes[id].build(out);
  if (ok) {
    for (int i = 0; i < out.count && ok; i++) ok = out.layers[i].encodeSpans();
  }
  if (!ok) { out.release(); return false; }
  out.id = id;
  return true;
}

}  // namespace tp
