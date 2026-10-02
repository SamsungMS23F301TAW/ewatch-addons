// Halo — the v2 default watch face, drawn with the anti-aliased engine (aa_gfx).
//
// Art direction: the theme's accent becomes light. A soft halo of it glows
// behind large Inter Display numerals, a faint rim light warms the bottom
// edge, and the four data slots are set as quiet typographic complications:
// small-caps dates, icon + value lines, a seconds track with a glowing knob,
// and one glass card for the bottom slot. Colours are derived from the user's
// four theme colours with integer maths (palette()), corrected for contrast so
// every theme stays legible.
//
// Pure C++11 and mirrored line for line in web/index.html ("HALO MIRROR"); the
// Node tests diff the two against frames rendered by tools/preview.
//
// The renderer is region based: buildScene() lays out everything a frame
// shows, paint() draws whatever intersects the surface's clip. Each element
// stays inside its own band of rows (regionRows), so FaceRender::Renderer can
// repaint just the bands whose content changed and get the same pixels as a
// full repaint.
#pragma once
#include <stdint.h>

#include "aa_gfx.h"
#include "face_render.h"

namespace halo {

struct Palette {
  aa::Rgb bg;        // theme background
  aa::Rgb text;      // foreground, contrast-corrected
  aa::Rgb ink;       // accent for text and strokes, contrast-corrected
  aa::Rgb dim;       // secondary (theme "line" colour), contrast-corrected
  aa::Rgb vivid;     // accent pushed to full brightness: the light source
  bool    light;     // light background
};
Palette palette(uint16_t bg, uint16_t fg, uint16_t accent, uint16_t line);
// `c` moved towards white (dark bg) or black (light bg) until its luma differs
// from bg's by at least minDiff.
aa::Rgb readable(aa::Rgb c, aa::Rgb bg, int minDiff);
// Gradient + accent glow centred at (120, glowY), plus a rim light at the
// bottom. Shared with the Companion screens so they feel like one product.
void backdrop(aa::Surface &s, const Palette &p, int glowY);
// Glass card: soft shadow, translucent graded fill, hairline edge.
void card(aa::Surface &s, const Palette &p, int x, int y, int w, int h, int r);

enum SlotKind : uint8_t { K_NONE = 0, K_TEXT, K_DATE, K_SECONDS, K_BATTERY };

struct Slot {
  uint8_t kind;
  uint8_t color;          // bleproto::SlotColor
  uint8_t icon;           // feed icon 1..15, or faceslots::kIconBattery
  uint8_t iconArg;        // battery percent (255 unknown)
  uint8_t sec;            // K_SECONDS
  uint8_t len;            // glyph count
  uint8_t split;          // glyphs [split, len) are a countdown suffix
  uint8_t pad;
  uint8_t glyphs[48];
};

struct Scene {
  Palette  pal;
  uint16_t theme[4];
  uint8_t  time[6];       // "HH:MM" / "H:MM" / "--:--" glyphs
  uint8_t  timeLen;
  uint8_t  timeOk;
  uint8_t  wifi;          // 0 hidden, 1 off, 2 searching, 3 hosting setup, 4 connected
  uint8_t  wifiBars;
  uint8_t  ble;           // FaceRender::BleGlyph
  uint8_t  bottomMode;    // 0 bottom slot, 1 stopwatch / timer lines
  uint8_t  swRun, tmrOn;
  uint32_t swMs, tmrMs;
  Slot     slots[4];
};

enum Region : uint8_t { R_STATUS, R_TOP, R_TIME, R_UPPER, R_LOWER, R_BOTTOM, R_COUNT };
void regionRows(int region, int &y0, int &y1);

void buildScene(const FaceRender::Frame &f, Scene &sc);   // zero-initialises sc
bool sameRegion(const Scene &a, const Scene &b, int region);
// Text box of the stopwatch (line 0) or timer (line 1) inside the bottom
// card, so a running clock repaints only its digits. False when not shown.
bool timerBox(const Scene &sc, int line, int &x0, int &y0, int &x1, int &y1);
void paint(aa::Surface &s, const Scene &sc);

}  // namespace halo
