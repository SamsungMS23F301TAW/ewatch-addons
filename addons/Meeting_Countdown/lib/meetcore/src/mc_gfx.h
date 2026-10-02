// Meeting Countdown — anti-aliased 2D drawing into an RGB565 frame buffer.
//
// Pure C++ (no Arduino): on the watch it draws into frameCanvas()'s PSRAM
// buffer before one flush(); on the host it draws into a plain array that the
// preview tool writes out as PNG. Same pixels either way.
//
// Colours are 0xRRGGBB; blending happens in 8-bit per channel and the result
// is stored as RGB565.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "gfxfont.h"

namespace mc {

struct Canvas {
  uint16_t *px;
  int w, h;
};

constexpr uint16_t rgb565(uint32_t c) {
  return (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
}
uint32_t rgb888(uint16_t c);
uint32_t mixRgb(uint32_t a, uint32_t b, float t);       // t=0 -> a, t=1 -> b
uint32_t scaleRgb(uint32_t c, float k);                 // brightness
uint32_t contrastOn(uint32_t bg);                       // black or white text for bg

void clear(Canvas &c, uint32_t rgb);
void fillRect(Canvas &c, int x, int y, int w, int h, uint32_t rgb);
void blend(Canvas &c, int x, int y, uint32_t rgb, float alpha);

// Anti-aliased shapes (coordinates are pixel-centre based floats).
void fillCircle(Canvas &c, float cx, float cy, float r, uint32_t rgb, float alpha = 1.0f);
void fillRoundRect(Canvas &c, float x, float y, float w, float h, float r, uint32_t rgb,
                   float alpha = 1.0f);
void strokeRoundRect(Canvas &c, float x, float y, float w, float h, float r, float width,
                     uint32_t rgb, float alpha = 1.0f);
void fillCapsule(Canvas &c, float x0, float y0, float x1, float y1, float r, uint32_t rgb,
                 float alpha = 1.0f);

// ---- the countdown ring ----------------------------------------------------
// A rounded-rectangle track that hugs the screen edge. Positions along it are
// a fraction s in [0, 1): 0 = 12 o'clock, increasing clockwise, uniform in
// arc length.
struct Ring {
  float cx, cy;          // centre
  float a, b;            // half extents of the centreline
  float rc;              // centreline corner radius
  float thick;           // stroke width
  float len;             // centreline length
};
Ring makeRing(int screenW, int screenH, float inset, float thick, float cornerRadius);
void ringPoint(const Ring &r, float s, float &x, float &y);
// Draws the track, then the filled arc [s0, s1] with round caps (s0 < s1,
// both in [0, 1]; s1 - s0 >= 1 draws the closed ring). `ticks` > 0 dots the
// track at that many even divisions.
void drawRing(Canvas &c, const Ring &r, float s0, float s1, uint32_t fillRgb,
              uint32_t trackRgb, int ticks, uint32_t tickRgb);

// ---- text ------------------------------------------------------------------
// Adafruit GFXfont glyphs, area-sampled at `scale` (< 1 gives smooth,
// anti-aliased text from the 24 pt bitmaps). y is the baseline.
float textWidth(const GFXfont *f, const char *s, float scale, float tracking = 0);
void  drawText(Canvas &c, const GFXfont *f, const char *s, float x, float y, float scale,
               uint32_t rgb, float tracking = 0, float alpha = 1.0f);
enum class Align : uint8_t { Left, Center, Right };
void  drawTextAligned(Canvas &c, const GFXfont *f, const char *s, float x, float y, float scale,
                      uint32_t rgb, Align a, float tracking = 0);
// Fits `s` into maxW: returns how many bytes of s fit on one line, breaking
// at a space when possible. If ellipsize, the last line gets "...".
int   fitText(const GFXfont *f, const char *s, float scale, float maxW, bool atWordBoundary);
// Copies s into out, shortened with "..." so it fits maxW.
void  ellipsize(const GFXfont *f, const char *s, float scale, float maxW, char *out, size_t cap);

// ---- clock digits ----------------------------------------------------------
// An original rounded monoline digit set drawn from strokes and arcs, so the
// time stays crisp and anti-aliased at any size. Handles "0123456789:".
float digitsWidth(const char *s, float height);
void  drawDigits(Canvas &c, const char *s, float x, float top, float height, float weight,
                 uint32_t rgb);

}  // namespace mc
