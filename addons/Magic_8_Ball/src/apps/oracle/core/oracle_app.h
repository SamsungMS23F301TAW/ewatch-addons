// Shake Oracle application logic, independent of Arduino.
//
// OracleApp owns the shake detector, the answer picker, the scene and the
// renderer, and plans haptics. The watch view (oracle_view.cpp) only feeds it
// IMU samples and touches, calls frame() and pushes pixels and buzzes out.
// The host preview tool and the native tests drive this same class.
#pragma once
#include <stdint.h>

#include "answers.h"
#include "oracle_render.h"
#include "oracle_scene.h"
#include "shake_detector.h"

namespace oracle {

struct Buzz {
  uint8_t  intensity;   // 0..255 (0 = a pause)
  uint16_t ms;
};

// Turns scene cues into hapticBuzz() requests. Pauses are timed here rather
// than sent as zero-intensity buzzes (how the DRV2603 treats 0 % PWM is
// unverified), and it never has more than one request waiting in the
// driver's 4-deep queue.
class HapticPlanner {
 public:
  static constexpr int kMax = 4;
  void reset();
  // Fills `out` with buzzes to post now. Returns the count.
  int  plan(uint32_t nowMs, float rumble, bool thunk, bool gold, bool tick, Buzz *out);
  bool busy(uint32_t nowMs) const { return (int32_t)(busyUntil_ - nowMs) > 0; }
  bool pending() const { return stepN_ > 0; }

 private:
  struct Step {
    uint32_t due;
    Buzz     b;
  };
  static constexpr int kSteps = 6;
  Step     steps_[kSteps];
  int      stepN_ = 0;
  uint32_t busyUntil_ = 0;
  uint32_t rng_ = 0x9E3779B9u;
};

struct FrameOut {
  Buzz buzz[HapticPlanner::kMax];
  int  buzzCount = 0;
  bool fullFlush = false;     // push the whole screen this frame
  bool labelFlush = false;    // push rows [labelY0, labelY0 + labelRows)
  int  labelY0 = 0, labelRows = 0;
  bool activity = false;      // the user did something: keep the screen awake
  bool packChanged = false;   // persist the pack choice
  bool landed = false;        // an answer just hit the glass
};

class OracleApp {
 public:
  // Allocates the renderer caches. `seed` should come from a hardware RNG.
  bool begin(const GFXfont *font, Renderer::AllocFn alloc, uint64_t seed);
  bool ready() const { return ready_; }

  // Paint the whole screen into fb and restart the idle state.
  void enter(uint16_t *fb, uint32_t nowMs);

  // Inputs. Samples are raw MMA8451 counts (4096 per g at +-2 g).
  void imuSample(uint32_t ms, int16_t ax, int16_t ay, int16_t az);
  void imuIdle(uint32_t ms);
  void setImuOk(bool ok) { imuOk_ = ok; }
  void tap(float x, float y);
  void swipePack(int dir);           // +1 next pack, -1 previous (or +-n)
  void stirEntropy(uint32_t e) { picker_.mix(e); }

  // Advance and draw one frame into fb (window rows, plus the label strip
  // or the whole screen when `out` says so).
  void frame(uint16_t *fb, uint32_t nowMs, FrameOut &out);
  uint16_t framePeriodMs() const;

  int  pack() const { return pack_; }
  void setPack(int p);               // before enter(); no animation

  // Last pick and RNG, so "never twice in a row" survives deep sleep.
  void exportState(int &lastPack, int &lastIndex, uint64_t &rng) const;
  void importState(int lastPack, int lastIndex, uint64_t rng);

  // Debug and previews: the next answer will be exactly this text.
  void forceNextAnswer(const char *text, bool golden);
  // Debug: behave as if a shake started / stopped right now.
  void simulateShake(bool start);

  const Scene &scene() const { return scene_; }
  const ShakeDetector &detector() const { return det_; }
  const char *answer() const { return answer_; }
  bool answerGolden() const { return answerGold_; }

 private:
  bool          ready_ = false;
  Renderer      renderer_;
  ShakeDetector det_;
  AnswerPicker  picker_;
  Scene         scene_;
  HapticPlanner haptics_;

  int      pack_ = 0;
  bool     imuOk_ = false;
  bool     started_ = false, stopped_ = false;   // detector edges this frame
  bool     tapped_ = false;
  float    tapX_ = 0, tapY_ = 0;
  int      swipe_ = 0;
  bool     tick_ = false;
  bool     simShaking_ = false;
  uint32_t lastMs_ = 0;
  bool     haveLast_ = false;
  const char *answer_ = "";
  bool     answerGold_ = false;
  const char *forced_ = nullptr;
  bool     forcedGold_ = false;

  void startAnswer();
};

}  // namespace oracle
