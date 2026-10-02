#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "pins.h"
#include "haptic.h"

static const uint32_t HAPTIC_FREQ_HZ  = 10000;
static const uint8_t  HAPTIC_RES_BITS = 8;
static const uint8_t  HAPTIC_LEDC_CH  = 1;

// One queue item is a whole pattern (a plain buzz is a 1-step pattern), so
// gaps inside a pattern are honoured and patterns never interleave.
struct BuzzReq {
  uint8_t    n;
  HapticStep steps[HAPTIC_MAX_STEPS];
};
static QueueHandle_t buzzQueue = nullptr;
static volatile bool s_running = false;

// Strength scale [0..100]. 0 silences all haptic feedback, 100 = pass-through.
// Reads on the I/O hot path, writes from settings tasks — uint8_t writes are
// atomic on Xtensa so no lock needed.
static volatile uint8_t g_strengthPct = 100;

static void hapticTask(void *) {
  BuzzReq r;
  for (;;) {
    if (xQueueReceive(buzzQueue, &r, portMAX_DELAY) != pdPASS) continue;
    s_running = true;
    for (uint8_t i = 0; i < r.n && i < HAPTIC_MAX_STEPS; i++) {
      const HapticStep &s = r.steps[i];
      if (s.onMs) {
        digitalWrite(PIN_MOTOR_EN, HIGH);
        ledcWrite(HAPTIC_LEDC_CH, s.intensity);
        vTaskDelay(pdMS_TO_TICKS(s.onMs));
        ledcWrite(HAPTIC_LEDC_CH, 0);
        digitalWrite(PIN_MOTOR_EN, LOW);
      }
      if (s.offMs) vTaskDelay(pdMS_TO_TICKS(s.offMs));
    }
    s_running = false;
  }
}

void hapticBegin() {
  pinMode(PIN_MOTOR_EN, OUTPUT);
  digitalWrite(PIN_MOTOR_EN, LOW);
  ledcSetup(HAPTIC_LEDC_CH, HAPTIC_FREQ_HZ, HAPTIC_RES_BITS);
  ledcAttachPin(PIN_MOTOR_PWM, HAPTIC_LEDC_CH);
  ledcWrite(HAPTIC_LEDC_CH, 0);

  buzzQueue = xQueueCreate(4, sizeof(BuzzReq));
  xTaskCreatePinnedToCore(hapticTask, "haptic", 2048, nullptr, 2, nullptr, 1);
}

static uint8_t scaled(uint8_t intensity, uint8_t pct) {
  uint16_t v = ((uint16_t)intensity * pct) / 100;
  return (uint8_t)(v > 255 ? 255 : v);
}

// Non-blocking. Just posts the request; the haptic task owns the timing.
// If the queue is full (motor already busy with several queued buzzes), the
// new request is dropped — UI feedback is not worth backpressure. Intensity
// is scaled by the user-configured strength percentage before queueing.
void hapticBuzz(uint8_t intensity, uint16_t duration_ms) {
  HapticStep s = {intensity, duration_ms, 0};
  hapticPattern(&s, 1);
}

void hapticPattern(const HapticStep *steps, uint8_t n) {
  if (!buzzQueue || !steps || n == 0) return;
  uint8_t pct = g_strengthPct;
  if (pct == 0) return;            // silent mode — drop the buzz entirely
  BuzzReq r;
  r.n = n > HAPTIC_MAX_STEPS ? HAPTIC_MAX_STEPS : n;
  for (uint8_t i = 0; i < r.n; i++) {
    r.steps[i] = steps[i];
    r.steps[i].intensity = scaled(steps[i].intensity, pct);
  }
  xQueueSend(buzzQueue, &r, 0);
}

bool hapticBusy() {
  if (s_running) return true;
  return buzzQueue && uxQueueMessagesWaiting(buzzQueue) > 0;
}

void hapticSetStrengthPct(uint8_t pct) {
  if (pct > 100) pct = 100;
  g_strengthPct = pct;
}
