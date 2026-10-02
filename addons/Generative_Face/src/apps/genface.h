// Dayprint's watch face (the default Screen::Watch).
//
// Today's artwork fills the screen; time, date and steps sit on top. The
// art job runs in ~22 ms slices per render() call, so a fresh piece blooms
// in over a moment after a reboot while the clock shows immediately, and a
// new step bucket just adds strokes on top. Gestures:
//   swipe left or up   launcher        swipe down   Gallery
//   tap                art view (museum label) / back to the clock
//   long press         Face settings
#pragma once
#include "view.h"

class GenFaceView : public View {
 public:
  void onEnter() override;
  void onExit() override {}
  void render() override;
  void onEvent(const Event &e) override;
  void onWake() override;

 private:
  void frame(bool forceCompose);
  void fallback();

  uint8_t  mode_ = 0;              // gf::kModeClock / kModeArt
  uint8_t  artTones_ = 0;          // caption tone for the art view
  bool     artTonesStale_ = true;
  bool     needCompose_ = true;
  uint32_t lastComposeMs_ = 0;
  uint32_t wakeMs_ = 0;
  uint32_t enterMs_ = 0;
  // Tap detection.
  bool     pressActive_ = false, pressMoved_ = false;
  uint16_t pressX_ = 0, pressY_ = 0;
  uint32_t pressMs_ = 0;
};
