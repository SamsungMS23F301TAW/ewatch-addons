#include "fr_radar.h"
#include "fr_motion.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace fr {

using namespace layout;
static constexpr float kPi = 3.14159265f;

static inline uint8_t angle8(float deg) {
  int a = (int)lroundf(deg * 256.f / 360.f);
  return (uint8_t)(a & 0xFF);
}
static inline uint8_t clampU8(float v) { return v <= 0.f ? 0 : (v >= 255.f ? 255 : (uint8_t)(v + 0.5f)); }

RadarCamera RadarCamera::lockOn(float bx, float by, float u) {
  RadarCamera c;
  c.fx = lerpf((float)cx, bx, u);
  c.fy = lerpf((float)cy, by, u);
  c.tx = lerpf((float)cx, dial::kFocusX, u);
  c.ty = lerpf((float)cy, dial::kFocusY, u);
  c.s = lerpf(1.f, dial::kFocusScale, u);
  c.focus = u;
  return c;
}

// ---------------------------------------------------------------------------
// Static layer: room, bezel, glass. Built once.
// ---------------------------------------------------------------------------
static void bezelShade(float d, float dx, float dy, int x, int y, float &R, float &G, float &B) {
  float t = (d - (float)Rg) / (Ro - (float)Rg);
  if (t < 0.f) t = 0.f;
  if (t > 1.f) t = 1.f;
  float ux = dx / d, uy = dy / d;
  // Profile across the bezel: a steep inner wall down to the glass, a broad
  // rounded crown, a quick roll-off at the outer edge.
  float slope;
  if (t < 0.14f) slope = 2.6f * (1.f - t / 0.14f) + 0.35f;           // inner wall (rises outward)
  else slope = cosf(kPi * (0.5f * (t - 0.14f) / 0.86f)) * 0.35f - 1.25f * powf((t - 0.14f) / 0.86f, 3.f);
  float nx = -slope * ux, ny = -slope * uy, nz = 1.f;
  float nl = 1.f / sqrtf(nx * nx + ny * ny + 1.f);
  nx *= nl; ny *= nl; nz *= nl;
  // Key light from the top-left; a studio "window" reflected in the metal.
  const float Lx = -0.50f, Ly = -0.66f, Lz = 0.56f;
  float diff = nx * Lx + ny * Ly + nz * Lz;
  if (diff < 0.f) diff = 0.f;
  float hx = Lx, hy = Ly, hz = Lz + 1.f;
  float hl = sqrtf(hx * hx + hy * hy + hz * hz);
  float sp = (nx * hx + ny * hy + nz * hz) / hl;
  float spec = 0.f;
  if (sp > 0.f) { float s2 = sp * sp, s4 = s2 * s2, s8 = s4 * s4, s16 = s8 * s8; spec = s16 * s16 * s8; }   // ^40
  // Reflection of the eye ray; above the horizon is the bright room, below is dark.
  float ry = 2.f * nz * ny;
  float env = smoothstepf(0.10f, -0.30f, ry);
  float k = 0.10f + 0.62f * diff;
  R = 30.f * k + 34.f * env + 235.f * spec;
  G = 36.f * k + 40.f * env + 242.f * spec;
  B = 42.f * k + 46.f * env + 248.f * spec;
  float brush = grainAt((int)(d * 5.f), 7, 0x77u) * 1.6f;          // concentric brushing
  float n = grainAt(x, y, 0x31u) * 1.0f;
  R += brush + n; G += brush + n; B += brush + n;
  if (t > 0.93f) {                                                   // outer edge into shadow
    float s = (t - 0.93f) / 0.07f;
    float m = 1.f - 0.5f * s;
    R *= m; G *= m; B *= m;
  }
}

static void glassShade(float d, int x, int y, float &R, float &G, float &B) {
  float u = d / (float)Rg;
  float e = powf(u, 1.3f);
  R = lerpf(9.f, 2.f, e);
  G = lerpf(36.f, 8.f, e);
  B = lerpf(32.f, 9.f, e);
  float bloom = 1.f - u;
  bloom = bloom * bloom * bloom;
  R += 3.f * bloom; G += 16.f * bloom; B += 12.f * bloom;
  float n = grainAt(x, y, 0x5EEDu) * 1.7f;
  R += n * 0.6f; G += n; B += n * 0.9f;
  if (y % 3 == 0) { R *= 0.92f; G *= 0.92f; B *= 0.92f; }   // faint scan lines
}

void RadarScene::build() {
  if (!bg_ || !polar_ || !overlay_ || !clean_) return;
  Canvas c(bg_, W, H);
  buildPageBackground(c);

  // The instrument's soft shadow on the room.
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - (cy + 8.f);
      float d = sqrtf(dx * dx + dy * dy);
      if (d > Ro + 22.f || d < Ro - 10.f) continue;
      float t = (d - (Ro - 10.f)) / 32.f;
      float a = 1.f - t;
      pixel(c, x, y, 0x0000, clampU8(200.f * a * a));
    }
  }
  // Bezel and glass, shaded per pixel, dithered to RGB565.
  for (int y = cy - (int)Ro - 2; y <= cy + (int)Ro + 2; ++y) {
    if (y < 0 || y >= H) continue;
    for (int x = cx - (int)Ro - 2; x <= cx + (int)Ro + 2; ++x) {
      if (x < 0 || x >= W) continue;
      float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
      float d = sqrtf(dx * dx + dy * dy);
      if (d > Ro + 0.6f) continue;
      float R, G, B;
      if (d >= (float)Rg + 0.5f) {
        bezelShade(d, dx, dy, x, y, R, G, B);
      } else if (d <= (float)Rg - 0.5f) {
        glassShade(d, x, y, R, G, B);
      } else {
        float r1, g1, b1, r2, g2, b2;
        glassShade(d, x, y, r1, g1, b1);
        bezelShade(fmaxf(d, (float)Rg + 0.01f), dx, dy, x, y, r2, g2, b2);
        float k = d - ((float)Rg - 0.5f);
        R = lerpf(r1, r2, k); G = lerpf(g1, g2, k); B = lerpf(b1, b2, k);
      }
      uint16_t col = ditherRgb(R, G, B, x, y);
      float cov = Ro + 0.5f - d;
      pixel(c, x, y, col, clampU8(255.f * (cov > 1.f ? 1.f : cov)));
    }
  }
  // Silk-screened ticks on the flanks (the top and bottom carry lettering).
  for (int i = 0; i < 72; ++i) {
    int deg = i * 5;
    int side = deg <= 180 ? deg : 360 - deg;                     // 0 top .. 180 bottom
    if (side < 52 || side > 128) continue;
    float a = (float)deg * kPi / 180.f;
    bool major = (deg % 30) == 0;
    float r0 = (float)Rg + 2.6f, r1 = (float)Rg + (major ? 7.2f : 5.2f);
    float sx = sinf(a), sy = -cosf(a);
    line(c, cx + r0 * sx + 0.6f, cy + r0 * sy + 0.8f, cx + r1 * sx + 0.6f, cy + r1 * sy + 0.8f,
         0.45f, 0x0000, 150);
    line(c, cx + r0 * sx, cy + r0 * sy, cx + r1 * sx, cy + r1 * sy, major ? 0.55f : 0.4f,
         major ? hex(0xB5C2C8) : hex(0x6F7D85), major ? 220 : 170);
  }
  // Maker's lettering along the bottom of the bezel.
  textArc(c, kFontXS, cx + 0.6f, cy + 0.9f, Ro - 4.2f, 180.f, "FRIEND RADAR", 0x0000, 170, 2.4f, true);
  textArc(c, kFontXS, cx, cy, Ro - 4.2f, 180.f, "FRIEND RADAR", hex(0x9AA7AE), 220, 2.4f, true);
  // The corner buttons are part of the room; only their pressed state is drawn per frame.
  drawBackButton(c, false);
  drawMenuButton(c, false);
  // Keep the bare glass, then engrave the overview's grooves and zone names.
  for (int j = 0; j < dial::D; ++j)
    memcpy(clean_ + (size_t)j * dial::D, bg_ + (size_t)(cy - Rg + j) * W + (cx - Rg), dial::D * sizeof(uint16_t));
  drawGrooves(c, (float)cx, (float)cy, 1.f, 1.f, 255, 255);

  // Polar table for the sweep: (angle8 << 8) | radius, radius 255 = outside.
  for (int j = 0; j < dial::D; ++j) {
    float dy = (float)j - (float)Rg + 0.5f;
    for (int i = 0; i < dial::D; ++i) {
      float dx = (float)i - (float)Rg + 0.5f;
      float r = sqrtf(dx * dx + dy * dy);
      uint16_t e = 0x00FF;
      if (r <= (float)Rg + 0.5f) {
        float ang = atan2f(dx, -dy);
        if (ang < 0.f) ang += 2.f * kPi;
        uint8_t a8 = (uint8_t)((int)(ang * 256.f / (2.f * kPi)) & 0xFF);
        uint8_t rr = (uint8_t)(r > 254.f ? 254.f : r);
        e = (uint16_t)((a8 << 8) | rr);
      }
      polar_[(size_t)j * dial::D + i] = e;
    }
  }
  // Glass overlay: (gloss << 8) | darken. Inner rim shadow (deeper under the
  // top lip, where the light comes from) and a soft reflection top-left.
  for (int j = 0; j < dial::D; ++j) {
    float dy = (float)j - (float)Rg + 0.5f;
    for (int i = 0; i < dial::D; ++i) {
      float dx = (float)i - (float)Rg + 0.5f;
      float d = sqrtf(dx * dx + dy * dy);
      uint16_t m = 0;
      if (d <= (float)Rg + 0.5f) {
        float s = smoothstepf((float)Rg - 16.f, (float)Rg + 0.5f, d);
        float dark = 185.f * s * s;
        float up = d > 1.f ? -dy / d : 0.f;
        if (up > 0.f) dark += 90.f * smoothstepf((float)Rg - 34.f, (float)Rg, d) * up * up;
        // Reflection: a soft crescent of window light across the upper-left
        // glass, held off the rim by a dark gap so it reads as a curved pane.
        float ang = atan2f(dx, -dy);                       // 0 = up, clockwise
        float dir = cosf(ang + 0.785398f);                 // 1 toward the top-left
        float g = 0.f;
        if (dir > 0.f) {
          float c1x = dx + 5.f, c1y = dy + 6.f;            // crescent outer edge (shifted circle)
          float d1 = sqrtf(c1x * c1x + c1y * c1y);
          float c2x = dx - 7.f, c2y = dy - 3.f;            // inner circle bites the crescent
          float d2 = sqrtf(c2x * c2x + c2y * c2y);
          float band = smoothstepf((float)Rg - 6.f, (float)Rg - 12.f, d1) *
                       smoothstepf((float)Rg - 26.f, (float)Rg - 14.f, d2);
          g += 46.f * band * dir * dir;
          // A thin bright edge on the crescent's outer boundary.
          float e = (d1 - ((float)Rg - 9.f)) / 1.3f;
          g += 60.f * expf(-e * e) * powf(dir, 4.f) * smoothstepf((float)Rg - 26.f, (float)Rg - 14.f, d2);
        }
        if (dark > 255.f) dark = 255.f;
        if (g > 255.f) g = 255.f;
        m = (uint16_t)(((uint16_t)g << 8) | (uint16_t)dark);
      }
      overlay_[(size_t)j * dial::D + i] = m;
    }
  }
  // Sweep tiles: the arc of angles each 16 px tile covers (span 0 = outside
  // the glass, 255 = contains the centre, i.e. every angle).
  for (int ty = 0; ty < kTiles; ++ty) {
    for (int tx = 0; tx < kTiles; ++tx) {
      const int t = ty * kTiles + tx;
      bool any = false, centre = false;
      int ref = -1, lo = 0, hi = 0;
      for (int j = ty * kTile; j < ty * kTile + kTile && j < dial::D; ++j) {
        for (int i = tx * kTile; i < tx * kTile + kTile && i < dial::D; ++i) {
          uint16_t e = polar_[(size_t)j * dial::D + i];
          if ((uint8_t)e > Rg) continue;
          if ((uint8_t)e < 2) centre = true;
          int a = e >> 8;
          if (ref < 0) { ref = a; any = true; }
          int d = (int)(int8_t)(uint8_t)(a - ref);
          if (d < lo) lo = d;
          if (d > hi) hi = d;
        }
      }
      if (!any) { tileSpan_[t] = 0; continue; }
      if (centre || hi - lo >= 250) { tileA0_[t] = 0; tileSpan_[t] = 255; continue; }
      tileA0_[t] = (uint8_t)(ref + lo);
      tileSpan_[t] = (uint8_t)(hi - lo + 1);
    }
  }
  // Glass pass: per row, the first/last non-zero overlay entries and the
  // clear run in the middle.
  for (int j = 0; j < dial::D; ++j) {
    const uint16_t *m = overlay_ + (size_t)j * dial::D;
    int first = dial::D, last = -1;
    for (int i = 0; i < dial::D; ++i) if (m[i]) { if (first == dial::D) first = i; last = i; }
    if (last < 0) { ovFirst_[j] = ovZ0_[j] = ovZ1_[j] = ovLast_[j] = 0; continue; }
    int bestA = last + 1, bestB = last + 1, runA = -1;
    for (int i = first; i <= last + 1; ++i) {
      bool zero = i <= last && !m[i];
      if (zero && runA < 0) runA = i;
      if (!zero && runA >= 0) {
        if (i - runA > bestB - bestA) { bestA = runA; bestB = i; }
        runA = -1;
      }
    }
    ovFirst_[j] = (uint8_t)first;
    ovZ0_[j] = (uint8_t)(bestB > bestA ? bestA : last + 1);
    ovZ1_[j] = (uint8_t)(bestB > bestA ? bestB : last + 1);
    ovLast_[j] = (uint8_t)(last + 1);
  }
  for (int d = 0; d < kTrail; ++d) trail_[d] = clampU8(150.f * expf(-(float)d / 24.f));
  for (int r = 0; r < 256; ++r) radial_[r] = clampU8(255.f * (0.45f + 0.55f * fminf(1.f, (float)r / Rg)));
  built_ = true;
}

// ---------------------------------------------------------------------------
// Per-frame passes
// ---------------------------------------------------------------------------
void RadarScene::drawBeam(Canvas &out, float beamDeg, uint8_t alpha) const {
  // Phosphor afterglow: exponential decay behind the leading edge.
  uint8_t trail[kTrail];
  for (int d = 0; d < kTrail; ++d) trail[d] = (uint8_t)((trail_[d] * alpha + 127) / 255);
  const uint8_t beam8 = angle8(beamDeg);
  for (int ty = 0; ty < kTiles; ++ty) {
    for (int tx = 0; tx < kTiles; ++tx) {
      const int t = ty * kTiles + tx;
      const uint8_t span = tileSpan_[t];
      if (span == 0) continue;                                   // tile outside the glass
      const uint8_t off = (uint8_t)(beam8 - tileA0_[t]);
      if (span != 255 && off > span && (uint8_t)(off - span) >= kTrail) continue;   // trail misses it
      const int j0 = ty * kTile, j1 = j0 + kTile < dial::D ? j0 + kTile : dial::D;
      const int i0 = tx * kTile, i1 = i0 + kTile < dial::D ? i0 + kTile : dial::D;
      for (int j = j0; j < j1; ++j) {
        const int y = cy - Rg + j;
        if (y < out.cy0 || y >= out.cy1) continue;
        uint16_t *row = out.px + (size_t)y * out.w + (cx - Rg);
        const uint16_t *pr = polar_ + (size_t)j * dial::D;
        for (int i = i0; i < i1; ++i) {
          const uint16_t e = pr[i];
          const uint8_t r = (uint8_t)e;
          if (r > Rg) continue;
          const uint8_t d = (uint8_t)(beam8 - (uint8_t)(e >> 8));
          if (d >= kTrail) continue;
          const uint8_t a = trail[d];
          if (a) row[i] = addColor(row[i], pal::phos, (uint8_t)((a * radial_[r]) >> 8));
        }
      }
    }
  }
  float ang = beamDeg * kPi / 180.f;
  float ex = cx + (float)(Rg - 2) * sinf(ang), ey = cy - (float)(Rg - 2) * cosf(ang);
  line(out, (float)cx, (float)cy, ex, ey, 2.4f, pal::phos, (uint8_t)(50 * alpha / 255));
  line(out, (float)cx, (float)cy, ex, ey, 0.6f, pal::phosHot, (uint8_t)(210 * alpha / 255));
}

void RadarScene::blipOverviewXY(const Blip &b, float &x, float &y) {
  float a = b.angleDeg * kPi / 180.f;
  x = (float)cx + b.radius * sinf(a);
  y = (float)cy - b.radius * cosf(a);
}

void RadarScene::blipScreenXY(const Blip &b, const RadarCamera &cam, float &x, float &y) {
  float ox, oy;
  blipOverviewXY(b, ox, oy);
  cam.apply(ox, oy, x, y);
}

int RadarScene::hitBlip(const Blip *blips, int n, const RadarCamera &cam, int px, int py) {
  int best = -1;
  float bestD = 24.f * 24.f;
  for (int i = 0; i < n; ++i) {
    if (blips[i].alpha < 60) continue;
    float x, y;
    blipScreenXY(blips[i], cam, x, y);
    float dx = x - (float)cx, dy = y - (float)cy;
    if (dx * dx + dy * dy > (float)(Rg * Rg)) continue;     // off the glass
    float d = (x - px) * (x - px) + (y - py) * (y - py);
    if (d < bestD) { bestD = d; best = i; }
  }
  return best;
}

namespace {
struct Box {
  int x0, y0, x1, y1;
  bool hits(const Box &o) const { return x0 < o.x1 && o.x0 < x1 && y0 < o.y1 && o.y0 < y1; }
};
bool insideGlass(const Box &b) {
  const float r2 = (float)((Rg - 3) * (Rg - 3));
  float xs[2] = {(float)b.x0, (float)b.x1}, ys[2] = {(float)b.y0, (float)b.y1};
  for (float x : xs)
    for (float y : ys)
      if ((x - cx) * (x - cx) + (y - cy) * (y - cy) > r2) return false;
  return true;
}
}  // namespace

void RadarScene::drawBlips(Canvas &out, const RadarFrame &f) const {
  const RadarCamera &cam = f.cam;
  const float u = cam.focus;
  const bool live = f.hud.radio == RadioState::Live || f.hud.radio == RadioState::Starting;
  const float beamVis = live ? (1.f - smoothstepf(0.f, 0.25f, u)) * smoothstepf(0.35f, 1.f, f.boot) : 0.f;
  const uint8_t beam8 = angle8(f.beamDeg);
  const float t = (float)f.tMs;
  const int n = f.nBlips < kMaxPeers ? f.nBlips : kMaxPeers;
  float bx[kMaxPeers], by[kMaxPeers], br[kMaxPeers];

  // Discs. Paint order: other EWatches, mates, then the focused one on top.
  for (int pass = 0; pass < 3; ++pass) {
    for (int i = 0; i < n; ++i) {
      const Blip &k = f.blips[i];
      bool focused = f.focusId && k.id == f.focusId;
      int want = focused ? 2 : (k.mate ? 1 : 0);
      if (want != pass) continue;
      blipScreenXY(k, cam, bx[i], by[i]);
      float x = bx[i], y = by[i];
      float s = k.scale * cam.s;
      float dim = focused ? 1.f : 1.f - 0.55f * clamp01f(u);
      uint8_t A = clampU8((float)k.alpha * dim * smoothstepf(0.2f, 0.8f, f.boot));
      br[i] = (k.mate ? 8.5f : 3.f) * s;
      if (!A) continue;
      uint8_t since = (uint8_t)(beam8 - angle8(k.angleDeg));
      float ping = expf(-(float)since / 34.f) * beamVis;
      if (k.zone == Zone::Lost) {
        ring(out, x, y, (k.mate ? 7.5f : 3.5f) * s, 0.9f, pal::zoneLost, (uint8_t)(A * 0.85f));
        continue;
      }
      if (k.mate) {
        float period = lerpf(2600.f, 1100.f, k.signal);
        float phase = (float)(mix32(k.id) & 1023) / 1023.f * 6.2831853f;
        float breath = 0.5f + 0.5f * sinf(t / period * 6.2831853f + phase);
        float halo = (0.30f + 0.55f * k.signal) * (0.6f + 0.4f * breath) + 0.65f * ping;
        drawOrb(out, x, y, br[i], k.color, halo, k.label[0], A);
        if (ping > 0.04f)
          ring(out, x, y, br[i] + 3.f + (1.f - ping) * 9.f, 0.8f, k.color, clampU8(150.f * ping * A / 255.f));
      } else {
        glow(out, x, y, 7.f * s, pal::stranger, clampU8((30.f + 120.f * ping) * A / 255.f));
        fillCircle(out, x, y, 2.4f * s, blend(pal::phosMid, pal::phosHot, clampU8(60.f + 195.f * ping)), A);
      }
      if (k.asleep) iconMoon(out, x + br[i] + 3.f, y - br[i] - 2.f, 8.f, pal::textDim, hex(0x061512), A);
    }
  }

  // Lock-on reticle around the focused mate.
  if (f.focusId && u > 0.05f) {
    for (int i = 0; i < n; ++i) {
      if (f.blips[i].id != f.focusId) continue;
      float x = bx[i], y = by[i];
      float hs = br[i] + 8.f + 16.f * (1.f - clamp01f(u));
      uint8_t a = clampU8(230.f * smoothstepf(0.15f, 0.8f, u));
      const float arm = 6.f;
      for (int q = 0; q < 4; ++q) {
        float sx = (q & 1) ? 1.f : -1.f, sy = (q & 2) ? 1.f : -1.f;
        float px = x + sx * hs, py = y + sy * hs;
        line(out, px, py, px - sx * arm, py, 0.8f, pal::phos, a);
        line(out, px, py, px, py - sy * arm, 0.8f, pal::phos, a);
      }
    }
  }

  // Names, placed greedily: inside the glass, clear of blips, each other and
  // the zone-label sector. Hidden while the camera is locked on.
  uint8_t labelA = clampU8(255.f * (1.f - smoothstepf(0.f, 0.3f, u)) * smoothstepf(0.5f, 1.f, f.boot));
  if (!labelA) return;
  const bool strangerLabels = n <= 4;
  Box taken[2 * kMaxPeers + 3];
  int nt = 0;
  for (int i = 0; i < n; ++i) {
    if (f.blips[i].alpha < 40) continue;
    int r = (int)br[i] + 3;
    taken[nt++] = {(int)bx[i] - r, (int)by[i] - r, (int)bx[i] + r, (int)by[i] + r};
  }
  // The zone names along the bottom arc.
  taken[nt++] = {cx - 34, cy + 36, cx + 34, cy + 92};
  for (int pass = 0; pass < 2; ++pass) {
    for (int i = 0; i < n; ++i) {
      const Blip &k = f.blips[i];
      if ((k.mate ? 0 : 1) != pass || k.alpha < 40) continue;
      if (!(k.mate || strangerLabels)) continue;
      char buf[16];
      fitText(kFontS, k.label, 80, buf, sizeof buf);
      int tw = textWidth(kFontS, buf);
      const int x = (int)lroundf(bx[i]), y = (int)lroundf(by[i]);
      const int gap = (int)br[i] + 6;
      bool inwardLeft = x > cx;   // prefer the side facing the centre
      int cand[4][2] = {{inwardLeft ? x - gap - tw : x + gap, y + 5},
                        {inwardLeft ? x + gap : x - gap - tw, y + 5},
                        {x - tw / 2, y - gap - 3},
                        {x - tw / 2, y + gap + 13}};
      int pick = -1;
      for (int pc = 0; pc < 4 && pick < 0; ++pc) {
        Box bb = {cand[pc][0] - 2, cand[pc][1] - 12, cand[pc][0] + tw + 2, cand[pc][1] + 4};
        if (!insideGlass(bb)) continue;
        bool clash = false;
        for (int q = 0; q < nt && !clash; ++q) {
          const Box &o = taken[q];
          if (abs((o.x0 + o.x1) / 2 - x) <= 1 && abs((o.y0 + o.y1) / 2 - y) <= 1) continue;   // own disc
          clash = bb.hits(o);
        }
        if (!clash) pick = pc;
      }
      if (pick < 0) pick = 0;
      int tx = cand[pick][0], base = cand[pick][1];
      if (nt < (int)(sizeof taken / sizeof taken[0])) taken[nt++] = {tx - 2, base - 12, tx + tw + 2, base + 4};
      uint8_t A = (uint8_t)((uint32_t)labelA * k.alpha / 255);
      bool lost = k.zone == Zone::Lost;
      uint16_t lc = lost ? pal::textFaint : (k.mate ? blend(pal::text, k.color, 60) : pal::textDim);
      textShadowed(out, kFontS, tx, base, buf, lc, A, hex(0x010504), 200);
    }
  }
}

void RadarScene::drawGrooves(Canvas &out, float ccx, float ccy, float s, float boot, uint8_t contentA,
                             uint8_t labelA) {
  // Etched zone grooves: a dark cut with a faint phosphor catch-light below.
  for (int i = 0; i < 3; ++i) {
    float grow = easeOutCubicf(clamp01f(boot * 1.7f - (float)i * 0.18f));
    float rr = ringR[i] * s * grow;
    if (rr < 1.f) continue;
    ring(out, ccx, ccy + 1.f, rr, 0.55f, hex(0x1C5C4A), (uint8_t)(90 * contentA / 255));
    ring(out, ccx, ccy, rr, 0.6f, hex(0x000302), (uint8_t)(220 * contentA / 255));
  }
  // Zone names set along the bottom arc, inside each band.
  if (labelA) {
    static const char *kNames[3] = {"NEAR", "AROUND", "FAR"};
    static const float kR[3] = {ringR[1] - 3.5f, ringR[2] - 3.5f, (float)Rg - 6.f};
    for (int i = 0; i < 3; ++i)
      textArc(out, kFontXS, ccx, ccy, kR[i] * s, 180.f, kNames[i], hex(0x2E8F72), labelA, 2.2f, true);
  }
}

void RadarScene::drawContent(Canvas &out, const RadarFrame &f, bool grooves) const {
  const RadarCamera &cam = f.cam;
  const float u = cam.focus;
  float ccx, ccy;
  cam.apply((float)cx, (float)cy, ccx, ccy);
  const uint8_t contentA = clampU8(255.f * smoothstepf(0.05f, 0.55f, f.boot));
  const bool live = f.hud.radio == RadioState::Live;
  if (grooves)
    drawGrooves(out, ccx, ccy, cam.s, f.boot, contentA, clampU8((float)contentA * (1.f - smoothstepf(0.f, 0.35f, u))));
  // The sweep (only meaningful in the overview: it fades as the camera moves).
  if (f.hud.radio == RadioState::Live || f.hud.radio == RadioState::Starting) {
    uint8_t beamA = clampU8(255.f * (1.f - smoothstepf(0.f, 0.25f, u)) * smoothstepf(0.35f, 1.f, f.boot));
    if (beamA) drawBeam(out, f.beamDeg, beamA);
  }
  // You: a small lamp at the centre, broadcasting ripples while on the air.
  if (live && contentA) {
    const uint32_t period = 2600;
    for (int k = 0; k < 2; ++k) {
      float p = (float)((f.tMs + k * period / 2) % period) / (float)period;
      float r = (6.f + p * ((float)Rg - 10.f)) * cam.s;
      ring(out, ccx, ccy, r, 0.7f, pal::phos, clampU8(90.f * (1.f - p) * (1.f - p) * contentA / 255.f));
    }
  }
  glow(out, ccx, ccy, 13.f * cam.s, pal::phos, (uint8_t)(70 * contentA / 255));
  sphere(out, ccx, ccy, 3.6f * cam.s, pal::phosHot, contentA);
  drawBlips(out, f);
}

void RadarScene::restoreOutsideGlass(Canvas &out) const {
  // Content is clipped to the glass's bounding square; halos, zoomed grooves
  // and confetti may still spill into its corners. Put the bezel back there.
  const float rr = (float)Rg - 0.5f;
  const int sx0 = cx - Rg, sx1 = cx + Rg;              // square columns [sx0, sx1]
  for (int j = 0; j < dial::D; ++j) {
    const int y = cy - Rg + j;
    if (y < 0 || y >= H) continue;
    uint16_t *dst = out.px + (size_t)y * W;
    const uint16_t *src = bg_ + (size_t)y * W;
    float dy = (float)y + 0.5f - cy;
    if (fabsf(dy) >= rr) { memcpy(dst + sx0, src + sx0, (size_t)(sx1 - sx0 + 1) * sizeof(uint16_t)); continue; }
    float xin = sqrtf(rr * rr - dy * dy);
    int l = (int)ceilf((float)cx - xin), r = (int)floorf((float)cx + xin);
    if (l > sx0) memcpy(dst + sx0, src + sx0, (size_t)(l - sx0) * sizeof(uint16_t));
    if (r < sx1) memcpy(dst + r + 1, src + r + 1, (size_t)(sx1 - r) * sizeof(uint16_t));
  }
}

void RadarScene::applyGlass(Canvas &out) const {
  for (int j = 0; j < dial::D; ++j) {
    int y = cy - Rg + j;
    if (y < 0 || y >= H) continue;
    uint16_t *row = out.px + (size_t)y * W + (cx - Rg);
    const uint16_t *m = overlay_ + (size_t)j * dial::D;
    auto pass = [&](int i0, int i1) {
      for (int i = i0; i < i1; ++i) {
        uint16_t v = m[i];
        if (!v) continue;
        uint16_t p = row[i];
        if (v & 0xFF) p = blend(p, 0x0000, (uint8_t)(v & 0xFF));
        if (v >> 8) p = addColor(p, 0xFFFF, (uint8_t)(v >> 8));
        row[i] = p;
      }
    };
    pass(ovFirst_[j], ovZ0_[j]);
    pass(ovZ1_[j], ovLast_[j]);
  }
}

void RadarScene::drawChrome(Canvas &out, const RadarFrame &f) const {
  // On-air lamp and lettering along the top of the bezel.
  char label[40];
  uint16_t led = pal::zoneLost;
  float glowK = 0.f;
  float pulse = 0.5f + 0.5f * sinf((float)f.tMs * 0.0042f);
  switch (f.hud.radio) {
    case RadioState::Live: {
      char up[kMaxName + 1];
      size_t i = 0;
      for (; f.hud.myName[i] && i < kMaxName; ++i) {
        char ch = f.hud.myName[i];
        up[i] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch;
      }
      up[i] = 0;
      snprintf(label, sizeof label, "VISIBLE AS %s", up);
      led = pal::live;
      glowK = 0.55f + 0.45f * pulse;
      break;
    }
    case RadioState::Starting:
      snprintf(label, sizeof label, "STARTING RADIO");
      led = pal::warn;
      glowK = pulse;
      break;
    case RadioState::Error:
      snprintf(label, sizeof label, "RADIO UNAVAILABLE");
      led = pal::danger;
      glowK = 0.7f;
      break;
    default:
      snprintf(label, sizeof label, "NOT VISIBLE");
      break;
  }
  const float base = (float)Rg + 7.6f;
  float half = textArcHalfSpanDeg(kFontS, base, label, 1.6f);
  if (half > 46.f && f.hud.radio == RadioState::Live) {   // a long name would run into the ticks
    snprintf(label, sizeof label, "VISIBLE");
    half = textArcHalfSpanDeg(kFontS, base, label, 1.6f);
  }
  const float lampDeg = -(half + 5.5f);
  textArc(out, kFontS, cx, cy, base, 2.8f, label, hex(0xC9D5DA), 235, 1.6f, false);
  float la = lampDeg * kPi / 180.f;
  float lx = cx + (base + 4.5f) * sinf(la), ly = cy - (base + 4.5f) * cosf(la);
  fillCircle(out, lx + 0.4f, ly + 0.6f, 4.6f, hex(0x5A666E), 120);   // socket lip catching light
  fillCircle(out, lx, ly, 4.4f, hex(0x030506));                        // recessed socket
  drawLed(out, lx, ly, led, glowK);

  if (f.backPressed) drawBackButton(out, true);
  if (f.menuPressed) drawMenuButton(out, true);

  if (f.hud.footer[0] && f.cardY >= (float)H - 1.f) {
    char ft[56];
    fitText(kFontS, f.hud.footer, 220, ft, sizeof ft);
    int w = textWidth(kFontS, ft);
    textShadowed(out, kFontS, W / 2 - w / 2, 273, ft, pal::textDim, 255, 0x0000, 200);
  }
}

void RadarScene::drawCard(Canvas &out, const RadarFrame &f) const {
  const CardInfo &k = f.card;
  const int y0 = (int)lroundf(f.cardY);
  const int dy = y0 - cardTop;                        // shift from the open position
  // A dark glass sheet: shadow onto the scope, gradient body, lit top edge.
  softShadow(out, 0, y0, W, H - y0 + 40, 24.f, 20.f, 170, -4);
  fillRoundRectV(out, 0, y0, W, H - y0 + 40, 24.f, hex(0x15212A), hex(0x0A1015), 250);
  strokeRoundRect(out, 0, y0, W, H - y0 + 40, 24.f, 0.5f, hex(0x34474F), 200);
  for (int x = 24; x < W - 24; ++x) {
    float t = (float)(x - 24) / (float)(W - 48);
    pixel(out, x, y0 + 1, 0xFFFF, clampU8(18.f + 40.f * sinf(t * kPi)));
  }
  fillRoundRect(out, W / 2 - 16, y0 + 8, 32, 4, 2.f, hex(0x4C5F68));
  if (!k.show) return;

  // Identity: orb, name, signal.
  float ox = 32.f, oy = (float)(y0 + 38);
  if (k.mate) {
    drawOrb(out, ox, oy, 12.f, k.color, 0.45f + 0.45f * k.signal, k.name[0]);
  } else {
    glow(out, ox, oy, 15.f, pal::stranger, 120);
    fillCircle(out, ox, oy, 4.5f, pal::phosHot);
  }
  const int nameX = 56;
  const int nameMax = 186 - nameX - (k.mate ? 18 : 0);
  const Font &nf = textWidth(kFontL, k.name) <= nameMax ? kFontL : kFontMB;
  char name[20];
  fitText(nf, k.name, nameMax, name, sizeof name);
  int endX = text(out, nf, nameX, y0 + 45, name, pal::text);
  if (k.mate) iconHeart(out, (float)endX + 10.f, (float)(y0 + 37), 6.f, k.color);
  drawSignalMeter(out, 196, y0 + 44, k.bars, k.zone == Zone::Lost ? pal::zoneLost : pal::phos);

  // The zone, set large; metres only as a quiet aside.
  uint16_t zc = zoneColor(k.zone);
  int zx = text(out, kFontMB, nameX, y0 + 68, zoneName(k.zone), zc);
  if (k.zone != Zone::Lost) {
    char dist[24], buf[32];
    formatApproxMeters(k.distM, dist, sizeof dist);
    snprintf(buf, sizeof buf, "  %s", dist);
    text(out, kFontS, zx, y0 + 68, buf, pal::textDim);
  }
  char ago[24], line3[48];
  formatAgo(k.agoSec, ago, sizeof ago);
  for (char *p = ago; *p; ++p) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
  if (k.asleep) snprintf(line3, sizeof line3, "SEEN %s " FR_MIDDOT " ASLEEP", ago);
  else snprintf(line3, sizeof line3, "SEEN %s", ago);
  drawCaps(out, nameX, y0 + 86, line3, pal::textFaint, 255, 2);

  // Actions.
  Rect l = cardBtnL, r = cardBtnR, wide = cardBtnWide;
  l.y = (int16_t)(l.y + dy); r.y = (int16_t)(r.y + dy); wide.y = (int16_t)(wide.y + dy);
  if (k.mate) {
    drawPill(out, l, "Rename", PillStyle::Glass, k.pressed == 1);
    drawPill(out, r, k.confirmRemove ? "Sure?" : "Remove",
             k.confirmRemove ? PillStyle::DangerSolid : PillStyle::Danger, k.pressed == 2);
  } else {
    drawPill(out, wide, "Add as mate", PillStyle::Primary, k.pressed == 1);
  }
}

void RadarScene::drawFrame(Canvas &out, const RadarFrame &f) const {
  if (!built_) return;
  memcpy(out.px, bg_, dial::kBgPixels * sizeof(uint16_t));
  // The static layer already has the overview's grooves; anything else
  // (zoomed, powering on) starts from the bare glass.
  const bool overview = f.cam.focus == 0.f && f.cam.s == 1.f && f.boot >= 1.f &&
                        f.cam.tx == f.cam.fx && f.cam.ty == f.cam.fy;
  if (!overview)
    for (int j = 0; j < dial::D; ++j)
      memcpy(out.px + (size_t)(cy - Rg + j) * W + (cx - Rg), clean_ + (size_t)j * dial::D, dial::D * sizeof(uint16_t));
  out.clip(Rect(cx - Rg, cy - Rg, dial::D, dial::D));
  drawContent(out, f, !overview);
  if (f.anim) drawPairAnim(out, *f.anim, f.animT);
  out.resetClip();
  restoreOutsideGlass(out);
  applyGlass(out);
  drawChrome(out, f);
  if (f.cardY < (float)H - 0.5f) drawCard(out, f);
}

// ---------------------------------------------------------------------------
// Blip animation
// ---------------------------------------------------------------------------
float BlipAnimator::targetRadius(Zone z, float pos) {
  int i = (int)z;
  if (i < 0 || i > 3) i = 3;
  if (pos < 0.f) pos = 0.f;
  if (pos > 1.f) pos = 1.f;
  return bandLo[i] + pos * (bandHi[i] - bandLo[i]);
}

float BlipAnimator::signalFor(float plDb) { return clamp01f((28.f - plDb) / 34.f); }

void BlipAnimator::update(const PeerView *peers, int n, uint32_t nowMs) {
  for (auto &t : tr_) t.present = false;
  for (int i = 0; i < n; ++i) {
    const PeerView &p = peers[i];
    Track *t = nullptr, *freeT = nullptr;
    for (auto &q : tr_) {
      if (q.used && q.id == p.id) { t = &q; break; }
      if (!q.used && !freeT) freeT = &q;
    }
    bool fresh = false;
    if (!t) {
      if (!freeT) continue;
      t = freeT;
      *t = Track();
      t->used = true;
      t->id = p.id;
      t->bornMs = nowMs;
      fresh = true;
    }
    t->present = true;
    memcpy(t->label, p.label, sizeof t->label);
    t->mate = p.mate;
    t->asleep = (p.flags & kFlagBackground) != 0;
    t->zone = p.zone;
    if (p.zone != Zone::Lost) {
      t->target = targetRadius(p.zone, p.bandPos);
      t->alphaTarget = 255.f;
      t->signal = signalFor(p.plDb);
    } else {
      t->alphaTarget = 120.f;
      t->signal = 0.f;
    }
    if (fresh) {
      t->r = (p.zone == Zone::Lost) ? targetRadius(Zone::Far, 1.f) : t->target;
      t->target = t->r;
      if (p.zone != Zone::Lost) t->target = targetRadius(p.zone, p.bandPos);
      t->alpha = 0.f;
    }
  }
  for (auto &t : tr_) if (t.used && !t.present) t.alphaTarget = 0.f;
}

void BlipAnimator::step(uint32_t nowMs) {
  uint32_t dt = lastMs_ ? nowMs - lastMs_ : 0;
  if (dt > 250) dt = 250;
  lastMs_ = nowMs;
  float k = 1.f - expf(-(float)dt / 320.f);
  float da = (float)dt * 255.f / 380.f;
  for (auto &t : tr_) {
    if (!t.used) continue;
    t.r += (t.target - t.r) * k;
    if (t.alpha < t.alphaTarget) t.alpha = fminf(t.alphaTarget, t.alpha + da);
    else if (t.alpha > t.alphaTarget) t.alpha = fmaxf(t.alphaTarget, t.alpha - da);
    if (t.alphaTarget == 0.f && t.alpha <= 0.f) t = Track();
  }
}

static float easeOutBackLocal(float t) {
  const float c1 = 1.70158f, c3 = c1 + 1.f;
  float u = t - 1.f;
  return 1.f + c3 * u * u * u + c1 * u * u;
}

int BlipAnimator::blips(Blip *out, int max, uint32_t selectedId) const {
  int n = 0;
  for (const auto &t : tr_) {
    if (!t.used || n >= max) continue;
    Blip b;
    b.id = t.id;
    memcpy(b.label, t.label, sizeof b.label);
    b.mate = t.mate;
    b.asleep = t.asleep;
    b.zone = t.present ? t.zone : Zone::Lost;
    b.angleDeg = (float)blipAngleDeg(t.id);
    b.radius = t.r;
    b.alpha = clampU8(t.alpha);
    uint32_t age = lastMs_ - t.bornMs;
    b.scale = age < 520 ? fmaxf(0.f, easeOutBackLocal((float)age / 520.f)) : 1.f;
    b.selected = (t.id == selectedId);
    b.color = t.mate ? mateColor(t.id) : pal::stranger;
    b.signal = t.signal;
    out[n++] = b;
  }
  return n;
}

bool BlipAnimator::find(uint32_t id, Blip &out) const {
  Blip tmp[kMaxPeers];
  int n = blips(tmp, kMaxPeers, 0);
  for (int i = 0; i < n; ++i) if (tmp[i].id == id) { out = tmp[i]; return true; }
  return false;
}

}  // namespace fr
