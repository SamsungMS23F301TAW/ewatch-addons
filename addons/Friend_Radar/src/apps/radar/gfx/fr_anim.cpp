#include "fr_anim.h"
#include "fr_motion.h"
#include "fr_ui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace fr {

static constexpr float kPi = 3.14159265f;
// Both animations play inside the scope's glass (the radar frame clips
// anything outside it and lays the glass reflection on top).
static constexpr float kCx = (float)layout::cx, kCy = (float)layout::cy;
static constexpr float kR = (float)layout::Rg;

static inline uint8_t u8(float v) { return v <= 0.f ? 0 : (v >= 255.f ? 255 : (uint8_t)(v + 0.5f)); }
static inline float easeOutBack(float t) {
  t = clamp01f(t);
  const float c1 = 1.70158f, c3 = c1 + 1.f;
  float u = t - 1.f;
  return 1.f + c3 * u * u * u + c1 * u * u;
}
// Fade in over [a, b], hold, fade out over [c, d].
static inline float envelope(float t, float a, float b, float c, float d) {
  if (t < a || t > d) return 0.f;
  if (t < b) return (t - a) / (b - a);
  if (t > c) return 1.f - (t - c) / (d - c);
  return 1.f;
}

// Deterministic per-particle randomness: same numbers on every watch.
static inline float hrand(uint32_t seed, uint32_t i, uint32_t k) {
  return (float)(mix32(seed ^ mix32(i * 0x9e3779b9u + k * 0x85ebca6bu)) >> 8) / 16777216.f;
}

PairPalette pairPalette(uint32_t a, uint32_t b) {
  uint32_t lo = a < b ? a : b, hi = a < b ? b : a;
  int ia = mateColorIndex(lo), ib = mateColorIndex(hi);
  int gap = (ib - ia + kMateColorCount) % kMateColorCount;
  if (gap <= 1 || gap == kMateColorCount - 1) ib = ia + kMateColorCount / 2;   // two distinct lights
  PairPalette p;
  p.a = mateColorAt(ia);
  p.b = mateColorAt(ib);
  p.mix = blend(p.a, p.b, 128);
  return p;
}

uint32_t animDurationMs(AnimKind k) {
  return k == AnimKind::Hello ? kHelloDurMs : (k == AnimKind::Celebrate ? kCelebrateDurMs : 0);
}

// The scope dims (its own content sinks into the glass) while a moment plays.
static void dimGlass(Canvas &c, float amount) {
  if (amount <= 0.f) return;
  const int r = (int)kR + 1;
  blendRect(c, (int)kCx - r, (int)kCy - r, 2 * r, 2 * r, hex(0x010504), u8(228.f * clamp01f(amount)));
}

// The other person's name, set inside the lower glass, with a small caps line.
static void caption(Canvas &c, const char *big, const char *caps, float alpha, float rise, int baseline) {
  if (alpha <= 0.f) return;
  uint8_t a = u8(255.f * alpha);
  const Font *f = &kFontXL;
  if (textWidth(kFontXL, big) > 150) f = &kFontL;
  char b[24];
  fitText(*f, big, 156, b, sizeof b);
  int y = baseline + (int)lroundf(rise);
  int w = textWidth(*f, b);
  // A soft dark bed under the type so it reads over the bloom.
  glow(c, kCx, (float)(y - f->capH / 2), (float)w * 0.62f + 14.f, hex(0x000000), u8(150.f * alpha), false);
  textShadowed(c, *f, (int)kCx - w / 2, y, b, pal::text, a, 0x0000, 200);
  if (caps && *caps) drawCapsCentered(c, (int)kCx, y + 19, caps, pal::phos, a, 3);
}

// A comet: a lit head with a phosphor trail that decays behind it.
static void comet(Canvas &c, float x[], float y[], int n, uint16_t col, float alpha) {
  for (int j = n - 1; j >= 1; --j) {
    float k = 1.f - (float)j / (float)n;
    glow(c, x[j], y[j], 4.f + 6.f * k, col, u8(110.f * k * k * alpha));
  }
  glow(c, x[0], y[0], 16.f, col, u8(210.f * alpha));
  sphere(c, x[0], y[0], 4.2f, blend(col, 0xFFFF, 110), u8(255.f * alpha));
}

static void drawHello(Canvas &c, const PairAnimSpec &s, float t) {
  const float cx = kCx, cy = kCy - 18.f;                 // bloom sits above the caption
  const PairPalette pp = pairPalette(s.idA, s.idB);
  const uint32_t seed = s.seed;
  const int petals = 5 + (int)(seed % 4);                 // 5..8, per pair
  const float dir = ((seed >> 3) & 1) ? 1.f : -1.f;
  const float theta0 = (float)((seed >> 8) % 360) * kPi / 180.f;
  const float twist = 0.6f + 0.8f * hrand(seed, 1, 1);    // petal spin speed

  dimGlass(c, envelope(t, 0.f, 300.f, 3150.f, 3600.f));

  // Phase A: two comets spiral in from the rim and meet.
  if (t < 1100.f) {
    for (int i = 0; i < 2; ++i) {
      float xs[12], ys[12];
      int n = 0;
      for (int j = 0; j < 12; ++j) {
        float tt = t - (float)j * 24.f;
        if (tt < 0.f) break;
        float e = easeInOutCubicf(tt / 1000.f);
        float ang = theta0 + (float)i * kPi + dir * 2.2f * kPi * e;
        float rad = (kR - 10.f) * powf(1.f - e, 1.1f);
        xs[n] = cx + rad * sinf(ang);
        ys[n] = cy + 18.f * (1.f - e) - rad * cosf(ang);   // spiral centred on the glass, lands on the bloom
        ++n;
      }
      if (n) comet(c, xs, ys, n, i ? pp.b : pp.a, clamp01f(t / 120.f) * clamp01f((1100.f - t) / 120.f));
    }
  }

  // Phase B: the meeting flash, ripples to the rim, a bloom and sparks.
  if (t >= 950.f) {
    float tb = t - 1000.f;
    float flash = tb < 0.f ? (t - 950.f) / 50.f : expf(-tb / 200.f);
    glow(c, cx, cy, 26.f + 44.f * clamp01f(tb / 300.f), hex(0xFFFFFF), u8(220.f * clamp01f(flash)));

    for (int k = 0; k < 3; ++k) {
      float tr = tb - (float)k * 170.f;
      if (tr < 0.f || tr > 1000.f) continue;
      float p = tr / 1000.f;
      uint16_t col = k == 0 ? pp.a : (k == 1 ? pp.b : pal::phosHot);
      ring(c, cx, cy, 8.f + easeOutCubicf(p) * (kR + 10.f), 1.5f * (1.f - p) + 0.5f, col, u8(210.f * (1.f - p)));
    }

    float grow = easeOutBack(tb / 650.f);
    float breathe = 1.f + 0.06f * sinf(tb * 0.006f);
    float fade = envelope(t, 1000.f, 1100.f, 2600.f, 3250.f);
    if (fade > 0.f) {
      float spin = dir * twist * tb * 0.0006f;
      glow(c, cx, cy, 58.f * grow, pp.mix, u8(70.f * fade));
      for (int p = 0; p < petals; ++p) {
        float ang = theta0 + (float)p * 2.f * kPi / (float)petals + spin;
        uint16_t col = (p & 1) ? pp.b : pp.a;
        float len = 58.f * grow * breathe;
        petal(c, cx, cy, ang, len, 8.f, col, u8(175.f * fade));
        petal(c, cx, cy, ang + kPi / (float)petals, len * 0.55f, 4.2f, pp.mix, u8(130.f * fade));
      }
      sphere(c, cx, cy, 9.f * grow, blend(pp.mix, 0xFFFF, 150), u8(245.f * fade));
      ring(c, cx, cy, 14.f * grow, 0.9f, pp.mix, u8(190.f * fade));
    }

    // Sparks drifting out with drag, twinkling.
    for (int i = 0; i < 22; ++i) {
      float tp = tb - 40.f * hrand(seed, i, 7);
      if (tp < 0.f) continue;
      float ang = 2.f * kPi * hrand(seed, i, 1);
      float spd = 60.f + 110.f * hrand(seed, i, 2);        // px/s
      float drag = 1.6f;
      float dist = spd * (1.f - expf(-drag * tp / 1000.f)) / drag;
      float x = cx + dist * sinf(ang), y = cy - dist * cosf(ang);
      float life = clamp01f(1.f - tp / 2000.f);
      float tw = 0.6f + 0.4f * sinf(tp * 0.02f + 6.f * hrand(seed, i, 3));
      uint16_t col = (i % 3 == 0) ? pal::phosHot : ((i & 1) ? pp.a : pp.b);
      fillCircle(c, x, y, 1.2f + 1.1f * hrand(seed, i, 4), col, u8(255.f * life * tw));
    }
  }

  float ca = envelope(t, 1250.f, 1600.f, 3150.f, 3500.f);
  caption(c, s.name, "IS NEAR", ca, 8.f * (1.f - easeOutCubicf((t - 1250.f) / 350.f)), 198);
}

static void drawCelebrate(Canvas &c, const PairAnimSpec &s, float t) {
  const float cx = kCx, cy = kCy - 22.f;
  const PairPalette pp = pairPalette(s.idA, s.idB);
  const uint32_t seed = s.seed;
  const uint16_t colors[5] = {pp.a, pp.b, pal::phosHot, hex(0xFFD966), pal::phos};

  dimGlass(c, envelope(t, 0.f, 150.f, 2550.f, 3000.f));
  glow(c, cx, cy, 70.f, hex(0xFFFFFF), u8(255.f * expf(-t / 160.f)));
  if (t < 900.f) {
    float p = t / 900.f;
    ring(c, cx, cy, 10.f + (kR + 20.f) * easeOutCubicf(p), 2.2f * (1.f - p) + 0.5f, pp.mix, u8(230.f * (1.f - p)));
  }
  // Confetti with drag and gravity, spinning; the glass keeps it inside, like
  // a snow globe.
  const float g = 300.f, k = 1.7f;
  float ts = t / 1000.f;
  float decay = (1.f - expf(-k * ts)) / k;
  for (int i = 0; i < 56; ++i) {
    float ang = -kPi / 2.f + (hrand(seed, i, 1) - 0.5f) * 2.f * kPi * 0.62f;   // mostly upward
    float spd = 160.f + 240.f * hrand(seed, i, 2);
    float vx = spd * cosf(ang), vy = spd * sinf(ang);
    float x = cx + vx * decay;
    float y = cy + vy * decay + (g / k) * (ts - decay);
    float dx = x - kCx, dy = y - kCy;
    if (dx * dx + dy * dy > (kR + 6.f) * (kR + 6.f)) continue;
    float spin = (hrand(seed, i, 3) - 0.5f) * 18.f * ts + 6.f * hrand(seed, i, 4);
    float len = 2.6f + 2.6f * hrand(seed, i, 5);
    float fade = clamp01f((3000.f - t) / 500.f);
    uint16_t col = colors[i % 5];
    float ex = len * cosf(spin), ey = len * sinf(spin) * fabsf(cosf(ts * 7.f + (float)i));
    line(c, x - ex, y - ey, x + ex, y + ey, 1.2f, col, u8(255.f * fade));
  }
  // Two hearts beating together, one in each watch's light.
  float hp = envelope(t, 120.f, 320.f, 2500.f, 2950.f);
  if (hp > 0.f) {
    float beat = 1.f + 0.12f * sinf(t * 0.012f);
    glow(c, cx, cy + 4.f, 44.f, pp.mix, u8(90.f * hp));
    iconHeart(c, cx - 15.f, cy + 4.f, 14.f * beat * easeOutBack((t - 120.f) / 400.f), pp.a, u8(245.f * hp));
    iconHeart(c, cx + 15.f, cy + 4.f, 14.f * beat * easeOutBack((t - 200.f) / 400.f), pp.b, u8(245.f * hp));
  }
  char caps[32];
  char up[kMaxName + 1];
  size_t i = 0;
  for (; s.name[i] && i < kMaxName; ++i) up[i] = (s.name[i] >= 'a' && s.name[i] <= 'z') ? (char)(s.name[i] - 32) : s.name[i];
  up[i] = 0;
  snprintf(caps, sizeof caps, "WITH %s", up);
  float ca = envelope(t, 200.f, 450.f, 2550.f, 2950.f);
  caption(c, "High five!", caps, ca, 10.f * (1.f - easeOutBack((t - 200.f) / 450.f)), 192);
}

void drawPairAnim(Canvas &c, const PairAnimSpec &spec, int32_t tMs) {
  if (tMs < 0 || (uint32_t)tMs >= animDurationMs(spec.kind)) return;
  if (spec.kind == AnimKind::Hello) drawHello(c, spec, (float)tMs);
  else if (spec.kind == AnimKind::Celebrate) drawCelebrate(c, spec, (float)tMs);
}

}  // namespace fr
