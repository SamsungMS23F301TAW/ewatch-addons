#include <string.h>
#include "gf_art.h"
#include "gf_family.h"

namespace gf {

// ---------------------------------------------------------------- names
const char *familyName(uint8_t f) {
  static const char *k[kFamilyCount] = {
    "CURRENTS", "ORBITS", "WEAVE", "GARDEN", "CONTOURS", "MOSAIC", "DUNES" };
  return f < kFamilyCount ? k[f] : "?";
}

const char *variantName(uint8_t f, uint8_t v) {
  static const char *k[kFamilyCount][3] = {
    { "SILK", "BRUSH", "DRIFT" },
    { "SOLAR", "HALO", "SYSTEMS" },
    { "RIBBON", "RAIL", "KNOT" },
    { "ORCHARD", "MEADOW", "REEF" },
    { "ISLANDS", "TERRACE", "NIGHT MAP" },
    { "CATHEDRAL", "TILEWORK", "CRACKLE" },
    { "DAY", "DUSK", "NIGHT" },
  };
  return (f < kFamilyCount && v < 3) ? k[f][v] : "?";
}

// ---------------------------------------------------------------- growth
uint16_t bucketForSteps(uint32_t steps) {
  uint32_t b = steps / STEPS_PER_BUCKET;
  return (uint16_t)(b > MAX_BUCKET ? MAX_BUCKET : b);
}

int32_t growthPermille(uint16_t b) {
  if (b >= FULL_BUCKET) return 1000;
  int32_t n = (int32_t)b;
  int32_t f = (int32_t)FULL_BUCKET;
  return (1000 * (2 * n * f - n * n)) / (f * f);      // ease-out quadratic
}

// ---------------------------------------------------------------- the date
// Weekly shuffle: each Monday-based week shows all seven families once, and
// a week never opens with the family that closed the previous one.
static void weekPerm(uint32_t week, uint8_t perm[kFamilyCount]) {
  Rng r;
  r.seed(0x5745454B44415953ULL ^ ((uint64_t)week * 0x2545F4914F6CDD1DULL), 7);
  for (uint8_t i = 0; i < kFamilyCount; i++) perm[i] = i;
  for (int32_t i = kFamilyCount - 1; i > 0; i--) {
    int32_t j = (int32_t)r.below((uint32_t)(i + 1));
    uint8_t t = perm[i]; perm[i] = perm[j]; perm[j] = t;
  }
}

static uint8_t scheduledFamily(uint16_t day) {
  uint32_t week = ((uint32_t)day + 5u) / 7u;     // 2000-01-03 starts week 1
  uint32_t slot = ((uint32_t)day + 5u) % 7u;     // 0 = Monday
  uint8_t perm[kFamilyCount], prev[kFamilyCount];
  weekPerm(week, perm);
  if (week > 0) {
    weekPerm(week - 1, prev);
    if (perm[0] == prev[kFamilyCount - 1]) { uint8_t t = perm[0]; perm[0] = perm[1]; perm[1] = t; }
  }
  return perm[slot];
}

// The year's colour drift: a hue anchor per month (mid-month), blended along
// the shorter way round the colour wheel. Daily randomness rides on top.
static int32_t yearlyHue(uint16_t day) {
  static const int16_t kAnchor[12] = { 205, 228, 165, 125, 95, 50, 28, 12, 38, 22, 335, 250 };
  int32_t doy = dayOfYear(day);                  // 1..366
  int32_t pos = (doy - 15) * 1000;               // anchors at doy 15, 45, ...
  if (pos < 0) pos += 365 * 1000;
  int32_t span = 30417;                          // 365/12 days, x1000
  int32_t i = pos / span;
  int32_t f = (pos % span) * 1000 / span;        // permille between anchors
  int32_t a = kAnchor[i % 12], b = kAnchor[(i + 1) % 12];
  int32_t d = b - a;
  if (d > 180) d -= 360;
  if (d < -180) d += 360;
  int32_t h = a + d * f / 1000;
  return ((h % 360) + 360) % 360;
}

// Chance (permille) of a dark background, per family and variant.
static const uint16_t kDarkPermille[kFamilyCount][3] = {
  { 820, 650, 750 },     // currents
  { 850, 800, 900 },     // orbits
  { 600, 500, 650 },     // weave
  { 450, 300, 1000 },    // garden (reef is always dark)
  { 450, 400, 1000 },    // contours (night map is always dark)
  { 900, 0, 1000 },      // mosaic (tilework light, crackle dark)
  { 0, 500, 1000 },      // dunes (day light, night dark)
};

void describeDay(uint16_t day, uint8_t algo, ArtSpec &out) {
  out.day = day;
  out.algo = algo;
  out.seed = 0x4441595052494E54ULL ^ ((uint64_t)day * 0x9E3779B97F4A7C15ULL) ^
             ((uint64_t)algo << 56);
  Rng r;
  r.seed(out.seed, 1);
  out.family = scheduledFamily(day);
  out.variant = (uint8_t)r.below(3);
  out.special = specialDayOf(day);
  if (out.special == kJuneSolstice)     { out.family = kDunes; out.variant = 0; }
  if (out.special == kDecemberSolstice) { out.family = kDunes; out.variant = 2; }

  int32_t hue = yearlyHue(day);
  uint32_t dark = kDarkPermille[out.family][out.variant];
  makePalette(r, hue, 75, dark, out.pal);

  // Special-day palettes.
  Palette &p = out.pal;
  switch (out.special) {
    case kNewYear:
      p.darkBg = true; p.baseHue = 45;
      p.bg0 = hsl(230, 500, 60);  p.bg1 = hsl(250, 450, 130);
      p.ink[0] = hsl(44, 900, 620); p.ink[1] = hsl(36, 700, 760);
      p.ink[2] = hsl(340, 650, 700); p.ink[3] = hsl(200, 300, 820);
      p.ink[4] = hsl(48, 950, 540); p.accent = hsl(0, 0, 980);
      p.light = hsl(45, 600, 940); p.dark = hsl(235, 500, 40);
      p.text = hsl(45, 500, 950); p.halo = hsl(235, 500, 40);
      break;
    case kJuneSolstice:
      p.darkBg = false; p.baseHue = 28;
      p.bg0 = hsl(205, 650, 720); p.bg1 = hsl(40, 900, 860);
      p.ink[0] = hsl(25, 850, 520); p.ink[1] = hsl(12, 750, 420);
      p.ink[2] = hsl(40, 900, 600); p.ink[3] = hsl(350, 600, 380);
      p.ink[4] = hsl(18, 700, 300); p.accent = hsl(50, 1000, 600);
      p.light = hsl(50, 1000, 950); p.dark = hsl(10, 600, 180);
      p.text = hsl(15, 600, 120); p.halo = hsl(45, 800, 940);
      break;
    case kDecemberSolstice:
      p.darkBg = true; p.baseHue = 215;
      p.bg0 = hsl(225, 600, 45); p.bg1 = hsl(215, 450, 160);
      p.ink[0] = hsl(200, 450, 640); p.ink[1] = hsl(215, 400, 480);
      p.ink[2] = hsl(190, 350, 360); p.ink[3] = hsl(230, 300, 260);
      p.ink[4] = hsl(205, 500, 200); p.accent = hsl(45, 700, 800);
      p.light = hsl(200, 300, 960); p.dark = hsl(225, 600, 30);
      p.text = hsl(200, 300, 960); p.halo = hsl(225, 600, 30);
      break;
    default:
      break;
  }
}

void describeWeekRemix(uint16_t monday, const uint16_t *days, const uint32_t *steps,
                       int32_t n, ArtSpec &out, uint16_t &bucketOut) {
  // A synthetic "day" far outside real dates gives the week its own seed.
  uint16_t week = (uint16_t)(((uint32_t)monday + 5u) / 7u);
  describeDay((uint16_t)(40000u + week), ALGO_VERSION, out);
  out.day = monday;
  out.special = kOrdinary;
  uint32_t best = 0, total = 0;
  int32_t sx = 0, sy = 0;                   // circular mean of the hues
  for (int32_t i = 0; i < n; i++) {
    ArtSpec d;
    describeDay(days[i], ALGO_VERSION, d);
    if (steps[i] >= best) { best = steps[i]; out.family = d.family; out.variant = d.variant; }
    total += steps[i];
    sx += icos(deg(d.pal.baseHue)) >> 4;
    sy += isin(deg(d.pal.baseHue)) >> 4;
  }
  int32_t hue = out.pal.baseHue;
  if (n > 0 && (sx != 0 || sy != 0)) {
    // atan2 by search over whole degrees: plenty for a hue, and integer-only.
    int32_t bestDot = -0x7FFFFFFF;
    for (int32_t a = 0; a < 360; a++) {
      int32_t dot = (int32_t)(((int64_t)sx * icos(deg(a)) + (int64_t)sy * isin(deg(a))) >> 14);
      if (dot > bestDot) { bestDot = dot; hue = a; }
    }
  }
  Rng r;
  r.seed(out.seed, 9);
  makePalette(r, hue, 25, out.pal.darkBg ? 800 : 400, out.pal);
  bucketOut = bucketForSteps(n > 0 ? total / (uint32_t)n : 0);
}

// ---------------------------------------------------------------- helpers
void backgroundRows(JobCtx &ctx, int32_t y0, int32_t y1, int32_t grain) {
  gradientRows(*ctx.cv, y0, y1, ctx.spec->pal.bg0, ctx.spec->pal.bg1,
               (uint32_t)(ctx.spec->seed >> 32) ^ 0xB16B00B5u, grain);
}

void sortKeys(uint32_t *k, int32_t n) {
  static const int32_t gaps[] = { 701, 301, 132, 57, 23, 10, 4, 1 };
  for (int32_t g = 0; g < 8; g++) {
    int32_t gap = gaps[g];
    for (int32_t i = gap; i < n; i++) {
      uint32_t v = k[i];
      int32_t j = i;
      while (j >= gap && k[j - gap] > v) { k[j] = k[j - gap]; j -= gap; }
      k[j] = v;
    }
  }
}

uint32_t pickInk(const Palette &p, Rng &r, uint32_t accentPermille) {
  uint32_t roll = r.below(1000);
  uint32_t which = r.below(5);
  if (roll < accentPermille) return p.accent;
  return p.ink[which];
}

uint32_t canvasHash(const Canvas &c) {
  uint32_t h = 2166136261u;
  int32_t n = (int32_t)c.w * c.h;
  for (int32_t i = 0; i < n; i++) {
    uint32_t v = c.px[i];
    uint8_t b[3] = { (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    h = fnv1a(b, 3, h);
  }
  return h;
}

// ---------------------------------------------------------------- the job
struct FamilyFns {
  void    (*init)(void *, JobCtx &);
  int32_t (*prepCount)(const void *);
  void    (*prep)(void *, JobCtx &, int32_t);
  int32_t (*elemCount)(const void *, int32_t);
  void    (*elem)(void *, JobCtx &, int32_t);
};

#define GF_FNS(n) { n##Init, n##PrepCount, n##Prep, n##ElemCount, n##Elem }
static const FamilyFns kFns[kFamilyCount] = {
  GF_FNS(currents), GF_FNS(orbits), GF_FNS(weave), GF_FNS(garden),
  GF_FNS(contours), GF_FNS(mosaic), GF_FNS(dunes),
};

struct FamState { uint8_t bytes[kStateBytes]; };

ArtJob::ArtJob() {}
ArtJob::~ArtJob() {}

static inline JobCtx makeCtx(Canvas *cv, uint8_t *scratch, const ArtSpec *spec,
                             Rng *fam, Rng *elem) {
  JobCtx c;
  c.cv = cv;
  c.mem = scratch + kStateBytes;
  c.memBytes = kScratchBytes - kStateBytes;
  c.spec = spec;
  c.famRng = fam;
  c.elemRng = elem;
  return c;
}

void ArtJob::begin(Canvas *canvas, uint8_t *scratch, uint16_t day, uint16_t bucket,
                   uint8_t algo) {
  ArtSpec spec;
  describeDay(day, algo, spec);
  beginSpec(canvas, scratch, spec, bucket);
}

void ArtJob::beginSpec(Canvas *canvas, uint8_t *scratch, const ArtSpec &spec, uint16_t bucket) {
  spec_ = spec;
  cv_ = canvas;
  scratch_ = scratch;
  st_ = reinterpret_cast<FamState *>(scratch);
  memset(scratch_, 0, kScratchBytes);
  famRng_.seed(spec_.seed, 2);
  elemRng_.seed(spec_.seed, 3);
  flourRng_.seed(spec_.seed, 4);
  JobCtx ctx = makeCtx(cv_, scratch_, &spec_, &famRng_, &elemRng_);
  kFns[spec_.family].init(st_, ctx);
  prepCount_ = kFns[spec_.family].prepCount(st_);
  prepDone_ = 0;
  elemDone_ = 0;
  flourDone_ = 0;
  ops_ = 0;
  bucket_ = bucket > MAX_BUCKET ? MAX_BUCKET : bucket;
  refreshTargets();
  phase_ = kPrep;
}

void ArtJob::refreshTargets() {
  int32_t g = growthPermille(bucket_);
  elemTarget_ = kFns[spec_.family].elemCount(st_, g);
  flourTarget_ = bucket_ > FULL_BUCKET ? (int32_t)(bucket_ - FULL_BUCKET) * 4 : 0;
}

bool ArtJob::extendTo(uint16_t bucket) {
  if (bucket > MAX_BUCKET) bucket = MAX_BUCKET;
  if (phase_ == kIdle || bucket < bucket_) return false;
  if (bucket == bucket_) return true;
  uint16_t old = bucket_;
  bucket_ = bucket;
  refreshTargets();
  // Elements must all precede any flourish. Flourish only exists once the
  // elements are saturated, so this cannot trigger for valid families;
  // refuse rather than draw out of order if a family ever breaks the rule.
  if (flourDone_ > 0 && elemDone_ < elemTarget_) {
    bucket_ = old;
    refreshTargets();
    return false;
  }
  if (phase_ == kDone || phase_ == kFlourish) {
    if (elemDone_ < elemTarget_)        phase_ = kElements;
    else if (flourDone_ < flourTarget_) phase_ = kFlourish;
  }
  return true;
}

bool ArtJob::run(uint32_t maxOps) {
  if (phase_ == kIdle) return false;
  JobCtx ctx = makeCtx(cv_, scratch_, &spec_, &famRng_, &elemRng_);
  const FamilyFns &f = kFns[spec_.family];
  uint32_t n = 0;
  while (n < maxOps) {
    if (phase_ == kPrep) {
      if (prepDone_ < prepCount_) { f.prep(st_, ctx, prepDone_++); n++; ops_++; continue; }
      phase_ = kElements;
    }
    if (phase_ == kElements) {
      if (elemDone_ < elemTarget_) { f.elem(st_, ctx, elemDone_++); n++; ops_++; continue; }
      phase_ = kFlourish;
    }
    if (phase_ == kFlourish) {
      if (flourDone_ < flourTarget_) {
        flourDone_++;
        int32_t x = flourRng_.range(10, kW - 10);
        int32_t y = flourRng_.range(10, kH - 10);
        int32_t size = flourRng_.range(384, 1280);
        bool acc = flourRng_.chance(300);
        int32_t alpha = flourRng_.range(150, 230);
        glint(*cv_, x * 256 + 128, y * 256 + 128, size,
              acc ? spec_.pal.accent : spec_.pal.light, (uint32_t)alpha);
        n++; ops_++;
        continue;
      }
      phase_ = kDone;
    }
    if (phase_ == kDone) return true;
  }
  return phase_ == kDone;
}

void ArtJob::renderFull(Canvas *canvas, uint8_t *scratch, uint16_t day,
                        uint16_t bucket, uint8_t algo) {
  ArtJob j;
  j.begin(canvas, scratch, day, bucket, algo);
  while (!j.run(1u << 20)) {}
}

}  // namespace gf
