// Dayprint's own settings, in NVS namespace "generative-face" (the shared
// "ewatch" namespace is left untouched so theme, WiFi and haptics carry
// across addon installs).
#pragma once
#include <stdint.h>

struct GfSettings {
  bool     classicFace = false;   // show the stock BaseOS face instead
  bool     bgSteps = true;        // count steps with the screen off
  uint16_t goal = 8000;           // daily goal (0 = off)
  bool     ambient = true;        // tint the art with the time of day
  bool     animate = true;        // slow drifting light while the face is on
};

void       gfSettingsLoad();
void       gfSettingsSave(const GfSettings &s);
GfSettings gfSettings();

// Background counting is on only when compiled in (GF_BG_STEPS) and the
// user hasn't switched it off.
bool       gfBackgroundSteps();
