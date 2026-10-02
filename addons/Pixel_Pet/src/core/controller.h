// Controller: bridges hardware to model + event queue.
//   - sensor reads (RTC, battery; accelerometer via accel.h)  -> mutate model
//   - input polling (button) + touch ISR drain -> post events
//   - actions exposed for views to commit changes (e.g. write RTC)
#pragma once
#include <Arduino.h>
#include "model.h"

void controllerInit();           // pinModes + ADC config + accelerometer setup
void controllerStartTasks();     // launches the FreeRTOS tasks

// Sensor sampling functions. Only the task that owns the I2C bus may call
// these (taskIO, or the background-sleep loop while taskIO is parked).
bool readRTC(uint8_t &h, uint8_t &m, uint8_t &s,
             uint8_t &weekday, uint8_t &day, uint8_t &month, uint16_t &year);
bool writeRTC(uint8_t h, uint8_t m, uint8_t s,
              uint8_t weekday, uint8_t day, uint8_t month, uint16_t year);
bool readBattery(float &volts, uint8_t &pct);

// Thread-safe entry from any task: queue an RTC write to be performed by
// taskIO at the start of its next cycle. Caller supplies HMS and date (year
// as full 4-digit value, weekday 0..6 per RV-3028 convention).
void requestSetRTC(uint8_t h, uint8_t m, uint8_t s,
                   uint8_t weekday, uint8_t day, uint8_t month, uint16_t year);

// Ask the render loop to turn the screen off now (the face's button press,
// Power Off -> Sleep). With background steps on this is the light-sleep
// sampler; otherwise stock deep sleep. Safe from any task.
void requestScreenOff();

// Cooperative pause of taskIO at a transaction boundary (<= 500 ms) so the
// background-sleep loop can own the I2C bus; resume re-arms the wake guard
// that swallows the press/touch which woke the watch.
void controllerSuspendIo();
void controllerResumeIo();

// Flush Pixel Pet's steps + pet state to NVS (before power-off/deep sleep).
void savePetAndSteps();

// Stock deep sleep (used when background steps are off): wake sources per the
// model's wakeOn* flags, then auto power-off after sleepToOffSec. Holds
// GPIO17 high during sleep so the LDO stays latched. Does not return.
void enterDeepSleep();

// Drop the LDO latch and stop. On battery the rail falls as soon as SW2 is
// released. If the chip is still running 3 s after that (USB keeps the rail
// up) it deep-sleeps with only SW2 as a wake source and the latch held low,
// so it looks off, and unplugging really powers it off. (BaseOS idled into
// the task watchdog instead: a crash reboot.) Does not return.
void powerOffNow();

// Deep sleep disconnects the digital pads. Drive the motor enable and the
// backlight low and hold them there (call right before esp_deep_sleep_start());
// releaseSleepPinHolds() first thing at boot drives them low and releases the
// holds (harmless when nothing is held).
void holdPinsForDeepSleep();
void releaseSleepPinHolds();
