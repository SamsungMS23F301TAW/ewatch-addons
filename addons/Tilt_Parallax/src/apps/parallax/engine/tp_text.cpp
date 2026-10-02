#include "tp_text.h"
#include "tp_color.h"
#include "tp_platform.h"
#include <math.h>
#include <string.h>
// The FreeFont headers mark their tables PROGMEM. On the ESP32 that macro is
// empty (const data already lives in mapped flash) and the engine doesn't
// pull in Arduino headers, so supply the same empty definition if needed.
#ifndef PROGMEM
#define PROGMEM
#endif
#include "gfxfont.h"
#include "FreeSansBold24pt7b.h"

namespace tp {

static const float kPi = 3.14159265f;

// ---------------------------------------------------------------------------
// Monoline digits as distance fields
// ---------------------------------------------------------------------------
struct Prim {
  uint8_t type;            // 0 segment, 1 arc, 2 disc
  float ax, ay, bx, by;    // segment ends / arc centre (ax,ay)
  float R, a0, a1;         // arc radius and angles (a1 > a0), disc radius
  // arc helpers: unit start/end directions, sweep, full circle flag
  float s0x, s0y, s1x, s1y, span;
  bool full;
};

static float distSeg(float px, float py, const Prim &p) {
  float vx = p.bx - p.ax, vy = p.by - p.ay;
  float wx = px - p.ax, wy = py - p.ay;
  float l2 = vx * vx + vy * vy;
  float t = l2 > 0 ? (wx * vx + wy * vy) / l2 : 0;
  t = t < 0 ? 0 : (t > 1 ? 1 : t);
  float dx = wx - vx * t, dy = wy - vy * t;
  return sqrtf(dx * dx + dy * dy);
}

// Distance to an arc. The angular range test uses cross products against
// the arc's start/end directions (precomputed in the Prim) instead of
// atan2, which is slow on the watch's FPU.
static float distArc(float px, float py, const Prim &p) {
  float vx = px - p.ax, vy = py - p.ay;
  float len = sqrtf(vx * vx + vy * vy);
  bool inside;
  if (p.full) {
    inside = true;
  } else {
    float c0 = p.s0x * vy - p.s0y * vx;          // >= 0: past the start, clockwise
    float c1 = p.s1x * vy - p.s1y * vx;          // <= 0: before the end
    inside = p.span <= kPi ? (c0 >= 0 && c1 <= 0) : (c0 >= 0 || c1 <= 0);
  }
  if (inside) return fabsf(len - p.R);
  float e0x = p.ax + p.R * p.s0x, e0y = p.ay + p.R * p.s0y;
  float e1x = p.ax + p.R * p.s1x, e1y = p.ay + p.R * p.s1y;
  float d0 = sqrtf((px - e0x) * (px - e0x) + (py - e0y) * (py - e0y));
  float d1 = sqrtf((px - e1x) * (px - e1x) + (py - e1y) * (py - e1y));
  return d0 < d1 ? d0 : d1;
}

static Prim seg(float ax, float ay, float bx, float by) {
  Prim p{}; p.type = 0; p.ax = ax; p.ay = ay; p.bx = bx; p.by = by; return p;
}
static Prim arc(float cx, float cy, float R, float a0, float a1) {
  Prim p{}; p.type = 1; p.ax = cx; p.ay = cy; p.R = R; p.a0 = a0; p.a1 = a1;
  p.span = a1 - a0;
  p.full = p.span >= 2 * kPi - 1e-4f;
  p.s0x = cosf(a0); p.s0y = sinf(a0);
  p.s1x = cosf(a1); p.s1y = sinf(a1);
  return p;
}
static Prim disc(float cx, float cy, float R) {
  Prim p{}; p.type = 2; p.ax = cx; p.ay = cy; p.R = R; return p;
}

// Builds the primitives for one glyph in a W x H box (y down). Angles: 0 =
// right, pi/2 = down, pi = left, 3pi/2 = up; arcs sweep from a0 to a1.
static int glyphPrims(char ch, float W, float H, float r, Prim *out) {
  const float L = r, Rt = W - r, T = r, B = H - r;
  const float cw = Rt - L, chh = B - T, cx = W * 0.5f;
  const float R2 = cw * 0.5f;
  int n = 0;
  switch (ch) {
    case '0':
      out[n++] = arc(cx, T + R2, R2, kPi, 2 * kPi);
      out[n++] = arc(cx, B - R2, R2, 0, kPi);
      out[n++] = seg(L, T + R2, L, B - R2);
      out[n++] = seg(Rt, T + R2, Rt, B - R2);
      break;
    case '1': {
      float x = cx + cw * 0.10f;
      out[n++] = seg(x, T, x, B);
      out[n++] = seg(x, T, x - cw * 0.42f, T + chh * 0.20f);
      break;
    }
    case '2': {
      float a1 = 2 * kPi + 0.62f;
      float ex = cx + R2 * cosf(a1), ey = T + R2 + R2 * sinf(a1);
      out[n++] = arc(cx, T + R2, R2, kPi - 0.18f, a1);
      out[n++] = seg(ex, ey, L, B);
      out[n++] = seg(L, B, Rt, B);
      break;
    }
    case '3': {
      float Rb = chh * 0.265f, Rtp = chh * 0.5f - Rb;
      if (Rb > R2) Rb = R2;
      out[n++] = arc(cx, T + Rtp, Rtp, kPi + 0.50f, 2 * kPi + kPi * 0.5f);
      out[n++] = arc(cx, B - Rb, Rb, kPi * 1.5f, 3 * kPi - 0.50f);
      break;
    }
    case '4': {
      float x = L + cw * 0.74f, y = T + chh * 0.67f;
      out[n++] = seg(x, T, x, B);
      out[n++] = seg(x, T, L, y);
      out[n++] = seg(L, y, Rt, y);
      break;
    }
    case '5': {
      float Rb = chh * 0.31f;
      if (Rb > R2) Rb = R2;
      float bcY = B - Rb;
      float a0 = kPi + 0.62f;
      float sx = cx + Rb * cosf(a0), sy = bcY + Rb * sinf(a0);
      out[n++] = seg(Rt - cw * 0.02f, T, L + cw * 0.08f, T);
      out[n++] = seg(L + cw * 0.08f, T, sx, sy);
      out[n++] = arc(cx, bcY, Rb, a0, 3 * kPi - 0.55f);
      break;
    }
    case '6': {
      float cy = B - R2;
      out[n++] = arc(cx, cy, R2, 0, 2 * kPi);
      out[n++] = seg(L, cy, L + cw * 0.70f, T);
      break;
    }
    case '7':
      out[n++] = seg(L, T, Rt, T);
      out[n++] = seg(Rt, T, L + cw * 0.30f, B);
      break;
    case '8': {
      float R1 = chh * 0.225f, Rb = chh * 0.5f - R1;
      if (Rb > R2) { Rb = R2; R1 = chh * 0.5f - Rb; }
      out[n++] = arc(cx, T + R1, R1, 0, 2 * kPi);
      out[n++] = arc(cx, B - Rb, Rb, 0, 2 * kPi);
      break;
    }
    case '9': {
      float cy = T + R2;
      out[n++] = arc(cx, cy, R2, 0, 2 * kPi);
      out[n++] = seg(Rt, cy, L + cw * 0.30f, B);
      break;
    }
    case ':': {
      float rd = r * 1.25f;
      out[n++] = disc(cx, H * 0.34f, rd);
      out[n++] = disc(cx, H * 0.71f, rd);
      break;
    }
    case '-': {
      out[n++] = seg(L + cw * 0.15f, H * 0.5f, Rt - cw * 0.15f, H * 0.5f);
      break;
    }
    default:
      break;
  }
  return n;
}

static float glyphAdvance(char ch, const ClockMetrics &m) {
  if (ch == ':') return m.colonW;
  if (ch == ' ') return m.digitW * 0.5f;
  return m.digitW;
}

float clockTextWidth(const char *text, const ClockMetrics &m) {
  float w = 0;
  int n = 0;
  for (const char *p = text; *p; ++p, ++n) w += glyphAdvance(*p, m);
  if (n > 1) w += m.gap * (n - 1);
  return w;
}

void drawClockText(Layer &L, float ox, float oy, const char *text, const ClockMetrics &m) {
  if (!L.alpha) return;
  const float r = m.stroke * 0.5f;
  float x = ox;
  for (const char *p = text; *p; ++p) {
    char ch = *p;
    float adv = glyphAdvance(ch, m);
    Prim prims[6];
    int np = glyphPrims(ch, adv, m.digitH, r, prims);
    if (np > 0) {
      int x0 = (int)floorf(x - 2), x1 = (int)ceilf(x + adv + 2);
      int y0 = (int)floorf(oy - 2), y1 = (int)ceilf(oy + m.digitH + 2);
      for (int py = y0; py < y1; py++) {
        if (py < 0 || py >= L.h) continue;
        for (int px = x0; px < x1; px++) {
          if (px < 0 || px >= L.w) continue;
          float gx = px + 0.5f - x, gy = py + 0.5f - oy;
          float d = 1e9f;
          for (int i = 0; i < np; i++) {
            float di;
            if (prims[i].type == 0) di = distSeg(gx, gy, prims[i]) - r;
            else if (prims[i].type == 1) di = distArc(gx, gy, prims[i]) - r;
            else di = sqrtf((gx - prims[i].ax) * (gx - prims[i].ax) +
                            (gy - prims[i].ay) * (gy - prims[i].ay)) - prims[i].R;
            if (di < d) d = di;
          }
          float cov = sat(0.5f - d);
          if (cov <= 0) continue;
          uint8_t a = (uint8_t)(cov * 255.0f + 0.5f);
          uint8_t &dst = L.alpha[(size_t)py * L.w + px];
          if (a > dst) dst = a;
        }
      }
    }
    x += adv + m.gap;
  }
}

// ---------------------------------------------------------------------------
// Small text from FreeSansBold 24pt, area-sampled down to the target size.
// ---------------------------------------------------------------------------
static const GFXfont &kSmallFont = FreeSansBold24pt7b;
static const float kFontCap = 34.0f;    // cap height of FreeSansBold24pt7b

static const GFXglyph *glyphFor(char c) {
  if ((uint8_t)c < kSmallFont.first || (uint8_t)c > kSmallFont.last) c = '?';
  return &kSmallFont.glyph[(uint8_t)c - kSmallFont.first];
}

static inline bool glyphBit(const GFXglyph *g, int gx, int gy) {
  if (gx < 0 || gy < 0 || gx >= g->width || gy >= g->height) return false;
  uint32_t bit = (uint32_t)gy * g->width + gx;
  uint8_t byte = kSmallFont.bitmap[g->bitmapOffset + (bit >> 3)];
  return (byte >> (7 - (bit & 7))) & 1;
}

int smallTextWidth(const char *text, float capPx) {
  float s = capPx / kFontCap;
  float w = 0;
  for (const char *p = text; *p; ++p) w += glyphFor(*p)->xAdvance;
  return (int)ceilf(w * s);
}

void drawSmallText(Layer &L, float ox, float baselineY, const char *text, float capPx) {
  if (!L.alpha) return;
  const float s = capPx / kFontCap;       // target px per font px
  const float inv = 1.0f / s;
  float penFont = 0;                      // pen x in font units
  for (const char *p = text; *p; ++p) {
    const GFXglyph *g = glyphFor(*p);
    // glyph box in target pixels
    float gx0 = ox + (penFont + g->xOffset) * s;
    float gy0 = baselineY + g->yOffset * s;
    float gw = g->width * s, gh = g->height * s;
    int x0 = (int)floorf(gx0), x1 = (int)ceilf(gx0 + gw);
    int y0 = (int)floorf(gy0), y1 = (int)ceilf(gy0 + gh);
    for (int py = y0; py < y1; py++) {
      if (py < 0 || py >= L.h) continue;
      for (int px = x0; px < x1; px++) {
        if (px < 0 || px >= L.w) continue;
        int hits = 0;
        for (int sy = 0; sy < 4; sy++) {
          float fy = ((py + (sy + 0.5f) * 0.25f) - gy0) * inv;
          for (int sx = 0; sx < 4; sx++) {
            float fx = ((px + (sx + 0.5f) * 0.25f) - gx0) * inv;
            if (glyphBit(g, (int)floorf(fx), (int)floorf(fy))) hits++;
          }
        }
        if (!hits) continue;
        uint8_t a = (uint8_t)(hits * 255 / 16);
        uint8_t &dst = L.alpha[(size_t)py * L.w + px];
        if (a > dst) dst = a;
      }
    }
    penFont += g->xAdvance;
  }
}

// Rounded-box signed distance.
static float sdRoundBox(float px, float py, float cx, float cy, float hw, float hh, float rr) {
  float qx = fabsf(px - cx) - hw + rr, qy = fabsf(py - cy) - hh + rr;
  float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
  float in = (qx > qy ? qx : qy);
  return sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - rr;
}

void drawBatteryGlyph(Layer &L, float ox, float oy, float w, float h, int pct) {
  if (!L.alpha) return;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  const float st = 1.3f;                       // outline stroke
  float cx = ox + w * 0.5f, cy = oy + h * 0.5f;
  float fillW = (w - 2 * st - 2.0f) * pct / 100.0f;
  for (int py = (int)oy - 1; py <= (int)(oy + h) + 1; py++) {
    if (py < 0 || py >= L.h) continue;
    for (int px = (int)ox - 1; px <= (int)(ox + w + 4); px++) {
      if (px < 0 || px >= L.w) continue;
      float fx = px + 0.5f, fy = py + 0.5f;
      float body = fabsf(sdRoundBox(fx, fy, cx, cy, w * 0.5f, h * 0.5f, 2.2f)) - st * 0.5f;
      float nub = sdRoundBox(fx, fy, ox + w + 1.4f, cy, 1.2f, h * 0.22f, 0.8f);
      float lvl = 1e9f;
      if (fillW > 0.5f) {
        float lx0 = ox + st + 1.0f;
        lvl = sdRoundBox(fx, fy, lx0 + fillW * 0.5f, cy, fillW * 0.5f,
                         h * 0.5f - st - 1.0f, 0.8f);
      }
      float d = body < nub ? body : nub;
      if (lvl < d) d = lvl;
      float cov = sat(0.5f - d);
      if (cov <= 0) continue;
      uint8_t a = (uint8_t)(cov * 255.0f + 0.5f);
      uint8_t &dst = L.alpha[(size_t)py * L.w + px];
      if (a > dst) dst = a;
    }
  }
}

// ---------------------------------------------------------------------------
// Shadow helper: dilate (max) then two box-blur passes in each direction.
// ---------------------------------------------------------------------------
void blurAlphaInto(const Layer &src, Layer &dst, int spread, int radius, float gain) {
  if (!src.alpha || !dst.alpha || src.w != dst.w || src.h != dst.h) return;
  const int W = src.w, H = src.h;
  uint16_t *tmp = (uint16_t *)allocBig(sizeof(uint16_t) * (size_t)W * H);
  uint16_t *tmp2 = (uint16_t *)allocBig(sizeof(uint16_t) * (size_t)W * H);
  if (!tmp || !tmp2) {
    if (tmp) freeMem(tmp);
    if (tmp2) freeMem(tmp2);
    memset(dst.alpha, 0, (size_t)W * H);
    return;
  }
  // dilate horizontally then vertically (separable max)
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      int m = 0;
      for (int k = -spread; k <= spread; k++) {
        int xx = x + k;
        if (xx >= 0 && xx < W && src.alpha[(size_t)y * W + xx] > m) m = src.alpha[(size_t)y * W + xx];
      }
      tmp[(size_t)y * W + x] = (uint16_t)m;
    }
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      int m = 0;
      for (int k = -spread; k <= spread; k++) {
        int yy = y + k;
        if (yy >= 0 && yy < H && tmp[(size_t)yy * W + x] > m) m = tmp[(size_t)yy * W + x];
      }
      tmp2[(size_t)y * W + x] = (uint16_t)m;
    }
  // box blur twice (approximates a Gaussian), horizontal + vertical each pass
  const int win = 2 * radius + 1;
  for (int pass = 0; pass < 2; pass++) {
    for (int y = 0; y < H; y++) {
      int acc = 0;
      for (int k = -radius; k <= radius; k++) {
        int xx = k < 0 ? 0 : (k >= W ? W - 1 : k);
        acc += tmp2[(size_t)y * W + xx];
      }
      for (int x = 0; x < W; x++) {
        tmp[(size_t)y * W + x] = (uint16_t)(acc / win);
        int xo = x - radius, xi = x + radius + 1;
        xo = xo < 0 ? 0 : xo;
        xi = xi >= W ? W - 1 : xi;
        acc += tmp2[(size_t)y * W + xi] - tmp2[(size_t)y * W + xo];
      }
    }
    for (int x = 0; x < W; x++) {
      int acc = 0;
      for (int k = -radius; k <= radius; k++) {
        int yy = k < 0 ? 0 : (k >= H ? H - 1 : k);
        acc += tmp[(size_t)yy * W + x];
      }
      for (int y = 0; y < H; y++) {
        tmp2[(size_t)y * W + x] = (uint16_t)(acc / win);
        int yo = y - radius, yi = y + radius + 1;
        yo = yo < 0 ? 0 : yo;
        yi = yi >= H ? H - 1 : yi;
        acc += tmp[(size_t)yi * W + x] - tmp[(size_t)yo * W + x];
      }
    }
  }
  for (size_t i = 0; i < (size_t)W * H; i++) {
    float v = tmp2[i] * gain;
    dst.alpha[i] = (uint8_t)(v > 255 ? 255 : v);
  }
  freeMem(tmp);
  freeMem(tmp2);
}

}  // namespace tp
