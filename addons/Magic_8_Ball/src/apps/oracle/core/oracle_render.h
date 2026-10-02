// Shake Oracle pixel renderer. Portable C++ (no Arduino).
//
// Draws straight into an RGB565 frame buffer (240 x 280, row-major, native
// endianness, i.e. Arduino_Canvas::getFramebuffer()). Two layers:
//
//   drawBall()   the static glossy ball, bevel and printed pack label. Whole
//                screen; rendered once and cached, then restored by memcpy.
//   drawWindow() everything inside the glass: liquid, murk, sediment, die,
//                bubbles, prompt, glare. Touches rows [kWinTop, kWinBottom)
//                only, so the caller can push just those rows to the panel.
//
// All per-pixel work is integer; per-object set-up is float. Colours are
// mixed at 8.8 fixed point and ordered-dithered down to RGB565, which keeps
// the dark blue gradients free of banding.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "gfxfont.h"
#include "oracle_config.h"
#include "text_layout.h"

namespace oracle {

struct Bubble {
  float x = 0, y = 0;   // screen px (centre)
  float r = 2;          // radius px
  float a = 1;          // opacity 0..1
  bool  front = false;  // in front of the die
  bool  spark = false;  // golden sparkle instead of a bubble
};

struct Mote {
  float x = 0, y = 0;   // screen px
  float a = 0.3f;       // opacity 0..1
};

// Everything that changes from frame to frame.
struct FrameState {
  float t = 0;                 // seconds, for idle shimmer

  bool  dieVisible = false;
  bool  dieGold = false;
  float dieX = kBallCX, dieY = kBallCY;   // screen position of the centroid
  float dieAngle = 0;          // radians, clockwise on screen
  float dieScale = 1;          // 1 = pressed against the glass
  float dieSquash = 1;         // 3-D tumble foreshortening, 0.08..1
  float dieSquashAxis = 0;     // radians, axis the squash acts along
  float dieFog = 0;            // 0 crisp .. 1 lost in the murk
  float dieTextAlpha = 1;      // answer text visibility
  float dieGlint = -1;         // gold sweep position 0..1, < 0 for none
  float dieFlash = 0;          // 0..1 brief brightening when it lands

  float murkX = 0, murkY = 0;  // drift of the first murk layer (px)
  float swirl = 0;             // rotation of the second layer (radians)
  float murk = 0.5f;           // murk contrast 0..1
  float stir = 0;              // 0..1 extra darkness while churned up

  float promptAlpha = 0;       // idle prompt visibility
  float promptDy = 0;          // prompt bob (px)

  float glareDx = 0, glareDy = 0;  // glass reflection parallax (px)

  const Bubble *bubbles = nullptr;
  int           bubbleCount = 0;
  const Mote   *motes = nullptr;
  int           moteCount = 0;
};

class Renderer {
 public:
  // `hot` marks tables read for every pixel of every frame; the watch puts
  // those in internal RAM when it can spare it, everything else in PSRAM.
  using AllocFn = void *(*)(size_t bytes, bool hot);

  // Allocates the caches (~330 KB in all). `alloc` defaults to calloc. Safe
  // to call again; it only allocates once.
  bool begin(const GFXfont *font, AllocFn alloc = nullptr);
  bool ready() const { return ready_; }

  // Render the static ball into the cache (expensive: only the first time),
  // print the pack label and page dots on it, and copy it all to `fb`.
  void drawBall(uint16_t *fb, const char *packName, int packIndex, int packCount);
  // Reprint just the label strip (cheap) and copy that strip into `fb`.
  // Returns the strip's first row and row count through y0/rows.
  void setPackLabel(uint16_t *fb, const char *packName, int packIndex, int packCount,
                    int &y0, int &rows);
  // Copy the cached ball into `fb` (cheap). Returns false if never drawn.
  bool restoreBall(uint16_t *fb) const;

  // Fit and rasterise the answer onto the die. Returns false if the text
  // could not fit at the minimum size (it is then drawn at that size anyway).
  bool setDieText(const char *text);
  const TextLayout &dieLayout() const { return dieLayout_; }

  // Fit and rasterise the idle prompt (mixed case).
  bool setPrompt(const char *text);

  void drawWindow(uint16_t *fb, const FrameState &s);

  // Die sprite geometry (for tests): sprite size and the centroid's texel.
  int   spriteW() const { return spriteW_; }
  int   spriteH() const { return spriteH_; }
  float spriteOX() const { return spriteOX_; }
  float spriteOY() const { return spriteOY_; }
  // Packed texel: byte 0 coverage, 1 shade, 2 text, 3 glow.
  const uint32_t *sprite() const { return sprite_; }

 private:
  bool  ready_ = false;
  const GFXfont *font_ = nullptr;

  // Static ball, RGB565, whole screen, plus a clean copy of the label strip.
  uint16_t *ball_ = nullptr;
  uint16_t *labelBase_ = nullptr;
  bool      ballValid_ = false;

  // Per-pixel window maps over the window's bounding square. light_: low
  // byte = light (vignette and rim shadow), high byte = edge coverage.
  // glare_: reflection on the glass, sampled with a small parallax offset.
  uint16_t *light_ = nullptr;
  uint8_t  *glare_ = nullptr;
  int       mapX0_ = 0, mapY0_ = 0, mapN_ = 0;

  // Tileable smooth noise for the murk, 128 x 128.
  uint8_t  *noise_ = nullptr;

  // Die sprite in die-local space (scale 1).
  uint32_t *sprite_ = nullptr;
  int       spriteW_ = 0, spriteH_ = 0;
  float     spriteOX_ = 0, spriteOY_ = 0;
  TextLayout dieLayout_;

  // Idle prompt: byte 0 text, byte 1 glow.
  uint16_t *prompt_ = nullptr;
  int       promptW_ = 0, promptH_ = 0;
  bool      promptValid_ = false;

  void buildWindowMap();
  void buildNoise();
  void buildDieShape();
  void renderBallPixels(uint16_t *dst);
  void printPackLabel(uint16_t *dst, const char *name, int index, int count);
};

// Launcher tile artwork: a small anti-aliased oracle ball, size x size RGB565
// pixels, blended onto `bg565` (the tile colour) at the edges.
void renderTileIcon(uint16_t *dst, int size, uint16_t bg565);

// RGB565 helpers shared with the view and tests.
inline uint16_t rgb565(int r, int g, int b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

}  // namespace oracle
