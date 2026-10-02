// Haptic driver — DRV2603 motor driver controlling an LRA/ERM.
//
// EN pin gates power; PWM on the IN pin sets vibration intensity.
//
// Contract:
//   * hapticBegin() must run before any hapticBuzz call. Spawns a small
//     dedicated FreeRTOS task that owns motor timing — `hapticBuzz` itself
//     is fire-and-forget so it can be called from any task without blocking.
//   * Requests are queued (depth 4). If the motor is busy and the queue is
//     full, the new request is silently dropped — UI feedback is not worth
//     backpressure.
//   * The strength multiplier (0..100 %) is applied to every queued buzz.
//     0 silences the motor entirely. Set from the Settings → Haptics page;
//     mirrored here so the driver doesn't pull in model.h.
//   * Pixel Pet addition: hapticPattern() queues a short on/off sequence as
//     one item (a non-blocking sequencer), and hapticBusy() lets the
//     background-sleep loop wait for the motor to finish before it sleeps.
#pragma once
#include <stdint.h>

#define HAPTIC_MAX_STEPS 8

struct HapticStep {
  uint8_t  intensity;   // 0..255 before the strength scaler
  uint16_t onMs;        // motor on
  uint16_t offMs;       // pause after
};

void hapticBegin();
void hapticBuzz(uint8_t intensity, uint16_t duration_ms);
void hapticPattern(const HapticStep *steps, uint8_t n);
bool hapticBusy();
void hapticSetStrengthPct(uint8_t pct);
