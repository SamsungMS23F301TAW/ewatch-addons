#include "mc_gfx.h"
#include <math.h>
#include <string.h>

namespace mc {

static const float kPi = 3.14159265358979f;

static inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
static inline float fminf2(float a, float b) { return a < b ? a : b; }
static inline float fmaxf2(float a, float b) { return a > b ? a : b; }

uint32_t rgb888(uint16_t c) {
  uint32_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
  r = (r << 3) | (r >> 2);
  g = (g << 2) | (g >> 4);
  b = (b << 3) | (b >> 2);
  return (r << 16) | (g << 8) | b;
}

uint32_t mixRgb(uint32_t a, uint32_t b, float t) {
  t = clamp01(t);
  int ar = (int)(a >> 16) & 0xFF, ag = (int)(a >> 8) & 0xFF, ab = (int)a & 0xFF;
  int br = (int)(b >> 16) & 0xFF, bg = (int)(b >> 8) & 0xFF, bb = (int)b & 0xFF;
  int r = ar + (int)lrintf((float)(br - ar) * t);
  int g = ag + (int)lrintf((float)(bg - ag) * t);
  int bl = ab + (int)lrintf((float)(bb - ab) * t);
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

uint32_t scaleRgb(uint32_t c, float k) {
  if (k < 0) k = 0;
  int r = (int)((float)((c >> 16) & 0xFF) * k), g = (int)((float)((c >> 8) & 0xFF) * k),
      b = (int)((float)(c & 0xFF) * k);
  if (r > 255) r = 255;
  if (g > 255) g = 255;
  if (b > 255) b = 255;
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

uint32_t contrastOn(uint32_t bg) {
  float l = 0.299f * (float)((bg >> 16) & 0xFF) + 0.587f * (float)((bg >> 8) & 0xFF) +
            0.114f * (float)(bg & 0xFF);
  return l < 140.0f ? 0xFFFFFF : 0x000000;
}

void clear(Canvas &c, uint32_t rgb) {
  uint16_t v = rgb565(rgb);
  size_t n = (size_t)c.w * (size_t)c.h;
  for (size_t i = 0; i < n; i++) c.px[i] = v;
}

void fillRect(Canvas &c, int x, int y, int w, int h, uint32_t rgb) {
  uint16_t v = rgb565(rgb);
  int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
  int x1 = x + w > c.w ? c.w : x + w, y1 = y + h > c.h ? c.h : y + h;
  for (int yy = y0; yy < y1; yy++)
    for (int xx = x0; xx < x1; xx++) c.px[yy * c.w + xx] = v;
}

void blend(Canvas &c, int x, int y, uint32_t rgb, float a) {
  if (x < 0 || y < 0 || x >= c.w || y >= c.h || a <= 0.0f) return;
  uint16_t *p = &c.px[y * c.w + x];
  if (a >= 0.999f) { *p = rgb565(rgb); return; }
  uint32_t d = rgb888(*p);
  int ia = (int)(a * 256.0f);
  int dr = (int)(d >> 16) & 0xFF, dg = (int)(d >> 8) & 0xFF, db = (int)d & 0xFF;
  int sr = (int)(rgb >> 16) & 0xFF, sg = (int)(rgb >> 8) & 0xFF, sb = (int)rgb & 0xFF;
  int r = dr + (((sr - dr) * ia) >> 8);
  int g = dg + (((sg - dg) * ia) >> 8);
  int b = db + (((sb - db) * ia) >> 8);
  *p = rgb565(((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b);
}

// ---------------------------------------------------------------------------
void fillCircle(Canvas &c, float cx, float cy, float r, uint32_t rgb, float alpha) {
  int x0 = (int)floorf(cx - r - 1), x1 = (int)ceilf(cx + r + 1);
  int y0 = (int)floorf(cy - r - 1), y1 = (int)ceilf(cy + r + 1);
  for (int y = y0; y <= y1; y++) {
    float dy = (float)y + 0.5f - cy;
    for (int x = x0; x <= x1; x++) {
      float dx = (float)x + 0.5f - cx;
      float d = sqrtf(dx * dx + dy * dy) - r;
      float cov = clamp01(0.5f - d);
      if (cov > 0) blend(c, x, y, rgb, cov * alpha);
    }
  }
}

static float sdRoundBox(float px, float py, float hw, float hh, float r) {
  float qx = fabsf(px) - (hw - r), qy = fabsf(py) - (hh - r);
  float ox = fmaxf2(qx, 0), oy = fmaxf2(qy, 0);
  return sqrtf(ox * ox + oy * oy) + fminf2(fmaxf2(qx, qy), 0) - r;
}

void fillRoundRect(Canvas &c, float x, float y, float w, float h, float r, uint32_t rgb,
                   float alpha) {
  float cx = x + w / 2, cy = y + h / 2;
  if (r > w / 2) r = w / 2;
  if (r > h / 2) r = h / 2;
  int x0 = (int)floorf(x) - 1, x1 = (int)ceilf(x + w) + 1;
  int y0 = (int)floorf(y) - 1, y1 = (int)ceilf(y + h) + 1;
  for (int yy = y0; yy <= y1; yy++) {
    for (int xx = x0; xx <= x1; xx++) {
      float d = sdRoundBox((float)xx + 0.5f - cx, (float)yy + 0.5f - cy, w / 2, h / 2, r);
      float cov = clamp01(0.5f - d);
      if (cov > 0) blend(c, xx, yy, rgb, cov * alpha);
    }
  }
}

void strokeRoundRect(Canvas &c, float x, float y, float w, float h, float r, float width,
                     uint32_t rgb, float alpha) {
  float cx = x + w / 2, cy = y + h / 2;
  int x0 = (int)floorf(x - width) - 1, x1 = (int)ceilf(x + w + width) + 1;
  int y0 = (int)floorf(y - width) - 1, y1 = (int)ceilf(y + h + width) + 1;
  for (int yy = y0; yy <= y1; yy++) {
    for (int xx = x0; xx <= x1; xx++) {
      float d = fabsf(sdRoundBox((float)xx + 0.5f - cx, (float)yy + 0.5f - cy, w / 2, h / 2, r)) -
                width / 2;
      float cov = clamp01(0.5f - d);
      if (cov > 0) blend(c, xx, yy, rgb, cov * alpha);
    }
  }
}

static float distSeg(float px, float py, float x0, float y0, float x1, float y1) {
  float vx = x1 - x0, vy = y1 - y0;
  float wx = px - x0, wy = py - y0;
  float L = vx * vx + vy * vy;
  float t = L > 0 ? (wx * vx + wy * vy) / L : 0;
  t = clamp01(t);
  float dx = wx - vx * t, dy = wy - vy * t;
  return sqrtf(dx * dx + dy * dy);
}

void fillCapsule(Canvas &c, float x0, float y0, float x1, float y1, float r, uint32_t rgb,
                 float alpha) {
  int bx0 = (int)floorf(fminf2(x0, x1) - r) - 1, bx1 = (int)ceilf(fmaxf2(x0, x1) + r) + 1;
  int by0 = (int)floorf(fminf2(y0, y1) - r) - 1, by1 = (int)ceilf(fmaxf2(y0, y1) + r) + 1;
  for (int y = by0; y <= by1; y++)
    for (int x = bx0; x <= bx1; x++) {
      float d = distSeg((float)x + 0.5f, (float)y + 0.5f, x0, y0, x1, y1) - r;
      float cov = clamp01(0.5f - d);
      if (cov > 0) blend(c, x, y, rgb, cov * alpha);
    }
}

// ---------------------------------------------------------------------------
// Ring
// ---------------------------------------------------------------------------
Ring makeRing(int screenW, int screenH, float inset, float thick, float cornerRadius) {
  Ring r;
  r.cx = (float)screenW / 2;
  r.cy = (float)screenH / 2;
  r.thick = thick;
  r.a = (float)screenW / 2 - inset - thick / 2;
  r.b = (float)screenH / 2 - inset - thick / 2;
  r.rc = cornerRadius;
  if (r.rc > r.a) r.rc = r.a;
  if (r.rc > r.b) r.rc = r.b;
  r.len = 4 * (r.a - r.rc) + 4 * (r.b - r.rc) + 2 * kPi * r.rc;
  return r;
}

struct RingSegs {
  float L0, Lq, L1, L2;
  float t1, t2, t3, t4, t5, t6, t7, t8;   // cumulative starts
};
static RingSegs segs(const Ring &r) {
  RingSegs s;
  s.L0 = r.a - r.rc;
  s.Lq = kPi * r.rc / 2;
  s.L1 = 2 * (r.b - r.rc);
  s.L2 = 2 * (r.a - r.rc);
  s.t1 = s.L0;               // TR arc
  s.t2 = s.t1 + s.Lq;        // right side
  s.t3 = s.t2 + s.L1;        // BR arc
  s.t4 = s.t3 + s.Lq;        // bottom
  s.t5 = s.t4 + s.L2;        // BL arc
  s.t6 = s.t5 + s.Lq;        // left side
  s.t7 = s.t6 + s.L1;        // TL arc
  s.t8 = s.t7 + s.Lq;        // top-left straight
  return s;
}

void ringPoint(const Ring &r, float s, float &x, float &y) {
  RingSegs g = segs(r);
  float t = s - floorf(s);
  t *= r.len;
  float A = r.a - r.rc, B = r.b - r.rc;
  float qx, qy;
  if (t < g.t1)      { qx = t;                    qy = -r.b; }
  else if (t < g.t2) { float th = -kPi / 2 + (t - g.t1) / r.rc; qx = A + r.rc * cosf(th); qy = -B + r.rc * sinf(th); }
  else if (t < g.t3) { qx = r.a;                  qy = -B + (t - g.t2); }
  else if (t < g.t4) { float th = (t - g.t3) / r.rc; qx = A + r.rc * cosf(th); qy = B + r.rc * sinf(th); }
  else if (t < g.t5) { qx = A - (t - g.t4);       qy = r.b; }
  else if (t < g.t6) { float th = kPi / 2 + (t - g.t5) / r.rc; qx = -A + r.rc * cosf(th); qy = B + r.rc * sinf(th); }
  else if (t < g.t7) { qx = -r.a;                 qy = B - (t - g.t6); }
  else if (t < g.t8) { float th = kPi + (t - g.t7) / r.rc; qx = -A + r.rc * cosf(th); qy = -B + r.rc * sinf(th); }
  else               { qx = -A + (t - g.t8);      qy = -r.b; }
  x = r.cx + qx;
  y = r.cy + qy;
}

// Signed distance to the centreline (positive outside) and arc parameter t.
static void ringLocate(const Ring &r, const RingSegs &g, float qx, float qy, float &dist, float &t) {
  float A = r.a - r.rc, B = r.b - r.rc;
  float dx = fabsf(qx) - A, dy = fabsf(qy) - B;
  if (dx > 0 && dy > 0) {
    float ccx = qx > 0 ? A : -A, ccy = qy > 0 ? B : -B;
    float vx = qx - ccx, vy = qy - ccy;
    float d = sqrtf(vx * vx + vy * vy);
    dist = d - r.rc;
    float th = atan2f(vy, vx);
    if (qx > 0 && qy < 0)      t = g.t1 + r.rc * (th + kPi / 2);
    else if (qx > 0)           t = g.t3 + r.rc * th;
    else if (qy > 0)           t = g.t5 + r.rc * (th - kPi / 2);
    else                       t = g.t7 + r.rc * (th + kPi);
    return;
  }
  bool horiz;
  if (dx <= 0 && dy <= 0) horiz = (r.b - fabsf(qy)) < (r.a - fabsf(qx));
  else horiz = dx <= 0;
  if (horiz) {
    dist = fabsf(qy) - r.b;
    if (qy < 0) t = qx >= 0 ? qx : r.len + qx;
    else        t = g.t4 + (A - qx);
  } else {
    dist = fabsf(qx) - r.a;
    if (qx > 0) t = g.t2 + (qy + B);
    else        t = g.t6 + (B - qy);
  }
}

void drawRing(Canvas &c, const Ring &r, float s0, float s1, uint32_t fillRgb, uint32_t trackRgb,
              int ticks, uint32_t tickRgb) {
  RingSegs g = segs(r);
  float half = r.thick / 2;
  bool full = (s1 - s0) >= 0.9999f;
  bool none = (s1 - s0) <= 0.0001f;
  float t0 = s0 * r.len, t1 = s1 * r.len;
  float c0x, c0y, c1x, c1y;
  ringPoint(r, s0, c0x, c0y);
  ringPoint(r, s1, c1x, c1y);

  // Inner skip span per row: pixels well inside the inner edge need no work.
  float ia = r.a - half - 1.5f, ib = r.b - half - 1.5f, irc = r.rc - half - 1.5f;
  if (irc < 0) irc = 0;
  for (int y = 0; y < c.h; y++) {
    float qy = (float)y + 0.5f - r.cy;
    float aqy = fabsf(qy);
    float hw = -1;
    if (aqy <= ib - irc) hw = ia;
    else if (aqy < ib) { float k = aqy - (ib - irc); hw = (ia - irc) + sqrtf(fmaxf2(irc * irc - k * k, 0)); }
    if (aqy > r.b + half + 1.5f) continue;
    for (int x = 0; x < c.w; x++) {
      float qx = (float)x + 0.5f - r.cx;
      if (hw > 0 && fabsf(qx) < hw) { x = (int)(r.cx + hw - 0.5f); continue; }
      if (fabsf(qx) > r.a + half + 1.5f) continue;
      float dist, t;
      ringLocate(r, g, qx, qy, dist, t);
      float ad = fabsf(dist);
      if (ad > half + 1.0f) continue;
      float trackCov = clamp01(half - ad + 0.5f);
      float cov = 0;
      if (!none) {
        if (full || (t >= t0 && t <= t1)) {
          cov = trackCov;
        } else {
          float px = (float)x + 0.5f, py = (float)y + 0.5f;
          float d0 = sqrtf((px - c0x) * (px - c0x) + (py - c0y) * (py - c0y));
          float d1 = sqrtf((px - c1x) * (px - c1x) + (py - c1y) * (py - c1y));
          cov = clamp01(half - fminf2(d0, d1) + 0.5f);
        }
      }
      // Composite: track under fill, both against what is already there.
      if (trackCov > 0 && cov < 0.999f) blend(c, x, y, trackRgb, trackCov);
      if (cov > 0) blend(c, x, y, fillRgb, cov);
    }
  }
  if (ticks > 0) {
    for (int k = 0; k < ticks; k++) {
      float s = (float)k / (float)ticks;
      // s = 0 and s = 1 are the same point (12 o'clock).
      bool covered = full || (s >= s0 - 0.004f && s <= s1 + 0.004f) ||
                     (k == 0 && s1 >= 0.996f);
      if (covered && !none) continue;
      float x, y;
      ringPoint(r, s, x, y);
      fillCircle(c, x, y, 1.4f, tickRgb);
    }
  }
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------
static const GFXglyph *glyphFor(const GFXfont *f, uint8_t ch) {
  if (ch < f->first || ch > f->last) return nullptr;
  return &f->glyph[ch - f->first];
}

float textWidth(const GFXfont *f, const char *s, float scale, float tracking) {
  float w = 0;
  int n = 0;
  for (const char *p = s; *p; p++) {
    const GFXglyph *g = glyphFor(f, (uint8_t)*p);
    if (!g) continue;
    w += (float)g->xAdvance * scale;
    n++;
  }
  if (n > 1) w += tracking * (float)(n - 1);
  return w;
}

static void drawGlyph(Canvas &c, const GFXfont *f, const GFXglyph *g, float x, float y,
                      float s, uint32_t rgb, float alpha) {
  if (!g->width || !g->height) return;
  const uint8_t *bm = f->bitmap + g->bitmapOffset;
  float gx = x + (float)g->xOffset * s, gy = y + (float)g->yOffset * s;
  int ox0 = (int)floorf(gx), oy0 = (int)floorf(gy);
  int ox1 = (int)ceilf(gx + (float)g->width * s), oy1 = (int)ceilf(gy + (float)g->height * s);
  float inv = 1.0f / s;
  int gw = g->width, gh = g->height;
  for (int oy = oy0; oy < oy1; oy++) {
    if (oy < 0 || oy >= c.h) continue;
    float sy0 = ((float)oy - gy) * inv, sy1 = ((float)oy + 1 - gy) * inv;
    int jy0 = (int)floorf(sy0), jy1 = (int)ceilf(sy1);
    if (jy0 < 0) jy0 = 0;
    if (jy1 > gh) jy1 = gh;
    for (int ox = ox0; ox < ox1; ox++) {
      if (ox < 0 || ox >= c.w) continue;
      float sx0 = ((float)ox - gx) * inv, sx1 = ((float)ox + 1 - gx) * inv;
      int ix0 = (int)floorf(sx0), ix1 = (int)ceilf(sx1);
      if (ix0 < 0) ix0 = 0;
      if (ix1 > gw) ix1 = gw;
      float acc = 0;
      for (int j = jy0; j < jy1; j++) {
        float wy = fminf2(sy1, (float)j + 1) - fmaxf2(sy0, (float)j);
        if (wy <= 0) continue;
        int row = j * gw;
        for (int i = ix0; i < ix1; i++) {
          int bit = row + i;
          if (bm[bit >> 3] & (0x80 >> (bit & 7))) {
            float wx = fminf2(sx1, (float)i + 1) - fmaxf2(sx0, (float)i);
            if (wx > 0) acc += wx * wy;
          }
        }
      }
      float cov = acc * s * s;
      if (cov > 0.004f) blend(c, ox, oy, rgb, clamp01(cov) * alpha);
    }
  }
}

void drawText(Canvas &c, const GFXfont *f, const char *s, float x, float y, float scale,
              uint32_t rgb, float tracking, float alpha) {
  for (const char *p = s; *p; p++) {
    const GFXglyph *g = glyphFor(f, (uint8_t)*p);
    if (!g) continue;
    drawGlyph(c, f, g, x, y, scale, rgb, alpha);
    x += (float)g->xAdvance * scale + tracking;
  }
}

void drawTextAligned(Canvas &c, const GFXfont *f, const char *s, float x, float y, float scale,
                     uint32_t rgb, Align a, float tracking) {
  float w = textWidth(f, s, scale, tracking);
  if (a == Align::Center) x -= w / 2;
  else if (a == Align::Right) x -= w;
  drawText(c, f, s, x, y, scale, rgb, tracking);
}

int fitText(const GFXfont *f, const char *s, float scale, float maxW, bool atWordBoundary) {
  float w = 0;
  int lastSpace = -1, i = 0;
  for (; s[i]; i++) {
    const GFXglyph *g = glyphFor(f, (uint8_t)s[i]);
    float adv = g ? (float)g->xAdvance * scale : 0;
    if (s[i] == ' ') lastSpace = i;
    if (w + adv > maxW) break;
    w += adv;
  }
  if (!s[i]) return i;                        // everything fits
  if (atWordBoundary && lastSpace > 0) return lastSpace;
  return i;
}

void ellipsize(const GFXfont *f, const char *s, float scale, float maxW, char *out, size_t cap) {
  if (!cap) return;
  size_t n = strlen(s);
  if (textWidth(f, s, scale) <= maxW) {
    size_t k = n < cap - 1 ? n : cap - 1;
    memcpy(out, s, k);
    out[k] = '\0';
    return;
  }
  float dots = textWidth(f, "...", scale);
  int fit = fitText(f, s, scale, maxW - dots, false);
  while (fit > 0 && s[fit - 1] == ' ') fit--;
  size_t k = (size_t)fit;
  if (k + 4 > cap) k = cap > 4 ? cap - 4 : 0;
  memcpy(out, s, k);
  memcpy(out + k, "...", 4);
}

// ---------------------------------------------------------------------------
// Clock digits: an original rounded monoline design built from line and arc
// strokes in a unit box (height 1, ink width kCellW).
// ---------------------------------------------------------------------------
namespace {
struct Prim {
  uint8_t type;        // 0 = line, 1 = arc (degrees, clockwise on screen), 2 = dot
  float a, b, c, d, e;
};
const float kCellW = 0.62f, kGap = 0.14f, kColonW = 0.30f;
#define L(x0, y0, x1, y1) {0, x0, y0, x1, y1, 0}
#define A(cx, cy, r, a0, a1) {1, cx, cy, r, a0, a1}
#define D(cx, cy, k) {2, cx, cy, k, 0, 0}
const Prim kD0[] = {A(0.31f, 0.31f, 0.24f, 180, 360), A(0.31f, 0.69f, 0.24f, 0, 180),
                    L(0.07f, 0.31f, 0.07f, 0.69f), L(0.55f, 0.31f, 0.55f, 0.69f)};
const Prim kD1[] = {L(0.37f, 0.07f, 0.37f, 0.93f), L(0.37f, 0.07f, 0.15f, 0.25f)};
const Prim kD2[] = {A(0.31f, 0.30f, 0.235f, 200, 401), L(0.4874f, 0.4542f, 0.07f, 0.93f),
                    L(0.07f, 0.93f, 0.56f, 0.93f)};
const Prim kD3[] = {A(0.30f, 0.28f, 0.21f, 205, 450), A(0.30f, 0.71f, 0.22f, 270, 515),
                    L(0.19f, 0.49f, 0.30f, 0.49f)};
const Prim kD4[] = {L(0.42f, 0.07f, 0.07f, 0.66f), L(0.07f, 0.66f, 0.57f, 0.66f),
                    L(0.42f, 0.07f, 0.42f, 0.93f)};
const Prim kD5[] = {L(0.54f, 0.07f, 0.12f, 0.07f), L(0.12f, 0.07f, 0.10f, 0.46f),
                    A(0.30f, 0.665f, 0.255f, 228, 505)};
const Prim kD6[] = {A(0.31f, 0.68f, 0.24f, 0, 360), L(0.0925f, 0.5786f, 0.36f, 0.07f)};
const Prim kD7[] = {L(0.07f, 0.07f, 0.55f, 0.07f), L(0.55f, 0.07f, 0.20f, 0.93f)};
const Prim kD8[] = {A(0.31f, 0.275f, 0.205f, 0, 360), A(0.31f, 0.70f, 0.23f, 0, 360)};
const Prim kD9[] = {A(0.31f, 0.32f, 0.24f, 0, 360), L(0.5275f, 0.4214f, 0.26f, 0.93f)};
const Prim kColon[] = {D(0.15f, 0.34f, 1.15f), D(0.15f, 0.76f, 1.15f)};
#undef L
#undef A
#undef D
struct Glyph { const Prim *p; int n; float w; };
Glyph digitGlyph(char ch) {
  switch (ch) {
    case '0': return {kD0, 4, kCellW};
    case '1': return {kD1, 2, kCellW};
    case '2': return {kD2, 3, kCellW};
    case '3': return {kD3, 3, kCellW};
    case '4': return {kD4, 3, kCellW};
    case '5': return {kD5, 3, kCellW};
    case '6': return {kD6, 2, kCellW};
    case '7': return {kD7, 2, kCellW};
    case '8': return {kD8, 2, kCellW};
    case '9': return {kD9, 2, kCellW};
    case ':': return {kColon, 2, kColonW};
    default:  return {nullptr, 0, kCellW * 0.5f};
  }
}

// Distance (unit space) from p to one primitive; dots return a negative
// "radius bonus" via r.
float primDist(const Prim &pr, float px, float py, float halfW, float &extra) {
  extra = 0;
  if (pr.type == 0) return distSeg(px, py, pr.a, pr.b, pr.c, pr.d);
  if (pr.type == 2) {
    extra = halfW * (pr.c - 1.0f);
    float dx = px - pr.a, dy = py - pr.b;
    return sqrtf(dx * dx + dy * dy);
  }
  // arc
  float vx = px - pr.a, vy = py - pr.b;
  float d = sqrtf(vx * vx + vy * vy);
  float span = pr.e - pr.d;
  if (span >= 359.9f) return fabsf(d - pr.c);
  float a0 = pr.d * kPi / 180.0f, a1 = pr.e * kPi / 180.0f;
  float u0x = cosf(a0), u0y = sinf(a0), u1x = cosf(a1), u1y = sinf(a1);
  bool in;
  float c0 = u0x * vy - u0y * vx;          // >= 0: v is clockwise of u0
  float c1 = vx * u1y - vy * u1x;          // >= 0: v is anticlockwise of u1
  if (span <= 180.0f) in = c0 >= 0 && c1 >= 0;
  else in = !(c0 < 0 && c1 < 0);
  if (in) return fabsf(d - pr.c);
  float e0x = pr.a + pr.c * u0x, e0y = pr.b + pr.c * u0y;
  float e1x = pr.a + pr.c * u1x, e1y = pr.b + pr.c * u1y;
  float d0 = sqrtf((px - e0x) * (px - e0x) + (py - e0y) * (py - e0y));
  float d1 = sqrtf((px - e1x) * (px - e1x) + (py - e1y) * (py - e1y));
  return fminf2(d0, d1);
}
}  // namespace

float digitsWidth(const char *s, float height) {
  float w = 0;
  int n = 0;
  for (const char *p = s; *p; p++) { w += digitGlyph(*p).w; n++; }
  if (n > 1) w += kGap * (float)(n - 1);
  return w * height;
}

void drawDigits(Canvas &c, const char *s, float x, float top, float height, float weight,
                uint32_t rgb) {
  float halfW = weight / 2;                       // unit space
  float pxPerUnit = height;
  for (const char *p = s; *p; p++) {
    Glyph g = digitGlyph(*p);
    if (g.p) {
      int x0 = (int)floorf(x) - 1, x1 = (int)ceilf(x + g.w * height) + 1;
      int y0 = (int)floorf(top) - 1, y1 = (int)ceilf(top + height) + 1;
      for (int yy = y0; yy <= y1; yy++) {
        if (yy < 0 || yy >= c.h) continue;
        float uy = ((float)yy + 0.5f - top) / pxPerUnit;
        for (int xx = x0; xx <= x1; xx++) {
          if (xx < 0 || xx >= c.w) continue;
          float ux = ((float)xx + 0.5f - x) / pxPerUnit;
          float best = 1e9f;
          for (int k = 0; k < g.n; k++) {
            float extra;
            float d = primDist(g.p[k], ux, uy, halfW, extra) - extra;
            if (d < best) best = d;
          }
          float cov = clamp01((halfW - best) * pxPerUnit + 0.5f);
          if (cov > 0) blend(c, xx, yy, rgb, cov);
        }
      }
    }
    x += (g.w + kGap) * height;
  }
}

}  // namespace mc
