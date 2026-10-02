// GARDEN — recursive branching plants rooted along the ground. The whole
// garden is generated up front (breadth-first across every plant), and each
// growth element draws the next branch, so the garden rises a generation at
// a time as you walk: trunks first, then limbs, twigs, and finally blossoms
// at the tips. Variants: ORCHARD (a few blossoming trees), MEADOW (a field of
// stems and flowers), REEF (swaying coral under water).
#include "gf_family.h"

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

namespace gf {

namespace {
struct Node {
  int32_t  x, y;          // Q8 base point
  int32_t  len;           // Q8
  int16_t  hw;            // Q8 base half-width
  uint16_t ang;
  uint16_t rot;           // blossom rotation
  uint16_t pr;            // blossom petal radius, Q8
  uint8_t  depth, plant, leaf, bloom, petals, pad;
};
static const int32_t kMaxNodes = 2400;

struct St {
  uint8_t  variant;
  int32_t  nPlants, groundY, total, n0;
  int32_t  widthF;                // Q8 taper per branch
  int32_t  bendQ8;                // pull toward vertical per sub-segment
  uint32_t wood[3], ground, groundTop;
  uint32_t bloom[3];
  uint32_t waveSeed;
  int32_t  sunX, sunY, sunR;
};
inline St &S(void *p) { return *static_cast<St *>(p); }
inline const St &S(const void *p) { return *static_cast<const St *>(p); }
inline Node *nodes(JobCtx &c) { return reinterpret_cast<Node *>(c.mem + 1024); }

static const uint16_t kUp = 49152;   // 270 degrees: straight up on screen

// Walks a branch as four bending sub-segments. Fills xy (5 points) and ws
// (5 half-widths) when non-null; always returns the tip and tip angle.
void trace(const St &s, const Node &n, int32_t *xy, int32_t *ws,
           int32_t &ex, int32_t &ey, uint16_t &ea) {
  int32_t x = n.x, y = n.y;
  uint16_t a = n.ang;
  int32_t sub = n.len / 4;
  int32_t hwEnd = (n.hw * s.widthF) >> 8;
  if (xy) { xy[0] = x; xy[1] = y; ws[0] = n.hw; }
  for (int32_t k = 1; k <= 4; k++) {
    int32_t d = angDiff(kUp, a);
    a = (uint16_t)(a + ((d * s.bendQ8) >> 8));
    if (s.variant == 2) {
      int32_t wv = noise1((y >> 4) * 40 + n.plant * 300000, s.waveSeed);
      a = (uint16_t)(a + (wv >> 6));
    }
    x += (int32_t)(((int64_t)icos(a) * sub) >> 14);
    y += (int32_t)(((int64_t)isin(a) * sub) >> 14);
    if (xy) { xy[2 * k] = x; xy[2 * k + 1] = y; ws[k] = n.hw + ((hwEnd - n.hw) * k) / 4; }
  }
  ex = x; ey = y; ea = a;
}
}  // namespace

void gardenInit(void *st, JobCtx &ctx) {
  St &s = S(st);
  Rng &r = *ctx.famRng;
  const Palette &p = ctx.spec->pal;
  s.variant = ctx.spec->variant;
  Node *nd = nodes(ctx);
  s.groundY = r.range(248, 262);
  s.waveSeed = r.next();
  s.sunX = r.range(40, 200) * 256;
  s.sunY = r.range(40, 110) * 256;
  s.sunR = r.range(14, 24) * 256;

  int32_t trunkLen0, trunkLen1, hw0, hw1, depth0, depth1;
  int32_t lenF0, lenF1, spread0, spread1, oneKid, threeKids;
  if (s.variant == 0) {          // ORCHARD
    s.nPlants = r.range(2, 4);
    depth0 = 7; depth1 = 8;
    trunkLen0 = 40; trunkLen1 = 70; hw0 = 900; hw1 = 1500;
    lenF0 = 165; lenF1 = 205; spread0 = 3000; spread1 = 6200;
    s.widthF = 178; s.bendQ8 = 14; oneKid = 40; threeKids = 120;
  } else if (s.variant == 1) {   // MEADOW
    s.nPlants = r.range(10, 16);
    depth0 = 2; depth1 = 4;
    trunkLen0 = 34; trunkLen1 = 96; hw0 = 160; hw1 = 290;
    lenF0 = 130; lenF1 = 190; spread0 = 1400; spread1 = 4200;
    s.widthF = 200; s.bendQ8 = 40; oneKid = 150; threeKids = 220;
  } else {                       // REEF
    s.nPlants = r.range(4, 7);
    depth0 = 5; depth1 = 7;
    trunkLen0 = 20; trunkLen1 = 44; hw0 = 480; hw1 = 900;
    lenF0 = 180; lenF1 = 228; spread0 = 2000; spread1 = 6000;
    s.widthF = 205; s.bendQ8 = 30; oneKid = 220; threeKids = 150;
  }
  uint32_t baseWood = p.darkBg ? scaleRGB(p.ink[4], 150) : scaleRGB(p.dark, 300);
  for (int32_t k = 0; k < 3; k++) s.wood[k] = baseWood;
  if (s.variant == 2) { s.wood[0] = p.ink[1]; s.wood[1] = p.ink[2]; s.wood[2] = p.ink[4]; }
  s.ground = p.darkBg ? scaleRGB(p.bg1, 70) : blend(p.bg1, p.ink[4], 70);
  s.groundTop = blend(s.ground, p.ink[3], 90);
  s.bloom[0] = p.ink[0]; s.bloom[1] = p.accent; s.bloom[2] = p.ink[3];

  // Trunks, spread evenly with jitter; each plant gets its own depth.
  int32_t plantDepth[24];
  s.total = 0;
  for (int32_t k = 0; k < s.nPlants && k < 24; k++) {
    Node &n = nd[s.total++];
    int32_t slot = kW * 256 / s.nPlants;
    int32_t jitter = r.range(-slot / 3, slot / 3);
    n.x = slot * k + slot / 2 + jitter;
    n.y = s.groundY * 256 + 512;
    n.len = r.range(trunkLen0, trunkLen1) * 256;
    n.hw = (int16_t)r.range(hw0, hw1);
    int32_t tilt = r.range(-1300, 1300);
    n.ang = (uint16_t)(kUp + tilt);
    n.depth = 0;
    n.plant = (uint8_t)k;
    plantDepth[k] = r.range(depth0, depth1);
  }
  // Breadth-first expansion of the whole garden (no drawing here).
  int32_t n0 = 0;
  for (int32_t i = 0; i < s.total; i++) {
    Node &n = nd[i];
    if (n.depth <= 1) n0 = i + 1;
    int32_t ex, ey; uint16_t ea;
    trace(s, n, nullptr, nullptr, ex, ey, ea);
    bool leaf = (n.depth + 1 >= plantDepth[n.plant]) || n.len < 3 * 256;
    uint32_t kroll = r.below(1000);
    int32_t kids = kroll < (uint32_t)oneKid ? 1 : (kroll > 1000u - (uint32_t)threeKids ? 3 : 2);
    if (n.depth == 0 && kids == 1) kids = 2;
    n.leaf = leaf ? 1 : 0;
    n.bloom = (uint8_t)((n.plant + r.below(3)) % 3);
    n.petals = (uint8_t)r.range(4, 6);
    n.pr = (uint16_t)r.range(150, 330);
    n.rot = r.angle();
    if (leaf) continue;
    int32_t hwEnd = imax((n.hw * s.widthF) >> 8, 70);
    for (int32_t k = 0; k < kids; k++) {
      int32_t spread = r.range(spread0, spread1);
      int32_t side = (kids == 3) ? (k - 1) : (kids == 1 ? 0 : (k == 0 ? -1 : 1));
      int32_t jitter = r.range(-600, 600);
      int32_t lf = r.range(lenF0, lenF1);
      if (kids == 1) lf = imin(lf + 20, 240);
      if (s.total >= kMaxNodes) continue;
      Node &c = nd[s.total++];
      c.x = ex; c.y = ey;
      c.ang = (uint16_t)(ea + side * spread + jitter);
      c.len = (n.len * lf) >> 8;
      c.hw = (int16_t)hwEnd;
      c.depth = (uint8_t)(n.depth + 1);
      c.plant = n.plant;
    }
  }
  s.n0 = imax(n0, s.nPlants);
}

int32_t gardenPrepCount(const void *) { return kH / kBgRowsPerOp + 2; }

void gardenPrep(void *st, JobCtx &ctx, int32_t i) {
  St &s = S(st);
  const Palette &p = ctx.spec->pal;
  Canvas &cv = *ctx.cv;
  int32_t bgOps = kH / kBgRowsPerOp;
  if (i < bgOps) { backgroundRows(ctx, i * kBgRowsPerOp, (i + 1) * kBgRowsPerOp, 3); return; }
  if (i == bgOps) {
    if (s.variant == 2) {
      // Light shafts falling through the water.
      Rng lr; lr.seed(ctx.spec->seed, 11);
      for (int32_t k = 0; k < 5; k++) {
        int32_t x = lr.range(-20, 260) * 256;
        int32_t dx = lr.range(-60, 60) * 256;
        int32_t w = lr.range(6, 18) * 256;
        segment(cv, x, -10 * 256, x + dx, s.groundY * 256, w, w * 3, p.light, 22, false, false);
      }
    } else {
      glow(cv, s.sunX, s.sunY, s.sunR * 4, p.light, p.darkBg ? 50 : 110);
      disc(cv, s.sunX, s.sunY, s.sunR, p.darkBg ? p.light : blend(p.light, p.accent, 60),
           p.darkBg ? 200 : 220);
    }
    return;
  }
  // Ground band with a soft lit edge.
  for (int32_t x = 0; x < kW; x++) {
    int32_t wob = noise1(x * 900, s.waveSeed) * 3;               // +-3 px
    int32_t top = s.groundY * 256 + (wob >> 8);
    columnFill(cv, x, top, kH, s.ground, 255);
    columnFill(cv, x, top, (top >> 8) + 2, s.groundTop, 150);
  }
}

int32_t gardenElemCount(const void *st, int32_t g) {
  const St &s = S(st);
  return growCount(s.n0, s.total, g);
}

void gardenElem(void *st, JobCtx &ctx, int32_t i) {
  St &s = S(st);
  if (i >= s.total) return;
  const Palette &p = ctx.spec->pal;
  Canvas &cv = *ctx.cv;
  const Node &n = nodes(ctx)[i];
  int32_t *xy = reinterpret_cast<int32_t *>(ctx.mem);
  int32_t ws[5];
  int32_t x, y; uint16_t a;
  trace(s, n, xy, ws, x, y, a);
  uint32_t wood = s.wood[n.plant % 3];
  if (n.depth == 0) wood = scaleRGB(wood, 230);
  polyline(cv, xy, 5, ws, 0, wood, s.variant == 1 ? 230 : 250);
  if (!n.leaf) return;

  // Tips: blossoms, flowers or glowing polyps.
  uint32_t bc = s.bloom[n.bloom];
  int32_t pr = n.pr;
  if (s.variant == 2) {
    glow(cv, x, y, pr * 9, bc, 90);
    disc(cv, x, y, pr + 160, bc, 240);
    disc(cv, x, y, pr / 2 + 40, p.light, 220);
    return;
  }
  int32_t ring = (s.variant == 1) ? pr * 3 / 2 + 220 : pr + 120;
  for (int32_t k = 0; k < n.petals; k++) {
    uint16_t pa = (uint16_t)(n.rot + (uint32_t)(65536 * k / n.petals));
    int32_t px = x + (int32_t)(((int64_t)icos(pa) * ring) >> 14);
    int32_t py = y + (int32_t)(((int64_t)isin(pa) * ring) >> 14);
    disc(cv, px, py, pr + (s.variant == 1 ? 140 : 40), bc, 215);
  }
  disc(cv, x, y, pr * 2 / 3 + 40, s.variant == 1 ? p.accent : p.light, 230);
}

}  // namespace gf
