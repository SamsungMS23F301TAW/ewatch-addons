// Text fitting and anti-aliased rasterising for Adafruit GFX fonts.
//
// Portable C++ (no Arduino). The fitter word-wraps a short phrase into a
// convex region (a triangle on the die, a disc for the idle prompt), centres
// every line, and shrinks the type until everything fits. The rasteriser
// draws the result into an 8-bit coverage buffer by splatting each set font
// pixel's area into the destination grid: an exact box filter, so the 24 pt
// bitmap fonts scale down to any size with smooth edges.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "gfxfont.h"

namespace oracle {

constexpr int kMaxTextLen   = 48;   // characters, including the terminator
constexpr int kMaxTextLines = 5;
constexpr int kMaxWords     = 12;

// A convex area that text must stay inside. span() returns the horizontal
// extent available across the whole vertical band [y0, y1] (local px).
class FitRegion {
 public:
  virtual ~FitRegion() = default;
  virtual bool span(float y0, float y1, float &xl, float &xr) const = 0;
  // Preferred vertical centre for a block of text.
  virtual float centreY() const = 0;
  // How far the block may slide off centreY() to fit (local px).
  virtual float slack() const = 0;
};

// Equilateral triangle centred on its centroid (0,0). Circumradius r, with an
// inset margin applied to every edge. pointsDown puts the wide edge on top.
class TriangleRegion : public FitRegion {
 public:
  TriangleRegion(float circumR, float inset, bool pointsDown);
  bool  span(float y0, float y1, float &xl, float &xr) const override;
  float centreY() const override;
  float slack() const override;
  // Signed distance from the triangle edges (negative inside), no rounding.
  float edgeDistance(float x, float y) const;
 private:
  float inR_;        // inradius after the inset
  bool  down_;
};

// Disc of radius r centred at (0,0).
class DiscRegion : public FitRegion {
 public:
  explicit DiscRegion(float r) : r_(r) {}
  bool  span(float y0, float y1, float &xl, float &xr) const override;
  float centreY() const override { return 0.f; }
  float slack() const override { return 0.f; }
 private:
  float r_;
};

struct FitParams {
  float capMax    = 16.f;   // largest cap height to try (px)
  float capMin    = 9.f;    // smallest acceptable cap height (px)
  float condense  = 1.f;    // horizontal scale relative to vertical
  float lineGap   = 0.3f;   // pitch = cap * (1 + lineGap)
  float tracking  = 0.f;    // extra px between letters, at the final size
  int   maxLines  = 4;
  bool  uppercase = true;
};

struct TextLine {
  uint8_t start = 0, len = 0;   // into TextLayout::text
  float   x = 0;                // pen x of the first glyph (local px)
  float   baseline = 0;         // local px
  float   inkW = 0;             // ink width (local px)
};

struct TextLayout {
  bool     ok = false;
  float    cap = 0;             // chosen cap height (px)
  float    sx = 0, sy = 0;      // font px -> local px
  float    tracking = 0;
  int      lineCount = 0;
  TextLine lines[kMaxTextLines];
  char     text[kMaxTextLen] = {0};  // normalised text the lines index into
};

// Cap height of a font in font pixels (from 'H', else yAdvance * 0.6).
float fontCapHeight(const GFXfont *font);

// Fit `text` into `region`. Returns false (layout.ok == false) if it does not
// fit even at capMin; the layout still holds the capMin attempt for debugging.
bool fitText(const GFXfont *font, const char *text, const FitRegion &region,
             const FitParams &params, TextLayout &layout);

// Draw a fitted layout into an 8-bit coverage buffer. (originX, originY) is
// the buffer position of the region's (0,0). Coverage adds and saturates, so
// drawing twice thickens; clear the buffer first.
void rasterizeText(const GFXfont *font, const TextLayout &layout,
                   uint8_t *dst, int w, int h, int stride,
                   float originX, float originY);

// Ink bounding box of the layout in local px (for tests and debugging).
void layoutInkBounds(const GFXfont *font, const TextLayout &layout,
                     float &x0, float &y0, float &x1, float &y1);

}  // namespace oracle
