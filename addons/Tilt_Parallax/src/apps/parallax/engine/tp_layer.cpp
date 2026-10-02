#include "tp_layer.h"
#include "tp_platform.h"
#include <string.h>

namespace tp {

// Opaque runs shorter than this are copied through the LUT rather than
// filled; switching span types costs more than a few lookups.
static const int kMinSolid = 8;

bool Layer::allocIndexed(int16_t w_, int16_t h_, bool withAlpha, uint8_t bands) {
  release();
  kind = LAYER_INDEXED;
  w = w_; h = h_;
  nBands = bands < 1 ? 1 : bands;
  size_t n = (size_t)w * h;
  idx = (uint8_t *)allocBig(n);
  if (withAlpha) alpha = (uint8_t *)allocBig(n);
  luts = (uint16_t *)allocBig((size_t)nBands * 256 * sizeof(uint16_t));
  if (!idx || (withAlpha && !alpha) || !luts) { release(); return false; }
  memset(idx, 0, n);
  if (alpha) memset(alpha, 0, n);
  memset(luts, 0, (size_t)nBands * 256 * sizeof(uint16_t));
  return true;
}

bool Layer::allocRGB(int16_t w_, int16_t h_) {
  release();
  kind = LAYER_RGB;
  w = w_; h = h_;
  rgb = (uint16_t *)allocBig((size_t)w * h * sizeof(uint16_t));
  if (!rgb) { release(); return false; }
  return true;
}

bool Layer::allocAlpha(int16_t w_, int16_t h_) {
  release();
  kind = LAYER_ALPHA;
  w = w_; h = h_;
  alpha = (uint8_t *)allocBig((size_t)w * h);
  if (!alpha) { release(); return false; }
  memset(alpha, 0, (size_t)w * h);
  return true;
}

void Layer::release() {
  if (idx) freeMem(idx);
  if (alpha) freeMem(alpha);
  if (rgb) freeMem(rgb);
  if (spans) freeMem(spans);
  if (rowStart) freeMem(rowStart);
  if (rowFlags) freeMem(rowFlags);
  if (luts) freeMem(luts);
  idx = nullptr; alpha = nullptr; rgb = nullptr; spans = nullptr;
  rowStart = nullptr; rowFlags = nullptr; luts = nullptr;
  spanCount = 0;
}

size_t Layer::bytes() const {
  size_t n = (size_t)w * h;
  size_t b = 0;
  if (idx) b += n;
  if (alpha) b += n;
  if (rgb) b += n * 2;
  if (spans) b += spanCount * sizeof(Span);
  if (rowStart) b += ((size_t)h + 1) * sizeof(uint32_t);
  if (rowFlags) b += h;
  if (luts) b += (size_t)nBands * 256 * sizeof(uint16_t);
  return b;
}

// Pixel class: 0 transparent, 1 partial, 2 opaque.
static inline uint8_t classOf(const Layer &L, size_t i) {
  if (L.alpha) {
    uint8_t a = L.alpha[i];
    if (L.kind == LAYER_INDEXED && L.idx[i] == 0) return 0;  // no colour = hole
    return a == 0 ? 0 : (a == 255 ? 2 : 1);
  }
  return L.idx[i] ? 2 : 0;
}

// Encodes one row. When `out` is null only counts. Returns span count and
// sets *opaque when the whole row is opaque.
static uint32_t encodeRow(const Layer &L, int r, Span *out, bool *opaque) {
  const size_t base = (size_t)r * L.w;
  uint32_t n = 0;
  bool allOpaque = true;
  int x = 0;
  auto emit = [&](int sx, int len, uint8_t kind, uint8_t id) {
    if (out) { out[n].x = (int16_t)sx; out[n].len = (int16_t)len;
               out[n].kind = kind; out[n].idx = id; out[n].pad = 0; }
    n++;
  };
  while (x < L.w) {
    uint8_t c = classOf(L, base + x);
    int e = x + 1;
    while (e < L.w && classOf(L, base + e) == c) e++;
    if (c != 2) allOpaque = false;
    if (c == 1) {
      emit(x, e - x, SPAN_BLEND, 0);
    } else if (c == 2) {
      if (L.kind == LAYER_ALPHA) {
        emit(x, e - x, SPAN_SOLID, 0);
      } else {
        // Split the opaque run into long solid stretches and LUT stretches.
        int runStart = x;      // start of the pending LUT stretch
        int p = x;
        while (p < e) {
          uint8_t v = L.idx[base + p];
          int q = p + 1;
          while (q < e && L.idx[base + q] == v) q++;
          if (q - p >= kMinSolid) {
            if (p > runStart) emit(runStart, p - runStart, SPAN_LUT, 0);
            emit(p, q - p, SPAN_SOLID, v);
            runStart = q;
          }
          p = q;
        }
        if (e > runStart) emit(runStart, e - runStart, SPAN_LUT, 0);
      }
    }
    x = e;
  }
  if (opaque) *opaque = allOpaque;
  return n;
}

bool Layer::encodeSpans() {
  if (spans) { freeMem(spans); spans = nullptr; }
  if (rowStart) { freeMem(rowStart); rowStart = nullptr; }
  if (rowFlags) { freeMem(rowFlags); rowFlags = nullptr; }
  spanCount = 0;
  rowFlags = (uint8_t *)allocBig((size_t)h);
  if (!rowFlags) { visible = false; return false; }
  if (kind == LAYER_RGB) {
    memset(rowFlags, ROW_OPAQUE, (size_t)h);
    return true;
  }
  rowStart = (uint32_t *)allocBig(((size_t)h + 1) * sizeof(uint32_t));
  if (!rowStart) { visible = false; return false; }
  uint32_t total = 0;
  for (int r = 0; r < h; r++) {
    rowStart[r] = total;
    total += encodeRow(*this, r, nullptr, nullptr);
  }
  rowStart[h] = total;
  spans = (Span *)allocBig((size_t)(total ? total : 1) * sizeof(Span));
  if (!spans) { visible = false; return false; }
  for (int r = 0; r < h; r++) {
    bool op = false;
    encodeRow(*this, r, spans + rowStart[r], &op);
    rowFlags[r] = op ? ROW_OPAQUE : 0;
  }
  spanCount = total;
  return true;
}

}  // namespace tp
