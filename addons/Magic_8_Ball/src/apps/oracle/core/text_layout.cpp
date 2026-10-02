// The Arduino build compiles everything at -Os; these per-pixel and
// per-sample loops run every frame, so they get -O2 on the watch.
#if defined(ESP_PLATFORM) && defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif
#include "text_layout.h"

#include <math.h>
#include <string.h>

namespace oracle {

namespace {

constexpr float kSqrt3 = 1.7320508f;

const GFXglyph *glyphFor(const GFXfont *f, char c) {
  uint8_t u = (uint8_t)c;
  if (u < f->first || u > f->last) u = '?';
  if (u < f->first || u > f->last) return nullptr;
  return &f->glyph[u - f->first];
}

// Horizontal ink metrics of a run of characters, in font pixels, ignoring
// tracking. left = ink start relative to the pen; right = ink end.
struct RunMetrics {
  float left = 0, right = 0;   // font px, relative to the starting pen
  float top = 0, bottom = 0;   // font px, relative to the baseline
  int   glyphs = 0;            // characters in the run (spaces included)
  bool  any = false;           // any inked glyph
};

RunMetrics measureRun(const GFXfont *f, const char *s, int len) {
  RunMetrics m;
  float pen = 0;
  for (int i = 0; i < len; i++) {
    const GFXglyph *g = glyphFor(f, s[i]);
    m.glyphs++;
    if (!g) continue;
    if (g->width && g->height) {
      float l = pen + g->xOffset;
      float r = l + g->width;
      float t = g->yOffset;
      float b = (float)g->yOffset + g->height;
      if (!m.any) { m.left = l; m.right = r; m.top = t; m.bottom = b; m.any = true; }
      else {
        if (l < m.left) m.left = l;
        if (r > m.right) m.right = r;
        if (t < m.top) m.top = t;
        if (b > m.bottom) m.bottom = b;
      }
    }
    pen += g->xAdvance;
  }
  return m;
}

// Copy, optionally uppercase, collapse runs of whitespace, trim.
void normalise(const char *in, bool upper, char *out) {
  // Whitespace runs collapse to one separator: '\n' if the run contains a
  // newline (a forced line break), otherwise ' '.
  int n = 0;
  char pending = 0;
  for (const char *p = in; p && *p && n < kMaxTextLen - 1; ++p) {
    char c = *p;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      if (n > 0 && pending != '\n') pending = (c == '\n') ? '\n' : ' ';
      continue;
    }
    if (pending) {
      if (n >= kMaxTextLen - 2) break;
      out[n++] = pending;
      pending = 0;
    }
    if (upper && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    out[n++] = c;
  }
  out[n] = '\0';
}

struct Word { uint8_t start, len; bool forced; };   // forced: '\n' before it

int splitWords(const char *s, Word *words) {
  int n = 0;
  int i = 0;
  int len = (int)strlen(s);
  while (i < len && n < kMaxWords) {
    bool forced = false;
    while (i < len && (s[i] == ' ' || s[i] == '\n')) {
      if (s[i] == '\n') forced = true;
      i++;
    }
    if (i >= len) break;
    int st = i;
    while (i < len && s[i] != ' ' && s[i] != '\n') i++;
    words[n].start = (uint8_t)st;
    words[n].len = (uint8_t)(i - st);
    words[n].forced = forced && n > 0;
    n++;
  }
  return n;
}

struct Candidate {
  bool  ok = false;
  int   lines = 0;
  int   breaks[kMaxTextLines + 1] = {0};  // word index where each line starts
  float offset = 0;
  float score = 1e9f;
};

}  // namespace

// ------------------------------------------------------------ regions

TriangleRegion::TriangleRegion(float circumR, float inset, bool pointsDown)
    : inR_(circumR * 0.5f - inset), down_(pointsDown) {
  if (inR_ < 0) inR_ = 0;
}

bool TriangleRegion::span(float y0, float y1, float &xl, float &xr) const {
  // Pointing down: top edge at y = -inR, apex at y = +2 inR.
  // Pointing up:   apex at y = -2 inR, bottom edge at y = +inR.
  float top = down_ ? -inR_ : -2.f * inR_;
  float bot = down_ ? 2.f * inR_ : inR_;
  if (y0 < top || y1 > bot || y1 < y0) return false;
  auto half = [&](float y) -> float {
    return down_ ? (2.f * inR_ - y) / kSqrt3 : (y + 2.f * inR_) / kSqrt3;
  };
  float h = half(y0);
  float h1 = half(y1);
  if (h1 < h) h = h1;
  if (h <= 0) return false;
  xl = -h;
  xr = h;
  return true;
}

float TriangleRegion::centreY() const { return 0.f; }

float TriangleRegion::slack() const { return inR_ * 0.45f; }

float TriangleRegion::edgeDistance(float x, float y) const {
  float s = down_ ? 1.f : -1.f;
  float d0 = -s * y;                               // wide edge
  float d1 = 0.8660254f * x + 0.5f * s * y;        // right slanted edge
  float d2 = -0.8660254f * x + 0.5f * s * y;       // left slanted edge
  float d = d0 > d1 ? d0 : d1;
  if (d2 > d) d = d2;
  return d - inR_;
}

bool DiscRegion::span(float y0, float y1, float &xl, float &xr) const {
  if (y1 < y0) return false;
  float a = fabsf(y0), b = fabsf(y1);
  float ym = a > b ? a : b;
  if (y0 < 0 && y1 > 0) ym = a > b ? a : b;   // band straddles the centre
  if (ym >= r_) return false;
  float h = sqrtf(r_ * r_ - ym * ym);
  xl = -h;
  xr = h;
  return true;
}

// ------------------------------------------------------------ fitting

float fontCapHeight(const GFXfont *font) {
  const GFXglyph *g = glyphFor(font, 'H');
  if (g && g->height) return (float)g->height;
  return font->yAdvance * 0.6f;
}

namespace {

struct FitContext {
  const GFXfont   *font;
  const FitRegion *region;
  const FitParams *p;
  const char      *text;
  Word             words[kMaxWords];
  int              wordCount;
  float            capFont;
  float            capTopFont;   // 'H' top relative to baseline (negative)
  int              forcedCount;  // forced line breaks in the text
};

// Ink metrics of words [a, b) joined by single spaces.
RunMetrics lineMetrics(const FitContext &c, int a, int b) {
  int st = c.words[a].start;
  int en = c.words[b - 1].start + c.words[b - 1].len;
  return measureRun(c.font, c.text + st, en - st);
}

// Try one split at one cap size; on success fill `cand` with the best offset.
bool trySplit(const FitContext &c, float cap, int lines, const int *breaks,
              Candidate &cand) {
  const FitParams &p = *c.p;
  float sy = cap / c.capFont;
  float sx = sy * p.condense;
  float pitch = cap * (1.f + p.lineGap);
  float blockH = cap + pitch * (lines - 1);

  RunMetrics m[kMaxTextLines];
  float widths[kMaxTextLines];
  float widest = 0;
  for (int k = 0; k < lines; k++) {
    m[k] = lineMetrics(c, breaks[k], breaks[k + 1]);
    widths[k] = (m[k].right - m[k].left) * sx + p.tracking * (m[k].glyphs - 1);
    if (widths[k] > widest) widest = widths[k];
  }

  float slack = c.region->slack();
  float step = cap * 0.25f;
  if (step < 0.5f) step = 0.5f;
  int steps = (int)(slack / step);
  bool found = false;
  for (int si = 0; si <= 2 * steps; si++) {
    // 0, +1, -1, +2, -2, ... (positive = toward larger y)
    int k = (si + 1) / 2;
    float off = (si & 1) ? k * step : -k * step;
    float centre = c.region->centreY() + off;
    float capTop0 = centre - blockH * 0.5f;     // top of the first cap box
    float baseline0 = capTop0 - c.capTopFont * sy;
    bool fits = true;
    float tightness = 0;                         // worst width / available
    for (int ln = 0; ln < lines && fits; ln++) {
      float base = baseline0 + pitch * ln;
      float y0 = base + m[ln].top * sy;
      float y1 = base + m[ln].bottom * sy;
      float xl, xr;
      if (!c.region->span(y0, y1, xl, xr)) { fits = false; break; }
      float avail = (xr < -xl ? xr : -xl) * 2.f;
      if (widths[ln] > avail) { fits = false; break; }
      float t = widths[ln] / avail;
      if (t > tightness) tightness = t;
    }
    if (!fits) continue;
    // Prefer centred blocks, then lines that sit comfortably.
    float score = fabsf(off) / (cap + 1.f) + tightness * 0.5f;
    if (!found || score < cand.score) {
      cand.ok = true;
      cand.lines = lines;
      for (int i = 0; i <= lines; i++) cand.breaks[i] = breaks[i];
      cand.offset = off;
      cand.score = score;
      found = true;
    }
    break;   // offsets are tried nearest-first; the first fit is the best one
  }
  (void)widest;
  return found;
}

// Enumerate every way to split `wordCount` words into `lines` lines.
bool tryLines(const FitContext &c, float cap, int lines, Candidate &best) {
  int n = c.wordCount;
  if (lines > n) return false;
  int breaks[kMaxTextLines + 1];
  breaks[0] = 0;
  breaks[lines] = n;
  // Interior breaks b1 < b2 < ... in [1, n-1].
  int idx[kMaxTextLines];
  for (int i = 1; i < lines; i++) idx[i] = i;
  bool any = false;
  for (;;) {
    for (int i = 1; i < lines; i++) breaks[i] = idx[i];
    // Every forced break must be a line start, and (when there are forced
    // breaks) lines may only start at forced breaks.
    bool legal = true;
    if (c.forcedCount > 0) {
      if (lines != c.forcedCount + 1) legal = false;
      for (int i = 1; i < lines && legal; i++)
        if (!c.words[breaks[i]].forced) legal = false;
    }
    Candidate cand;
    if (legal && trySplit(c, cap, lines, breaks, cand)) {
      if (!any || cand.score < best.score) best = cand;
      any = true;
    }
    // Next combination.
    int i = lines - 1;
    while (i >= 1 && idx[i] == n - (lines - i)) i--;
    if (i < 1) break;
    idx[i]++;
    for (int j = i + 1; j < lines; j++) idx[j] = idx[j - 1] + 1;
  }
  return any;
}

bool fitsAt(const FitContext &c, float cap, Candidate &best) {
  bool any = false;
  best = Candidate();
  int maxLines = c.p->maxLines;
  if (maxLines > kMaxTextLines) maxLines = kMaxTextLines;
  for (int lines = 1; lines <= maxLines; lines++) {
    Candidate cand;
    if (tryLines(c, cap, lines, cand)) {
      if (!any || cand.score < best.score) best = cand;
      any = true;
    }
  }
  return any;
}

void buildLayout(const FitContext &c, float cap, const Candidate &cand,
                 TextLayout &out) {
  const FitParams &p = *c.p;
  out.cap = cap;
  out.sy = cap / c.capFont;
  out.sx = out.sy * p.condense;
  out.tracking = p.tracking;
  out.lineCount = cand.lines;
  float pitch = cap * (1.f + p.lineGap);
  float blockH = cap + pitch * (cand.lines - 1);
  float centre = c.region->centreY() + cand.offset;
  float baseline0 = centre - blockH * 0.5f - c.capTopFont * out.sy;
  for (int k = 0; k < cand.lines; k++) {
    int a = cand.breaks[k], b = cand.breaks[k + 1];
    TextLine &L = out.lines[k];
    L.start = c.words[a].start;
    L.len = (uint8_t)(c.words[b - 1].start + c.words[b - 1].len - L.start);
    RunMetrics m = measureRun(c.font, c.text + L.start, L.len);
    L.inkW = (m.right - m.left) * out.sx + p.tracking * (m.glyphs - 1);
    L.x = -L.inkW * 0.5f - m.left * out.sx;
    L.baseline = baseline0 + pitch * k;
  }
}

}  // namespace

bool fitText(const GFXfont *font, const char *text, const FitRegion &region,
             const FitParams &params, TextLayout &layout) {
  layout = TextLayout();
  if (!font || !text) return false;
  normalise(text, params.uppercase, layout.text);

  FitContext c;
  c.font = font;
  c.region = &region;
  c.p = &params;
  c.text = layout.text;
  c.wordCount = splitWords(layout.text, c.words);
  c.forcedCount = 0;
  for (int i = 0; i < c.wordCount; i++) if (c.words[i].forced) c.forcedCount++;
  c.capFont = fontCapHeight(font);
  const GFXglyph *h = glyphFor(font, 'H');
  c.capTopFont = h ? (float)h->yOffset : -c.capFont;
  if (c.wordCount == 0) return false;

  Candidate best;
  float lo = params.capMin, hi = params.capMax;
  if (fitsAt(c, hi, best)) {
    buildLayout(c, hi, best, layout);
    layout.ok = true;
    return true;
  }
  Candidate atLo;
  if (!fitsAt(c, lo, atLo)) {
    // Report the failing attempt at capMin as a single line for debugging.
    Candidate dbg;
    dbg.lines = 1;
    dbg.breaks[0] = 0;
    dbg.breaks[1] = c.wordCount;
    buildLayout(c, lo, dbg, layout);
    layout.ok = false;
    return false;
  }
  best = atLo;
  float bestCap = lo;
  for (int it = 0; it < 9; it++) {
    float mid = 0.5f * (lo + hi);
    Candidate cand;
    if (fitsAt(c, mid, cand)) {
      lo = mid;
      best = cand;
      bestCap = mid;
    } else {
      hi = mid;
    }
  }
  buildLayout(c, bestCap, best, layout);
  layout.ok = true;
  return true;
}

// ------------------------------------------------------------ raster

namespace {

inline void splat(uint8_t *dst, int w, int h, int stride,
                  float x0, float y0, float sx, float sy) {
  float x1 = x0 + sx, y1 = y0 + sy;
  int ix0 = (int)floorf(x0), iy0 = (int)floorf(y0);
  int ix1 = (int)ceilf(x1) - 1, iy1 = (int)ceilf(y1) - 1;
  for (int iy = iy0; iy <= iy1; iy++) {
    if (iy < 0 || iy >= h) continue;
    float oy = (y1 < iy + 1 ? y1 : iy + 1) - (y0 > iy ? y0 : iy);
    if (oy <= 0) continue;
    uint8_t *row = dst + iy * stride;
    for (int ix = ix0; ix <= ix1; ix++) {
      if (ix < 0 || ix >= w) continue;
      float ox = (x1 < ix + 1 ? x1 : ix + 1) - (x0 > ix ? x0 : ix);
      if (ox <= 0) continue;
      int a = (int)(ox * oy * 255.f + 0.5f);
      int v = row[ix] + a;
      row[ix] = (uint8_t)(v > 255 ? 255 : v);
    }
  }
}

}  // namespace

void rasterizeText(const GFXfont *font, const TextLayout &L, uint8_t *dst,
                   int w, int h, int stride, float originX, float originY) {
  if (!font || !dst || L.lineCount <= 0) return;
  for (int k = 0; k < L.lineCount; k++) {
    const TextLine &line = L.lines[k];
    float pen = line.x;
    for (int i = 0; i < line.len; i++) {
      char ch = L.text[line.start + i];
      const GFXglyph *g = glyphFor(font, ch);
      if (!g) continue;
      if (ch != ' ' && g->width && g->height) {
        const uint8_t *bits = font->bitmap + g->bitmapOffset;
        uint8_t byte = 0;
        int bit = 0;
        for (int gy = 0; gy < g->height; gy++) {
          float y = originY + line.baseline + (g->yOffset + gy) * L.sy;
          for (int gx = 0; gx < g->width; gx++) {
            if (!(bit & 7)) byte = bits[bit >> 3];
            bit++;
            if (byte & 0x80) {
              float x = originX + pen + (g->xOffset + gx) * L.sx;
              splat(dst, w, h, stride, x, y, L.sx, L.sy);
            }
            byte <<= 1;
          }
        }
      }
      pen += g->xAdvance * L.sx + L.tracking;
    }
  }
}

void layoutInkBounds(const GFXfont *font, const TextLayout &L,
                     float &x0, float &y0, float &x1, float &y1) {
  bool any = false;
  x0 = y0 = x1 = y1 = 0;
  for (int k = 0; k < L.lineCount; k++) {
    const TextLine &line = L.lines[k];
    float pen = line.x;
    for (int i = 0; i < line.len; i++) {
      char ch = L.text[line.start + i];
      const GFXglyph *g = glyphFor(font, ch);
      if (!g) continue;
      if (ch != ' ' && g->width && g->height) {
        float gl = pen + g->xOffset * L.sx;
        float gr = gl + g->width * L.sx;
        float gt = line.baseline + g->yOffset * L.sy;
        float gb = gt + g->height * L.sy;
        if (!any) { x0 = gl; x1 = gr; y0 = gt; y1 = gb; any = true; }
        else {
          if (gl < x0) x0 = gl;
          if (gr > x1) x1 = gr;
          if (gt < y0) y0 = gt;
          if (gb > y1) y1 = gb;
        }
      }
      pen += g->xAdvance * L.sx + L.tracking;
    }
  }
}

}  // namespace oracle
