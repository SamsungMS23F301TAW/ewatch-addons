// IMU sample stream (added for Rep Counter).
//
// taskIO is the only task allowed on the I2C bus, so it is also the only one
// that reads the accelerometer. When someone wants every sample (the Rep
// Counter view, or a serial `REC on` session), taskIO switches the MMA8451
// to 100 Hz high-resolution mode with its FIFO and pushes every sample into
// this lock-free ring. Readers (one producer, several independent readers)
// each keep their own cursor and detect overruns themselves.
//
// While nobody asks for samples the chip keeps BaseOS's exact configuration.
#pragma once
#include <stdint.h>
#include <atomic>

enum : uint8_t {
  IMU_FLAG_GAP  = 1 << 0,   // samples were lost before this one
  IMU_FLAG_FIFO = 1 << 1,   // from the 100 Hz FIFO (else one poll per taskIO cycle)
  IMU_FLAG_4G   = 1 << 2,   // raw counts are 2048/g (else 4096/g)
};

struct ImuSample {
  uint32_t seq;             // running sample number (gaps show as jumps)
  uint32_t tMs;             // millis() estimate for this sample
  int16_t  x, y, z;         // raw 14-bit counts
  uint8_t  flags;
  uint8_t  pad;
};

static const uint32_t IMU_RING_SIZE = 512;   // 5 s at 100 Hz

// Who wants the stream. Any task may set these; taskIO reads them.
enum ImuClient : uint8_t { IMU_CLIENT_APP = 1, IMU_CLIENT_REC = 2 };
void imuStreamWant(ImuClient who, bool on);
bool imuStreamWanted();

// Producer side (taskIO only).
void imuStreamPush(int16_t x, int16_t y, int16_t z, uint32_t tMs, uint8_t flags);
void imuStreamMarkGap();          // next pushed sample carries IMU_FLAG_GAP

// Reader side. A cursor starts at imuStreamHead() (only new samples).
uint32_t imuStreamHead();
// Returns false when there is nothing new. `lost` is set when the reader fell
// so far behind that samples were overwritten (the cursor is moved forward).
bool imuStreamRead(uint32_t &cursor, ImuSample &out, uint32_t &lost);
float imuCountsPerG(uint8_t flags);
