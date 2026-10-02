// IMU sample stream: every accelerometer sample taskIO reads, timestamped,
// for views whose algorithms need all of them (shake detection here).
//
// A lock-free single-producer / single-consumer ring:
//   producer  taskIO only, imuStreamPush() right after readAccel()
//   consumer  one view on the render task: imuStreamEnable(true) in onEnter,
//             imuStreamPop() each frame, imuStreamEnable(false) in onExit
//
// While disabled, pushes are dropped immediately, so the stream costs taskIO
// one atomic load per cycle when nobody is listening. taskIO runs at about
// 45 Hz with jitter, so the 64-sample ring holds well over a second.
#pragma once
#include <stdint.h>

struct ImuSample {
  uint32_t ms;          // millis() when the sample was read
  int16_t  x, y, z;     // raw MMA8451 counts (4096 per g at +-2 g)
};

void     imuStreamEnable(bool on);   // consumer: start (clears) or stop
bool     imuStreamEnabled();
void     imuStreamPush(uint32_t ms, int16_t x, int16_t y, int16_t z);   // taskIO
bool     imuStreamPop(ImuSample &out);                                  // consumer
uint32_t imuStreamDropped();         // samples lost to a full ring (diagnostic)
