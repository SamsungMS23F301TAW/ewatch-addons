// Tilt Parallax — the watch face view (default Screen::Watch).
//
//   tap          next scene (layers drop away, the new landscape rises in)
//   long-press   Parallax settings (also swipe down)
//   swipe up / left   app launcher
//
// Frames are composed in 32-row strips in internal SRAM and pushed straight
// to the panel, but only for rows that actually changed. When nothing moves
// the face stops drawing entirely.
#pragma once
#include "view.h"
#include "tp_engine.h"
#include "tp_tilt.h"

class ParallaxFaceView : public View {
public:
  void onEnter() override;
  void onExit() override;
  void render() override;
  void onEvent(const Event &e) override;
  uint16_t desiredFrameMs() const override;

private:
  bool ensureRenderer();
  void adoptScene(tp::Scene *s, bool animateIn, uint32_t now);
  void startSceneChange(int targetId, uint32_t now);
  void updateTransition(uint32_t now);
  void present(uint32_t now);
  void runCommand(const char *line, uint32_t now);
  void bench();
  void drawFallback();
  void openSettings();

  tp::FaceRenderer fr_;
  bool       frReady_ = false;
  bool       frFailed_ = false;
  tp::Scene *scene_ = nullptr;
  tp::TiltFilter tilt_;
  uint16_t  *strip_ = nullptr;
  bool       firstFramePending_ = true;
  uint32_t   enterMs_ = 0;

  enum Phase : uint8_t { PH_IDLE, PH_OUT, PH_WAIT, PH_IN };
  Phase      phase_ = PH_IDLE;
  uint32_t   phaseStart_ = 0;
  tp::Scene *parked_ = nullptr;     // new scene that arrived mid-"out" animation
  int        targetScene_ = -1;
  int        outFrom_[tp::Scene::kMaxLayers] = { 0 };   // offsets when "out" began
  uint32_t   lastGenReqMs_ = 0;     // retry backoff if a build failed (OOM)

  bool       pressActive_ = false, pressMoved_ = false, longFired_ = false;
  uint16_t   pressX_ = 0, pressY_ = 0;
  uint32_t   pressMs_ = 0;

  bool       moving_ = false;       // spring not at rest
  int        shownBattery_ = -1;    // battery % on screen (with hysteresis)
  uint32_t   batteryShownMs_ = 0;
  uint8_t    lastSec_ = 255;
  uint32_t   secAnchorMs_ = 0;

  // debug overrides (serial)
  int        timeOverride_ = -1;    // minutes since midnight, -1 = off
  bool       perfLog_ = true;

  // perf counters for the current 2 s window
  uint32_t   perfStart_ = 0, frames_ = 0, rows_ = 0, composeUs_ = 0, pushUs_ = 0;
};

// True when the user picked the stock BaseOS face in Parallax settings.
bool parallaxUseClassicFace();
