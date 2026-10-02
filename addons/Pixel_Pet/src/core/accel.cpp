#include <Arduino.h>
#include <Wire.h>
#include "pins.h"
#include "accel.h"

namespace accel {

static uint8_t s_addr = 0;

// Register map (MMA8451Q)
enum : uint8_t {
  F_STATUS = 0x00, OUT_X_MSB = 0x01, F_SETUP = 0x09, SYSMOD = 0x0B, INT_SOURCE = 0x0C,
  WHO_AM_I = 0x0D, XYZ_DATA_CFG = 0x0E, HP_FILTER_CUTOFF = 0x0F, FF_MT_SRC = 0x16, TRANSIENT_CFG = 0x1D,
  TRANSIENT_SRC = 0x1E, TRANSIENT_THS = 0x1F, TRANSIENT_COUNT = 0x20,
  CTRL_REG1 = 0x2A, CTRL_REG2 = 0x2B, CTRL_REG3 = 0x2C, CTRL_REG4 = 0x2D, CTRL_REG5 = 0x2E,
};

// CTRL_REG1: DR = 101 (12.5 Hz) in bits 5:3, ACTIVE bit 0.
static const uint8_t kCtrl1Odr = (5 << 3);

static bool wr(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(s_addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool rd(uint8_t reg, uint8_t &val) {
  Wire.beginTransmission(s_addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)s_addr, 1) != 1) return false;
  val = (uint8_t)Wire.read();
  return true;
}

static bool ping(uint8_t a) {
  Wire.beginTransmission(a);
  return Wire.endTransmission() == 0;
}

bool begin() {
  if (ping(0x1C)) s_addr = 0x1C;
  else if (ping(0x1D)) s_addr = 0x1D;
  else s_addr = 0;
  if (s_addr) {
    uint8_t id = 0;
    if (!rd(WHO_AM_I, id) || id != 0x1A) {
      Serial.printf("accel: WHO_AM_I 0x%02X (expected 0x1A) - disabled\n", id);
      s_addr = 0;
    }
  }
  return s_addr != 0;
}

bool present() { return s_addr != 0; }
uint8_t address() { return s_addr; }

void standby() {
  if (!s_addr) return;
  uint8_t c1 = 0;
  if (rd(CTRL_REG1, c1)) wr(CTRL_REG1, (uint8_t)(c1 & ~0x01));
  else wr(CTRL_REG1, 0x00);
}

void configSampling(bool joltWake, uint8_t joltThs) {
  if (!s_addr) return;
  standby();
  wr(F_SETUP, 0x00);                         // FIFO off before changing its mode
  wr(XYZ_DATA_CFG, 0x00);                    // ±2 g, no high-pass on the data
  wr(HP_FILTER_CUTOFF, 0x00);                // default cutoff for the jolt engine
  wr(CTRL_REG2, 0x00);                       // MODS normal oversampling, no auto-sleep
  wr(CTRL_REG3, 0x02);                       // IPOL active-high, push-pull, FIFO gate off
  if (joltWake) {
    if (joltThs < 0x04) joltThs = 0x04;
    wr(TRANSIENT_CFG, 0x1E);                 // ELE + X/Y/Z, high-passed
    wr(TRANSIENT_THS, (uint8_t)(joltThs & 0x7F));
    wr(TRANSIENT_COUNT, 0x01);               // 1 sample (80 ms) debounce at 12.5 Hz
  } else {
    wr(TRANSIENT_CFG, 0x00);
  }
  wr(CTRL_REG4, (uint8_t)(0x40 | (joltWake ? 0x20 : 0x00)));   // FIFO (+ TRANSIENT)
  wr(CTRL_REG5, (uint8_t)(joltWake ? 0x20 : 0x00));            // FIFO->INT2, TRANS->INT1
  wr(F_SETUP, (uint8_t)(0x40 | (kWatermark & 0x3F)));          // circular + watermark
  wr(CTRL_REG1, (uint8_t)(kCtrl1Odr | 0x01));                  // 12.5 Hz, active
  clearLatches();
}

void configMotionWake(uint8_t ths) {
  if (!s_addr) return;
  standby();
  wr(F_SETUP, 0x00);                         // FIFO off: no INT2 watermark in deep sleep
  wr(CTRL_REG2, 0x03);                       // MODS low-power: plenty for a motion wake
  wr(CTRL_REG3, 0x02);
  // SEL=00: the highest of the low-power cutoffs at 12.5 Hz (a fraction of a
  // hertz). Arm swings at walking pace (~1 Hz) pass, slow drifts don't, and
  // the filter settles within about a second of going active (a lower cutoff
  // could keep the gravity step above threshold for many seconds).
  wr(HP_FILTER_CUTOFF, 0x00);
  if (ths < 0x02) ths = 0x02;
  wr(TRANSIENT_CFG, 0x1E);
  wr(TRANSIENT_THS, (uint8_t)(ths & 0x7F));
  wr(TRANSIENT_COUNT, 0x02);                 // 160 ms of sustained change
  wr(CTRL_REG4, 0x20);                       // TRANSIENT only
  wr(CTRL_REG5, 0x20);                       // -> INT1
  wr(CTRL_REG1, (uint8_t)(kCtrl1Odr | 0x01));
  clearLatches();
}

void configJoltWake(uint8_t ths) {
  if (!s_addr) return;
  standby();
  wr(F_SETUP, 0x00);                         // FIFO off: no INT2 in deep sleep
  wr(XYZ_DATA_CFG, 0x00);
  wr(HP_FILTER_CUTOFF, 0x00);                // default cutoff: sharp jolts only
  wr(CTRL_REG2, 0x00);                       // normal oversampling
  if (ths < 0x04) ths = 0x04;                // BaseOS's sane minimum
  wr(TRANSIENT_CFG, 0x1E);
  wr(TRANSIENT_THS, (uint8_t)(ths & 0x7F));
  wr(TRANSIENT_COUNT, 0x05);                 // 5 samples debounce
  wr(CTRL_REG3, 0x02);                       // active high, push-pull
  wr(CTRL_REG4, 0x20);                       // TRANSIENT only
  wr(CTRL_REG5, 0x20);                       // -> INT1
  wr(CTRL_REG1, 0x01);                       // 800 Hz, active (as BaseOS)
  clearLatches();
}

int drainFifo(int16_t *xyz, int maxSamples, bool *overflow) {
  if (overflow) *overflow = false;
  if (!s_addr) return -1;
  uint8_t st = 0;
  if (!rd(F_STATUS, st)) return -1;
  if (overflow) *overflow = (st & 0x80) != 0;
  int count = st & 0x3F;
  if (count > maxSamples) count = maxSamples;
  int got = 0;
  while (got < count) {
    // Wire's buffer is 128 bytes on this core: at most 20 samples per burst.
    int chunk = count - got;
    if (chunk > 20) chunk = 20;
    Wire.beginTransmission(s_addr);
    Wire.write(OUT_X_MSB);
    if (Wire.endTransmission(false) != 0) break;
    int want = chunk * 6;
    int n = Wire.requestFrom((int)s_addr, want);
    if (n != want) {
      while (Wire.available()) (void)Wire.read();
      break;
    }
    for (int i = 0; i < chunk; i++) {
      uint8_t b[6];
      for (int k = 0; k < 6; k++) b[k] = (uint8_t)Wire.read();
      int16_t *o = xyz + (got + i) * 3;
      o[0] = (int16_t)((b[0] << 8) | b[1]) >> 2;
      o[1] = (int16_t)((b[2] << 8) | b[3]) >> 2;
      o[2] = (int16_t)((b[4] << 8) | b[5]) >> 2;
    }
    got += chunk;
  }
  uint8_t dummy;
  rd(F_STATUS, dummy);                      // re-read: releases the watermark latch
  return got;
}

void clearLatches() {
  if (!s_addr) return;
  uint8_t v;
  rd(TRANSIENT_SRC, v);
  rd(FF_MT_SRC, v);
  rd(INT_SOURCE, v);
  rd(F_STATUS, v);
}

bool int1High() { return digitalRead(PIN_MMA_INT1) == HIGH; }
bool int2High() { return digitalRead(PIN_MMA_INT2) == HIGH; }

}  // namespace accel
