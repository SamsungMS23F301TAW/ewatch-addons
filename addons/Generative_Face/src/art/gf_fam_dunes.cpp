// DUNES — a sky with sun or moon over layered ridges that run from hazy and
// far to dark and near. Ridges are painted back to front; walking brings
// the nearer ranges in, so the landscape comes closer as the day goes on.
// Variants: DAY (high sun, pale sky), DUSK (low sun, warm gradient), NIGHT
// (stars, a crescent moon, moonlit crests).
#include "gf_family.h"

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

namespace gf {

namespace {
static const int32_t kMaxR = 9;
static const int32_t kChunks = 6;                 // column chunks per ridge
static const int32_t kChunkW = kW / kChunks;      // 40 px

struct St {
  uint8_t  variant;
  int32_t  nR;
  int32_t  baseY[kMaxR], amp[kMaxR], scaleQ16[kMaxR], sharp[kMaxR];
  uint32_t seed[kMaxR];
  uint32_t colTop[kMaxR], colBot[kMaxR];
  uint32_t sky0, sky1, horizon, sunColor, rim;
  int32_t  sunX, sunY, sunR, sunSide;
  uint32_t starSeed;
};
inline St &S(void *p) { return *static_cast<St *>(p); }
inline const St &S(const void *p) { return *static_cast<const St *>(p); }

uint32_t skyAt(const St &s, int32_t y) {
  int32_t mid = kH * 55 / 100;
  if (y <= mid) return blend(s.sky0, s.sky1, (uint32_t)(y * 256 / mid));
  return blend(s.sky1, s.horizon, (uint32_t)iclamp((y - mid) * 256 / (kH - mid), 0, 256));
}

// Ridge crest height at column x (Q8 y).
int32_t crest(const St &s, int32_t k, int32_t x) {
  int32_t nx = (int32_t)((int64_t)x * s.scaleQ16[k]);
  int32_t n = fbm1(nx, s.seed[k], 3);
  // Fold away from zero so the sharp crests land at irregular places
  // (1-D gradient noise is zero at every lattice point).
  int32_t ridged = 65536 - 2 * iabs(n - 19661);
  int32_t prof = (int32_t)(((int64_t)n * (1000 - s.sharp[k]) + (int64_t)ridged * s.sharp[k]) / 1000);
  return s.baseY[k] - (int32_t)(((int64_t)s.amp[k] * prof) >> 16);
}
}  // namespace

void dunesInit(void *st, JobCtx &ctx) {
  St &s = S(st);
  Rng &r = *ctx.famRng;
  const Palette &p = ctx.spec->pal;
  s.variant = ctx.spec->variant;
  s.nR = r.range(6, kMaxR);
  s.starSeed = r.next();
  s.sunSide = r.chance(500) ? 1 : -1;
  int32_t hue = p.baseHue;
  // Night skies read best in the blue-violet-teal range; steer other hues there.
  int32_t nightHue = (hue >= 170 && hue <= 300) ? hue : 200 + (hue % 70);
  int32_t landHue = hue + r.range(-20, 20);

  if (s.variant == 0) {          // DAY
    int32_t skyHue = 188 + ((hue + 180) % 360) / 7;              // always sky-ish
    s.sky0 = hsl(skyHue, r.range(420, 620), r.range(600, 700));
    s.sky1 = hsl(skyHue - 8, r.range(320, 480), r.range(800, 860));
    s.horizon = hsl(hue + 20, 520, 900);
    s.sunColor = hsl(46, 950, 925);
    s.sunX = r.range(40, 200) * 256;
    s.sunY = r.range(34, 84) * 256;
    s.sunR = r.range(10, 16) * 256;
  } else if (s.variant == 1) {   // DUSK
    s.sky0 = hsl(hue + 230, 550, 160);
    s.sky1 = hsl(hue + 320, 580, 420);
    s.horizon = hsl(hue + 25, 850, 680);
    s.sunColor = hsl(hue + 40, 1000, 780);
    s.sunX = r.range(50, 190) * 256;
    s.sunY = r.range(116, 140) * 256;
    s.sunR = r.range(18, 28) * 256;
  } else {                       // NIGHT
    s.sky0 = hsl(nightHue + 10, 600, 45);
    s.sky1 = hsl(nightHue, 520, 110);
    s.horizon = hsl(nightHue - 15, 420, 230);
    s.sunColor = hsl(nightHue + 180, 200, 920);
    s.sunX = r.range(40, 200) * 256;
    s.sunY = r.range(36, 96) * 256;
    s.sunR = r.range(9, 14) * 256;
  }
  if (ctx.spec->special == kJuneSolstice) {
    s.sunR = 30 * 256;
    s.sunY = 70 * 256;
    s.sunX = 120 * 256;
  }
  if (ctx.spec->special == kDecemberSolstice) {
    s.sunR = 22 * 256;                        // the longest night: a full moon
    s.sunY = 64 * 256;
    s.sunX = 120 * 256;
  }

  // Land colours: one hue family from hazy (far) to deep (near).
  uint32_t nearC, farC;
  if (s.variant == 0) {
    farC  = blend(s.horizon, hsl(landHue, 550, 700), 150);
    nearC = hsl(landHue + 15, 600, 330);
  } else if (s.variant == 1) {
    farC  = blend(s.horizon, hsl(landHue + 300, 450, 450), 140);
    nearC = hsl(landHue + 260, 500, 110);
  } else {
    farC  = blend(s.horizon, hsl(nightHue + 10, 400, 260), 120);
    nearC = hsl(nightHue + 20, 450, 45);
  }

  // Ridges: far (k = 0) to near; perspective spacing.
  for (int32_t k = 0; k < s.nR; k++) {
    int32_t f = k * 1000 / (s.nR - 1);
    int32_t persp = (f * 4 + (f * f / 1000) * 6) / 10;          // eases toward the front
    s.baseY[k] = (kH * (430 + persp * 470 / 1000) / 1000) * 256;
    int32_t a0 = r.range(9, 18);
    int32_t a1 = r.range(26, 48);
    s.amp[k] = (a0 + (a1 - a0) * f / 1000) * 256;
    int32_t farPx = r.range(45, 80);
    int32_t nearExtra = r.range(60, 140);
    int32_t featurePx = farPx + f * nearExtra / 1000;
    s.scaleQ16[k] = 65536 / featurePx;
    s.sharp[k] = r.range(200, 800);
    s.seed[k] = r.next();
    int32_t jitter = r.range(-14, 14);
    uint32_t c = blend(farC, nearC, (uint32_t)iclamp(f * 256 / 1000 + jitter, 0, 256));
    s.colTop[k] = c;
    s.colBot[k] = scaleRGB(c, s.variant == 0 ? 205 : 170);
  }
  s.rim = (s.variant == 2) ? blend(s.sunColor, p.light, 80) : blend(p.light, s.sunColor, 120);
}

int32_t dunesPrepCount(const void *) { return kH / kBgRowsPerOp + 2; }

void dunesPrep(void *st, JobCtx &ctx, int32_t i) {
  St &s = S(st);
  const Palette &p = ctx.spec->pal;
  Canvas &cv = *ctx.cv;
  int32_t bgOps = kH / kBgRowsPerOp;
  if (i < bgOps) {
    uint32_t gseed = (uint32_t)(ctx.spec->seed >> 24);
    for (int32_t y = i * kBgRowsPerOp; y < (i + 1) * kBgRowsPerOp; y++) {
      uint32_t base = skyAt(s, y);
      uint32_t *row = cv.px + y * cv.w;
      for (int32_t x = 0; x < kW; x++) {
        uint32_t n = hash2(x, y, gseed) & 0xFF;
        int32_t o = (int32_t)((n * 5) >> 8) - 2;
        row[x] = rgb((uint32_t)iclamp((int32_t)chR(base) + o, 0, 255),
                     (uint32_t)iclamp((int32_t)chG(base) + o, 0, 255),
                     (uint32_t)iclamp((int32_t)chB(base) + o, 0, 255));
      }
    }
    return;
  }
  if (i == bgOps) {
    if (s.variant == 2) {
      // Stars, denser toward the zenith.
      Rng sr; sr.seed(s.starSeed, 21);
      int32_t n = sr.range(70, 130);
      for (int32_t k = 0; k < n; k++) {
        int32_t x = sr.range(0, kW - 1);
        int32_t y = sr.range(0, kH * 6 / 10);
        int32_t y2 = sr.range(0, kH * 6 / 10);
        int32_t bright = sr.range(90, 255);
        int32_t big = sr.range(0, 20);
        int32_t yy = imin(y, y2);
        disc(cv, x * 256 + 128, yy * 256 + 128, big == 0 ? 300 : 150, p.light, (uint32_t)bright);
      }
    }
    return;
  }
  // Sun or moon.
  int32_t glowR = s.sunR * (s.variant == 1 ? 7 : 4);
  glow(cv, s.sunX, s.sunY, glowR, s.sunColor, s.variant == 1 ? 150 : 90);
  disc(cv, s.sunX, s.sunY, s.sunR, s.sunColor, 245);
  if (s.variant == 2 && ctx.spec->special != kDecemberSolstice) {
    int32_t off = s.sunR * 2 / 5;
    disc(cv, s.sunX + off * s.sunSide, s.sunY - off / 2, s.sunR * 23 / 25,
         skyAt(s, s.sunY >> 8), 245);
  }
}

int32_t dunesElemCount(const void *st, int32_t g) {
  const St &s = S(st);
  int32_t ridges = 2 + (s.nR - 2) * g / 1000;
  return ridges * kChunks;
}

void dunesElem(void *st, JobCtx &ctx, int32_t i) {
  St &s = S(st);
  int32_t k = i / kChunks;
  if (k >= s.nR) return;
  Canvas &cv = *ctx.cv;
  int32_t x0 = (i % kChunks) * kChunkW;
  int32_t f = k * 1000 / (s.nR - 1);
  int32_t depthPx = 60 + f * 70 / 1000;
  for (int32_t x = x0; x < x0 + kChunkW; x++) {
    int32_t top = crest(s, k, x);
    int32_t slope = crest(s, k, x + 1) - crest(s, k, x - 1);    // Q8 per 2 px
    int32_t ty = top >> 8;
    int32_t gy0 = (s.baseY[k] - s.amp[k]) >> 8;               // gradient starts level
    for (int32_t y = imax(ty, 0); y < kH; y++) {
      int32_t d = imax(y - gy0, 0);
      uint32_t c = blend(s.colTop[k], s.colBot[k], (uint32_t)imin(d * 256 / depthPx, 256));
      uint32_t &px = cv.px[y * cv.w + x];
      if (y == ty) px = blend(px, c, (uint32_t)(256 - (top & 255)));
      else         px = c;
    }
    // Rim light on faces turned toward the sun or moon.
    int32_t lit = iclamp(-slope * s.sunSide / 3, 0, 220);
    if (lit > 8 && ty >= -1 && ty < kH) {
      uint32_t a = (uint32_t)lit;
      cv.mix(x, ty + 1, s.rim, a);
      cv.mix(x, ty + 2, s.rim, a / 3);
      cv.mix(x, ty, s.rim, a * (uint32_t)(256 - (top & 255)) >> 8);
    }
  }
}

}  // namespace gf
