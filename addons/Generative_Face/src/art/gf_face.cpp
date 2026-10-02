#include <stdio.h>
#include <string.h>
#include "gf_face.h"

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

namespace gf {

namespace {

// How strongly each family needs a scrim behind the clock (0..255): busy,
// saturated pieces get more, skies and gardens with calm upper halves less.
static const uint8_t kScrimTop[kFamilyCount] = {
  120,   // currents
  110,   // orbits
  150,   // weave
  60,    // garden
  130,   // contours
  160,   // mosaic
  45,    // dunes
};

// Smooth 0..255 plateau between [a, b] ramping in over `inW` rows and out
// over `outW` rows.
uint8_t band(int32_t y, int32_t a, int32_t b, int32_t inW, int32_t outW) {
  int32_t t;
  if (y < a - inW || y > b + outW) return 0;
  if (y < a)      t = ((y - (a - inW)) * 65536) / imax(inW, 1);
  else if (y > b) t = (((b + outW) - y) * 65536) / imax(outW, 1);
  else            t = 65536;
  return (uint8_t)((smoothQ16(t) * 255) >> 16);
}

void ambientFactors(uint8_t h, uint8_t m, int32_t &fr, int32_t &fg, int32_t &fb) {
  // Keyframes: minute of day -> channel multipliers (Q8).
  static const int16_t kKeys[][4] = {
    {    0, 206, 212, 238 },   // deep night: cooler and dimmer
    {  270, 206, 212, 238 },
    {  390, 256, 236, 212 },   // dawn: warm
    {  480, 256, 248, 240 },
    {  540, 256, 256, 256 },   // day
    { 1050, 256, 256, 256 },
    { 1140, 256, 238, 214 },   // golden hour
    { 1230, 244, 236, 236 },
    { 1350, 206, 212, 238 },   // night again
    { 1440, 206, 212, 238 },
  };
  int32_t t = h * 60 + m;
  int32_t n = (int32_t)(sizeof(kKeys) / sizeof(kKeys[0]));
  for (int32_t i = 0; i + 1 < n; i++) {
    if (t >= kKeys[i][0] && t <= kKeys[i + 1][0]) {
      int32_t span = imax(kKeys[i + 1][0] - kKeys[i][0], 1);
      int32_t f = ((t - kKeys[i][0]) * 256) / span;
      fr = kKeys[i][1] + ((kKeys[i + 1][1] - kKeys[i][1]) * f >> 8);
      fg = kKeys[i][2] + ((kKeys[i + 1][2] - kKeys[i][2]) * f >> 8);
      fb = kKeys[i][3] + ((kKeys[i + 1][3] - kKeys[i][3]) * f >> 8);
      return;
    }
  }
  fr = fg = fb = 256;
}

static const uint8_t kBayer[4][4] = {
  { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

// Draws one text item as a soft halo (three widening passes) plus the text.
void textItem(Mask &mt, Mask &mh, int32_t cx, int32_t baseline, const char *s,
              int32_t cap, int32_t hw, int32_t tracking, int32_t haloPx) {
  int32_t h1 = hw + haloPx * 256 / 3, h2 = hw + haloPx * 256 * 2 / 3, h3 = hw + haloPx * 256;
  drawTextCentered(mh, cx, baseline, s, cap, h3, tracking, 70);
  drawTextCentered(mh, cx, baseline, s, cap, h2, tracking, 150);
  drawTextCentered(mh, cx, baseline, s, cap, h1, tracking, 235);
  drawTextCentered(mt, cx, baseline, s, cap, hw, tracking, 255);
}

void dateLine(uint16_t day, bool withYear, char *buf, size_t n) {
  uint16_t y; uint8_t m, d;
  dayToDate(day, y, m, d);
  if (withYear) snprintf(buf, n, "%s %u %s %u", weekdayShort(weekdayOf(day)), d, monthShort(m), y);
  else          snprintf(buf, n, "%s %u %s", weekdayShort(weekdayOf(day)), d, monthShort(m));
}
}  // namespace

void formatSteps(uint32_t steps, char *buf) {
  if (steps >= 1000000) steps = 999999;
  if (steps >= 1000) snprintf(buf, 16, "%u,%03u", (unsigned)(steps / 1000), (unsigned)(steps % 1000));
  else               snprintf(buf, 16, "%u", (unsigned)steps);
}

// Mean luma of a region, sampling every other pixel.
static int32_t meanLuma(const Canvas &c, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
  int32_t sum = 0, n = 0;
  for (int32_t y = y0; y < y1; y += 2)
    for (int32_t x = x0; x < x1; x += 2) { sum += luma(c.px[y * c.w + x]); n++; }
  return n ? sum / n : 0;
}

uint8_t chooseTones(const Canvas &art, const ArtSpec &spec, uint8_t mode, uint8_t prev) {
  (void)spec;
  int32_t top = (mode == kModeClock) ? meanLuma(art, 24, 40, 216, 140) : 0;
  int32_t bot = meanLuma(art, 40, 236, 200, 276);
  uint8_t t = 0;
  bool topDark = (prev & 1) ? top > 112 : top > 152;
  bool botDark = (prev & 2) ? bot > 112 : bot > 152;
  if (mode == kModeClock && topDark) t |= 1;
  if (botDark) t |= 2;
  return t;
}

uint32_t faceKey(const ArtSpec &spec, const FaceInputs &in, uint8_t tones) {
  uint32_t k[10] = { spec.day, (uint32_t)spec.family << 8 | spec.algo, in.day,
                    (uint32_t)in.hour << 8 | in.minute,
                    (uint32_t)in.rtcOk | (uint32_t)in.timeUnset << 1 | (uint32_t)in.batteryLow << 2 |
                        (uint32_t)in.isToday << 3,
                    in.steps, in.goal, in.mode, tones, 0x5EED };
  uint32_t h = fnv1a(k, sizeof(k));
  if (in.note) h = fnv1a(in.note, strlen(in.note), h);
  if (in.caption1) h = fnv1a(in.caption1, strlen(in.caption1), h ^ 1);
  if (in.caption2) h = fnv1a(in.caption2, strlen(in.caption2), h ^ 2);
  return h;
}

void buildFaceLayer(const ArtSpec &spec, const FaceInputs &in, uint8_t tones, FaceLayer &L) {
  const Palette &p = spec.pal;
  memset(L.maskText, 0, (size_t)kW * kH);
  memset(L.maskHalo, 0, (size_t)kW * kH);
  memset(L.scrim, 0, sizeof(L.scrim));
  Mask mt{ L.maskText, kW, kH }, mh{ L.maskHalo, kW, kH };
  int32_t hue = p.baseHue;
  L.textColor[0] = p.darkBg ? p.text : hsl(hue, 260, 955);
  L.haloColor[0] = p.darkBg ? p.halo : hsl(hue + 180, 420, 45);
  L.textColor[1] = p.darkBg ? hsl(hue + 180, 420, 90) : p.text;
  L.haloColor[1] = p.darkBg ? hsl(hue, 300, 965) : p.halo;
  L.tones = tones;
  char buf[48], num[16];
  int32_t cx = kW * 128;

  if (in.mode == kModeClock) {
    // Time.
    if (in.rtcOk) snprintf(buf, sizeof(buf), "%02u:%02u", in.hour, in.minute);
    else          snprintf(buf, sizeof(buf), "--:--");
    textItem(mt, mh, cx, 106 * 256, buf, 60 * 256, 590, -4, 4);
    // Date (or a hint if the clock was never set).
    if (in.timeUnset) {
      snprintf(buf, sizeof(buf), "SET TIME IN SETTINGS");
    } else if (spec.special != kOrdinary && spec.day == in.day) {
      uint16_t yy; uint8_t mo, dd;
      dayToDate(in.day, yy, mo, dd);
      snprintf(buf, sizeof(buf), "%u %s %c %s", dd, monthShort(mo), kGlyphDot,
               specialDayName(spec.special));
    } else {
      dateLine(in.day, false, buf, sizeof(buf));
    }
    textItem(mt, mh, cx, 131 * 256, buf, 10 * 256 + 128, 205, 42, 3);
    // Steps with the footprints glyph.
    formatSteps(in.steps, num);
    snprintf(buf, sizeof(buf), "%c %s", kGlyphSteps, num);
    textItem(mt, mh, cx, 264 * 256, buf, 12 * 256, 240, 18, 3);
    // Goal progress: a dim hairline track with the walked part bright.
    if (in.goal > 0) {
      int32_t half = 36 * 256, y = 247 * 256 + 128;
      uint32_t frac = in.steps >= in.goal ? 256u : (in.steps * 256u) / in.goal;
      int32_t xe = cx - half + (int32_t)((2 * half * (int32_t)frac) >> 8);
      maskSegment(mh, cx - half, y, cx + half, y, 600, 200);
      maskSegment(mt, cx - half, y, cx + half, y, 110, 95);
      if (frac > 0) maskSegment(mt, cx - half, y, xe, y, 150, 255);
    }
    if (in.batteryLow) textItem(mt, mh, cx, 26 * 256, "\x05", 8 * 256, 200, 0, 2);
    uint8_t top = kScrimTop[spec.family < kFamilyCount ? spec.family : 0];
    for (int32_t y = 0; y < kH; y++) {
      int32_t a = (band(y, 54, 136, 34, 30) * top) >> 8;
      int32_t b = (band(y, 252, 290, 34, 0) * (top * 3 / 4 + 30)) >> 8;
      L.scrim[y] = (uint8_t)imax(a, b);
    }
  } else if (in.mode == kModePlain) {
    // Nothing on top of the art.
  } else {
    // Museum-label caption at the bottom (art view and gallery pages).
    uint16_t doy = dayOfYear(in.day);
    const char *special = specialDayName(spec.special);
    if (in.mode == kModeGallery) {
      if (in.isToday) snprintf(buf, sizeof(buf), "TODAY");
      else            dateLine(in.day, true, buf, sizeof(buf));
    } else {
      snprintf(buf, sizeof(buf), "N%c %u %c %s", kGlyphNumero, doy, kGlyphDot, familyName(spec.family));
    }
    if (in.caption1) snprintf(buf, sizeof(buf), "%s", in.caption1);
    textItem(mt, mh, cx, 250 * 256, buf, 10 * 256 + 128, 215, 40, 3);
    formatSteps(in.steps, num);
    if (in.mode == kModeGallery) {
      snprintf(buf, sizeof(buf), "%c %s  %c  %s", kGlyphSteps, num, kGlyphDot, familyName(spec.family));
    } else if (special[0]) {
      snprintf(buf, sizeof(buf), "%s %c %s %c %s", variantName(spec.family, spec.variant),
               kGlyphDot, special, kGlyphDot, num);
    } else {
      char dl[24];
      dateLine(in.day, true, dl, sizeof(dl));
      snprintf(buf, sizeof(buf), "%s %c %s", dl, kGlyphDot, num);
    }
    if (in.caption2) snprintf(buf, sizeof(buf), "%s", in.caption2);
    textItem(mt, mh, cx, 267 * 256, buf, 8 * 256 + 128, 180, 34, 3);
    if (in.mode == kModeGallery && (in.note || special[0])) {
      if (in.note) snprintf(buf, sizeof(buf), "%s", in.note);
      else         snprintf(buf, sizeof(buf), "%c %s", kGlyphSparkle, special);
      textItem(mt, mh, cx, 26 * 256, buf, 8 * 256 + 128, 180, 40, 3);
      for (int32_t y = 0; y < 44; y++) L.scrim[y] = (uint8_t)((band(y, 0, 26, 1, 18) * 90) >> 8);
    }
    for (int32_t y = 0; y < kH; y++) {
      uint8_t a = (uint8_t)((band(y, 248, 290, 46, 0) * 150) >> 8);
      if (a > L.scrim[y]) L.scrim[y] = a;
    }
  }

  // Row flags let composeFrame skip mask lookups on empty rows; bit1 picks
  // the dark-text tone (top region above y = 200, bottom region below).
  for (int32_t y = 0; y < kH; y++) {
    const uint8_t *a = L.maskText + y * kW, *b = L.maskHalo + y * kW;
    uint8_t f = 0;
    for (int32_t x = 0; x < kW; x++) if (a[x] | b[x]) { f = 1; break; }
    bool dark = (y < 200) ? (tones & 1) : (tones & 2);
    L.rowFlags[y] = (uint8_t)(f | (dark ? 2 : 0));
  }
  L.key = faceKey(spec, in, tones);
  L.valid = true;
}

void composeFrame(const Canvas &art, const ArtSpec &spec, const FaceInputs &in,
                  const FaceLayer &L, uint16_t *out, int32_t y0, int32_t y1) {
  (void)spec;
  int32_t fr = 256, fg = 256, fb = 256;
  bool tint = in.ambient && in.mode == kModeClock;
  if (tint) ambientFactors(in.hour, in.minute, fr, fg, fb);
  // Drifting highlight centre for the optional slow animation.
  bool light = in.lightPhase != 0;
  int32_t lx = 0, ly = 0;
  if (light) {
    lx = 120 + ((80 * isin(in.lightPhase)) >> 14);
    ly = 130 + ((100 * isin((uint16_t)(in.lightPhase * 2 + 9000))) >> 14);
  }
  const int32_t lr2 = 150 * 150;
  y0 = imax(y0, 0); y1 = imin(y1, kH);
  for (int32_t y = y0; y < y1; y++) {
    const uint32_t *src = art.px + y * art.w;
    uint16_t *dst = out + (y - y0) * kW;
    uint32_t sa = L.valid ? L.scrim[y] : 0;
    bool masks = L.valid && (L.rowFlags[y] & 1);
    uint32_t tone = (L.valid && (L.rowFlags[y] & 2)) ? 1 : 0;
    uint32_t textC = L.textColor[tone], haloC = L.haloColor[tone];
    const uint8_t *mt = L.maskText + y * kW, *mh = L.maskHalo + y * kW;
    const uint8_t *bay = kBayer[y & 3];
    int32_t dy2 = (y - ly) * (y - ly);
    for (int32_t x = 0; x < kW; x++) {
      uint32_t c = src[x];
      int32_t r = (int32_t)chR(c), g = (int32_t)chG(c), b = (int32_t)chB(c);
      if (tint) { r = (r * fr) >> 8; g = (g * fg) >> 8; b = (b * fb) >> 8; }
      if (light) {
        int32_t d2 = (x - lx) * (x - lx) + dy2;
        if (d2 < lr2) {
          int32_t u = 256 - (d2 * 256) / lr2;
          int32_t boost = 256 + ((u * u) >> 11);              // up to +12.5 %
          r = imin((r * boost) >> 8, 255); g = imin((g * boost) >> 8, 255); b = imin((b * boost) >> 8, 255);
        }
      }
      if (tint || light) c = rgb((uint32_t)r, (uint32_t)g, (uint32_t)b);
      if (sa) c = blend(c, haloC, sa);
      if (masks) {
        uint32_t h = mh[x], t = mt[x];
        if (h) c = blend(c, haloC, (h * 200) >> 8);
        if (t) c = blend(c, textC, t + (t >> 7));
      }
      uint32_t th = bay[x & 3];
      uint32_t rr = imin((int32_t)(chR(c) + ((th * 8) >> 4)), 255);
      uint32_t gg = imin((int32_t)(chG(c) + ((th * 4) >> 4)), 255);
      uint32_t bb = imin((int32_t)(chB(c) + ((th * 8) >> 4)), 255);
      dst[x] = (uint16_t)(((rr >> 3) << 11) | ((gg >> 2) << 5) | (bb >> 3));
    }
  }
}

}  // namespace gf
