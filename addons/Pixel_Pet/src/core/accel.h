// MMA8451Q driver for Pixel Pet: FIFO-paced sampling for the step detector,
// a TRANSIENT (high-passed) jolt interrupt for raise-to-wake, and a
// low-threshold motion interrupt for waking from deep sleep.
//
// BUS RULE: every function here talks I2C, so only the task that currently
// owns the bus may call it — taskIO while the screen is on, the
// background-sleep loop (render task, taskIO parked) while it is off, or
// setup() before the tasks start.
//
// Pins: FIFO watermark -> INT2 (GPIO4); TRANSIENT -> INT1 (GPIO3).
// Both push-pull, active high.
#pragma once
#include <stdint.h>

namespace accel {

constexpr float   kOdrHz     = 12.5f;  // fixed sample rate the detector assumes
constexpr uint8_t kWatermark = 20;     // samples: one FIFO wake every 1.6 s
constexpr int     kFifoSize  = 32;

bool    begin();          // probe 0x1C/0x1D; false if absent
bool    present();
uint8_t address();

// Normal sampling: 12.5 Hz, FIFO circular with watermark -> INT2. When
// `joltWake`, the TRANSIENT engine raises INT1 above `joltThs` (0.063 g/LSB)
// so a wrist flick can wake the screen from light sleep.
void configSampling(bool joltWake, uint8_t joltThs);

// Deep-sleep mode: FIFO off, TRANSIENT at a low threshold -> INT1, so any
// real movement (a walk starting) wakes the chip. INT1 is only meaningful
// once the chip is up and its high-pass filter has settled (see bgpower's
// enterDeepTier(), which waits for INT1 to stay quiet before sleeping).
void configMotionWake(uint8_t ths);

// Stock deep sleep (background steps off): BaseOS's own raise-to-wake jolt
// setup, unchanged (800 Hz, default high-pass, 5-sample debounce -> INT1).
void configJoltWake(uint8_t ths);

// Standby: no sampling, no interrupts (stock deep sleep without IMU wake).
void standby();

// Read everything in the FIFO (up to `maxSamples`). xyz receives 3 int16 per
// sample (14-bit counts, 4096/g). Returns the count, or -1 on a bus error.
int  drainFifo(int16_t *xyz, int maxSamples, bool *overflow);

// Read the latched interrupt sources so INT1/INT2 release.
void clearLatches();

// Raw pin levels (no I2C).
bool int1High();
bool int2High();

}  // namespace accel
