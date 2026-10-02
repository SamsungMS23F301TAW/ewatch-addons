// Settings -> Face: Dayprint's own options (saved immediately to NVS
// namespace "generative-face"):
//   Watch face        Dayprint / Classic (the stock BaseOS face; pick its
//                     style under Settings -> Font)
//   Steps when dark   background step counting on / off
//   Daily goal        off, 2,000 .. 30,000
//   Light of day      tint the art with the time of day
//   Drifting light    a slow light that drifts over the face while it's on
#pragma once
#include "view.h"

class FaceSettingsView : public View {
 public:
  void onEnter() override;
  void render() override;
  void onEvent(const Event &e) override;

 private:
  void drawRows();
  bool dirty_ = true;
  bool first_ = true;
};
