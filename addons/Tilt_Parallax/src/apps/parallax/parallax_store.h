// Tilt Parallax — settings, persisted in the addon's own NVS namespace
// ("tilt-parallax"); the BaseOS "ewatch" namespace is never touched.
//
// Access rules: loaded once in setup() before the tasks start, then read and
// written only from the render task (face, settings page, serial commands
// are forwarded there), so no lock is needed.
#pragma once
#include <stdint.h>

struct ParallaxSettings {
  uint8_t scene = 0;        // index into tp::sceneCount()
  uint8_t depth = 2;        // strength preset (tp::kStrengthScale)
  bool    h12 = false;      // 12-hour clock
  bool    ambient = true;   // drifting clouds, twinkling stars
  bool    classicFace = false;  // show the stock BaseOS face instead
  bool    raiseToWake = false;  // experimental (TP_RAISE_TO_WAKE builds only)
};

ParallaxSettings &parallaxSettings();
void parallaxSettingsLoad();     // safe before the tasks start
// Writes only keys whose values changed since the last save. A flash write
// briefly stalls both cores (flash and PSRAM share the cache), so callers
// don't save on every tap: the face, the settings page and the sleep /
// power-off paths call it at quiet moments.
void parallaxSettingsSave();
