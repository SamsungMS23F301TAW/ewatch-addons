// The Arduino build compiles everything at -Os; these per-pixel and
// per-sample loops run every frame, so they get -O2 on the watch.
#if defined(ESP_PLATFORM) && defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif
#include "oracle_scene.h"

#include <math.h>
#include <string.h>

#include "oracle_config.h"

namespace oracle {

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kTwoPi = 6.2831853f;

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float smoothstep(float e0, float e1, float x) {
  float t = clampf((x - e0) / (e1 - e0), 0.f, 1.f);
  return t * t * (3.f - 2.f * t);
}
// Exponential approach: move `v` toward `target` with time constant tau.
inline float approach(float v, float target, float tau, float dt) {
  if (tau <= 0) return target;
  return v + (target - v) * (1.f - expf(-dt / tau));
}
// Wrap into (-pi, pi]. fmodf keeps this O(1) for any input; a non-finite
// input comes back NaN and is caught by the guards in stepDie().
inline float wrapPi(float a) {
  a = fmodf(a + kPi, kTwoPi);
  if (a < 0) a += kTwoPi;
  return a - kPi;
}
inline bool finite(float v) { return v == v && v < 1e30f && v > -1e30f; }

// Depth (0 = against the glass, 1 = deep) to perspective scale and fog.
inline float depthScale(float d) { return 1.f / (1.f + 0.85f * clampf(d, 0.f, 1.2f)); }
inline float depthFog(float d) { return smoothstep(0.02f, 1.0f, d) * 0.97f; }

}  // namespace

void Scene::reset(uint32_t seed) {
  rng_.seed64(seed ? seed : 1);
  phase_ = Phase::Idle;
  phaseT_ = 0;
  t_ = 0;
  x_ = vx_ = vy_ = 0;
  y_ = kDieRestDy;
  depth_ = 0.86f;
  vDepth_ = 0;
  ang_ = rng_.unit() * kTwoPi - kPi;
  vAng_ = 0.15f;
  tumble_ = 0.6f;
  vTumble_ = 0.2f;
  tumbleAxis_ = rng_.unit() * kPi;
  textA_ = 0;
  gold_ = false;
  landed_ = false;
  glint_ = -1;
  settleT_ = 0;
  impact_ = 0;
  rumbleEnv_ = 0;
  murk_ = 0.38f;
  stir_ = 0;
  swirl_ = 0;
  swirlV_ = 0.04f;
  murkX_ = rng_.unit() * 128.f;
  murkY_ = rng_.unit() * 128.f;
  promptA_ = 0;
  idleBubbleT_ = 0.6f;
  tiltPrimed_ = false;
  upX_ = baseX_ = 0;
  upY_ = baseY_ = -0.6f;
  for (auto &b : bubbles_) b.alive = false;
  bubbleCount_ = 0;
  for (auto &m : motes_) spawnMote(m, true);
  buildFrame();
}

void Scene::setPhase(Phase p) {
  phase_ = p;
  phaseT_ = 0;
}

bool Scene::busy() const {
  if (phase_ == Phase::Churning || phase_ == Phase::Rising || phase_ == Phase::Sinking)
    return true;
  if (stir_ > 0.08f || fabsf(swirlV_) > 0.25f) return true;
  if (phase_ == Phase::Idle && promptA_ < 0.98f && phaseT_ < 1.5f) return true;
  if (gold_ && phase_ == Phase::Showing) return true;   // glint and sparkles
  for (const auto &b : bubbles_)
    if (b.alive && (b.view.front || b.view.spark)) return true;
  return false;
}

// ------------------------------------------------------------------ events

void Scene::shakeStarted() {
  if (phase_ == Phase::Churning) return;
  setPhase(Phase::Churning);
  landed_ = false;
  glint_ = -1;
  wanderT_ = 0;
  // Knock the die off the glass and set it spinning.
  vDepth_ += 1.6f;
  vAng_ += (rng_.unit() - 0.5f) * 10.f;
  vTumble_ += 4.f + rng_.unit() * 4.f;
  swirlV_ += (rng_.unit() < 0.5f ? -1.f : 1.f) * 1.2f;
  // A burst of bubbles as the liquid churns.
  for (int i = 0; i < 8; i++) {
    float a = rng_.unit() * kTwoPi, r = rng_.unit() * 60.f;
    spawnBubble(kBallCX + cosf(a) * r, kBallCY + sinf(a) * r, 0.9f + rng_.unit() * 1.4f,
                (rng_.unit() - 0.5f) * 40.f, -10.f - rng_.unit() * 30.f,
                0.5f + rng_.unit() * 0.8f, false, false);
  }
}

void Scene::shakeStopped(bool golden) {
  gold_ = golden;
  landed_ = false;
  glint_ = -1;
  settleT_ = 0;
  textA_ = 1.f;
  ang_ = wrapPi(ang_);
  // Keep a little spin so it visibly turns to face you on the way up.
  if (fabsf(ang_) < 0.35f) vAng_ += (ang_ >= 0 ? 1.f : -1.f) * 1.2f;
  setPhase(Phase::Rising);
}

void Scene::tap(float x, float y) {
  if (phase_ == Phase::Showing || phase_ == Phase::Rising) {
    setPhase(Phase::Sinking);
    vDepth_ = 0.25f;
    vAng_ += (rng_.unit() - 0.5f) * 1.2f;
    return;
  }
  if (phase_ == Phase::Idle) {
    // A playful poke: a little stream of bubbles from the fingertip.
    float dx = x - kBallCX, dy = y - kBallCY;
    float r = sqrtf(dx * dx + dy * dy);
    if (r > kWinR - 8.f) {
      float k = (kWinR - 8.f) / (r > 1e-3f ? r : 1.f);
      x = kBallCX + dx * k;
      y = kBallCY + dy * k;
    }
    for (int i = 0; i < 6; i++) {
      spawnBubble(x + (rng_.unit() - 0.5f) * 6.f, y + (rng_.unit() - 0.5f) * 6.f,
                  0.9f + rng_.unit() * 1.8f, (rng_.unit() - 0.5f) * 14.f,
                  -18.f - rng_.unit() * 22.f, 0.9f + rng_.unit() * 0.9f, true, false);
    }
    swirlV_ += (dx >= 0 ? 0.25f : -0.25f);
  }
}

void Scene::packChanged() {
  if (phase_ == Phase::Showing || phase_ == Phase::Rising) {
    setPhase(Phase::Sinking);
    vDepth_ = 0.3f;
  }
  // Swirl the murk so the change feels like a new oracle waking up, and let
  // the new pack's prompt fade in from nothing.
  swirlV_ += (rng_.unit() < 0.5f ? -0.9f : 0.9f);
  stir_ = clampf(stir_ + 0.25f, 0.f, 1.f);
  promptA_ = 0.f;
  if (phase_ == Phase::Idle) setPhase(Phase::Idle);
}

// ------------------------------------------------------------------ update

SceneCues Scene::update(const SceneInput &inRaw) {
  SceneCues cues;
  SceneInput in = inRaw;
  float dtAll = clampf(in.dt, 0.f, 0.1f);

  // Tilt: the in-plane "up" direction, with a slow baseline.
  float ux = in.imuOk ? in.upX : 0.f, uy = in.imuOk ? in.upY : -0.6f;
  if (!tiltPrimed_) {
    upX_ = baseX_ = ux;
    upY_ = baseY_ = uy;
    tiltPrimed_ = true;
  }
  upX_ = approach(upX_, ux, 0.12f, dtAll);
  upY_ = approach(upY_, uy, 0.12f, dtAll);
  baseX_ = approach(baseX_, upX_, 2.5f, dtAll);
  baseY_ = approach(baseY_, upY_, 2.5f, dtAll);

  // Sub-step the physics so stiff springs stay stable at low frame rates.
  int steps = (int)ceilf(dtAll / 0.008f);
  if (steps < 1) steps = 1;
  float dt = dtAll / steps;
  for (int i = 0; i < steps; i++) {
    t_ += dt;
    phaseT_ += dt;
    stepDie(dt, in, cues);
  }
  stepLiquid(dtAll, in);
  stepBubbles(dtAll, in);

  // The rumble follows the live shake through a peak follower (instant
  // attack, ~0.1 s release), so it fades as soon as you stop; the churn
  // itself lasts until the detector is sure, about 0.4 s later.
  float level = clampf(in.dynMag / 2.2f, 0.f, 1.f);
  if (level > rumbleEnv_) rumbleEnv_ = level;
  else rumbleEnv_ = approach(rumbleEnv_, level, 0.10f, dtAll);
  if (phase_ == Phase::Churning && in.shaking) {
    float e = in.dynMag > 0.f ? rumbleEnv_ : clampf(in.intensity, 0.f, 1.f);
    cues.rumble = e < 0.18f ? 0.f : sqrtf(e);
  }
  buildFrame();
  return cues;
}

void Scene::stepDie(float dt, const SceneInput &in, SceneCues &cues) {
  impact_ -= impact_ * (1.f - expf(-dt / 0.09f));
  if (impact_ < 0.002f) impact_ = 0.f;

  // Drift target from tilt: react to changes, keep a little of the absolute.
  float dX = (upX_ - baseX_) * kTiltDriftPx + upX_ * 2.5f;
  float dY = (upY_ - baseY_) * kTiltDriftPx + upY_ * 2.5f;
  dX = clampf(dX, -kTiltDriftPx, kTiltDriftPx);
  dY = clampf(dY, -kTiltDriftPx, kTiltDriftPx);

  float tx = dX, ty = dY + kDieRestDy;   // position target
  float kp = 14.f, cp = 5.f;
  float angT = 0, kA = 28.f, cA = 3.4f;
  float sinkTo = 0.86f;   // idle depth: a faint shape lurking in the deep

  switch (phase_) {
    case Phase::Idle: {
      // Lost in the deep: drift and turn lazily, barely visible.
      tx = 10.f * sinf(t_ * 0.21f);
      ty = kDieRestDy + 6.f * sinf(t_ * 0.17f + 1.f);
      kp = 2.f; cp = 2.f;
      vAng_ += (0.18f - vAng_) * (1.f - expf(-dt / 1.5f));
      ang_ += vAng_ * dt;
      vDepth_ += ((sinkTo - depth_) * 3.f - vDepth_ * 2.5f) * dt;
      depth_ += vDepth_ * dt;
      vTumble_ += (0.25f - vTumble_) * (1.f - expf(-dt / 2.f));
      tumble_ += vTumble_ * dt;
      textA_ = approach(textA_, 0.f, 0.3f, dt);
      break;
    }
    case Phase::Churning: {
      float I = clampf(in.intensity, 0.f, 1.f);
      // Random wandering target, re-picked a few times a second.
      wanderT_ -= dt;
      if (wanderT_ <= 0.f) {
        wanderT_ = 0.25f + 0.25f * rng_.unit();
        wanderA_ = rng_.unit() * kTwoPi;
        wanderR_ = rng_.unit() * (10.f + 14.f * I);
      }
      tx = cosf(wanderA_) * wanderR_;
      ty = kDieRestDy + sinf(wanderA_) * wanderR_;
      kp = 10.f; cp = 3.5f;
      // Slosh: the liquid lags the watch, so the die is pushed the other way.
      vx_ += -in.dynX * 900.f * dt;
      vy_ += -in.dynY * 900.f * dt;
      // Tumble and spin, harder for a harder shake.
      float spinT = (rng_.unit() - 0.5f) * 2.f * (3.f + 7.f * I);
      vAng_ += (spinT - vAng_) * (1.f - expf(-dt / 0.35f));
      ang_ += vAng_ * dt;
      vTumble_ += ((2.5f + 6.f * I) - vTumble_) * (1.f - expf(-dt / 0.3f));
      tumble_ += vTumble_ * dt;
      tumbleAxis_ += 0.7f * dt;
      float dT = 0.50f + 0.10f * sinf(t_ * 2.3f) + 0.06f * (rng_.unit() - 0.5f);
      vDepth_ += ((dT - depth_) * 16.f - vDepth_ * 5.f) * dt;
      depth_ += vDepth_ * dt;
      textA_ = approach(textA_, 0.f, 0.08f, dt);
      angT = ang_;   // no angular spring while churning
      kA = 0; cA = 0;
      break;
    }
    case Phase::Rising:
    case Phase::Showing: {
      // Buoyancy against drag; the glass is a floor at depth 0.
      const float buoy = 4.2f, drag = 3.4f;
      vDepth_ += (-buoy - drag * vDepth_) * dt;
      depth_ += vDepth_ * dt;
      if (depth_ <= 0.f) {
        depth_ = 0.f;
        if (vDepth_ < 0.f) {
          float impact = -vDepth_;
          if (!landed_) {
            landed_ = true;
            cues.landed = true;
            cues.goldLanded = gold_;
            // The knock sets it wobbling (gently: it hit the glass face-on).
            vAng_ += (rng_.unit() < 0.5f ? -1.f : 1.f) * (0.30f + 0.25f * impact);
            vTumble_ += 1.2f * impact;
            impact_ = 1.f;
            // Bubbles squeezed out around the edges.
            for (int i = 0; i < 7; i++) {
              float a = -kPi * 0.5f + i * (kTwoPi / 7.f) + rng_.unit() * 0.4f;
              float r = kDieR * (0.55f + 0.25f * rng_.unit());
              spawnBubble(kBallCX + x_ + cosf(a) * r, kBallCY + y_ + sinf(a) * r,
                          0.8f + rng_.unit() * 1.1f, cosf(a) * 18.f, sinf(a) * 18.f - 14.f,
                          0.6f + rng_.unit() * 0.5f, true, false);
            }
            if (gold_) glint_ = 0.f;
          }
          vDepth_ = impact > 0.15f ? impact * 0.28f : 0.f;
        }
      }
      // Turn to face the glass (tumble to the nearest flat orientation).
      float flat = roundf(tumble_ / kPi) * kPi;
      vTumble_ += (-(tumble_ - flat) * 34.f - vTumble_ * 6.5f) * dt;
      tumble_ += vTumble_ * dt;
      if (phase_ == Phase::Showing) {
        angT = 0.014f * sinf(t_ * 0.8f);
        tx += 0.9f * sinf(t_ * 0.6f);
        ty += 0.7f * sinf(t_ * 0.93f + 0.5f);
      }
      if (phase_ == Phase::Rising && landed_) {
        settleT_ += dt;
        bool calm = fabsf(vAng_) < 0.12f && fabsf(ang_) < 0.04f && depth_ < 0.01f &&
                    fabsf(vDepth_) < 0.05f;
        if (calm || settleT_ > 2.6f) {
          setPhase(Phase::Showing);
          cues.settled = true;
        }
      }
      if (gold_ && glint_ >= 0.f) {
        glint_ += dt / 0.75f;
        if (glint_ > 3.2f) glint_ = 0.f;   // sweep, pause, sweep again
      }
      textA_ = 1.f;
      break;
    }
    case Phase::Sinking: {
      // Heavier than the liquid now: sink with drag, fade into the murk.
      vDepth_ += (2.6f - 3.0f * vDepth_) * dt;
      depth_ += vDepth_ * dt;
      vTumble_ += (0.6f - vTumble_) * (1.f - expf(-dt / 0.6f));
      tumble_ += vTumble_ * dt;
      angT = ang_;
      kA = 0;
      cA = 0.6f;
      ang_ += vAng_ * dt;
      if (depth_ >= 0.9f) {
        setPhase(Phase::Idle);
        gold_ = false;
        glint_ = -1;
      }
      break;
    }
  }

  // Angular spring (Rising/Showing) or free spin with light damping.
  if (kA > 0) {
    ang_ = wrapPi(ang_);
    vAng_ += (-(ang_ - angT) * kA - vAng_ * cA) * dt;
    ang_ += vAng_ * dt;
  } else if (cA > 0) {
    vAng_ -= vAng_ * cA * dt;
  }

  // Position spring.
  vx_ += ((tx - x_) * kp - vx_ * cp) * dt;
  vy_ += ((ty - y_) * kp - vy_ * cp) * dt;
  x_ += vx_ * dt;
  y_ += vy_ * dt;
  // Keep the die inside the glass.
  float lim = 24.f;
  float ry = y_ - kDieRestDy;
  float r = sqrtf(x_ * x_ + ry * ry);
  if (r > lim) {
    x_ *= lim / r;
    y_ = kDieRestDy + ry * (lim / r);
    vx_ *= 0.5f;
    vy_ *= 0.5f;
  }
  depth_ = clampf(depth_, 0.f, 1.1f);
  // Free spins accumulate; keep the angles small so float precision holds.
  if (fabsf(ang_) > 64.f) ang_ = wrapPi(ang_);
  if (fabsf(tumble_) > 64.f) tumble_ = fmodf(tumble_, kTwoPi);   // squash is pi-periodic
  // Guards: a NaN or infinity must never reach the renderer.
  if (!finite(ang_) || !finite(vAng_)) { ang_ = 0; vAng_ = 0; }
  if (!finite(x_) || !finite(y_) || !finite(vx_) || !finite(vy_)) {
    x_ = vx_ = vy_ = 0;
    y_ = kDieRestDy;
  }
  if (!finite(depth_) || !finite(vDepth_)) { depth_ = 0.5f; vDepth_ = 0; }
  if (!finite(tumble_) || !finite(vTumble_)) { tumble_ = 0; vTumble_ = 0; }
}

void Scene::stepLiquid(float dt, const SceneInput &in) {
  float I = clampf(in.intensity, 0.f, 1.f);
  float murkT = 0.38f, stirT = 0.f;
  if (phase_ == Phase::Churning) {
    murkT = 0.55f + 0.45f * I;
    stirT = 0.25f + 0.6f * I;
    float dir = swirlV_ >= 0 ? 1.f : -1.f;
    swirlV_ = approach(swirlV_, dir * (1.0f + 2.4f * I), 0.4f, dt);
  } else {
    float dir = swirlV_ >= 0 ? 1.f : -1.f;
    swirlV_ = approach(swirlV_, dir * 0.04f, 1.8f, dt);
  }
  murk_ = approach(murk_, murkT, phase_ == Phase::Churning ? 0.3f : 2.0f, dt);
  stir_ = approach(stir_, stirT, phase_ == Phase::Churning ? 0.25f : 1.6f, dt);
  swirl_ += swirlV_ * dt;
  if (swirl_ > kTwoPi) swirl_ -= kTwoPi;
  if (swirl_ < 0) swirl_ += kTwoPi;
  murkX_ += (2.5f + 26.f * stir_) * dt - in.dynX * 30.f * dt;
  murkY_ += (-1.8f - 18.f * stir_) * dt - in.dynY * 30.f * dt;
  if (murkX_ > 1280.f || murkX_ < -1280.f) murkX_ = fmodf(murkX_, 128.f);
  if (murkY_ > 1280.f || murkY_ < -1280.f) murkY_ = fmodf(murkY_, 128.f);

  float promptT = (phase_ == Phase::Idle && phaseT_ > 0.35f) ? 1.f : 0.f;
  promptA_ = approach(promptA_, promptT, promptT > promptA_ ? 0.45f : 0.12f, dt);
}

void Scene::spawnBubble(float x, float y, float r, float vx, float vy, float life,
                        bool front, bool spark) {
  for (auto &b : bubbles_) {
    if (b.alive) continue;
    b.alive = true;
    b.view.x = x;
    b.view.y = y;
    b.view.r = r;
    b.view.a = 0;
    b.view.front = front;
    b.view.spark = spark;
    b.vx = vx;
    b.vy = vy;
    b.age = 0;
    b.life = life;
    b.wob = rng_.unit() * kTwoPi;
    b.wobF = 5.f + rng_.unit() * 5.f;
    return;
  }
}

void Scene::spawnMote(M &m, bool anywhere) {
  float a = rng_.unit() * kTwoPi;
  float r = anywhere ? sqrtf(rng_.unit()) * (kWinR - 6.f) : (kWinR - 8.f);
  m.x = kBallCX + cosf(a) * r;
  m.y = kBallCY + sinf(a) * r;
  m.a = 0.10f + 0.22f * rng_.unit();
  m.ph = rng_.unit() * kTwoPi;
}

void Scene::stepBubbles(float dt, const SceneInput &in) {
  float I = clampf(in.intensity, 0.f, 1.f);
  // Which way is up for a bubble: mostly toward the high side, biased to
  // screen-up so a flat watch still has a sensible direction.
  float bx = upX_ * 0.8f, by = upY_ * 0.8f - 0.35f;
  float bl = sqrtf(bx * bx + by * by);
  if (bl < 1e-3f) { bx = 0; by = -1; } else { bx /= bl; by /= bl; }

  // Spawning.
  if (phase_ == Phase::Churning) {
    float rate = 6.f + 22.f * I;
    float expected = rate * dt;
    while (expected > 0.f) {
      if (rng_.unit() < expected) {
        float a = rng_.unit() * kTwoPi, r = sqrtf(rng_.unit()) * (kWinR - 10.f);
        spawnBubble(kBallCX + cosf(a) * r, kBallCY + sinf(a) * r, 0.7f + rng_.unit() * 1.6f,
                    (rng_.unit() - 0.5f) * 30.f, (rng_.unit() - 0.5f) * 30.f,
                    0.4f + rng_.unit() * 0.9f, rng_.unit() < 0.3f, false);
      }
      expected -= 1.f;
    }
  } else {
    idleBubbleT_ -= dt;
    if (idleBubbleT_ <= 0.f) {
      idleBubbleT_ = 0.8f + rng_.unit() * 1.6f;
      // Start low (opposite "up") so it has a long way to rise.
      float side = (rng_.unit() - 0.5f) * 70.f;
      float sx = kBallCX - bx * 70.f + (-by) * side;
      float sy = kBallCY - by * 70.f + bx * side;
      float r = 1.1f + rng_.unit() * 1.7f;
      float speed = 10.f + rng_.unit() * 8.f + r * 2.f;
      spawnBubble(sx, sy, r, bx * speed, by * speed, 4.5f + rng_.unit() * 2.f, false, false);
    }
  }
  // Gold: twinkling sparks around the die while it shows.
  if (gold_ && (phase_ == Phase::Showing || (phase_ == Phase::Rising && landed_))) {
    if (rng_.unit() < 9.f * dt) {
      float a = rng_.unit() * kTwoPi, r = kDieR * (0.35f + 0.6f * rng_.unit());
      spawnBubble(kBallCX + x_ + cosf(a) * r, kBallCY + y_ + sinf(a) * r * 0.8f,
                  1.0f + rng_.unit() * 1.2f, 0, -6.f, 0.5f + rng_.unit() * 0.6f, true, true);
    }
  }

  // Motion.
  float sw = swirlV_ * 0.8f;
  bubbleCount_ = 0;
  for (auto &b : bubbles_) {
    if (!b.alive) continue;
    b.age += dt;
    if (b.age >= b.life) { b.alive = false; continue; }
    float dx = b.view.x - kBallCX, dy = b.view.y - kBallCY;
    if (!b.view.spark) {
      // Buoyancy (bigger rises faster), swirl, wobble, drag.
      float lift = 22.f + 6.f * b.view.r;
      b.vx += (bx * lift - b.vx * 1.6f) * dt;
      b.vy += (by * lift - b.vy * 1.6f) * dt;
      float wob = sinf(b.age * b.wobF + b.wob) * 7.f;
      b.view.x += (b.vx + (-dy) * sw + (-by) * wob) * dt;
      b.view.y += (b.vy + dx * sw + bx * wob) * dt;
    } else {
      b.view.x += b.vx * dt;
      b.view.y += b.vy * dt;
    }
    float r = sqrtf(dx * dx + dy * dy);
    if (r > kWinR - b.view.r - 2.f && b.age < b.life - 0.18f) b.age = b.life - 0.18f;
    // Fade in, hold, fade out.
    float a = smoothstep(0.f, 0.15f, b.age) * (1.f - smoothstep(b.life - 0.3f, b.life, b.age));
    if (b.view.spark) a *= 0.55f + 0.45f * sinf(b.age * 22.f + b.wob);
    b.view.a = a * (b.view.spark ? 1.f : 0.85f);
    bubbleView_[bubbleCount_++] = b.view;
  }

  // Sediment motes ride the swirl and drift.
  float swm = swirlV_ * 0.7f;
  for (int i = 0; i < kMaxMotes; i++) {
    M &m = motes_[i];
    float dx = m.x - kBallCX, dy = m.y - kBallCY;
    m.ph += dt * (0.5f + 0.3f * (i % 3));
    m.x += ((-dy) * swm + sinf(m.ph) * 2.2f + bx * 1.2f - in.dynX * 25.f) * dt;
    m.y += (dx * swm + cosf(m.ph * 0.8f) * 1.8f + by * 1.2f - in.dynY * 25.f) * dt;
    float rr = (m.x - kBallCX) * (m.x - kBallCX) + (m.y - kBallCY) * (m.y - kBallCY);
    if (rr > (kWinR - 4.f) * (kWinR - 4.f)) spawnMote(m, true);
    moteView_[i].x = m.x;
    moteView_[i].y = m.y;
    moteView_[i].a = m.a * (0.6f + 0.4f * sinf(m.ph * 1.7f)) * (0.7f + 0.8f * stir_);
  }
}

void Scene::buildFrame() {
  FrameState &f = frame_;
  f.t = t_;
  f.dieVisible = true;
  f.dieGold = gold_;
  f.dieX = kBallCX + x_;
  f.dieY = kBallCY + y_;
  f.dieAngle = ang_;
  f.dieScale = depthScale(depth_) * (1.f + 0.03f * impact_);
  f.dieFlash = 0.8f * impact_;
  f.dieSquash = 0.22f + 0.78f * fabsf(cosf(tumble_));
  f.dieSquashAxis = tumbleAxis_;
  f.dieFog = depthFog(depth_);
  f.dieTextAlpha = textA_;
  f.dieGlint = (gold_ && glint_ >= 0.f && glint_ <= 1.f) ? glint_ : -1.f;
  f.murkX = murkX_;
  f.murkY = murkY_;
  f.swirl = swirl_;
  f.murk = murk_;
  f.stir = stir_;
  f.promptAlpha = promptA_;
  f.promptDy = 1.6f * sinf(t_ * 1.1f);
  float gx = -(upX_ - baseX_) * kGlareShiftPx * 1.5f;
  float gy = -(upY_ - baseY_) * kGlareShiftPx * 1.5f;
  f.glareDx = clampf(gx, -kGlareShiftPx, kGlareShiftPx);
  f.glareDy = clampf(gy, -kGlareShiftPx, kGlareShiftPx);
  f.bubbles = bubbleView_;
  f.bubbleCount = bubbleCount_;
  f.motes = moteView_;
  f.moteCount = kMaxMotes;
}

}  // namespace oracle
