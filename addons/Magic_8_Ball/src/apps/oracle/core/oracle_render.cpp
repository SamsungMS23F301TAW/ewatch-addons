// The Arduino build compiles everything at -Os; these per-pixel and
// per-sample loops run every frame, so they get -O2 on the watch.
#if defined(ESP_PLATFORM) && defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif
#include "oracle_render.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace oracle {

namespace {

// ------------------------------------------------------------ palette
// 0..255 sRGB-ish values; the panel shows RGB565 directly.
struct C3 { float r, g, b; };

constexpr C3 kLiqLo   = {1, 2, 12};       // liquid at the dark rim
constexpr C3 kLiqHi   = {9, 34, 118};     // liquid where light pools
constexpr C3 kFaceLo  = {22, 44, 168};    // die face, shadowed
constexpr C3 kFaceHi  = {74, 120, 255};   // die face, lit
constexpr C3 kText    = {244, 248, 255};
constexpr C3 kGlow    = {120, 170, 255};
constexpr C3 kGoldLo  = {170, 104, 10};
constexpr C3 kGoldHi  = {255, 222, 120};
constexpr C3 kGoldTxt = {60, 30, 0};
constexpr C3 kGoldGlw = {255, 236, 170};
constexpr C3 kBubble  = {170, 215, 255};
constexpr C3 kSpark   = {255, 214, 110};
constexpr C3 kMote    = {110, 150, 225};
constexpr C3 kPrompt  = {150, 196, 255};
constexpr C3 kPromptG = {30, 80, 230};
constexpr C3 kLabel   = {150, 158, 172};

constexpr int   kPad = 2;           // sprite padding (texels)
// Label strip on the ball below the window (rows, full width).
constexpr int   kLabelY0 = (int)(kBallCY + kWinR + kBevelW) + 1;
constexpr int   kLabelRows = kScreenH - kLabelY0;
constexpr float kHaloR = 13.f;      // die contact shadow reach (local px)

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float smoothstep(float e0, float e1, float x) {
  float t = clampf((x - e0) / (e1 - e0), 0.f, 1.f);
  return t * t * (3.f - 2.f * t);
}
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline int   clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

const uint8_t kBayer[4][4] = {
    {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

// 8.8 fixed channels -> dithered RGB565.
inline uint16_t pack565(int r, int g, int b, int bayer) {
  r += bayer << 7;  g += bayer << 6;  b += bayer << 7;
  int r5 = r >> 11, g6 = g >> 10, b5 = b >> 11;
  if (r5 > 31) r5 = 31; if (r5 < 0) r5 = 0;
  if (g6 > 63) g6 = 63; if (g6 < 0) g6 = 0;
  if (b5 > 31) b5 = 31; if (b5 < 0) b5 = 0;
  return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

inline void unpack565(uint16_t c, int &r, int &g, int &b) {
  r = ((c >> 11) & 31) * 255 / 31;
  g = ((c >> 5) & 63) * 255 / 63;
  b = (c & 31) * 255 / 31;
}

// Exact signed distance from point p to a triangle (negative inside), plus
// the outward unit gradient (gx, gy) from the nearest feature.
float sdTriangle(float px, float py, const float *vx, const float *vy,
                 float *gx = nullptr, float *gy = nullptr) {
  float best = 1e9f, bdx = 0, bdy = 0;
  bool inside = true;
  for (int i = 0; i < 3; i++) {
    int j = (i + 1) % 3;
    float ex = vx[j] - vx[i], ey = vy[j] - vy[i];
    float wx = px - vx[i], wy = py - vy[i];
    float t = clampf((wx * ex + wy * ey) / (ex * ex + ey * ey), 0.f, 1.f);
    float dx = wx - ex * t, dy = wy - ey * t;
    float d2 = dx * dx + dy * dy;
    if (d2 < best) { best = d2; bdx = dx; bdy = dy; }
    if (ex * wy - ey * wx < 0) inside = false;   // clockwise winding
  }
  float d = sqrtf(best);
  if (gx && gy) {
    float s = inside ? -1.f : 1.f;   // vector from the edge toward p, flipped inside
    if (d > 1e-5f) { *gx = s * bdx / d; *gy = s * bdy / d; }
    else           { *gx = 0; *gy = 0; }
  }
  return inside ? -d : d;
}

// Triangle vertices (centroid at origin), clockwise on screen.
void dieVertices(float R, float *vx, float *vy) {
  // Pointing up: apex at top. Screen y grows downward.
  float c = 0.8660254f * R, h = 0.5f * R;
  if (!kDiePointsDown) {
    vx[0] = 0;  vy[0] = -R;
    vx[1] = c;  vy[1] = h;
    vx[2] = -c; vy[2] = h;
  } else {
    vx[0] = -c; vy[0] = -h;
    vx[1] = c;  vy[1] = -h;
    vx[2] = 0;  vy[2] = R;
  }
}

void boxBlur(uint8_t *buf, uint8_t *tmp, int w, int h, int r) {
  int win = 2 * r + 1;
  for (int y = 0; y < h; y++) {           // horizontal into tmp
    const uint8_t *src = buf + y * w;
    uint8_t *dst = tmp + y * w;
    int acc = 0;
    for (int x = -r; x <= r; x++) acc += src[clampi(x, 0, w - 1)];
    for (int x = 0; x < w; x++) {
      dst[x] = (uint8_t)(acc / win);
      acc += src[clampi(x + r + 1, 0, w - 1)] - src[clampi(x - r, 0, w - 1)];
    }
  }
  for (int x = 0; x < w; x++) {           // vertical back into buf
    int acc = 0;
    for (int y = -r; y <= r; y++) acc += tmp[clampi(y, 0, h - 1) * w + x];
    for (int y = 0; y < h; y++) {
      buf[y * w + x] = (uint8_t)(acc / win);
      acc += tmp[clampi(y + r + 1, 0, h - 1) * w + x] - tmp[clampi(y - r, 0, h - 1) * w + x];
    }
  }
}

// Shared scratch for text rasterising and blurring (single render task).
uint8_t *gScratchA = nullptr;
uint8_t *gScratchB = nullptr;
uint8_t *gScratchC = nullptr;
size_t   gScratchSize = 0;

// Murk shaping curve over the sum of two noise samples (0..510).
uint8_t gCloud[511];
bool    gCloudReady = false;

// One row of the window in 8.8 fixed point, per channel.
constexpr int kRowMax = 192;
int32_t gRowR[kRowMax], gRowG[kRowMax], gRowB[kRowMax];

}  // namespace

// ================================================================= setup

bool Renderer::begin(const GFXfont *font, AllocFn alloc) {
  if (ready_) return true;
  if (!alloc) alloc = [](size_t n, bool) -> void * { return calloc(1, n); };
  font_ = font;

  mapX0_ = (int)floorf(kBallCX - kWinR) - 1;
  mapY0_ = kWinTop;
  mapN_ = kWinRows;

  float R = kDieR;
  spriteW_ = (int)ceilf(2.f * 0.8660254f * R) + 2 * kPad;
  spriteH_ = (int)ceilf(1.5f * R) + 2 * kPad;
  spriteOX_ = spriteW_ * 0.5f;
  spriteOY_ = kDiePointsDown ? kPad + 0.5f * R : kPad + R;

  promptW_ = 2 * (int)kWinR - 24;
  promptH_ = 64;

  size_t spriteN = (size_t)spriteW_ * spriteH_;
  size_t promptN = (size_t)promptW_ * promptH_;
  gScratchSize = spriteN > promptN ? spriteN : promptN;
  if (gScratchSize < 64 * 32) gScratchSize = 64 * 32;

  // Hot tables first, so they get first pick of internal RAM.
  light_     = (uint16_t *)alloc((size_t)mapN_ * mapN_ * 2, true);
  glare_     = (uint8_t *)alloc((size_t)mapN_ * mapN_, true);
  noise_     = (uint8_t *)alloc(128 * 128, true);
  ball_      = (uint16_t *)alloc((size_t)kScreenW * kScreenH * 2, false);
  labelBase_ = (uint16_t *)alloc((size_t)kScreenW * kLabelRows * 2, false);
  sprite_    = (uint32_t *)alloc(spriteN * 4, false);
  prompt_    = (uint16_t *)alloc(promptN * 2, false);
  if (!gScratchA) gScratchA = (uint8_t *)alloc(gScratchSize, false);
  if (!gScratchB) gScratchB = (uint8_t *)alloc(gScratchSize, false);
  if (!gScratchC) gScratchC = (uint8_t *)alloc(gScratchSize, false);
  if (!light_ || !glare_ || !noise_ || !ball_ || !labelBase_ || !sprite_ || !prompt_ ||
      !gScratchA || !gScratchB || !gScratchC)
    return false;   // the caller falls back to a plain text screen

  buildWindowMap();
  buildNoise();
  buildDieShape();
  memset(prompt_, 0, promptN * 2);
  ballValid_ = false;
  ready_ = true;
  return true;
}

void Renderer::buildWindowMap() {
  for (int j = 0; j < mapN_; j++) {
    for (int i = 0; i < mapN_; i++) {
      float dx = mapX0_ + i + 0.5f - kBallCX;
      float dy = mapY0_ + j + 0.5f - kBallCY;
      float d = sqrtf(dx * dx + dy * dy);
      float nd = d / kWinR;

      // Light: vignette x rim shadow x a pool of light up and to the left.
      float vs = smoothstep(0.18f, 1.02f, nd);
      float vig = 1.f - 0.72f * vs * (0.8f + 0.2f * vs);
      float sx = dx - 4.5f, sy = dy - 6.0f;
      float lit = smoothstep(-2.f, 11.f, kWinR - sqrtf(sx * sx + sy * sy));
      float shadow = 0.40f + 0.60f * lit;
      float gx = dx + 16.f, gy = dy + 22.f;
      float pool = expf(-(gx * gx + gy * gy) / (2.f * 50.f * 50.f));
      float light = clampf(vig * shadow * (0.70f + 0.42f * pool), 0.f, 1.f);

      // Glare on the glass: a soft sheen, a bright crescent on the upper-left
      // rim and a faint answering arc lower right.
      float ca = cosf(-0.62f), sa = sinf(-0.62f);
      float ex = dx + 30.f, ey = dy + 44.f;
      float erx = ex * ca - ey * sa, ery = ex * sa + ey * ca;
      float e = sqrtf((erx / 54.f) * (erx / 54.f) + (ery / 21.f) * (ery / 21.f));
      float sheen = 0.20f * (1.f - smoothstep(0.30f, 1.0f, e));
      float ux = d > 0 ? dx / d : 0, uy = d > 0 ? dy / d : 0;
      float band = smoothstep(0.86f, 0.935f, nd) * (1.f - smoothstep(0.955f, 0.995f, nd));
      float w1 = smoothstep(0.30f, 0.97f, ux * -0.6f + uy * -0.8f);
      float w2 = smoothstep(0.55f, 0.98f, ux * 0.6f + uy * 0.8f);
      float glare = clampf(sheen + 0.42f * band * w1 + 0.14f * band * w2, 0.f, 1.f);

      float cov = clampf(kWinR + 0.5f - d, 0.f, 1.f);
      uint32_t L = (uint32_t)(light * 255.f + 0.5f);
      uint32_t C = (uint32_t)(cov * 255.f + 0.5f);
      light_[j * mapN_ + i] = (uint16_t)(L | (C << 8));
      glare_[j * mapN_ + i] = (uint8_t)(glare * 255.f + 0.5f);
    }
  }
}

void Renderer::buildNoise() {
  // Tileable value noise, three octaves, smoothed with a cubic fade.
  static const int kN = 128;
  uint32_t seed = 0x2545F491u;
  auto hash = [&](int x, int y, int period) -> float {
    x = ((x % period) + period) % period;
    y = ((y % period) + period) % period;
    uint32_t h = (uint32_t)(x * 374761393u + y * 668265263u) ^ seed ^ (uint32_t)period * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (h & 0xFFFF) / 65535.f;
  };
  const int   periods[3] = {4, 8, 16};
  const float amps[3]    = {0.62f, 0.28f, 0.10f};
  float mn = 1e9f, mx = -1e9f;
  // Two passes: find the range, then normalise. Values are recomputed rather
  // than stored to avoid a float buffer.
  for (int pass = 0; pass < 2; pass++) {
    for (int y = 0; y < kN; y++) {
      for (int x = 0; x < kN; x++) {
        float v = 0;
        for (int o = 0; o < 3; o++) {
          int P = periods[o];
          float cell = (float)kN / P;
          float fx = x / cell, fy = y / cell;
          int ix = (int)floorf(fx), iy = (int)floorf(fy);
          float tx = fx - ix, ty = fy - iy;
          tx = tx * tx * (3 - 2 * tx);
          ty = ty * ty * (3 - 2 * ty);
          float a = hash(ix, iy, P), b = hash(ix + 1, iy, P);
          float c = hash(ix, iy + 1, P), d = hash(ix + 1, iy + 1, P);
          v += amps[o] * lerpf(lerpf(a, b, tx), lerpf(c, d, tx), ty);
        }
        if (pass == 0) {
          if (v < mn) mn = v;
          if (v > mx) mx = v;
        } else {
          float n = (v - mn) / (mx - mn);
          noise_[y * kN + x] = (uint8_t)clampi((int)(n * 255.f + 0.5f), 0, 255);
        }
      }
    }
  }
}

void Renderer::buildDieShape() {
  float R = kDieR, rc = kDieCorner;
  float vx[3], vy[3];
  dieVertices(R - 2.f * rc, vx, vy);   // shrunken triangle, rounded by rc
  const float lx = -0.6f, ly = -0.8f;  // light from the upper left (2-D)
  for (int j = 0; j < spriteH_; j++) {
    for (int i = 0; i < spriteW_; i++) {
      float px = i + 0.5f - spriteOX_, py = j + 0.5f - spriteOY_;
      float gxn, gyn;   // outward normal of the nearest edge
      float sd = sdTriangle(px, py, vx, vy, &gxn, &gyn) - rc;
      float cov = clampf(0.5f - sd, 0.f, 1.f);
      float facing = gxn * lx + gyn * ly;
      // Face: gently lit from the upper left, a rim catching light on the
      // edges that face it, and a soft inner shadow on the others.
      float grad = 0.5f + 0.5f * clampf((-(px * 0.6f + py * 0.8f)) / R, -1.f, 1.f);
      float shade = 0.42f + 0.40f * grad;
      float edge = 1.f - smoothstep(0.f, 4.0f, -sd);
      shade += edge * facing * 0.42f;
      float inner = smoothstep(1.5f, 7.f, -sd) * (1.f - smoothstep(7.f, 14.f, -sd));
      shade -= inner * (facing < 0 ? -facing : 0) * 0.10f;
      shade = clampf(shade, 0.f, 1.f);
      uint32_t C = (uint32_t)(cov * 255.f + 0.5f);
      uint32_t S = (uint32_t)(shade * 255.f + 0.5f);
      sprite_[j * spriteW_ + i] = C | (S << 8);
    }
  }
  memset(&dieLayout_, 0, sizeof(dieLayout_));
}

// ================================================================= text

bool Renderer::setDieText(const char *text) {
  if (!ready_) return false;
  TriangleRegion region(kDieR, kDieTextInset, kDiePointsDown);
  FitParams p;
  p.capMax = kDieCapMax;
  p.capMin = kDieCapMin;
  p.condense = kDieCondense;
  p.lineGap = kDieLineGap;
  p.maxLines = kDieMaxLines;
  p.uppercase = true;
  bool ok = fitText(font_, text, region, p, dieLayout_);

  int n = spriteW_ * spriteH_;
  memset(gScratchA, 0, (size_t)n);
  rasterizeText(font_, dieLayout_, gScratchA, spriteW_, spriteH_, spriteW_,
                spriteOX_, spriteOY_);
  // Glow: a wide soft copy of the text.
  memcpy(gScratchB, gScratchA, (size_t)n);
  uint8_t *glow = gScratchB;
  boxBlur(glow, gScratchC, spriteW_, spriteH_, 2);
  boxBlur(glow, gScratchC, spriteW_, spriteH_, 3);
  for (int k = 0; k < n; k++) {
    uint32_t v = sprite_[k] & 0xFFFFu;
    uint32_t g = glow[k];
    g = g * 2 > 255 ? 255 : g * 2;
    sprite_[k] = v | ((uint32_t)gScratchA[k] << 16) | (g << 24);
  }
  return ok;
}

bool Renderer::setPrompt(const char *text) {
  if (!ready_) return false;
  DiscRegion region(kWinR - 22.f);
  FitParams p;
  p.capMax = kPromptCapMax;
  p.capMin = kPromptCapMin;
  p.condense = 0.92f;
  p.lineGap = 0.55f;
  p.maxLines = 3;
  p.uppercase = false;
  p.tracking = 0.3f;
  TextLayout L;
  bool ok = fitText(font_, text, region, p, L);
  int n = promptW_ * promptH_;
  uint8_t *a = gScratchA, *b = gScratchB;
  memset(a, 0, (size_t)n);
  rasterizeText(font_, L, a, promptW_, promptH_, promptW_, promptW_ * 0.5f, promptH_ * 0.5f);
  memcpy(b, a, (size_t)n);
  boxBlur(b, gScratchC, promptW_, promptH_, 3);
  boxBlur(b, gScratchC, promptW_, promptH_, 3);
  for (int k = 0; k < n; k++) prompt_[k] = a[k];
  for (int k = 0; k < n; k++) {
    uint32_t g = b[k] * 2u;
    if (g > 255) g = 255;
    prompt_[k] = (uint16_t)(prompt_[k] | (g << 8));
  }
  promptValid_ = true;
  return ok;
}

// ================================================================= ball

void Renderer::renderBallPixels(uint16_t *dst) {
  // A glossy black sphere seen head-on. The glass covers the middle, so only
  // the outer ring shows, where the surface turns away from the viewer; its
  // reflections come from beside and behind the ball. The key "softbox" sits
  // up and to the left and slightly behind, which lays a bright crescent on
  // the upper-left of the ring; a cool rim light answers it lower right.
  const float kDeg = 57.29578f;
  const float L1x = -0.45f, L1y = -0.65f, L1z = 0.61f;   // for the bevel
  auto angDiff = [](float a, float b) -> float {
    float d = a - b;
    while (d > 180.f) d -= 360.f;
    while (d < -180.f) d += 360.f;
    return d;
  };
  for (int y = 0; y < kScreenH; y++) {
    for (int x = 0; x < kScreenW; x++) {
      float px = x + 0.5f - kBallCX, py = y + 0.5f - kBallCY;
      float d = sqrtf(px * px + py * py);
      float cov = clampf(kBallR + 0.5f - d, 0.f, 1.f);
      float r = 0, g = 0, b = 0;
      if (cov > 0.f && d < kWinR - 0.5f) {
        // Under the glass: placeholder liquid, painted over every frame.
        r = kLiqLo.r; g = kLiqLo.g; b = kLiqLo.b;
      } else if (cov > 0.f) {
        float nx = px / kBallR, ny = py / kBallR;
        float nz2 = 1.f - nx * nx - ny * ny;
        float nz = nz2 > 0 ? sqrtf(nz2) : 0.f;
        float rx = 2 * nz * nx, ry = 2 * nz * ny, rz = 2 * nz * nz - 1;
        float az = atan2f(ry, rx) * kDeg;                  // -180..180
        float el = asinf(clampf(rz, -1.f, 1.f)) * kDeg;     // +90 = at viewer
        float om = 1.f - nz, om2 = om * om;
        float fres = 0.05f + 0.95f * om2 * om2 * om;
        // Key softbox: upper left, beside and a little behind.
        float dA = fabsf(angDiff(az, -124.f));
        float soft = (1.f - smoothstep(18.f, 34.f, dA)) *
                     smoothstep(-62.f, -46.f, el) * (1.f - smoothstep(-26.f, -14.f, el));
        float grad = 0.65f + 0.35f * (1.f - smoothstep(-50.f, -18.f, el));
        float hot = expf(-(dA * dA + (el + 40.f) * (el + 40.f)) / (2.f * 5.5f * 5.5f));
        // Cool rim light, lower right.
        float dB = fabsf(angDiff(az, 52.f));
        float rim = (1.f - smoothstep(26.f, 58.f, dB)) *
                    smoothstep(-86.f, -70.f, el) * (1.f - smoothstep(-58.f, -44.f, el));
        // Dim studio around everything, brighter overhead.
        float env = 0.035f + 0.05f * clampf(-ry, 0.f, 1.f);
        r = 3.f + 255.f * (env * fres) + soft * grad * 168.f + hot * 210.f + rim * 30.f;
        g = 3.f + 255.f * (env * fres) * 1.05f + soft * grad * 172.f + hot * 214.f + rim * 40.f;
        b = 5.f + 255.f * (env * fres) * 1.25f + soft * grad * 182.f + hot * 222.f + rim * 58.f;

        // Recessed bevel around the window.
        float rimR = kWinR + kBevelW;
        if (d < rimR + 0.5f) {
          float t = clampf((d - kWinR) / kBevelW, 0.f, 1.f);
          float ux = d > 0 ? px / d : 0, uy = d > 0 ? py / d : 0;
          // Wall normal leans inward and toward the viewer.
          float k = 1.7f * (1.f - t) + 0.2f;
          float wx = -ux * k, wy = -uy * k, wz = 1.f;
          float wn = 1.f / sqrtf(wx * wx + wy * wy + wz * wz);
          float ln = 1.f / sqrtf(L1x * L1x + L1y * L1y + L1z * L1z);
          float wl = (wx * L1x + wy * L1y + wz * L1z) * wn * ln;
          if (wl < 0) wl = 0;
          float lip = smoothstep(0.55f, 0.88f, t) * (1.f - smoothstep(0.90f, 1.0f, t)) *
                      smoothstep(0.10f, 0.95f, ux * -0.6f + uy * -0.8f);
          float gasket = 1.f - smoothstep(0.f, 0.25f, t);
          float br = 4 + wl * wl * 58 * (1.f - gasket);
          float wr = br + lip * 120, wg = br + lip * 122, wb = br * 1.2f + 4 + lip * 130;
          float edge = clampf(rimR + 0.5f - d, 0.f, 1.f);   // AA with the ball
          r = lerpf(r, wr, edge);
          g = lerpf(g, wg, edge);
          b = lerpf(b, wb, edge);
        }
        r *= cov; g *= cov; b *= cov;
      }
      int bay = kBayer[y & 3][x & 3];
      dst[y * kScreenW + x] = pack565((int)(clampf(r, 0, 255) * 256.f),
                                      (int)(clampf(g, 0, 255) * 256.f),
                                      (int)(clampf(b, 0, 255) * 256.f), bay);
    }
  }
}

void Renderer::printPackLabel(uint16_t *dst, const char *name, int index, int count) {
  // Printed on the ball below the window, like lettering on the plastic.
  const float baseY = kBallCY + kWinR + kBevelW + 13.5f;
  const int bw = 140, bh = 16;
  uint8_t *a = gScratchA;
  memset(a, 0, (size_t)bw * bh);
  if (name && *name) {
    // A wide band, so the text is laid out on one line at a fixed size.
    struct Band : FitRegion {
      bool span(float y0, float y1, float &xl, float &xr) const override {
        (void)y0; (void)y1; xl = -68.f; xr = 68.f; return true;
      }
      float centreY() const override { return 0.f; }
      float slack() const override { return 0.f; }
    } band;
    FitParams p;
    p.capMax = 6.6f;
    p.capMin = 6.6f;
    p.condense = 1.0f;
    p.lineGap = 0.f;
    p.maxLines = 1;
    p.uppercase = true;
    p.tracking = 1.6f;
    TextLayout L;
    fitText(font_, name, band, p, L);
    rasterizeText(font_, L, a, bw, bh, bw, bw * 0.5f, bh * 0.5f);
  }
  int x0 = (int)(kBallCX - bw * 0.5f), y0 = (int)(baseY - bh * 0.5f);
  for (int j = 0; j < bh; j++) {
    int y = y0 + j;
    if (y < 0 || y >= kScreenH) continue;
    for (int i = 0; i < bw; i++) {
      int x = x0 + i;
      if (x < 0 || x >= kScreenW || !a[j * bw + i]) continue;
      float al = a[j * bw + i] / 255.f * 0.9f;
      int r, g, b;
      unpack565(dst[y * kScreenW + x], r, g, b);
      dst[y * kScreenW + x] = rgb565((int)lerpf(r, kLabel.r, al), (int)lerpf(g, kLabel.g, al),
                                     (int)lerpf(b, kLabel.b, al));
    }
  }
  // Page dots under the label.
  if (count > 1) {
    float dotY = baseY + 8.5f, gap = 8.f;
    float startX = kBallCX - gap * (count - 1) * 0.5f;
    for (int k = 0; k < count; k++) {
      float cx = startX + gap * k;
      float rad = k == index ? 1.9f : 1.4f;
      float br = k == index ? 205.f : 64.f;
      for (int y = (int)(dotY - 3); y <= (int)(dotY + 3); y++) {
        for (int x = (int)(cx - 3); x <= (int)(cx + 3); x++) {
          if (x < 0 || y < 0 || x >= kScreenW || y >= kScreenH) continue;
          float dd = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - dotY) * (y + 0.5f - dotY));
          float al = clampf(rad + 0.5f - dd, 0.f, 1.f);
          if (al <= 0) continue;
          int r, g, b;
          unpack565(dst[y * kScreenW + x], r, g, b);
          dst[y * kScreenW + x] = rgb565((int)lerpf(r, br * 0.95f, al), (int)lerpf(g, br, al),
                                         (int)lerpf(b, br * 1.08f > 255 ? 255 : br * 1.08f, al));
        }
      }
    }
  }
}

void Renderer::drawBall(uint16_t *fb, const char *packName, int packIndex, int packCount) {
  if (!ready_) return;
  if (!ballValid_) {
    renderBallPixels(ball_);
    memcpy(labelBase_, ball_ + kLabelY0 * kScreenW, (size_t)kScreenW * kLabelRows * 2);
    ballValid_ = true;
  }
  int y0, rows;
  setPackLabel(nullptr, packName, packIndex, packCount, y0, rows);
  if (fb) memcpy(fb, ball_, (size_t)kScreenW * kScreenH * 2);
}

void Renderer::setPackLabel(uint16_t *fb, const char *packName, int packIndex, int packCount,
                            int &y0, int &rows) {
  y0 = kLabelY0;
  rows = kLabelRows;
  if (!ready_ || !ballValid_) return;
  uint16_t *strip = ball_ + kLabelY0 * kScreenW;
  memcpy(strip, labelBase_, (size_t)kScreenW * kLabelRows * 2);
  printPackLabel(ball_, packName, packIndex, packCount);
  if (fb) memcpy(fb + kLabelY0 * kScreenW, strip, (size_t)kScreenW * kLabelRows * 2);
}

bool Renderer::restoreBall(uint16_t *fb) const {
  if (!ready_ || !ballValid_ || !fb) return false;
  memcpy(fb, ball_, (size_t)kScreenW * kScreenH * 2);
  return true;
}

// ================================================================= window

namespace {

struct DieXf {
  bool  on = false;
  // screen -> local (sprite px relative to the centroid), 16.16 fixed
  int32_t a11, a12, a21, a22;
  float fa11, fa12, fa21, fa22;
  float cx, cy;
  int   x0, x1, y0, y1;            // screen bbox incl. halo
  int   alpha;                     // 0..256, (1 - fog)
  int   textA;                     // 0..256
  int   faceLo[3], faceHi[3], text[3], glow[3];
  int   flash;                     // added to the face shade, 0..~200
  bool  gold;
  float glint;
  float scale;
};

inline void addLight(int32_t &r, int32_t &g, int32_t &b, const C3 &c, int a8) {
  // a8: 0..256 strength; additive in 8.8
  r += (int32_t)(c.r * a8);
  g += (int32_t)(c.g * a8);
  b += (int32_t)(c.b * a8);
}

}  // namespace

void Renderer::drawWindow(uint16_t *fb, const FrameState &s) {
  if (!ready_ || !fb) return;

  // ---- per-frame constants
  int ax = (int)floorf(s.murkX), ay = (int)floorf(s.murkY);
  const float layerScale = 0.62f;   // second murk layer: bigger, slower clouds
  int32_t cs = (int32_t)(cosf(s.swirl) * layerScale * 65536.f);
  int32_t sn = (int32_t)(sinf(s.swirl) * layerScale * 65536.f);
  // Murk: density factor f = mA + mB * cloud (8.8), centred near 1.0.
  float mk = clampf(s.murk, 0.f, 1.f);
  int mA = (int)((1.f - 0.55f * mk) * 256.f);
  int mB = (int)(1.25f * mk * 256.f);
  int mT = (int)(0.22f * mk * 256.f);
  if (!gCloudReady) {
    for (int v = 0; v < 511; v++)
      gCloud[v] = (uint8_t)(smoothstep(0.30f, 0.78f, v / 510.f) * 255.f + 0.5f);
    gCloudReady = true;
  }
  int dark = (int)((1.f - 0.38f * clampf(s.stir, 0.f, 1.f)) * 256.f);
  int lo[3] = {(int)kLiqLo.r, (int)kLiqLo.g, (int)kLiqLo.b};
  int hi[3] = {(int)kLiqHi.r, (int)kLiqHi.g, (int)kLiqHi.b};
  // Idle shimmer: the light pool breathes very slightly.
  int breathe = (int)(256.f * (1.f + 0.03f * sinf(s.t * 1.3f)));

  int gdx = (int)lroundf(clampf(s.glareDx, -4.f, 4.f));
  int gdy = (int)lroundf(clampf(s.glareDy, -4.f, 4.f));

  // ---- die transform
  DieXf die;
  if (s.dieVisible && s.dieFog < 0.995f) {
    die.on = true;
    float sc = s.dieScale < 0.05f ? 0.05f : s.dieScale;
    float sq = clampf(s.dieSquash, 0.08f, 1.f);
    // Forward: p = T + Rot(theta) * Rot(phi) * diag(sq,1) * Rot(-phi) * sc * l
    // Inverse: l = (1/sc) * Rot(phi) * diag(1/sq,1) * Rot(-phi) * Rot(-theta) * (p - T)
    float ct = cosf(s.dieAngle), st = sinf(s.dieAngle);
    float cp = cosf(s.dieSquashAxis), sp = sinf(s.dieSquashAxis);
    // M1 = Rot(-theta)
    float m1[4] = {ct, st, -st, ct};
    // M2 = Rot(phi) diag(1/sq,1) Rot(-phi)
    float iq = 1.f / sq;
    float m2[4] = {cp * cp * iq + sp * sp, cp * sp * iq - cp * sp,
                   sp * cp * iq - sp * cp, sp * sp * iq + cp * cp};
    float inv[4] = {(m2[0] * m1[0] + m2[1] * m1[2]) / sc, (m2[0] * m1[1] + m2[1] * m1[3]) / sc,
                    (m2[2] * m1[0] + m2[3] * m1[2]) / sc, (m2[2] * m1[1] + m2[3] * m1[3]) / sc};
    die.fa11 = inv[0]; die.fa12 = inv[1]; die.fa21 = inv[2]; die.fa22 = inv[3];
    die.a11 = (int32_t)(inv[0] * 65536.f);
    die.a12 = (int32_t)(inv[1] * 65536.f);
    die.a21 = (int32_t)(inv[2] * 65536.f);
    die.a22 = (int32_t)(inv[3] * 65536.f);
    die.cx = s.dieX;
    die.cy = s.dieY;
    die.scale = sc;
    // Screen bbox: forward-transform the sprite rect corners (plus halo).
    float det = inv[0] * inv[3] - inv[1] * inv[2];
    float fw[4] = {inv[3] / det, -inv[1] / det, -inv[2] / det, inv[0] / det};
    float hx0 = -spriteOX_ - kHaloR, hx1 = spriteW_ - spriteOX_ + kHaloR;
    float hy0 = -spriteOY_ - kHaloR, hy1 = spriteH_ - spriteOY_ + kHaloR;
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    for (int k = 0; k < 4; k++) {
      float lx = (k & 1) ? hx1 : hx0, ly = (k & 2) ? hy1 : hy0;
      float X = s.dieX + fw[0] * lx + fw[1] * ly;
      float Y = s.dieY + fw[2] * lx + fw[3] * ly;
      if (X < minx) minx = X;
      if (X > maxx) maxx = X;
      if (Y < miny) miny = Y;
      if (Y > maxy) maxy = Y;
    }
    die.x0 = (int)floorf(minx);
    die.x1 = (int)ceilf(maxx);
    die.y0 = (int)floorf(miny);
    die.y1 = (int)ceilf(maxy);
    die.alpha = (int)((1.f - clampf(s.dieFog, 0.f, 1.f)) * 256.f);
    die.textA = (int)(clampf(s.dieTextAlpha, 0.f, 1.f) * 256.f);
    die.gold = s.dieGold;
    die.glint = s.dieGlint;
    die.flash = (int)(clampf(s.dieFlash, 0.f, 1.f) * 90.f);
    const C3 &flo = s.dieGold ? kGoldLo : kFaceLo;
    const C3 &fhi = s.dieGold ? kGoldHi : kFaceHi;
    const C3 &ftx = s.dieGold ? kGoldTxt : kText;
    const C3 &fgl = s.dieGold ? kGoldGlw : kGlow;
    die.faceLo[0] = (int)flo.r; die.faceLo[1] = (int)flo.g; die.faceLo[2] = (int)flo.b;
    die.faceHi[0] = (int)fhi.r; die.faceHi[1] = (int)fhi.g; die.faceHi[2] = (int)fhi.b;
    die.text[0] = (int)ftx.r;   die.text[1] = (int)ftx.g;   die.text[2] = (int)ftx.b;
    die.glow[0] = (int)fgl.r;   die.glow[1] = (int)fgl.g;   die.glow[2] = (int)fgl.b;
  }
  // Un-rounded triangle edges for the halo (local px).
  const float inR = kDieR * 0.5f;
  const float es = kDiePointsDown ? 1.f : -1.f;

  // Prompt placement.
  int pa = (int)(clampf(s.promptAlpha, 0.f, 1.f) * 256.f);
  int px0 = (int)lroundf(kBallCX - promptW_ * 0.5f);
  int py0 = (int)lroundf(kBallCY + s.promptDy - promptH_ * 0.5f);

  const int pitch = kScreenW;

  for (int y = kWinTop; y < kWinBottom; y++) {
    float fy = y + 0.5f - kBallCY;
    float h2 = (kWinR + 1.f) * (kWinR + 1.f) - fy * fy;
    if (h2 <= 0) continue;
    float half = sqrtf(h2);
    int x0 = (int)floorf(kBallCX - half), x1 = (int)ceilf(kBallCX + half);
    if (x0 < mapX0_) x0 = mapX0_;
    if (x1 > mapX0_ + mapN_ - 1) x1 = mapX0_ + mapN_ - 1;
    int n = x1 - x0 + 1;
    if (n <= 0 || n > kRowMax) continue;
    const uint16_t *mrow = light_ + (y - mapY0_) * mapN_ + (x0 - mapX0_);
    const uint8_t *noiseRowA = noise_ + ((y + ay) & 127) * 128;

    // ---- 1. liquid + murk
    // Layer B rotates about the window centre; step it along the row.
    int32_t dyc = (int32_t)((y + 0.5f - kBallCY) * 65536.f);
    int32_t dxc = (int32_t)((x0 + 0.5f - kBallCX) * 65536.f);
    int32_t ub = (int32_t)(((int64_t)dxc * cs - (int64_t)dyc * sn) >> 16) + (64 << 16);
    int32_t vb = (int32_t)(((int64_t)dxc * sn + (int64_t)dyc * cs) >> 16) + (64 << 16);
    for (int i = 0; i < n; i++, ub += cs, vb += sn) {
      int L = mrow[i] & 0xFF;
      int nA = noiseRowA[(x0 + i + ax) & 127];
      int nB = noise_[(((uint32_t)vb >> 16) & 127) * 128 + (((uint32_t)ub >> 16) & 127)];
      int cloud = gCloud[nA + nB];                    // 0..255, shaped
      int f = mA + ((mB * cloud) >> 8);               // murk density factor
      int Lb = (L * breathe) >> 8;
      if (Lb > 255) Lb = 255;
      // colour = mix(lo, hi, L) * murk * stir-darkness, kept in 8.8. Lighter
      // clouds lean slightly teal, like silt catching the light.
      int k = (f * dark) >> 8;
      int kg = k + ((mT * cloud) >> 8);
      int r = lo[0] + (((hi[0] - lo[0]) * Lb) >> 8);
      int g = lo[1] + (((hi[1] - lo[1]) * Lb) >> 8);
      int b = lo[2] + (((hi[2] - lo[2]) * Lb) >> 8);
      gRowR[i] = r * k;
      gRowG[i] = g * kg;
      gRowB[i] = b * k;
    }

    // ---- 2. idle prompt (additive glow in the liquid)
    if (pa > 0 && promptValid_ && y >= py0 && y < py0 + promptH_) {
      const uint16_t *prow = prompt_ + (y - py0) * promptW_;
      int ia = x0 > px0 ? x0 : px0;
      int ib = x1 < px0 + promptW_ - 1 ? x1 : px0 + promptW_ - 1;
      for (int x = ia; x <= ib; x++) {
        uint16_t v = prow[x - px0];
        if (!v) continue;
        int t = ((v & 0xFF) * pa) >> 8;
        int gl = (((v >> 8) & 0xFF) * pa) >> 8;
        int i = x - x0;
        addLight(gRowR[i], gRowG[i], gRowB[i], kPromptG, (gl * 90) >> 8);
        // text replaces toward a bright tint so it stays crisp
        gRowR[i] += (((int32_t)kPrompt.r * 256 - gRowR[i]) * t) >> 8;
        gRowG[i] += (((int32_t)kPrompt.g * 256 - gRowG[i]) * t) >> 8;
        gRowB[i] += (((int32_t)kPrompt.b * 256 - gRowB[i]) * t) >> 8;
      }
    }

    // ---- 3. sediment motes and bubbles behind the die
    float cyRow = y + 0.5f;
    auto drawSprites = [&](bool front) {
      for (int k = 0; k < s.moteCount && !front; k++) {
        const Mote &m = s.motes[k];
        float dyv = cyRow - m.y;
        if (dyv <= -1.f || dyv >= 1.f) continue;
        float wy = 1.f - fabsf(dyv);
        int xa = (int)floorf(m.x - 0.5f);
        float fx = m.x - 0.5f - xa;
        for (int q = 0; q < 2; q++) {
          int xx = xa + q;
          if (xx < x0 || xx > x1) continue;
          float wx = q ? fx : 1.f - fx;
          int a8 = (int)(m.a * wx * wy * 256.f);
          int i = xx - x0;
          addLight(gRowR[i], gRowG[i], gRowB[i], kMote, a8);
        }
      }
      for (int k = 0; k < s.bubbleCount; k++) {
        const Bubble &bb = s.bubbles[k];
        if (bb.front != front) continue;
        float r = bb.r;
        float dyv = cyRow - bb.y;
        if (dyv < -r - 1.5f || dyv > r + 1.5f) continue;
        int xa = (int)floorf(bb.x - r - 1.5f), xb = (int)ceilf(bb.x + r + 1.5f);
        if (xa < x0) xa = x0;
        if (xb > x1) xb = x1;
        for (int xx = xa; xx <= xb; xx++) {
          float dxv = xx + 0.5f - bb.x;
          float dd = sqrtf(dxv * dxv + dyv * dyv);
          float a;
          const C3 *col = bb.spark ? &kSpark : &kBubble;
          if (bb.spark) {
            // Soft round glow with a cross glint.
            float core = 1.f - smoothstep(0.f, r, dd);
            float cross = (1.f - smoothstep(0.f, 0.9f, fabsf(dxv))) * (1.f - smoothstep(0.f, r * 2.4f, fabsf(dyv))) +
                          (1.f - smoothstep(0.f, 0.9f, fabsf(dyv))) * (1.f - smoothstep(0.f, r * 2.4f, fabsf(dxv)));
            a = core * 0.9f + cross * 0.5f;
          } else if (r < 1.6f) {
            a = (1.f - smoothstep(0.f, r + 0.6f, dd)) * 0.85f;
          } else {
            float rimA = smoothstep(r - 1.3f, r - 0.3f, dd) * (1.f - smoothstep(r - 0.3f, r + 0.7f, dd));
            float fill = (1.f - smoothstep(r - 1.6f, r, dd)) * 0.10f;
            float hx = dxv + r * 0.38f, hy = dyv + r * 0.42f;
            float spec = 1.f - smoothstep(0.f, r * 0.34f + 0.4f, sqrtf(hx * hx + hy * hy));
            a = rimA * 0.75f + fill + spec * 0.95f;
          }
          a *= bb.a;
          if (a <= 0.004f) continue;
          int i = xx - x0;
          addLight(gRowR[i], gRowG[i], gRowB[i], *col, (int)(a * 256.f));
        }
      }
    };
    drawSprites(false);

    // ---- 4. the die
    if (die.on && y >= die.y0 && y <= die.y1) {
      int ia = die.x0 > x0 ? die.x0 : x0;
      int ib = die.x1 < x1 ? die.x1 : x1;
      if (ia <= ib) {
        float dx0 = ia + 0.5f - die.cx, dyv = y + 0.5f - die.cy;
        float lu = die.fa11 * dx0 + die.fa12 * dyv;
        float lv = die.fa21 * dx0 + die.fa22 * dyv;
        int32_t U = (int32_t)((lu + spriteOX_ - 0.5f) * 65536.f);
        int32_t V = (int32_t)((lv + spriteOY_ - 0.5f) * 65536.f);
        const int32_t maxU = (spriteW_ - 1) << 16, maxV = (spriteH_ - 1) << 16;
        for (int xx = ia; xx <= ib; xx++, U += die.a11, V += die.a21, lu += die.fa11, lv += die.fa21) {
          int i = xx - x0;
          // Contact shadow (halo) on the liquid around the die.
          float ed = (0.8660254f * fabsf(lu) + 0.5f * es * lv);
          float e0 = -es * lv;
          if (e0 > ed) ed = e0;
          ed -= inR;
          if (ed > -2.f && ed < kHaloR) {
            float h = 1.f - clampf(ed / kHaloR, 0.f, 1.f);
            int hk = 256 - (int)(h * h * 0.55f * die.alpha);
            gRowR[i] = (gRowR[i] * hk) >> 8;
            gRowG[i] = (gRowG[i] * hk) >> 8;
            gRowB[i] = (gRowB[i] * hk) >> 8;
          }
          if (U < 0 || V < 0 || U >= maxU || V >= maxV) continue;
          int tu = U >> 16, tv = V >> 16;
          int fu = (U >> 8) & 0xFF, fv = (V >> 8) & 0xFF;
          const uint32_t *t0 = sprite_ + tv * spriteW_ + tu;
          uint32_t p00 = t0[0], p10 = t0[1], p01 = t0[spriteW_], p11 = t0[spriteW_ + 1];
          if (!((p00 | p10 | p01 | p11) & 0xFF)) continue;   // outside the face
          int w00 = (256 - fu) * (256 - fv), w10 = fu * (256 - fv);
          int w01 = (256 - fu) * fv, w11 = fu * fv;          // sum = 65536
          auto ch = [&](int sh) -> int {
            return (int)((((p00 >> sh) & 0xFF) * w00 + ((p10 >> sh) & 0xFF) * w10 +
                          ((p01 >> sh) & 0xFF) * w01 + ((p11 >> sh) & 0xFF) * w11) >> 16);
          };
          int cov = ch(0), shade = ch(8), txt = ch(16), glw = ch(24);
          if (die.gold && die.glint >= 0.f) {
            float gpos = (lu * 0.6f - lv * 0.8f) / (kDieR * 1.7f) + 0.5f;
            float dgl = fabsf(gpos - die.glint);
            if (dgl < 0.09f) shade += (int)((1.f - dgl / 0.09f) * 120.f);
            if (shade > 255) shade = 255;
          }
          shade += die.flash;
          if (shade > 255) shade = 255;
          txt = (txt * die.textA) >> 8;
          glw = (glw * die.textA) >> 8;
          int fr = die.faceLo[0] + (((die.faceHi[0] - die.faceLo[0]) * shade) >> 8);
          int fg = die.faceLo[1] + (((die.faceHi[1] - die.faceLo[1]) * shade) >> 8);
          int fb2 = die.faceLo[2] + (((die.faceHi[2] - die.faceLo[2]) * shade) >> 8);
          // glow brightens the face around the letters, then the letters
          fr += (die.glow[0] * glw) >> 9;
          fg += (die.glow[1] * glw) >> 9;
          fb2 += (die.glow[2] * glw) >> 9;
          fr += ((die.text[0] - fr) * txt) >> 8;
          fg += ((die.text[1] - fg) * txt) >> 8;
          fb2 += ((die.text[2] - fb2) * txt) >> 8;
          // Light falls off toward the rim of the glass for the die too.
          int L = mrow[i] & 0xFF;
          int lk = 150 + ((L * 106) >> 8);                 // 150..256
          fr = (fr * lk) >> 8; fg = (fg * lk) >> 8; fb2 = (fb2 * lk) >> 8;
          int a = (cov * die.alpha) >> 8;                  // 0..255
          gRowR[i] += ((fr * 256 - gRowR[i]) * a) >> 8;
          gRowG[i] += ((fg * 256 - gRowG[i]) * a) >> 8;
          gRowB[i] += ((fb2 * 256 - gRowB[i]) * a) >> 8;
        }
      }
    }

    // ---- 5. bubbles in front of the die
    drawSprites(true);

    // ---- 6. glass glare, edge anti-aliasing, dither, pack
    uint16_t *out = fb + y * pitch;
    const uint8_t *bay = kBayer[y & 3];
    int gy = y - gdy - mapY0_;
    bool gyOk = gy >= 0 && gy < mapN_;
    const uint8_t *grow = gyOk ? glare_ + gy * mapN_ : nullptr;
    for (int i = 0; i < n; i++) {
      int x = x0 + i;
      int cov = mrow[i] >> 8;
      if (!cov) continue;
      int r = gRowR[i], g = gRowG[i], b = gRowB[i];
      int gxm = x - gdx - mapX0_;
      if (grow && gxm >= 0 && gxm < mapN_) {
        int G = grow[gxm];
        if (G) {
          // screen blend toward white
          r += ((65280 - r) * G) >> 8;
          g += ((65280 - g) * G) >> 8;
          b += ((65280 - b) * G) >> 8;
        }
      }
      if (cov < 255) {
        int br = 0, bg = 0, bb2 = 0;
        if (ballValid_) unpack565(ball_[y * kScreenW + x], br, bg, bb2);
        r = ((br << 8) * (255 - cov) + r * cov) / 255;
        g = ((bg << 8) * (255 - cov) + g * cov) / 255;
        b = ((bb2 << 8) * (255 - cov) + b * cov) / 255;
      }
      out[x] = pack565(r, g, b, bay[x & 3]);
    }
  }
}

// ================================================================= tile icon

void renderTileIcon(uint16_t *dst, int size, uint16_t bg565) {
  if (!dst || size < 8) return;
  int br, bgc, bb;
  unpack565(bg565, br, bgc, bb);
  const float c = size * 0.5f;
  const float R = c - 0.75f;            // ball radius
  const float wr = R * 0.66f;           // window radius
  float vx[3], vy[3];
  const float tr = wr * 0.80f, rc = tr * 0.16f;
  // Die (pointing up), shrunk by the corner radius and nudged down a touch.
  vx[0] = 0;                      vy[0] = -(tr - 2.f * rc);
  vx[1] = 0.866f * (tr - 2.f * rc); vy[1] = 0.5f * (tr - 2.f * rc);
  vx[2] = -vx[1];                 vy[2] = vy[1];
  const float dieDy = wr * 0.08f;
  for (int y = 0; y < size; y++) {
    for (int x = 0; x < size; x++) {
      float px = x + 0.5f - c, py = y + 0.5f - c;
      float d = sqrtf(px * px + py * py);
      float r = (float)br, g = (float)bgc, b = (float)bb;
      // Ball body: black with a soft key highlight and a cool rim.
      float ballA = clampf(R + 0.5f - d, 0.f, 1.f);
      if (ballA > 0.f) {
        float nd = d / R;
        float hx = px + R * 0.42f, hy = py + R * 0.50f;
        float hl = 1.f - smoothstep(0.f, R * 0.30f, sqrtf(hx * hx * 0.7f + hy * hy * 1.6f));
        float rim = smoothstep(0.80f, 1.0f, nd) * smoothstep(0.2f, 1.f, (px * 0.6f + py * 0.8f) / (d + 1e-3f));
        float kr = 10 + 150 * hl + 40 * rim, kg = 10 + 152 * hl + 52 * rim, kb = 14 + 160 * hl + 80 * rim;
        r = lerpf(r, kr, ballA);
        g = lerpf(g, kg, ballA);
        b = lerpf(b, kb, ballA);
      }
      // Window: deep blue, lighter toward the middle, a dark gasket ring.
      float winA = clampf(wr + 0.5f - d, 0.f, 1.f);
      float gasket = clampf(wr + 1.8f - d, 0.f, 1.f) - winA;
      if (gasket > 0.f) { r = lerpf(r, 30, gasket); g = lerpf(g, 32, gasket); b = lerpf(b, 40, gasket); }
      if (winA > 0.f) {
        float t = 1.f - smoothstep(0.f, wr, d);
        float lr = lerpf(kLiqLo.r, kLiqHi.r, t), lg = lerpf(kLiqLo.g, kLiqHi.g, t);
        float lb = lerpf(kLiqLo.b + 20.f, kLiqHi.b, t);
        r = lerpf(r, lr, winA);
        g = lerpf(g, lg, winA);
        b = lerpf(b, lb, winA);
        // The die.
        float gx, gy;
        float sd = sdTriangle(px, py - dieDy, vx, vy, &gx, &gy) - rc;
        float dA = clampf(0.5f - sd, 0.f, 1.f) * winA;
        if (dA > 0.f) {
          float lit = 0.55f + 0.45f * clampf(-(px * 0.6f + py * 0.8f) / tr, -1.f, 1.f);
          float edge = (1.f - smoothstep(0.f, 1.6f, -sd)) * clampf(-(gx * 0.6f + gy * 0.8f), 0.f, 1.f);
          float fr = lerpf(kFaceLo.r, kFaceHi.r, lit) + edge * 120.f;
          float fg = lerpf(kFaceLo.g, kFaceHi.g, lit) + edge * 110.f;
          float fb = lerpf(kFaceLo.b, kFaceHi.b, lit) + edge * 60.f;
          r = lerpf(r, fr, dA);
          g = lerpf(g, fg, dA);
          b = lerpf(b, fb, dA);
        }
      }
      dst[y * size + x] = rgb565((int)clampf(r, 0, 255), (int)clampf(g, 0, 255), (int)clampf(b, 0, 255));
    }
  }
}

}  // namespace oracle
