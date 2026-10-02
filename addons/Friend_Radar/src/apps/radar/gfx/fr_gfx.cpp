#include "fr_gfx.h"
#include <math.h>
#include <string.h>

namespace fr {

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline float fclamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
static inline uint8_t covAlpha(float cov, uint8_t a) {
  return (uint8_t)(fclamp01(cov) * (float)a + 0.5f);
}

void Canvas::clip(const Rect &r) {
  cx0 = (int16_t)imax(cx0, r.x);
  cy0 = (int16_t)imax(cy0, r.y);
  cx1 = (int16_t)imin(cx1, r.x + r.w);
  cy1 = (int16_t)imin(cy1, r.y + r.h);
  if (cx1 < cx0) cx1 = cx0;
  if (cy1 < cy0) cy1 = cy0;
}

// ---- colour -----------------------------------------------------------------
uint16_t addColor(uint16_t bg, uint16_t fg, uint8_t a) {
  int r = (bg >> 11) + (((fg >> 11) * a + 127) >> 8);
  int g = ((bg >> 5) & 63) + (((((fg >> 5) & 63)) * a + 127) >> 8);
  int b = (bg & 31) + (((fg & 31) * a + 127) >> 8);
  if (r > 31) r = 31;
  if (g > 63) g = 63;
  if (b > 31) b = 31;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

uint16_t scaleColor(uint16_t c, uint8_t k) { return blend(0, c, k); }

uint16_t ditherRgb(float r, float g, float b, int x, int y) {
  static const uint8_t kBayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
  float t = ((float)kBayer[((y & 3) << 2) | (x & 3)] + 0.5f) / 16.f;   // 0..1
  auto q = [t](float v, float levels) {
    float s = v * levels / 255.f;
    int i = (int)s;
    if (s - (float)i > t) ++i;
    if (i < 0) i = 0;
    if (i > (int)levels) i = (int)levels;
    return i;
  };
  int R = q(r, 31.f), G = q(g, 63.f), B = q(b, 31.f);
  return (uint16_t)((R << 11) | (G << 5) | B);
}

uint16_t hsv(uint16_t hue, uint8_t s, uint8_t v) {
  hue %= 360;
  uint8_t region = (uint8_t)(hue / 60);
  uint16_t rem = (uint16_t)((hue % 60) * 255 / 60);
  uint8_t p = (uint8_t)((v * (255 - s)) / 255);
  uint8_t q = (uint8_t)((v * (255 - (s * rem) / 255)) / 255);
  uint8_t t = (uint8_t)((v * (255 - (s * (255 - rem)) / 255)) / 255);
  switch (region) {
    case 0:  return rgb(v, t, p);
    case 1:  return rgb(q, v, p);
    case 2:  return rgb(p, v, t);
    case 3:  return rgb(p, q, v);
    case 4:  return rgb(t, p, v);
    default: return rgb(v, p, q);
  }
}

uint8_t luma(uint16_t c) {
  uint32_t r = ((c >> 11) & 31) * 255 / 31;
  uint32_t g = ((c >> 5) & 63) * 255 / 63;
  uint32_t b = (c & 31) * 255 / 31;
  return (uint8_t)((r * 3 + g * 6 + b) / 10);
}

// ---- fills ------------------------------------------------------------------
void clear(Canvas &c, uint16_t col) {
  if (!c.px) return;
  size_t n = (size_t)c.w * c.h;
  for (size_t i = 0; i < n; ++i) c.px[i] = col;
}

void fillRect(Canvas &c, int x, int y, int w, int h, uint16_t col) {
  int x0 = imax(x, c.cx0), y0 = imax(y, c.cy0);
  int x1 = imin(x + w, c.cx1), y1 = imin(y + h, c.cy1);
  for (int yy = y0; yy < y1; ++yy) {
    uint16_t *row = c.px + (size_t)yy * c.w;
    for (int xx = x0; xx < x1; ++xx) row[xx] = col;
  }
}

void blendRect(Canvas &c, int x, int y, int w, int h, uint16_t col, uint8_t a) {
  if (a == 0) return;
  if (a == 255) { fillRect(c, x, y, w, h, col); return; }
  int x0 = imax(x, c.cx0), y0 = imax(y, c.cy0);
  int x1 = imin(x + w, c.cx1), y1 = imin(y + h, c.cy1);
  for (int yy = y0; yy < y1; ++yy) {
    uint16_t *row = c.px + (size_t)yy * c.w;
    for (int xx = x0; xx < x1; ++xx) row[xx] = blend(row[xx], col, a);
  }
}

void pixel(Canvas &c, int x, int y, uint16_t col, uint8_t a) {
  if (x < c.cx0 || y < c.cy0 || x >= c.cx1 || y >= c.cy1 || !a) return;
  uint16_t &p = c.px[(size_t)y * c.w + x];
  p = (a == 255) ? col : blend(p, col, a);
}

static inline void put(Canvas &c, int x, int y, uint16_t col, uint8_t a) {
  // caller guarantees (x, y) inside the clip rect
  if (!a) return;
  uint16_t &p = c.px[(size_t)y * c.w + x];
  p = (a == 255) ? col : blend(p, col, a);
}

void vGradient(Canvas &c, int x, int y, int w, int h, uint16_t top, uint16_t bottom) {
  if (h <= 0) return;
  for (int i = 0; i < h; ++i) {
    uint8_t t = (uint8_t)(h > 1 ? (i * 255) / (h - 1) : 0);
    fillRect(c, x, y + i, w, 1, blend(top, bottom, t));
  }
}

void vGradientDither(Canvas &c, int x, int y, int w, int h, uint32_t top, uint32_t bottom) {
  if (h <= 0) return;
  float r0 = (float)((top >> 16) & 255), g0 = (float)((top >> 8) & 255), b0 = (float)(top & 255);
  float r1 = (float)((bottom >> 16) & 255), g1 = (float)((bottom >> 8) & 255), b1 = (float)(bottom & 255);
  int ya = imax(y, c.cy0), yb = imin(y + h, c.cy1);
  int xa = imax(x, c.cx0), xb = imin(x + w, c.cx1);
  for (int yy = ya; yy < yb; ++yy) {
    float t = h > 1 ? (float)(yy - y) / (float)(h - 1) : 0.f;
    float r = r0 + (r1 - r0) * t, g = g0 + (g1 - g0) * t, b = b0 + (b1 - b0) * t;
    uint16_t *row = c.px + (size_t)yy * c.w;
    for (int xx = xa; xx < xb; ++xx) row[xx] = ditherRgb(r, g, b, xx, yy);
  }
}

// ---- circles ----------------------------------------------------------------
void fillCircle(Canvas &c, float cx, float cy, float r, uint16_t col, uint8_t a) {
  if (r <= 0.f || !a) return;
  const float ro = r + 0.5f, ri = r - 0.5f;
  const float ro2 = ro * ro, ri2 = ri > 0.f ? ri * ri : 0.f;
  int y0 = imax(c.cy0, (int)floorf(cy - ro)), y1 = imin(c.cy1 - 1, (int)ceilf(cy + ro));
  for (int y = y0; y <= y1; ++y) {
    float dy = (float)y + 0.5f - cy, dy2 = dy * dy;
    if (dy2 >= ro2) continue;
    float xo = sqrtf(ro2 - dy2);
    int xa = imax(c.cx0, (int)floorf(cx - xo)), xb = imin(c.cx1 - 1, (int)ceilf(cx + xo));
    float xi = dy2 < ri2 ? sqrtf(ri2 - dy2) : -1.f;
    int xs0 = xi >= 0.f ? (int)ceilf(cx - xi - 0.5f) : 1 << 30;
    int xs1 = xi >= 0.f ? (int)floorf(cx + xi - 0.5f) : -(1 << 30);
    uint16_t *row = c.px + (size_t)y * c.w;
    for (int x = xa; x <= xb; ++x) {
      if (x >= xs0 && x <= xs1) {
        row[x] = (a == 255) ? col : blend(row[x], col, a);
        continue;
      }
      float dx = (float)x + 0.5f - cx;
      float d = sqrtf(dx * dx + dy2) - r;
      uint8_t al = covAlpha(0.5f - d, a);
      if (al) row[x] = (al == 255) ? col : blend(row[x], col, al);
    }
  }
}

void ring(Canvas &c, float cx, float cy, float r, float hw, uint16_t col, uint8_t a) {
  if (r <= 0.f || hw <= 0.f || !a) return;
  const float ro = r + hw + 0.5f, ri = r - hw - 0.5f;
  const float ro2 = ro * ro, ri2 = ri > 0.f ? ri * ri : 0.f;
  int y0 = imax(c.cy0, (int)floorf(cy - ro)), y1 = imin(c.cy1 - 1, (int)ceilf(cy + ro));
  for (int y = y0; y <= y1; ++y) {
    float dy = (float)y + 0.5f - cy, dy2 = dy * dy;
    if (dy2 >= ro2) continue;
    float xo = sqrtf(ro2 - dy2);
    float xi = (ri > 0.f && dy2 < ri2) ? sqrtf(ri2 - dy2) : 0.f;
    auto span = [&](int xa, int xb) {
      xa = imax(xa, c.cx0); xb = imin(xb, c.cx1 - 1);
      for (int x = xa; x <= xb; ++x) {
        float dx = (float)x + 0.5f - cx;
        float d = sqrtf(dx * dx + dy2);
        float cov = hw + 0.5f - fabsf(d - r);
        if (cov <= 0.f) continue;
        put(c, x, y, col, covAlpha(cov, a));
      }
    };
    int la = (int)floorf(cx - xo), rb = (int)ceilf(cx + xo);
    if (xi <= 0.f) {
      span(la, rb);                                  // row misses the hole
    } else {
      int lb = (int)ceilf(cx - xi), ra = (int)floorf(cx + xi);
      if (ra <= lb) ra = lb + 1;                     // never touch a pixel twice
      span(la, lb);
      span(ra, rb);
    }
  }
}

static inline float angleDeg(float dx, float dy) {   // 0 = up, clockwise
  float a = atan2f(dx, -dy) * 57.2957795f;
  return a < 0.f ? a + 360.f : a;
}

void arc(Canvas &c, float cx, float cy, float r, float hw, float a0, float a1,
         uint16_t col, uint8_t a) {
  if (a1 - a0 >= 359.9f) { ring(c, cx, cy, r, hw, col, a); return; }
  if (a1 <= a0 || !a) return;
  const float ro = r + hw + 0.5f, ri = r - hw - 0.5f;
  int y0 = imax(c.cy0, (int)floorf(cy - ro)), y1 = imin(c.cy1 - 1, (int)ceilf(cy + ro));
  int x0 = imax(c.cx0, (int)floorf(cx - ro)), x1 = imin(c.cx1 - 1, (int)ceilf(cx + ro));
  float s0 = fmodf(a0, 360.f); if (s0 < 0.f) s0 += 360.f;
  float span = a1 - a0;
  for (int y = y0; y <= y1; ++y) {
    float dy = (float)y + 0.5f - cy;
    for (int x = x0; x <= x1; ++x) {
      float dx = (float)x + 0.5f - cx;
      float d = sqrtf(dx * dx + dy * dy);
      if (d > ro || d < ri) continue;
      float cov = hw + 0.5f - fabsf(d - r);
      if (cov <= 0.f) continue;
      float ang = angleDeg(dx, dy) - s0;
      if (ang < 0.f) ang += 360.f;
      if (ang > span) continue;
      put(c, x, y, col, covAlpha(cov, a));
    }
  }
  // Round caps.
  float r0 = (a0) * 0.0174532925f, r1 = (a1) * 0.0174532925f;
  fillCircle(c, cx + r * sinf(r0), cy - r * cosf(r0), hw, col, a);
  fillCircle(c, cx + r * sinf(r1), cy - r * cosf(r1), hw, col, a);
}

// ---- lines ------------------------------------------------------------------
void line(Canvas &c, float x0, float y0, float x1, float y1, float hw, uint16_t col, uint8_t a) {
  if (!a) return;
  float vx = x1 - x0, vy = y1 - y0;
  float len2 = vx * vx + vy * vy;
  float len = sqrtf(len2);
  const float reach = hw + 0.75f;
  int bx0 = imax(c.cx0, (int)floorf(fminf(x0, x1) - reach));
  int bx1 = imin(c.cx1 - 1, (int)ceilf(fmaxf(x0, x1) + reach));
  int by0 = imax(c.cy0, (int)floorf(fminf(y0, y1) - reach));
  int by1 = imin(c.cy1 - 1, (int)ceilf(fmaxf(y0, y1) + reach));
  if (len < 1e-3f) { fillCircle(c, x0, y0, hw, col, a); return; }
  float ux = vx / len, uy = vy / len;
  float nx = -uy, ny = ux;                       // unit normal
  for (int y = by0; y <= by1; ++y) {
    float py = (float)y + 0.5f;
    int xa = bx0, xb = bx1;
    if (fabsf(nx) > 0.05f) {
      // x range where |n . (p - p0)| <= reach
      float base = ny * (py - y0);
      float e0 = (-reach - base) / nx + x0, e1 = (reach - base) / nx + x0;
      if (e0 > e1) { float t = e0; e0 = e1; e1 = t; }
      xa = imax(xa, (int)floorf(e0 - 0.5f));
      xb = imin(xb, (int)ceilf(e1 + 0.5f));
    }
    for (int x = xa; x <= xb; ++x) {
      float px = (float)x + 0.5f;
      float wx = px - x0, wy = py - y0;
      float t = (wx * vx + wy * vy) / len2;
      if (t < 0.f) t = 0.f; else if (t > 1.f) t = 1.f;
      float ex = wx - t * vx, ey = wy - t * vy;
      float d = sqrtf(ex * ex + ey * ey);
      float cov = hw + 0.5f - d;
      if (cov <= 0.f) continue;
      put(c, x, y, col, covAlpha(cov, a));
    }
  }
}

// ---- rounded rectangles -----------------------------------------------------
// Signed distance from pixel centre to a rounded box (negative inside).
static inline float rrSdf(float px, float py, float x, float y, float w, float h, float r) {
  float hx = w * 0.5f, hy = h * 0.5f;
  float qx = fabsf(px - (x + hx)) - (hx - r);
  float qy = fabsf(py - (y + hy)) - (hy - r);
  float ox = qx > 0.f ? qx : 0.f, oy = qy > 0.f ? qy : 0.f;
  float outside = sqrtf(ox * ox + oy * oy);
  float inside = fminf(fmaxf(qx, qy), 0.f);
  return outside + inside - r;
}

void fillRoundRect(Canvas &c, int x, int y, int w, int h, float r, uint16_t col, uint8_t a) {
  if (w <= 0 || h <= 0 || !a) return;
  float maxR = (float)imin(w, h) * 0.5f;
  if (r > maxR) r = maxR;
  if (r < 0.f) r = 0.f;
  int ri = (int)ceilf(r);
  int y0 = imax(c.cy0, y), y1 = imin(c.cy1, y + h);
  int x0 = imax(c.cx0, x), x1 = imin(c.cx1, x + w);
  for (int yy = y0; yy < y1; ++yy) {
    bool cornerRow = (yy < y + ri) || (yy >= y + h - ri);
    uint16_t *row = c.px + (size_t)yy * c.w;
    for (int xx = x0; xx < x1; ++xx) {
      bool cornerCol = (xx < x + ri) || (xx >= x + w - ri);
      if (cornerRow && cornerCol) {
        float d = rrSdf((float)xx + 0.5f, (float)yy + 0.5f, (float)x, (float)y, (float)w, (float)h, r);
        uint8_t al = covAlpha(0.5f - d, a);
        if (al) row[xx] = (al == 255) ? col : blend(row[xx], col, al);
      } else {
        row[xx] = (a == 255) ? col : blend(row[xx], col, a);
      }
    }
  }
}

void strokeRoundRect(Canvas &c, int x, int y, int w, int h, float r, float hw,
                     uint16_t col, uint8_t a) {
  if (w <= 0 || h <= 0 || !a) return;
  float maxR = (float)imin(w, h) * 0.5f;
  if (r > maxR) r = maxR;
  int band = (int)ceilf(2.f * hw + 1.f) + (int)ceilf(r);
  int bandH = (int)ceilf(2.f * hw + 1.f);
  int y0 = imax(c.cy0, y), y1 = imin(c.cy1, y + h);
  int x0 = imax(c.cx0, x), x1 = imin(c.cx1, x + w);
  for (int yy = y0; yy < y1; ++yy) {
    bool edgeRow = (yy < y + bandH) || (yy >= y + h - bandH);
    for (int xx = x0; xx < x1; ++xx) {
      if (!edgeRow && xx >= x + band && xx < x + w - band) { xx = x + w - band - 1; continue; }
      float d = rrSdf((float)xx + 0.5f, (float)yy + 0.5f, (float)x, (float)y, (float)w, (float)h, r);
      float cov = hw + 0.5f - fabsf(d + hw);
      if (cov <= 0.f) continue;
      put(c, xx, yy, col, covAlpha(cov, a));
    }
  }
}

void petal(Canvas &c, float cx, float cy, float ang, float len, float width,
           uint16_t col, uint8_t a) {
  if (len < 1.f || width <= 0.f || !a) return;
  const float ux = sinf(ang), uy = -cosf(ang);          // along the petal
  const float ex = cx + ux * len, ey = cy + uy * len;
  const float reach = width + 1.f;
  int x0 = imax(c.cx0, (int)floorf(fminf(cx, ex) - reach));
  int x1 = imin(c.cx1 - 1, (int)ceilf(fmaxf(cx, ex) + reach));
  int y0 = imax(c.cy0, (int)floorf(fminf(cy, ey) - reach));
  int y1 = imin(c.cy1 - 1, (int)ceilf(fmaxf(cy, ey) + reach));
  // Half-width profile sampled once per call (no powf/sinf per pixel: this
  // runs for a dozen petals per animation frame on the watch).
  float prof[65];
  for (int i = 0; i <= 64; ++i)
    prof[i] = width * sinf(3.14159265f * powf((float)i / 64.f, 0.8f));
  const float inv = 1.f / len;
  for (int y = y0; y <= y1; ++y) {
    float py = (float)y + 0.5f - cy;
    for (int x = x0; x <= x1; ++x) {
      float px = (float)x + 0.5f - cx;
      float u = (px * ux + py * uy) * inv;               // 0..1 along
      if (u <= 0.f || u >= 1.f) continue;
      float v = fabsf(px * uy - py * ux);                // distance from the axis
      if (v > width + 0.5f) continue;
      float fi = u * 64.f;
      int i = (int)fi;
      float w = prof[i] + (prof[i + 1] - prof[i]) * (fi - (float)i);
      float cov = w - v + 0.5f;
      if (cov <= 0.f) continue;
      put(c, x, y, col, covAlpha(cov, a));
    }
  }
}

void glow(Canvas &c, float cx, float cy, float r, uint16_t col, uint8_t a, bool additive) {
  if (r <= 0.f || !a) return;
  int y0 = imax(c.cy0, (int)floorf(cy - r)), y1 = imin(c.cy1 - 1, (int)ceilf(cy + r));
  int x0 = imax(c.cx0, (int)floorf(cx - r)), x1 = imin(c.cx1 - 1, (int)ceilf(cx + r));
  const float inv = 1.f / (r * r);
  for (int y = y0; y <= y1; ++y) {
    float dy = (float)y + 0.5f - cy;
    uint16_t *row = c.px + (size_t)y * c.w;
    for (int x = x0; x <= x1; ++x) {
      float dx = (float)x + 0.5f - cx;
      float t = 1.f - (dx * dx + dy * dy) * inv;
      if (t <= 0.f) continue;
      uint8_t al = (uint8_t)(t * t * (float)a);
      if (!al) continue;
      row[x] = additive ? addColor(row[x], col, al) : blend(row[x], col, al);
    }
  }
}

// ---- materials ----------------------------------------------------------------
float grainAt(int x, int y, uint32_t seed) {
  uint32_t h = (uint32_t)x * 0x9E3779B1u ^ (uint32_t)y * 0x85EBCA77u ^ seed;
  h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;
  return (float)(h & 0xFFFF) / 32767.5f - 1.f;
}

void sphere(Canvas &c, float cx, float cy, float r, uint16_t col, uint8_t a) {
  if (r <= 0.5f || !a) return;
  float br, bg, bb;
  unpack(col, br, bg, bb);
  // Light from the top-left, slightly in front; Blinn half-vector toward the eye.
  const float Lx = -0.44f, Ly = -0.58f, Lz = 0.69f;
  float hx = Lx, hy = Ly, hz = Lz + 1.f;
  float hl = sqrtf(hx * hx + hy * hy + hz * hz);
  hx /= hl; hy /= hl; hz /= hl;
  int x0 = imax(c.cx0, (int)floorf(cx - r - 1)), x1 = imin(c.cx1 - 1, (int)ceilf(cx + r + 1));
  int y0 = imax(c.cy0, (int)floorf(cy - r - 1)), y1 = imin(c.cy1 - 1, (int)ceilf(cy + r + 1));
  const float inv = 1.f / r;
  for (int y = y0; y <= y1; ++y) {
    float dy = ((float)y + 0.5f - cy) * inv;
    for (int x = x0; x <= x1; ++x) {
      float dx = ((float)x + 0.5f - cx) * inv;
      float d2 = dx * dx + dy * dy;
      float cov = (1.f - sqrtf(d2)) * r + 0.5f;
      if (cov <= 0.f) continue;
      float nz = d2 < 1.f ? sqrtf(1.f - d2) : 0.f;
      float diff = Lx * dx + Ly * dy + Lz * nz;
      if (diff < 0.f) diff = 0.f;
      float spec = hx * dx + hy * dy + hz * nz;
      spec = spec > 0.f ? powf(spec, 30.f) * 0.9f : 0.f;
      float rim = (1.f - nz);
      rim = rim * rim * 0.45f;                      // light scattering at the edge
      float shade = 0.26f + 0.86f * diff + rim;
      float R = br * shade + 255.f * spec, G = bg * shade + 255.f * spec, B = bb * shade + 255.f * spec;
      uint16_t px = rgb((uint8_t)(R > 255.f ? 255.f : R), (uint8_t)(G > 255.f ? 255.f : G),
                        (uint8_t)(B > 255.f ? 255.f : B));
      put(c, x, y, px, covAlpha(cov, a));
    }
  }
}

void softShadow(Canvas &c, int x, int y, int w, int h, float r, float spread, uint8_t a,
                int dy, bool skipInside) {
  if (w <= 0 || h <= 0 || !a || spread <= 0.f) return;
  float maxR = (float)imin(w, h) * 0.5f;
  if (r > maxR) r = maxR;
  int s = (int)ceilf(spread);
  int y0 = imax(c.cy0, y + dy - s), y1 = imin(c.cy1, y + dy + h + s);
  int x0 = imax(c.cx0, x - s), x1 = imin(c.cx1, x + w + s);
  const float inv = 1.f / spread;
  const float hy = (float)h * 0.5f;
  const float cyr = (float)y + hy;
  // Columns whose x lies on the straight top/bottom edges: there the
  // distance only depends on the row, so they are handled as one span.
  const int mL = imax(x0, (int)ceilf((float)x + r - 0.5f));
  const int mR = imin(x1, (int)floorf((float)(x + w) - r - 0.5f) + 1);
  auto shade = [&](uint16_t *row, int xx, float d) {
    if (d <= 0.f) {
      if (!skipInside) row[xx] = blend(row[xx], 0x0000, a);
      return;
    }
    if (d >= spread) return;
    float t = 1.f - d * inv;
    uint8_t al = (uint8_t)(t * t * (float)a);
    if (al) row[xx] = blend(row[xx], 0x0000, al);
  };
  for (int yy = y0; yy < y1; ++yy) {
    uint16_t *row = c.px + (size_t)yy * c.w;
    const float py = (float)(yy - dy) + 0.5f;
    for (int xx = x0; xx < imin(x1, mL); ++xx)
      shade(row, xx, rrSdf((float)xx + 0.5f, py, (float)x, (float)y, (float)w, (float)h, r));
    if (mR > mL) {
      float qy = fabsf(py - cyr) - (hy - r);
      if (qy > 0.f) {
        float d = qy - r;
        if (d <= 0.f) {
          if (!skipInside) for (int xx = mL; xx < mR; ++xx) row[xx] = blend(row[xx], 0x0000, a);
        } else if (d < spread) {
          float t = 1.f - d * inv;
          uint8_t al = (uint8_t)(t * t * (float)a);
          if (al) for (int xx = mL; xx < mR; ++xx) row[xx] = blend(row[xx], 0x0000, al);
        }
      } else if (!skipInside) {
        for (int xx = mL; xx < mR; ++xx) row[xx] = blend(row[xx], 0x0000, a);
      }
    }
    for (int xx = imax(x0, mR > mL ? mR : mL); xx < x1; ++xx)
      shade(row, xx, rrSdf((float)xx + 0.5f, py, (float)x, (float)y, (float)w, (float)h, r));
  }
}

void fillRoundRectV(Canvas &c, int x, int y, int w, int h, float r, uint16_t top,
                    uint16_t bottom, uint8_t a) {
  if (w <= 0 || h <= 0 || !a) return;
  float maxR = (float)imin(w, h) * 0.5f;
  if (r > maxR) r = maxR;
  if (r < 0.f) r = 0.f;
  int ri = (int)ceilf(r);
  int y0 = imax(c.cy0, y), y1 = imin(c.cy1, y + h);
  int x0 = imax(c.cx0, x), x1 = imin(c.cx1, x + w);
  for (int yy = y0; yy < y1; ++yy) {
    uint16_t col = blend(top, bottom, (uint8_t)(h > 1 ? (yy - y) * 255 / (h - 1) : 0));
    bool cornerRow = (yy < y + ri) || (yy >= y + h - ri);
    uint16_t *row = c.px + (size_t)yy * c.w;
    for (int xx = x0; xx < x1; ++xx) {
      bool cornerCol = (xx < x + ri) || (xx >= x + w - ri);
      uint8_t al = a;
      if (cornerRow && cornerCol) {
        float d = rrSdf((float)xx + 0.5f, (float)yy + 0.5f, (float)x, (float)y, (float)w, (float)h, r);
        al = covAlpha(0.5f - d, a);
        if (!al) continue;
      }
      row[xx] = (al == 255) ? col : blend(row[xx], col, al);
    }
  }
}

// ---- text -------------------------------------------------------------------
static inline const FontGlyph *glyphOf(const Font &f, unsigned char ch) {
  if (ch < f.first || ch > f.last) ch = '?';
  return &f.glyphs[ch - f.first];
}

static inline int nibAt(const Font &f, const FontGlyph *g, int x, int y) {
  if (x < 0 || y < 0 || x >= g->w || y >= g->h) return 0;
  int k = y * g->w + x;
  uint8_t b = f.bits[g->off + (k >> 1)];
  return (k & 1) ? (b & 0x0F) : (b >> 4);
}

// Draw one glyph rotated: pen origin (px, py) on the baseline, reading
// direction (tx, ty) as a unit vector; glyph "down" is that vector turned
// +90 degrees. Coverage is bilinearly sampled from the 4-bit bitmap.
static void glyphRotated(Canvas &c, const Font &f, unsigned char ch, float px, float py,
                         float tx, float ty, uint16_t col, uint8_t a) {
  const FontGlyph *g = glyphOf(f, ch);
  if (!g->w || !g->h) return;
  const float nx = -ty, ny = tx;
  float lx[4] = {(float)g->dx, (float)(g->dx + g->w), (float)g->dx, (float)(g->dx + g->w)};
  float ly[4] = {(float)g->dy, (float)g->dy, (float)(g->dy + g->h), (float)(g->dy + g->h)};
  float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
  for (int i = 0; i < 4; ++i) {
    float sx = px + lx[i] * tx + ly[i] * nx, sy = py + lx[i] * ty + ly[i] * ny;
    minx = fminf(minx, sx); maxx = fmaxf(maxx, sx);
    miny = fminf(miny, sy); maxy = fmaxf(maxy, sy);
  }
  int x0 = imax(c.cx0, (int)floorf(minx) - 1), x1 = imin(c.cx1 - 1, (int)ceilf(maxx) + 1);
  int y0 = imax(c.cy0, (int)floorf(miny) - 1), y1 = imin(c.cy1 - 1, (int)ceilf(maxy) + 1);
  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      float dx = (float)x + 0.5f - px, dy = (float)y + 0.5f - py;
      float u = dx * tx + dy * ty - (float)g->dx - 0.5f;   // bitmap coords, pixel centres
      float v = dx * nx + dy * ny - (float)g->dy - 0.5f;
      if (u < -1.f || v < -1.f || u > (float)g->w || v > (float)g->h) continue;
      int ix = (int)floorf(u), iy = (int)floorf(v);
      float fx = u - (float)ix, fy = v - (float)iy;
      float cov = ((float)nibAt(f, g, ix, iy) * (1.f - fx) + (float)nibAt(f, g, ix + 1, iy) * fx) * (1.f - fy) +
                  ((float)nibAt(f, g, ix, iy + 1) * (1.f - fx) + (float)nibAt(f, g, ix + 1, iy + 1) * fx) * fy;
      if (cov <= 0.05f) continue;
      put(c, x, y, col, (uint8_t)(cov * (float)a / 15.f));
    }
  }
}

float textArcHalfSpanDeg(const Font &f, float radius, const char *s, float trackPx) {
  if (!s || radius <= 1.f) return 0.f;
  float w = 0.f;
  int n = 0;
  for (const unsigned char *p = (const unsigned char *)s; *p; ++p, ++n) w += glyphOf(f, *p)->adv;
  if (n > 1) w += trackPx * (float)(n - 1);
  return (w * 0.5f / radius) * 57.2957795f;
}

void textArc(Canvas &c, const Font &f, float cx, float cy, float radius, float centreDeg,
             const char *s, uint16_t col, uint8_t a, float trackPx, bool bottom) {
  if (!s || !*s || radius <= 1.f || !a) return;
  float total = 0.f;
  int n = 0;
  for (const unsigned char *p = (const unsigned char *)s; *p; ++p, ++n) total += glyphOf(f, *p)->adv;
  total += trackPx * (float)(n > 1 ? n - 1 : 0);
  const float c0 = centreDeg * 0.0174532925f;
  float pos = -total * 0.5f;                       // arc position of the pen
  for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
    const FontGlyph *g = glyphOf(f, *p);
    float mid = pos + (float)g->adv * 0.5f;        // glyph centre along the arc
    float th = bottom ? c0 - mid / radius : c0 + mid / radius;
    float sx = cx + radius * sinf(th), sy = cy - radius * cosf(th);
    float tx = bottom ? -cosf(th) : cosf(th);
    float ty = bottom ? -sinf(th) : sinf(th);
    float ox = sx - tx * (float)g->adv * 0.5f, oy = sy - ty * (float)g->adv * 0.5f;
    if (*p != ' ') glyphRotated(c, f, *p, ox, oy, tx, ty, col, a);
    pos += (float)g->adv + trackPx;
  }
}

int textWidth(const Font &f, const char *s) {
  int w = 0;
  if (!s) return 0;
  for (const unsigned char *p = (const unsigned char *)s; *p; ++p) w += glyphOf(f, *p)->adv;
  return w;
}

int text(Canvas &c, const Font &f, int x, int baseline, const char *s, uint16_t col, uint8_t a) {
  if (!s || !a) return x;
  for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
    const FontGlyph *g = glyphOf(f, *p);
    const uint8_t *bits = f.bits + g->off;
    int gx = x + g->dx, gy = baseline + g->dy;
    for (int j = 0; j < g->h; ++j) {
      int yy = gy + j;
      if (yy < c.cy0 || yy >= c.cy1) continue;
      uint16_t *row = c.px + (size_t)yy * c.w;
      for (int i = 0; i < g->w; ++i) {
        int xx = gx + i;
        if (xx < c.cx0 || xx >= c.cx1) continue;
        int k = j * g->w + i;
        uint8_t nib = (k & 1) ? (bits[k >> 1] & 0x0F) : (bits[k >> 1] >> 4);
        if (!nib) continue;
        uint8_t al = (uint8_t)((nib * 17u * a + 127u) / 255u);
        row[xx] = (al == 255) ? col : blend(row[xx], col, al);
      }
    }
    x += g->adv;
  }
  return x;
}

void textCentered(Canvas &c, const Font &f, int cx, int baseline, const char *s, uint16_t col, uint8_t a) {
  text(c, f, cx - textWidth(f, s) / 2, baseline, s, col, a);
}

int textWidthTracked(const Font &f, const char *s, int track) {
  if (!s || !*s) return 0;
  int n = (int)strlen(s);
  return textWidth(f, s) + track * (n - 1);
}

int textTracked(Canvas &c, const Font &f, int x, int baseline, const char *s, uint16_t col,
                uint8_t a, int track) {
  if (!s) return x;
  char one[2] = {0, 0};
  for (const char *p = s; *p; ++p) {
    one[0] = *p;
    x = text(c, f, x, baseline, one, col, a);
    if (p[1]) x += track;
  }
  return x;
}

void textCenteredTracked(Canvas &c, const Font &f, int cx, int baseline, const char *s,
                         uint16_t col, uint8_t a, int track) {
  textTracked(c, f, cx - textWidthTracked(f, s, track) / 2, baseline, s, col, a, track);
}

void textShadowed(Canvas &c, const Font &f, int x, int baseline, const char *s, uint16_t col,
                  uint8_t a, uint16_t shadow, uint8_t shadowA) {
  text(c, f, x + 1, baseline + 1, s, shadow, (uint8_t)((uint32_t)shadowA * a / 255));
  text(c, f, x, baseline, s, col, a);
}

void textRight(Canvas &c, const Font &f, int rx, int baseline, const char *s, uint16_t col, uint8_t a) {
  text(c, f, rx - textWidth(f, s), baseline, s, col, a);
}

void fitText(const Font &f, const char *s, int maxW, char *out, size_t cap) {
  if (!cap) return;
  size_t n = s ? strlen(s) : 0;
  if (n >= cap) n = cap - 1;
  memcpy(out, s ? s : "", n);
  out[n] = '\0';
  if (textWidth(f, out) <= maxW) return;
  const int ell = glyphOf(f, 0x80)->adv;
  while (n > 0) {
    out[--n] = '\0';
    while (n > 0 && out[n - 1] == ' ') out[--n] = '\0';
    if (textWidth(f, out) + ell <= maxW) break;
  }
  if (n + 1 < cap) { out[n] = (char)0x80; out[n + 1] = '\0'; }
}

// Greedy word wrap. Calls emit(lineStart, lineLen) for each line.
template <typename Emit>
static int wrapLines(const Font &f, int maxW, const char *s, Emit emit) {
  int lines = 0;
  const char *p = s;
  while (p && *p) {
    while (*p == ' ') ++p;
    if (!*p) break;
    const char *lineStart = p, *lastBreak = nullptr, *q = p;
    int w = 0;
    while (*q && *q != '\n') {
      int gw = glyphOf(f, (unsigned char)*q)->adv;
      if (w + gw > maxW && q > lineStart) break;
      if (*q == ' ') lastBreak = q;
      w += gw;
      ++q;
    }
    const char *end = q;
    if (*q && *q != '\n' && lastBreak) end = lastBreak;     // break at a space
    emit(lineStart, (size_t)(end - lineStart));
    ++lines;
    p = end;
    if (*p == '\n') ++p;
  }
  return lines;
}

int wrappedLineCount(const Font &f, int maxW, const char *s) {
  return wrapLines(f, maxW, s, [](const char *, size_t) {});
}

int textWrapped(Canvas &c, const Font &f, int x, int baseline, int maxW, const char *s,
                uint16_t col, int lineH, uint8_t a, bool centered) {
  if (lineH <= 0) lineH = f.lineH;
  int y = baseline;
  return wrapLines(f, maxW, s, [&](const char *start, size_t len) {
    char buf[96];
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, start, len);
    buf[len] = '\0';
    while (len > 0 && buf[len - 1] == ' ') buf[--len] = '\0';
    if (centered) textCentered(c, f, x + maxW / 2, y, buf, col, a);
    else          text(c, f, x, y, buf, col, a);
    y += lineH;
  });
}

}  // namespace fr
