// Shake Oracle scene: the state machine and the physics. Portable C++.
//
// Deterministic for a given seed and input sequence, so host tests and the
// preview renderer drive exactly the code that runs on the watch.
//
//   Idle      die lost in the deep, prompt glowing, slow bubbles
//   Churning  the user is shaking: die tumbles in a stirred-up murk
//   Rising    shaking stopped: the die floats up, hits the glass, wobbles
//   Showing   answer settled against the glass, gently bobbing
//   Sinking   tapped away: the die sinks back into the dark
#pragma once
#include <stdint.h>

#include "answers.h"
#include "oracle_render.h"

namespace oracle {

enum class Phase : uint8_t { Idle, Churning, Rising, Showing, Sinking };

struct SceneInput {
  float dt = 0.03f;          // seconds since the previous update
  bool  shaking = false;     // ShakeDetector::shaking()
  float intensity = 0;       // ShakeDetector::intensity(), 0..1
  float dynX = 0, dynY = 0;  // gravity-removed acceleration, screen axes, g
  float dynMag = 0;          // |gravity-removed acceleration| (all axes), g
  float upX = 0, upY = 0;    // in-plane "up" (the high side), screen axes, g
  bool  imuOk = false;
};

struct SceneCues {
  bool  landed = false;      // the die just touched the glass (thunk now)
  bool  settled = false;     // the answer just came to rest
  bool  goldLanded = false;  // landed and it is the golden answer
  float rumble = 0;          // 0..1 rumble level wanted right now
};

class Scene {
 public:
  static constexpr int kMaxBubbles = 40;
  static constexpr int kMaxMotes = 30;

  void reset(uint32_t seed);

  // Events from the view.
  void shakeStarted();
  void shakeStopped(bool golden);     // the answer text is already on the die
  void tap(float x, float y);         // clears an answer, or blows bubbles
  void packChanged();                 // sinks any answer from the old pack

  SceneCues update(const SceneInput &in);

  const FrameState &frame() const { return frame_; }
  Phase phase() const { return phase_; }
  bool  answerVisible() const { return phase_ == Phase::Rising || phase_ == Phase::Showing; }
  // True while something moves fast enough to deserve the busy frame rate.
  bool  busy() const;
  float timeInPhase() const { return phaseT_; }

 private:
  Pcg32 rng_;
  Phase phase_ = Phase::Idle;
  float phaseT_ = 0;
  float t_ = 0;

  // Die physics. Position is the centroid's offset from the window centre.
  float x_ = 0, y_ = 0, vx_ = 0, vy_ = 0;
  float depth_ = 0.95f, vDepth_ = 0;
  float ang_ = 0, vAng_ = 0;
  float tumble_ = 0, vTumble_ = 0;
  float tumbleAxis_ = 0;
  float textA_ = 0;
  bool  gold_ = false;
  bool  landed_ = false;
  float glint_ = -1;
  float settleT_ = 0;
  float wanderT_ = 0, wanderA_ = 0, wanderR_ = 0;   // churn wander target
  float impact_ = 0;                                // landing pop, decays
  float rumbleEnv_ = 0;                             // peak follower for the rumble

  // Liquid.
  float murk_ = 0.35f, stir_ = 0;
  float swirl_ = 0, swirlV_ = 0.03f;
  float murkX_ = 0, murkY_ = 0;
  float promptA_ = 0;
  float idleBubbleT_ = 1.0f;

  // Tilt: a slow baseline so the die reacts to changes, then mostly recentres.
  float upX_ = 0, upY_ = -0.6f, baseX_ = 0, baseY_ = -0.6f;
  bool  tiltPrimed_ = false;

  struct B {
    Bubble view;
    float  vx, vy, age, life, wob, wobF;
    bool   alive;
  };
  B      bubbles_[kMaxBubbles];
  Bubble bubbleView_[kMaxBubbles];
  int    bubbleCount_ = 0;

  struct M {
    float x, y, a, ph;
  };
  M    motes_[kMaxMotes];
  Mote moteView_[kMaxMotes];

  FrameState frame_;

  void setPhase(Phase p);
  void spawnBubble(float x, float y, float r, float vx, float vy, float life,
                   bool front, bool spark);
  void spawnMote(M &m, bool anywhere);
  void stepDie(float dt, const SceneInput &in, SceneCues &cues);
  void stepLiquid(float dt, const SceneInput &in);
  void stepBubbles(float dt, const SceneInput &in);
  void buildFrame();
};

}  // namespace oracle
