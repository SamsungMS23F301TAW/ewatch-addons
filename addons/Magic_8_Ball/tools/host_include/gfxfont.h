// Host-only stand-in for GFX Library for Arduino's gfxfont.h.
//
// The firmware gets the real header from the GFX library. Host builds (the
// native unit tests and tools/oracle_preview.cpp) put this directory on the
// include path instead, so the BaseOS font headers in src/apps/assets/fonts
// compile on macOS/Linux unchanged. The layout must match the library's
// structs exactly; the font data is plain const arrays either way.
#pragma once
#include <stdint.h>

#ifndef PROGMEM
#define PROGMEM
#endif

typedef struct {
  uint16_t bitmapOffset;  // offset into GFXfont::bitmap
  uint8_t  width;         // bitmap size in pixels
  uint8_t  height;
  uint8_t  xAdvance;      // cursor advance
  int8_t   xOffset;       // pen to upper-left corner
  int8_t   yOffset;
} GFXglyph;

typedef struct {
  uint8_t  *bitmap;       // concatenated glyph bitmaps
  GFXglyph *glyph;        // glyph table
  uint16_t  first;        // first character code
  uint16_t  last;         // last character code
  uint8_t   yAdvance;     // line height
} GFXfont;
