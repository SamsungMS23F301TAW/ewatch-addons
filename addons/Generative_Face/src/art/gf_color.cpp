#include "gf_color.h"

namespace gf {

uint32_t hsl(int32_t h, int32_t s, int32_t l) {
  h %= 360; if (h < 0) h += 360;
  s = iclamp(s, 0, 1000);
  l = iclamp(l, 0, 1000);
  // Chroma, all in permille.
  int32_t c = (1000 - iabs(2 * l - 1000)) * s / 1000;
  int32_t hp = h * 1000 / 60;                         // sector position x1000
  int32_t x = c * (1000 - iabs((hp % 2000) - 1000)) / 1000;
  int32_t m = l - c / 2;
  int32_t r = 0, g = 0, b = 0;
  switch (h / 60) {
    case 0:  r = c; g = x; b = 0; break;
    case 1:  r = x; g = c; b = 0; break;
    case 2:  r = 0; g = c; b = x; break;
    case 3:  r = 0; g = x; b = c; break;
    case 4:  r = x; g = 0; b = c; break;
    default: r = c; g = 0; b = x; break;
  }
  r = iclamp((r + m) * 255 + 500, 0, 255500) / 1000;
  g = iclamp((g + m) * 255 + 500, 0, 255500) / 1000;
  b = iclamp((b + m) * 255 + 500, 0, 255500) / 1000;
  return rgb((uint32_t)r, (uint32_t)g, (uint32_t)b);
}

const char *schemeName(uint8_t s) {
  static const char *kNames[kSchemeCount] = {
    "ANALOGOUS", "COMPLEMENTARY", "SPLIT", "TRIADIC", "TETRADIC", "MONO" };
  return s < kSchemeCount ? kNames[s] : "?";
}

// Hue offsets per scheme for ink[0..4] and the accent.
static const int16_t kSchemeHues[kSchemeCount][6] = {
  {   0,  28, -28,  52, -14, 180 },   // analogous, accent = complement
  {   0, 180,  22, 198, -16,  30 },   // complementary
  {   0, 150, 210,  16, 180,  40 },   // split-complementary
  {   0, 120, 240,  18, 138, 300 },   // triadic
  {   0,  90, 180, 270,  12, 135 },   // tetradic
  {   0,   6,  -6,  12, -12, 180 },   // monochrome (accent breaks it)
};

// Nudges l until the colour's luma clears `bgLuma` by `minGap` in the
// requested direction. Integer loop, at most 20 steps.
static uint32_t withContrast(int32_t h, int32_t s, int32_t &l, int32_t bgLuma,
                             int32_t minGap, bool lighter) {
  uint32_t c = hsl(h, s, l);
  for (int i = 0; i < 20; i++) {
    int32_t gap = lighter ? luma(c) - bgLuma : bgLuma - luma(c);
    if (gap >= minGap) break;
    l += lighter ? 30 : -30;
    l = iclamp(l, 40, 960);
    c = hsl(h, s, l);
  }
  return c;
}

void makePalette(Rng &rng, int32_t hueCenter, int32_t hueSpread,
                 uint32_t darkPermille, Palette &p) {
  int32_t spread = rng.tri(hueSpread);
  int32_t base = hueCenter + spread;
  base %= 360; if (base < 0) base += 360;
  uint8_t scheme = (uint8_t)rng.below(kSchemeCount);
  bool dark = rng.chance(darkPermille);
  p.baseHue = (int16_t)base;
  p.scheme = scheme;
  p.darkBg = dark;

  // Background: low-chroma version of the base hue or its complement.
  bool bgComp = rng.chance(300);
  int32_t bgHue = base + (bgComp ? 180 : 0) + rng.range(-12, 12);
  int32_t bgSat = rng.range(220, 520);
  int32_t bgL0, bgL1;
  if (dark) {
    bgL0 = rng.range(60, 120);
    bgL1 = bgL0 + rng.range(10, 70);
  } else {
    bgL0 = rng.range(880, 940);
    bgL1 = bgL0 - rng.range(30, 90);
    bgSat = rng.range(180, 420);
  }
  int32_t bgShift = rng.range(-30, 30);
  p.bg0 = hsl(bgHue, bgSat, bgL0);
  p.bg1 = hsl(bgHue + bgShift, bgSat, bgL1);
  int32_t bgLuma = (luma(p.bg0) + luma(p.bg1)) / 2;

  // Inks: varied lightness so the palette has real value contrast.
  static const int16_t kDarkL[5]  = { 700, 600, 520, 780, 450 };
  static const int16_t kLightL[5] = { 380, 480, 300, 560, 250 };
  for (int i = 0; i < 5; i++) {
    int32_t h = base + kSchemeHues[scheme][i] + rng.range(-6, 6);
    int32_t s = rng.range(520, 900);
    int32_t jitter = rng.range(-50, 50);
    int32_t l = (dark ? kDarkL[i] : kLightL[i]) + jitter;
    if (scheme == kMonochrome) s = iclamp(s - 120 + i * 60, 250, 950);
    p.ink[i] = withContrast(h, s, l, bgLuma, dark ? 75 : 70, dark);
  }
  {
    int32_t h = base + kSchemeHues[scheme][5] + rng.range(-10, 10);
    int32_t l = dark ? rng.range(600, 700) : rng.range(420, 520);
    p.accent = withContrast(h, rng.range(780, 980), l, bgLuma, 80, dark);
  }
  p.light = hsl(base, rng.range(200, 400), dark ? 930 : 975);
  p.dark  = hsl(base + 180, rng.range(250, 500), dark ? 35 : 140);

  // Clock colours: near-white on dark days, near-black on light ones, tinted
  // with the base hue so the type belongs to the piece.
  if (dark) {
    p.text = hsl(base, 260, 955);
    p.halo = hsl(bgHue, 400, 40);
  } else {
    p.text = hsl(base + 180, 420, 90);
    p.halo = hsl(bgHue, 300, 965);
  }
}

}  // namespace gf
