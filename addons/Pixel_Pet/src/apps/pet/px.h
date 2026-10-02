// px — a tiny software renderer for Pixel Pet. Pure C++ (no Arduino): the
// same code paints the watch's PSRAM frame canvas and the host previews in
// tools/preview, so what the previews show is what the watch draws.
//
// Pixels are RGB565 in native byte order (what Arduino_Canvas stores).
// Sprites are 8-bit palette indices, 0 = transparent, drawn with integer
// nearest-neighbour scaling so pixel art stays crisp.
#pragma once
#include <stdint.h>

namespace px {

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
constexpr uint16_t hex(uint32_t c) {
  return rgb((uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c);
}
// Linear mix of two RGB565 colours, t = 0..256 (0 = a, 256 = b).
uint16_t mix(uint16_t a, uint16_t b, int t);

struct Canvas {
  uint16_t *buf;
  int16_t w, h;
  int16_t cx0, cy0, cx1, cy1;   // clip rectangle, max exclusive

  Canvas(uint16_t *b, int16_t width, int16_t height)
      : buf(b), w(width), h(height), cx0(0), cy0(0), cx1(width), cy1(height) {}
  void clip(int x0, int y0, int x1, int y1);   // intersected with the canvas
  void noClip() { cx0 = 0; cy0 = 0; cx1 = w; cy1 = h; }
};

void fill(Canvas &c, uint16_t col);
void rect(Canvas &c, int x, int y, int w, int h, uint16_t col);
void frame(Canvas &c, int x, int y, int w, int h, uint16_t col);   // 1-px outline
// Pixel-art rounded rectangle: corners cut by `r` (0..3) steps.
void roundRect(Canvas &c, int x, int y, int w, int h, int r, uint16_t col);
// Ordered-dither blend of two colours over a rect; level 0..16 (16 = all b).
void dither(Canvas &c, int x, int y, int w, int h, uint16_t a, uint16_t b, int level);
// Blit an index sprite. `pal[i]` for i>0; index 0 is transparent.
void blit(Canvas &c, const uint8_t *idx, int sw, int sh, int x, int y, int scale,
          const uint16_t *pal, bool flipX = false);
// Same, but every opaque pixel is painted `solid` (silhouettes, flashes).
void blitSolid(Canvas &c, const uint8_t *idx, int sw, int sh, int x, int y, int scale,
               uint16_t solid);

// ---- text ------------------------------------------------------------------
enum class Font : uint8_t { Small, Big };   // 5x7 caps/digits; 6x9 clock digits
int  text(Canvas &c, const char *s, int x, int y, int scale, uint16_t col,
          Font f = Font::Small, int spacing = 1);
int  textWidth(const char *s, int scale, Font f = Font::Small, int spacing = 1);
// Centered horizontally on cx. Returns the left x.
int  textC(Canvas &c, const char *s, int cx, int y, int scale, uint16_t col,
           Font f = Font::Small, int spacing = 1);
// Text with a 1-scaled-pixel drop shadow (readable over busy scenes).
int  textShadow(Canvas &c, const char *s, int x, int y, int scale, uint16_t col,
                uint16_t shadow, Font f = Font::Small, int spacing = 1);
int  glyphHeight(Font f);

// Integer -> "12,345" (thousands separators). Returns length.
int  fmtThousands(char *out, int cap, uint32_t v);

}  // namespace px
