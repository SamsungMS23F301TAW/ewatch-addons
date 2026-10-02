// Shake Oracle: the watch-side view. A thin adapter between BaseOS (events,
// IMU stream, canvas, haptics, NVS) and the portable OracleApp in core/.
#pragma once
#include <Arduino_GFX_Library.h>

#include "view.h"

class OracleView : public View {
 public:
  void     onEnter() override;
  void     onExit() override;
  void     render() override;
  void     onEvent(const Event &e) override;
  uint16_t framePeriodMs() const override;
  uint16_t minAwakeSec() const override;
};

// Serial console hook, called from loop() in main.cpp with one input line.
// Returns true if the line was an "oracle ..." command.
bool oracleConsoleCommand(const char *line);

// Launcher tile icon: a tiny oracle ball (see AppEntry::icon in view.cpp).
void oracleDrawTileIcon(Arduino_GFX *g, int16_t cx, int16_t cy, int16_t r, uint16_t bg);
