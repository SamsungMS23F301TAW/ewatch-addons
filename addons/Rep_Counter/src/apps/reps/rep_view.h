// Rep Counter: the app screen (BaseOS View).
//
// Glues the pure-C++ pieces to the watch: drains taskIO's accelerometer
// stream into the detector, runs the session, plays haptics, saves to NVS,
// and renders frames (rep_ui) into the shared frame canvas, pushing only the
// rows that changed.
#pragma once
#include "view.h"

class RepView : public View {
public:
  void onEnter() override;
  void onExit() override;
  void render() override;
  void onEvent(const Event &e) override;
};

// For taskRender's blockSleep chain: a workout is running on this screen, or
// a serial REC session is streaming.
bool repsBlockSleep();
