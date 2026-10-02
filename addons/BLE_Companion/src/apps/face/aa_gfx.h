// aa — anti-aliased 2-D drawing into an RGB565 buffer, integer maths only.
//
// Pure C++11 (no Arduino). The watch draws into frameCanvas()'s framebuffer,
// the host preview into a plain array, and web/index.html carries a line for
// line JavaScript port ("AA MIRROR") that the Node tests diff against frames
// rendered by this code. Bit-exact agreement is by construction:
//   * no floating point anywhere (coverage comes from integer signed
//     distances, square roots from isqrt32),
//   * every divide has a non-negative numerator (C++ truncation == JS floor),
//   * all intermediates stay below 2^31,
//   * a pixel's value depends only on its position and the call's arguments,
//     never on the clip rectangle, so a partial redraw through a clip is
//     identical to a full one (the face renderer relies on this).
// Type and icons are pre-rendered 4-bit coverage masks (aa_assets.*, made by
// tools/gen_aa_assets.py).
//
// Conventions: rectangles are integer pixels, half-open. Circles, capsules and
// arcs take Q8 coordinates (1/256 px; a pixel's centre is x*256 + 128).
// Alpha is 0..255. Colours are 8-bit Rgb; blends round, stores truncate to
// RGB565, and gradients are ordered-dithered.
#pragma once
#include <stdint.h>

namespace aa {

struct Rgb { uint8_t r, g, b; };

struct Glyph { uint32_t off; uint8_t w, h; int8_t dx, dy; uint8_t adv; };
struct Font {
  const uint8_t *bits;        // per-glyph 4-bit nibble streams, high nibble first
  const Glyph   *glyphs;
  const uint8_t *index;       // 256 entries: glyph code -> glyph, 0xFF = none
  uint8_t ascent, descent, cap, fallback;
};
struct Mask { const uint8_t *bits; uint8_t w, h; };

struct Surface {
  uint16_t *px;
  int16_t w, h;
  int16_t cx0, cy0, cx1, cy1;   // clip, half-open
};

// ---- colour ----------------------------------------------------------------
Rgb      unpack(uint16_t c565);                 // bit-replicated 8-bit channels
uint16_t pack(Rgb c);                           // truncating
Rgb      rgb(uint8_t r, uint8_t g, uint8_t b);
Rgb      mix(Rgb a, Rgb b, int t256);           // t = 0 -> a, 256 -> b
int      luma(Rgb c);                           // 0..255 (77 r + 150 g + 29 b) >> 8
uint32_t isqrt32(uint32_t n);

// ---- surface ---------------------------------------------------------------
void init(Surface &s, uint16_t *px, int w, int h);
void setClip(Surface &s, int x0, int y0, int x1, int y1);   // intersected with the surface
void resetClip(Surface &s);

// ---- primitives ------------------------------------------------------------
void fillRect(Surface &s, int x, int y, int w, int h, Rgb c);
void blendRect(Surface &s, int x, int y, int w, int h, Rgb c, int a);

// Soft elliptical light: weight (1 - q)^2 where q = (dx/rx)^2 + (dy/ry)^2.
struct Glow { int16_t cx, cy, rx, ry; Rgb c; uint8_t a; };
// Vertical gradient top -> bottom over [y, y + h) plus up to 3 glows, mixed at
// 16-bit precision and dithered once. Fills the rectangle (no blending).
// Columns past 240 are left untouched.
void backdrop(Surface &s, int x, int y, int w, int h, Rgb top, Rgb bottom,
              const Glow *glows, int nGlows);

// Signed distance (Q8 px) from pixel (px, py)'s centre to a rounded box.
int  sdRoundBox(int px, int py, int x, int y, int w, int h, int r);
void roundRect(Surface &s, int x, int y, int w, int h, int r, Rgb c, int a);
// Vertical gradient fill, `top` on the first row, `bottom` on the last.
void roundRectV(Surface &s, int x, int y, int w, int h, int r, Rgb top, Rgb bottom, int a);
// One colour, alpha graded from aTop on the first row to aBottom on the last.
void roundRectVA(Surface &s, int x, int y, int w, int h, int r, Rgb c, int aTop, int aBottom);
void roundRectStroke(Surface &s, int x, int y, int w, int h, int r, int t, Rgb c, int a);
// Stroke with alpha graded from aTop (first row) to aBottom (last row): a lit edge.
void roundRectStrokeVA(Surface &s, int x, int y, int w, int h, int r, int t, Rgb c, int aTop,
                       int aBottom);
// Shadow that fades from alpha `a` at the box edge to 0 at `blur` px outside.
void softShadow(Surface &s, int x, int y, int w, int h, int r, int blur, Rgb c, int a);

void disc(Surface &s, int cx, int cy, int r, Rgb c, int a);                    // Q8
void ring(Surface &s, int cx, int cy, int r, int thick, Rgb c, int a);          // Q8, centreline r
// Segment (x0,y0)-(x1,y1) with round caps of radius r, all Q8.
void capsule(Surface &s, int x0, int y0, int x1, int y1, int r, Rgb c, int a);
// Arc of a ring (Q8 centre, centreline radius and thickness) from turn a0 to
// a1 in 1/1024 turn, clockwise from 12 o'clock, round caps. a1 - a0 >= 1024
// draws the closed ring.
void arc(Surface &s, int cx, int cy, int r, int thick, int a0, int a1, Rgb c, int a);
// Unit vector of turn t (1/1024 turn, 0 = up, clockwise) in Q14.
void dir(int t, int &x, int &y);

void mask(Surface &s, const Mask &m, int x, int y, Rgb c, int a);

// ---- text ------------------------------------------------------------------
enum TextFlags : uint8_t { TF_NONE = 0, TF_TABULAR = 1, TF_UPPER = 2 };
// Glyph codes are bytes (faceslots::utf8ToGlyphs output or plain ASCII).
// `tracking` is extra px after every glyph but the last.
int  textWidth(const Font &f, const uint8_t *s, int n, int tracking, uint8_t flags);
// Draws with the pen at x and the baseline at y; returns the advance.
int  text(Surface &s, const Font &f, const uint8_t *g, int n, int x, int y, Rgb c, int a,
          int tracking, uint8_t flags);
// Same, with the colour graded from `top` at row y0 to `bottom` at row y1.
int  textV(Surface &s, const Font &f, const uint8_t *g, int n, int x, int y, Rgb top,
           Rgb bottom, int y0, int y1, int a, int tracking, uint8_t flags);
// Convenience for C strings.
int  textWidthS(const Font &f, const char *s, int tracking = 0, uint8_t flags = 0);
int  textS(Surface &s, const Font &f, const char *str, int x, int y, Rgb c, int a = 255,
           int tracking = 0, uint8_t flags = 0);
// Horizontally centred on cx.
int  textCS(Surface &s, const Font &f, const char *str, int cx, int y, Rgb c, int a = 255,
            int tracking = 0, uint8_t flags = 0);

}  // namespace aa
