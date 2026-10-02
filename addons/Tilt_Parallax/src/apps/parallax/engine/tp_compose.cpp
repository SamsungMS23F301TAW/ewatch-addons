#include "tp_compose.h"
#include "tp_color.h"
#include <string.h>

namespace tp {

typedef uint32_t __attribute__((__may_alias__)) u32a;

static inline void fill16(uint16_t *d, uint16_t c, int n) {
  if (n <= 0) return;
  if (((uintptr_t)d & 2u) != 0) { *d++ = c; n--; }
  uint32_t c2 = (uint32_t)c | ((uint32_t)c << 16);
  u32a *d32 = (u32a *)d;
  int n2 = n >> 1;
  for (int i = 0; i < n2; i++) d32[i] = c2;
  if (n & 1) d[n - 1] = c;
}

bool layerRowFor(const Layer &L, int y, int &r) {
  r = y - layerScreenY(L);
  if (r < 0) {
    if (!L.clampTop) return false;
    r = 0;
  } else if (r >= L.h) {
    if (!L.clampBottom) return false;
    r = L.h - 1;
  }
  return true;
}

// Draws the spans of one layer row with its column 0 at screen x `lx`.
static void drawSpans(const Layer &L, int r, int lx, uint16_t *dst, int W) {
  const Span *s = L.spans + L.rowStart[r];
  const Span *e = L.spans + L.rowStart[r + 1];
  const size_t rowBase = (size_t)r * L.w;
  const uint8_t *irow = L.idx ? L.idx + rowBase : nullptr;
  const uint8_t *arow = L.alpha ? L.alpha + rowBase : nullptr;
  const bool isAlpha = (L.kind == LAYER_ALPHA);
  const uint16_t *lut = isAlpha ? nullptr : L.lutForRow(r);
  for (; s < e; ++s) {
    int sx0 = s->x + lx;
    if (sx0 >= W) break;                 // spans are sorted by x
    int sx1 = sx0 + s->len;
    if (sx1 <= 0) continue;
    int c0 = sx0 < 0 ? 0 : sx0;
    int c1 = sx1 > W ? W : sx1;
    int n = c1 - c0;
    int lc = c0 - lx;
    uint16_t *d = dst + c0;
    switch (s->kind) {
      case SPAN_SOLID:
        if (isAlpha) {
          if (L.opacity == 255) fill16(d, L.color, n);
          else for (int i = 0; i < n; i++) d[i] = blend565(L.color, d[i], L.opacity);
        } else {
          fill16(d, lut[s->idx], n);
        }
        break;
      case SPAN_LUT: {
        const uint8_t *src = irow + lc;
        int i = 0;
        for (; i + 4 <= n; i += 4) {
          d[i] = lut[src[i]]; d[i + 1] = lut[src[i + 1]];
          d[i + 2] = lut[src[i + 2]]; d[i + 3] = lut[src[i + 3]];
        }
        for (; i < n; i++) d[i] = lut[src[i]];
        break;
      }
      case SPAN_BLEND: {
        const uint8_t *a = arow + lc;
        if (isAlpha) {
          uint32_t op = L.opacity;
          for (int i = 0; i < n; i++) {
            uint32_t aa = (op == 255) ? a[i] : ((uint32_t)a[i] * op + 128) >> 8;
            d[i] = blend565(L.color, d[i], aa);
          }
        } else {
          const uint8_t *src = irow + lc;
          for (int i = 0; i < n; i++) d[i] = blend565(lut[src[i]], d[i], a[i]);
        }
        break;
      }
    }
  }
}

static void drawLayerRow(const Layer &L, int r, uint16_t *dst, int W) {
  int lx = layerScreenX(L);
  if (L.kind == LAYER_RGB) {
    const uint16_t *src = L.rgb + (size_t)r * L.w;
    if (!L.wrapX) {
      int c0 = lx > 0 ? lx : 0;
      int c1 = lx + L.w < W ? lx + L.w : W;
      if (c1 > c0) memcpy(dst + c0, src + (c0 - lx), (size_t)(c1 - c0) * 2);
    } else {
      int t = lx % L.w; if (t > 0) t -= L.w;
      for (; t < W; t += L.w) {
        int c0 = t > 0 ? t : 0;
        int c1 = t + L.w < W ? t + L.w : W;
        if (c1 > c0) memcpy(dst + c0, src + (c0 - t), (size_t)(c1 - c0) * 2);
      }
    }
    return;
  }
  if (!L.spans) return;
  if (!L.wrapX) {
    drawSpans(L, r, lx, dst, W);
  } else {
    int t = lx % L.w; if (t > 0) t -= L.w;
    for (; t < W; t += L.w) drawSpans(L, r, t, dst, W);
  }
}

static inline bool coversScreen(const Layer &L, int W) {
  if (L.wrapX) return true;
  int lx = layerScreenX(L);
  return lx <= 0 && lx + L.w >= W;
}

void composeRows(Layer *const *layers, int count, int y0, int y1, int W,
                 uint16_t *dst, int stride) {
  for (int y = y0; y < y1; y++) {
    uint16_t *row = dst + (size_t)(y - y0) * stride;
    int start = -1;
    for (int i = count - 1; i >= 0; --i) {
      const Layer &L = *layers[i];
      if (!L.visible || !L.rowFlags) continue;
      int r;
      if (!layerRowFor(L, y, r)) continue;
      if ((L.rowFlags[r] & ROW_OPAQUE) && coversScreen(L, W) &&
          !(L.kind == LAYER_ALPHA && L.opacity != 255)) {
        start = i;
        break;
      }
    }
    if (start < 0) { fill16(row, 0, W); start = 0; }
    for (int i = start; i < count; i++) {
      const Layer &L = *layers[i];
      if (!L.visible || !L.rowFlags) continue;
      int r;
      if (!layerRowFor(L, y, r)) continue;
      drawLayerRow(L, r, row, W);
    }
  }
}

// ---------------------------------------------------------------------------
// Reference path for tests: straightforward per-pixel evaluation.
// ---------------------------------------------------------------------------
static bool samplePixel(const Layer &L, int x, int y, uint16_t below, uint16_t &out) {
  int r;
  if (!layerRowFor(L, y, r)) return false;
  int c = x - layerScreenX(L);
  if (L.wrapX) { c %= L.w; if (c < 0) c += L.w; }
  if (c < 0 || c >= L.w) return false;
  size_t i = (size_t)r * L.w + c;
  switch (L.kind) {
    case LAYER_RGB: out = L.rgb[i]; return true;
    case LAYER_ALPHA: {
      uint32_t a = L.alpha[i];
      if (a == 0) return false;
      if (L.opacity != 255) a = (a == 255) ? L.opacity : ((a * L.opacity + 128) >> 8);
      out = (a == 255) ? L.color : blend565(L.color, below, a);
      return true;
    }
    case LAYER_INDEXED: {
      uint8_t ix = L.idx[i];
      if (ix == 0) return false;
      uint16_t col = L.lutForRow(r)[ix];
      if (!L.alpha) { out = col; return true; }
      uint8_t a = L.alpha[i];
      if (a == 0) return false;
      out = (a == 255) ? col : blend565(col, below, a);
      return true;
    }
  }
  return false;
}

void composeRowsReference(Layer *const *layers, int count, int y0, int y1,
                          int W, uint16_t *dst, int stride) {
  for (int y = y0; y < y1; y++) {
    uint16_t *row = dst + (size_t)(y - y0) * stride;
    for (int x = 0; x < W; x++) {
      uint16_t c = 0;
      for (int i = 0; i < count; i++) {
        const Layer &L = *layers[i];
        if (!L.visible) continue;
        uint16_t o;
        if (samplePixel(L, x, y, c, o)) c = o;
      }
      row[x] = c;
    }
  }
}

}  // namespace tp
