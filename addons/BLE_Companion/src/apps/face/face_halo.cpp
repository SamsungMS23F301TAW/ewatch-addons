// Halo face — see face_halo.h. Mirrored in web/index.html ("HALO MIRROR").
#include "face_halo.h"

#include <stdio.h>
#include <string.h>

#include "aa_assets.h"
#include "face_slots.h"

using namespace aa;
using namespace bleproto;

namespace halo {

static const Rgb kWhite = { 255, 255, 255 };
static const Rgb kBlack = { 0, 0, 0 };

// ---------------------------------------------------------------------------
// Geometry (240 x 280). Every element stays inside its region's rows.
// ---------------------------------------------------------------------------
static const int kCx = 120;
static const int kTimeBase = 150;           // numerals are 57 px tall
static const int kSlotBase[4] = { 68, 178, 204, 247 };
static const int kLineW = 204;              // slot line width budget
static const int kCardX = 20, kCardY = 222, kCardW = 200, kCardH = 40, kCardR = 20;
static const int kCardInnerW = 172;
static const int kRegion[R_COUNT][2] = {
  { 0, 44 }, { 44, 80 }, { 80, 160 }, { 160, 186 }, { 186, 210 }, { 210, 280 },
};

void regionRows(int region, int &y0, int &y1) {
  y0 = kRegion[region][0];
  y1 = kRegion[region][1];
}

// ---------------------------------------------------------------------------
// Palette, backdrop, card
// ---------------------------------------------------------------------------
Rgb readable(Rgb c, Rgb bg, int minDiff) {
  int lb = luma(bg);
  Rgb target = lb >= 128 ? kBlack : kWhite;
  for (int t = 0; t <= 256; t += 16) {
    Rgb m = mix(c, target, t);
    int d = luma(m) - lb;
    if (d < 0) d = -d;
    if (d >= minDiff) return m;
  }
  return target;
}

Palette palette(uint16_t bg, uint16_t fg, uint16_t accent, uint16_t line) {
  Palette p;
  p.bg = unpack(bg);
  Rgb a = unpack(accent);
  p.light = luma(p.bg) >= 128;
  p.text = readable(unpack(fg), p.bg, 120);
  int m = a.r > a.g ? a.r : a.g;
  if (a.b > m) m = a.b;
  if (m < 24) p.vivid = p.text;
  else p.vivid = rgb((uint8_t)(a.r * 255 / m), (uint8_t)(a.g * 255 / m), (uint8_t)(a.b * 255 / m));
  p.ink = readable(p.light ? a : p.vivid, p.bg, 96);
  p.dim = readable(unpack(line), p.bg, 80);
  return p;
}

void backdrop(Surface &s, const Palette &p, int glowY) {
  Glow g[2];
  Rgb top, bottom;
  int n;
  if (!p.light) {
    top = mix(p.bg, p.vivid, 26);
    bottom = p.bg;
    Glow a = { kCx, (int16_t)glowY, 170, 118, p.vivid, 92 };
    Glow b = { kCx, 336, 230, 120, p.vivid, 44 };
    g[0] = a; g[1] = b;
    n = 2;
  } else {
    top = p.bg;
    bottom = mix(p.bg, p.vivid, 22);
    Glow a = { kCx, (int16_t)glowY, 175, 120, p.vivid, 40 };
    g[0] = a;
    n = 1;
  }
  aa::backdrop(s, 0, 0, s.w, s.h, top, bottom, g, n);
}

void card(Surface &s, const Palette &p, int x, int y, int w, int h, int r) {
  if (!p.light) {
    softShadow(s, x, y + 4, w, h, r, 12, kBlack, 140);
    roundRectVA(s, x, y, w, h, r, p.text, 34, 12);
    roundRectStrokeVA(s, x, y, w, h, r, 1, p.text, 84, 16);
  } else {
    softShadow(s, x, y + 3, w, h, r, 10, mix(p.bg, kBlack, 96), 90);
    roundRectVA(s, x, y, w, h, r, kWhite, 235, 205);
    roundRectStrokeVA(s, x, y, w, h, r, 1, p.text, 14, 34);
  }
}

// ---------------------------------------------------------------------------
// Scene
// ---------------------------------------------------------------------------
static const int kTrack = 2;                // date tracking (px)

static int glyphW(const Font &f, const uint8_t *g, int n, int tracking, uint8_t flags) {
  return textWidth(f, g, n, tracking, flags);
}

// Copy body[0, keep) minus trailing spaces into out, then an ellipsis.
static int clipWithEllipsis(const uint8_t *body, int keep, uint8_t *out) {
  while (keep > 0 && body[keep - 1] == ' ') keep--;
  memcpy(out, body, (size_t)keep);
  out[keep] = 0x1C;
  return keep + 1;
}

// Fit raw content into `avail` px: body (+ ' ' + countdown suffix), shortening
// the body with an ellipsis first and keeping the suffix whole.
static void fit(const Font &f, int tracking, uint8_t flags, const faceslots::SlotRaw &raw,
                int avail, Slot &out) {
  uint8_t buf[48];
  const int bodyLen = raw.bodyLen > 40 ? 40 : raw.bodyLen;
  const int sufLen = raw.countdown ? raw.suffixLen : 0;
  for (int keep = bodyLen; keep >= 0; keep--) {
    int n = 0;
    if (keep == bodyLen) {
      memcpy(buf, raw.body, (size_t)bodyLen);
      n = bodyLen;
    } else if (keep >= 2) {
      n = clipWithEllipsis(raw.body, keep, buf);
      if (n < 3) continue;                    // all spaces: not worth a stub
    } else if (sufLen == 0) {
      buf[0] = 0x1C;                          // nothing fits: a lone ellipsis
      n = 1;
    } else {
      n = 0;                                  // suffix alone
    }
    int split = n;
    if (sufLen) {
      if (n > 0) buf[n++] = ' ';
      split = n;
      for (int k = 0; k < sufLen; k++) buf[n++] = (uint8_t)raw.suffix[k];
    }
    if (glyphW(f, buf, n, tracking, flags) <= avail || (keep == 0)) {
      memcpy(out.glyphs, buf, (size_t)n);
      out.len = (uint8_t)n;
      out.split = (uint8_t)(sufLen ? split : n);
      return;
    }
    if (keep < 2 && sufLen == 0) {           // the lone ellipsis is the floor
      memcpy(out.glyphs, buf, (size_t)n);
      out.len = (uint8_t)n;
      out.split = (uint8_t)n;
      return;
    }
  }
}

static const int kIconW = 16, kIconGap = 6;
static const int kBatW = 23, kBatGap = 7;

static void layoutSlot(int idx, const SlotCfg &cfg, const faceslots::FaceData &d, Slot &out) {
  faceslots::SlotRaw raw;
  if (!faceslots::slotRaw(cfg, d, raw)) return;
  out.color = raw.color;
  int maxW = idx == SLOT_BOTTOM ? kCardInnerW : kLineW;
  switch (raw.source) {
    case SRC_SECONDS:
      out.kind = K_SECONDS;
      out.sec = (uint8_t)(d.now.second % 60);
      return;
    case SRC_DATE:
    case SRC_SHORT_DATE:
      out.kind = K_DATE;
      fit(aa_assets::kFontLabel, kTrack, TF_UPPER, raw, maxW, out);
      return;
    case SRC_BATTERY:
      out.kind = K_BATTERY;
      out.icon = raw.icon;
      out.iconArg = raw.iconArg;
      if (raw.icon) maxW -= kBatW + kBatGap;
      fit(aa_assets::kFontUi, 0, TF_NONE, raw, maxW, out);
      return;
    default:
      out.kind = K_TEXT;
      out.icon = raw.icon;
      out.iconArg = 255;
      if (raw.icon) maxW -= kIconW + kIconGap;
      fit(aa_assets::kFontUi, 0, TF_NONE, raw, maxW, out);
      return;
  }
}

void buildScene(const FaceRender::Frame &f, Scene &sc) {
  memset(&sc, 0, sizeof(sc));
  sc.pal = palette(f.bg, f.fg, f.accent, f.line);
  sc.theme[0] = f.bg; sc.theme[1] = f.fg; sc.theme[2] = f.accent; sc.theme[3] = f.line;

  bool twelve = (f.options & kFaceOpt12h) != 0;
  unsigned h = f.now.hour;
  if (twelve) { h %= 12; if (h == 0) h = 12; }
  char buf[8];
  if (!f.rtcOk)    snprintf(buf, sizeof(buf), "--:--");
  else if (twelve) snprintf(buf, sizeof(buf), "%u:%02u", h, (unsigned)(f.now.minute % 60));
  else             snprintf(buf, sizeof(buf), "%02u:%02u", h % 100, (unsigned)(f.now.minute % 60));
  sc.timeLen = (uint8_t)strlen(buf);
  memcpy(sc.time, buf, sc.timeLen);
  sc.timeOk = f.rtcOk ? 1 : 0;

  if (!f.wifiShown)          sc.wifi = 0;
  else if (!f.wifiEnabled)   sc.wifi = 1;
  else if (f.wifiAp)         { sc.wifi = 3; sc.wifiBars = 3; }
  else if (f.wifiConnected) {
    sc.wifi = 4;
    sc.wifiBars = f.wifiRssi >= -55 ? 3 : (f.wifiRssi >= -65 ? 2 : (f.wifiRssi >= -75 ? 1 : 0));
  } else                     sc.wifi = 2;
  sc.ble = f.ble;

  sc.bottomMode = (f.swRun || f.tmrOn) ? 1 : 0;
  sc.swRun = f.swRun ? 1 : 0;
  sc.swMs = f.swRun ? f.swMs : 0;
  sc.tmrOn = f.tmrOn ? 1 : 0;
  sc.tmrMs = f.tmrOn ? f.tmrMs : 0;

  faceslots::FaceData d;
  d.rtcOk = f.rtcOk;
  d.now = f.now;
  d.unixNow = f.unixNow;
  d.batOk = f.batOk;
  d.batPct = f.batPct;
  d.texts = f.texts;
  d.feeds = f.feeds;
  for (int i = 0; i < kSlotCount; i++) {
    if (i == SLOT_BOTTOM && sc.bottomMode) continue;
    layoutSlot(i, f.slots[i], d, sc.slots[i]);
  }
}

bool sameRegion(const Scene &a, const Scene &b, int region) {
  switch (region) {
    case R_STATUS:
      return a.wifi == b.wifi && a.wifiBars == b.wifiBars && a.ble == b.ble;
    case R_TIME:
      return a.timeLen == b.timeLen && a.timeOk == b.timeOk && memcmp(a.time, b.time, a.timeLen) == 0;
    case R_TOP:
      return memcmp(&a.slots[SLOT_TOP], &b.slots[SLOT_TOP], sizeof(Slot)) == 0;
    case R_UPPER:
      return memcmp(&a.slots[SLOT_UPPER], &b.slots[SLOT_UPPER], sizeof(Slot)) == 0;
    case R_LOWER:
      return memcmp(&a.slots[SLOT_LOWER], &b.slots[SLOT_LOWER], sizeof(Slot)) == 0;
    default:
      return a.bottomMode == b.bottomMode && a.swRun == b.swRun && a.swMs == b.swMs &&
             a.tmrOn == b.tmrOn && a.tmrMs == b.tmrMs &&
             memcmp(&a.slots[SLOT_BOTTOM], &b.slots[SLOT_BOTTOM], sizeof(Slot)) == 0;
  }
}

bool timerBox(const Scene &sc, int line, int &x0, int &y0, int &x1, int &y1) {
  bool shown = line == 0 ? sc.swRun : sc.tmrOn;
  if (!sc.bottomMode || !shown) return false;
  bool both = sc.swRun && sc.tmrOn;
  int base = both ? 240 : 247;
  if (line == 1 && sc.swRun) base += 22;
  x0 = kCardX + 10; x1 = kCardX + kCardW - 10;
  y0 = base - 13; y1 = base + 3;
  return true;
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------
static Rgb slotColor(const Palette &p, uint8_t c) {
  return c == COLOR_ACCENT ? p.ink : (c == COLOR_DIM ? p.dim : p.text);
}

static void battery(Surface &s, const Palette &p, int x, int y, uint8_t pct, Rgb col) {
  roundRectStroke(s, x, y, 20, 11, 3, 2, col, 210);
  roundRect(s, x + 21, y + 3, 2, 5, 1, col, 210);
  if (pct <= 100) {
    int fw = (14 * pct + 50) / 100;
    Rgb fill = pct <= 15 ? readable(rgb(255, 69, 58), p.bg, 70) : col;
    if (fw > 0) roundRect(s, x + 3, y + 3, fw, 5, 1, fill, 255);
  }
}

static void paintTime(Surface &s, const Scene &sc) {
  const Palette &p = sc.pal;
  const Font &f = aa_assets::kFontTime;
  int w = textWidth(f, sc.time, sc.timeLen, -1, TF_NONE);
  int x = kCx - w / 2;
  Rgb top = p.text, bottom = mix(p.text, p.ink, 90);
  if (!sc.timeOk) top = bottom = readable(rgb(255, 69, 58), p.bg, 70);
  if (!p.light) text(s, f, sc.time, sc.timeLen, x, kTimeBase + 3, kBlack, 110, -1, TF_NONE);
  textV(s, f, sc.time, sc.timeLen, x, kTimeBase, top, bottom, kTimeBase - 57, kTimeBase, 255, -1,
        TF_NONE);
}

static void paintSeconds(Surface &s, const Palette &p, const Slot &sl, int base) {
  const Font &f = aa_assets::kFontSmall;
  uint8_t num[2] = { (uint8_t)('0' + sl.sec / 10), (uint8_t)('0' + sl.sec % 10) };
  const int trackW = 104, gap = 10;
  int numW = textWidth(f, num, 2, 0, TF_TABULAR);
  int x0 = kCx - (trackW + gap + numW) / 2;
  int cy = (base - 5) * 256 + 128;
  int xa = (x0 + 2) * 256 + 128, xb = (x0 + trackW - 3) * 256 + 128;
  int xp = xa + (xb - xa) * sl.sec / 59;
  capsule(s, xa, cy, xb, cy, 384, p.dim, 96);
  capsule(s, xa, cy, xp, cy, 384, p.ink, 255);
  disc(s, xp, cy, 7 * 256, p.ink, 64);
  disc(s, xp, cy, 896, p.text, 255);
  text(s, f, num, 2, x0 + trackW + gap, base, slotColor(p, sl.color), 255, 0, TF_TABULAR);
}

static void paintSlot(Surface &s, const Scene &sc, int idx) {
  const Slot &sl = sc.slots[idx];
  if (sl.kind == K_NONE) return;
  const Palette &p = sc.pal;
  int base = kSlotBase[idx];
  if (idx == SLOT_BOTTOM) card(s, p, kCardX, kCardY, kCardW, kCardH, kCardR);
  Rgb col = slotColor(p, sl.color);
  if (sl.kind == K_SECONDS) { paintSeconds(s, p, sl, base); return; }
  if (sl.kind == K_DATE) {
    const Font &f = aa_assets::kFontLabel;
    int w = textWidth(f, sl.glyphs, sl.len, kTrack, TF_UPPER);
    text(s, f, sl.glyphs, sl.len, kCx - w / 2, base, col, 255, kTrack, TF_UPPER);
    return;
  }
  const Font &f = aa_assets::kFontUi;
  int tw = textWidth(f, sl.glyphs, sl.len, 0, TF_NONE);
  int iw = 0;
  if (sl.icon) iw = sl.kind == K_BATTERY ? kBatW + kBatGap : kIconW + kIconGap;
  int x = kCx - (iw + tw) / 2;
  if (sl.icon) {
    if (sl.kind == K_BATTERY) battery(s, p, x, base - 11, sl.iconArg, col);
    else mask(s, aa_assets::kMasks[aa_assets::M_SUN_16 + sl.icon - 1], x, base - 13, col, 255);
    x += iw;
  }
  Rgb suffix = sl.color == COLOR_ACCENT ? p.text : p.ink;
  int adv = text(s, f, sl.glyphs, sl.split, x, base, col, 255, 0, TF_NONE);
  if (sl.split < sl.len) text(s, f, sl.glyphs + sl.split, sl.len - sl.split, x + adv, base, suffix, 255, 0, TF_NONE);
}

// "SW 01:02.345" / "T-1:02:03.456" (BaseOS's stopwatch and timer lines).
static int fmtClockMs(uint8_t *out, const char *prefix, uint32_t ms) {
  uint32_t totalSec = ms / 1000u, msPart = ms % 1000u;
  uint32_t hh = totalSec / 3600u, mm = (totalSec % 3600u) / 60u, ss = totalSec % 60u;
  char buf[32];
  if (hh > 0) snprintf(buf, sizeof(buf), "%s%lu:%02lu:%02lu.%03lu", prefix, (unsigned long)hh,
                       (unsigned long)mm, (unsigned long)ss, (unsigned long)msPart);
  else        snprintf(buf, sizeof(buf), "%s%02lu:%02lu.%03lu", prefix, (unsigned long)mm,
                       (unsigned long)ss, (unsigned long)msPart);
  int n = (int)strlen(buf);
  memcpy(out, buf, (size_t)n);
  return n;
}

static void paintTimers(Surface &s, const Scene &sc) {
  const Palette &p = sc.pal;
  const Font &f = aa_assets::kFontUi;
  bool both = sc.swRun && sc.tmrOn;
  int y = both ? 218 : kCardY, h = both ? 54 : kCardH;
  card(s, p, kCardX, y, kCardW, h, 20);
  int base = both ? 240 : 247;
  uint8_t buf[32];
  if (sc.swRun) {
    int n = fmtClockMs(buf, "SW ", sc.swMs);
    int w = textWidth(f, buf, n, 0, TF_TABULAR);
    text(s, f, buf, n, kCx - w / 2, base, readable(rgb(48, 209, 88), p.bg, 80), 255, 0, TF_TABULAR);
    base += 22;
  }
  if (sc.tmrOn) {
    int n = fmtClockMs(buf, "T-", sc.tmrMs);
    int w = textWidth(f, buf, n, 0, TF_TABULAR);
    text(s, f, buf, n, kCx - w / 2, base, readable(rgb(255, 159, 10), p.bg, 80), 255, 0, TF_TABULAR);
  }
}

static void paintStatus(Surface &s, const Scene &sc) {
  const Palette &p = sc.pal;
  using namespace aa_assets;
  if (sc.wifi) {
    Rgb on = p.dim;
    if (sc.wifi == 2) on = readable(rgb(255, 159, 10), p.bg, 80);
    if (sc.wifi == 3) on = readable(rgb(255, 214, 10), p.bg, 80);
    if (sc.wifi == 4) on = readable(rgb(48, 209, 88), p.bg, 80);
    int lit = sc.wifi == 1 ? -1 : (sc.wifi == 2 ? 0 : sc.wifiBars);
    for (int k = 0; k <= 3; k++) {
      bool isOn = k <= lit;
      mask(s, kMasks[M_WIFI0_22 + k], 14, 9, isOn ? on : p.dim, isOn ? 255 : 110);
    }
  }
  if (sc.ble) {
    Rgb c = p.dim;
    if (sc.ble == FaceRender::BLE_PAIRING) c = readable(rgb(255, 159, 10), p.bg, 80);
    if (sc.ble == FaceRender::BLE_CONNECTED) {
      c = p.ink;
      disc(s, kCx * 256, 21 * 256, 12 * 256, c, 56);
    }
    mask(s, kMasks[M_BT_16], kCx - 8, 13, c, 255);
  }
  mask(s, kMasks[M_POWER_20], 204, 11, p.dim, 230);
}

void paint(Surface &s, const Scene &sc) {
  backdrop(s, sc.pal, 112);
  paintStatus(s, sc);
  paintTime(s, sc);
  for (int i = 0; i < kSlotCount; i++) paintSlot(s, sc, i);
  if (sc.bottomMode) paintTimers(s, sc);
}

}  // namespace halo
