// Tilt Parallax — the per-frame compositor.
//
// composeRows() draws screen rows [y0,y1) of a back-to-front layer stack into
// `dst` (row stride in pixels). It never touches the art generators: a frame
// is span fills, LUT copies and a few alpha blends per row. For each row it
// first finds the nearest layer that is opaque across the whole screen and
// starts there, so the ground near the bottom of the screen costs one pass.
#pragma once
#include <stdint.h>
#include "tp_layer.h"

namespace tp {

// Screen-space row of layer L that covers screen row y, honouring the current
// offsets and clamping. Returns false when the layer doesn't touch row y.
bool layerRowFor(const Layer &L, int y, int &r);

// Screen x of the layer's column 0.
static inline int layerScreenX(const Layer &L) { return L.x0 + L.dx + L.animDx; }
static inline int layerScreenY(const Layer &L) { return L.y0 + L.dy + L.animDy; }

void composeRows(Layer *const *layers, int count, int y0, int y1, int screenW,
                 uint16_t *dst, int stride);

// Reference implementation: per-pixel, no spans, no culling. Used by the unit
// tests to prove the fast path is pixel-identical.
void composeRowsReference(Layer *const *layers, int count, int y0, int y1,
                          int screenW, uint16_t *dst, int stride);

}  // namespace tp
