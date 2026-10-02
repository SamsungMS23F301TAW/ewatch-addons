#include "fr_ui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace fr {

static inline uint8_t u8f(float v) { return v <= 0.f ? 0 : (v >= 255.f ? 255 : (uint8_t)(v + 0.5f)); }

uint16_t zoneColor(Zone z) {
  switch (z) {
    case Zone::RightHere: return pal::zoneHere;
    case Zone::Near:      return pal::zoneNear;
    case Zone::Around:    return pal::zoneAround;
    case Zone::Far:       return pal::zoneFar;
    default:              return pal::zoneLost;
  }
}

// Ten warm and cool lights that never read as the scope's phosphor green,
// ordered by hue so that neighbours are the similar ones.
static const uint32_t kMateColors[kMateColorCount] = {
  0xFFB547,   // amber
  0xFFD966,   // gold
  0xFFA98A,   // peach
  0xFF7F66,   // coral
  0xFF6F9F,   // rose
  0xE07BF0,   // orchid
  0xC9A6FF,   // lilac
  0xA58CFF,   // violet
  0x6FA8FF,   // azure
  0x5CCBFF,   // sky
};

int mateColorIndex(uint32_t id) { return (int)(mix32(id ^ 0xC0FFEEu) % (uint32_t)kMateColorCount); }
uint16_t mateColorAt(int i) { return hex(kMateColors[((i % kMateColorCount) + kMateColorCount) % kMateColorCount]); }
uint16_t mateColor(uint32_t id) { return mateColorAt(mateColorIndex(id)); }

uint16_t inkOn(uint16_t fill) { return luma(fill) > 140 ? hex(0x06120E) : hex(0xFFFFFF); }

// ---------------------------------------------------------------------------
// Icons
// ---------------------------------------------------------------------------
static const float kStroke = 1.05f;

void iconChevronLeft(Canvas &c, float cx, float cy, float s, uint16_t col, uint8_t a) {
  line(c, cx + s * 0.22f, cy - s * 0.42f, cx - s * 0.2f, cy, 1.3f, col, a);
  line(c, cx - s * 0.2f, cy, cx + s * 0.22f, cy + s * 0.42f, 1.3f, col, a);
}

void iconChevronRight(Canvas &c, float cx, float cy, float s, uint16_t col, uint8_t a) {
  line(c, cx - s * 0.18f, cy - s * 0.38f, cx + s * 0.18f, cy, kStroke, col, a);
  line(c, cx + s * 0.18f, cy, cx - s * 0.18f, cy + s * 0.38f, kStroke, col, a);
}

void iconDots(Canvas &c, float cx, float cy, uint16_t col, uint8_t a) {
  for (int i = -1; i <= 1; ++i) fillCircle(c, cx + i * 6.5f, cy, 2.1f, col, a);
}

void iconCheck(Canvas &c, float cx, float cy, float s, uint16_t col, uint8_t a) {
  line(c, cx - s * 0.42f, cy + s * 0.02f, cx - s * 0.12f, cy + s * 0.32f, 1.5f, col, a);
  line(c, cx - s * 0.12f, cy + s * 0.32f, cx + s * 0.45f, cy - s * 0.32f, 1.5f, col, a);
}

void iconPlus(Canvas &c, float cx, float cy, float s, uint16_t col, uint8_t a) {
  line(c, cx - s * 0.45f, cy, cx + s * 0.45f, cy, 1.3f, col, a);
  line(c, cx, cy - s * 0.45f, cx, cy + s * 0.45f, 1.3f, col, a);
}

void iconCross(Canvas &c, float cx, float cy, float s, uint16_t col, uint8_t a) {
  line(c, cx - s * 0.38f, cy - s * 0.38f, cx + s * 0.38f, cy + s * 0.38f, 1.2f, col, a);
  line(c, cx - s * 0.38f, cy + s * 0.38f, cx + s * 0.38f, cy - s * 0.38f, 1.2f, col, a);
}

void iconHeart(Canvas &c, float cx, float cy, float s, uint16_t col, uint8_t a) {
  // Implicit heart curve, 3x3 supersampled.
  int x0 = (int)floorf(cx - s), x1 = (int)ceilf(cx + s);
  int y0 = (int)floorf(cy - s), y1 = (int)ceilf(cy + s);
  const float k = 1.25f / s;
  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      int hits = 0;
      for (int sy = 0; sy < 3; ++sy)
        for (int sx = 0; sx < 3; ++sx) {
          float u = ((float)x + (sx + 0.5f) / 3.f - cx) * k;
          float v = -((float)y + (sy + 0.5f) / 3.f - cy) * k + 0.25f;
          float q = u * u + v * v - 1.f;
          if (q * q * q - u * u * v * v * v <= 0.f) ++hits;
        }
      if (hits) pixel(c, x, y, col, (uint8_t)(hits * a / 9));
    }
  }
}

void iconPeople(Canvas &c, float cx, float cy, float s, uint16_t col, uint8_t a) {
  float k = s / 20.f;
  ring(c, cx + 4.5f * k, cy - 5.f * k, 2.7f * k, 0.85f, col, (uint8_t)(a * 3 / 4));
  arc(c, cx + 5.f * k, cy + 6.f * k, 5.5f * k, 0.85f, 300.f, 420.f, col, (uint8_t)(a * 3 / 4));
  ring(c, cx - 2.5f * k, cy - 3.5f * k, 3.4f * k, kStroke, col, a);
  arc(c, cx - 2.5f * k, cy + 8.5f * k, 7.f * k, kStroke, 280.f, 440.f, col, a);
}

void iconTag(Canvas &c, float cx, float cy, float s, uint16_t col, uint8_t a) {
  float k = s / 20.f;
  float l = cx - 8.f * k, r = cx + 4.f * k, t = cy - 6.f * k, b = cy + 6.f * k, tip = cx + 9.5f * k;
  line(c, l, t, r, t, kStroke, col, a);
  line(c, r, t, tip, cy, kStroke, col, a);
  line(c, tip, cy, r, b, kStroke, col, a);
  line(c, r, b, l, b, kStroke, col, a);
  line(c, l, b, l, t, kStroke, col, a);
  ring(c, cx - 3.5f * k, cy, 1.8f * k, 0.8f, col, a);
}

void iconTarget(Canvas &c, float cx, float cy, float s, uint16_t col, uint8_t a) {
  float k = s / 20.f;
  ring(c, cx, cy, 7.5f * k, kStroke, col, a);
  ring(c, cx, cy, 3.3f * k, kStroke, col, a);
  fillCircle(c, cx, cy, 1.2f * k, col, a);
  line(c, cx, cy - 10.5f * k, cx, cy - 8.f * k, 0.9f, col, a);
  line(c, cx, cy + 8.f * k, cx, cy + 10.5f * k, 0.9f, col, a);
  line(c, cx - 10.5f * k, cy, cx - 8.f * k, cy, 0.9f, col, a);
  line(c, cx + 8.f * k, cy, cx + 10.5f * k, cy, 0.9f, col, a);
}

void iconBell(Canvas &c, float cx, float cy, float s, uint16_t col, uint8_t a) {
  float k = s / 20.f;
  arc(c, cx, cy - 1.f * k, 6.f * k, kStroke, 270.f, 450.f, col, a);
  line(c, cx - 6.f * k, cy - 1.f * k, cx - 6.5f * k, cy + 5.f * k, kStroke, col, a);
  line(c, cx + 6.f * k, cy - 1.f * k, cx + 6.5f * k, cy + 5.f * k, kStroke, col, a);
  line(c, cx - 8.5f * k, cy + 5.5f * k, cx + 8.5f * k, cy + 5.5f * k, kStroke, col, a);
  fillCircle(c, cx, cy + 8.3f * k, 1.6f * k, col, a);
  line(c, cx, cy - 9.f * k, cx, cy - 7.f * k, 0.9f, col, a);
}

void iconMoon(Canvas &c, float cx, float cy, float s, uint16_t col, uint16_t bg, uint8_t a) {
  fillCircle(c, cx, cy, s * 0.42f, col, a);
  fillCircle(c, cx + s * 0.2f, cy - s * 0.15f, s * 0.36f, bg, a);
}

void iconClock(Canvas &c, float cx, float cy, float s, uint16_t col, uint8_t a) {
  float k = s / 20.f;
  ring(c, cx, cy, 8.f * k, kStroke, col, a);
  line(c, cx, cy, cx, cy - 5.f * k, kStroke, col, a);
  line(c, cx, cy, cx + 3.8f * k, cy + 2.2f * k, kStroke, col, a);
}

void iconQuestion(Canvas &c, float cx, float cy, float s, uint16_t col, uint8_t a) {
  (void)s;
  ring(c, cx, cy, 8.f, kStroke, col, a);
  textCentered(c, kFontS, (int)cx, (int)cy + 5, "?", col, a);
}

void iconRefresh(Canvas &c, float cx, float cy, float s, uint16_t col, uint8_t a) {
  float k = s / 20.f;
  arc(c, cx, cy, 7.f * k, kStroke, 40.f, 330.f, col, a);
  // Arrowhead at the arc's start (40 degrees), pointing clockwise.
  float ax = cx + 7.f * k * sinf(0.698f), ay = cy - 7.f * k * cosf(0.698f);
  line(c, ax, ay, ax - 4.f * k, ay - 1.f * k, kStroke, col, a);
  line(c, ax, ay, ax + 0.5f * k, ay - 4.2f * k, kStroke, col, a);
}

void iconPencil(Canvas &c, float cx, float cy, float s, uint16_t col, uint8_t a) {
  float k = s / 20.f;
  line(c, cx - 5.f * k, cy + 5.f * k, cx + 4.5f * k, cy - 4.5f * k, 2.2f * k, col, a);
  line(c, cx - 7.f * k, cy + 7.f * k, cx - 5.5f * k, cy + 5.5f * k, 1.f, col, a);
  line(c, cx + 5.5f * k, cy - 5.5f * k, cx + 7.f * k, cy - 7.f * k, 1.6f * k, col, a);
}

void iconBackspace(Canvas &c, float cx, float cy, float s, uint16_t col, uint8_t a) {
  float l = cx - s * 0.55f, r = cx + s * 0.5f, t = cy - s * 0.32f, b = cy + s * 0.32f;
  float m = cx - s * 0.22f;
  line(c, l, cy, m, t, 1.0f, col, a);
  line(c, l, cy, m, b, 1.0f, col, a);
  line(c, m, t, r, t, 1.0f, col, a);
  line(c, m, b, r, b, 1.0f, col, a);
  line(c, r, t, r, b, 1.0f, col, a);
  iconCross(c, cx + s * 0.14f, cy, s * 0.36f, col, a);
}

void iconShift(Canvas &c, float cx, float cy, float s, uint16_t col, bool filled) {
  float t = cy - s * 0.5f, m = cy, b = cy + s * 0.45f;
  float hw = s * 0.45f, sw = s * 0.2f;
  if (filled) {
    for (int y = (int)t; y <= (int)b; ++y) {
      float half = (float)y < m ? hw * ((float)y - t) / (m - t) : sw;
      fillRect(c, (int)(cx - half + 0.5f), y, (int)(2 * half + 0.5f), 1, col);
    }
  }
  line(c, cx, t, cx - hw, m, 1.0f, col);
  line(c, cx, t, cx + hw, m, 1.0f, col);
  line(c, cx - hw, m, cx - sw, m, 1.0f, col);
  line(c, cx + hw, m, cx + sw, m, 1.0f, col);
  line(c, cx - sw, m, cx - sw, b, 1.0f, col);
  line(c, cx + sw, m, cx + sw, b, 1.0f, col);
  line(c, cx - sw, b, cx + sw, b, 1.0f, col);
}

void iconWaves(Canvas &c, float cx, float cy, float s, uint16_t col, uint8_t a) {
  fillCircle(c, cx, cy, s * 0.15f, col, a);
  arc(c, cx, cy, s * 0.42f, 0.9f, 45.f, 135.f, col, a);
  arc(c, cx, cy, s * 0.42f, 0.9f, 225.f, 315.f, col, a);
  arc(c, cx, cy, s * 0.72f, 0.9f, 50.f, 130.f, col, (uint8_t)(a * 3 / 5));
  arc(c, cx, cy, s * 0.72f, 0.9f, 230.f, 310.f, col, (uint8_t)(a * 3 / 5));
}

void iconWatch(Canvas &c, float cx, float cy, float s, uint16_t screenGlow) {
  // A tiny EWatch: strap, gunmetal case, a glowing phosphor screen.
  float k = s / 30.f;
  fillRoundRectV(c, (int)(cx - 7 * k), (int)(cy - 21 * k), (int)(14 * k), (int)(42 * k), 3.f,
                 hex(0x1A2026), hex(0x0E1216));
  softShadow(c, (int)(cx - 11 * k), (int)(cy - 13 * k), (int)(22 * k), (int)(26 * k), 6.f * k, 6.f, 120, 2);
  fillRoundRectV(c, (int)(cx - 11 * k), (int)(cy - 13 * k), (int)(22 * k), (int)(26 * k), 6.f * k,
                 hex(0x55616B), hex(0x1E252C));
  fillRoundRectV(c, (int)(cx - 8.5f * k), (int)(cy - 10.5f * k), (int)(17 * k), (int)(21 * k), 4.f * k,
                 hex(0x0B2622), hex(0x041110));
  glow(c, cx, cy, 9.f * k, screenGlow, 150);
  fillCircle(c, cx, cy, 1.6f * k, blend(screenGlow, 0xFFFF, 120));
}

// ---------------------------------------------------------------------------
// Materials
// ---------------------------------------------------------------------------
void buildPageBackground(Canvas &c) {
  const float W = (float)c.w, H = (float)c.h;
  for (int y = 0; y < c.h; ++y) {
    float ty = (float)y / (H - 1.f);
    for (int x = 0; x < c.w; ++x) {
      // Base gradient, a soft phosphor glow from above, corner vignette, grain.
      float r = 5.5f + 4.f * ty, g = 9.f + 5.f * ty, b = 12.f + 6.f * ty;
      float dx = ((float)x - W * 0.5f) / W, dy = ((float)y + 30.f) / H;
      float glowK = 1.f - (dx * dx * 2.4f + dy * dy * 1.6f);
      if (glowK > 0.f) { glowK *= glowK; r += 6.f * glowK; g += 22.f * glowK; b += 17.f * glowK; }
      float vx = ((float)x - W * 0.5f) / (W * 0.5f), vy = ((float)y - H * 0.5f) / (H * 0.5f);
      float vig = 1.f - 0.28f * (vx * vx * vy * vy + 0.25f * (vx * vx + vy * vy));
      float n = grainAt(x, y, 0xA5A5u) * 1.6f;
      c.px[(size_t)y * c.w + x] = ditherRgb(r * vig + n, g * vig + n, b * vig + n, x, y);
    }
  }
}

void drawGlassDisc(Canvas &c, float cx, float cy, float r, bool pressed, bool primary) {
  int ir = (int)ceilf(r);
  softShadow(c, (int)(cx - r), (int)(cy - r), 2 * ir, 2 * ir, r, 8.f, 140, 3);
  // Metal rim with light from the top-left.
  fillCircle(c, cx, cy, r, hex(0x2C363E));
  arc(c, cx, cy, r - 0.8f, 0.9f, 270.f, 420.f, hex(0x7D8A93), 150);
  arc(c, cx, cy, r - 0.8f, 0.9f, 100.f, 230.f, hex(0x05080A), 160);
  if (primary) {
    sphere(c, cx, cy, r - 2.2f, pressed ? pal::phosMid : pal::phos);
    return;
  }
  fillCircle(c, cx, cy, r - 2.2f, pressed ? hex(0x14232A) : hex(0x0A1216));
  // Glass: a faint inner glow at the bottom and a highlight across the top.
  glow(c, cx, cy + r * 0.45f, r * 0.9f, pal::phosFaint, pressed ? 200 : 90);
  arc(c, cx, cy, r - 4.f, 1.2f, 300.f, 400.f, 0xFFFF, pressed ? 40 : 28);
  if (pressed) ring(c, cx, cy, r - 2.2f, 1.f, pal::phos, 120);
}

void drawBackButton(Canvas &c, bool pressed) {
  drawGlassDisc(c, layout::backCx, layout::backCy, layout::btnR, pressed);
  iconChevronLeft(c, layout::backCx - 1.f, layout::backCy, 15.f, pressed ? pal::phosHot : pal::text);
}

void drawMenuButton(Canvas &c, bool pressed) {
  drawGlassDisc(c, layout::menuCx, layout::menuCy, layout::btnR, pressed);
  iconDots(c, layout::menuCx, layout::menuCy, pressed ? pal::phosHot : pal::text);
}

void drawPageTitle(Canvas &c, const char *title, uint8_t a) {
  char buf[32];
  fitText(kFontMB, title, 112, buf, sizeof buf);
  textCenteredTracked(c, kFontMB, layout::W / 2, 33, buf, pal::text, a, 1);
  // Engraved hairline that fades out at both ends.
  for (int x = 36; x < 204; ++x) {
    float t = (float)(x - 36) / 167.f;
    float k = sinf(t * 3.14159265f);
    pixel(c, x, 52, pal::engrave, (uint8_t)(200.f * k));
    pixel(c, x, 53, hex(0x2E3E46), (uint8_t)(150.f * k * a / 255.f));
  }
}

void drawTile(Canvas &c, int x, int y, int w, int h, bool pressed, float r, uint8_t a) {
  softShadow(c, x, y, w, h, r, 7.f, (uint8_t)(120 * a / 255), 3);
  uint16_t top = pressed ? hex(0x1B3036) : pal::panelTop;
  uint16_t bot = pressed ? hex(0x10202A) : pal::panelBot;
  fillRoundRectV(c, x, y, w, h, r, top, bot, a);
  strokeRoundRect(c, x, y, w, h, r, 0.5f, pressed ? pal::phosDim : pal::panelEdge, (uint8_t)(170 * a / 255));
  // Light catching the top edge of the glass.
  for (int xx = x + (int)r; xx < x + w - (int)r; ++xx) {
    float t = (float)(xx - x - (int)r) / (float)(w - 2 * (int)r);
    pixel(c, xx, y + 1, 0xFFFF, (uint8_t)((14.f + 26.f * sinf(t * 3.14159265f)) * a / 255.f));
  }
}

void drawIconWell(Canvas &c, float cx, float cy, float r) {
  fillCircle(c, cx, cy, r, pal::well);
  arc(c, cx, cy, r - 0.6f, 0.8f, 280.f, 440.f, 0x0000, 170);          // inner shadow, top
  arc(c, cx, cy, r - 0.4f, 0.7f, 110.f, 250.f, hex(0x3A4A52), 120);   // catch light, bottom
}

void drawPill(Canvas &c, const Rect &r, const char *label, PillStyle style, bool pressed, const Font &f) {
  const float rad = r.h * 0.5f;
  int oy = pressed ? 1 : 0;
  uint16_t top, bot, edge, ink;
  uint8_t edgeA = 170;
  switch (style) {
    case PillStyle::Primary:
      top = pressed ? pal::phosMid : hex(0x6AF7C9); bot = pressed ? hex(0x1D8A68) : hex(0x23B585);
      edge = pal::phosHot; ink = hex(0x04150F); edgeA = 90;
      break;
    case PillStyle::Danger:
      top = pressed ? hex(0x3A1A20) : hex(0x2A1418); bot = hex(0x160A0D);
      edge = hex(0x6E2A33); ink = pal::danger;
      break;
    case PillStyle::DangerSolid:
      top = hex(0xFF7484); bot = hex(0xC93448); edge = hex(0xFFB0BA); ink = 0xFFFF; edgeA = 90;
      break;
    case PillStyle::Disabled:
      top = hex(0x111A1F); bot = hex(0x0B1115); edge = hex(0x1E2A31); ink = pal::textFaint;
      break;
    default:
      top = pressed ? hex(0x1C2B33) : pal::panelTop; bot = pal::panelBot;
      edge = pal::panelEdge; ink = pal::text;
      break;
  }
  if (style == PillStyle::Primary || style == PillStyle::DangerSolid) {
    uint16_t gc = style == PillStyle::Primary ? pal::phos : pal::danger;
    for (int i = 3; i >= 1; --i)
      strokeRoundRect(c, r.x - i, r.y - i + oy, r.w + 2 * i, r.h + 2 * i, rad + i, 0.8f, gc, (uint8_t)(16 * (4 - i)));
  } else {
    softShadow(c, r.x, r.y, r.w, r.h, rad, 7.f, 120, 3);
  }
  fillRoundRectV(c, r.x, r.y + oy, r.w, r.h, rad, top, bot);
  strokeRoundRect(c, r.x, r.y + oy, r.w, r.h, rad, 0.5f, edge, edgeA);
  for (int xx = r.x + (int)rad; xx < r.x + r.w - (int)rad; ++xx)
    pixel(c, xx, r.y + oy + 1, 0xFFFF, style == PillStyle::Primary ? 90 : 26);
  char buf[32];
  fitText(f, label, r.w - 20, buf, sizeof buf);
  int base = r.y + oy + (r.h + f.capH) / 2;
  textCentered(c, f, r.x + r.w / 2, base, buf, ink);
}

void drawSwitch(Canvas &c, int x, int y, float pos, bool pressed) {
  if (pos < 0.f) pos = 0.f;
  if (pos > 1.f) pos = 1.f;
  const int w = 50, h = 28;
  // Track: a recessed slot that fills with phosphor light when on.
  fillRoundRect(c, x, y, w, h, 14.f, pal::well);
  if (pos > 0.f) fillRoundRectV(c, x, y, w, h, 14.f, hex(0x2BC48F), hex(0x168A63), (uint8_t)(255.f * pos));
  strokeRoundRect(c, x, y, w, h, 14.f, 0.6f, pos > 0.5f ? pal::phos : hex(0x24323A), 160);
  for (int xx = x + 10; xx < x + w - 10; ++xx) pixel(c, xx, y + 1, 0x0000, 120);   // inner shadow
  // Knob: a glossy bead with its own shadow.
  float kx = (float)x + 14.f + pos * (float)(w - 28), ky = (float)y + 14.f;
  softShadow(c, (int)(kx - 11), (int)(ky - 11), 22, 22, 11.f, 5.f, 140, 2);
  sphere(c, kx, ky, pressed ? 11.5f : 11.f, blend(hex(0xAEB9B5), hex(0xF4FFFB), (uint8_t)(255.f * pos)));
}

void drawCaps(Canvas &c, int x, int baseline, const char *s, uint16_t col, uint8_t a, int track) {
  textTracked(c, kFontXS, x, baseline, s, col, a, track);
}

void drawCapsCentered(Canvas &c, int cx, int baseline, const char *s, uint16_t col, uint8_t a, int track) {
  textCenteredTracked(c, kFontXS, cx, baseline, s, col, a, track);
}

void drawOrb(Canvas &c, float cx, float cy, float r, uint16_t col, float halo, char initial, uint8_t a) {
  if (halo > 0.f) glow(c, cx, cy, r * 2.5f, col, u8f(170.f * halo * a / 255.f));
  sphere(c, cx, cy, r, col, a);
  if (initial) {
    char s[2] = {initial, 0};
    if (s[0] >= 'a' && s[0] <= 'z') s[0] = (char)(s[0] - 32);
    const Font &f = r >= 14.f ? kFontL : (r >= 10.f ? kFontMB : kFontS);
    int w = textWidth(f, s);
    text(c, f, (int)lroundf(cx - w / 2.f), (int)lroundf(cy + f.capH / 2.f), s, hex(0x0A1612), (uint8_t)(200 * a / 255));
  }
}

void drawLed(Canvas &c, float cx, float cy, uint16_t col, float glowK) {
  if (glowK > 0.f) glow(c, cx, cy, 12.f, col, u8f(150.f * glowK));
  sphere(c, cx, cy, 3.f, blend(hex(0x1A2428), col, u8f(110.f + 145.f * glowK)));
  if (glowK > 0.f) fillCircle(c, cx, cy, 1.6f, blend(col, 0xFFFF, 110), u8f(200.f * glowK));
}

void drawSignalMeter(Canvas &c, int x, int baseline, int bars, uint16_t on) {
  for (int i = 0; i < 5; ++i) {
    int hgt = 4 + i * 2;
    bool lit = i < bars;
    fillRoundRect(c, x + i * 5, baseline - hgt, 3, hgt, 1.f, lit ? on : hex(0x22313A));
  }
}

void composeSlide(Canvas &c, const uint16_t *under, float k, bool forward) {
  if (!under || c.w != layout::W || c.h != layout::H) return;
  const int W = layout::W;
  if (k < 0.f) k = 0.f;
  const float kk = k > 1.f ? 1.f : k;
  // The front page slides; the back page drifts with parallax and dims.
  int edge = (int)lroundf((float)W * (forward ? 1.f - k : k));
  if (edge <= 0) return;                       // front page fully in place
  if (edge > W) edge = W;
  const int bs = (int)lroundf(0.3f * (float)W * (forward ? kk : 1.f - kk));
  const uint8_t dimA = u8f(255.f * 0.55f * (forward ? kk : 1.f - kk));
  uint16_t tmp[layout::W];
  for (int y = 0; y < layout::H; ++y) {
    uint16_t *row = c.px + (size_t)y * W;
    const uint16_t *old = under + (size_t)y * W;
    memcpy(tmp, row, sizeof tmp);
    const uint16_t *front = forward ? tmp : old;
    const uint16_t *back = forward ? old : tmp;
    for (int x = 0; x < edge; ++x) {
      int sx = x + bs;
      uint16_t p = back[sx < W ? sx : W - 1];
      int d = edge - x;                          // shadow cast by the front page's edge
      uint8_t a = dimA;
      if (d < 14) {
        uint8_t sh = u8f(130.f * (1.f - (float)d / 14.f));
        a = (uint8_t)(a + (((255 - a) * sh) >> 8));
      }
      row[x] = a ? blend(p, 0x0000, a) : p;
    }
    for (int x = edge; x < W; ++x) row[x] = front[x - edge];
  }
}

void formatAgo(uint32_t s, char *out, size_t cap) {
  if (s < 5)              snprintf(out, cap, "just now");
  else if (s < 60)        snprintf(out, cap, "%lu s ago", (unsigned long)s);
  else if (s < 3600)      snprintf(out, cap, "%lu min ago", (unsigned long)(s / 60));
  else if (s < 86400)     snprintf(out, cap, "%lu h ago", (unsigned long)(s / 3600));
  else if (s < 2 * 86400) snprintf(out, cap, "yesterday");
  else                    snprintf(out, cap, "%lu days ago", (unsigned long)(s / 86400));
}

}  // namespace fr
