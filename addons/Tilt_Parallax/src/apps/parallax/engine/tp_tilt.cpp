#include "tp_tilt.h"
#include <math.h>

namespace tp {

void springStep(float &x, float &v, float target, float w, float dt) {
  if (dt <= 0) return;
  // x(t) = target + (y0 + (v0 + w y0) t) e^{-wt}
  // v(t) = (v0 - w (v0 + w y0) t) e^{-wt}
  float y0 = x - target;
  float e = expf(-w * dt);
  float c = v + w * y0;
  x = target + (y0 + c * dt) * e;
  v = (v - w * c * dt) * e;
}

void TiltFilter::reset() {
  started_ = false;
  stepped_ = false;
  gX_ = 0; gY_ = 0; gZ_ = 1;
  nX_ = 0; nY_ = 0; nZ_ = 1;
  rawX_ = rawY_ = 0;
  tgtX_ = tgtY_ = 0;
  posX_ = posY_ = velX_ = velY_ = 0;
}

static inline void normalise(float &x, float &y, float &z) {
  float m = sqrtf(x * x + y * y + z * z);
  if (m < 1e-6f) { x = 0; y = 0; z = 1; return; }
  x /= m; y /= m; z /= m;
}

void TiltFilter::addSample(int16_t ax, int16_t ay, int16_t az, uint32_t tMs) {
  // Chip frame -> screen frame. BaseOS's IMU diagnostics treat the screen
  // frame as (-x, y, z); see tp_config.h for the orientation flags.
  float fx = -ax / 4096.0f, fy = ay / 4096.0f, fz = az / 4096.0f;
  float mag = sqrtf(fx * fx + fy * fy + fz * fz);
  if (mag < 0.2f || mag > 4.0f) return;          // garbage / free fall: ignore

  if (!started_) {
    started_ = true;
    firstSampleMs_ = lastSampleMs_ = tMs;
    gX_ = fx; gY_ = fy; gZ_ = fz;
    nX_ = fx; nY_ = fy; nZ_ = fz;
    normalise(nX_, nY_, nZ_);
    return;
  }
  float dt = (int32_t)(tMs - lastSampleMs_) * 0.001f;
  lastSampleMs_ = tMs;
  if (dt <= 0) dt = 0.001f;
  if (dt > 1.0f) dt = 1.0f;

  // 1. Low-pass gravity, trusting samples near 1 g more.
  float dev = (mag - 1.0f) / cfg_.trustG;
  float trust = 1.0f / (1.0f + dev * dev);
  float a = (1.0f - expf(-dt / cfg_.lpfTauS)) * trust;
  gX_ += a * (fx - gX_);
  gY_ += a * (fy - gY_);
  gZ_ += a * (fz - gZ_);
  float ux = gX_, uy = gY_, uz = gZ_;
  normalise(ux, uy, uz);

  // 2. Neutral pose drifts toward the current direction.
  float since = (int32_t)(tMs - firstSampleMs_) * 0.001f;
  float tau = since < cfg_.settleS ? cfg_.neutralFastTauS : cfg_.neutralTauS;
  float b = 1.0f - expf(-dt / tau);
  nX_ += b * (ux - nX_);
  nY_ += b * (uy - nY_);
  nZ_ += b * (uz - nZ_);
  normalise(nX_, nY_, nZ_);

  // 3. Rotation taking neutral to current: r = axis * angle. The eye's
  //    displacement across the screen is r x (0,0,1) = (r_y, -r_x).
  float cx = nY_ * uz - nZ_ * uy;
  float cy = nZ_ * ux - nX_ * uz;
  float cz = nX_ * uy - nY_ * ux;
  float s = sqrtf(cx * cx + cy * cy + cz * cz);
  float c = nX_ * ux + nY_ * uy + nZ_ * uz;
  float ex = 0, ey = 0;
  if (s > 1e-7f) {
    float ang = atan2f(s, c);
    float k = ang / s;
    ex = cy * k;           // eye moved right (+) / left (-)
    ey = -cx * k;          // eye moved up (+) / down (-)
  }
  // Far content moves with the eye: screen x right, y down.
  float tx = ex / cfg_.fullAngleRad;
  float ty = -ey / cfg_.fullAngleRad;
  if (cfg_.invertX) tx = -tx;
  if (cfg_.invertY) ty = -ty;
  // Soft saturation on the vector magnitude.
  float m = sqrtf(tx * tx + ty * ty);
  if (m > 1e-6f) {
    float m4 = m * m * m * m;
    float sm = m / sqrtf(sqrtf(1.0f + m4));
    tx *= sm / m;
    ty *= sm / m;
  }
  rawX_ = tx;
  rawY_ = ty;

  // 4. Dead-zone with backlash: the target only moves once the input has
  //    pushed more than deadZone away from it, then follows at that distance.
  float dx = tx - tgtX_, dy = ty - tgtY_;
  float d = sqrtf(dx * dx + dy * dy);
  if (d > cfg_.deadZone) {
    float k = (d - cfg_.deadZone) / d;
    tgtX_ += dx * k;
    tgtY_ += dy * k;
  }
}

void TiltFilter::step(uint32_t tMs, float &outX, float &outY) {
  if (!stepped_) {
    stepped_ = true;
    lastStepMs_ = tMs;
    // Start at rest on the target: no swoosh when the face first appears.
    posX_ = tgtX_; posY_ = tgtY_;
    velX_ = velY_ = 0;
  }
  float dt = (int32_t)(tMs - lastStepMs_) * 0.001f;
  lastStepMs_ = tMs;
  if (dt > 0) {
    springStep(posX_, velX_, tgtX_, cfg_.springOmega, dt);
    springStep(posY_, velY_, tgtY_, cfg_.springOmega, dt);
  }
  outX = posX_;
  outY = posY_;
}

bool TiltFilter::settled() const {
  return fabsf(posX_ - tgtX_) < 0.002f && fabsf(posY_ - tgtY_) < 0.002f &&
         fabsf(velX_) < 0.01f && fabsf(velY_) < 0.01f;
}

}  // namespace tp
