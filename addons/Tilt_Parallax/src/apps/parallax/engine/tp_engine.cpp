#include "tp_engine.h"
#include "tp_compose.h"
#include "tp_shade.h"
#include "tp_paint.h"
#include "tp_noise.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

namespace tp {

// ---- layout (screen pixels at zero parallax) ----
static const float kClockTop    = 50.0f;   // top of the digits
static const int   kClockMargin = 12;      // room for the shadow blur
static const int   kStatusTop   = 14;      // status sprite top
static const int   kStatusH     = 26;
static const float kStatusCap   = 10.5f;   // cap height of the date line
static const float kStatusBase  = 30.0f;   // baseline (screen y)

static ClockMetrics clockMetrics() {
  ClockMetrics m;
  m.digitH = 56; m.digitW = 34; m.stroke = 6.6f; m.gap = 7; m.colonW = 11;
  return m;
}

bool FaceRenderer::begin() {
  end();
  // Sky: deepest layer. Allocated for the worst case (full height); each
  // scene uses only the rows it needs (h is reduced in setScene()).
  int ovx = overscanX(1.0f), ovy = overscanY(1.0f);
  skyL_.depth = 1.0f;
  skyL_.x0 = (int16_t)-ovx;
  skyL_.y0 = (int16_t)-ovy;
  skyL_.clampTop = true;
  skyL_.clampBottom = true;
  if (!skyL_.allocRGB((int16_t)(kScreenW + 2 * ovx), (int16_t)(kScreenH + 2 * ovy)))
    return false;
  memset(skyL_.rgb, 0, (size_t)skyL_.w * skyL_.h * 2);
  if (!skyL_.encodeSpans()) return false;

  ClockMetrics cm = clockMetrics();
  int ch = (int)cm.digitH + 2 * kClockMargin;
  clock_.depth = kClockDepth;
  clock_.x0 = 0;
  clock_.y0 = (int16_t)(kClockTop - kClockMargin);
  if (!clock_.allocAlpha(kScreenW, (int16_t)ch)) return false;
  shadow_.depth = kShadowDepth;
  shadow_.x0 = 0;
  shadow_.y0 = clock_.y0;
  if (!shadow_.allocAlpha(kScreenW, (int16_t)ch)) return false;
  shadow_.color = 0x0000;
  shadow_.opacity = 150;

  status_.depth = kClockDepth;
  status_.x0 = 0;
  status_.y0 = kStatusTop;
  if (!status_.allocAlpha(kScreenW, kStatusH)) return false;
  statusShadow_.depth = kClockDepth;
  statusShadow_.x0 = 0;
  statusShadow_.y0 = kStatusTop;
  if (!statusShadow_.allocAlpha(kScreenW, kStatusH)) return false;
  statusShadow_.color = 0x0000;
  statusShadow_.opacity = 120;

  buildClouds();
  clockText_[0] = 0;
  clockDirty_ = statusDirty_ = true;
  todBucket_ = -1;
  rebuildStack();
  markAllDirty();
  return true;
}

void FaceRenderer::end() {
  glow_.release();
  skyL_.release();
  clouds_.release();
  shadow_.release();
  clock_.release();
  statusShadow_.release();
  status_.release();
  scene_ = nullptr;
  stackN_ = 0;
}

size_t FaceRenderer::bytes() const {
  return skyL_.bytes() + clouds_.bytes() + shadow_.bytes() + clock_.bytes() +
         statusShadow_.bytes() + status_.bytes() + glow_.bytes();
}

void FaceRenderer::rebuildStack() {
  int n = 0;
  stack_[n++] = &skyL_;
  cloudsActive_ = clouds_.spans && (!scene_ || scene_->clouds);
  if (cloudsActive_) stack_[n++] = &clouds_;
  int i = 0, cnt = scene_ ? scene_->count : 0;
  while (i < cnt && scene_->layers[i].depth >= kShadowDepth) stack_[n++] = &scene_->layers[i++];
  stack_[n++] = &shadow_;
  while (i < cnt && scene_->layers[i].depth >= kClockDepth) stack_[n++] = &scene_->layers[i++];
  stack_[n++] = &clock_;
  stack_[n++] = &statusShadow_;
  stack_[n++] = &status_;
  while (i < cnt) stack_[n++] = &scene_->layers[i++];
  stackN_ = n;
  // Night glows sit just in front of the last terrain layer at their depth.
  if (glow_.alpha && scene_) {
    int pos = 0;
    for (int k = 0; k < stackN_; k++)
      if (stack_[k]->kind == LAYER_INDEXED && stack_[k] != &clouds_ &&
          stack_[k]->depth >= glow_.depth - 1e-4f)
        pos = k + 1;
    for (int k = stackN_; k > pos; k--) stack_[k] = stack_[k - 1];
    stack_[pos] = &glow_;
    stackN_++;
  }
}

void FaceRenderer::buildGlows() {
  glow_.release();
  if (!scene_ || scene_->nGlows == 0) return;
  float y0 = 1e9f, y1 = -1e9f;
  for (int i = 0; i < scene_->nGlows; i++) {
    const GlowSpot &g = scene_->glows[i];
    y0 = fminf(y0, g.y - g.r - 1);
    y1 = fmaxf(y1, g.y + g.r + 1);
  }
  int ov = overscanX(scene_->glowDepth);
  glow_.depth = scene_->glowDepth;
  glow_.x0 = (int16_t)-ov;
  glow_.y0 = (int16_t)floorf(y0);
  if (!glow_.allocAlpha((int16_t)(kScreenW + 2 * ov), (int16_t)(ceilf(y1) - floorf(y0)))) return;
  for (int i = 0; i < scene_->nGlows; i++) {
    const GlowSpot &g = scene_->glows[i];
    for (int py = (int)(g.y - g.r - 1); py <= (int)(g.y + g.r + 1); py++)
      for (int px = (int)(g.x - g.r - 1); px <= (int)(g.x + g.r + 1); px++) {
        int lx = px - glow_.x0, ly = py - glow_.y0;
        if (lx < 0 || ly < 0 || lx >= glow_.w || ly >= glow_.h) continue;
        float dx = px + 0.5f - g.x, dy = py + 0.5f - g.y;
        float u = 1.0f - sqrtf(dx * dx + dy * dy) / g.r;
        if (u <= 0) continue;
        uint8_t a = (uint8_t)(u * u * 255.0f);
        uint8_t &d = glow_.alpha[(size_t)ly * glow_.w + lx];
        if (a > d) d = a;
      }
  }
  glow_.color = to565(rgb(255, 206, 136));
  glow_.opacity = 0;
  glow_.visible = false;
  if (!glow_.encodeSpans()) glow_.release();
}

void FaceRenderer::setScene(Scene *s) {
  scene_ = s;
  if (s) {
    skyStyle_ = s->sky;
    int ovy = overscanY(1.0f);
    int need = s->skyBottom + ovy + 4 - skyL_.y0;   // rows from the top overscan
    int cap = kScreenH + 2 * ovy;
    if (need > cap) need = cap;
    // Shrinking h keeps the same row stride (w), so this is safe in place.
    skyL_.h = (int16_t)need;
    skyL_.encodeSpans();
    for (int i = 0; i < s->count; i++) {
      s->layers[i].animDx = 0;
      s->layers[i].animDy = 0;
    }
  }
  todBucket_ = -1;              // force sky + LUTs for the new scene
  buildGlows();
  rebuildStack();
  // Re-apply the current tilt to the new layers.
  for (int i = 0; i < stackN_; i++) { stack_[i]->dx = 0; stack_[i]->dy = 0; }
  setParallax(tiltX_, tiltY_, strength_);
  markAllDirty();
}

void FaceRenderer::relightScene() {
  if (scene_) {
    for (int i = 0; i < scene_->count; i++) buildLuts(scene_->layers[i], scene_->looks[i], sky_);
  }
  if (clouds_.luts) buildLuts(clouds_, cloudLook_, sky_);
}

bool FaceRenderer::setTimeOfDay(const LocalTime &t, bool force) {
  int bucket = (t.hour * 60 + t.minute) / 5;
  int sid = scene_ ? scene_->id : -1;
  if (!force && bucket == todBucket_ && sid == sceneIdLit_) return false;
  todBucket_ = bucket;
  sceneIdLit_ = sid;
  computeSky(t, skyStyle_, sky_);
  renderSky(skyL_, sky_, skyStyle_, &twinkle_);
  relightScene();
  uint16_t txt = to565(sky_.text);
  clock_.color = txt;
  status_.color = txt;
  if (glow_.alpha) {
    float g = smoothstepf(0.10f, 0.75f, sky_.night);
    glow_.opacity = (uint8_t)(170.0f * g);
    glow_.visible = glow_.opacity > 4;
  }
  // Shadow falls away from the key light; deeper at night for contrast.
  shadow_.opacity = (uint8_t)(150 + 40 * sky_.night);
  clockDirty_ = true;           // shadow offset may change with the light
  renderClock();
  markAllDirty();
  return true;
}

void FaceRenderer::setClock(int hour, int minute, bool valid, bool h12) {
  char buf[8];
  if (!valid) {
    snprintf(buf, sizeof(buf), "--:--");
  } else if (h12) {
    int h = hour % 12;
    if (h == 0) h = 12;
    snprintf(buf, sizeof(buf), "%d:%02d", h, minute);
  } else {
    snprintf(buf, sizeof(buf), "%02d:%02d", hour, minute);
  }
  if (strcmp(buf, clockText_) == 0 && !clockDirty_) return;
  strncpy(clockText_, buf, sizeof(clockText_) - 1);
  clockText_[sizeof(clockText_) - 1] = 0;
  clockDirty_ = true;
  renderClock();
}

void FaceRenderer::renderClock() {
  if (!clock_.alpha || !clockDirty_ || !clockText_[0]) return;
  ClockMetrics cm = clockMetrics();
  memset(clock_.alpha, 0, (size_t)clock_.w * clock_.h);
  float w = clockTextWidth(clockText_, cm);
  float x = (kScreenW - w) * 0.5f;
  drawClockText(clock_, x, (float)kClockMargin, clockText_, cm);
  clock_.encodeSpans();
  // Soft shadow: dilated + blurred copy, nudged away from the light.
  blurAlphaInto(clock_, shadow_, 1, 3, 0.95f);
  int offX = (int)lroundf(-sky_.lightX * 2.5f);
  shadow_.x0 = (int16_t)(clock_.x0 + offX);
  shadow_.y0 = (int16_t)(clock_.y0 + 3);
  shadow_.encodeSpans();
  clockDirty_ = false;
  int a0, a1, b0, b1;
  layerExtent(clock_, a0, a1);
  layerExtent(shadow_, b0, b1);
  markRows(a0 < b0 ? a0 : b0, a1 > b1 ? a1 : b1);
}

void FaceRenderer::setStatus(const StatusInfo &st) {
  if (strcmp(st.date, status_info_.date) == 0 && st.battery == status_info_.battery &&
      !statusDirty_)
    return;
  status_info_ = st;
  statusDirty_ = true;
  renderStatus();
}

void FaceRenderer::renderStatus() {
  if (!status_.alpha) return;
  memset(status_.alpha, 0, (size_t)status_.w * status_.h);
  const float bw = 15.0f, bh = 8.0f, gapB = 7.0f;
  bool showBat = status_info_.battery >= 0;
  int batt = status_info_.battery > 100 ? 100 : status_info_.battery;
  char pct[16] = { 0 };
  if (showBat) snprintf(pct, sizeof(pct), "%d%%", batt);
  float wDate = (float)smallTextWidth(status_info_.date, kStatusCap);
  float wPct = showBat ? (float)smallTextWidth(pct, kStatusCap * 0.92f) : 0.0f;
  float total = wDate + (showBat ? gapB * 2 + bw + 3 + 3 + wPct : 0.0f);
  float x = (kScreenW - total) * 0.5f;
  float base = kStatusBase - kStatusTop;
  if (status_info_.date[0]) drawSmallText(status_, x, base, status_info_.date, kStatusCap);
  if (showBat) {
    float bx = x + wDate + gapB * 2;
    drawBatteryGlyph(status_, bx, base - kStatusCap + (kStatusCap - bh) * 0.5f, bw, bh, batt);
    drawSmallText(status_, bx + bw + 6, base, pct, kStatusCap * 0.92f);
  }
  status_.encodeSpans();
  blurAlphaInto(status_, statusShadow_, 1, 2, 1.0f);
  statusShadow_.encodeSpans();
  statusDirty_ = false;
  int a0, a1;
  layerExtent(status_, a0, a1);
  markRows(a0, a1);
}

void FaceRenderer::layerExtent(const Layer &L, int &y0, int &y1) const {
  int top = L.y0 + L.dy + L.animDy;
  int bot = top + L.h;
  if (L.clampTop) top = 0;
  if (L.clampBottom) bot = kScreenH;
  if (top < 0) top = 0;
  if (bot > kScreenH) bot = kScreenH;
  y0 = top;
  y1 = bot;
}

void FaceRenderer::applyOffset(Layer &L, int dx, int dy) {
  if (L.dx == dx && L.dy == dy) return;
  int a0, a1, b0, b1;
  layerExtent(L, a0, a1);
  L.dx = (int16_t)dx;
  L.dy = (int16_t)dy;
  layerExtent(L, b0, b1);
  markRows(a0 < b0 ? a0 : b0, a1 > b1 ? a1 : b1);
}

static inline int hystRound(int cur, float target) {
  if (fabsf(target - (float)cur) > 0.62f) return (int)lroundf(target);
  return cur;
}

bool FaceRenderer::setParallax(float tx, float ty, float strength) {
  tiltX_ = tx;
  tiltY_ = ty;
  strength_ = strength;
  bool changed = false;
  for (int i = 0; i < stackN_; i++) {
    Layer &L = *stack_[i];
    float dX = L.depth, dY = L.depthY < 0 ? L.depth : L.depthY;
    float fx = dX * kMaxShiftX * strength * tx;
    float fy = dY * kMaxShiftX * kVertRatio * strength * ty;
    // Never exceed the overscan the layer was built with.
    float limX = (float)overscanX(dX) - 1.0f, limY = (float)overscanY(dY) - 1.0f;
    fx = clampf(fx, -limX, limX);
    fy = clampf(fy, -limY, limY);
    int nx = hystRound(L.dx, fx), ny = hystRound(L.dy, fy);
    if (nx != L.dx || ny != L.dy) {
      applyOffset(L, nx, ny);
      changed = true;
    }
  }
  return changed;
}

void FaceRenderer::setLayerAnim(int i, int dx, int dy) {
  if (!scene_ || i < 0 || i >= scene_->count) return;
  Layer &L = scene_->layers[i];
  if (L.animDx == dx && L.animDy == dy) return;
  int a0, a1, b0, b1;
  layerExtent(L, a0, a1);
  L.animDx = (int16_t)dx;
  L.animDy = (int16_t)dy;
  layerExtent(L, b0, b1);
  markRows(a0 < b0 ? a0 : b0, a1 > b1 ? a1 : b1);
}

bool FaceRenderer::twinkle(uint32_t ms) {
  if (twinkle_.n == 0 || !skyL_.rgb) return false;
  const int W = skyL_.w;
  const int ox[5] = { 0, -1, 1, 0, 0 }, oy[5] = { 0, 0, 0, -1, 1 };
  for (int i = 0; i < twinkle_.n; i++) {
    TwinkleStar &t = twinkle_.s[i];
    uint32_t h = hash32((uint32_t)(t.x * 7919 + t.y));
    float freq = 0.35f + 0.8f * hashUnit(h);            // Hz
    float ph = 6.2832f * hashUnit(h >> 3);
    float f = 0.5f + 0.5f * sinf(6.2832f * freq * (ms * 0.001f) + ph);
    uint8_t level = (uint8_t)(f * 7.0f + 0.5f);          // 8 steps is plenty
    if (level == t.lastLevel) continue;
    t.lastLevel = level;
    float a = t.b * (0.45f + 0.55f * level / 7.0f);
    for (int k = 0; k < 5; k++) {
      float ak = k == 0 ? a : a * 0.45f;
      skyL_.rgb[(size_t)(t.y + oy[k]) * W + t.x + ox[k]] = to565(screen(from565(t.bg[k]), t.col, sat(ak)));
    }
    int sy = t.y + skyL_.y0 + skyL_.dy;
    markRows(sy - 1, sy + 2);
  }
  return true;
}

int FaceRenderer::layerAnimDy(int i) const {
  if (!scene_ || i < 0 || i >= scene_->count) return 0;
  return scene_->layers[i].animDy;
}

void FaceRenderer::setCloudDrift(int dx) {
  if (!cloudsActive_ || clouds_.animDx == dx) return;
  clouds_.animDx = (int16_t)dx;
  int a0, a1;
  layerExtent(clouds_, a0, a1);
  markRows(a0, a1);
}

// ---------------------------------------------------------------------------
// Clouds: a horizontally tiling band of soft, flat-bottomed cumulus, lit on
// top. Drifts slowly when ambient motion is on.
// ---------------------------------------------------------------------------
void FaceRenderer::buildClouds() {
  const int period = 400, top = 44, h = 62;
  clouds_.depth = 0.92f;
  clouds_.x0 = 0;
  clouds_.y0 = top;
  clouds_.wrapX = true;
  if (!clouds_.allocIndexed(period, h, true, 4)) { clouds_.release(); return; }
  cloudLook_.mats[0] = Material(MAT_RAMP, rgb(255, 255, 255), rgb(176, 186, 210));
  cloudLook_.mats[1] = Material(MAT_RAMP, rgb(240, 236, 240), rgb(150, 156, 184));
  cloudLook_.haze = 0.18f;
  cloudLook_.mist = 0.10f;
  cloudLook_.nightFloor = 0.0f;
  Painter P(clouds_);
  Rng rng(0xC10D5u);
  // A few clouds spread around the loop; each is a cluster of puffs.
  const int nClouds = 4;
  for (int c = 0; c < nClouds; c++) {
    float cx = (c + rng.range(0.15f, 0.85f)) * period / nClouds;
    float cy = rng.range(20.0f, 46.0f);
    float len = rng.range(34.0f, 64.0f);
    int puffs = 5 + (int)(len / 10);
    for (int p = 0; p < puffs; p++) {
      float u = (p + 0.5f) / puffs;
      float px = cx - len * 0.5f + u * len + rng.range(-3.0f, 3.0f);
      float r = (4.0f + 9.0f * sinf(u * 3.14159f)) * rng.range(0.75f, 1.15f);
      float py = cy - r * 0.35f;
      int x0 = (int)floorf(px - r - 1), x1 = (int)ceilf(px + r + 1);
      int y0 = (int)floorf(py - r - 1), y1 = (int)ceilf(cy + 1);
      for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
          float fx = x + 0.5f - px, fy = y + 0.5f - py;
          float d = sqrtf(fx * fx + fy * fy);
          float cov = sat(r + 0.5f - d);
          if (y + 0.5f > cy) cov *= sat(cy + 0.5f - (y + 0.5f) + 1.0f);  // flat base
          if (cov <= 0) continue;
          float lit = sat(0.5f - fy / (2.0f * r)) * 0.8f + 0.2f;
          int wx = ((x % period) + period) % period;
          P.put(wx, y, packIdxF(lit > 0.62f ? 0 : 1, lit), cov * 0.92f);
        }
    }
  }
  if (!clouds_.encodeSpans()) clouds_.release();
}

// ---------------------------------------------------------------------------
// Dirty rows
// ---------------------------------------------------------------------------
void FaceRenderer::markAllDirty() { memset(dirty_, 1, sizeof(dirty_)); }

void FaceRenderer::markRows(int y0, int y1) {
  if (y0 < 0) y0 = 0;
  if (y1 > kScreenH) y1 = kScreenH;
  for (int y = y0; y < y1; y++) dirty_[y] = 1;
}

bool FaceRenderer::anyDirty() const {
  for (int y = 0; y < kScreenH; y++) if (dirty_[y]) return true;
  return false;
}

int FaceRenderer::dirtyCount() const {
  int n = 0;
  for (int y = 0; y < kScreenH; y++) n += dirty_[y];
  return n;
}

bool FaceRenderer::popDirtyRun(int &y0, int &y1, int maxRows) {
  int y = 0;
  while (y < kScreenH && !dirty_[y]) y++;
  if (y >= kScreenH) return false;
  y0 = y;
  while (y < kScreenH && dirty_[y] && y - y0 < maxRows) { dirty_[y] = 0; y++; }
  y1 = y;
  return true;
}

void FaceRenderer::composeRows(int y0, int y1, uint16_t *dst, int stride) {
  tp::composeRows(stack_, stackN_, y0, y1, kScreenW, dst, stride);
}

}  // namespace tp
