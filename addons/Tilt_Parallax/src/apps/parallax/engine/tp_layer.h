// Tilt Parallax — layers and their run-length "span" encoding.
//
// A layer is a pre-rendered image a little wider than the screen (overscan),
// so it can slide by its parallax offset without exposing an edge. Three
// flavours:
//
//   INDEXED  8-bit palette indices (+ optional 8-bit coverage for anti-aliased
//            edges). Colours come from per-row-band LUTs, so the time-of-day
//            lighting changes by rebuilding 256-entry tables, never by
//            re-rendering art. Index 0 is transparent when there's no alpha.
//   RGB      Opaque RGB565 (the sky, which is regenerated each minute).
//   ALPHA    8-bit coverage only, drawn in one colour (clock digits, shadows).
//
// After the pixels are drawn, encodeSpans() turns every row into a short list
// of runs: SOLID (one colour, no memory reads), LUT (indexed copy) and BLEND
// (anti-aliased edge pixels). Transparent pixels produce no runs at all, so a
// frame is just a few tight loops per row. Rows that are opaque across the
// whole layer width are flagged so the compositor can skip everything behind
// them.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace tp {

enum SpanKind : uint8_t { SPAN_SOLID = 0, SPAN_LUT = 1, SPAN_BLEND = 2 };

struct Span {
  int16_t  x;      // first column, layer space
  int16_t  len;    // >= 1
  uint8_t  kind;   // SpanKind
  uint8_t  idx;    // SOLID: palette index (INDEXED layers)
  uint16_t pad;
};

enum LayerKind : uint8_t { LAYER_INDEXED = 0, LAYER_RGB = 1, LAYER_ALPHA = 2 };

static const uint8_t ROW_OPAQUE = 0x01;   // every column of the row is opaque

struct Layer {
  // ---- set by the generator ----
  LayerKind kind = LAYER_INDEXED;
  int16_t   w = 0, h = 0;        // buffer size in pixels
  int16_t   x0 = 0, y0 = 0;      // screen position of pixel (0,0) at zero offset
  float     depth = 0;           // parallax factor (0 = glass, 1 = sky)
  float     depthY = -1;         // vertical factor; < 0 means "same as depth"
  bool      clampTop = false;    // rows above 0 repeat row 0
  bool      clampBottom = false; // rows below h-1 repeat row h-1
  bool      wrapX = false;       // tiles horizontally with period w
  uint8_t  *idx = nullptr;       // INDEXED: w*h indices
  uint8_t  *alpha = nullptr;     // INDEXED (optional) / ALPHA: w*h coverage
  uint16_t *rgb = nullptr;       // RGB: w*h pixels
  // ---- built by encodeSpans() ----
  Span     *spans = nullptr;
  uint32_t *rowStart = nullptr;  // h+1 entries into spans[]
  uint8_t  *rowFlags = nullptr;  // h entries
  uint32_t  spanCount = 0;
  // ---- colour ----
  uint8_t   nBands = 1;          // INDEXED: LUT bands, rows split evenly
  uint16_t *luts = nullptr;      // INDEXED: nBands*256 RGB565 entries
  uint16_t  color = 0xFFFF;      // ALPHA: draw colour
  uint8_t   opacity = 255;       // ALPHA: global multiplier
  bool      visible = true;
  // ---- per-frame state (set by the engine) ----
  int16_t   dx = 0, dy = 0;      // current parallax offset, pixels
  int16_t   animDx = 0, animDy = 0; // extra offset for transitions / drift

  // Allocates pixel planes (PSRAM). Returns false on allocation failure.
  bool allocIndexed(int16_t w, int16_t h, bool withAlpha, uint8_t bands);
  bool allocRGB(int16_t w, int16_t h);
  bool allocAlpha(int16_t w, int16_t h);
  void release();                // frees everything, keeps geometry fields

  // Builds spans/rowStart/rowFlags from the pixel planes. Call after drawing.
  // Returns false on allocation failure (layer is then left invisible).
  bool encodeSpans();

  // Bytes of PSRAM held (pixels + spans + luts), for diagnostics.
  size_t bytes() const;

  uint16_t *lutForRow(int r) const {
    int b = (nBands <= 1) ? 0 : (r * nBands) / h;
    return luts + (b << 8);
  }
};

// Copy-free rectangle in screen space, used for dirty tracking.
struct RowRange { int16_t y0, y1; };   // [y0, y1)

}  // namespace tp
