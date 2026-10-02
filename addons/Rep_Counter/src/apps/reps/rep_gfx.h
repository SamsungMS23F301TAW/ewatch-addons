// Rep Counter: a tiny anti-aliased 2D library for an RGB565 framebuffer.
//
// The app composes every frame into the BaseOS frame canvas
// (Arduino_Canvas::getFramebuffer()) with these functions and then pushes
// only the rows that changed. Nothing here touches Arduino, so the exact same
// pixels can be rendered on the host for previews and tests.
//
// Shapes are anti-aliased with signed-distance coverage. Text comes in two
// flavours: the generated 4-bit numeral fonts (rep_font.h) and the 1-bit
// Adafruit-format FreeFonts that BaseOS already ships.
#pragma once
#include <stdint.h>
#include "gfxfont.h"
#ifndef PROGMEM          // the FreeFont headers expect it; flash is mapped on ESP32
#define PROGMEM
#endif

namespace rgfx {

// ---- colour ----------------------------------------------------------------
static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static inline uint8_t red8(uint16_t c) { return (uint8_t)(((c >> 11) & 0x1F) * 255 / 31); }
static inline uint8_t green8(uint16_t c) { return (uint8_t)(((c >> 5) & 0x3F) * 255 / 63); }
static inline uint8_t blue8(uint16_t c) { return (uint8_t)((c & 0x1F) * 255 / 31); }

// a = 0 -> bg, 255 -> fg.
uint16_t blend(uint16_t fg, uint16_t bg, uint8_t a);
// Perceived brightness 0..255.
uint8_t luma(uint16_t c);

// ---- surface ---------------------------------------------------------------
struct Surface {
  uint16_t *px = nullptr;
  int w = 0, h = 0;
  int cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;   // clip rectangle [x0, x1) x [y0, y1)
  Surface() = default;
  Surface(uint16_t *p, int w_, int h_) : px(p), w(w_), h(h_), cx0(0), cy0(0), cx1(w_), cy1(h_) {}
  void clip(int x, int y, int ww, int hh);
  void noClip() { cx0 = 0; cy0 = 0; cx1 = w; cy1 = h; }
  inline void plot(int x, int y, uint16_t c, uint8_t a) {
    if (x < cx0 || y < cy0 || x >= cx1 || y >= cy1 || a == 0) return;
    uint16_t &d = px[y * w + x];
    d = (a >= 255) ? c : blend(c, d, a);
  }
};

void fill(Surface &s, uint16_t c);
void fillRect(Surface &s, int x, int y, int w, int h, uint16_t c);
// Vertical gradient from top colour to bottom colour.
void fillRectV(Surface &s, int x, int y, int w, int h, uint16_t top, uint16_t bottom);

// Anti-aliased shapes. Coordinates are in pixels (pixel centres at +0.5).
void fillRoundRect(Surface &s, float x, float y, float w, float h, float r, uint16_t c);
void strokeRoundRect(Surface &s, float x, float y, float w, float h, float r,
                     float t, uint16_t c);
void fillCircle(Surface &s, float cx, float cy, float r, uint16_t c);
void strokeCircle(Surface &s, float cx, float cy, float r, float t, uint16_t c);
// Thick line with round caps.
void capsule(Surface &s, float x0, float y0, float x1, float y1, float r, uint16_t c);
void fillTriangle(Surface &s, float x0, float y0, float x1, float y1, float x2, float y2,
                  uint16_t c);
// Arc of a ring, angles in degrees clockwise from 12 o'clock, round ends.
void arc(Surface &s, float cx, float cy, float r, float t, float a0, float a1, uint16_t c);

// ---- 4-bit anti-aliased fonts (rep_font.h) ---------------------------------
struct AaGlyph {
  uint32_t offset;          // into data
  uint8_t w, h;             // bitmap size
  int8_t xOff, yOff;        // from pen x / em-box top
  uint8_t adv;              // advance
};
struct AaFont {
  const uint8_t *data;
  const AaGlyph *glyphs;
  const char *chars;        // characters in glyph order
  uint8_t count;
  uint8_t height;           // em height (cap/digit height incl. stroke)
};
int  aaWidth(const AaFont &f, const char *s, int tracking = 0);
// Draw with the em box's top edge at y. Returns the pen x after the text.
int  aaText(Surface &s, const AaFont &f, int x, int y, const char *str, uint16_t c,
            int tracking = 0);
void aaTextCentered(Surface &s, const AaFont &f, int cx, int y, const char *str,
                    uint16_t c, int tracking = 0);

// ---- 1-bit Adafruit GFXfonts (BaseOS FreeFonts) ----------------------------
int  gfxWidth(const GFXfont *f, const char *s);
// Height from the baseline to the top of capital letters (uses 'H').
int  gfxCapHeight(const GFXfont *f);
// Draw with the baseline at y. Returns the pen x after the text.
int  gfxText(Surface &s, const GFXfont *f, int x, int baseline, const char *str, uint16_t c);
void gfxTextCentered(Surface &s, const GFXfont *f, int cx, int baseline, const char *str,
                     uint16_t c);
void gfxTextRight(Surface &s, const GFXfont *f, int right, int baseline, const char *str,
                  uint16_t c);

}  // namespace rgfx
