// FaceRender — see face_render.h. The style table, styled text, 7-segment
// digits, corner icons and stopwatch/timer lines are BaseOS's own code moved
// here unchanged in behaviour; the slot rows and BLE glyph are new.
//
// The web preview (web/index.html, "FACE MIRROR") re-implements this file on
// top of a port of the same Arduino_GFX primitives; test/web/ diff-checks the
// two against frames rendered by tools/preview.
#include "face_render.h"

#include <Arduino_GFX.h>
#include <stdio.h>
#include <string.h>

#include "FreeMono12pt7b.h"
#include "FreeMono24pt7b.h"
#include "FreeSans12pt7b.h"
#include "FreeSans24pt7b.h"
#include "FreeSansBold12pt7b.h"
#include "FreeSansBold24pt7b.h"
#include "FreeSerifBold12pt7b.h"
#include "FreeSerifBold24pt7b.h"
#include "aa_gfx.h"
#include "face_halo.h"
#include "face_icons.h"

using namespace bleproto;

namespace FaceRender {

// RGB565 constants used by the stock face (kept literal so the host build and
// the web mirror don't depend on Arduino_GFX's colour macros).
static const uint16_t C_RED      = 0xF800;
static const uint16_t C_GREEN    = 0x07E0;
static const uint16_t C_ORANGE   = 0xFD20;
static const uint16_t C_YELLOW   = 0xFFE0;
static const uint16_t C_DARKGREY = 0x7BEF;
static const uint16_t C_BLE_BLUE = 0x2D7F;

// =====================================================================
// Watch face style profiles (BaseOS). Bitmap styles: timeY is the top of
// the row. FreeFont styles: timeY is the baseline.
// =====================================================================
enum class Effect : uint8_t { Plain = 0, Bold = 1, Outline = 2, Shadow = 3 };

struct Style {
  const char    *name;
  const GFXfont *timeFont;
  const GFXfont *uiFont;
  uint8_t  timeSize, secSize, dateSize;
  int16_t  timeY, secY, dateY;
  Effect   effect;
  bool     digital;
};

static const Style kStyles[] = {
  // name     timeFont               uiFont                tsz ssz dsz timeY secY dateY effect           digital
  { "Classic", nullptr,              nullptr,                6,  3,  2,  108, 174, 212, Effect::Plain,   false },
  { "Sans",    &FreeSans24pt7b,      &FreeSans12pt7b,        1,  3,  2,  146, 186, 220, Effect::Plain,   false },
  { "Bold",    &FreeSansBold24pt7b,  &FreeSansBold12pt7b,    1,  3,  2,  146, 186, 220, Effect::Plain,   false },
  { "Serif",   &FreeSerifBold24pt7b, &FreeSerifBold12pt7b,   1,  3,  2,  146, 186, 220, Effect::Plain,   false },
  { "Mono",    &FreeMono24pt7b,      &FreeMono12pt7b,        1,  3,  2,  146, 186, 220, Effect::Plain,   false },
  { "Digital", nullptr,              nullptr,                0,  3,  2,  102, 186, 220, Effect::Plain,   true  },
  { "Outline", nullptr,              nullptr,                6,  3,  2,  108, 174, 212, Effect::Outline, false },
  { "Shadow",  nullptr,              nullptr,                6,  3,  2,  108, 174, 212, Effect::Shadow,  false },
  // Halo (face_halo.cpp). The geometry here only matters for the Classic
  // fallback; the UI font is used by BaseOS's title bars.
  { "Halo",    nullptr,              &FreeSans12pt7b,        6,  3,  2,  108, 174, 212, Effect::Plain,   false },
};
static const int kStyleCount = (int)(sizeof(kStyles) / sizeof(kStyles[0]));

static const Style &styleFor(uint8_t idx) {
  return kStyles[idx < kStyleCount ? idx : 0];
}

int styleCount() { return kStyleCount; }
bool styleIsAA(int idx) { return idx == kStyleHalo; }
const char *styleName(int idx) { return (idx >= 0 && idx < kStyleCount) ? kStyles[idx].name : ""; }
const GFXfont *styleUiFont(int idx) { return (idx >= 0 && idx < kStyleCount) ? kStyles[idx].uiFont : nullptr; }
const GFXfont *styleTimeFont(int idx) { return (idx >= 0 && idx < kStyleCount) ? kStyles[idx].timeFont : nullptr; }

static const int16_t kTopSlotY = 68;
static const int16_t kBottomSlotY = 250;

RowGeom slotRow(uint8_t slot, uint8_t style) {
  const Style &s = styleFor(style);
  RowGeom r;
  switch (slot) {
    case SLOT_TOP:    r.y = kTopSlotY;    r.size = 2;          break;
    case SLOT_UPPER:  r.y = s.secY;       r.size = s.secSize;  break;
    case SLOT_LOWER:  r.y = s.dateY;      r.size = s.dateSize; break;
    default:          r.y = kBottomSlotY; r.size = 2;          break;
  }
  return r;
}

// =====================================================================
// Text helpers (BaseOS)
// =====================================================================
static int16_t centerX(const char *s, uint8_t size) {
  return (int16_t)((kW - (int16_t)strlen(s) * 6 * size) / 2);
}

// Bitmap-font text with a per-style effect, drawn transparently over a band
// the caller already cleared.
static void drawStyledText(Arduino_GFX *g, int16_t x, int16_t y, uint8_t size,
                           uint16_t fg, uint16_t bg, Effect ef, const char *text) {
  g->setTextSize(size);
  switch (ef) {
    case Effect::Plain:
      g->setTextColor(fg);
      g->setCursor(x, y);
      g->print(text);
      break;
    case Effect::Bold:
      g->setTextColor(fg);
      g->setCursor(x, y);     g->print(text);
      g->setCursor(x + 1, y); g->print(text);
      break;
    case Effect::Outline:
      g->setTextColor(fg);
      for (int8_t dx = -1; dx <= 1; dx++)
        for (int8_t dy = -1; dy <= 1; dy++) {
          if (dx == 0 && dy == 0) continue;
          g->setCursor(x + dx, y + dy);
          g->print(text);
        }
      g->setTextColor(bg);
      g->setCursor(x, y);
      g->print(text);
      break;
    case Effect::Shadow:
      g->setTextColor(C_DARKGREY);
      g->setCursor(x + 3, y + 3);
      g->print(text);
      g->setTextColor(fg);
      g->setCursor(x, y);
      g->print(text);
      break;
  }
}

// =====================================================================
// 7-segment HH:MM (BaseOS)
// =====================================================================
static void drawHSeg(Arduino_GFX *g, int16_t x, int16_t y, int16_t w, int16_t t, uint16_t color) {
  g->fillRect(x + t / 2, y, w - t, t, color);
  for (int16_t i = 0; i < t / 2; i++) {
    g->fillRect(x + (t / 2) - i - 1, y + i, 1, t - 2 * i, color);
    g->fillRect(x + w - (t / 2) + i, y + i, 1, t - 2 * i, color);
  }
}

static void drawVSeg(Arduino_GFX *g, int16_t x, int16_t y, int16_t h, int16_t t, uint16_t color) {
  g->fillRect(x, y + t / 2, t, h - t, color);
  for (int16_t i = 0; i < t / 2; i++) {
    g->fillRect(x + i, y + (t / 2) - i - 1, t - 2 * i, 1, color);
    g->fillRect(x + i, y + h - (t / 2) + i, t - 2 * i, 1, color);
  }
}

static void draw7SegDigit(Arduino_GFX *g, int16_t x, int16_t y, int16_t dw, int16_t dh,
                          int16_t t, uint8_t segs, uint16_t color) {
  int16_t halfH = dh / 2;
  if (segs & 0x40) drawHSeg(g, x, y, dw, t, color);
  if (segs & 0x20) drawVSeg(g, x + dw - t, y, halfH + t / 2, t, color);
  if (segs & 0x10) drawVSeg(g, x + dw - t, y + halfH - t / 2, halfH + t / 2, t, color);
  if (segs & 0x08) drawHSeg(g, x, y + dh - t, dw, t, color);
  if (segs & 0x04) drawVSeg(g, x, y + halfH - t / 2, halfH + t / 2, t, color);
  if (segs & 0x02) drawVSeg(g, x, y, halfH + t / 2, t, color);
  if (segs & 0x01) drawHSeg(g, x, y + halfH - t / 2, dw, t, color);
}

static const uint8_t k7SegDigit[10] = {
  0x7E, 0x30, 0x6D, 0x79, 0x33, 0x5B, 0x5F, 0x70, 0x7F, 0x7B,
};

// `blankLead`: 12-hour clock before 10 o'clock shows only the ghost segments
// in the first position.
static void draw7SegTime(Arduino_GFX *g, int16_t topY, uint8_t h, uint8_t m,
                         uint16_t color, uint16_t bg, bool blankLead) {
  const int16_t dw = 40, dh = 70, t = 8;
  const int16_t gap = 8;
  const int16_t colW = 18;
  int16_t totalW = dw * 4 + gap * 3 + colW;
  int16_t x = (kW - totalW) / 2;
  g->fillRect(0, topY - 2, kW, dh + 6, bg);

  uint16_t ghost = C_DARKGREY;
  uint8_t d0 = h / 10, d1 = h % 10, d2 = m / 10, d3 = m % 10;
  draw7SegDigit(g, x,                            topY, dw, dh, t, 0x7F, ghost);
  draw7SegDigit(g, x + dw + gap,                 topY, dw, dh, t, 0x7F, ghost);
  int16_t colX = x + 2 * (dw + gap);
  draw7SegDigit(g, colX + colW,                  topY, dw, dh, t, 0x7F, ghost);
  draw7SegDigit(g, colX + colW + dw + gap,       topY, dw, dh, t, 0x7F, ghost);

  if (!blankLead) draw7SegDigit(g, x, topY, dw, dh, t, k7SegDigit[d0], color);
  draw7SegDigit(g, x + dw + gap,                 topY, dw, dh, t, k7SegDigit[d1], color);
  int16_t dot = t;
  int16_t dotX = colX + (colW - dot) / 2;
  g->fillRect(dotX, topY + dh / 3 - dot / 2,       dot, dot, color);
  g->fillRect(dotX, topY + (2 * dh) / 3 - dot / 2, dot, dot, color);
  draw7SegDigit(g, colX + colW,                  topY, dw, dh, t, k7SegDigit[d2], color);
  draw7SegDigit(g, colX + colW + dw + gap,       topY, dw, dh, t, k7SegDigit[d3], color);
}

// "prefix" + H:MM:SS.mmm / MM:SS.mmm (BaseOS)
static void fmtClockMs(char *buf, size_t n, const char *prefix, uint32_t ms) {
  uint32_t totalSec = ms / 1000u;
  uint32_t msPart = ms % 1000u;
  uint32_t hh = totalSec / 3600u;
  uint32_t mm = (totalSec % 3600u) / 60u;
  uint32_t ss = totalSec % 60u;
  if (hh > 0) snprintf(buf, n, "%s%lu:%02lu:%02lu.%03lu", prefix, (unsigned long)hh,
                       (unsigned long)mm, (unsigned long)ss, (unsigned long)msPart);
  else        snprintf(buf, n, "%s%02lu:%02lu.%03lu", prefix, (unsigned long)mm,
                       (unsigned long)ss, (unsigned long)msPart);
}

// =====================================================================
// Renderer
// =====================================================================
struct Renderer::HaloState { halo::Scene scene; };

Renderer::Renderer() : halo_(new HaloState()), haloValid_(false) { invalidate(); }
Renderer::~Renderer() { delete halo_; }

void Renderer::invalidate() {
  full_ = true;
  bg_ = fg_ = accent_ = line_ = 0;
  style_ = 0xFF;
  options_ = 0xFF;
  time_.h = 99; time_.m = 99; time_.rtcOk = false;
  for (uint8_t i = 0; i < kSlotCount; i++) slotValid_[i] = false;
  swRun_ = false; tmrOn_ = false; swMs_ = 0xFFFFFFFF; tmrMs_ = 0xFFFFFFFF;
  wifi_.shown = false; wifi_.en = false; wifi_.ap = false; wifi_.conn = false; wifi_.bars = -1;
  ble_ = 0xFF;
  bottomMode_ = 0xFF;
  haloValid_ = false;
  dirtyFull_ = false;
  dirtyN_ = 0;
}

void Renderer::addDirty(int16_t y0, int16_t y1) {
  if (dirtyFull_) return;
  if (y0 < 0) y0 = 0;
  if (y1 > kH) y1 = kH;
  if (y1 <= y0) return;
  // Merge with an overlapping / touching span.
  for (int i = 0; i < dirtyN_; i++) {
    if (y0 <= dirty_[i].y1 && y1 >= dirty_[i].y0) {
      if (y0 < dirty_[i].y0) dirty_[i].y0 = y0;
      if (y1 > dirty_[i].y1) dirty_[i].y1 = y1;
      return;
    }
  }
  if (dirtyN_ == kMaxSpans) { dirtyFull_ = true; return; }
  dirty_[dirtyN_].y0 = y0;
  dirty_[dirtyN_].y1 = y1;
  dirtyN_++;
}

void Renderer::draw(Arduino_GFX *g, const Frame &f, uint16_t *fb) {
  dirtyFull_ = false;
  dirtyN_ = 0;
  if (!g) return;
  if (styleIsAA(f.style)) {
    if (fb) { drawHalo(fb, f); return; }
    Frame fallback = f;                      // no framebuffer: draw Classic
    fallback.style = 0;
    draw(g, fallback, nullptr);
    return;
  }
  haloValid_ = false;

  // A colour, style or option change repaints everything (BaseOS behaviour).
  if (full_ || f.bg != bg_ || f.fg != fg_ || f.accent != accent_ || f.line != line_ ||
      f.style != style_ || f.options != options_) {
    g->fillScreen(f.bg);
    bg_ = f.bg; fg_ = f.fg; accent_ = f.accent; line_ = f.line;
    style_ = f.style; options_ = f.options;
    full_ = false;
    time_.h = 99; time_.m = 99;
    for (uint8_t i = 0; i < kSlotCount; i++) slotValid_[i] = false;
    swMs_ = 0xFFFFFFFF; tmrMs_ = 0xFFFFFFFF; swRun_ = false; tmrOn_ = false;
    bottomMode_ = (f.swRun || f.tmrOn) ? 1 : 0;
    drawPowerIcon(g);
    drawWifiIcon(g, f, true);
    drawBleIcon(g, f.ble, true);
    dirtyFull_ = true;
  } else {
    drawWifiIcon(g, f, false);
    drawBleIcon(g, f.ble, false);
  }

  // Rows 236..280 belong either to the bottom slot or to BaseOS's stopwatch /
  // timer lines. Hand them over cleanly when that changes.
  uint8_t mode = (f.swRun || f.tmrOn) ? 1 : 0;
  if (mode != bottomMode_) {
    g->fillRect(0, 236, kW, kH - 236, bg_);
    addDirty(236, kH);
    bottomMode_ = mode;
    slotValid_[SLOT_BOTTOM] = false;
    swRun_ = false; tmrOn_ = false; swMs_ = 0xFFFFFFFF; tmrMs_ = 0xFFFFFFFF;
  }

  // Slots may grow over GFX's right edge in the Shadow effect; never wrap.
  g->setTextWrap(false);
  drawTime(g, f);
  drawSlots(g, f);
  if (bottomMode_ == 1) drawTimers(g, f);
  g->setTextWrap(true);
}

// Halo: lay the frame out, then repaint only the row bands whose content
// changed. paint() honours the clip and every element stays inside its band,
// so a partial repaint equals a full one (tools/preview checks this).
void Renderer::drawHalo(uint16_t *fb, const Frame &f) {
  halo::Scene sc;
  halo::buildScene(f, sc);
  aa::Surface s;
  aa::init(s, fb, kW, kH);
  halo::Scene &old = halo_->scene;
  bool full = !haloValid_ || memcmp(sc.theme, old.theme, sizeof(sc.theme)) != 0 ||
              f.style != style_ || f.options != options_;
  if (full) {
    halo::paint(s, sc);
    dirtyFull_ = true;
  } else {
    for (int r = 0; r < halo::R_COUNT; r++) {
      if (halo::sameRegion(sc, old, r)) continue;
      if (r == halo::R_BOTTOM && sc.bottomMode && old.bottomMode && sc.swRun == old.swRun &&
          sc.tmrOn == old.tmrOn) {
        // Only the running stopwatch / timer digits changed: repaint just those.
        for (int line = 0; line < 2; line++) {
          bool changed = line == 0 ? sc.swMs != old.swMs : sc.tmrMs != old.tmrMs;
          int x0, y0, x1, y1;
          if (!changed || !halo::timerBox(sc, line, x0, y0, x1, y1)) continue;
          aa::setClip(s, x0, y0, x1, y1);
          halo::paint(s, sc);
          addDirty((int16_t)y0, (int16_t)y1);
        }
        continue;
      }
      int y0, y1;
      halo::regionRows(r, y0, y1);
      aa::setClip(s, 0, y0, kW, y1);
      halo::paint(s, sc);
      addDirty((int16_t)y0, (int16_t)y1);
    }
  }
  old = sc;
  haloValid_ = true;
  // The Classic path must repaint everything if the style changes back.
  full_ = true;
  style_ = f.style;
  options_ = f.options;
}

void Renderer::drawTime(Arduino_GFX *g, const Frame &f) {
  bool twelve = (f.options & kFaceOpt12h) != 0;
  uint8_t h = f.now.hour;
  if (twelve) { h = (uint8_t)(h % 12); if (h == 0) h = 12; }
  if (h == time_.h && f.now.minute == time_.m && f.rtcOk == time_.rtcOk) return;
  const Style &fs = styleFor(f.style);

  if (fs.digital) {
    if (f.rtcOk) {
      draw7SegTime(g, fs.timeY, h, f.now.minute, fg_, bg_, twelve && h < 10);
      addDirty((int16_t)(fs.timeY - 2), (int16_t)(fs.timeY + 74));
    } else {
      g->fillRect(0, 100, kW, 68, bg_);
      g->setFont(nullptr);
      g->setTextSize(6);
      g->setTextColor(C_RED, bg_);
      const char *buf = "--:--";
      g->setCursor(centerX(buf, 6), 108);
      g->print(buf);
      addDirty(100, 168);
    }
  } else {
    g->fillRect(0, 100, kW, 68, bg_);
    char buf[8];
    if (!f.rtcOk)   strcpy(buf, "--:--");
    else if (twelve) snprintf(buf, sizeof(buf), "%u:%02u", (unsigned)h, (unsigned)f.now.minute);
    else            snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)h, (unsigned)f.now.minute);
    uint16_t col = f.rtcOk ? fg_ : C_RED;
    if (fs.timeFont) {
      g->setFont(fs.timeFont);
      g->setTextSize(fs.timeSize);
      g->setTextColor(col);
      int16_t x1, y1;
      uint16_t tw, th;
      g->getTextBounds(buf, 0, fs.timeY, &x1, &y1, &tw, &th);
      int16_t cx = (int16_t)((kW - (int16_t)tw) / 2 - x1);
      g->setCursor(cx, fs.timeY);
      g->print(buf);
      g->setFont(nullptr);
    } else {
      drawStyledText(g, centerX(buf, fs.timeSize), fs.timeY, fs.timeSize, col, bg_, fs.effect, buf);
    }
    addDirty(100, 168);
  }
  time_.h = h;
  time_.m = f.now.minute;
  time_.rtcOk = f.rtcOk;
}

static uint16_t slotColor(uint8_t c, uint16_t fg, uint16_t accent, uint16_t line) {
  return c == COLOR_ACCENT ? accent : (c == COLOR_DIM ? line : fg);
}

void Renderer::drawSlots(Arduino_GFX *g, const Frame &f) {
  const Style &fs = styleFor(f.style);
  faceslots::FaceData d;
  d.rtcOk = f.rtcOk;
  d.now = f.now;
  d.unixNow = f.unixNow;
  d.batOk = f.batOk;
  d.batPct = f.batPct;
  d.texts = f.texts;
  d.feeds = f.feeds;
  int16_t pad = (fs.effect == Effect::Shadow) ? 4 : 2;

  for (uint8_t i = 0; i < kSlotCount; i++) {
    if (i == SLOT_BOTTOM && bottomMode_ == 1) continue;   // timer lines own the rows
    RowGeom rg = slotRow(i, f.style);
    faceslots::SlotContent c;
    faceslots::layoutSlot(f.slots[i], d, rg.size, kW, c);
    if (slotValid_[i] && faceslots::sameContent(c, slot_[i])) continue;

    int16_t bandY = (int16_t)(rg.y - pad);
    int16_t bandH = (int16_t)(rg.size * 8 + 2 * pad);
    g->fillRect(0, bandY, kW, bandH, bg_);
    if (c.len > 0) {
      uint16_t col = slotColor(c.color, fg_, accent_, line_);
      int16_t x = (int16_t)((kW - faceslots::contentWidth(c)) / 2);
      // GFX text only clips right/bottom per pixel: the Outline effect's left
      // pass at x-1 must not start off-screen (it would wrap a row up).
      if (fs.effect == Effect::Outline && x < 1) x = 1;
      int16_t y = (int16_t)(rg.y + (rg.size - c.size) * 4);   // centre a shrunk row
      if (c.icon) {
        FaceIcons::draw(g, c.icon, c.iconArg, x, y, c.size, col, bg_);
        x = (int16_t)(x + faceslots::iconWidth(c.icon, c.size) + faceslots::iconGap(c.size));
      }
      drawStyledText(g, x, y, c.size, col, bg_, fs.effect, (const char *)c.glyphs);
    }
    addDirty(bandY, (int16_t)(bandY + bandH));
    slot_[i] = c;
    slotValid_[i] = true;
  }
}

void Renderer::drawTimers(Arduino_GFX *g, const Frame &f) {
  uint32_t swMs = f.swRun ? f.swMs : 0;
  if (f.swRun != swRun_ || swMs != swMs_) {
    if (f.swRun || swRun_) {
      g->fillRect(0, 236, kW, 18, bg_);   // BaseOS line positions
      if (f.swRun) {
        char buf[24];
        fmtClockMs(buf, sizeof(buf), "SW ", swMs);
        g->setTextSize(2);
        g->setTextColor(C_GREEN, bg_);
        g->setCursor(centerX(buf, 2), 238);
        g->print(buf);
      }
      addDirty(236, 254);
    }
    swRun_ = f.swRun;
    swMs_ = swMs;
  }
  uint32_t tmrMs = f.tmrOn ? f.tmrMs : 0;
  if (f.tmrOn != tmrOn_ || tmrMs != tmrMs_) {
    if (f.tmrOn || tmrOn_) {
      g->fillRect(0, 258, kW, 20, bg_);
      if (f.tmrOn) {
        char buf[24];
        fmtClockMs(buf, sizeof(buf), "T-", tmrMs);
        g->setTextSize(2);
        g->setTextColor(C_ORANGE, bg_);
        g->setCursor(centerX(buf, 2), 260);
        g->print(buf);
      }
      addDirty(258, 278);
    }
    tmrOn_ = f.tmrOn;
    tmrMs_ = tmrMs;
  }
}

// Power icon (BaseOS): two-pixel ring, erased top notch, double stem.
void Renderer::drawPowerIcon(Arduino_GFX *g) {
  const int16_t cx = 212, cy = 24, r = 13;
  g->drawCircle(cx, cy, r, fg_);
  g->drawCircle(cx, cy, r - 1, fg_);
  g->fillRect(cx - 2, cy - r - 2, 5, 5, bg_);
  g->drawFastVLine(cx, cy - r + 1, r - 2, fg_);
  g->drawFastVLine(cx + 1, cy - r + 1, r - 2, fg_);
}

// WiFi fan icon (BaseOS): yellow = hosting the setup AP, green with bars =
// connected, orange dot = scanning, grey = off.
void Renderer::drawWifiIcon(Arduino_GFX *g, const Frame &f, bool force) {
  if (!f.wifiShown) return;
  int8_t bars = 0;
  uint16_t col = C_DARKGREY;
  bool en = f.wifiEnabled;
  if (en && f.wifiAp)          { col = C_YELLOW; bars = 3; }
  else if (en && f.wifiConnected) {
    col = C_GREEN;
    if      (f.wifiRssi >= -55) bars = 3;
    else if (f.wifiRssi >= -65) bars = 2;
    else if (f.wifiRssi >= -75) bars = 1;
    else                        bars = 0;
  }
  else if (en)                 { col = C_ORANGE; bars = 0; }

  if (!force && wifi_.shown && wifi_.en == en && wifi_.ap == f.wifiAp &&
      wifi_.conn == f.wifiConnected && wifi_.bars == bars) return;
  wifi_.shown = true; wifi_.en = en; wifi_.ap = f.wifiAp; wifi_.conn = f.wifiConnected; wifi_.bars = bars;

  const int16_t cx = 28, cy = 42, r1 = 5, r2 = 11, r3 = 17;
  g->fillRect(2, 4, 56, 46, bg_);
  uint16_t off = C_DARKGREY;
  uint16_t cDot = en ? col : off;
  uint16_t cA1 = (en && bars >= 1) ? col : off;
  uint16_t cA2 = (en && bars >= 2) ? col : off;
  uint16_t cA3 = (en && bars >= 3) ? col : off;
  g->drawCircle(cx, cy, r1, cA1);
  g->drawCircle(cx, cy, r1 + 1, cA1);
  g->drawCircle(cx, cy, r2, cA2);
  g->drawCircle(cx, cy, r2 + 1, cA2);
  g->drawCircle(cx, cy, r3, cA3);
  g->drawCircle(cx, cy, r3 + 1, cA3);
  g->fillRect(2, cy + 1, 56, 22, bg_);
  g->fillCircle(cx, cy - 2, 2, cDot);
  addDirty(4, 65);
}

// BLE link glyph at the top centre: two nodes joined by a bar. Hollow grey
// while visible, orange while waiting for the code, solid blue when paired.
void Renderer::drawBleIcon(Arduino_GFX *g, uint8_t state, bool force) {
  if (!force && state == ble_) return;
  ble_ = state;
  const int16_t y = 22, xl = 111, xr = 129, r = 4;
  g->fillRect(100, 12, 40, 21, bg_);
  if (state != BLE_OFF) {
    if (state == BLE_ADVERTISING) {
      g->drawCircle(xl, y, r, C_DARKGREY);
      g->drawCircle(xr, y, r, C_DARKGREY);
      for (int16_t x = xl + r + 2; x <= xr - r - 2; x += 3) g->fillRect(x, y - 1, 2, 2, C_DARKGREY);
    } else {
      uint16_t col = (state == BLE_PAIRING) ? C_ORANGE : C_BLE_BLUE;
      g->fillCircle(xl, y, r, col);
      g->fillCircle(xr, y, r, col);
      g->fillRect(xl, y - 1, xr - xl, 3, col);
    }
  }
  addDirty(12, 33);
}

}  // namespace FaceRender
