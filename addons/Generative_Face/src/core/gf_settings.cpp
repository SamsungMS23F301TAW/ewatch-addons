#include "gf_settings.h"
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include "steps_hw.h"

#ifndef GF_BG_STEPS
#define GF_BG_STEPS 1
#endif

static GfSettings sSet;
static portMUX_TYPE sMux = portMUX_INITIALIZER_UNLOCKED;

void gfSettingsLoad() {
  Preferences p;
  GfSettings s;
  if (p.begin("generative-face", true)) {
    s.classicFace = p.getBool("classic", s.classicFace);
    s.bgSteps     = p.getBool("bgSteps", s.bgSteps);
    s.goal        = p.getUShort("goal", s.goal);
    s.ambient     = p.getBool("ambient", s.ambient);
    s.animate     = p.getBool("animate", s.animate);
    p.end();
  }
  if (s.goal > 40000) s.goal = 8000;
  portENTER_CRITICAL(&sMux);
  sSet = s;
  portEXIT_CRITICAL(&sMux);
  stepsSetGoal(s.goal);
}

void gfSettingsSave(const GfSettings &s) {
  portENTER_CRITICAL(&sMux);
  sSet = s;
  portEXIT_CRITICAL(&sMux);
  stepsSetGoal(s.goal);
  Preferences p;
  if (!p.begin("generative-face", false)) return;
  p.putBool("classic", s.classicFace);
  p.putBool("bgSteps", s.bgSteps);
  p.putUShort("goal", s.goal);
  p.putBool("ambient", s.ambient);
  p.putBool("animate", s.animate);
  p.end();
}

GfSettings gfSettings() {
  portENTER_CRITICAL(&sMux);
  GfSettings s = sSet;
  portEXIT_CRITICAL(&sMux);
  return s;
}

bool gfBackgroundSteps() {
#if GF_BG_STEPS
  return gfSettings().bgSteps;
#else
  return false;
#endif
}
