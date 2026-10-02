// Host-side stand-in for GFX Library for Arduino's gfxfont.h, so the GFX
// FreeFont headers (and the meetcore renderer) compile with plain clang++ for
// unit tests and preview rendering. Layout is identical to the library's.
#ifndef _GFXFONT_H_
#define _GFXFONT_H_
#include <stdint.h>

#ifndef PROGMEM
#define PROGMEM
#endif

typedef struct {
  uint16_t bitmapOffset;
  uint8_t width;
  uint8_t height;
  uint8_t xAdvance;
  int8_t xOffset;
  int8_t yOffset;
} GFXglyph;

typedef struct {
  uint8_t *bitmap;
  GFXglyph *glyph;
  uint16_t first;
  uint16_t last;
  uint8_t yAdvance;
} GFXfont;

#endif
