#include "mc_haptic.h"
#include <Arduino.h>
#include <esp_timer.h>
#include "haptic.h"

namespace mchaptic {

struct Step { uint8_t intensity; uint16_t onMs; uint16_t offMs; };

static const Step kAlert[] = {{255, 70, 95}, {255, 70, 95}, {255, 70, 230}, {255, 360, 0}};
static const Step kLeave[] = {{255, 280, 170}, {255, 280, 0}};
static const Step kStart[] = {{255, 480, 140}, {210, 90, 90}, {210, 90, 0}};

static esp_timer_handle_t gTimer = nullptr;
static portMUX_TYPE gMux = portMUX_INITIALIZER_UNLOCKED;
static const Step *gSteps = nullptr;
static int gN = 0, gIdx = 0, gRepeatsLeft = 0;
static uint16_t gGapMs = 0;
static volatile bool gActive = false;

static void onStep(void *) {
  Step s;
  uint32_t next;
  portENTER_CRITICAL(&gMux);
  if (!gActive || !gSteps) { portEXIT_CRITICAL(&gMux); return; }
  if (gIdx >= gN) {
    if (gRepeatsLeft <= 0) { gActive = false; portEXIT_CRITICAL(&gMux); return; }
    gRepeatsLeft--;
    gIdx = 0;
  }
  s = gSteps[gIdx++];
  next = (uint32_t)s.onMs + s.offMs;
  if (gIdx >= gN) next += gGapMs;
  portEXIT_CRITICAL(&gMux);
  hapticBuzz(s.intensity, s.onMs);
  esp_timer_start_once(gTimer, (uint64_t)next * 1000ULL);
}

void play(Pattern p, int repeats, uint16_t gapMs) {
  if (!gTimer) {
    esp_timer_create_args_t a = {};
    a.callback = &onStep;
    a.dispatch_method = ESP_TIMER_TASK;
    a.name = "mchaptic";
    if (esp_timer_create(&a, &gTimer) != ESP_OK) return;
  }
  esp_timer_stop(gTimer);
  portENTER_CRITICAL(&gMux);
  switch (p) {
    case Pattern::Alert: gSteps = kAlert; gN = sizeof(kAlert) / sizeof(kAlert[0]); break;
    case Pattern::Leave: gSteps = kLeave; gN = sizeof(kLeave) / sizeof(kLeave[0]); break;
    case Pattern::Start: gSteps = kStart; gN = sizeof(kStart) / sizeof(kStart[0]); break;
  }
  gIdx = 0;
  gRepeatsLeft = repeats > 0 ? repeats - 1 : 0;
  gGapMs = gapMs;
  gActive = true;
  portEXIT_CRITICAL(&gMux);
  esp_timer_start_once(gTimer, 1000);           // first step from the timer task
}

void stop() {
  portENTER_CRITICAL(&gMux);
  gActive = false;
  portEXIT_CRITICAL(&gMux);
  if (gTimer) esp_timer_stop(gTimer);
}

bool playing() { return gActive; }

}  // namespace mchaptic
