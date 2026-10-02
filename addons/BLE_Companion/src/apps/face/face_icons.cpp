// Vector icons — see face_icons.h. Mirrored in web/index.html ("ICONS
// MIRROR"); any change here must be made there too (the pixel-diff test in
// test/web/ will catch a mismatch).
#include "face_icons.h"

#include <Arduino_GFX.h>

#include "ble_proto.h"
#include "face_slots.h"

namespace FaceIcons {

namespace {

// Maps the 16x16 design grid onto the requested text size using integer
// maths only, so the browser preview can reproduce it exactly.
struct Pen {
  Arduino_GFX *g;
  int16_t x, y;
  uint8_t s;
  uint16_t c, bg;

  int16_t P(int v) const { return (int16_t)((v * s + 1) / 2); }
  int16_t X(int v) const { return (int16_t)(x + P(v)); }
  int16_t Y(int v) const { return (int16_t)(y + P(v)); }
  int16_t R(int v) const { int16_t r = P(v); return r < 1 ? 1 : r; }
  int16_t W(int v0, int len) const {
    int16_t w = (int16_t)(P(v0 + len) - P(v0));
    return w < 1 ? 1 : w;
  }

  void circle(int cx, int cy, int r, uint16_t col) const { g->fillCircle(X(cx), Y(cy), R(r), col); }
  void rect(int x0, int y0, int w, int h, uint16_t col) const {
    g->fillRect(X(x0), Y(y0), W(x0, w), W(y0, h), col);
  }
  void rrect(int x0, int y0, int w, int h, int r, uint16_t col) const {
    g->fillRoundRect(X(x0), Y(y0), W(x0, w), W(y0, h), R(r), col);
  }
  void tri(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t col) const {
    g->fillTriangle(X(x0), Y(y0), X(x1), Y(y1), X(x2), Y(y2), col);
  }
  // A two-pixel-wide stroke (the line plus a copy one pixel to the right).
  void stroke(int x0, int y0, int x1, int y1, uint16_t col) const {
    g->drawLine(X(x0), Y(y0), X(x1), Y(y1), col);
    g->drawLine((int16_t)(X(x0) + 1), Y(y0), (int16_t)(X(x1) + 1), Y(y1), col);
  }
};

// Cloud silhouette. dy shifts it up (negative) to leave room for weather below.
void cloud(const Pen &p, int dy, uint16_t col) {
  p.circle(5, 9 + dy, 3, col);
  p.circle(10, 7 + dy, 4, col);
  p.rrect(1, 9 + dy, 14, 5, 2, col);
}

void drawBattery(const Pen &p, uint8_t pct) {
  // Two-pixel body outline, terminal nub, proportional fill (red when low).
  p.rect(0, 3, 19, 2, p.c);
  p.rect(0, 11, 19, 2, p.c);
  p.rect(0, 3, 2, 10, p.c);
  p.rect(17, 3, 2, 10, p.c);
  p.rect(19, 6, 3, 4, p.c);
  if (pct <= 100) {
    int w = (13 * pct + 50) / 100;
    if (w > 0) p.rect(3, 6, w, 4, pct <= 15 ? (uint16_t)0xF800 : p.c);
  }
}

}  // namespace

void draw(Arduino_GFX *g, uint8_t id, uint8_t arg, int16_t x, int16_t y,
          uint8_t size, uint16_t color, uint16_t bg) {
  if (!g || id == 0 || size == 0) return;
  Pen p = { g, x, y, size, color, bg };
  switch (id) {
    case faceslots::kIconBattery:
      drawBattery(p, arg);
      break;

    case bleproto::ICON_SUN:
      p.circle(8, 8, 4, color);
      p.rect(7, 0, 2, 2, color);  p.rect(7, 14, 2, 2, color);
      p.rect(0, 7, 2, 2, color);  p.rect(14, 7, 2, 2, color);
      p.rect(2, 2, 2, 2, color);  p.rect(12, 2, 2, 2, color);
      p.rect(2, 12, 2, 2, color); p.rect(12, 12, 2, 2, color);
      break;

    case bleproto::ICON_PARTLY:
      p.circle(11, 5, 3, color);
      p.rect(10, 0, 2, 1, color);
      p.rect(15, 4, 1, 2, color);
      p.rect(14, 1, 1, 1, color);
      // bg halo so the cloud reads in front of the sun
      p.circle(5, 11, 4, bg);
      p.circle(9, 10, 4, bg);
      p.rrect(0, 10, 14, 6, 2, bg);
      p.circle(5, 11, 3, color);
      p.circle(9, 10, 3, color);
      p.rrect(1, 11, 12, 4, 2, color);
      break;

    case bleproto::ICON_CLOUD:
      cloud(p, 0, color);
      break;

    case bleproto::ICON_RAIN:
      cloud(p, -3, color);
      p.stroke(4, 12, 3, 15, color);
      p.stroke(8, 12, 7, 15, color);
      p.stroke(12, 12, 11, 15, color);
      break;

    case bleproto::ICON_STORM:
      // A bold bolt reads better than cloud + bolt at 16 px.
      p.tri(11, 0, 3, 9, 9, 9, color);
      p.tri(7, 7, 13, 7, 5, 16, color);
      break;

    case bleproto::ICON_SNOW:
      cloud(p, -3, color);
      p.circle(4, 14, 1, color);
      p.circle(8, 13, 1, color);
      p.circle(12, 14, 1, color);
      break;

    case bleproto::ICON_FOG:
      p.rrect(1, 3, 12, 2, 1, color);
      p.rrect(3, 7, 12, 2, 1, color);
      p.rrect(1, 11, 14, 2, 1, color);
      break;

    case bleproto::ICON_MOON:
      p.circle(8, 8, 6, color);
      p.circle(11, 6, 5, bg);
      break;

    case bleproto::ICON_CALENDAR:
      p.rrect(1, 2, 14, 13, 2, color);
      p.rect(3, 7, 10, 6, bg);
      p.rect(4, 0, 2, 4, color);
      p.rect(10, 0, 2, 4, color);
      p.rect(4, 8, 2, 2, color);
      p.rect(7, 8, 2, 2, color);
      p.rect(10, 8, 2, 2, color);
      p.rect(4, 11, 2, 1, color);
      p.rect(7, 11, 2, 1, color);
      break;

    case bleproto::ICON_BELL:
      p.rect(7, 0, 2, 2, color);
      p.circle(8, 7, 5, color);
      p.rect(3, 7, 10, 5, color);
      p.rrect(1, 11, 14, 2, 1, color);
      p.circle(8, 14, 1, color);
      break;

    case bleproto::ICON_HEART:
      p.circle(5, 6, 4, color);
      p.circle(11, 6, 4, color);
      p.tri(1, 7, 15, 7, 8, 15, color);
      break;

    case bleproto::ICON_STAR:
      p.tri(8, 1, 6, 6, 10, 6, color);      // spikes
      p.tri(15, 6, 10, 6, 11, 9, color);
      p.tri(12, 14, 11, 9, 8, 12, color);
      p.tri(4, 14, 8, 12, 5, 9, color);
      p.tri(1, 6, 5, 9, 6, 6, color);
      p.tri(6, 6, 10, 6, 11, 9, color);     // body
      p.tri(6, 6, 11, 9, 8, 12, color);
      p.tri(6, 6, 8, 12, 5, 9, color);
      break;

    case bleproto::ICON_CHAT:
      p.rrect(1, 2, 14, 10, 3, color);
      p.tri(4, 11, 8, 11, 3, 15, color);
      p.circle(5, 7, 1, bg);
      p.circle(8, 7, 1, bg);
      p.circle(11, 7, 1, bg);
      break;

    case bleproto::ICON_CHECK:
      p.circle(8, 8, 7, color);
      p.stroke(4, 8, 7, 11, bg);
      p.stroke(4, 9, 7, 12, bg);
      p.stroke(7, 11, 11, 5, bg);
      p.stroke(7, 12, 11, 6, bg);
      break;

    case bleproto::ICON_ALERT:
      p.tri(8, 1, 15, 14, 1, 14, color);
      p.rect(7, 5, 2, 5, bg);
      p.rect(7, 11, 2, 2, bg);
      break;

    default:
      break;
  }
}

}  // namespace FaceIcons
