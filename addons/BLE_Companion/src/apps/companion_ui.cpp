// Companion UI painter — see companion_ui.h.
#include "companion_ui.h"

#include <stdio.h>
#include <string.h>

#include "aa_assets.h"

using namespace aa;
using namespace aa_assets;
using halo::readable;

namespace cui {

static const Rgb kWhite = { 255, 255, 255 };
static const Rgb kBlack = { 0, 0, 0 };
static const Rgb kRed = { 255, 69, 58 };
static const Rgb kInkDark = { 14, 17, 22 };

const Rect kBtnHide       = { 142, 238, 90, 40 };
const Rect kBtnPairCancel = { 50, 234, 140, 44 };
const Rect kBtnShowFace   = { 12, 218, 110, 50 };
const Rect kBtnEnd        = { 122, 218, 110, 50 };
const Rect kBtnVisible    = { 12, 178, 216, 46 };
const Rect kBtnResetFace  = { 12, 226, 110, 54 };
const Rect kBtnResetLook  = { 122, 226, 110, 54 };

bool inRect(uint16_t x, uint16_t y, const Rect &r) {
  return (int)x >= r.x && (int)x < r.x + r.w && (int)y >= r.y && (int)y < r.y + r.h;
}

// Visual rects (inside the touch targets).
static const Rect vHide       = { 148, 244, 76, 30 };
static const Rect vPairCancel = { 62, 240, 116, 32 };
static const Rect vShowFace   = { 16, 224, 102, 38 };
static const Rect vEnd        = { 122, 224, 102, 38 };
static const Rect vVisible    = { 20, 182, 200, 38 };
static const Rect vResetFace  = { 16, 231, 102, 42 };
static const Rect vResetLook  = { 122, 231, 102, 42 };

// Geometry
static const int kCx = 120;
static const int kEmblemY = 96;          // orb centre on most screens
static const int kOffY = 90;             // ... and when off / locked out
static const int kDialY = 130, kDialR = 98;
static const uint32_t kPulseMs = 2400;   // sonar rings
static const uint32_t kBreathMs = 1800;  // glyph breathing

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
struct ClipGuard {                        // intersect the clip, restore on exit
  Surface &s;
  int16_t x0, y0, x1, y1;
  ClipGuard(Surface &sf, int ax, int ay, int bx, int by) : s(sf), x0(sf.cx0), y0(sf.cy0), x1(sf.cx1), y1(sf.cy1) {
    setClip(s, ax > x0 ? ax : x0, ay > y0 ? ay : y0, bx < x1 ? bx : x1, by < y1 ? by : y1);
  }
  ~ClipGuard() { s.cx0 = x0; s.cy0 = y0; s.cx1 = x1; s.cy1 = y1; }
};

static Rgb onColor(Rgb base) { return luma(base) > 150 ? kInkDark : kWhite; }

// 0..16384 smooth pulse with period `period` ms: (1 - cos) / 2.
static int pulse(uint32_t ms, uint32_t period) {
  int t = (int)((ms % period) * 1024 / period);
  int x, y;
  dir(t, x, y);                           // y = -cos
  return (16384 + y) / 2;
}

static void textC(Surface &s, const Font &f, const char *str, int y, Rgb c, int a = 255, int tr = 0,
                  uint8_t flags = 0) {
  textCS(s, f, str, kCx, y, c, a, tr, flags);
}

// Glossy sphere: graded body, lit rim, glass highlight, optional glow.
static void orb(Surface &s, int cx, int cy, int r, Rgb base, Rgb glow, int glowA) {
  if (glowA > 0) softShadow(s, cx - r, cy - r, 2 * r, 2 * r, r, 18, glow, glowA);
  roundRectV(s, cx - r, cy - r, 2 * r, 2 * r, r, mix(base, kWhite, 70), mix(base, kBlack, 80), 255);
  roundRectStrokeVA(s, cx - r, cy - r, 2 * r, 2 * r, r, 1, kWhite, 110, 0);
  int hw = r * 5 / 4, hh = r * 3 / 4;
  roundRectVA(s, cx - hw / 2, cy - r + r / 8, hw, hh, hh / 2, kWhite, 120, 0);
}

static void glassOrb(Surface &s, const halo::Palette &p, int cx, int cy, int r) {
  roundRectVA(s, cx - r, cy - r, 2 * r, 2 * r, r, p.text, 30, 10);
  roundRectStrokeVA(s, cx - r, cy - r, 2 * r, 2 * r, r, 1, p.text, 80, 14);
}

enum Kind : uint8_t { PRIMARY, DANGER, GLASS, GLASS_DANGER, DISABLED };

static void pill(Surface &s, const halo::Palette &p, const Rect &r, const char *label, Kind k,
                 const char *sub = nullptr, int progress = -1) {
  int rad = r.h / 2;
  Rgb labelC = p.text;
  if (k == PRIMARY || k == DANGER) {
    Rgb base = k == PRIMARY ? p.ink : readable(rgb(229, 72, 77), p.bg, 40);
    Rgb glow = k == PRIMARY ? p.vivid : base;
    softShadow(s, r.x + 6, r.y + 8, r.w - 12, r.h - 6, rad, 14, glow, p.light ? 80 : 120);
    roundRectV(s, r.x, r.y, r.w, r.h, rad, mix(base, kWhite, 60), mix(base, kBlack, 64), 255);
    roundRectStrokeVA(s, r.x, r.y, r.w, r.h, rad, 1, kWhite, 150, 0);
    labelC = onColor(base);
  } else {
    if (!p.light) softShadow(s, r.x, r.y + 3, r.w, r.h, rad, 10, kBlack, 110);
    else softShadow(s, r.x, r.y + 2, r.w, r.h, rad, 8, mix(p.bg, kBlack, 96), 70);
    if (p.light) roundRectVA(s, r.x, r.y, r.w, r.h, rad, kWhite, 235, 205);
    else roundRectVA(s, r.x, r.y, r.w, r.h, rad, p.text, 36, 14);
    roundRectStrokeVA(s, r.x, r.y, r.w, r.h, rad, 1, p.text, p.light ? 20 : 96, p.light ? 40 : 18);
    if (k == GLASS_DANGER) labelC = readable(kRed, p.bg, 70);
    if (k == DISABLED) labelC = p.dim;
  }
  if (progress > 0) {
    ClipGuard g(s, r.x, r.y, r.x + r.w * (progress > 1000 ? 1000 : progress) / 1000, r.y + r.h);
    roundRect(s, r.x, r.y, r.w, r.h, rad, p.ink, 120);
  }
  const Font &f = kFontUi;
  if (sub) {
    textCS(s, kFontSmall, label, r.x + r.w / 2, r.y + 18, labelC);
    textCS(s, kFontLabel, sub, r.x + r.w / 2, r.y + 32, p.dim, 200, 1, TF_UPPER);
  } else {
    textCS(s, f, label, r.x + r.w / 2, r.y + (r.h + 11) / 2, labelC);
  }
}

static void header(Surface &s, const halo::Palette &p, const Model &m, const char *title) {
  glassOrb(s, p, 30, 27, 16);
  mask(s, kMasks[M_BACK_18], 30 - 10, 27 - 9, p.text, 255);
  if (m.toast[0]) {
    const Font &f = kFontSmall;
    int w = textWidthS(f, m.toast) + 26;
    Rect r = { (int16_t)(kCx - w / 2), 14, (int16_t)w, 26 };
    if (p.light) roundRectVA(s, r.x, r.y, r.w, r.h, 13, kWhite, 240, 220);
    else roundRectVA(s, r.x, r.y, r.w, r.h, 13, p.ink, 90, 60);
    roundRectStrokeVA(s, r.x, r.y, r.w, r.h, 13, 1, p.light ? p.ink : kWhite, 120, 30);
    textC(s, f, m.toast, 32, p.text);
  } else if (title) {
    textC(s, kFontLabel, title, 32, p.dim, 255, 2, TF_UPPER);
  }
}

// Countdown ring: track, glow, lit arc for the remaining fraction, knob.
static int remain1024(const Model &m) {
  if (!m.totalMs) return 0;
  uint32_t r = m.remainMs > m.totalMs ? m.totalMs : m.remainMs;
  return (int)((uint64_t)r * 1024 / m.totalMs);
}

static void ringPoint(int cx, int cy, int r, int t, int &x, int &y) {
  int ux, uy;
  dir(t, ux, uy);
  x = cx * 256 + ((r * 256 >> 4) * ux >> 10);
  y = cy * 256 + ((r * 256 >> 4) * uy >> 10);
}

static void countdownRing(Surface &s, const halo::Palette &p, int cx, int cy, int r, int thick,
                          int rem, Rgb c, Rgb glow, bool ticks) {
  ring(s, cx * 256, cy * 256, r * 256, thick * 256, p.dim, 56);
  if (ticks) {
    for (int i = 0; i < 60; i++) {
      int x, y;
      ringPoint(cx, cy, r - 12, (i * 1024 + 30) / 60, x, y);
      disc(s, x, y, i % 5 == 0 ? 400 : 230, p.dim, i % 5 == 0 ? 150 : 90);
    }
  }
  if (rem <= 0) return;
  int a0 = 1024 - rem;
  arc(s, cx * 256, cy * 256, r * 256, (thick + 12) * 256, a0, 1024, glow, p.light ? 22 : 40);
  arc(s, cx * 256, cy * 256, r * 256, thick * 256, a0, 1024, c, 255);
  if (rem < 1024) {
    int x, y;
    ringPoint(cx, cy, r, a0, x, y);
    disc(s, x, y, 10 * 256, glow, 70);
    disc(s, x, y, 1280, p.light ? kWhite : p.text, 255);
  }
}

void countdownText(const Model &m, char *out, int cap) {
  uint32_t sec = (m.remainMs + 999) / 1000;
  switch (m.screen) {
    case SC_VISIBLE:
      snprintf(out, (size_t)cap, "%lu:%02lu", (unsigned long)(sec / 60), (unsigned long)(sec % 60));
      break;
    case SC_PAIRING:
      if (m.attemptsLeft < m.maxAttempts) {
        snprintf(out, (size_t)cap, "Wrong code \xFA %u %s left", (unsigned)m.attemptsLeft,
                 m.attemptsLeft == 1 ? "try" : "tries");
      } else {
        snprintf(out, (size_t)cap, "%lu s left", (unsigned long)sec);
      }
      break;
    case SC_CONNECTED: {
      uint32_t ss = m.sessionMs / 1000;
      snprintf(out, (size_t)cap, "%lu change%s \xFA %lu:%02lu", (unsigned long)m.writes,
               m.writes == 1 ? "" : "s", (unsigned long)(ss / 60), (unsigned long)(ss % 60));
      break;
    }
    case SC_LOCKED:
      snprintf(out, (size_t)cap, "Try again in %lu s", (unsigned long)sec);
      break;
    case SC_OFF:
      snprintf(out, (size_t)cap, "%s", m.status);
      break;
    default:
      out[0] = 0;
  }
}

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------
static void paintVisible(Surface &s, const halo::Palette &p, const Model &m) {
  header(s, p, m, "Companion");
  for (int k = 0; k < 3; k++) {
    uint32_t ph = (m.animMs + (uint32_t)k * (kPulseMs / 3)) % kPulseMs;
    int r = 30 * 256 + (int)(28 * 256 * ph / kPulseMs);
    int a = (int)(170 * (kPulseMs - ph) / kPulseMs);
    ring(s, kCx * 256, kEmblemY * 256, r, 640, p.ink, a);
  }
  orb(s, kCx, kEmblemY, 28, p.ink, p.vivid, p.light ? 70 : 110);
  mask(s, kMasks[M_BT_44], kCx - 22, kEmblemY - 22, onColor(p.ink), 255);

  textC(s, kFontLabel, "Visible as", 170, p.dim, 255, 2, TF_UPPER);
  textC(s, kFontTitle, m.device, 194, p.text);
  textC(s, kFontSmall, "Open the Companion page in", 216, p.dim);
  textC(s, kFontSmall, "Chrome, then press Connect.", 232, p.dim);

  char buf[16];
  countdownText(m, buf, sizeof(buf));
  disc(s, 24 * 256, 259 * 256, 3 * 256, p.ink, 255);
  disc(s, 24 * 256, 259 * 256, 7 * 256, p.ink, 50);
  textS(s, kFontUi, buf, 34, 264, p.text, 255, 0, TF_TABULAR);
  pill(s, p, vHide, "Hide", GLASS);
}

static void paintPairing(Surface &s, const halo::Palette &p, const Model &m) {
  header(s, p, m, nullptr);
  countdownRing(s, p, kCx, kDialY, kDialR, 6, remain1024(m), p.ink, p.vivid, true);

  int br = pulse(m.animMs, kBreathMs);                 // 0..16384
  softShadow(s, kCx - 14, 68 - 14, 28, 28, 14, 10 + br * 8 / 16384, p.vivid, 50 + br * 90 / 16384);
  orb(s, kCx, 68, 14, p.ink, p.vivid, 0);
  mask(s, kMasks[M_BT_16], kCx - 8, 68 - 8, onColor(p.ink), 255);

  textC(s, kFontLabel, "Enter on the page", 106, p.dim, 255, 2, TF_UPPER);
  char code[8];
  snprintf(code, sizeof(code), "%03lu %03lu", (unsigned long)(m.code / 1000 % 1000),
           (unsigned long)(m.code % 1000));
  int w = textWidthS(kFontCode, code, 0, TF_TABULAR);
  const uint8_t *g = (const uint8_t *)code;
  textV(s, kFontCode, g, 7, kCx - w / 2, 152, p.text, mix(p.text, p.ink, 80), 152 - 29, 152, 255, 0,
        TF_TABULAR);

  char buf[40];
  countdownText(m, buf, sizeof(buf));
  bool wrong = m.attemptsLeft < m.maxAttempts;
  textC(s, kFontSmall, buf, 180, wrong ? readable(kRed, p.bg, 80) : p.dim);
  pill(s, p, vPairCancel, "Disconnect", GLASS_DANGER);
}

static void paintConnected(Surface &s, const halo::Palette &p, const Model &m) {
  header(s, p, m, "Companion");
  ring(s, kCx * 256, kEmblemY * 256, 42 * 256, 3 * 256, p.ink, 200);
  ring(s, kCx * 256, kEmblemY * 256, 42 * 256, 14 * 256, p.vivid, 36);
  orb(s, kCx, kEmblemY, 30, p.ink, p.vivid, p.light ? 60 : 90);
  mask(s, kMasks[M_CHECK_40], kCx - 20, kEmblemY - 20, onColor(p.ink), 255);

  textC(s, kFontTitle, "Connected", 164, p.text);
  textC(s, kFontSmall, "Changes appear live on the watch", 184, p.dim);
  char buf[40];
  countdownText(m, buf, sizeof(buf));
  textC(s, kFontLabel, buf, 204, p.ink, 255, 1, TF_UPPER);
  pill(s, p, vShowFace, "Show face", PRIMARY);
  pill(s, p, vEnd, "Disconnect", GLASS_DANGER);
}

static void holdButtons(Surface &s, const halo::Palette &p, const Model &m) {
  pill(s, p, vResetFace, "Reset face", GLASS, "hold 1 s", m.hold == 1 ? m.holdPermille : -1);
  pill(s, p, vResetLook, "Reset colours", GLASS, "hold 1 s", m.hold == 2 ? m.holdPermille : -1);
}

static void paintOff(Surface &s, const halo::Palette &p, const Model &m) {
  header(s, p, m, "Companion");
  ring(s, kCx * 256, kOffY * 256, 42 * 256, 2 * 256, p.dim, 70);
  glassOrb(s, p, kCx, kOffY, 30);
  mask(s, kMasks[M_BT_44], kCx - 22, kOffY - 22, p.dim, 255);
  textC(s, kFontTitle, "Companion is off", 154, p.text);
  char buf[48];
  countdownText(m, buf, sizeof(buf));
  textC(s, kFontSmall, buf, 172, p.dim);
  pill(s, p, vVisible, "Make visible", PRIMARY);
  holdButtons(s, p, m);
}

static void paintLocked(Surface &s, const halo::Palette &p, const Model &m) {
  header(s, p, m, "Companion");
  Rgb red = readable(kRed, p.bg, 80);
  countdownRing(s, p, kCx, kOffY, 38, 5, remain1024(m), red, red, false);
  mask(s, kMasks[M_LOCK_36], kCx - 18, kOffY - 19, red, 255);
  textC(s, kFontTitle, "Too many tries", 154, p.text);
  char buf[40];
  countdownText(m, buf, sizeof(buf));
  textC(s, kFontSmall, buf, 172, p.dim);
  pill(s, p, vVisible, "Make visible", DISABLED);
  holdButtons(s, p, m);
}

// Greedy word wrap of CP437 glyphs to `maxW` px; returns the line count.
static int wrap(const Font &f, const uint8_t *g, int n, int maxW, int *start, int *len, int maxLines) {
  int lines = 0, i = 0;
  while (i < n && lines < maxLines) {
    while (i < n && g[i] == ' ') i++;
    if (i >= n) break;
    int end = i, lastBreak = -1;
    while (end < n) {
      if (textWidth(f, g + i, end - i + 1, 0, TF_NONE) > maxW) break;
      if (g[end] == ' ') lastBreak = end;
      end++;
    }
    int take = end - i;
    if (end < n && lastBreak > i) take = lastBreak - i;
    if (take <= 0) take = 1;
    while (take > 1 && g[i + take - 1] == ' ') take--;
    start[lines] = i;
    len[lines] = take;
    lines++;
    i += take;
  }
  return lines;
}

static void paintMessage(Surface &s, const halo::Palette &p, const Model &m) {
  uint8_t icon = m.icon >= 1 && m.icon <= 15 ? m.icon : 13;   // chat bubble
  orb(s, kCx, 52, 25, p.ink, p.vivid, p.light ? 70 : 110);
  mask(s, kMasks[M_SUN_34 + icon - 1], kCx - 17, 52 - 17, onColor(p.ink), 255);
  textC(s, kFontLabel, "From your browser", 100, p.dim, 255, 2, TF_UPPER);
  halo::card(s, p, 14, 112, 212, 128, 22);

  const Font &f = kFontMsg;
  int st[5], ln[5];
  int lines = wrap(f, m.msg, m.msgLen, 188, st, ln, 5);
  const int lh = 24;
  int y = 112 + (128 - lines * lh) / 2 + 17;
  for (int k = 0; k < lines; k++, y += lh) {
    uint8_t buf[80];
    int n = ln[k];
    memcpy(buf, m.msg + st[k], (size_t)n);
    bool last = k == lines - 1 && st[k] + ln[k] < m.msgLen;   // more text than fits
    if (last) {
      while (n > 0 && textWidth(f, buf, n, 0, TF_NONE) + textWidth(f, (const uint8_t *)"\x1C", 1, 0, 0) > 188) n--;
      buf[n++] = 0x1C;
    }
    int w = textWidth(f, buf, n, 0, TF_NONE);
    text(s, f, buf, n, kCx - w / 2, y, p.text, 255, 0, TF_NONE);
  }

  textC(s, kFontSmall, "Tap to dismiss", 260, p.dim);
  int xa = 84 * 256, xb = 156 * 256, cy = 270 * 256 + 128;
  capsule(s, xa, cy, xb, cy, 384, p.dim, 70);
  int rem = remain1024(m);
  if (rem > 0) capsule(s, xa, cy, xa + (xb - xa) * rem / 1024, cy, 384, p.ink, 255);
}

void paint(Surface &s, const halo::Palette &p, const Model &m) {
  halo::backdrop(s, p, m.screen == SC_MESSAGE ? 70 : (m.screen == SC_PAIRING ? kDialY : 104));
  switch (m.screen) {
    case SC_VISIBLE:   paintVisible(s, p, m); break;
    case SC_PAIRING:   paintPairing(s, p, m); break;
    case SC_CONNECTED: paintConnected(s, p, m); break;
    case SC_LOCKED:    paintLocked(s, p, m); break;
    case SC_MESSAGE:   paintMessage(s, p, m); break;
    default:           paintOff(s, p, m); break;
  }
}

// ---------------------------------------------------------------------------
// Incremental redraw helpers
// ---------------------------------------------------------------------------
int animRects(const Model &m, Rect *out, int max) {
  int n = 0;
  if (max < 1) return 0;
  if (m.screen == SC_VISIBLE) {
    Rect r = { kCx - 62, kEmblemY - 62, 124, 124 };
    out[n++] = r;
  } else if (m.screen == SC_PAIRING) {
    Rect r = { kCx - 34, 68 - 34, 68, 68 };
    out[n++] = r;
  }
  return n;
}

Rect ringEnd(const Model &m) {
  Rect none = { 0, 0, 0, 0 };
  int cx, cy, r;
  if (m.screen == SC_PAIRING) { cx = kCx; cy = kDialY; r = kDialR; }
  else if (m.screen == SC_LOCKED) { cx = kCx; cy = kOffY; r = 38; }
  else if (m.screen == SC_MESSAGE) {
    Rect b = { 80, 264, 80, 12 };
    return b;
  } else return none;
  int x, y;
  ringPoint(cx, cy, r, 1024 - remain1024(m), x, y);
  Rect b = { (int16_t)((x >> 8) - 13), (int16_t)((y >> 8) - 13), 27, 27 };
  return b;
}

Rect textRect(const Model &m) {
  switch (m.screen) {
    case SC_VISIBLE:   { Rect r = { 14, 246, 110, 26 }; return r; }
    case SC_PAIRING:   { Rect r = { 30, 166, 180, 20 }; return r; }
    case SC_CONNECTED: { Rect r = { 20, 192, 200, 18 }; return r; }
    case SC_OFF:
    case SC_LOCKED:    { Rect r = { 0, 158, 240, 20 }; return r; }
    default:           { Rect r = { 0, 0, 0, 0 }; return r; }
  }
}

Rect toastRect() { Rect r = { 30, 6, 180, 42 }; return r; }

Rect holdRect(uint8_t hold) {
  Rect r = hold == 2 ? vResetLook : vResetFace;
  return r;
}

Rect fullRect() { Rect r = { 0, 0, 240, 280 }; return r; }

}  // namespace cui
