#include "fr_screens.h"
#include "fr_clock.h"
#include "fr_motion.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace fr {

using namespace layout;

static inline uint8_t u8f(float v) { return v <= 0.f ? 0 : (v >= 255.f ? 255 : (uint8_t)(v + 0.5f)); }
static inline int imaxLocal(int a, int b) { return a > b ? a : b; }

// ---------------------------------------------------------------------------
// Shared chrome
// ---------------------------------------------------------------------------
void drawBackdrop(Canvas &c, const PageChrome &pc) {
  if (pc.backdrop && c.w == W && c.h == H) memcpy(c.px, pc.backdrop, (size_t)W * H * sizeof(uint16_t));
  else buildPageBackground(c);
}

static void drawAirLamp(Canvas &c, const PageChrome &pc) {
  if (!pc.onAir) return;
  float pulse = 0.5f + 0.5f * sinf((float)pc.tMs * 0.0042f);
  fillCircle(c, 211.4f, 20.6f, 4.6f, hex(0x5A666E), 110);
  fillCircle(c, 211.f, 20.f, 4.4f, hex(0x030506));
  drawLed(c, 211.f, 20.f, pal::live, 0.55f + 0.45f * pulse);
  drawCapsCentered(c, 211, 41, "VISIBLE", pal::textDim, 255, 1);
}

void drawTitleBar(Canvas &c, const PageChrome &pc, const char *title) {
  drawBackButton(c, pc.backPressed);
  if (title && *title) drawPageTitle(c, title);
  drawAirLamp(c, pc);
}

// List content slides under the title bar: fade it into the room there, and
// at the bottom edge when more is waiting below.
static void fadeEdges(Canvas &c, const PageChrome &pc, bool moreBelow) {
  const int top = listTop, n = 12;
  for (int i = 0; i < n; ++i) {
    float k = 1.f - (float)i / (float)n;
    uint8_t a = u8f(255.f * k * k);
    uint16_t *row = c.px + (size_t)(top + i) * W;
    const uint16_t *bg = pc.backdrop ? pc.backdrop + (size_t)(top + i) * W : nullptr;
    for (int x = 0; x < W; ++x) row[x] = blend(row[x], bg ? bg[x] : pal::room0, a);
  }
  if (!moreBelow) return;
  for (int i = 0; i < 16; ++i) {
    int y = H - 1 - i;
    float k = 1.f - (float)i / 16.f;
    uint8_t a = u8f(230.f * k * k);
    uint16_t *row = c.px + (size_t)y * W;
    const uint16_t *bg = pc.backdrop ? pc.backdrop + (size_t)y * W : nullptr;
    for (int x = 0; x < W; ++x) row[x] = blend(row[x], bg ? bg[x] : pal::room1, a);
  }
}

// An engraved scroll groove with a lit thumb at the right edge.
static void scrollGroove(Canvas &c, int content, int view, int scroll) {
  if (content <= view) return;
  const int x = 235, y0 = listTop + 8, y1 = H - 10;
  const int len = y1 - y0;
  fillRoundRect(c, x - 1, y0, 3, len, 1.5f, hex(0x030607), 200);
  int th = imaxLocal(18, len * view / content);
  int ty = y0 + (len - th) * scroll / (content - view);
  fillRoundRect(c, x - 1, ty, 3, th, 1.5f, pal::phosMid);
  fillRect(c, x, ty + 2, 1, th - 4, pal::phos);
}

// ---------------------------------------------------------------------------
// Lists
// ---------------------------------------------------------------------------
int listMaxScroll(int rows) {
  int content = rows * rowH + 10;
  return content > kListViewH ? content - kListViewH : 0;
}

void listClampScroll(ListView &v, int rows) {
  int m = listMaxScroll(rows);
  if (v.scroll < 0) v.scroll = 0;
  if (v.scroll > m) v.scroll = m;
}

int listRowAt(int y, const ListView &v, int rows) {
  if (y < listTop) return -1;
  int i = (y - listTop + v.scroll) / rowH;
  return (i >= 0 && i < rows) ? i : -1;
}

// ---------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------
static void menuIcon(Canvas &c, MenuRow::Icon ic, float x, float y, uint16_t col) {
  switch (ic) {
    case MenuRow::IcPeople:   iconPeople(c, x, y, 19.f, col); break;
    case MenuRow::IcTag:      iconTag(c, x, y, 19.f, col); break;
    case MenuRow::IcTarget:   iconTarget(c, x, y, 19.f, col); break;
    case MenuRow::IcBell:     iconBell(c, x, y, 19.f, col); break;
    case MenuRow::IcMoon:     iconMoon(c, x, y, 16.f, col, pal::well); break;
    case MenuRow::IcClock:    iconClock(c, x, y, 19.f, col); break;
    case MenuRow::IcQuestion: iconQuestion(c, x, y, 19.f, col); break;
    case MenuRow::IcRefresh:  iconRefresh(c, x, y, 19.f, col); break;
    default: break;
  }
}

void drawMenu(Canvas &c, const PageChrome &pc, const char *title, const MenuRow *rows, int n,
              const ListView &v) {
  drawBackdrop(c, pc);
  c.clip(Rect(0, listTop, W, H - listTop));
  for (int i = 0; i < n; ++i) {
    int y = listTop + i * rowH - v.scroll;
    if (y + rowH < listTop || y > H) continue;
    const MenuRow &r = rows[i];
    const bool pressed = v.pressed == i;
    const bool sub = r.sub[0] != 0;
    const int ty = y + 3, cyRow = ty + tileH / 2;
    drawTile(c, 10, ty, 220, tileH, pressed);
    int tx;
    int rightEdge = 214;
    if (r.kind == MenuRow::Toggle) {
      float pos = r.knob >= 0.f ? r.knob : (r.on ? 1.f : 0.f);
      drawSwitch(c, 20, cyRow - 14, pos, pressed);
      tx = 80;
      rightEdge = 222;
    } else {
      bool danger = r.kind == MenuRow::Danger;
      drawIconWell(c, 35.f, (float)cyRow, 15.f);
      menuIcon(c, r.icon, 35.f, (float)cyRow, danger ? pal::danger : pal::phos);
      tx = 60;
      if (r.kind == MenuRow::Nav) {
        iconChevronRight(c, 218.f, (float)cyRow, 12.f, pal::textFaint);
        rightEdge = 208;
        if (r.value[0]) {
          char vb[20];
          fitText(kFontM, r.value, 80, vb, sizeof vb);
          int vw = textWidth(kFontM, vb);
          text(c, kFontM, rightEdge - vw, sub ? ty + 21 : ty + 29, vb, pal::phos);
          rightEdge -= vw + 8;
        }
      }
    }
    const uint16_t titleCol = r.kind == MenuRow::Danger ? pal::danger : pal::text;
    char tb[24];
    fitText(kFontM, r.title, rightEdge - tx, tb, sizeof tb);
    text(c, kFontM, tx, sub ? ty + 21 : ty + 29, tb, titleCol);
    if (sub) {
      char sb[44];
      int subRight = r.kind == MenuRow::Nav ? 208 : 222;
      fitText(kFontS, r.sub, subRight - tx, sb, sizeof sb);
      text(c, kFontS, tx, ty + 38, sb, pal::textDim);
    }
  }
  c.resetClip();
  int content = n * rowH + 10;
  fadeEdges(c, pc, v.scroll < content - kListViewH);
  scrollGroove(c, content, kListViewH, v.scroll);
  drawTitleBar(c, pc, title);
}

// ---------------------------------------------------------------------------
// Mates list
// ---------------------------------------------------------------------------
void drawMates(Canvas &c, const PageChrome &pc, const MateRowView *rows, int n, const ListView &v) {
  drawBackdrop(c, pc);
  if (n == 0) {
    // An empty scope waiting for its first light.
    const float ex = 120.f, ey = 124.f;
    glow(c, ex, ey, 70.f, pal::phosFaint, 120);
    for (int i = 0; i < 3; ++i) ring(c, ex, ey, 16.f + 16.f * i, 0.55f, pal::phosDim, (uint8_t)(170 - i * 40));
    sphere(c, ex, ey, 3.f, pal::phosHot);
    float pulse = 0.5f + 0.5f * sinf((float)pc.tMs * 0.003f);
    ring(c, ex + 34.f, ey - 26.f, 9.f, 0.7f, pal::textFaint, u8f(120.f + 80.f * pulse));
    iconPlus(c, ex + 34.f, ey - 26.f, 9.f, pal::textDim);
    textCentered(c, kFontL, W / 2, 202, "No mates yet", pal::text);
    textWrapped(c, kFontS, 22, 224, 196,
                "Tap someone on the radar, then Add as mate.", pal::textDim, 18, 255, true);
    drawTitleBar(c, pc, "Mates");
    return;
  }
  c.clip(Rect(0, listTop, W, H - listTop));
  for (int i = 0; i < n; ++i) {
    int y = listTop + i * rowH - v.scroll;
    if (y + rowH < listTop || y > H) continue;
    const MateRowView &m = rows[i];
    const int ty = y + 3, cyRow = ty + tileH / 2;
    drawTile(c, 10, ty, 220, tileH, v.pressed == i);
    // The mate's light: lit and breathing when heard, a dark glass bead when not.
    float ox = 35.f, oy = (float)cyRow;
    if (m.live) {
      float breath = 0.5f + 0.5f * sinf((float)pc.tMs * 0.004f + (float)i);
      drawOrb(c, ox, oy, 13.f, m.color, (0.35f + 0.5f * m.signal) * (0.7f + 0.3f * breath), m.name[0]);
    } else {
      drawOrb(c, ox, oy, 13.f, blend(hex(0x1A2328), m.color, 70), 0.f, m.name[0]);
    }
    char nb[16];
    fitText(kFontMB, m.name, 140, nb, sizeof nb);
    text(c, kFontMB, 60, ty + 21, nb, pal::text);
    char sb[32];
    fitText(kFontS, m.status, 148, sb, sizeof sb);
    text(c, kFontS, 60, ty + 38, sb, m.live ? zoneColor(m.zone) : pal::textDim);
    iconChevronRight(c, 218.f, (float)cyRow, 12.f, pal::textFaint);
  }
  c.resetClip();
  int content = n * rowH + 10;
  fadeEdges(c, pc, v.scroll < content - kListViewH);
  scrollGroove(c, content, kListViewH, v.scroll);
  drawTitleBar(c, pc, "Mates");
}

// ---------------------------------------------------------------------------
// Mate detail
// ---------------------------------------------------------------------------
static const Rect kRenameBtn(14, 230, 102, 42);
static const Rect kRemoveBtn(124, 230, 102, 42);
const Rect &mateDetailRenameBtn() { return kRenameBtn; }
const Rect &mateDetailRemoveBtn() { return kRemoveBtn; }

void drawMateDetail(Canvas &c, const PageChrome &pc, const MateDetailView &m) {
  drawBackdrop(c, pc);
  // Hero: their light, large, with the name set beside it.
  const float ox = 52.f, oy = 88.f;
  if (m.live) {
    float breath = 0.5f + 0.5f * sinf((float)pc.tMs * 0.0035f);
    glow(c, ox, oy, 62.f, m.color, u8f(40.f + 40.f * m.signal));
    drawOrb(c, ox, oy, 24.f, m.color, (0.4f + 0.5f * m.signal) * (0.75f + 0.25f * breath), m.name[0]);
  } else {
    drawOrb(c, ox, oy, 24.f, blend(hex(0x1A2328), m.color, 80), 0.f, m.name[0]);
  }
  const Font &nf = textWidth(kFontL, m.name) <= 132 ? kFontL : kFontMB;
  char nb[20];
  fitText(nf, m.name, 136, nb, sizeof nb);
  text(c, nf, 88, 86, nb, pal::text);
  if (m.live) {
    int zx = text(c, kFontMB, 88, 108, zoneName(m.zone), zoneColor(m.zone));
    if (m.zone != Zone::Lost) {
      char d[16], buf[24];
      formatApproxMeters(m.distM, d, sizeof d);
      snprintf(buf, sizeof buf, "  %s", d);
      text(c, kFontS, zx, 108, buf, pal::textDim);
    }
  } else {
    char sb[40];
    fitText(kFontS, m.status, 148, sb, sizeof sb);
    text(c, kFontS, 88, 107, sb, pal::textDim);
  }

  // Seen together: a timeline groove with a lamp per meeting.
  drawCaps(c, 18, 136, "SEEN TOGETHER", pal::textFaint, 255, 2);
  if (m.nLog == 0) {
    textWrapped(c, kFontS, 18, 160, 204, "Nothing yet. Meetings appear here.", pal::textDim, 18);
  } else {
    const int n = m.nLog < 5 ? m.nLog : 5;
    const int y0 = 156, step = 16;
    fillRect(c, 23, y0 - 8, 2, (n - 1) * step + 4, hex(0x030607));
    fillRect(c, 25, y0 - 8, 1, (n - 1) * step + 4, hex(0x22323A));
    for (int i = 0; i < n; ++i) {
      int y = y0 + i * step;
      uint16_t zc = zoneColor(m.closest[i]);
      if (i == 0) glow(c, 24.f, (float)y - 4.5f, 9.f, zc, 110);
      sphere(c, 24.f, (float)y - 4.5f, 3.6f, zc);
      text(c, kFontS, 38, y, m.when[i], i == 0 ? pal::text : pal::textDim);
      textRight(c, kFontS, 222, y, m.dur[i], pal::textFaint);
    }
  }
  drawPill(c, kRenameBtn, "Rename", PillStyle::Glass, m.pressed == 1);
  drawPill(c, kRemoveBtn, m.confirmRemove ? "Sure?" : "Remove",
           m.confirmRemove ? PillStyle::DangerSolid : PillStyle::Danger, m.pressed == 2);
  drawTitleBar(c, pc, nullptr);
}

// ---------------------------------------------------------------------------
// Keyboard
// ---------------------------------------------------------------------------
// Alphabetical, six to a row. Codes: >0 characters, <0 KeyCode.
static const int kKbCols = 6, kKbRows = 5, kKbTop = 58, kKbCellW = 40, kKbCellH = 44;
static const int kLetters[kKbRows][kKbCols] = {
  {'a', 'b', 'c', 'd', 'e', 'f'},
  {'g', 'h', 'i', 'j', 'k', 'l'},
  {'m', 'n', 'o', 'p', 'q', 'r'},
  {'s', 't', 'u', 'v', 'w', 'x'},
  {kKeyShift, 'y', 'z', kKeySpace, kKeyMode, kKeyBackspace},
};
static const int kSymbols[kKbRows][kKbCols] = {
  {'1', '2', '3', '4', '5', '6'},
  {'7', '8', '9', '0', '.', ','},
  {'\'', '-', '!', '?', '&', '+'},
  {'(', ')', '_', '@', '#', ':'},
  {'/', '*', '=', kKeySpace, kKeyMode, kKeyBackspace},
};
static const Rect kDoneHit = menuHit;
const Rect &keyboardDoneBtn() { return kDoneHit; }

static int cellCode(const KeyboardView &k, int row, int col) {
  int code = (k.symbols ? kSymbols : kLetters)[row][col];
  if (!k.symbols && k.shift && code >= 'a' && code <= 'z') code -= 32;
  return code;
}

Rect keyboardKeyRect(int row, int col) {
  return Rect(col * kKbCellW + 3, kKbTop + row * kKbCellH + 3, kKbCellW - 6, kKbCellH - 7);
}

int keyboardHit(const KeyboardView &k, int x, int y) {
  if (kDoneHit.contains(x, y)) return kKeyDone;
  if (y < kKbTop - 2 || x < 0 || x >= W) return kKeyNone;
  int row = (y - kKbTop) / kKbCellH;
  if (y < kKbTop) row = 0;
  if (row >= kKbRows) row = kKbRows - 1;
  int col = x / kKbCellW;
  if (col >= kKbCols) col = kKbCols - 1;
  return cellCode(k, row, col);
}

static void keycap(Canvas &c, const Rect &r, bool pressed, bool function, bool lit) {
  int oy = pressed ? 1 : 0;
  if (!pressed) fillRoundRect(c, r.x, r.y + 2, r.w, r.h, 9.f, 0x0000, 150);   // the cap's shadow
  uint16_t top = function ? hex(0x111A20) : hex(0x1A262E);
  uint16_t bot = function ? hex(0x0A1015) : hex(0x0E151A);
  if (pressed) { top = hex(0x1F4A40); bot = hex(0x123029); }
  if (lit) { top = hex(0x1C3A33); bot = hex(0x10241F); }
  fillRoundRectV(c, r.x, r.y + oy, r.w, r.h, 9.f, top, bot);
  strokeRoundRect(c, r.x, r.y + oy, r.w, r.h, 9.f, 0.5f, pressed || lit ? pal::phosDim : hex(0x26343C), 190);
  for (int x = r.x + 8; x < r.x + r.w - 8; ++x) pixel(c, x, r.y + oy + 1, 0xFFFF, pressed ? 50 : 30);
  if (pressed) glow(c, r.x + r.w * 0.5f, r.y + r.h * 0.5f + 1.f, r.w * 0.8f, pal::phos, 60);
}

void drawKeyboard(Canvas &c, const PageChrome &pc, const KeyboardView &k) {
  drawBackdrop(c, pc);
  // The text field: a recessed phosphor display between Back and Done.
  const int fx = 52, fy = 7, fw = 136, fh = 44;
  fillRoundRect(c, fx, fy, fw, fh, 12.f, hex(0x020605));
  glow(c, fx + fw * 0.5f, fy + fh * 0.8f, 70.f, pal::phosFaint, 140);
  strokeRoundRect(c, fx, fy, fw, fh, 12.f, 0.6f, hex(0x26343C), 200);
  for (int x = fx + 12; x < fx + fw - 12; ++x) pixel(c, x, fy + fh, 0xFFFF, 22);   // lip catching light
  for (int x = fx + 12; x < fx + fw - 12; ++x) pixel(c, x, fy + 1, 0x0000, 200);   // inner shadow
  char cap[24];
  snprintf(cap, sizeof cap, "%s", k.title);
  for (char *p = cap; *p; ++p) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
  drawCaps(c, fx + 10, fy + 15, cap, pal::phosDim, 255, 1);
  size_t len = strlen(k.text);
  char count[8];
  snprintf(count, sizeof count, "%u/%u", (unsigned)len, (unsigned)kMaxName);
  int cw = textWidth(kFontXS, count);
  text(c, kFontXS, fx + fw - 10 - cw, fy + 15, count, len >= kMaxName ? pal::warn : pal::textFaint);
  const int tx = fx + 10, maxW = fw - 22;
  const char *shown = k.text;
  while (*shown && textWidth(kFontL, shown) > maxW) ++shown;     // show the tail
  int endX = tx;
  if (len) endX = text(c, kFontL, tx, fy + 38, shown, pal::phosHot);
  if ((k.tMs % 1000) < 600) {
    fillRect(c, endX + 1, fy + 21, 2, 20, pal::phos);
    glow(c, (float)endX + 2.f, (float)(fy + 31), 8.f, pal::phos, 70);
  }

  // Keys.
  for (int row = 0; row < kKbRows; ++row) {
    for (int col = 0; col < kKbCols; ++col) {
      int code = cellCode(k, row, col);
      Rect r = keyboardKeyRect(row, col);
      bool pressed = k.pressedKey == code;
      if (code > 0) {
        keycap(c, r, pressed, false, false);
        char s[2] = {(char)code, 0};
        int oy = pressed ? 1 : 0;
        textCentered(c, kFontMB, r.x + r.w / 2, r.y + oy + 25, s, pressed ? pal::phosHot : pal::text);
        continue;
      }
      float mx = r.x + r.w * 0.5f, my = r.y + r.h * 0.5f + (pressed ? 1.f : 0.f);
      switch (code) {
        case kKeyShift: {
          bool on = k.shift && !k.symbols;
          keycap(c, r, pressed, true, on);
          iconShift(c, mx, my, 15.f, on ? pal::phosHot : pal::textDim, on);
          break;
        }
        case kKeySpace:
          keycap(c, r, pressed, true, false);
          line(c, mx - 8.f, my + 4.f, mx + 8.f, my + 4.f, 0.8f, pal::textDim);
          line(c, mx - 8.f, my + 4.f, mx - 8.f, my, 0.8f, pal::textDim);
          line(c, mx + 8.f, my + 4.f, mx + 8.f, my, 0.8f, pal::textDim);
          break;
        case kKeyMode:
          keycap(c, r, pressed, true, false);
          textCentered(c, kFontS, (int)mx, (int)my + 5, k.symbols ? "abc" : "123", pal::textDim);
          break;
        case kKeyBackspace:
          keycap(c, r, pressed, true, false);
          iconBackspace(c, mx, my, 18.f, pal::textDim);
          break;
        default: break;
      }
    }
  }
  // Back (cancel) and Done.
  drawBackButton(c, pc.backPressed);
  bool donePressed = k.pressedKey == kKeyDone;
  drawGlassDisc(c, menuCx, menuCy, btnR, donePressed, true);
  iconCheck(c, menuCx, menuCy + 1.f, 15.f, hex(0x04150F));
}

// ---------------------------------------------------------------------------
// Calibration
// ---------------------------------------------------------------------------
static const Rect kCalLeft(14, 230, 102, 42);
static const Rect kCalRight(124, 230, 102, 42);
static const Rect kCalAction(18, 228, 204, 44);
const Rect &calibratePrimaryBtn(const CalibrateView &v) {
  return v.phase == CalibrateView::Result ? kCalRight : kCalAction;
}
const Rect &calibrateSecondaryBtn() { return kCalLeft; }

// A small instrument: a bezel ring around a phosphor well, 60 engraved
// ticks that light one by one as readings arrive.
static void gauge(Canvas &c, float gx, float gy, int lit, int total, uint16_t col, float glowK) {
  const float R = 64.f;
  // Shadow, bezel, recessed well.
  for (int i = 3; i >= 1; --i) ring(c, gx, gy + 3.f, R + (float)i * 2.f, 2.f, 0x0000, (uint8_t)(50 * (4 - i)));
  fillCircle(c, gx, gy, R, hex(0x20292F));
  arc(c, gx, gy, R - 1.f, 1.3f, 280.f, 440.f, hex(0x8C99A1), 150);
  arc(c, gx, gy, R - 1.f, 1.3f, 100.f, 260.f, hex(0x05080A), 170);
  fillCircle(c, gx, gy, R - 7.f, hex(0x020807));
  glow(c, gx, gy + 6.f, R - 4.f, pal::phosFaint, 170);
  arc(c, gx, gy, R - 7.5f, 1.2f, 270.f, 450.f, 0x0000, 180);          // inner lip shadow
  if (total < 1) total = 1;
  for (int i = 0; i < 60; ++i) {
    float a = (float)i * 6.f * 0.0174533f;
    float r0 = R - 17.f, r1 = R - 11.f;
    float sx = sinf(a), sy = -cosf(a);
    bool on = i < lit * 60 / total;
    uint16_t tc = on ? col : hex(0x163029);
    line(c, gx + r0 * sx, gy + r0 * sy, gx + r1 * sx, gy + r1 * sy, on ? 0.9f : 0.7f, tc);
  }
  if (glowK > 0.f) glow(c, gx, gy, R - 10.f, col, u8f(60.f * glowK));
  // Gloss across the top of the well.
  arc(c, gx, gy, R - 22.f, 4.f, 300.f, 380.f, 0xFFFF, 14);
}

static void bigNumber(Canvas &c, int cx, int baseline, const char *s, uint16_t col) {
  int w = textWidth(kFontD, s);
  glow(c, (float)cx, (float)(baseline - 15), (float)w * 0.7f, col, 50);
  text(c, kFontD, cx - w / 2, baseline, s, col);
}

void drawCalibrate(Canvas &c, const PageChrome &pc, const CalibrateView &v) {
  drawBackdrop(c, pc);
  char buf[64];
  const float gx = 120.f, gy = 124.f;
  switch (v.phase) {
    case CalibrateView::Intro: {
      // Two watches, an arm's length apart.
      iconWatch(c, 50.f, 104.f, 34.f, pal::phos);
      iconWatch(c, 190.f, 104.f, 34.f, pal::phos);
      for (int x = 72; x <= 168; x += 6) fillRect(c, x, 104, 3, 1, pal::phosMid);
      line(c, 72.f, 97.f, 72.f, 111.f, 0.7f, pal::phosMid);
      line(c, 168.f, 97.f, 168.f, 111.f, 0.7f, pal::phosMid);
      glow(c, 120.f, 90.f, 30.f, pal::phos, 50);
      textCentered(c, kFontL, W / 2, 96, "1 m", pal::phosHot);
      textWrapped(c, kFontS, 16, 150, 208,
                  "Stand an arm's length apart with a friend who has Friend Radar open, "
                  "screens facing each other.",
                  pal::textDim, 18, 255, true);
      snprintf(buf, sizeof buf, "NOW %d DBM AT 1 M%s", (int)v.currentRef,
               v.currentCalibrated ? "" : " " FR_MIDDOT " DEFAULT");
      drawCapsCentered(c, W / 2, 216, buf, pal::textFaint, 255, 1);
      if (v.peerAvailable) {
        snprintf(buf, sizeof buf, "Measure with %s", v.peer);
        drawPill(c, kCalAction, buf, PillStyle::Primary, v.pressed == 1);
      } else {
        drawPill(c, kCalAction, "Waiting for a friend" FR_ELLIPSIS, PillStyle::Disabled, false);
      }
      drawTitleBar(c, pc, "Calibrate");
      return;
    }
    case CalibrateView::Measuring: {
      float pulse = 0.5f + 0.5f * sinf((float)v.tMs * 0.006f);
      gauge(c, gx, gy, v.samples, v.target, pal::phos, 0.4f + 0.6f * pulse);
      if (v.liveRssi < 0) snprintf(buf, sizeof buf, "%d", v.liveRssi);
      else snprintf(buf, sizeof buf, "--");
      bigNumber(c, (int)gx, (int)gy + 8, buf, pal::phosHot);
      drawCapsCentered(c, (int)gx, (int)gy + 26, "DBM", pal::phosMid, 255, 2);
      snprintf(buf, sizeof buf, "Measuring %s" FR_ELLIPSIS, v.peer);
      char fb[40];
      fitText(kFontMB, buf, 220, fb, sizeof fb);
      textCentered(c, kFontMB, W / 2, 218, fb, pal::text);
      textCentered(c, kFontS, W / 2, 240, "Keep still, 1 m apart", pal::textDim);
      snprintf(buf, sizeof buf, "%d OF %d READINGS", v.samples, v.target);
      drawCapsCentered(c, W / 2, 264, buf, pal::textFaint, 255, 1);
      drawTitleBar(c, pc, "Calibrate");
      return;
    }
    case CalibrateView::Result: {
      gauge(c, gx, gy, 1, 1, pal::live, 0.7f);
      snprintf(buf, sizeof buf, "%d", (int)v.result);
      bigNumber(c, (int)gx, (int)gy + 6, buf, pal::text);
      drawCapsCentered(c, (int)gx, (int)gy + 23, "DBM AT 1 M", pal::live, 255, 1);
      snprintf(buf, sizeof buf, "Spread %u dB over %d readings", (unsigned)v.spread, v.samples);
      textCentered(c, kFontS, W / 2, 214, buf, pal::textDim);
      drawPill(c, kCalLeft, "Retry", PillStyle::Glass, v.pressed == 2);
      drawPill(c, kCalRight, "Save", PillStyle::Primary, v.pressed == 1);
      drawTitleBar(c, pc, "Calibrate");
      return;
    }
    case CalibrateView::Failed: {
      gauge(c, gx, 112.f, 0, 1, pal::danger, 0.f);
      bigNumber(c, (int)gx, 128, "!", pal::danger);
      const char *msg = "Something went wrong. Try again.";
      switch (v.failReason) {
        case CalResult::TooFew:
          msg = "Not enough readings. Is your friend's Friend Radar open and close by?"; break;
        case CalResult::TooNoisy:
          msg = "The signal jumped around too much. Keep still and try again."; break;
        case CalResult::OutOfRange:
          msg = "That reading looks wrong for 1 m. Check the distance and try again."; break;
        default: break;
      }
      textWrapped(c, kFontS, 16, 196, 208, msg, pal::textDim, 18, 255, true);
      drawPill(c, kCalAction, "Try again", PillStyle::Primary, v.pressed == 1);
      drawTitleBar(c, pc, "Calibrate");
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// Help
// ---------------------------------------------------------------------------
struct HelpPara { const char *head; const char *body; };
static const HelpPara kHelp[] = {
  {"WHAT IT DOES", "Friend Radar listens for other EWatches over Bluetooth and guesses how far "
                   "away they are from signal strength."},
  {"NO DIRECTION", "Bluetooth can't sense direction, so everyone keeps their own spot on the "
                   "dial. Only the distance from the centre means anything."},
  {"A FRIENDLY GUESS", "Bodies, walls and pockets weaken the signal, so treat zones as a "
                       "guess. Calibrating with a friend makes them better."},
  {"MATES", "Tap someone to add them as a mate. When a mate comes near you both get a buzz "
            "and the same animation at the same moment."},
  {"HIGH FIVE", "Right next to a mate? Both shake your wrists within a second or so to "
                "celebrate."},
  {"PRIVACY", "Your watch is visible only while this app is open, or while background alerts "
              "are on. The lamp at the top says when. Back or swipe right to leave."},
};
static const int kHelpCount = (int)(sizeof kHelp / sizeof kHelp[0]);
static const int kLegendH = 156, kParaW = 206, kLineH = 18;

int helpContentHeight() {
  int h = kLegendH;
  for (int i = 0; i < kHelpCount; ++i) h += 20 + wrappedLineCount(kFontS, kParaW, kHelp[i].body) * kLineH + 12;
  return h;
}

void drawHelp(Canvas &c, const PageChrome &pc, int scroll) {
  drawBackdrop(c, pc);
  c.clip(Rect(0, listTop, W, H - listTop));
  int y0 = listTop - scroll;
  // Legend: a small scope with its zones, names and distances beside it.
  {
    const float sx = 62.f, sy = (float)y0 + 76.f;
    for (int i = 3; i >= 1; --i) ring(c, sx, sy + 3.f, 50.f + (float)i * 2.f, 2.f, 0x0000, (uint8_t)(45 * (4 - i)));
    fillCircle(c, sx, sy, 50.f, hex(0x1E272D));
    arc(c, sx, sy, 49.f, 1.1f, 280.f, 440.f, hex(0x8C99A1), 140);
    arc(c, sx, sy, 49.f, 1.1f, 100.f, 260.f, hex(0x05080A), 160);
    fillCircle(c, sx, sy, 45.f, hex(0x031009));
    glow(c, sx, sy, 45.f, pal::phosFaint, 210);
    static const float kRr[3] = {11.5f, 25.f, 36.5f};
    for (int i = 0; i < 3; ++i) ring(c, sx, sy, kRr[i], 0.5f, pal::phosDim, 220);
    sphere(c, sx, sy, 2.4f, pal::phosHot);
    static const struct { Zone z; float r; float ang; } kDots[4] = {
      {Zone::RightHere, 6.5f, 40.f}, {Zone::Near, 18.5f, 300.f}, {Zone::Around, 31.f, 110.f}, {Zone::Far, 41.f, 230.f}};
    for (const auto &d : kDots) {
      float a = d.ang * 0.0174533f;
      float dx = sx + d.r * sinf(a), dy = sy - d.r * cosf(a);
      glow(c, dx, dy, 7.f, zoneColor(d.z), 90);
      sphere(c, dx, dy, 3.2f, zoneColor(d.z));
    }
    static const char *kNames[4] = {"Right here", "Near", "Around", "Far"};
    static const char *kDist[4] = {"within reach", "about 1 to 3 m", "about 3 to 10 m", "10 m or more"};
    for (int i = 0; i < 4; ++i) {
      int ly = y0 + 20 + i * 36;
      text(c, kFontMB, 126, ly, kNames[i], zoneColor((Zone)i));
      text(c, kFontS, 126, ly + 16, kDist[i], pal::textDim);
    }
  }
  int y = y0 + kLegendH;
  for (int i = 0; i < kHelpCount; ++i) {
    drawCaps(c, 17, y + 10, kHelp[i].head, pal::phos, 255, 2);
    y += 20;
    int lines = textWrapped(c, kFontS, 17, y + 12, kParaW, kHelp[i].body, pal::text, kLineH);
    y += lines * kLineH + 12;
  }
  c.resetClip();
  int content = helpContentHeight();
  fadeEdges(c, pc, scroll < content - kListViewH);
  scrollGroove(c, content, kListViewH, scroll);
  drawTitleBar(c, pc, "How it works");
}

// ---------------------------------------------------------------------------
// Toast
// ---------------------------------------------------------------------------
void drawToast(Canvas &c, const char *msg, float t) {
  if (t <= 0.01f || !msg || !*msg) return;
  float a = clamp01f(t);
  char buf[40];
  fitText(kFontMB, msg, 180, buf, sizeof buf);
  int w = textWidth(kFontMB, buf) + 50;
  int x = (W - w) / 2;
  int y = 224 + (int)lroundf((1.f - t) * 30.f);
  uint8_t A = u8f(255.f * a);
  softShadow(c, x, y, w, 40, 20.f, 14.f, u8f(190.f * a), 5);
  fillRoundRectV(c, x, y, w, 40, 20.f, hex(0x1B2A31), hex(0x0C1418), u8f(250.f * a));
  strokeRoundRect(c, x, y, w, 40, 20.f, 0.5f, hex(0x3A4E57), u8f(220.f * a));
  for (int xx = x + 20; xx < x + w - 20; ++xx) pixel(c, xx, y + 1, 0xFFFF, u8f(40.f * a));
  if (a > 0.5f) drawLed(c, (float)x + 21.f, (float)y + 20.f, pal::live, (a - 0.5f) * 2.f);
  text(c, kFontMB, x + 34, y + 26, buf, pal::text, A);
}

// ---------------------------------------------------------------------------
// Dates
// ---------------------------------------------------------------------------
static void civilFromDays2000(uint32_t days, int &y, unsigned &m, unsigned &d) {
  // Howard Hinnant's civil_from_days, shifted from 1970 to 2000.
  int64_t z = (int64_t)days + 10957 + 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned doe = (unsigned)(z - era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t yy = (int64_t)yoe + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = (int)(yy + (m <= 2));
}

void formatWhen(uint32_t whenSec, uint32_t nowSec, char *out, size_t cap) {
  static const char *kWd[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char *kMon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  uint32_t day = whenSec / 86400u, today = nowSec / 86400u;
  unsigned hh = (unsigned)((whenSec % 86400u) / 3600u), mm = (unsigned)((whenSec % 3600u) / 60u);
  if (day == today)            snprintf(out, cap, "Today %02u:%02u", hh, mm);
  else if (day + 1 == today)   snprintf(out, cap, "Yesterday %02u:%02u", hh, mm);
  else if (day < today && today - day < 7)
    snprintf(out, cap, "%s %02u:%02u", kWd[(day + 6) % 7], hh, mm);   // 2000-01-01 was a Saturday
  else {
    int y; unsigned m, d;
    civilFromDays2000(day, y, m, d);
    snprintf(out, cap, "%u %s %02u:%02u", d, kMon[(m - 1) % 12], hh, mm);
  }
}

void formatMinutes(uint16_t minutes, char *out, size_t cap) {
  if (minutes < 1)        snprintf(out, cap, "<1 min");
  else if (minutes < 60)  snprintf(out, cap, "%u min", (unsigned)minutes);
  else if (minutes % 60)  snprintf(out, cap, "%u h %u min", (unsigned)(minutes / 60), (unsigned)(minutes % 60));
  else                    snprintf(out, cap, "%u h", (unsigned)(minutes / 60));
}

}  // namespace fr
