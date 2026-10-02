#include "tp_sky.h"
#include "tp_config.h"
#include "tp_noise.h"
#include "tp_platform.h"
#include <math.h>
#include <string.h>

namespace tp {

static const float kPi = 3.14159265f;
static const float kDeg = kPi / 180.0f;
static const float kLatitude = 48.0f;        // assumed; see README
static const float kSolarNoon = 12.5f;       // local clock, compromise for DST

// ---------------------------------------------------------------------------
// Calendar helpers
// ---------------------------------------------------------------------------
static bool isLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0); }

static int dayOfYear(const LocalTime &t) {
  static const int cum[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
  int m = t.month < 1 ? 1 : (t.month > 12 ? 12 : t.month);
  int d = cum[m - 1] + (t.day < 1 ? 1 : t.day);
  if (m > 2 && isLeap(t.year)) d++;
  return d;
}

double daysSince2000(const LocalTime &t) {
  long days = 0;
  int y0 = t.year < 2000 ? 2000 : t.year;
  for (int y = 2000; y < y0; y++) days += isLeap(y) ? 366 : 365;
  days += dayOfYear(t) - 1;
  return (double)days + (t.hour + t.minute / 60.0 + t.second / 3600.0) / 24.0;
}

float moonPhaseFor(const LocalTime &t) {
  // Reference new moon: 2000-01-06 18:14 UTC (5.76 days after the epoch).
  double p = (daysSince2000(t) - 5.76) / 29.530588853;
  p -= floor(p);
  return (float)p;
}

static float solarDeclination(int doy) {
  return 23.44f * sinf(2.0f * kPi * (284.0f + doy) / 365.0f);
}

// Altitude (degrees) of a body with declination `decl` at hour angle `hDeg`.
static float altitudeDeg(float decl, float hDeg) {
  float s = sinf(kLatitude * kDeg) * sinf(decl * kDeg) +
            cosf(kLatitude * kDeg) * cosf(decl * kDeg) * cosf(hDeg * kDeg);
  if (s > 1) s = 1;
  if (s < -1) s = -1;
  return asinf(s) / kDeg;
}

// Half the time (hours) a body with this declination spends above the horizon.
static float halfDayHours(float decl) {
  float c = -tanf(kLatitude * kDeg) * tanf(decl * kDeg);
  if (c >= 1) return 0.5f;     // never rises (polar night) — keep sane
  if (c <= -1) return 11.5f;   // never sets
  return acosf(c) / kDeg / 15.0f;
}

static float wrap24(float h) {
  while (h < -12) h += 24;
  while (h >= 12) h -= 24;
  return h;
}

// ---------------------------------------------------------------------------
// Colour keyframes by sun altitude (degrees).
// ---------------------------------------------------------------------------
struct SkyKey {
  float alt;
  RGB zen, hor, glow, amb, sun;
};
static const SkyKey kKeys[] = {
  //  alt   zenith            horizon            glow               ambient(x/255)    sun(x/255)
  { -18, { 7, 9, 24 },    { 16, 20, 44 },    { 22, 24, 52 },    { 34, 40, 74 },   { 0, 0, 0 } },
  { -12, { 10, 14, 38 },  { 30, 34, 72 },    { 46, 42, 86 },    { 42, 46, 84 },   { 0, 0, 0 } },
  {  -8, { 17, 23, 60 },  { 66, 56, 100 },   { 122, 72, 102 },  { 58, 56, 96 },   { 0, 0, 0 } },
  {  -4, { 31, 42, 96 },  { 148, 92, 114 },  { 236, 120, 92 },  { 84, 72, 110 },  { 50, 24, 16 } },
  {  -1, { 49, 67, 128 }, { 232, 140, 102 }, { 255, 148, 84 },  { 108, 92, 120 }, { 150, 76, 44 } },
  {   2, { 62, 96, 160 }, { 246, 176, 120 }, { 255, 188, 110 }, { 118, 108, 126 },{ 250, 150, 86 } },
  {   6, { 66, 120, 190 },{ 236, 204, 170 }, { 255, 214, 158 }, { 126, 124, 138 },{ 255, 196, 146 } },
  {  12, { 58, 128, 208 },{ 194, 218, 238 }, { 255, 236, 206 }, { 132, 138, 152 },{ 255, 232, 204 } },
  {  25, { 44, 120, 212 },{ 168, 208, 242 }, { 255, 246, 230 }, { 138, 146, 160 },{ 255, 246, 230 } },
  {  60, { 34, 108, 210 },{ 152, 200, 244 }, { 255, 252, 242 }, { 142, 150, 166 },{ 255, 252, 244 } },
};
static const int kKeyN = sizeof(kKeys) / sizeof(kKeys[0]);

static void sampleKeys(float alt, SkyKey &o) {
  if (alt <= kKeys[0].alt) { o = kKeys[0]; return; }
  if (alt >= kKeys[kKeyN - 1].alt) { o = kKeys[kKeyN - 1]; return; }
  int i = 0;
  while (i < kKeyN - 2 && kKeys[i + 1].alt <= alt) i++;
  const SkyKey &a = kKeys[i], &b = kKeys[i + 1];
  float u = (alt - a.alt) / (b.alt - a.alt);
  u = u * u * (3 - 2 * u);
  o.alt = alt;
  o.zen = mix(a.zen, b.zen, u);
  o.hor = mix(a.hor, b.hor, u);
  o.glow = mix(a.glow, b.glow, u);
  o.amb = mix(a.amb, b.amb, u);
  o.sun = mix(a.sun, b.sun, u);
}

// Screen position of a body given its hour offset from transit and its
// half-day length: x sweeps left (rise) to right (set); y follows altitude.
static void bodyScreenPos(float hFromTransit, float halfDay, float alt,
                          int16_t horizonY, float &x, float &y) {
  float u = hFromTransit / (halfDay > 0.5f ? halfDay : 0.5f);
  if (u < -1.12f) u = -1.12f;
  if (u > 1.12f) u = 1.12f;
  x = 120.0f + 102.0f * u;
  float s = sinf((alt > 0 ? alt : 0) * kDeg);
  float rise = 150.0f * powf(s, 0.62f);
  float topLimit = horizonY - 24.0f;   // never higher than y = 24-ish
  if (rise > topLimit - 22.0f) rise = topLimit - 22.0f;
  y = horizonY - rise;
  if (alt < 0) y = horizonY - alt * 3.0f;   // sinking below the horizon
}

void computeSky(const LocalTime &t, const SkyStyle &style, SkyState &s) {
  int doy = dayOfYear(t);
  float tod = t.hour + t.minute / 60.0f + t.second / 3600.0f;
  s.tod = tod;

  // ---- sun ----
  float decl = solarDeclination(doy);
  float hSun = wrap24(tod - kSolarNoon);
  s.sunAlt = altitudeDeg(decl, hSun * 15.0f);
  s.morning = hSun < 0;
  s.sunUp = s.sunAlt > -1.0f;
  bodyScreenPos(hSun, halfDayHours(decl), s.sunAlt, style.horizonY, s.sunX, s.sunY);

  // ---- moon ----
  s.moonPhase = moonPhaseFor(t);
  s.moonLit = 0.5f * (1.0f - cosf(2.0f * kPi * s.moonPhase));
  float transit = kSolarNoon + s.moonPhase * 24.84f;
  float mDecl = decl * cosf(2.0f * kPi * s.moonPhase);
  float hMoon = wrap24(tod - transit);
  s.moonAlt = altitudeDeg(mDecl, hMoon * 15.0f);
  s.moonUp = s.moonAlt > 0.5f && s.moonLit > 0.03f;
  bodyScreenPos(hMoon, halfDayHours(mDecl), s.moonAlt, style.horizonY, s.moonX, s.moonY);

  // ---- colours ----
  SkyKey k;
  sampleKeys(s.sunAlt, k);
  s.zenith = k.zen;
  s.horizon = k.hor;
  s.glow = k.glow;
  s.ambient = k.amb;
  s.sunLight = k.sun;
  s.twilight = sat(1.0f - fabsf(s.sunAlt - 0.5f) / 9.0f);
  s.night = sat((-s.sunAlt - 2.0f) / 12.0f);
  s.starAmt = sat((-s.sunAlt - 4.0f) / 9.0f);

  // Dawn leans rose / violet, dusk leans amber / red.
  if (s.twilight > 0) {
    RGB tint = s.morning ? rgb(236, 150, 178) : rgb(255, 128, 72);
    s.horizon = mix(s.horizon, tint, 0.22f * s.twilight);
    s.glow = mix(s.glow, tint, 0.18f * s.twilight);
  }

  // Key light: the sun by day, the moon at night (dimmed by phase).
  if (s.sunAlt > -2.0f) {
    float a = (s.sunAlt < 4 ? 4 : s.sunAlt) * kDeg;
    s.lightX = (s.morning ? -1.0f : 1.0f) * cosf(a);
    s.lightY = sinf(a);
  } else {
    float a = (s.moonAlt < 8 ? 8 : s.moonAlt) * kDeg;
    s.lightX = (hMoon < 0 ? -1.0f : 1.0f) * cosf(a);
    s.lightY = sinf(a);
    if (s.moonUp) {
      float m = 0.45f * s.moonLit * s.night;
      s.sunLight = add(s.sunLight, rgb(120 * m, 140 * m, 190 * m));
    }
  }
  // Haze is the horizon colour, a touch lifted toward the zenith tint.
  s.haze = mix(s.horizon, s.zenith, 0.18f);

  // City windows: more lit in the evening than in the small hours.
  float sched;
  if (tod >= 17.0f || tod < 0.5f) sched = 0.80f;
  else if (tod < 5.0f) sched = 0.30f;
  else sched = 0.55f;
  s.windowsLit = sched * smoothstepf(-1.0f, -7.0f, s.sunAlt);

  // Clock tint: warm white by day, moonlit blue-white at night.
  s.text = mix(rgb(255, 250, 240), rgb(222, 232, 255), s.night);
  (void)style;
}

// ---------------------------------------------------------------------------
// Sky renderer
// ---------------------------------------------------------------------------
void renderSky(Layer &sky, const SkyState &s, const SkyStyle &st, TwinkleSet *tw) {
  if (tw) tw->n = 0;
  if (!sky.rgb) return;
  const int W = sky.w, H = sky.h;
  const float hy = st.horizonY;
  const float top = (float)sky.y0;            // screen y of row 0

  // Separable glow factors: around the sun (or the point on the horizon below
  // it when it has set) plus a wide, flat twilight band along the horizon.
  float gx = s.sunX, gy = s.sunUp ? s.sunY : hy;
  float sunGlow = s.sunUp ? 0.55f : 0.0f;
  sunGlow += 0.45f * s.twilight;
  float bandAmt = 0.75f * s.twilight;
  float *colG = (float *)allocBig(sizeof(float) * 2 * (size_t)W);
  if (!colG) return;
  float *colB = colG + W;
  float sig = 30.0f + 22.0f * (1.0f - s.twilight);
  for (int x = 0; x < W; x++) {
    float dx = (x + sky.x0) - gx;
    colG[x] = expf(-dx * dx / (2 * sig * sig));
    colB[x] = expf(-dx * dx / (2 * 110.0f * 110.0f));
  }

  // Stars are drawn per row from a seeded list (positions in layer space).
  const int kMaxStars = 170;
  struct Star { int16_t x, y; uint8_t b, big; };
  Star *stars = (Star *)allocBig(sizeof(Star) * kMaxStars);
  int nStars = 0;
  if (stars && s.starAmt > 0.01f) {
    Rng rng(st.seed ^ 0xA11CE5u);
    int want = (int)(150 * st.starDensity);
    if (want > kMaxStars) want = kMaxStars;
    for (int i = 0; i < want; i++) {
      Star z;
      z.x = (int16_t)rng.irange(0, W - 1);
      z.y = (int16_t)rng.irange(0, (int)(hy - top) - 6);
      float b = rng.unit();
      z.b = (uint8_t)(70 + 185 * b * b);
      z.big = (rng.unit() < 0.06f) ? 1 : 0;
      stars[nStars++] = z;
    }
  }

  const float lp = st.lightPollution * s.night;
  for (int y = 0; y < H; y++) {
    float sy = top + y;
    uint16_t *out = sky.rgb + (size_t)y * W;
    RGB base;
    bool belowHorizon = sy > hy;
    if (!belowHorizon) {
      float t = sat((sy - top) / (hy - top));
      t = powf(t, 1.7f);
      base = mix(s.zenith, s.horizon, t);
    } else {
      base = s.horizon;
    }
    float dyS = sy - gy;
    float rowG = expf(-dyS * dyS / (2 * sig * sig)) * sunGlow;
    float dyB = sy - hy;
    float rowB = expf(-dyB * dyB / (2 * 26.0f * 26.0f)) * bandAmt;
    float rowLP = lp > 0 ? expf(-dyB * dyB / (2 * 34.0f * 34.0f)) * lp : 0.0f;

    if (st.sea && belowHorizon) {
      // ---- sea: reflects the sky, darkens toward the viewer, glitter path
      // under the sun or moon, horizontal wave streaks.
      float d = sat((sy - hy) / 110.0f);
      RGB refl = mix(s.horizon, s.zenith, 0.35f + 0.4f * d);
      RGB water = mix(refl, mul(st.seaDeep, mix(rgb(255, 255, 255), s.ambient, 0.6f)),
                      0.35f + 0.5f * d);
      for (int x = 0; x < W; x++) {
        float sx = x + sky.x0;
        float wave = fbm2(sx * 0.05f, sy * 0.55f, st.seed + 77u, 3);
        RGB c = scale(water, 0.92f + 0.12f * wave);
        // glitter column under the sun / moon
        float lx = s.sunUp ? s.sunX : (s.moonUp ? s.moonX : -999.0f);
        if (lx > -900) {
          float amt = s.sunUp ? (0.55f + 0.45f * s.twilight) : 0.55f * s.moonLit;
          float spread = 8.0f + 26.0f * d;
          float dx = (sx - lx) / spread;
          float d2 = dx * dx;
          float col = d2 > 9.0f ? 0.0f : 1.0f / (1.0f + d2 + 0.5f * d2 * d2);  // ~exp(-d2)
          float sparkle = hashUnit(hash2((int)sx, (int)sy, st.seed + 5u));
          float streak = wave > 0.25f ? 1.0f : 0.25f;
          float g = col * amt * streak * (0.35f + 0.65f * sparkle);
          RGB gc = s.sunUp ? s.glow : rgb(220, 226, 240);
          c = screen(c, gc, sat(g));
        }
        c = screen(c, s.glow, sat(colB[x] * rowB * 0.35f));
        out[x] = to565Dither(c, x, y);
      }
      continue;
    }

    for (int x = 0; x < W; x++) {
      RGB c = base;
      float g = colG[x] * rowG + colB[x] * rowB;
      if (g > 0.003f) c = screen(c, s.glow, sat(g));
      if (rowLP > 0.003f) c = screen(c, rgb(255, 150, 80), sat(rowLP * 0.5f));
      out[x] = to565Dither(c, x, y);
    }
  }

  // ---- stars ----
  for (int i = 0; i < nStars; i++) {
    const Star &z = stars[i];
    if (z.x < 0 || z.x >= W || z.y < 0 || z.y >= H) continue;
    float sy = top + z.y;
    float fade = s.starAmt * smoothstepf(hy - 2.0f, hy - 70.0f, sy);
    if (fade <= 0.02f) continue;
    float b = z.b * fade / 255.0f;
    RGB starC = mix(rgb(255, 240, 220), rgb(210, 225, 255), hashUnit(hash32(i * 31u + st.seed)));
    auto plot = [&](int px, int py, float a) {
      if (px < 0 || px >= W || py < 0 || py >= H) return;
      uint16_t &o = sky.rgb[(size_t)py * W + px];
      o = to565(screen(from565(o), starC, sat(a)));
    };
    bool canTwinkle = tw && z.big && b > 0.35f && tw->n < TwinkleSet::kMax &&
                      z.x >= 1 && z.x < W - 1 && z.y >= 1 && z.y < H - 1;
    if (canTwinkle) {
      TwinkleStar &t = tw->s[tw->n++];
      t.x = z.x; t.y = z.y; t.col = starC; t.b = b; t.lastLevel = 255;
      const int ox[5] = { 0, -1, 1, 0, 0 }, oy[5] = { 0, 0, 0, -1, 1 };
      for (int k = 0; k < 5; k++) t.bg[k] = sky.rgb[(size_t)(z.y + oy[k]) * W + z.x + ox[k]];
    }
    plot(z.x, z.y, b);
    if (z.big) {
      plot(z.x - 1, z.y, b * 0.45f); plot(z.x + 1, z.y, b * 0.45f);
      plot(z.x, z.y - 1, b * 0.45f); plot(z.x, z.y + 1, b * 0.45f);
    }
  }
  // Remember what each twinkle star's centre looks like now, so stars that
  // end up under the moon or sun can be dropped below.
  uint16_t drawnCentre[TwinkleSet::kMax];
  if (tw)
    for (int i = 0; i < tw->n; i++) drawnCentre[i] = sky.rgb[(size_t)tw->s[i].y * W + tw->s[i].x];

  // ---- moon (disc with phase terminator, soft halo) ----
  if (s.moonUp) {
    float mx = s.moonX - sky.x0, my = s.moonY - sky.y0;
    const float R = 9.5f;
    float dayFade = 1.0f - 0.65f * (1.0f - s.night);   // pale by day
    RGB lit = rgb(242, 238, 224), dark = mix(s.zenith, rgb(40, 44, 70), 0.3f);
    float k = cosf(2.0f * kPi * s.moonPhase);
    bool waxing = s.moonPhase < 0.5f;
    int x0 = (int)(mx - R - 14), x1 = (int)(mx + R + 14);
    int y0 = (int)(my - R - 14), y1 = (int)(my + R + 14);
    for (int y = y0; y <= y1; y++) {
      if (y < 0 || y >= H) continue;
      for (int x = x0; x <= x1; x++) {
        if (x < 0 || x >= W) continue;
        float dx = x + 0.5f - mx, dy = y + 0.5f - my;
        float d = sqrtf(dx * dx + dy * dy);
        uint16_t &o = sky.rgb[(size_t)y * W + x];
        RGB bg = from565(o);
        // halo
        float halo = expf(-(d - R) * (d - R) / 60.0f) * 0.22f * s.night * s.moonLit;
        RGB c = d > R ? screen(bg, rgb(200, 210, 235), sat(halo)) : bg;
        float cov = sat(R + 0.5f - d);
        if (cov > 0) {
          float u = dx / R, v = dy / R;
          float hw = sqrtf(fmaxf(0.0f, 1.0f - v * v));
          float term = hw * k;
          float litAmt = waxing ? smoothstepf(term - 0.08f, term + 0.08f, u)
                                : 1.0f - smoothstepf(-term - 0.08f, -term + 0.08f, u);
          // faint maria
          float maria = 0.86f + 0.14f * noise2(dx * 0.35f + 3.0f, dy * 0.35f, 99u);
          RGB face = mix(dark, scale(lit, maria), litAmt);
          face = mix(bg, face, dayFade);
          c = mix(c, face, cov);
        }
        o = to565Dither(c, x, y);
      }
    }
  }

  // ---- sun disc ----
  if (s.sunUp && s.sunY < hy + 12) {
    float mx = s.sunX - sky.x0, my = s.sunY - sky.y0;
    const float R = 11.0f;
    RGB core = mix(rgb(255, 250, 230), rgb(255, 196, 120), s.twilight);
    for (int y = (int)(my - R - 2); y <= (int)(my + R + 2); y++) {
      if (y < 0 || y >= H) continue;
      for (int x = (int)(mx - R - 2); x <= (int)(mx + R + 2); x++) {
        if (x < 0 || x >= W) continue;
        float dx = x + 0.5f - mx, dy = y + 0.5f - my;
        float d = sqrtf(dx * dx + dy * dy);
        float cov = sat(R + 0.5f - d);
        if (cov <= 0) continue;
        uint16_t &o = sky.rgb[(size_t)y * W + x];
        o = to565Dither(mix(from565(o), core, cov), x, y);
      }
    }
  }
  if (tw) {
    int k = 0;
    for (int i = 0; i < tw->n; i++)
      if (sky.rgb[(size_t)tw->s[i].y * W + tw->s[i].x] == drawnCentre[i]) tw->s[k++] = tw->s[i];
    tw->n = k;
  }
  freeMem(colG);
  if (stars) freeMem(stars);
}

}  // namespace tp
