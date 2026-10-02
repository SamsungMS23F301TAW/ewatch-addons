// px — software renderer + original pixel fonts. See px.h.
#include "px.h"
#include <string.h>

namespace px {

uint16_t mix(uint16_t a, uint16_t b, int t) {
  if (t <= 0) return a;
  if (t >= 256) return b;
  int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
  int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
  int r = ar + (((br - ar) * t) >> 8);
  int g = ag + (((bg - ag) * t) >> 8);
  int bl = ab + (((bb - ab) * t) >> 8);
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

void Canvas::clip(int x0, int y0, int x1, int y1) {
  cx0 = (int16_t)(x0 < 0 ? 0 : x0);
  cy0 = (int16_t)(y0 < 0 ? 0 : y0);
  cx1 = (int16_t)(x1 > w ? w : x1);
  cy1 = (int16_t)(y1 > h ? h : y1);
}

void fill(Canvas &c, uint16_t col) { rect(c, c.cx0, c.cy0, c.cx1 - c.cx0, c.cy1 - c.cy0, col); }

void rect(Canvas &c, int x, int y, int w, int h, uint16_t col) {
  int x0 = x < c.cx0 ? c.cx0 : x;
  int y0 = y < c.cy0 ? c.cy0 : y;
  int x1 = x + w > c.cx1 ? c.cx1 : x + w;
  int y1 = y + h > c.cy1 ? c.cy1 : y + h;
  if (x1 <= x0 || y1 <= y0) return;
  for (int yy = y0; yy < y1; yy++) {
    uint16_t *p = c.buf + yy * c.w + x0;
    for (int xx = x0; xx < x1; xx++) *p++ = col;
  }
}

void frame(Canvas &c, int x, int y, int w, int h, uint16_t col) {
  rect(c, x, y, w, 1, col);
  rect(c, x, y + h - 1, w, 1, col);
  rect(c, x, y, 1, h, col);
  rect(c, x + w - 1, y, 1, h, col);
}

void roundRect(Canvas &c, int x, int y, int w, int h, int r, uint16_t col) {
  if (r < 0) r = 0;
  if (r > 3) r = 3;
  // cut pattern per corner row: r=1 -> {1}, r=2 -> {2,1}, r=3 -> {3,1,1}
  static const uint8_t cuts[4][3] = {{0, 0, 0}, {1, 0, 0}, {2, 1, 0}, {3, 1, 1}};
  for (int row = 0; row < h; row++) {
    int inset = 0;
    if (row < 3) inset = cuts[r][row];
    if (h - 1 - row < 3) {
      int i2 = cuts[r][h - 1 - row];
      if (i2 > inset) inset = i2;
    }
    rect(c, x + inset, y + row, w - 2 * inset, 1, col);
  }
}

void dither(Canvas &c, int x, int y, int w, int h, uint16_t a, uint16_t b, int level) {
  static const uint8_t bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
  int x0 = x < c.cx0 ? c.cx0 : x;
  int y0 = y < c.cy0 ? c.cy0 : y;
  int x1 = x + w > c.cx1 ? c.cx1 : x + w;
  int y1 = y + h > c.cy1 ? c.cy1 : y + h;
  for (int yy = y0; yy < y1; yy++) {
    uint16_t *p = c.buf + yy * c.w + x0;
    for (int xx = x0; xx < x1; xx++) {
      *p++ = (bayer[yy & 3][xx & 3] < level) ? b : a;
    }
  }
}

static inline void blitImpl(Canvas &c, const uint8_t *idx, int sw, int sh, int x, int y,
                            int scale, const uint16_t *pal, bool flipX, bool solid,
                            uint16_t solidCol) {
  if (scale < 1) scale = 1;
  for (int sy = 0; sy < sh; sy++) {
    int py = y + sy * scale;
    if (py + scale <= c.cy0 || py >= c.cy1) continue;
    const uint8_t *row = idx + sy * sw;
    for (int sx = 0; sx < sw; sx++) {
      uint8_t v = row[flipX ? (sw - 1 - sx) : sx];
      if (!v) continue;
      rect(c, x + sx * scale, py, scale, scale, solid ? solidCol : pal[v]);
    }
  }
}

void blit(Canvas &c, const uint8_t *idx, int sw, int sh, int x, int y, int scale,
          const uint16_t *pal, bool flipX) {
  blitImpl(c, idx, sw, sh, x, y, scale, pal, flipX, false, 0);
}

void blitSolid(Canvas &c, const uint8_t *idx, int sw, int sh, int x, int y, int scale,
               uint16_t solid) {
  blitImpl(c, idx, sw, sh, x, y, scale, nullptr, false, true, solid);
}

// ---------------------------------------------------------------------------
// Fonts. Both are original designs, stored as row strings ('#' = ink).
// ---------------------------------------------------------------------------
struct Glyph { char ch; const char *rows; };

// 5x7 — friendly rounded caps, digits and punctuation.
static const Glyph kSmall[] = {
  {'A', ".###." "#...#" "#...#" "#####" "#...#" "#...#" "#...#"},
  {'B', "####." "#...#" "#...#" "####." "#...#" "#...#" "####."},
  {'C', ".###." "#...#" "#...." "#...." "#...." "#...#" ".###."},
  {'D', "####." "#...#" "#...#" "#...#" "#...#" "#...#" "####."},
  {'E', "#####" "#...." "#...." "####." "#...." "#...." "#####"},
  {'F', "#####" "#...." "#...." "####." "#...." "#...." "#...."},
  {'G', ".###." "#...#" "#...." "#.###" "#...#" "#...#" ".####"},
  {'H', "#...#" "#...#" "#...#" "#####" "#...#" "#...#" "#...#"},
  {'I', ".###." "..#.." "..#.." "..#.." "..#.." "..#.." ".###."},
  {'J', "..###" "...#." "...#." "...#." "#..#." "#..#." ".##.."},
  {'K', "#...#" "#..#." "#.#.." "##..." "#.#.." "#..#." "#...#"},
  {'L', "#...." "#...." "#...." "#...." "#...." "#...." "#####"},
  {'M', "#...#" "##.##" "#.#.#" "#.#.#" "#...#" "#...#" "#...#"},
  {'N', "#...#" "##..#" "#.#.#" "#..##" "#...#" "#...#" "#...#"},
  {'O', ".###." "#...#" "#...#" "#...#" "#...#" "#...#" ".###."},
  {'P', "####." "#...#" "#...#" "####." "#...." "#...." "#...."},
  {'Q', ".###." "#...#" "#...#" "#...#" "#.#.#" "#..#." ".##.#"},
  {'R', "####." "#...#" "#...#" "####." "#.#.." "#..#." "#...#"},
  {'S', ".####" "#...." "#...." ".###." "....#" "....#" "####."},
  {'T', "#####" "..#.." "..#.." "..#.." "..#.." "..#.." "..#.."},
  {'U', "#...#" "#...#" "#...#" "#...#" "#...#" "#...#" ".###."},
  {'V', "#...#" "#...#" "#...#" "#...#" "#...#" ".#.#." "..#.."},
  {'W', "#...#" "#...#" "#...#" "#.#.#" "#.#.#" "#.#.#" ".#.#."},
  {'X', "#...#" "#...#" ".#.#." "..#.." ".#.#." "#...#" "#...#"},
  {'Y', "#...#" "#...#" ".#.#." "..#.." "..#.." "..#.." "..#.."},
  {'Z', "#####" "....#" "...#." "..#.." ".#..." "#...." "#####"},
  {'0', ".###." "#...#" "#..##" "#.#.#" "##..#" "#...#" ".###."},
  {'1', "..#.." ".##.." "..#.." "..#.." "..#.." "..#.." ".###."},
  {'2', ".###." "#...#" "....#" "...#." "..#.." ".#..." "#####"},
  {'3', "####." "....#" "....#" ".###." "....#" "....#" "####."},
  {'4', "...#." "..##." ".#.#." "#..#." "#####" "...#." "...#."},
  {'5', "#####" "#...." "####." "....#" "....#" "#...#" ".###."},
  {'6', "..##." ".#..." "#...." "####." "#...#" "#...#" ".###."},
  {'7', "#####" "....#" "...#." "..#.." ".#..." ".#..." ".#..."},
  {'8', ".###." "#...#" "#...#" ".###." "#...#" "#...#" ".###."},
  {'9', ".###." "#...#" "#...#" ".####" "....#" "...#." ".##.."},
  {' ', "....." "....." "....." "....." "....." "....." "....."},
  {'.', "....." "....." "....." "....." "....." ".##.." ".##.."},
  {',', "....." "....." "....." "....." ".##.." "..#.." ".#..."},
  {':', "....." ".##.." ".##.." "....." ".##.." ".##.." "....."},
  {'!', "..#.." "..#.." "..#.." "..#.." "..#.." "....." "..#.."},
  {'?', ".###." "#...#" "....#" "..##." "..#.." "....." "..#.."},
  {'\'', "..#.." "..#.." ".#..." "....." "....." "....." "....."},
  {'-', "....." "....." "....." ".###." "....." "....." "....."},
  {'+', "....." "..#.." "..#.." "#####" "..#.." "..#.." "....."},
  {'/', "....#" "....#" "...#." "..#.." ".#..." "#...." "#...."},
  {'%', "##..#" "##..#" "...#." "..#.." ".#..." "#..##" "#..##"},
  {'(', "...#." "..#.." ".#..." ".#..." ".#..." "..#.." "...#."},
  {')', ".#..." "..#.." "...#." "...#." "...#." "..#.." ".#..."},
  {'<', "...#." "..#.." ".#..." "#...." ".#..." "..#.." "...#."},
  {'>', ".#..." "..#.." "...#." "....#" "...#." "..#.." ".#..."},
  {'=', "....." "....." "#####" "....." "#####" "....." "....."},
  {'x', "....." "....." "#...#" ".#.#." "..#.." ".#.#." "#...#"},
  {'*', "....." "#.#.#" ".###." "#####" ".###." "#.#.#" "....."},
  // Icons mapped onto rarely used ASCII codes:
  //  '^' heart, '~' footprint, '@' snack (berry), '#' flame, '$' star,
  //  '&' moon, '{' left arrow, '}' right arrow, '|' up arrow, '`' down arrow
  {'^', "....." ".#.#." "#####" "#####" ".###." "..#.." "....."},
  {'~', ".##...." "###...." "###...." ".#..##." "...###." "...###." "....#.."},
  {'@', "...#." "..#.." ".###." "##.##" "#####" "#####" ".###."},
  {'#', "..#.." ".##.." ".###." "##.##" "#...#" "#...#" ".###."},
  {'$', "..#.." "..#.." "#####" ".###." ".###." "##.##" "#...#"},
  {'&', ".###." "##..." "#...." "#...." "#...." "##..." ".###."},
  {'{', "..#.." ".##.." "#####" ".##.." "..#.." "....." "....."},
  {'}', "..#.." "..##." "#####" "..##." "..#.." "....." "....."},
  {'|', "..#.." ".###." "#####" "..#.." "..#.." "..#.." "....."},
  {'`', "..#.." "..#.." "..#.." "#####" ".###." "..#.." "....."},
};

// 6x9 — chunky clock digits with 2-px strokes, readable at a glance.
static const Glyph kBig[] = {
  {'0', ".####." "##..##" "##..##" "##..##" "##..##" "##..##" "##..##" "##..##" ".####."},
  {'1', "..##.." ".###.." "####.." "..##.." "..##.." "..##.." "..##.." "..##.." "######"},
  {'2', ".####." "##..##" "....##" "....##" "...##." "..##.." ".##..." "##...." "######"},
  {'3', ".####." "##..##" "....##" "....##" "..###." "....##" "....##" "##..##" ".####."},
  {'4', "...##." "..###." ".####." "##.##." "##.##." "######" "...##." "...##." "...##."},
  {'5', "######" "##...." "##...." "#####." "....##" "....##" "....##" "##..##" ".####."},
  {'6', "..###." ".##..." "##...." "#####." "##..##" "##..##" "##..##" "##..##" ".####."},
  {'7', "######" "....##" "....##" "...##." "...##." "..##.." "..##.." ".##..." ".##..."},
  {'8', ".####." "##..##" "##..##" "##..##" ".####." "##..##" "##..##" "##..##" ".####."},
  {'9', ".####." "##..##" "##..##" "##..##" ".#####" "....##" "....##" "...##." ".###.."},
  {':', ".." ".." "##" "##" ".." ".." "##" "##" ".."},
  {' ', "......" "......" "......" "......" "......" "......" "......" "......" "......"},
  {'-', "......" "......" "......" "......" ".####." "......" "......" "......" "......"},
};

static const Glyph *findGlyph(Font f, char ch, int &gw, int &gh) {
  const Glyph *tab = (f == Font::Big) ? kBig : kSmall;
  const int n = (f == Font::Big) ? (int)(sizeof(kBig) / sizeof(kBig[0]))
                                 : (int)(sizeof(kSmall) / sizeof(kSmall[0]));
  gh = (f == Font::Big) ? 9 : 7;
  if (f == Font::Small && ch >= 'a' && ch <= 'z' && ch != 'x') ch = (char)(ch - 32);
  for (int i = 0; i < n; i++) {
    if (tab[i].ch == ch) {
      gw = (int)strlen(tab[i].rows) / gh;
      return &tab[i];
    }
  }
  gw = (f == Font::Big) ? 6 : 5;
  return nullptr;   // unknown: blank advance
}

int glyphHeight(Font f) { return f == Font::Big ? 9 : 7; }

static int drawGlyph(Canvas &c, Font f, char ch, int x, int y, int scale, uint16_t col, bool draw) {
  int gw, gh;
  const Glyph *g = findGlyph(f, ch, gw, gh);
  if (g && draw) {
    for (int r = 0; r < gh; r++) {
      const char *row = g->rows + r * gw;
      int run = -1;
      for (int k = 0; k <= gw; k++) {
        bool on = k < gw && row[k] == '#';
        if (on && run < 0) run = k;
        if (!on && run >= 0) {
          rect(c, x + run * scale, y + r * scale, (k - run) * scale, scale, col);
          run = -1;
        }
      }
    }
  }
  return gw;
}

int text(Canvas &c, const char *s, int x, int y, int scale, uint16_t col, Font f, int spacing) {
  int cx = x;
  for (; *s; s++) {
    int gw = drawGlyph(c, f, *s, cx, y, scale, col, true);
    cx += (gw + spacing) * scale;
  }
  return cx - x - (cx > x ? spacing * scale : 0);
}

int textWidth(const char *s, int scale, Font f, int spacing) {
  int w = 0;
  bool any = false;
  for (; *s; s++) {
    int gw, gh;
    findGlyph(f, *s, gw, gh);
    w += (gw + spacing) * scale;
    any = true;
  }
  return any ? w - spacing * scale : 0;
}

int textC(Canvas &c, const char *s, int cx, int y, int scale, uint16_t col, Font f, int spacing) {
  int x = cx - textWidth(s, scale, f, spacing) / 2;
  text(c, s, x, y, scale, col, f, spacing);
  return x;
}

int textShadow(Canvas &c, const char *s, int x, int y, int scale, uint16_t col, uint16_t shadow,
               Font f, int spacing) {
  text(c, s, x + scale, y + scale, scale, shadow, f, spacing);
  return text(c, s, x, y, scale, col, f, spacing);
}

int fmtThousands(char *out, int cap, uint32_t v) {
  char tmp[16];
  int n = 0;
  do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 15);
  int len = n + (n - 1) / 3;
  if (len >= cap) { if (cap > 0) out[0] = 0; return 0; }
  int o = 0;
  for (int i = n - 1; i >= 0; i--) {
    out[o++] = tmp[i];
    if (i > 0 && i % 3 == 0) out[o++] = ',';
  }
  out[o] = 0;
  return o;
}

}  // namespace px
