// Controller: bridges hardware to model + event queue.
//   - sensor reads (RTC, IMU, battery)  -> mutate model
//   - input polling (button, IMU gestures) + touch ISR drain -> post events
//   - actions exposed for views to commit changes (e.g. write RTC)
#pragma once
#include <Arduino.h>
#include "model.h"

void controllerInit();           // pinModes + ADC config
void controllerStartTasks();     // launches the FreeRTOS tasks

// Sensor sampling functions. Only taskIO may call these — they touch the
// shared I2C bus or the battery ADC sequence and have no internal locking.
bool readRTC(uint8_t &h, uint8_t &m, uint8_t &s,
             uint8_t &weekday, uint8_t &day, uint8_t &month, uint16_t &year);
bool writeRTC(uint8_t h, uint8_t m, uint8_t s,
              uint8_t weekday, uint8_t day, uint8_t month, uint16_t year);
bool readAccel(int16_t &x, int16_t &y, int16_t &z);
bool readBattery(float &volts, uint8_t &pct);

// Thread-safe entry from any task: queue an RTC write to be performed by
// taskIO at the start of its next cycle. Caller supplies HMS and date (year
// as full 4-digit value, weekday 0..6 per RV-3028 convention).
void requestSetRTC(uint8_t h, uint8_t m, uint8_t s,
                   uint8_t weekday, uint8_t day, uint8_t month, uint16_t year);

// Put the watch to sleep. With Dayprint's background steps on (the
// default), this is a dark-screen LIGHT sleep that keeps counting steps and
// RETURNS when the user wakes the watch (see power_bg.h). Otherwise it is
// the stock deep sleep with wake sources per the model's wakeOn* flags
// (does not return; a wake is a reboot).
void enterDeepSleep();

// Light-sleep support: park taskIO at a transaction boundary (blocks up to
// 500 ms) so the caller owns the I2C bus, then let it run again. Returns
// false (and leaves taskIO running) if it didn't park in time.
bool controllerSuspendIo();
void controllerResumeIo();
uint8_t controllerMmaAddr();             // 0 if no accelerometer answered
