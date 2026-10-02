#include "oracle_app.h"

#include <math.h>

#include "oracle_config.h"

namespace oracle {

// ------------------------------------------------------------ haptics

void HapticPlanner::reset() {
  stepN_ = 0;
  busyUntil_ = 0;
  rng_ = 0x9E3779B9u;
}

int HapticPlanner::plan(uint32_t nowMs, float rumble, bool thunk, bool gold, bool tick,
                        Buzz *out) {
  int n = 0;
  if (thunk) {
    // A firm knock and a softer rebound, "thunk-tk". The golden answer adds
    // a little two-note twinkle after it.
    stepN_ = 0;
    steps_[stepN_++] = {nowMs, {kThunkHit, kThunkHitMs}};
    steps_[stepN_++] = {nowMs + kThunkHitMs + 30, {kThunkTail, kThunkTailMs}};
    if (gold) {
      steps_[stepN_++] = {nowMs + 190, {150, 22}};
      steps_[stepN_++] = {nowMs + 290, {200, 28}};
    }
    busyUntil_ = nowMs;   // the knock goes out right away
  }
  if (stepN_ > 0) {
    // Post the next scheduled step once it is due and the motor is free.
    if ((int32_t)(nowMs - steps_[0].due) >= 0 && !busy(nowMs) && n < kMax) {
      out[n++] = steps_[0].b;
      busyUntil_ = nowMs + steps_[0].b.ms + 6;
      for (int i = 1; i < stepN_; i++) steps_[i - 1] = steps_[i];
      stepN_--;
    }
    return n;
  }
  if (busy(nowMs)) return 0;
  if (rumble > 0.04f) {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    int jitter = (int)(rng_ % 29) - 14;
    float r = rumble > 1.f ? 1.f : rumble;
    int level = kRumbleMin + (int)((kRumbleMax - kRumbleMin) * r) + jitter;
    if (level < 1) level = 1;
    if (level > 255) level = 255;
    out[n++] = {(uint8_t)level, kRumblePulseMs};
    busyUntil_ = nowMs + kRumblePulseMs + kRumbleGapMs;
    return n;
  }
  if (tick) {
    out[n++] = {70, 22};
    busyUntil_ = nowMs + 22 + 8;
  }
  return n;
}

// ------------------------------------------------------------ app

bool OracleApp::begin(const GFXfont *font, Renderer::AllocFn alloc, uint64_t seed) {
  if (ready_) return true;
  if (!font || !renderer_.begin(font, alloc)) return false;
  picker_.seed(seed);
  scene_.reset((uint32_t)(seed ^ (seed >> 32)));
  renderer_.setPrompt(packAt(pack_).prompt);
  ready_ = true;
  return true;
}

void OracleApp::setPack(int p) {
  int n = packCount();
  pack_ = ((p % n) + n) % n;
  if (ready_) renderer_.setPrompt(packAt(pack_).prompt);
}

void OracleApp::enter(uint16_t *fb, uint32_t nowMs) {
  if (!ready_) return;
  scene_.reset(picker_.rng().next());
  det_.reset();
  haptics_.reset();
  started_ = stopped_ = tapped_ = false;
  swipe_ = 0;
  tick_ = false;
  simShaking_ = false;
  renderer_.setPrompt(packAt(pack_).prompt);
  renderer_.drawBall(fb, packAt(pack_).name, pack_, packCount());
  renderer_.drawWindow(fb, scene_.frame());
  lastMs_ = nowMs;
  haveLast_ = true;
}

void OracleApp::imuSample(uint32_t ms, int16_t ax, int16_t ay, int16_t az) {
  ShakeDetector::Event ev =
      det_.feed(ms, ax / kCountsPerG, ay / kCountsPerG, az / kCountsPerG);
  if (ev == ShakeDetector::Event::Started) started_ = true;
  else if (ev == ShakeDetector::Event::Stopped) stopped_ = true;
}

void OracleApp::imuIdle(uint32_t ms) {
  if (det_.idle(ms) == ShakeDetector::Event::Stopped) stopped_ = true;
}

void OracleApp::tap(float x, float y) {
  tapped_ = true;
  tapX_ = x;
  tapY_ = y;
}

void OracleApp::swipePack(int dir) { swipe_ += dir; }

void OracleApp::simulateShake(bool start) {
  if (start) { started_ = true; simShaking_ = true; }
  else       { stopped_ = true; simShaking_ = false; }
}

void OracleApp::forceNextAnswer(const char *text, bool golden) {
  forced_ = text;
  forcedGold_ = golden;
}

void OracleApp::exportState(int &lastPack, int &lastIndex, uint64_t &rng) const {
  lastPack = picker_.lastPack();
  lastIndex = picker_.lastIndex();
  rng = picker_.rngState();
}

void OracleApp::importState(int lastPack, int lastIndex, uint64_t rng) {
  picker_.setLast(lastPack, lastIndex);
  if (rng) picker_.rng().setState(rng);
}

uint16_t OracleApp::framePeriodMs() const {
  return scene_.busy() || det_.shaking() ? kFramePeriodBusy : kFramePeriodCalm;
}

void OracleApp::startAnswer() {
  const char *text;
  bool gold;
  if (forced_) {
    text = forced_;
    gold = forcedGold_;
    forced_ = nullptr;
  } else {
    Pick p = picker_.next(pack_);
    text = p.text;
    gold = p.golden;
  }
  answer_ = text;
  answerGold_ = gold;
  renderer_.setDieText(text);
  scene_.shakeStopped(gold);
}

void OracleApp::frame(uint16_t *fb, uint32_t nowMs, FrameOut &out) {
  out = FrameOut();
  if (!ready_) return;
  float dt = haveLast_ ? (uint32_t)(nowMs - lastMs_) * 0.001f : 0.03f;
  lastMs_ = nowMs;
  haveLast_ = true;

  if (swipe_ != 0) {
    int n = packCount();
    pack_ = (((pack_ + swipe_) % n) + n) % n;
    swipe_ = 0;
    renderer_.setPrompt(packAt(pack_).prompt);
    renderer_.setPackLabel(fb, packAt(pack_).name, pack_, n, out.labelY0, out.labelRows);
    out.labelFlush = true;
    out.packChanged = true;
    out.activity = true;
    scene_.packChanged();
    tick_ = true;
  }
  if (started_) {
    started_ = false;
    scene_.shakeStarted();
    picker_.mix(nowMs);
    out.activity = true;
  }
  if (stopped_) {
    stopped_ = false;
    if (scene_.phase() == Phase::Churning) startAnswer();
    out.activity = true;
  }
  if (tapped_) {
    tapped_ = false;
    bool clears = scene_.answerVisible();
    scene_.tap(tapX_, tapY_);
    if (clears) tick_ = true;
    out.activity = true;
  }
  bool shaking = det_.shaking() || simShaking_;
  if (shaking) out.activity = true;

  SceneInput in;
  in.dt = dt;
  in.shaking = shaking;
  in.intensity = simShaking_ ? 0.7f : det_.intensity();
  in.dynX = kTiltSignX * det_.dynX();
  in.dynY = kTiltSignY * det_.dynY();
  in.dynMag = simShaking_ ? 0.f
                          : sqrtf(det_.dynX() * det_.dynX() + det_.dynY() * det_.dynY() +
                                  det_.dynZ() * det_.dynZ());
  in.upX = kTiltSignX * det_.gravX();
  in.upY = kTiltSignY * det_.gravY();
  in.imuOk = imuOk_;
  SceneCues cues = scene_.update(in);
  if (cues.landed) {
    out.landed = true;
    out.activity = true;
  }
  out.buzzCount = haptics_.plan(nowMs, cues.rumble, cues.landed, cues.goldLanded, tick_, out.buzz);
  tick_ = false;
  renderer_.drawWindow(fb, scene_.frame());
}

}  // namespace oracle
