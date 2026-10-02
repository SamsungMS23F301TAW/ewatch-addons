// Friend Radar software renderer: anti-aliased drawing into an RGB565 frame.
//
// Pure C++ (no Arduino). On the watch it draws straight into the shared
// frameCanvas() framebuffer, which is then flushed to the panel; on the host
// the same code renders PNG previews. Colours are native-endian RGB565, as
// Arduino_Canvas stores them. All primitives clip to the canvas clip rect.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "fr_font.h"

namespace fr {

struct Rect {
  int16_t x = 0, y = 0, w = 0, h = 0;
  Rect() = default;
  Rect(int x_, int y_, int w_, int h_) : x((int16_t)x_), y((int16_t)y_), w((int16_t)w_), h((int16_t)h_) {}
  bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  Rect grow(int d) const { return Rect(x - d, y - d, w + 2 * d, h + 2 * d); }
};

struct Canvas {
  uint16_t *px = nullptr;
  int16_t w = 0, h = 0;
  int16_t cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;   // clip [cx0, cx1) x [cy0, cy1)

  Canvas() = default;
  Canvas(uint16_t *buf, int width, int height) { attach(buf, width, height); }
  void attach(uint16_t *buf, int width, int height) {
    px = buf; w = (int16_t)width; h = (int16_t)height; resetClip();
  }
  void resetClip() { cx0 = 0; cy0 = 0; cx1 = w; cy1 = h; }
  void clip(const Rect &r);   // intersect with r
};

// ---- colour -----------------------------------------------------------------
constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
constexpr uint16_t hex(uint32_t c) { return rgb((uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c); }

// a = 0 keeps bg, 255 gives fg.
static inline uint16_t blend(uint16_t bg, uint16_t fg, uint8_t a) {
  uint32_t a5 = ((uint32_t)a + 4) >> 3;            // 0..32
  uint32_t b = bg, f = fg;
  b = (b | (b << 16)) & 0x07E0F81Fu;
  f = (f | (f << 16)) & 0x07E0F81Fu;
  uint32_t r = ((((f - b) * a5) >> 5) + b) & 0x07E0F81Fu;
  return (uint16_t)(r | (r >> 16));
}
// 8-bit-per-channel colour quantised to RGB565 with a 4x4 ordered dither, so
// dark gradients do not band. Use for static layers (cost per pixel is small
// but not free).
uint16_t ditherRgb(float r, float g, float b, int x, int y);
static inline void unpack(uint16_t c, float &r, float &g, float &b) {
  r = (float)((c >> 11) & 31) * (255.f / 31.f);
  g = (float)((c >> 5) & 63) * (255.f / 63.f);
  b = (float)(c & 31) * (255.f / 31.f);
}
uint16_t addColor(uint16_t bg, uint16_t fg, uint8_t a);   // additive, saturating
uint16_t scaleColor(uint16_t c, uint8_t k);                // c * k / 255
uint16_t hsv(uint16_t hueDeg, uint8_t sat, uint8_t val);   // HSV -> RGB565
uint8_t  luma(uint16_t c);                                 // 0..255 perceived brightness

// ---- fills ------------------------------------------------------------------
void clear(Canvas &c, uint16_t col);
void fillRect(Canvas &c, int x, int y, int w, int h, uint16_t col);
void blendRect(Canvas &c, int x, int y, int w, int h, uint16_t col, uint8_t a);
void pixel(Canvas &c, int x, int y, uint16_t col, uint8_t a = 255);
void vGradient(Canvas &c, int x, int y, int w, int h, uint16_t top, uint16_t bottom);
// Dithered vertical gradient between two 24-bit colours (0xRRGGBB).
void vGradientDither(Canvas &c, int x, int y, int w, int h, uint32_t top, uint32_t bottom);

// ---- anti-aliased shapes (alpha scales the whole shape) ---------------------
void fillCircle(Canvas &c, float cx, float cy, float r, uint16_t col, uint8_t a = 255);
// Ring centred on radius r with half-width hw (total stroke 2*hw).
void ring(Canvas &c, float cx, float cy, float r, float hw, uint16_t col, uint8_t a = 255);
// Arc of a ring from angle a0 to a1 (degrees, 0 = up, clockwise), round ends.
void arc(Canvas &c, float cx, float cy, float r, float hw, float a0, float a1,
         uint16_t col, uint8_t a = 255);
void line(Canvas &c, float x0, float y0, float x1, float y1, float hw,
          uint16_t col, uint8_t a = 255);
void fillRoundRect(Canvas &c, int x, int y, int w, int h, float r, uint16_t col, uint8_t a = 255);
void strokeRoundRect(Canvas &c, int x, int y, int w, int h, float r, float hw,
                     uint16_t col, uint8_t a = 255);
// A leaf / petal shape from (cx, cy) outwards at `angleRad` (0 = up, clockwise):
// half-width follows width * sin(pi * u^0.8) along its length.
void petal(Canvas &c, float cx, float cy, float angleRad, float len, float width,
           uint16_t col, uint8_t a = 255);
// Soft radial glow: alpha falls off as (1 - d/r)^2, additive or blended.
void glow(Canvas &c, float cx, float cy, float r, uint16_t col, uint8_t a, bool additive = true);

// ---- materials ----------------------------------------------------------------
// Lit sphere (an "orb"): diffuse + specular from the top-left, a soft rim glow.
void sphere(Canvas &c, float cx, float cy, float r, uint16_t col, uint8_t a = 255);
// Soft drop shadow of a rounded rectangle, fading out over `spread` pixels.
// With skipInside the area under the shape itself is left alone (opaque panels).
void softShadow(Canvas &c, int x, int y, int w, int h, float r, float spread, uint8_t a,
                int dy = 0, bool skipInside = true);
// Rounded rectangle filled with a vertical gradient.
void fillRoundRectV(Canvas &c, int x, int y, int w, int h, float r, uint16_t top,
                    uint16_t bottom, uint8_t a = 255);
// Deterministic per-pixel noise in [-1, 1] (film grain for static layers).
float grainAt(int x, int y, uint32_t seed);

// ---- text -------------------------------------------------------------------
int  textWidth(const Font &f, const char *s);
// Letter-spaced text: `track` extra pixels after every glyph but the last.
int  textWidthTracked(const Font &f, const char *s, int track);
int  textTracked(Canvas &c, const Font &f, int x, int baseline, const char *s, uint16_t col,
                 uint8_t a, int track);
void textCenteredTracked(Canvas &c, const Font &f, int cx, int baseline, const char *s,
                         uint16_t col, uint8_t a, int track);
// Text with a soft 1 px drop shadow under it.
void textShadowed(Canvas &c, const Font &f, int x, int baseline, const char *s, uint16_t col,
                  uint8_t a = 255, uint16_t shadow = 0x0000, uint8_t shadowA = 150);
// Text set along a circular arc with each glyph rotated to follow it.
// centreDeg: 0 = 12 o'clock, clockwise. radius: baseline radius.
// bottom = true reads left to right along the bottom (glyph tops point inward);
// false reads along the top (glyph tops point outward).
void textArc(Canvas &c, const Font &f, float cx, float cy, float radius, float centreDeg,
             const char *s, uint16_t col, uint8_t a, float trackPx, bool bottom);
// Angular half-width (degrees) the arc text above would cover.
float textArcHalfSpanDeg(const Font &f, float radius, const char *s, float trackPx);
// Draws at pen x and baseline y; returns the pen x after the text.
int  text(Canvas &c, const Font &f, int x, int baseline, const char *s,
          uint16_t col, uint8_t a = 255);
void textCentered(Canvas &c, const Font &f, int cx, int baseline, const char *s,
                  uint16_t col, uint8_t a = 255);
void textRight(Canvas &c, const Font &f, int rx, int baseline, const char *s,
               uint16_t col, uint8_t a = 255);
// Copy s into out, shortened with an ellipsis so it fits in maxW pixels.
void fitText(const Font &f, const char *s, int maxW, char *out, size_t cap);
// Word-wrap s into lines no wider than maxW; draws them from `baseline` with
// the font's line height (or lineH if > 0). Returns the number of lines.
int  textWrapped(Canvas &c, const Font &f, int x, int baseline, int maxW, const char *s,
                 uint16_t col, int lineH = 0, uint8_t a = 255, bool centered = false);
int  wrappedLineCount(const Font &f, int maxW, const char *s);

}  // namespace fr
