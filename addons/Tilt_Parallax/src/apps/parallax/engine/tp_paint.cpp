#include "tp_paint.h"
#include "tp_color.h"
#include "tp_platform.h"
#include <math.h>

namespace tp {

void fillBelowRidge(Painter &P, const float *ridge, GroundShader shade, void *ctx) {
  Layer &L = P.L;
  // Per column: 4 sub-column ridge samples (for anti-aliasing) and their range.
  float *sub = (float *)allocBig(sizeof(float) * 6 * (size_t)L.w);
  if (!sub) return;
  float *rmin = sub + 4 * (size_t)L.w, *rmax = rmin + L.w;
  float top = 1e9f;
  for (int x = 0; x < L.w; x++) {
    float lo = 1e9f, hi = -1e9f;
    for (int k = 0; k < 4; k++) {
      float r = sampleCol(ridge, L.w, x + (k + 0.5f) * 0.25f - 0.5f);
      sub[x * 4 + k] = r;
      lo = fminf(lo, r);
      hi = fmaxf(hi, r);
    }
    rmin[x] = lo;
    rmax[x] = hi;
    top = fminf(top, lo);
  }
  int yStart = P.ly(top) - 1;
  if (yStart < 0) yStart = 0;
  // Row-major so shaders can cache per-row work.
  for (int y = yStart; y < L.h; y++) {
    float sy = P.sy(y);
    for (int x = 0; x < L.w; x++) {
      if (sy + 1.0f <= rmin[x]) continue;
      float cov;
      if (sy >= rmax[x]) {
        cov = 1.0f;
      } else {
        const float *r = sub + x * 4;
        cov = 0.25f * (sat(sy + 1.0f - r[0]) + sat(sy + 1.0f - r[1]) +
                       sat(sy + 1.0f - r[2]) + sat(sy + 1.0f - r[3]));
        if (cov <= 0) continue;
      }
      uint8_t ix = shade(ctx, x, sy + 0.5f, sy + 0.5f - ridge[x]);
      P.put(x, y, ix, cov);
    }
  }
  freeMem(sub);
}

void drawPine(Painter &P, float cx, float baseY, float height, float width,
              int matL, int matR, float level, uint32_t seed) {
  if (height < 2) return;
  const int tiers = height < 14 ? 3 : (height < 30 ? 4 : (height < 55 ? 5 : 6));
  const float trunkHW = width * 0.07f < 0.6f ? 0.6f : width * 0.07f;
  int x0 = (int)floorf(cx - width * 0.5f - 2), x1 = (int)ceilf(cx + width * 0.5f + 2);
  int y0 = (int)floorf(baseY - height - 1), y1 = (int)ceilf(baseY + 1);
  for (int py = y0; py < y1; py++) {
    // per-row jitter keeps the edge from looking like a ruler
    float jit = (hashUnit(hash2(py, (int)cx, seed)) - 0.5f) * 0.9f;
    for (int px = x0; px < x1; px++) {
      int hits = 0, hitsL = 0;
      float tfAcc = 0;
      bool trunkHit = false;
      for (int sy = 0; sy < 3; sy++) {
        float fy = py + (sy + 0.5f) / 3.0f;
        float rel = (baseY - fy) / height;
        if (rel < 0 || rel > 1) continue;
        for (int sx = 0; sx < 3; sx++) {
          float fx = px + (sx + 0.5f) / 3.0f;
          float dx = fx - cx;
          bool inside = false;
          float tf = 0;
          if (rel >= 0.10f) {
            float t = (rel - 0.10f) / 0.90f;
            float k = (1.0f - t) * tiers;
            tf = k - floorf(k);
            float hw = width * 0.5f * powf(1.0f - t, 0.85f) * (0.60f + 0.40f * tf) + jit * (1.0f - t);
            inside = fabsf(dx) < hw;
          }
          if (!inside && rel < 0.16f && fabsf(dx) < trunkHW) { inside = true; trunkHit = true; }
          if (inside) {
            hits++;
            if (dx < 0) hitsL++;
            tfAcc += tf;
          }
        }
      }
      if (!hits) continue;
      float cov = hits / 9.0f;
      float tf = tfAcc / hits;
      bool left = hitsL * 2 > hits;
      float lv = level + 0.30f * tf - 0.12f;
      if (trunkHit && hits < 3) lv = level * 0.4f;
      lv = sat(lv);
      P.put(px - P.L.x0, py - P.L.y0, packIdxF(left ? matL : matR, lv), cov);
    }
  }
}

void drawBush(Painter &P, float cx, float baseY, float radius, int matL, int matR,
              float level, uint32_t seed) {
  Rng rng(seed);
  struct D { float x, y, r; } d[7];
  int n = 4 + (int)(rng.unit() * 3);
  for (int i = 0; i < n; i++) {
    float a = rng.range(-1.0f, 1.0f);
    d[i].x = cx + a * radius * 0.65f;
    d[i].r = radius * rng.range(0.45f, 0.70f);
    d[i].y = baseY - d[i].r - rng.range(0.0f, radius * 0.55f) * (1.0f - fabsf(a));
  }
  int x0 = (int)floorf(cx - radius * 1.4f), x1 = (int)ceilf(cx + radius * 1.4f);
  int y0 = (int)floorf(baseY - radius * 2.2f), y1 = (int)ceilf(baseY + 1);
  for (int py = y0; py < y1; py++)
    for (int px = x0; px < x1; px++) {
      int hits = 0;
      float lit = 0;
      for (int s = 0; s < 9; s++) {
        float fx = px + ((s % 3) + 0.5f) / 3.0f, fy = py + ((s / 3) + 0.5f) / 3.0f;
        if (fy > baseY) continue;
        for (int i = 0; i < n; i++) {
          float ddx = fx - d[i].x, ddy = fy - d[i].y;
          if (ddx * ddx + ddy * ddy < d[i].r * d[i].r) {
            hits++;
            lit += sat(0.5f - ddy / (2 * d[i].r));   // tops of blobs lighter
            break;
          }
        }
      }
      if (!hits) continue;
      float lv = sat(level + 0.35f * (lit / hits) - 0.15f);
      P.put(px - P.L.x0, py - P.L.y0, packIdxF(px + 0.5f < cx ? matL : matR, lv), hits / 9.0f);
    }
}

void drawBlade(Painter &P, float bx, float by, float h, float w, float bend,
               int mat, float levelBase, float levelTip) {
  if (h < 1) return;
  int y0 = (int)floorf(by - h), y1 = (int)ceilf(by);
  for (int py = y0; py < y1; py++) {
    // vertical coverage: only the tip row is partially covered
    float top = by - h;
    if (py + 1 <= top) continue;
    float vc = sat(py + 1.0f - top);
    float t = sat((by - (py + 0.5f)) / h);
    float xc = bx + bend * t * t;
    float hw = 0.5f * w * (1.0f - t) + 0.25f;
    int x0 = (int)floorf(xc - hw - 1), x1 = (int)ceilf(xc + hw + 1);
    float lv = lerpf(levelBase, levelTip, t);
    uint8_t ix = packIdxF(mat, sat(lv));
    for (int px = x0; px <= x1; px++) {
      // horizontal coverage of [px, px+1] by [xc-hw, xc+hw]
      float a = fmaxf((float)px, xc - hw), b = fminf((float)(px + 1), xc + hw);
      float hc = b > a ? b - a : 0;
      float cov = hc * (py < top ? vc : 1.0f);
      P.put(px - P.L.x0, py - P.L.y0, ix, sat(cov));
    }
  }
}

void drawBoulder(Painter &P, float cx, float baseY, float rw, float rh, int matL, int matR,
                 uint32_t seed) {
  float cy = baseY - rh * 0.8f;
  int x0 = (int)floorf(cx - rw * 1.3f), x1 = (int)ceilf(cx + rw * 1.3f);
  int y0 = (int)floorf(cy - rh * 1.3f), y1 = (int)ceilf(baseY + 1);
  for (int py = y0; py < y1; py++)
    for (int px = x0; px < x1; px++) {
      int hits = 0;
      for (int s = 0; s < 9; s++) {
        float fx = px + ((s % 3) + 0.5f) / 3.0f, fy = py + ((s / 3) + 0.5f) / 3.0f;
        if (fy > baseY) continue;
        float dx = (fx - cx) / rw, dy = (fy - cy) / rh;
        float ang = atan2f(dy, dx);
        float rr = 1.0f + 0.13f * noise1(ang * 1.6f + 10.0f, seed);
        if (dx * dx + dy * dy < rr * rr) hits++;
      }
      if (!hits) continue;
      float dx = (px + 0.5f - cx) / rw, dy = (py + 0.5f - cy) / rh;
      float steep = sat(0.25f + 0.75f * fabsf(dx) + 0.25f * dy);
      steep = sat(steep + 0.12f * noise2(px * 0.4f, py * 0.4f, seed + 3u));
      P.put(px - P.L.x0, py - P.L.y0, packIdxF(dx < 0 ? matL : matR, steep), hits / 9.0f);
    }
}

void drawDisc(Painter &P, float cx, float cy, float r, uint8_t ix) {
  int x0 = (int)floorf(cx - r - 1), x1 = (int)ceilf(cx + r + 1);
  int y0 = (int)floorf(cy - r - 1), y1 = (int)ceilf(cy + r + 1);
  for (int py = y0; py < y1; py++)
    for (int px = x0; px < x1; px++) {
      float dx = px + 0.5f - cx, dy = py + 0.5f - cy;
      float cov = sat(r + 0.5f - sqrtf(dx * dx + dy * dy));
      if (cov > 0) P.put(px - P.L.x0, py - P.L.y0, ix, cov);
    }
}

void fillRect(Painter &P, float x0, float y0, float x1, float y1, uint8_t ix) {
  int ix0 = (int)floorf(x0), ix1 = (int)ceilf(x1);
  int iy0 = (int)floorf(y0), iy1 = (int)ceilf(y1);
  for (int py = iy0; py < iy1; py++) {
    float vc = fminf((float)(py + 1), y1) - fmaxf((float)py, y0);
    if (vc <= 0) continue;
    for (int px = ix0; px < ix1; px++) {
      float hc = fminf((float)(px + 1), x1) - fmaxf((float)px, x0);
      if (hc <= 0) continue;
      P.put(px - P.L.x0, py - P.L.y0, ix, sat(hc * vc));
    }
  }
}

}  // namespace tp

namespace tp {

void drawCapsule(Painter &P, float x0, float y0, float x1, float y1, float r, uint8_t ix) {
  int bx0 = (int)floorf(fminf(x0, x1) - r - 1), bx1 = (int)ceilf(fmaxf(x0, x1) + r + 1);
  int by0 = (int)floorf(fminf(y0, y1) - r - 1), by1 = (int)ceilf(fmaxf(y0, y1) + r + 1);
  float vx = x1 - x0, vy = y1 - y0;
  float l2 = vx * vx + vy * vy;
  for (int py = by0; py < by1; py++)
    for (int px = bx0; px < bx1; px++) {
      float wx = px + 0.5f - x0, wy = py + 0.5f - y0;
      float t = l2 > 0 ? (wx * vx + wy * vy) / l2 : 0;
      t = t < 0 ? 0 : (t > 1 ? 1 : t);
      float dx = wx - vx * t, dy = wy - vy * t;
      float cov = sat(r + 0.5f - sqrtf(dx * dx + dy * dy));
      if (cov > 0) P.put(px - P.L.x0, py - P.L.y0, ix, cov);
    }
}

static bool insidePoly(const float *xy, int n, float x, float y) {
  bool in = false;
  for (int i = 0, j = n - 1; i < n; j = i++) {
    float xi = xy[2 * i], yi = xy[2 * i + 1], xj = xy[2 * j], yj = xy[2 * j + 1];
    if (((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi) + xi)) in = !in;
  }
  return in;
}

void fillPolygon(Painter &P, const float *xy, int n, uint8_t ix, PolyShader shade, void *ctx) {
  if (n < 3) return;
  float minx = xy[0], maxx = xy[0], miny = xy[1], maxy = xy[1];
  for (int i = 1; i < n; i++) {
    minx = fminf(minx, xy[2 * i]); maxx = fmaxf(maxx, xy[2 * i]);
    miny = fminf(miny, xy[2 * i + 1]); maxy = fmaxf(maxy, xy[2 * i + 1]);
  }
  for (int py = (int)floorf(miny); py < (int)ceilf(maxy); py++)
    for (int px = (int)floorf(minx); px < (int)ceilf(maxx); px++) {
      int hits = 0;
      for (int s = 0; s < 16; s++) {
        float fx = px + ((s & 3) + 0.5f) * 0.25f, fy = py + ((s >> 2) + 0.5f) * 0.25f;
        if (insidePoly(xy, n, fx, fy)) hits++;
      }
      if (!hits) continue;
      uint8_t c = shade ? shade(ctx, px + 0.5f, py + 0.5f) : ix;
      P.put(px - P.L.x0, py - P.L.y0, c, hits / 16.0f);
    }
}

}  // namespace tp
