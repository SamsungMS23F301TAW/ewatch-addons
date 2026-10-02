// FaceRender — the watch face styles. Index 8, "Halo", is the anti-aliased v2
// default (face_halo.*); 0..7 are BaseOS's own styles ("Classic" is BaseOS's
// default face), extracted from view.cpp so they compile on the host
// (tools/preview) as well as on the watch, and extended with the four
// configurable data-source slots.
//
// Classic-style layout (240 x 280), top to bottom:
//   corner icons      WiFi (top-left), BLE link (top-centre), power (top-right)
//   SLOT_TOP          y = 68,          text size 2
//   time              y = 100..176,    style-dependent (bitmap / FreeFont / 7-seg)
//   SLOT_UPPER        the stock seconds row  (style's secY,  size 3)
//   SLOT_LOWER        the stock date row     (style's dateY, size 2)
//   SLOT_BOTTOM       y = 250, size 2 (hidden while a stopwatch / timer line shows)
// With the default slot configuration the face is pixel-identical to BaseOS.
//
// The renderer is incremental: it remembers what it drew and repaints only
// rows whose content changed, reporting them as dirty spans so the caller can
// push just those rows from an off-screen canvas.
#pragma once
#include <stdint.h>

#include <gfxfont.h>      // GFXfont (a typedef'd anonymous struct, so no forward decl)

#include "ble_proto.h"
#include "face_slots.h"

class Arduino_GFX;

namespace FaceRender {

static const int16_t kW = 240;
static const int16_t kH = 280;

// ---- Face styles (BaseOS's kWatchFaceStyles plus Halo) ----
static const uint8_t kStyleHalo = 8;        // the v2 face; appended so saved indices keep meaning
static const uint8_t kDefaultStyle = kStyleHalo;
int            styleCount();
bool           styleIsAA(int idx);          // drawn by face_halo (needs a framebuffer)
const char    *styleName(int idx);
const GFXfont *styleUiFont(int idx);        // nullptr = built-in bitmap font
const GFXfont *styleTimeFont(int idx);      // nullptr for bitmap / 7-segment styles

struct RowGeom { int16_t y; uint8_t size; };
RowGeom slotRow(uint8_t slot, uint8_t style);   // clamps invalid styles to 0

// BLE glyph states (match BleConfig::State numerically).
enum BleGlyph : uint8_t { BLE_OFF = 0, BLE_ADVERTISING = 1, BLE_PAIRING = 2, BLE_CONNECTED = 3 };

// Everything one frame needs, as plain data.
struct Frame {
  uint16_t bg, fg, accent, line;
  uint8_t  style;
  uint8_t  options;                          // bleproto::kFaceOpt*
  bool     rtcOk;
  bleproto::CivilTime now;                   // local time
  uint32_t unixNow;                          // UTC, 0 when unknown
  bool     batOk;
  uint8_t  batPct;
  bool     wifiShown;                        // WiFi compiled in
  bool     wifiEnabled, wifiAp, wifiConnected;
  int8_t   wifiRssi;
  uint8_t  ble;                              // BleGlyph
  bool     swRun;  uint32_t swMs;            // stopwatch line
  bool     tmrOn;  uint32_t tmrMs;           // countdown line
  bleproto::SlotCfg          slots[bleproto::kSlotCount];
  const bleproto::TextEntry *texts;          // kTextCount
  const bleproto::FeedEntry *feeds;          // kFeedCount
};

class Renderer {
public:
  struct Span { int16_t y0, y1; };
  static const int kMaxSpans = 8;

  Renderer();
  ~Renderer();
  Renderer(const Renderer &) = delete;
  Renderer &operator=(const Renderer &) = delete;
  void invalidate();                         // everything repaints on the next draw()
  // `fb` is g's framebuffer when g is an off-screen canvas. Halo needs it; if
  // it is null a Halo frame falls back to the Classic style.
  void draw(Arduino_GFX *g, const Frame &f, uint16_t *fb = nullptr);

  // Rows touched by the last draw(): either "full" or up to kMaxSpans spans.
  bool        dirtyFull() const { return dirtyFull_; }
  int         dirtyCount() const { return dirtyN_; }
  const Span &dirtySpan(int i) const { return dirty_[i]; }

private:
  void addDirty(int16_t y0, int16_t y1);
  void drawHalo(uint16_t *fb, const Frame &f);
  void drawTime(Arduino_GFX *g, const Frame &f);
  void drawSlots(Arduino_GFX *g, const Frame &f);
  void drawTimers(Arduino_GFX *g, const Frame &f);
  void drawPowerIcon(Arduino_GFX *g);
  void drawWifiIcon(Arduino_GFX *g, const Frame &f, bool force);
  void drawBleIcon(Arduino_GFX *g, uint8_t state, bool force);

  bool     full_;
  uint16_t bg_, fg_, accent_, line_;
  uint8_t  style_, options_;
  struct { uint8_t h, m; bool rtcOk; } time_;
  faceslots::SlotContent slot_[bleproto::kSlotCount];
  bool     slotValid_[bleproto::kSlotCount];
  bool     swRun_, tmrOn_;
  uint32_t swMs_, tmrMs_;
  struct { bool shown, en, ap, conn; int8_t bars; } wifi_;
  uint8_t  ble_;
  uint8_t  bottomMode_;                      // 0 = bottom slot owns rows 236..280, 1 = timer lines
  struct HaloState;                          // face_halo's last scene (opaque here)
  HaloState *halo_;
  bool     haloValid_;

  bool dirtyFull_;
  int  dirtyN_;
  Span dirty_[kMaxSpans];
};

}  // namespace FaceRender
