// Rep Counter: anti-aliased 2D drawing on an RGB565 framebuffer.
#include "rep_gfx.h"
#include <math.h>
#include <string.h>

namespace rgfx {

static inline float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
static inline uint8_t cov(float d) {      // signed distance (px, <0 inside) -> alpha
  return (uint8_t)(clamp01(0.5f - d) * 255.f + 0.5f);
}
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int imin(int a, int b) { return a < b ? a : b; }

uint16_t blend(uint16_t fg, uint16_t bg, uint8_t a) {
  if (a == 0) return bg;
  if (a == 255) return fg;
  uint32_t wa = (uint32_t)a + (a >> 7);   // 0..255 -> 0..256
  uint32_t ia = 256 - wa;
  uint32_t r = (((fg >> 11) & 31) * wa + ((bg >> 11) & 31) * ia) >> 8;
  uint32_t g = (((fg >> 5) & 63) * wa + ((bg >> 5) & 63) * ia) >> 8;
  uint32_t b = ((fg & 31) * wa + (bg & 31) * ia) >> 8;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

uint8_t luma(uint16_t c) {
  return (uint8_t)((red8(c) * 77 + green8(c) * 150 + blue8(c) * 29) >> 8);
}

void Surface::clip(int x, int y, int ww, int hh) {
  cx0 = imax(0, x);
  cy0 = imax(0, y);
  cx1 = imin(w, x + ww);
  cy1 = imin(h, y + hh);
  if (cx1 < cx0) cx1 = cx0;
  if (cy1 < cy0) cy1 = cy0;
}

void fill(Surface &s, uint16_t c) { fillRect(s, 0, 0, s.w, s.h, c); }

void fillRect(Surface &s, int x, int y, int w, int h, uint16_t c) {
  int x0 = imax(x, s.cx0), y0 = imax(y, s.cy0);
  int x1 = imin(x + w, s.cx1), y1 = imin(y + h, s.cy1);
  if (x1 <= x0 || y1 <= y0) return;
  for (int yy = y0; yy < y1; yy++) {
    uint16_t *p = s.px + yy * s.w + x0;
    for (int xx = x0; xx < x1; xx++) *p++ = c;
  }
}

void fillRectV(Surface &s, int x, int y, int w, int h, uint16_t top, uint16_t bottom) {
  for (int i = 0; i < h; i++) {
    uint8_t a = (uint8_t)(h > 1 ? (i * 255) / (h - 1) : 0);
    fillRect(s, x, y + i, w, 1, blend(bottom, top, a));
  }
}

// Coverage of one rounded corner square; (ccx, ccy) is the circle centre.
static void cornerAA(Surface &s, int x0, int y0, int n, float ccx, float ccy, float r,
                     uint16_t c) {
  for (int py = y0; py < y0 + n; py++) {
    for (int px = x0; px < x0 + n; px++) {
      float dx = px + 0.5f - ccx, dy = py + 0.5f - ccy;
      float d = sqrtf(dx * dx + dy * dy) - r;
      s.plot(px, py, c, cov(d));
    }
  }
}

void fillRoundRect(Surface &s, float xf, float yf, float wf, float hf, float rf, uint16_t c) {
  int x = (int)lroundf(xf), y = (int)lroundf(yf), w = (int)lroundf(wf), h = (int)lroundf(hf);
  if (w <= 0 || h <= 0) return;
  float rmax = (w < h ? w : h) * 0.5f;
  if (rf > rmax) rf = rmax;
  if (rf < 0.5f) { fillRect(s, x, y, w, h, c); return; }
  int r = (int)ceilf(rf);
  fillRect(s, x + r, y, w - 2 * r, h, c);
  fillRect(s, x, y + r, r, h - 2 * r, c);
  fillRect(s, x + w - r, y + r, r, h - 2 * r, c);
  cornerAA(s, x, y, r, x + rf, y + rf, rf, c);
  cornerAA(s, x + w - r, y, r, x + w - rf, y + rf, rf, c);
  cornerAA(s, x, y + h - r, r, x + rf, y + h - rf, rf, c);
  cornerAA(s, x + w - r, y + h - r, r, x + w - rf, y + h - rf, rf, c);
}

void strokeRoundRect(Surface &s, float xf, float yf, float wf, float hf, float rf,
                     float t, uint16_t c) {
  // Distance to a rounded box, banded to a stroke of width t (inside edge).
  int x0 = (int)floorf(xf), y0 = (int)floorf(yf);
  int x1 = (int)ceilf(xf + wf), y1 = (int)ceilf(yf + hf);
  float cx = xf + wf * 0.5f, cy = yf + hf * 0.5f;
  float hx = wf * 0.5f, hy = hf * 0.5f;
  float rmax = (hx < hy ? hx : hy);
  if (rf > rmax) rf = rmax;
  for (int py = imax(y0, s.cy0); py < imin(y1, s.cy1); py++) {
    float qy = fabsf(py + 0.5f - cy) - (hy - rf);
    for (int px = imax(x0, s.cx0); px < imin(x1, s.cx1); px++) {
      float qx = fabsf(px + 0.5f - cx) - (hx - rf);
      float ox = qx > 0.f ? qx : 0.f, oy = qy > 0.f ? qy : 0.f;
      float d = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0.f) - rf;   // <0 inside
      float band = fabsf(d + t * 0.5f) - t * 0.5f;
      s.plot(px, py, c, cov(band));
    }
  }
}

void fillCircle(Surface &s, float cx, float cy, float r, uint16_t c) {
  int y0 = (int)floorf(cy - r - 1), y1 = (int)ceilf(cy + r + 1);
  for (int py = imax(y0, s.cy0); py < imin(y1, s.cy1); py++) {
    float dy = py + 0.5f - cy;
    float hw2 = (r + 1.f) * (r + 1.f) - dy * dy;
    if (hw2 <= 0.f) continue;
    float hw = sqrtf(hw2);
    int xa = (int)floorf(cx - hw), xb = (int)ceilf(cx + hw);
    // Solid core where even the far pixel corner is inside.
    float in2 = (r - 0.75f) * (r - 0.75f) - dy * dy;
    int sa = xb, sb = xa;
    if (in2 > 0.f) {
      float iw = sqrtf(in2);
      sa = (int)ceilf(cx - iw);
      sb = (int)floorf(cx + iw);
    }
    for (int px = imax(xa, s.cx0); px < imin(xb, s.cx1); px++) {
      if (px >= sa && px < sb) {
        s.px[py * s.w + px] = c;
        continue;
      }
      float dx = px + 0.5f - cx;
      s.plot(px, py, c, cov(sqrtf(dx * dx + dy * dy) - r));
    }
  }
}

void strokeCircle(Surface &s, float cx, float cy, float r, float t, uint16_t c) {
  int y0 = (int)floorf(cy - r - t), y1 = (int)ceilf(cy + r + t);
  int x0 = (int)floorf(cx - r - t), x1 = (int)ceilf(cx + r + t);
  float rin = r - t;
  for (int py = imax(y0, s.cy0); py < imin(y1, s.cy1); py++) {
    float dy = py + 0.5f - cy;
    for (int px = imax(x0, s.cx0); px < imin(x1, s.cx1); px++) {
      float dx = px + 0.5f - cx;
      float d = sqrtf(dx * dx + dy * dy);
      if (d < rin - 1.f || d > r + 1.f) continue;
      float band = fabsf(d - (r - t * 0.5f)) - t * 0.5f;
      s.plot(px, py, c, cov(band));
    }
  }
}

void capsule(Surface &s, float ax, float ay, float bx, float by, float r, uint16_t c) {
  int x0 = (int)floorf(fminf(ax, bx) - r - 1), x1 = (int)ceilf(fmaxf(ax, bx) + r + 1);
  int y0 = (int)floorf(fminf(ay, by) - r - 1), y1 = (int)ceilf(fmaxf(ay, by) + r + 1);
  float vx = bx - ax, vy = by - ay;
  float vv = vx * vx + vy * vy;
  for (int py = imax(y0, s.cy0); py < imin(y1, s.cy1); py++) {
    for (int px = imax(x0, s.cx0); px < imin(x1, s.cx1); px++) {
      float wx = px + 0.5f - ax, wy = py + 0.5f - ay;
      float t = vv > 0.f ? (wx * vx + wy * vy) / vv : 0.f;
      t = clamp01(t);
      float dx = wx - vx * t, dy = wy - vy * t;
      float d = sqrtf(dx * dx + dy * dy) - r;
      if (d < -1.f) s.px[py * s.w + px] = c;
      else s.plot(px, py, c, cov(d));
    }
  }
}

void fillTriangle(Surface &s, float x0, float y0, float x1, float y1, float x2, float y2,
                  uint16_t c) {
  // Make it counter-clockwise in screen space so edge distances are + inside.
  float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
  if (area < 0.f) { float tx = x1, ty = y1; x1 = x2; y1 = y2; x2 = tx; y2 = ty; }
  float ex[3] = {x0, x1, x2}, ey[3] = {y0, y1, y2};
  float nx[3], ny[3], nc[3];
  for (int i = 0; i < 3; i++) {
    int j = (i + 1) % 3;
    float dx = ex[j] - ex[i], dy = ey[j] - ey[i];
    float l = sqrtf(dx * dx + dy * dy);
    if (l < 1e-6f) return;
    nx[i] = -dy / l;
    ny[i] = dx / l;
    nc[i] = -(nx[i] * ex[i] + ny[i] * ey[i]);
  }
  int bx0 = (int)floorf(fminf(x0, fminf(x1, x2))), bx1 = (int)ceilf(fmaxf(x0, fmaxf(x1, x2)));
  int by0 = (int)floorf(fminf(y0, fminf(y1, y2))), by1 = (int)ceilf(fmaxf(y0, fmaxf(y1, y2)));
  for (int py = imax(by0, s.cy0); py < imin(by1 + 1, s.cy1); py++) {
    for (int px = imax(bx0, s.cx0); px < imin(bx1 + 1, s.cx1); px++) {
      float fx = px + 0.5f, fy = py + 0.5f;
      float m = 1e9f;
      for (int i = 0; i < 3; i++) {
        float d = nx[i] * fx + ny[i] * fy + nc[i];
        if (d < m) m = d;
      }
      s.plot(px, py, c, (uint8_t)(clamp01(m + 0.5f) * 255.f + 0.5f));
    }
  }
}

void arc(Surface &s, float cx, float cy, float r, float t, float a0, float a1, uint16_t c) {
  if (a1 < a0) { float tmp = a0; a0 = a1; a1 = tmp; }
  if (a1 - a0 >= 360.f) { strokeCircle(s, cx, cy, r + t * 0.5f, t, c); return; }
  const float D2R = 0.017453293f;
  int x0 = (int)floorf(cx - r - t), x1 = (int)ceilf(cx + r + t);
  int y0 = (int)floorf(cy - r - t), y1 = (int)ceilf(cy + r + t);
  for (int py = imax(y0, s.cy0); py < imin(y1, s.cy1); py++) {
    float dy = py + 0.5f - cy;
    for (int px = imax(x0, s.cx0); px < imin(x1, s.cx1); px++) {
      float dx = px + 0.5f - cx;
      float d = sqrtf(dx * dx + dy * dy);
      float band = fabsf(d - r) - t * 0.5f;
      if (band > 0.5f) continue;
      // Angle clockwise from 12 o'clock, 0..360.
      float ang = atan2f(dx, -dy) / D2R;
      if (ang < 0.f) ang += 360.f;
      bool in = false;
      for (float off = 0.f; off <= 360.f; off += 360.f)
        if (ang + off >= a0 && ang + off <= a1) in = true;
      if (in) s.plot(px, py, c, cov(band));
    }
  }
  // Round caps.
  for (int k = 0; k < 2; k++) {
    float a = (k ? a1 : a0) * D2R;
    fillCircle(s, cx + sinf(a) * r, cy - cosf(a) * r, t * 0.5f, c);
  }
}

// ---- 4-bit fonts -------------------------------------------------------------
static int glyphIndex(const AaFont &f, char ch) {
  for (int i = 0; i < f.count; i++)
    if (f.chars[i] == ch) return i;
  return -1;
}

int aaWidth(const AaFont &f, const char *str, int tracking) {
  int w = 0, n = 0;
  for (const char *p = str; *p; p++) {
    int gi = glyphIndex(f, *p);
    w += (gi >= 0) ? f.glyphs[gi].adv : f.height / 3;
    n++;
  }
  return n ? w + tracking * (n - 1) : 0;
}

int aaText(Surface &s, const AaFont &f, int x, int y, const char *str, uint16_t c, int tracking) {
  for (const char *p = str; *p; p++) {
    int gi = glyphIndex(f, *p);
    if (gi < 0) { x += f.height / 3 + tracking; continue; }
    const AaGlyph &g = f.glyphs[gi];
    const uint8_t *d = f.data + g.offset;
    int stride = (g.w + 1) / 2;
    int gx = x + g.xOff, gy = y + g.yOff;
    for (int row = 0; row < g.h; row++) {
      int py = gy + row;
      if (py < s.cy0 || py >= s.cy1) continue;
      const uint8_t *rp = d + row * stride;
      for (int col = 0; col < g.w; col++) {
        uint8_t nib = (col & 1) ? (rp[col >> 1] & 0x0F) : (rp[col >> 1] >> 4);
        if (nib) s.plot(gx + col, py, c, (uint8_t)(nib * 17));
      }
    }
    x += g.adv + tracking;
  }
  return x;
}

void aaTextCentered(Surface &s, const AaFont &f, int cx, int y, const char *str, uint16_t c,
                    int tracking) {
  aaText(s, f, cx - aaWidth(f, str, tracking) / 2, y, str, c, tracking);
}

// ---- 1-bit GFXfonts ----------------------------------------------------------
int gfxWidth(const GFXfont *f, const char *str) {
  int w = 0;
  for (const char *p = str; *p; p++) {
    uint8_t ch = (uint8_t)*p;
    if (ch < f->first || ch > f->last) continue;
    w += f->glyph[ch - f->first].xAdvance;
  }
  return w;
}

int gfxCapHeight(const GFXfont *f) {
  uint8_t ch = 'H';
  if (ch < f->first || ch > f->last) return f->yAdvance / 2;
  return -f->glyph[ch - f->first].yOffset;
}

int gfxText(Surface &s, const GFXfont *f, int x, int baseline, const char *str, uint16_t c) {
  for (const char *p = str; *p; p++) {
    uint8_t ch = (uint8_t)*p;
    if (ch < f->first || ch > f->last) continue;
    const GFXglyph &g = f->glyph[ch - f->first];
    const uint8_t *bm = f->bitmap + g.bitmapOffset;
    uint32_t bit = 0;
    for (int yy = 0; yy < g.height; yy++) {
      int py = baseline + g.yOffset + yy;
      for (int xx = 0; xx < g.width; xx++, bit++) {
        if (bm[bit >> 3] & (0x80 >> (bit & 7))) {
          int px = x + g.xOffset + xx;
          if (px >= s.cx0 && px < s.cx1 && py >= s.cy0 && py < s.cy1) s.px[py * s.w + px] = c;
        }
      }
    }
    x += g.xAdvance;
  }
  return x;
}

void gfxTextCentered(Surface &s, const GFXfont *f, int cx, int baseline, const char *str,
                     uint16_t c) {
  gfxText(s, f, cx - gfxWidth(f, str) / 2, baseline, str, c);
}

void gfxTextRight(Surface &s, const GFXfont *f, int right, int baseline, const char *str,
                  uint16_t c) {
  gfxText(s, f, right - gfxWidth(f, str), baseline, str, c);
}

}  // namespace rgfx
