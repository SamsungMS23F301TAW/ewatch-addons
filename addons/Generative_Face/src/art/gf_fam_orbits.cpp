// ORBITS — arcs on concentric tracks, some dotted or dashed, a few carrying
// "planets". Every element is one arc. Variants: SOLAR (one system near the
// middle), HALO (a huge system centred off-screen, sweeping arcs), SYSTEMS
// (several small systems joined like a constellation).
#include "gf_family.h"

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

namespace gf {

namespace {
struct St {
  uint8_t variant;
  int32_t nSys;
  int32_t cx[6], cy[6], rMin[6], rMax[6];
  int32_t trackQ8;
  int32_t n0, n1;
};
inline St &S(void *p) { return *static_cast<St *>(p); }
inline const St &S(const void *p) { return *static_cast<const St *>(p); }
}  // namespace

void orbitsInit(void *st, JobCtx &ctx) {
  St &s = S(st);
  Rng &r = *ctx.famRng;
  s.variant = ctx.spec->variant;
  if (s.variant == 0) {
    s.nSys = 1;
    s.cx[0] = r.range(70, 170) * 256;
    s.cy[0] = r.range(100, 190) * 256;
    s.rMin[0] = r.range(10, 22) * 256;
    s.rMax[0] = r.range(150, 200) * 256;
    s.trackQ8 = r.range(5, 9) * 256;
    s.n0 = 14; s.n1 = 170;
  } else if (s.variant == 1) {
    s.nSys = 1;
    int32_t side = (int32_t)r.below(4);
    int32_t along = r.range(-40, 280);
    int32_t out = r.range(30, 110);
    if (side == 0)      { s.cx[0] = along;      s.cy[0] = kH + out; }
    else if (side == 1) { s.cx[0] = along;      s.cy[0] = -out; }
    else if (side == 2) { s.cx[0] = -out;       s.cy[0] = along; }
    else                { s.cx[0] = kW + out;   s.cy[0] = along; }
    s.cx[0] *= 256; s.cy[0] *= 256;
    s.rMin[0] = (out + r.range(20, 60)) * 256;
    s.rMax[0] = (out + r.range(300, 380)) * 256;
    s.trackQ8 = r.range(6, 11) * 256;
    s.n0 = 12; s.n1 = 150;
  } else {
    // Two to four systems kept apart (fixed number of placement attempts,
    // so the result stays deterministic).
    int32_t want = r.range(2, 4);
    s.nSys = 0;
    for (int32_t attempt = 0; attempt < 40 && s.nSys < want; attempt++) {
      int32_t x = r.range(46, 194);
      int32_t y = r.range(50, 236);
      int32_t rad = r.range(34, 64);
      bool ok = true;
      for (int32_t k = 0; k < s.nSys; k++) {
        int32_t dx = x - (s.cx[k] >> 8), dy = y - (s.cy[k] >> 8);
        int32_t need = rad + (s.rMax[k] >> 8) - 10;
        if (dx * dx + dy * dy < need * need) { ok = false; break; }
      }
      if (!ok) continue;
      s.cx[s.nSys] = x * 256;
      s.cy[s.nSys] = y * 256;
      s.rMin[s.nSys] = r.range(5, 8) * 256;
      s.rMax[s.nSys] = rad * 256;
      s.nSys++;
    }
    if (s.nSys == 0) { s.nSys = 1; s.cx[0] = 120 * 256; s.cy[0] = 150 * 256; s.rMin[0] = 1536; s.rMax[0] = 60 * 256; }
    s.trackQ8 = r.range(4, 6) * 256;
    s.n0 = 14; s.n1 = 150;
  }
}

int32_t orbitsPrepCount(const void *) { return kH / kBgRowsPerOp + 2; }

void orbitsPrep(void *st, JobCtx &ctx, int32_t i) {
  St &s = S(st);
  const Palette &p = ctx.spec->pal;
  int32_t bgOps = kH / kBgRowsPerOp;
  if (i < bgOps) { backgroundRows(ctx, i * kBgRowsPerOp, (i + 1) * kBgRowsPerOp, 3); return; }
  if (i == bgOps) {
    // Soft light pooled at each system's centre.
    for (int32_t k = 0; k < s.nSys; k++) {
      uint32_t c = p.darkBg ? p.ink[0] : p.light;
      int32_t rr = s.variant == 1 ? s.rMin[k] + 60 * 256 : s.rMax[k];
      glow(*ctx.cv, s.cx[k], s.cy[k], rr, c, p.darkBg ? 46 : 70);
    }
    return;
  }
  // Centres: a small star for SOLAR, constellation lines + nodes for SYSTEMS.
  if (s.variant == 0) {
    disc(*ctx.cv, s.cx[0], s.cy[0], s.rMin[0] / 2, p.accent, 235);
    glow(*ctx.cv, s.cx[0], s.cy[0], s.rMin[0] * 2, p.accent, 90);
  } else if (s.variant == 2) {
    for (int32_t k = 0; k + 1 < s.nSys; k++) {
      // Dotted tether between neighbouring systems.
      int32_t dx = s.cx[k + 1] - s.cx[k], dy = s.cy[k + 1] - s.cy[k];
      int32_t len = (int32_t)isqrt64((uint64_t)((int64_t)dx * dx + (int64_t)dy * dy)) >> 8;
      int32_t dots = imax(len / 6, 1);
      for (int32_t d = 1; d < dots; d++) {
        disc(*ctx.cv, s.cx[k] + dx * d / dots, s.cy[k] + dy * d / dots, 110,
             p.darkBg ? p.light : p.dark, 120);
      }
    }
    for (int32_t k = 0; k < s.nSys; k++) {
      disc(*ctx.cv, s.cx[k], s.cy[k], s.rMin[k] / 2 + 128, p.accent, 230);
    }
  }
}

int32_t orbitsElemCount(const void *st, int32_t g) {
  const St &s = S(st);
  return growCount(s.n0, s.n1, g);
}

void orbitsElem(void *st, JobCtx &ctx, int32_t) {
  St &s = S(st);
  Rng &r = *ctx.elemRng;
  const Palette &p = ctx.spec->pal;
  int32_t k = s.nSys > 1 ? (int32_t)r.below((uint32_t)s.nSys) : 0;
  int32_t tracks = imax((s.rMax[k] - s.rMin[k]) / s.trackQ8, 1);
  int32_t t = r.range(0, tracks);
  int32_t radius = s.rMin[k] + t * s.trackQ8;
  uint32_t wroll = r.below(1000);
  int32_t hw;
  if (wroll < 700)      hw = r.range(90, 230);
  else if (wroll < 940) hw = r.range(280, 520);
  else                  hw = r.range(640, 1150);
  if (s.variant == 2 && hw > 400) hw = r.range(150, 400);
  uint16_t a0 = r.angle();
  uint32_t sweep = (uint32_t)r.range(2800, 58000);
  uint32_t style = r.below(1000);
  uint32_t color = pickInk(p, r, 60);
  if (s.variant == 2 && r.chance(650)) color = p.ink[k % 5];     // each system has a hue
  int32_t alpha = r.range(170, 250);
  bool planet = r.chance(170);
  int32_t pr = r.range(2, 6) * 256;
  Canvas &cv = *ctx.cv;

  if (style < 700) {
    arc(cv, s.cx[k], s.cy[k], radius, hw, a0, sweep, color, (uint32_t)alpha);
  } else if (style < 850) {
    // Dotted: dots every ~7 px of arc length.
    int32_t arcLen = (int32_t)(((int64_t)radius * 6 * (int64_t)sweep) >> 16);  // ~2*pi*r*frac
    int32_t dots = iclamp(arcLen / (7 * 256), 2, 240);
    int32_t dr = imax(hw, 150) + 60;
    for (int32_t d = 0; d <= dots; d++) {
      uint16_t a = (uint16_t)(a0 + (uint32_t)(((uint64_t)sweep * (uint32_t)d) / (uint32_t)dots));
      int32_t x = s.cx[k] + (int32_t)(((int64_t)icos(a) * radius) >> 14);
      int32_t y = s.cy[k] + (int32_t)(((int64_t)isin(a) * radius) >> 14);
      disc(cv, x, y, dr, color, (uint32_t)alpha);
    }
  } else {
    // Dashed: 9-degree dashes with 5-degree gaps.
    uint32_t dash = 1638, gap = 910;
    for (uint32_t off = 0; off + 200 < sweep; off += dash + gap) {
      uint32_t len = (off + dash > sweep) ? sweep - off : dash;
      arc(cv, s.cx[k], s.cy[k], radius, hw, (uint16_t)(a0 + off), len, color, (uint32_t)alpha);
    }
  }
  if (planet) {
    uint16_t a = (uint16_t)(a0 + sweep);
    int32_t x = s.cx[k] + (int32_t)(((int64_t)icos(a) * radius) >> 14);
    int32_t y = s.cy[k] + (int32_t)(((int64_t)isin(a) * radius) >> 14);
    disc(cv, x, y, pr + 200, p.darkBg ? p.bg0 : p.bg1, 255);
    disc(cv, x, y, pr, color, 255);
    arc(cv, x, y, pr + 3 * 256, 70, 0, 65536, color, 150);
  }
}

}  // namespace gf
