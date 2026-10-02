// Face data-source slots: what each of the four configurable rows of the
// watch face shows, as glyphs ready for the 6x8 bitmap font.
//
// Pure C++11 (no Arduino), shared by:
//   * the watch face renderer (src/apps/face/face_render.cpp),
//   * the host unit tests (test/test_protocol), and
//   * mirrored line-for-line in web/index.html so the browser preview shows
//     exactly what the watch will show. Shared vectors keep them in step.
//
// Layout model
//   A slot row has a natural text size (2 or 3) and a width in pixels. The
//   slot's text is laid out at the natural size if it fits; otherwise at size
//   2; otherwise it is truncated with "..". Battery and feed slots may carry a
//   vector icon drawn to the left of the text.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "ble_proto.h"

namespace faceslots {

// Glyph capacity of one slot (a full-width size-1 row would be 40 glyphs).
static const uint8_t kMaxGlyphs = 40;
static const uint8_t kMinSize = 2;           // never shrink below text size 2

// Icon id used for the battery glyph (feed icons use bleproto::Icon 1..15).
static const uint8_t kIconBattery = 0x80;

// Map UTF-8 text to glyph codes of the CP437-layout 6x8 font. Printable ASCII
// passes through; common Latin-1 letters and symbols (° £ é ü ñ ß ½ ...),
// arrows, card suits and a few dingbats map to their CP437 glyphs; zero-width
// characters (ZWJ, variation selectors) are dropped; "…" becomes "...";
// anything else becomes '?'. Malformed UTF-8 yields one '?' per bad byte.
// Never emits 0x00, '\n' (0x0A) or '\r' (0x0D), which GFX treats as controls.
// Writes at most `cap` glyphs plus a NUL; returns the glyph count.
size_t utf8ToGlyphs(const char *utf8, size_t len, uint8_t *out, size_t cap);

// Everything the formatter needs, as a plain snapshot (no locks inside).
struct FaceData {
  bool     rtcOk;
  bleproto::CivilTime now;          // local time read from the RTC
  uint32_t unixNow;                 // UTC seconds; 0 when the RTC is not OK
  bool     batOk;
  uint8_t  batPct;
  const bleproto::TextEntry *texts; // bleproto::kTextCount entries
  const bleproto::FeedEntry *feeds; // bleproto::kFeedCount entries
};

// The laid-out content of one slot. Equality = nothing to redraw.
struct SlotContent {
  uint8_t glyphs[kMaxGlyphs + 1];   // NUL-terminated glyph codes
  uint8_t len;                      // glyph count, 0 = nothing to draw
  uint8_t icon;                     // 0, bleproto::Icon, or kIconBattery
  uint8_t iconArg;                  // battery percent (255 = unknown)
  uint8_t color;                    // bleproto::SlotColor
  uint8_t size;                     // text size used (0 when empty)
  bool    truncated;
};
bool sameContent(const SlotContent &a, const SlotContent &b);

// Pixel metrics shared by the renderer and the web preview.
int16_t iconWidth(uint8_t icon, uint8_t size);   // 0 for no icon
int16_t iconGap(uint8_t size);                   // space between icon and text
// Width the content occupies when drawn (icon + gap + glyphs * 6 * size).
int16_t contentWidth(const SlotContent &c);

// A slot's content before any fitting: `body` glyphs, optionally followed by a
// countdown `suffix` ("in 25m") that truncation must keep. layoutSlot() fits it
// to the 6x8 bitmap font; the anti-aliased Halo face fits it by pixel width.
struct SlotRaw {
  uint8_t source;                   // bleproto::Source actually shown (0 = nothing)
  uint8_t body[kMaxGlyphs + 1];
  uint8_t bodyLen;
  char    suffix[16];
  uint8_t suffixLen;
  bool    countdown;
  uint8_t icon;                     // 0, bleproto::Icon, or kIconBattery
  uint8_t iconArg;                  // battery percent (255 = unknown)
  uint8_t color;                    // bleproto::SlotColor
};
// False when the slot shows nothing (source NONE, no clock for a date, an
// expired feed, an empty text...). `out` is always initialised.
bool slotRaw(const bleproto::SlotCfg &cfg, const FaceData &d, SlotRaw &out);

// Lay out slot `cfg` for a row whose natural text size is `rowSize` and whose
// usable width is `width` pixels.
void layoutSlot(const bleproto::SlotCfg &cfg, const FaceData &d,
                uint8_t rowSize, int16_t width, SlotContent &out);

// Individual formatters (exposed for tests and the preview). Each writes
// NUL-terminated ASCII/glyph text and returns its length.
size_t formatDate(const bleproto::CivilTime &t, bool shortForm, char *out, size_t cap);
// "in 25m", "in 3h5m", "in 2d", "now" for a target `deltaSec` seconds away.
size_t formatCountdown(int64_t deltaSec, char *out, size_t cap);
// True when a feed has content that should be shown at `unixNow`.
bool   feedVisible(const bleproto::FeedEntry &f, uint32_t unixNow);

}  // namespace faceslots
