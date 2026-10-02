#include "haptic_pattern.h"
#include <Arduino.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include "haptic.h"

// Mate near: a soft tick when the comets appear, then "da-da-DUM" as they
// meet and bloom (t = 1000 ms in the hello animation).
const HapticStep kHapticMateNear[] = {
  {0,    110, 35},
  {1000, 210, 60},
  {1130, 210, 60},
  {1280, 255, 210},
};
const uint8_t kHapticMateNearN = sizeof(kHapticMateNear) / sizeof(kHapticMateNear[0]);

// Celebrate: three quick taps and a long buzz with the confetti burst.
const HapticStep kHapticCelebrate[] = {
  {0,   255, 45},
  {110, 255, 45},
  {220, 255, 45},
  {380, 255, 230},
};
const uint8_t kHapticCelebrateN = sizeof(kHapticCelebrate) / sizeof(kHapticCelebrate[0]);

static portMUX_TYPE sMux = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t sTimer = nullptr;   // created once, re-armed per step
static HapticStep sSteps[8];
static uint8_t sN = 0, sNext = 0;
static uint32_t sT0 = 0;

static void armNext();

static void onTimer(void *) {
  HapticStep step;
  portENTER_CRITICAL(&sMux);
  if (sNext >= sN) { portEXIT_CRITICAL(&sMux); return; }
  step = sSteps[sNext++];
  portEXIT_CRITICAL(&sMux);
  hapticBuzz(step.intensity, step.durMs);
  armNext();
}

static void armNext() {
  uint32_t now = millis();
  uint32_t due;
  portENTER_CRITICAL(&sMux);
  // Skip steps whose time has passed (late join), but keep the last one.
  while (sNext + 1 < sN && (int32_t)(sT0 + sSteps[sNext].atMs - now) < -40) ++sNext;
  if (sNext >= sN) { portEXIT_CRITICAL(&sMux); return; }
  int32_t d = (int32_t)(sT0 + sSteps[sNext].atMs - now);
  due = d > 0 ? (uint32_t)d : 0;
  portEXIT_CRITICAL(&sMux);

  if (!sTimer) {
    esp_timer_create_args_t a = {};
    a.callback = &onTimer;
    a.dispatch_method = ESP_TIMER_TASK;
    a.name = "hpat";
    if (esp_timer_create(&a, &sTimer) != ESP_OK) { sTimer = nullptr; return; }
  }
  esp_timer_stop(sTimer);                     // harmless if not armed
  esp_timer_start_once(sTimer, (uint64_t)due * 1000ULL + 1);
}

void hapticPatternPlay(const HapticStep *steps, uint8_t n, uint32_t t0Ms) {
  if (!steps || !n) return;
  if (n > 8) n = 8;
  portENTER_CRITICAL(&sMux);
  for (uint8_t i = 0; i < n; ++i) sSteps[i] = steps[i];
  sN = n;
  sNext = 0;
  sT0 = t0Ms;
  portEXIT_CRITICAL(&sMux);
  armNext();
}

void hapticPatternStop() {
  portENTER_CRITICAL(&sMux);
  sN = 0;
  portEXIT_CRITICAL(&sMux);
  if (sTimer) esp_timer_stop(sTimer);
}
