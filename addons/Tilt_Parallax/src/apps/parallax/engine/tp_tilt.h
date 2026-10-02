// Tilt Parallax — from accelerometer samples to smooth layer offsets.
//
// Pipeline (all timestamps in ms, every sample is used):
//   1. Map raw counts to the screen frame (x right, y up, z out of the glass)
//      and low-pass the gravity vector. Samples whose magnitude is far from
//      1 g (the arm is accelerating) are trusted less.
//   2. A neutral pose follows the gravity direction slowly (seconds), so
//      however the wearer naturally holds the watch reads as centred: the
//      face reacts to *changes* in angle. Right after a reset it adapts fast
//      so a wake-up mid-gesture doesn't start off-centre.
//   3. The rotation from neutral to current gravity gives how far the eye has
//      moved relative to the screen (pitch and roll, in radians). That is
//      normalised (full scale ~22 degrees) and softly saturated.
//   4. A small dead-zone with backlash stops sensor noise from nudging the
//      target, and a critically damped spring (no overshoot) eases the
//      displayed offset toward it, integrated exactly for any frame time.
//
// Output is in screen orientation (x right, y down): +x moves deep layers
// right. Pure C++ for unit tests.
#pragma once
#include <stdint.h>

namespace tp {

struct TiltConfig {
  float lpfTauS = 0.05f;          // gravity low-pass time constant
  float neutralTauS = 2.5f;       // neutral pose adaptation (slow)
  float neutralFastTauS = 0.25f;  // ... during the settle window after reset
  float settleS = 1.0f;           // length of that window
  float fullAngleRad = 0.38f;     // tilt (~22 deg) that maps to 1.0
  float deadZone = 0.012f;        // backlash, normalised units
  float springOmega = 11.0f;      // rad/s; critically damped
  float trustG = 0.20f;           // |a| deviation at which a sample counts half
  bool  invertX = false;          // board-orientation fixes (see tp_config.h)
  bool  invertY = false;
};

class TiltFilter {
public:
  explicit TiltFilter(const TiltConfig &c = TiltConfig()) : cfg_(c) { reset(); }

  void configure(const TiltConfig &c) { cfg_ = c; }
  const TiltConfig &config() const { return cfg_; }

  // Forget everything; the next sample becomes the neutral pose.
  void reset();

  // One accelerometer sample: raw counts at 4096 counts/g (MMA8451 +-2 g),
  // in the chip's frame, with its timestamp.
  void addSample(int16_t ax, int16_t ay, int16_t az, uint32_t tMs);

  // Advance the spring to tMs; outputs the smoothed tilt (screen x right,
  // y down), each roughly in [-1, 1].
  void step(uint32_t tMs, float &outX, float &outY);

  // True when the spring is at rest on its target.
  bool settled() const;
  bool hasData() const { return started_; }

  // Diagnostics.
  float targetX() const { return tgtX_; }
  float targetY() const { return tgtY_; }
  float rawX() const { return rawX_; }   // before dead-zone
  float rawY() const { return rawY_; }
  void  neutral(float &x, float &y, float &z) const { x = nX_; y = nY_; z = nZ_; }

private:
  TiltConfig cfg_;
  bool     started_ = false;
  uint32_t lastSampleMs_ = 0, firstSampleMs_ = 0, lastStepMs_ = 0;
  bool     stepped_ = false;
  float    gX_ = 0, gY_ = 0, gZ_ = 1;        // low-passed gravity (g)
  float    nX_ = 0, nY_ = 0, nZ_ = 1;        // neutral direction (unit)
  float    rawX_ = 0, rawY_ = 0;             // normalised tilt before dead-zone
  float    tgtX_ = 0, tgtY_ = 0;             // spring target
  float    posX_ = 0, posY_ = 0, velX_ = 0, velY_ = 0;
};

// Exact critically damped spring step (exposed for tests): advances position
// x and velocity v toward `target` by dt seconds with natural frequency w.
void springStep(float &x, float &v, float target, float w, float dt);

}  // namespace tp
