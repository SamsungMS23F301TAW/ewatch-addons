// Tilt filter: neutral-pose adaptation, spring behaviour (no overshoot),
// dead-zone, direction conventions, saturation and shake rejection.
#include <unity.h>
#include <math.h>
#include <stdlib.h>
#include "tp_tilt.h"
#include "tp_ring.h"

using namespace tp;

static const float kPi = 3.14159265f;

// Raw MMA8451 counts for a screen-frame "up" vector (x right, y up, z out).
// The filter maps chip -> screen as (-x, y, z), so invert that here.
static void upToRaw(float ux, float uy, float uz, int16_t &ax, int16_t &ay, int16_t &az) {
  ax = (int16_t)lroundf(-ux * 4096.0f);
  ay = (int16_t)lroundf(uy * 4096.0f);
  az = (int16_t)lroundf(uz * 4096.0f);
}

// Pose: pitch (about screen x, top edge toward/away) and roll (about screen
// y, right edge up/down) applied to a watch held `base` degrees toward the
// face (a typical glance).
static void poseUp(float baseDeg, float pitchDeg, float rollDeg, float &x, float &y, float &z) {
  float b = (baseDeg + pitchDeg) * kPi / 180.0f, r = rollDeg * kPi / 180.0f;
  // start from "up" for a watch tilted by b about x: (0, sin b, cos b)
  float x0 = 0, y0 = sinf(b), z0 = cosf(b);
  // roll about the screen y axis
  x = x0 * cosf(r) + z0 * sinf(r);
  y = y0;
  z = -x0 * sinf(r) + z0 * cosf(r);
}

struct Sim {
  TiltFilter f;
  uint32_t t = 1000;
  float ox = 0, oy = 0;
  // feeds `seconds` of samples at ~45 Hz (jittered 18-26 ms), stepping the
  // spring every frame like the face does.
  void run(float seconds, float baseDeg, float pitchDeg, float rollDeg, float noiseG = 0,
           unsigned seed = 1) {
    srand(seed);
    uint32_t end = t + (uint32_t)(seconds * 1000);
    while (t < end) {
      t += 18 + (uint32_t)(rand() % 9);
      float x, y, z;
      poseUp(baseDeg, pitchDeg, rollDeg, x, y, z);
      if (noiseG > 0) {
        x += noiseG * ((rand() / (float)RAND_MAX) * 2 - 1);
        y += noiseG * ((rand() / (float)RAND_MAX) * 2 - 1);
        z += noiseG * ((rand() / (float)RAND_MAX) * 2 - 1);
      }
      int16_t ax, ay, az;
      upToRaw(x, y, z, ax, ay, az);
      f.addSample(ax, ay, az, t);
      f.step(t, ox, oy);
    }
  }
};

void setUp() {}
void tearDown() {}

// Whatever angle the wrist rests at reads as centre.
static void test_any_resting_pose_reads_centred() {
  const float poses[][2] = { { 0, 0 }, { 35, 0 }, { 60, 10 }, { 20, -25 } };
  for (auto &p : poses) {
    Sim s;
    s.run(8.0f, p[0], 0, p[1]);
    TEST_ASSERT_FLOAT_WITHIN(0.02f, 0.0f, s.ox);
    TEST_ASSERT_FLOAT_WITHIN(0.02f, 0.0f, s.oy);
  }
}

// A change in angle produces an offset, which then recentres over seconds.
static void test_change_then_neutral_adapts() {
  Sim s;
  s.run(5.0f, 35, 0, 0);                       // settle at a typical glance
  s.run(0.6f, 35, 0, 8);                       // roll 8 degrees
  float peak = s.ox;
  TEST_ASSERT_TRUE(peak > 0.20f);              // ~8/22 of full scale, minus a bit of adaptation
  TEST_ASSERT_TRUE(peak < 0.45f);
  s.run(1.0f, 35, 0, 8);
  float mid = s.ox;
  TEST_ASSERT_TRUE(mid < peak);                // drifting back...
  TEST_ASSERT_TRUE(mid > 0.05f);               // ...slowly (seconds, not instantly)
  s.run(12.0f, 35, 0, 8);
  TEST_ASSERT_FLOAT_WITHIN(0.02f, 0.0f, s.ox); // recentred at the new pose
}

// Direction conventions (screen frame, output x right / y down).
static void test_directions() {
  {
    Sim s;
    s.run(3.0f, 30, 0, 0);
    s.run(0.4f, 30, 0, 10);      // right edge up: eye moves right -> far layers right
    TEST_ASSERT_TRUE(s.ox > 0.1f);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.0f, s.oy);
  }
  {
    Sim s;
    s.run(3.0f, 30, 0, 0);
    s.run(0.4f, 30, 10, 0);      // more upright: eye moves toward the top -> far layers up
    TEST_ASSERT_TRUE(s.oy < -0.1f);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.0f, s.ox);
  }
  {
    Sim s;                        // inverted axes flip the outputs
    TiltConfig c; c.invertX = true; c.invertY = true;
    s.f.configure(c);
    s.run(3.0f, 30, 0, 0);
    s.run(0.4f, 30, 10, 10);
    TEST_ASSERT_TRUE(s.ox < -0.1f);
    TEST_ASSERT_TRUE(s.oy > 0.1f);
  }
}

// Large tilts saturate smoothly at 1.
static void test_saturation() {
  Sim s;
  s.run(3.0f, 30, 0, 0);
  s.run(0.5f, 30, 0, 70);
  float m = sqrtf(s.ox * s.ox + s.oy * s.oy);
  TEST_ASSERT_TRUE(m <= 1.0f);
  TEST_ASSERT_TRUE(m > 0.8f);
}

// Spring: step response is monotonic (no overshoot) and settles.
static void test_spring_step_no_overshoot() {
  const float w = 11.0f;
  float x = 0, v = 0;
  float prev = 0;
  float t = 0;
  bool reached99 = false;
  for (int i = 0; i < 400; i++) {
    float dt = 0.008f + 0.03f * (float)((i * 7919) % 13) / 13.0f;   // jittery frames
    springStep(x, v, 1.0f, w, dt);
    t += dt;
    TEST_ASSERT_TRUE(x <= 1.0f + 1e-5f);
    TEST_ASSERT_TRUE(x >= prev - 1e-6f);
    prev = x;
    if (!reached99 && x >= 0.99f) {
      reached99 = true;
      TEST_ASSERT_TRUE(t < 0.75f);       // ~6.6/w = 0.6 s
    }
  }
  TEST_ASSERT_TRUE(reached99);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, x);
}

// Spring: following a ramp and then stopping must not overshoot either.
static void test_spring_ramp_stop_no_overshoot() {
  float x = 0, v = 0, target = 0;
  for (int i = 0; i < 40; i++) {           // ramp 0 -> 1 over ~0.66 s
    target += 1.0f / 40.0f;
    springStep(x, v, target, 11.0f, 1.0f / 60.0f);
  }
  float maxX = x;
  for (int i = 0; i < 300; i++) {
    springStep(x, v, 1.0f, 11.0f, 1.0f / 60.0f);
    if (x > maxX) maxX = x;
  }
  TEST_ASSERT_TRUE(maxX <= 1.0f + 1e-5f);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, x);
}

// Spring: any frame time is stable (exact integration), even a huge one.
static void test_spring_large_dt_stable() {
  float x = 0, v = 0;
  springStep(x, v, 1.0f, 11.0f, 5.0f);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, x);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, v);
}

// Dead-zone: sensor-level noise around a still pose never moves the target.
static void test_deadzone_holds_still() {
  Sim s;
  s.run(4.0f, 35, 0, 0);
  float tx = s.f.targetX(), ty = s.f.targetY();
  // ~2 mg of noise is far beyond the MMA8451's real noise at this rate.
  s.run(6.0f, 35, 0, 0, 0.002f, 7);
  TEST_ASSERT_FLOAT_WITHIN(0.004f, tx, s.f.targetX());
  TEST_ASSERT_FLOAT_WITHIN(0.004f, ty, s.f.targetY());
  TEST_ASSERT_TRUE(s.f.settled());
}

// Dead-zone: a real movement still gets through (it's backlash, not a gate).
static void test_deadzone_passes_motion() {
  Sim s;
  s.run(4.0f, 35, 0, 0);
  s.run(0.3f, 35, 0, 3);       // a deliberate 3 degree roll
  TEST_ASSERT_TRUE(s.f.targetX() > 0.05f);
}

// Shake: 2 g arm jolts barely move the scene compared with a real tilt.
static void test_shake_rejection() {
  Sim s;
  s.run(4.0f, 35, 0, 0);
  uint32_t t = s.t;
  float maxAbs = 0;
  for (int i = 0; i < 20; i++) {             // ~0.45 s of +-1.5 g sideways jolts
    t += 22;
    float x, y, z;
    poseUp(35, 0, 0, x, y, z);
    x += (i & 1) ? 1.5f : -1.5f;
    int16_t ax, ay, az;
    upToRaw(x, y, z, ax, ay, az);
    s.f.addSample(ax, ay, az, t);
    float ox, oy;
    s.f.step(t, ox, oy);
    if (fabsf(ox) > maxAbs) maxAbs = fabsf(ox);
  }
  TEST_ASSERT_TRUE(maxAbs < 0.15f);
}

// Starting the face mid-gesture: the first sample is the neutral pose, so
// the scene starts centred, and the fast settle window absorbs the rest of
// the wrist motion instead of leaving the scene off-centre for seconds.
static void test_wake_mid_motion() {
  Sim s;
  s.run(0.3f, 10, 0, 0);
  s.run(0.5f, 40, 0, 0);       // still raising the wrist
  s.run(1.5f, 40, 0, 0);
  TEST_ASSERT_FLOAT_WITHIN(0.08f, 0.0f, s.oy);
}

// Ring buffer used to carry samples from taskIO.
static void test_ring() {
  SpscRing<8> r;
  ImuSample s;
  TEST_ASSERT_FALSE(r.pop(s));
  for (int i = 0; i < 10; i++) {
    ImuSample in; in.x = (int16_t)i; in.y = 0; in.z = 0; in.tMs = (uint32_t)i;
    r.push(in);
  }
  TEST_ASSERT_EQUAL_UINT32(8, r.size());
  TEST_ASSERT_EQUAL_UINT32(2, r.dropped());
  for (int i = 0; i < 8; i++) {
    TEST_ASSERT_TRUE(r.pop(s));
    TEST_ASSERT_EQUAL_INT16(i, s.x);       // oldest first, newest dropped
  }
  TEST_ASSERT_FALSE(r.pop(s));
  ImuSample in; in.x = 42; in.y = 0; in.z = 0; in.tMs = 0;
  r.push(in);
  r.clear();
  TEST_ASSERT_FALSE(r.pop(s));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_any_resting_pose_reads_centred);
  RUN_TEST(test_change_then_neutral_adapts);
  RUN_TEST(test_directions);
  RUN_TEST(test_saturation);
  RUN_TEST(test_spring_step_no_overshoot);
  RUN_TEST(test_spring_ramp_stop_no_overshoot);
  RUN_TEST(test_spring_large_dt_stable);
  RUN_TEST(test_deadzone_holds_still);
  RUN_TEST(test_deadzone_passes_motion);
  RUN_TEST(test_shake_rejection);
  RUN_TEST(test_wake_mid_motion);
  RUN_TEST(test_ring);
  return UNITY_END();
}
