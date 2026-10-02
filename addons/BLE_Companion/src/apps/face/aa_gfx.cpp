// aa — see aa_gfx.h. Mirrored line for line in web/index.html ("AA MIRROR").
#include "aa_gfx.h"

namespace aa {

// ---------------------------------------------------------------------------
// Colour
// ---------------------------------------------------------------------------
Rgb rgb(uint8_t r, uint8_t g, uint8_t b) {
  Rgb c;
  c.r = r; c.g = g; c.b = b;
  return c;
}

Rgb unpack(uint16_t c) {
  int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
  return rgb((uint8_t)((r << 3) | (r >> 2)), (uint8_t)((g << 2) | (g >> 4)),
             (uint8_t)((b << 3) | (b >> 2)));
}

uint16_t pack(Rgb c) {
  return (uint16_t)(((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3));
}

Rgb mix(Rgb a, Rgb b, int t) {
  if (t <= 0) return a;
  if (t >= 256) return b;
  int u = 256 - t;
  return rgb((uint8_t)((a.r * u + b.r * t + 128) >> 8), (uint8_t)((a.g * u + b.g * t + 128) >> 8),
             (uint8_t)((a.b * u + b.b * t + 128) >> 8));
}

int luma(Rgb c) { return (c.r * 77 + c.g * 150 + c.b * 29) >> 8; }

uint32_t isqrt32(uint32_t n) {
  uint32_t res = 0, bit = 1u << 30;
  while (bit > n) bit >>= 2;
  while (bit) {
    if (n >= res + bit) {
      n -= res + bit;
      res = (res >> 1) + bit;
    } else {
      res >>= 1;
    }
    bit >>= 2;
  }
  return res;
}

static inline void blendPx(uint16_t *p, Rgb c, int a) {
  if (a <= 0) return;
  if (a >= 255) { *p = pack(c); return; }
  Rgb d = unpack(*p);
  int ia = 255 - a;
  int r = (c.r * a + d.r * ia + 127) / 255;
  int g = (c.g * a + d.g * ia + 127) / 255;
  int b = (c.b * a + d.b * ia + 127) / 255;
  *p = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// ---------------------------------------------------------------------------
// Surface
// ---------------------------------------------------------------------------
void init(Surface &s, uint16_t *px, int w, int h) {
  s.px = px;
  s.w = (int16_t)w;
  s.h = (int16_t)h;
  resetClip(s);
}

void resetClip(Surface &s) {
  s.cx0 = 0; s.cy0 = 0; s.cx1 = s.w; s.cy1 = s.h;
}

void setClip(Surface &s, int x0, int y0, int x1, int y1) {
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > s.w) x1 = s.w;
  if (y1 > s.h) y1 = s.h;
  if (x1 < x0) x1 = x0;
  if (y1 < y0) y1 = y0;
  s.cx0 = (int16_t)x0; s.cy0 = (int16_t)y0; s.cx1 = (int16_t)x1; s.cy1 = (int16_t)y1;
}

// Clip [x, x + w) x [y, y + h) to the surface clip. False when empty.
static bool clipBox(const Surface &s, int &x0, int &y0, int &x1, int &y1) {
  if (x0 < s.cx0) x0 = s.cx0;
  if (y0 < s.cy0) y0 = s.cy0;
  if (x1 > s.cx1) x1 = s.cx1;
  if (y1 > s.cy1) y1 = s.cy1;
  return x0 < x1 && y0 < y1;
}

// ---------------------------------------------------------------------------
// Rectangles and the backdrop
// ---------------------------------------------------------------------------
void fillRect(Surface &s, int x, int y, int w, int h, Rgb c) {
  int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
  if (!clipBox(s, x0, y0, x1, y1)) return;
  uint16_t v = pack(c);
  for (int py = y0; py < y1; py++) {
    uint16_t *row = s.px + (int32_t)py * s.w;
    for (int px = x0; px < x1; px++) row[px] = v;
  }
}

void blendRect(Surface &s, int x, int y, int w, int h, Rgb c, int a) {
  int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
  if (!clipBox(s, x0, y0, x1, y1)) return;
  for (int py = y0; py < y1; py++) {
    uint16_t *row = s.px + (int32_t)py * s.w;
    for (int px = x0; px < x1; px++) blendPx(row + px, c, a);
  }
}

static const uint8_t kBayer[4][4] = {
  { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 },
};

static const int kMaxGlows = 3;
static const int kMaxW = 240;
static int16_t sGlowDx[kMaxGlows][kMaxW];   // per-column glow terms (render task only)

void backdrop(Surface &s, int x, int y, int w, int h, Rgb top, Rgb bottom,
              const Glow *glows, int nGlows) {
  int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
  if (!clipBox(s, x0, y0, x1, y1)) return;
  if (x1 > kMaxW) x1 = kMaxW;
  if (nGlows > kMaxGlows) nGlows = kMaxGlows;
  int32_t rx2[kMaxGlows], ry2[kMaxGlows];
  for (int i = 0; i < nGlows; i++) {
    const Glow &g = glows[i];
    rx2[i] = (int32_t)(g.rx < 1 ? 1 : g.rx) * (g.rx < 1 ? 1 : g.rx);
    ry2[i] = (int32_t)(g.ry < 1 ? 1 : g.ry) * (g.ry < 1 ? 1 : g.ry);
    for (int px = x0; px < x1; px++) {
      int32_t dx = px - g.cx;
      int32_t q = dx * dx * 4096 / rx2[i];
      sGlowDx[i][px] = (int16_t)(q > 4096 ? 4096 : q);
    }
  }
  const int32_t h1 = h > 1 ? h - 1 : 1;
  for (int py = y0; py < y1; py++) {
    int32_t k = py - y;
    if (k > h1) k = h1;
    int32_t br = (top.r * 256 * (h1 - k) + bottom.r * 256 * k) / h1;
    int32_t bg = (top.g * 256 * (h1 - k) + bottom.g * 256 * k) / h1;
    int32_t bb = (top.b * 256 * (h1 - k) + bottom.b * 256 * k) / h1;
    int32_t dy2[kMaxGlows];
    for (int i = 0; i < nGlows; i++) {
      int32_t dy = py - glows[i].cy;
      int32_t q = dy * dy * 4096 / ry2[i];
      dy2[i] = q > 4096 ? 4096 : q;
    }
    uint16_t *row = s.px + (int32_t)py * s.w;
    const uint8_t *bay = kBayer[py & 3];
    for (int px = x0; px < x1; px++) {
      int32_t r = br, g = bg, b = bb;
      for (int i = 0; i < nGlows; i++) {
        int32_t q = sGlowDx[i][px] + dy2[i];
        if (q >= 4096) continue;
        int32_t t = 4096 - q;
        int32_t wq = (t * t) >> 12;
        int32_t W = wq * glows[i].a / 255;
        r += ((glows[i].c.r * 256 - r) * W) >> 12;
        g += ((glows[i].c.g * 256 - g) * W) >> 12;
        b += ((glows[i].c.b * 256 - b) * W) >> 12;
      }
      int32_t d = bay[px & 3] * 4096 + 2048;
      int32_t r5 = (r * 31 + d) >> 16, g6 = (g * 63 + d) >> 16, b5 = (b * 31 + d) >> 16;
      row[px] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
    }
  }
}

// ---------------------------------------------------------------------------
// Rounded boxes
// ---------------------------------------------------------------------------
static int clampRadius(int r, int w, int h) {
  int m = (w < h ? w : h) / 2;
  if (r > m) r = m;
  return r < 0 ? 0 : r;
}

int sdRoundBox(int px, int py, int x, int y, int w, int h, int r) {
  int X2 = 2 * px + 1, Y2 = 2 * py + 1;
  int ax = X2 - (2 * x + w), ay = Y2 - (2 * y + h);
  if (ax < 0) ax = -ax;
  if (ay < 0) ay = -ay;
  int qx = ax - (w - 2 * r), qy = ay - (h - 2 * r);
  int d;
  if (qx > 0 && qy > 0) d = (int)isqrt32(((uint32_t)(qx * qx + qy * qy)) << 14);
  else d = (qx > qy ? qx : qy) * 128;
  return d - r * 256;
}

static inline int coverage(int d) {      // Q8 signed distance -> 0..256
  int c = 128 - d;
  return c < 0 ? 0 : (c > 256 ? 256 : c);
}

void roundRect(Surface &s, int x, int y, int w, int h, int r, Rgb c, int a) {
  if (w <= 0 || h <= 0) return;
  r = clampRadius(r, w, h);
  int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
  if (!clipBox(s, x0, y0, x1, y1)) return;
  for (int py = y0; py < y1; py++) {
    uint16_t *row = s.px + (int32_t)py * s.w;
    for (int px = x0; px < x1; px++) {
      int cov = coverage(sdRoundBox(px, py, x, y, w, h, r));
      if (cov) blendPx(row + px, c, (cov * a) >> 8);
    }
  }
}

void roundRectV(Surface &s, int x, int y, int w, int h, int r, Rgb top, Rgb bottom, int a) {
  if (w <= 0 || h <= 0) return;
  r = clampRadius(r, w, h);
  int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
  if (!clipBox(s, x0, y0, x1, y1)) return;
  int h1 = h > 1 ? h - 1 : 1;
  for (int py = y0; py < y1; py++) {
    Rgb c = mix(top, bottom, (py - y) * 256 / h1);
    uint16_t *row = s.px + (int32_t)py * s.w;
    for (int px = x0; px < x1; px++) {
      int cov = coverage(sdRoundBox(px, py, x, y, w, h, r));
      if (cov) blendPx(row + px, c, (cov * a) >> 8);
    }
  }
}

void roundRectVA(Surface &s, int x, int y, int w, int h, int r, Rgb c, int aTop, int aBottom) {
  if (w <= 0 || h <= 0) return;
  r = clampRadius(r, w, h);
  int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
  if (!clipBox(s, x0, y0, x1, y1)) return;
  int h1 = h > 1 ? h - 1 : 1;
  for (int py = y0; py < y1; py++) {
    int k = py - y;
    int a = (aTop * (h1 - k) + aBottom * k) / h1;
    uint16_t *row = s.px + (int32_t)py * s.w;
    for (int px = x0; px < x1; px++) {
      int cov = coverage(sdRoundBox(px, py, x, y, w, h, r));
      if (cov) blendPx(row + px, c, (cov * a) >> 8);
    }
  }
}

void roundRectStrokeVA(Surface &s, int x, int y, int w, int h, int r, int t, Rgb c, int aTop,
                       int aBottom) {
  if (w <= 0 || h <= 0 || t <= 0) return;
  r = clampRadius(r, w, h);
  int iw = w - 2 * t, ih = h - 2 * t, ir = r - t;
  if (ir < 0) ir = 0;
  bool inner = iw > 0 && ih > 0;
  if (inner) ir = clampRadius(ir, iw, ih);
  int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
  if (!clipBox(s, x0, y0, x1, y1)) return;
  int h1 = h > 1 ? h - 1 : 1;
  for (int py = y0; py < y1; py++) {
    int k = py - y;
    int a = (aTop * (h1 - k) + aBottom * k) / h1;
    uint16_t *row = s.px + (int32_t)py * s.w;
    for (int px = x0; px < x1; px++) {
      int cov = coverage(sdRoundBox(px, py, x, y, w, h, r));
      if (inner) cov -= coverage(sdRoundBox(px, py, x + t, y + t, iw, ih, ir));
      if (cov > 0) blendPx(row + px, c, (cov * a) >> 8);
    }
  }
}

void roundRectStroke(Surface &s, int x, int y, int w, int h, int r, int t, Rgb c, int a) {
  roundRectStrokeVA(s, x, y, w, h, r, t, c, a, a);
}

void softShadow(Surface &s, int x, int y, int w, int h, int r, int blur, Rgb c, int a) {
  if (w <= 0 || h <= 0 || blur <= 0) return;
  r = clampRadius(r, w, h);
  int x0 = x - blur, y0 = y - blur, x1 = x + w + blur, y1 = y + h + blur;
  if (!clipBox(s, x0, y0, x1, y1)) return;
  const int lim = blur * 256;
  for (int py = y0; py < y1; py++) {
    uint16_t *row = s.px + (int32_t)py * s.w;
    for (int px = x0; px < x1; px++) {
      int d = sdRoundBox(px, py, x, y, w, h, r);
      if (d >= lim) continue;
      int k = 256;
      if (d > 0) {
        int t = 256 - d / blur;
        k = (t * t) >> 8;
      }
      blendPx(row + px, c, (k * a) >> 8);
    }
  }
}

// ---------------------------------------------------------------------------
// Circles, capsules, arcs (Q8)
// ---------------------------------------------------------------------------
static inline int floorDiv256(int v) { return v >> 8; }   // arithmetic shift == floor

static inline uint32_t dist2(int dx, int dy) {
  uint32_t ux = (uint32_t)(dx < 0 ? -dx : dx), uy = (uint32_t)(dy < 0 ? -dy : dy);
  return ux * ux + uy * uy;
}

// Exact shortcuts (no change to any value): coverage is 0 when
// d2 >= (r + 128)^2 and full when d2 < (r - 127)^2, so isqrt only runs on
// the anti-aliased edge.
void disc(Surface &s, int cx, int cy, int r, Rgb c, int a) {
  int x0 = floorDiv256(cx - r) - 1, x1 = floorDiv256(cx + r) + 2;
  int y0 = floorDiv256(cy - r) - 1, y1 = floorDiv256(cy + r) + 2;
  if (!clipBox(s, x0, y0, x1, y1)) return;
  const uint32_t outer = (uint32_t)(r + 128) * (uint32_t)(r + 128);
  const uint32_t inner = r > 127 ? (uint32_t)(r - 127) * (uint32_t)(r - 127) : 0;
  for (int py = y0; py < y1; py++) {
    uint16_t *row = s.px + (int32_t)py * s.w;
    int dy = py * 256 + 128 - cy;
    for (int px = x0; px < x1; px++) {
      int dx = px * 256 + 128 - cx;
      uint32_t d2 = dist2(dx, dy);
      if (d2 >= outer) continue;
      if (d2 < inner) { blendPx(row + px, c, a); continue; }
      int cov = coverage((int)isqrt32(d2) - r);
      if (cov) blendPx(row + px, c, (cov * a) >> 8);
    }
  }
}

// Squared-distance band outside which a ring of centreline r and half
// thickness `half` has zero coverage (exact, see disc()).
static void annulus(int r, int half, uint32_t &lo, uint32_t &hi) {
  int o = r + half + 128, i = r - half - 127;
  hi = (uint32_t)o * (uint32_t)o;
  lo = i > 0 ? (uint32_t)i * (uint32_t)i : 0;
}

void ring(Surface &s, int cx, int cy, int r, int thick, Rgb c, int a) {
  uint32_t lo, hi;
  annulus(r, thick / 2, lo, hi);
  int ro = r + thick / 2;
  int x0 = floorDiv256(cx - ro) - 1, x1 = floorDiv256(cx + ro) + 2;
  int y0 = floorDiv256(cy - ro) - 1, y1 = floorDiv256(cy + ro) + 2;
  if (!clipBox(s, x0, y0, x1, y1)) return;
  for (int py = y0; py < y1; py++) {
    uint16_t *row = s.px + (int32_t)py * s.w;
    int dy = py * 256 + 128 - cy;
    for (int px = x0; px < x1; px++) {
      int dx = px * 256 + 128 - cx;
      uint32_t d2 = dist2(dx, dy);
      if (d2 >= hi || d2 < lo) continue;
      int d = (int)isqrt32(d2) - r;
      if (d < 0) d = -d;
      int cov = coverage(d - thick / 2);
      if (cov) blendPx(row + px, c, (cov * a) >> 8);
    }
  }
}

void capsule(Surface &s, int x0q, int y0q, int x1q, int y1q, int r, Rgb c, int a) {
  // Axis-aligned segments only (horizontal unless x0 == x1).
  bool vertical = x0q == x1q;
  if (vertical ? (y1q < y0q) : (x1q < x0q)) {
    int t = x0q; x0q = x1q; x1q = t;
    t = y0q; y0q = y1q; y1q = t;
  }
  int bx0 = floorDiv256((x0q < x1q ? x0q : x1q) - r) - 1, bx1 = floorDiv256((x0q > x1q ? x0q : x1q) + r) + 2;
  int by0 = floorDiv256((y0q < y1q ? y0q : y1q) - r) - 1, by1 = floorDiv256((y0q > y1q ? y0q : y1q) + r) + 2;
  if (!clipBox(s, bx0, by0, bx1, by1)) return;
  for (int py = by0; py < by1; py++) {
    uint16_t *row = s.px + (int32_t)py * s.w;
    int Y = py * 256 + 128;
    for (int px = bx0; px < bx1; px++) {
      int X = px * 256 + 128;
      int dx, dy;
      if (vertical) {
        dx = X - x0q;
        dy = Y < y0q ? Y - y0q : (Y > y1q ? Y - y1q : 0);
      } else {
        dy = Y - y0q;
        dx = X < x0q ? X - x0q : (X > x1q ? X - x1q : 0);
      }
      int d = (int)isqrt32(dist2(dx, dy)) - r;
      int cov = coverage(d);
      if (cov) blendPx(row + px, c, (cov * a) >> 8);
    }
  }
}

// sin() over a quarter turn in 256 steps, Q14.
static const int16_t kSinQ14[257] = {
  0, 101, 201, 302, 402, 503, 603, 704, 804, 904, 1005, 1105, 1205, 1306, 1406, 1506,
  1606, 1706, 1806, 1906, 2006, 2105, 2205, 2305, 2404, 2503, 2603, 2702, 2801, 2900, 2999, 3098,
  3196, 3295, 3393, 3492, 3590, 3688, 3786, 3883, 3981, 4078, 4176, 4273, 4370, 4467, 4563, 4660,
  4756, 4852, 4948, 5044, 5139, 5235, 5330, 5425, 5520, 5614, 5708, 5803, 5897, 5990, 6084, 6177,
  6270, 6363, 6455, 6547, 6639, 6731, 6823, 6914, 7005, 7096, 7186, 7276, 7366, 7456, 7545, 7635,
  7723, 7812, 7900, 7988, 8076, 8163, 8250, 8337, 8423, 8509, 8595, 8680, 8765, 8850, 8935, 9019,
  9102, 9186, 9269, 9352, 9434, 9516, 9598, 9679, 9760, 9841, 9921, 10001, 10080, 10159, 10238, 10316,
  10394, 10471, 10549, 10625, 10702, 10778, 10853, 10928, 11003, 11077, 11151, 11224, 11297, 11370, 11442, 11514,
  11585, 11656, 11727, 11797, 11866, 11935, 12004, 12072, 12140, 12207, 12274, 12340, 12406, 12472, 12537, 12601,
  12665, 12729, 12792, 12854, 12916, 12978, 13039, 13100, 13160, 13219, 13279, 13337, 13395, 13453, 13510, 13567,
  13623, 13678, 13733, 13788, 13842, 13896, 13949, 14001, 14053, 14104, 14155, 14206, 14256, 14305, 14354, 14402,
  14449, 14497, 14543, 14589, 14635, 14680, 14724, 14768, 14811, 14854, 14896, 14937, 14978, 15019, 15059, 15098,
  15137, 15175, 15213, 15250, 15286, 15322, 15357, 15392, 15426, 15460, 15493, 15525, 15557, 15588, 15619, 15649,
  15679, 15707, 15736, 15763, 15791, 15817, 15843, 15868, 15893, 15917, 15941, 15964, 15986, 16008, 16029, 16049,
  16069, 16088, 16107, 16125, 16143, 16160, 16176, 16192, 16207, 16221, 16235, 16248, 16261, 16273, 16284, 16295,
  16305, 16315, 16324, 16332, 16340, 16347, 16353, 16359, 16364, 16369, 16373, 16376, 16379, 16381, 16383, 16384,
  16384,
};

void dir(int t, int &x, int &y) {
  t &= 1023;
  int q = t >> 8, k = t & 255;
  int sn = kSinQ14[k], cs = kSinQ14[256 - k];
  int s, c;
  switch (q) {
    case 0:  s = sn;  c = cs;  break;
    case 1:  s = cs;  c = -sn; break;
    case 2:  s = -sn; c = -cs; break;
    default: s = -cs; c = sn;  break;
  }
  x = s;       // screen x grows right
  y = -c;      // screen y grows down; turn 0 points up
}

void arc(Surface &s, int cx, int cy, int r, int thick, int a0, int a1, Rgb c, int a) {
  int span = a1 - a0;
  if (span <= 0) return;
  if (span >= 1024) { ring(s, cx, cy, r, thick, c, a); return; }
  int u0x, u0y, u1x, u1y;
  dir(a0, u0x, u0y);
  dir(a1, u1x, u1y);
  int p0x = cx + ((r * u0x) >> 14), p0y = cy + ((r * u0y) >> 14);
  int p1x = cx + ((r * u1x) >> 14), p1y = cy + ((r * u1y) >> 14);
  int half = thick / 2;
  uint32_t lo, hi;
  annulus(r, half, lo, hi);
  int ro = r + half;
  int x0 = floorDiv256(cx - ro) - 1, x1 = floorDiv256(cx + ro) + 2;
  int y0 = floorDiv256(cy - ro) - 1, y1 = floorDiv256(cy + ro) + 2;
  if (!clipBox(s, x0, y0, x1, y1)) return;
  for (int py = y0; py < y1; py++) {
    uint16_t *row = s.px + (int32_t)py * s.w;
    int vy = py * 256 + 128 - cy;
    for (int px = x0; px < x1; px++) {
      int vx = px * 256 + 128 - cx;
      uint32_t v2 = dist2(vx, vy);
      if (v2 >= hi || v2 < lo) continue;      // farther than half + 0.5 px from the circle
      int c0 = (u0x >> 2) * (vy >> 2) - (u0y >> 2) * (vx >> 2);
      int c1 = (u1x >> 2) * (vy >> 2) - (u1y >> 2) * (vx >> 2);
      bool inside = span <= 512 ? (c0 >= 0 && c1 <= 0) : (c0 >= 0 || c1 <= 0);
      int d;
      if (inside) {
        d = (int)isqrt32(v2) - r;
        if (d < 0) d = -d;
      } else {
        int d0 = (int)isqrt32(dist2(px * 256 + 128 - p0x, py * 256 + 128 - p0y));
        int d1 = (int)isqrt32(dist2(px * 256 + 128 - p1x, py * 256 + 128 - p1y));
        d = d0 < d1 ? d0 : d1;
      }
      int cov = coverage(d - half);
      if (cov) blendPx(row + px, c, (cov * a) >> 8);
    }
  }
}

// ---------------------------------------------------------------------------
// Masks and text
// ---------------------------------------------------------------------------
static inline int nibble(const uint8_t *bits, int32_t i) {
  uint8_t b = bits[i >> 1];
  return (i & 1) ? (b & 15) : (b >> 4);
}

static void blitNibbles(Surface &s, const uint8_t *bits, int w, int h, int x, int y, Rgb top,
                        Rgb bottom, int gy0, int gy1, bool grad, int a) {
  int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
  if (!clipBox(s, x0, y0, x1, y1)) return;
  int span = gy1 - gy0;
  if (span < 1) span = 1;
  for (int py = y0; py < y1; py++) {
    Rgb c = top;
    if (grad) {
      int k = py - gy0;
      if (k < 0) k = 0;
      if (k > span) k = span;
      c = mix(top, bottom, k * 256 / span);
    }
    uint16_t *row = s.px + (int32_t)py * s.w;
    int32_t base = (int32_t)(py - y) * w - x;
    for (int px = x0; px < x1; px++) {
      int n = nibble(bits, base + px);
      if (!n) continue;
      blendPx(row + px, c, (n * 17 * a + 127) / 255);
    }
  }
}

void mask(Surface &s, const Mask &m, int x, int y, Rgb c, int a) {
  blitNibbles(s, m.bits, m.w, m.h, x, y, c, c, 0, 1, false, a);
}

static const Glyph &glyphFor(const Font &f, uint8_t code, uint8_t flags) {
  if ((flags & TF_UPPER) && code >= 'a' && code <= 'z') code = (uint8_t)(code - 32);
  uint8_t gi = f.index[code];
  if (gi == 0xFF) gi = f.fallback;
  return f.glyphs[gi];
}

static int digitCell(const Font &f) {
  int cell = 0;
  for (uint8_t d = '0'; d <= '9'; d++) {
    uint8_t gi = f.index[d];
    if (gi != 0xFF && f.glyphs[gi].adv > cell) cell = f.glyphs[gi].adv;
  }
  return cell;
}

static inline bool isDigit(uint8_t c) { return c >= '0' && c <= '9'; }

int textWidth(const Font &f, const uint8_t *g, int n, int tracking, uint8_t flags) {
  int cell = (flags & TF_TABULAR) ? digitCell(f) : 0;
  int w = 0;
  for (int i = 0; i < n; i++) {
    const Glyph &gl = glyphFor(f, g[i], flags);
    w += (cell && isDigit(g[i])) ? cell : gl.adv;
    if (i + 1 < n) w += tracking;
  }
  return w;
}

static int drawRun(Surface &s, const Font &f, const uint8_t *g, int n, int x, int y, Rgb top,
                   Rgb bottom, int gy0, int gy1, bool grad, int a, int tracking, uint8_t flags) {
  int cell = (flags & TF_TABULAR) ? digitCell(f) : 0;
  int pen = x;
  for (int i = 0; i < n; i++) {
    const Glyph &gl = glyphFor(f, g[i], flags);
    int adv = gl.adv, shift = 0;
    if (cell && isDigit(g[i])) {
      shift = (cell - adv) / 2;
      adv = cell;
    }
    if (gl.w && gl.h) {
      blitNibbles(s, f.bits + gl.off, gl.w, gl.h, pen + shift + gl.dx, y + gl.dy, top, bottom,
                  gy0, gy1, grad, a);
    }
    pen += adv;
    if (i + 1 < n) pen += tracking;
  }
  return pen - x;
}

int text(Surface &s, const Font &f, const uint8_t *g, int n, int x, int y, Rgb c, int a,
         int tracking, uint8_t flags) {
  return drawRun(s, f, g, n, x, y, c, c, 0, 1, false, a, tracking, flags);
}

int textV(Surface &s, const Font &f, const uint8_t *g, int n, int x, int y, Rgb top,
          Rgb bottom, int y0, int y1, int a, int tracking, uint8_t flags) {
  return drawRun(s, f, g, n, x, y, top, bottom, y0, y1, true, a, tracking, flags);
}

static int slen(const char *s) {
  int n = 0;
  while (s[n]) n++;
  return n;
}

int textWidthS(const Font &f, const char *s, int tracking, uint8_t flags) {
  return textWidth(f, (const uint8_t *)s, slen(s), tracking, flags);
}

int textS(Surface &s, const Font &f, const char *str, int x, int y, Rgb c, int a, int tracking,
          uint8_t flags) {
  return text(s, f, (const uint8_t *)str, slen(str), x, y, c, a, tracking, flags);
}

int textCS(Surface &s, const Font &f, const char *str, int cx, int y, Rgb c, int a, int tracking,
           uint8_t flags) {
  int w = textWidthS(f, str, tracking, flags);
  return textS(s, f, str, cx - w / 2, y, c, a, tracking, flags);
}

}  // namespace aa
