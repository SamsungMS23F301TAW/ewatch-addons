#include "gf_font.h"
#include "gf_core.h"

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

namespace gf {

namespace {
enum : int16_t { E = 0, M = 1, L = 2, A = 3, D = 4, W = 5 };

// Glyph programs. Grid: cap height 100 (y = 0 top, 100 baseline).
//   M x y            start a stroke
//   L x y            line to
//   A cx cy rx ry a0 a1   elliptical arc, degrees, 0 = right, 90 = down;
//                    a1 > a0 runs clockwise on screen, a1 < a0 anticlockwise
//   D x y            dot
//   W k              stroke width multiplier for what follows, in 1/16ths
#define G(name) static const int16_t name[]
G(g0) = { A, 30, 50, 30, 50, 0, 360, E };
G(g1) = { M, 16, 20, L, 38, 0, L, 38, 100, E };
G(g2) = { A, 29, 29, 28, 29, 200, 400, L, 2, 100, L, 58, 100, E };
G(g3) = { A, 28, 26, 25, 24, 205, 450, A, 28, 75, 27, 25, 270, 520, E };
G(g4) = { M, 44, 100, L, 44, 0, L, 0, 70, L, 60, 70, E };
G(g5) = { M, 52, 0, L, 9, 0, L, 5, 51, A, 29, 69, 28, 31, 215, 495, E };
G(g6) = { M, 47, 0, L, 5, 58, A, 30, 72, 28, 28, 210, 570, E };
G(g7) = { M, 0, 0, L, 58, 0, L, 18, 100, E };
G(g8) = { A, 30, 25, 23, 23, 90, 450, A, 30, 73, 27, 27, 270, 630, E };
G(g9) = { A, 30, 28, 28, 28, 30, 390, L, 13, 100, E };
G(gColon) = { D, 6, 32, D, 6, 76, E };
G(gA) = { M, 0, 100, L, 31, 0, L, 62, 100, M, 12, 64, L, 50, 64, E };
G(gB) = { M, 0, 48, L, 30, 48, A, 30, 74, 26, 26, 270, 450, L, 0, 100, L, 0, 0,
          L, 27, 0, A, 27, 24, 24, 24, 270, 450, L, 0, 48, E };
G(gC) = { A, 34, 50, 34, 50, 318, 42, E };
G(gD) = { M, 0, 0, L, 0, 100, L, 22, 100, A, 22, 50, 36, 50, 90, -90, L, 0, 0, E };
G(gE) = { M, 54, 0, L, 0, 0, L, 0, 100, L, 54, 100, M, 0, 50, L, 44, 50, E };
G(gF) = { M, 54, 0, L, 0, 0, L, 0, 100, M, 0, 50, L, 44, 50, E };
G(gG) = { A, 35, 50, 35, 50, 318, 8, L, 70, 56, L, 42, 56, E };
G(gH) = { M, 0, 0, L, 0, 100, M, 58, 0, L, 58, 100, M, 0, 50, L, 58, 50, E };
G(gI) = { M, 0, 0, L, 0, 100, E };
G(gJ) = { M, 42, 0, L, 42, 70, A, 21, 70, 21, 30, 0, 180, E };
G(gK) = { M, 0, 0, L, 0, 100, M, 54, 0, L, 0, 62, M, 18, 45, L, 58, 100, E };
G(gL) = { M, 0, 0, L, 0, 100, L, 48, 100, E };
G(gM) = { M, 0, 100, L, 0, 0, L, 35, 66, L, 70, 0, L, 70, 100, E };
G(gN) = { M, 0, 100, L, 0, 0, L, 58, 100, L, 58, 0, E };
G(gO) = { A, 37, 50, 37, 50, 0, 360, E };
G(gP) = { M, 0, 100, L, 0, 0, L, 27, 0, A, 27, 26, 26, 26, 270, 450, L, 0, 52, E };
G(gQ) = { A, 37, 50, 37, 50, 0, 360, M, 46, 68, L, 76, 104, E };
G(gR) = { M, 0, 100, L, 0, 0, L, 27, 0, A, 27, 26, 26, 26, 270, 450, L, 0, 52,
          M, 22, 52, L, 56, 100, E };
G(gS) = { A, 29, 26, 25, 24, 330, 90, A, 29, 75, 27, 25, 270, 510, E };
G(gT) = { M, 0, 0, L, 62, 0, M, 31, 0, L, 31, 100, E };
G(gU) = { M, 0, 0, L, 0, 68, A, 29, 68, 29, 32, 180, 0, L, 58, 0, E };
G(gV) = { M, 0, 0, L, 31, 100, L, 62, 0, E };
G(gW) = { M, 0, 0, L, 21, 100, L, 43, 28, L, 65, 100, L, 86, 0, E };
G(gX) = { M, 0, 0, L, 58, 100, M, 58, 0, L, 0, 100, E };
G(gY) = { M, 0, 0, L, 31, 52, L, 62, 0, M, 31, 52, L, 31, 100, E };
G(gZ) = { M, 2, 0, L, 58, 0, L, 0, 100, L, 58, 100, E };
G(gDot) = { D, 4, 96, E };
G(gComma) = { M, 7, 90, L, 1, 110, E };
G(gDash) = { M, 0, 58, L, 36, 58, E };
G(gSlash) = { M, 42, 0, L, 0, 100, E };
G(gPlus) = { M, 0, 56, L, 44, 56, M, 22, 34, L, 22, 78, E };
G(gHash) = { M, 16, 18, L, 10, 86, M, 40, 18, L, 34, 86, M, 2, 40, L, 48, 40,
             M, 0, 64, L, 46, 64, E };
G(gApos) = { M, 3, 0, L, 3, 26, E };
G(gNumero) = { W, 12, A, 11, 30, 11, 15, 0, 360, M, 0, 62, L, 22, 62, E };
G(gSteps) = { W, 46, M, 12, 44, L, 14, 66, W, 34, D, 15, 92,
              W, 46, M, 44, 10, L, 46, 32, W, 34, D, 47, 58, E };
G(gSun) = { A, 40, 54, 17, 17, 0, 360, M, 40, 4, L, 40, 18, M, 40, 90, L, 40, 104,
            M, 0, 54, L, 14, 54, M, 66, 54, L, 80, 54, M, 12, 26, L, 21, 35,
            M, 59, 73, L, 68, 82, M, 68, 26, L, 59, 35, M, 21, 73, L, 12, 82, E };
G(gSparkle) = { M, 30, 0, L, 30, 100, M, 0, 50, L, 60, 50, W, 10, M, 10, 20,
                L, 50, 80, M, 50, 20, L, 10, 80, E };
G(gBattery) = { M, 0, 30, L, 70, 30, L, 70, 90, L, 0, 90, L, 0, 30, M, 80, 48,
                L, 80, 72, W, 40, M, 14, 60, L, 18, 60, E };
G(gMidDot) = { D, 8, 56, E };
#undef G

struct Glyph { char c; int16_t adv; const int16_t *prog; };
static const Glyph kGlyphs[] = {
  { '0', 60, g0 }, { '1', 60, g1 }, { '2', 60, g2 }, { '3', 60, g3 }, { '4', 60, g4 },
  { '5', 60, g5 }, { '6', 60, g6 }, { '7', 60, g7 }, { '8', 60, g8 }, { '9', 60, g9 },
  { ':', 12, gColon },
  { 'A', 62, gA }, { 'B', 56, gB }, { 'C', 68, gC }, { 'D', 58, gD }, { 'E', 54, gE },
  { 'F', 52, gF }, { 'G', 70, gG }, { 'H', 58, gH }, { 'I', 0, gI }, { 'J', 42, gJ },
  { 'K', 58, gK }, { 'L', 48, gL }, { 'M', 70, gM }, { 'N', 58, gN }, { 'O', 74, gO },
  { 'P', 53, gP }, { 'Q', 76, gQ }, { 'R', 56, gR }, { 'S', 58, gS }, { 'T', 62, gT },
  { 'U', 58, gU }, { 'V', 62, gV }, { 'W', 86, gW }, { 'X', 58, gX }, { 'Y', 62, gY },
  { 'Z', 58, gZ },
  { '.', 8, gDot }, { ',', 10, gComma }, { '-', 36, gDash }, { '/', 42, gSlash },
  { '+', 44, gPlus }, { '#', 48, gHash }, { '\'', 6, gApos },
  { '\x01', 24, gNumero }, { '\x02', 46, gSteps }, { '\x03', 80, gSun },
  { '\x04', 60, gSparkle }, { '\x05', 82, gBattery }, { '\x06', 16, gMidDot },
};
static const int32_t kSpaceAdv = 34;
static const int32_t kGap = 22;        // default spacing between glyphs, font units

const Glyph *findGlyph(char c) {
  if (c >= 'a' && c <= 'z') c = (char)(c - 32);
  for (const Glyph &g : kGlyphs) if (g.c == c) return &g;
  return nullptr;
}

// Font units (Q8) to pixels (Q8).
inline int32_t fu(int32_t q8units, int32_t capQ8) {
  return (int32_t)(((int64_t)q8units * capQ8) / (100 * 256));
}
}  // namespace

void maskSegment(Mask &m, int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t hw,
                 int32_t alpha) {
  int32_t dx = bx - ax, dy = by - ay;
  int64_t len2 = (int64_t)dx * dx + (int64_t)dy * dy;
  int32_t pad = hw + 384;
  int32_t x0 = imax(floorShift(imin(ax, bx) - pad, 8), 0);
  int32_t x1 = imin((imax(ax, bx) + pad) >> 8, m.w - 1);
  int32_t y0 = imax(floorShift(imin(ay, by) - pad, 8), 0);
  int32_t y1 = imin((imax(ay, by) + pad) >> 8, m.h - 1);
  int32_t peak = imin(2 * hw, 256);
  int32_t len = imax((int32_t)isqrt64((uint64_t)len2), 1);
  for (int32_t y = y0; y <= y1; y++) {
    int64_t py = y * 256 + 128 - ay;
    uint8_t *row = m.px + y * m.w;
    for (int32_t x = x0; x <= x1; x++) {
      int64_t px = x * 256 + 128 - ax;
      // Distance to the segment (clamped projection), Q8.
      int64_t t = len2 > 0 ? px * dx + py * dy : 0;
      int64_t qx, qy;
      if (t <= 0)          { qx = px; qy = py; }
      else if (t >= len2)  { qx = px - dx; qy = py - dy; }
      else {
        int64_t nn = px * dy - py * dx;                     // cross, Q16 * len
        int32_t d = (int32_t)((nn < 0 ? -nn : nn) / len);
        int32_t cov = (imin(peak, hw + 128 - d) * alpha) >> 8;
        if (cov > 0 && cov > row[x]) row[x] = (uint8_t)imin(cov, 255);
        continue;
      }
      int32_t d = (int32_t)isqrt64((uint64_t)(qx * qx + qy * qy));
      int32_t cov = (imin(peak, hw + 128 - d) * alpha) >> 8;
      if (cov > 0 && cov > row[x]) row[x] = (uint8_t)imin(cov, 255);
    }
  }
}

int32_t textWidth(const char *s, int32_t capQ8, int32_t tracking) {
  int32_t units = 0;
  bool first = true;
  for (const char *p = s; *p; p++) {
    if (!first) units += kGap + tracking;
    first = false;
    if (*p == ' ') { units += kSpaceAdv; continue; }
    const Glyph *g = findGlyph(*p);
    units += g ? g->adv : kSpaceAdv;
  }
  return fu(units * 256, capQ8);
}

void drawText(Mask &m, int32_t x, int32_t y, const char *s, int32_t capQ8,
              int32_t hw, int32_t tracking, int32_t alpha) {
  int32_t penU = 0;                       // pen position in font units
  bool first = true;
  int32_t top = y - capQ8;                // y of font row 0
  for (const char *p = s; *p; p++) {
    if (!first) penU += kGap + tracking;
    first = false;
    if (*p == ' ') { penU += kSpaceAdv; continue; }
    const Glyph *g = findGlyph(*p);
    if (!g) { penU += kSpaceAdv; continue; }
    int32_t ox = x + fu(penU * 256, capQ8);
    int32_t mult = 16;
    bool have = false;
    int32_t cx = 0, cy = 0;               // current point, Q8 px
    const int16_t *c = g->prog;
    while (*c != E) {
      int16_t op = *c++;
      int32_t w = (hw * mult) >> 4;
      if (op == W) { mult = *c++; continue; }
      if (op == M || op == L || op == D) {
        int32_t px = ox + fu(c[0] * 256, capQ8), py = top + fu(c[1] * 256, capQ8);
        c += 2;
        if (op == D) { maskSegment(m, px, py, px, py, imax(w * 3 / 2, 300), alpha); have = false; continue; }
        if (op == L && have) maskSegment(m, cx, cy, px, py, w, alpha);
        cx = px; cy = py; have = true;
        continue;
      }
      if (op == A) {
        int32_t acx = c[0], acy = c[1], rx = c[2], ry = c[3], a0 = c[4], a1 = c[5];
        c += 6;
        int32_t rpx = imax(fu(imax(rx, ry) * 256, capQ8) >> 8, 1);
        int32_t stepDeg = iclamp(150 / rpx, 3, 24);
        int32_t span = a1 - a0;
        int32_t n = imax(iabs(span) / stepDeg, 2);
        for (int32_t k = 0; k <= n; k++) {
          int32_t ad = a0 + span * k / n;
          uint16_t ang = deg(ad);
          int32_t fxq = acx * 256 + ((rx * icos(ang)) >> 6);
          int32_t fyq = acy * 256 + ((ry * isin(ang)) >> 6);
          int32_t px = ox + fu(fxq, capQ8), py = top + fu(fyq, capQ8);
          if (have) maskSegment(m, cx, cy, px, py, w, alpha);
          cx = px; cy = py; have = true;
        }
        continue;
      }
      break;                              // unknown op: stop this glyph
    }
    penU += g->adv;
  }
}

void drawTextCentered(Mask &m, int32_t cx, int32_t y, const char *s, int32_t capQ8,
                      int32_t hw, int32_t tracking, int32_t alpha) {
  drawText(m, cx - textWidth(s, capQ8, tracking) / 2, y, s, capQ8, hw, tracking, alpha);
}

}  // namespace gf
