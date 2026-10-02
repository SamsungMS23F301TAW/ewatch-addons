// Vector icons for face slots and pushed messages (sun, cloud, calendar, ...).
//
// Each icon is designed on a 16x16 grid and drawn with Arduino_GFX
// primitives. Coordinates are scaled with integer maths only,
//   px(v) = (v * size + 1) / 2      (size 2 -> 1:1, size 3 -> 1.5x)
// so the web preview (web/index.html, "ICONS MIRROR") reproduces every pixel.
// Host-compilable: depends on Arduino_GFX only.
#pragma once
#include <stdint.h>

class Arduino_GFX;

namespace FaceIcons {

// Draw icon `id` (bleproto::Icon 1..15, or faceslots::kIconBattery with
// `arg` = percent / 255 unknown) with its top-left corner at (x, y) for text
// size `size` (box = faceslots::iconWidth(id, size) x 8*size). `bg` is used to
// carve details (moon crescent, alert mark) out of filled shapes.
void draw(Arduino_GFX *g, uint8_t id, uint8_t arg, int16_t x, int16_t y,
          uint8_t size, uint16_t color, uint16_t bg);

}  // namespace FaceIcons
