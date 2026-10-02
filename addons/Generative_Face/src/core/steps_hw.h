// Hardware glue for the step detector (steps.h): the MMA8451 FIFO, the live
// count in RTC memory, NVS checkpoints, the midnight rollover into the day
// collection, and the goal buzz.
//
// Accelerometer configuration while running (both screen-on and screen-off):
//   * 50 Hz, low-power oversampling, +-2 g, FIFO in circular mode with a
//     watermark of 30 samples (0.6 s) routed to INT2 (GPIO4): the light-sleep
//     loop wakes on it, drains the FIFO and sleeps again.
//   * TRANSIENT (high-pass motion, ~0.19 g) latched on INT1 (GPIO3): the
//     light-sleep loop uses it to wake from "still" mode, and deep sleep uses
//     it (at 12.5 Hz) to wake when you start moving again.
// Functions marked [bus] touch I2C and must run on the bus owner: taskIO, or
// the render task while taskIO is parked for screen-off.
#pragma once
#include <Arduino.h>
#include <stdint.h>

struct StepDrain {
  uint8_t  samples = 0;      // samples read this call
  bool     ok = true;        // false: an I2C transfer failed
  bool     overflow = false; // FIFO overran (samples were lost)
  uint32_t motion = 0;       // mean |delta| per sample, raw counts
  int16_t  x = 0, y = 0, z = 0;   // newest sample
};

void      stepsHwInit(uint8_t mmaAddr);       // [bus] configure + restore the count
StepDrain stepsHwDrain();                     // [bus] read the FIFO into the detector
void      stepsHwClearMotionLatch();          // [bus] release INT1
void      stepsHwPrepareDeepSleep();          // [bus] FIFO off, 12.5 Hz motion wake
bool      stepsHwPresent();

// Counting state (safe from any task).
uint32_t  stepsToday();
uint16_t  stepsDay();                         // day index the count belongs to
bool      stepsWalking();
uint32_t  stepsGoal();
void      stepsSetGoal(uint32_t goal);

// Date-driven housekeeping: rollover at midnight (filing yesterday into the
// collection), NVS checkpoints, the goal buzz. Call with the RTC's current
// date whenever it is known; `force` checkpoints immediately.
void      stepsService(uint16_t today, bool dateValid, bool force = false);
void      stepsCheckpointNow();               // before power-off / deep sleep

void      stepsDebugPrint(Print &out);
