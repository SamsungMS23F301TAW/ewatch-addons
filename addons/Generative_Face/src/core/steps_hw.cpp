#include "steps_hw.h"
#include <Wire.h>
#include <Preferences.h>
#include <esp_attr.h>
#include <freertos/FreeRTOS.h>
#include <string.h>
#include "steps.h"
#include "dayrec.h"
#include "daystore.h"
#include "haptic.h"
#include "console.h"
#include "gf_art.h"

// ---------------------------------------------------------------- state
static gf::StepDetector sDet(50.0f);
static uint8_t  sAddr = 0;
static portMUX_TYPE sMux = portMUX_INITIALIZER_UNLOCKED;
static int16_t  sLastX = 0, sLastY = 0, sLastZ = 4096;

// The live count survives deep sleep and soft resets in RTC memory; NVS
// checkpoints cover a full power-off.
struct RtcLive {
  uint32_t magic;
  uint16_t day;
  uint16_t celebrated;     // day the goal buzz already fired for
  uint32_t steps;
  uint32_t check;
};
static const uint32_t kMagic = 0x44505354;         // "DPST"
RTC_DATA_ATTR static RtcLive rtcLive;

static uint32_t sGoal = 8000;
static uint32_t sSavedSteps = 0xFFFFFFFF;
static uint16_t sSavedDay = 0xFFFF;
static uint32_t sSavedMs = 0;

// Days that closed while the collection couldn't be written wait here and
// in NVS ("pend") until a later attempt files them. Never dropped silently.
static const uint8_t kPendMax = 8;
static gf::DayRecord sPend[kPendMax];
static uint8_t  sPendN = 0;
static uint32_t sPendTryMs = 0;

static uint32_t liveCheck(const RtcLive &r) {
  return r.magic ^ ((uint32_t)r.day << 16) ^ r.celebrated ^ r.steps ^ 0xA5C35A3Cu;
}
static void sealLive() { rtcLive.check = liveCheck(rtcLive); }

// ---------------------------------------------------------------- I2C
static bool wr(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(sAddr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}
static bool rd(uint8_t reg, uint8_t &val) {
  Wire.beginTransmission(sAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)sAddr, 1) != 1) return false;
  val = Wire.read();
  return true;
}

static void configureStreaming() {
  wr(0x2A, 0x00);          // CTRL_REG1: standby while configuring
  wr(0x2B, 0x03);          // CTRL_REG2: MODS = low power
  wr(0x0E, 0x00);          // XYZ_DATA_CFG: +-2 g, no high-pass on output
  wr(0x09, 0x00);          // F_SETUP: FIFO off (flushes it)
  wr(0x09, 0x40 | 30);     // F_SETUP: circular, watermark 30 (0.6 s)
  wr(0x1D, 0x1E);          // TRANSIENT_CFG: latch + X/Y/Z, high-passed
  wr(0x1F, 0x03);          // TRANSIENT_THS: ~0.19 g
  wr(0x20, 0x02);          // TRANSIENT_COUNT: 2 samples
  wr(0x2C, 0x02);          // CTRL_REG3: active-high, push-pull
  wr(0x2D, 0x60);          // CTRL_REG4: FIFO + TRANSIENT interrupts
  wr(0x2E, 0x20);          // CTRL_REG5: TRANSIENT -> INT1, FIFO -> INT2
  wr(0x2A, 0x21);          // CTRL_REG1: 50 Hz, active
  uint8_t dummy;
  rd(0x1E, dummy);         // clear any latched transient
}

bool stepsHwPresent() { return sAddr != 0; }

void stepsHwInit(uint8_t mmaAddr) {
  sAddr = mmaAddr;
  if (sAddr) configureStreaming();
  // Restore today's count: RTC memory first (deep-sleep wake), then NVS.
  if (rtcLive.magic != kMagic || rtcLive.check != liveCheck(rtcLive)) {
    Preferences p;
    p.begin("generative-face", true);
    rtcLive.magic = kMagic;
    rtcLive.day = p.getUShort("cDay", 0);
    rtcLive.steps = p.getULong("cSteps", 0);
    rtcLive.celebrated = p.getUShort("cGoalDay", 0);
    p.end();
    sealLive();
    gfLog("Steps  : restored %lu steps for day %u from NVS\n",
          (unsigned long)rtcLive.steps, rtcLive.day);
  } else {
    gfLog("Steps  : %lu steps for day %u kept in RTC memory\n",
          (unsigned long)rtcLive.steps, rtcLive.day);
  }
  sSavedSteps = rtcLive.steps;
  sSavedDay = rtcLive.day;
  sSavedMs = millis();
  // Days still waiting to be filed.
  Preferences p;
  if (p.begin("generative-face", true)) {
    size_t n = p.getBytesLength("pend");
    if (n && n % sizeof(gf::DayRecord) == 0 && n <= sizeof(sPend)) {
      p.getBytes("pend", sPend, n);
      sPendN = (uint8_t)(n / sizeof(gf::DayRecord));
      gfLog("Steps  : %u closed day(s) waiting for the collection\n", sPendN);
    }
    p.end();
  }
}

static void pendSave() {
  Preferences p;
  if (!p.begin("generative-face", false)) return;
  if (sPendN) p.putBytes("pend", sPend, sizeof(gf::DayRecord) * sPendN);
  else p.remove("pend");
  p.end();
}

// Files a closed day, or parks it when the collection is unavailable.
static void fileDay(const gf::DayRecord &r) {
  if (daystoreAdd(r)) return;
  for (uint8_t i = 0; i < sPendN; i++) {
    if (sPend[i].day == r.day) {
      if (r.steps > sPend[i].steps) sPend[i] = r;
      pendSave();
      return;
    }
  }
  if (sPendN == kPendMax) {                   // keep the newest eight
    memmove(&sPend[0], &sPend[1], sizeof(gf::DayRecord) * (kPendMax - 1));
    sPendN--;
  }
  sPend[sPendN++] = r;
  pendSave();
  gfLog("Steps  : collection unavailable; day %u kept for later\n", r.day);
}

static void pendRetry() {
  uint8_t k = 0;
  for (uint8_t i = 0; i < sPendN; i++) {
    if (!daystoreAdd(sPend[i])) sPend[k++] = sPend[i];
  }
  if (k != sPendN) {
    sPendN = k;
    pendSave();
  }
}

StepDrain stepsHwDrain() {
  StepDrain d;
  if (!sAddr) { d.ok = false; return d; }
  uint8_t st;
  if (!rd(0x00, st)) { d.ok = false; return d; }       // F_STATUS
  uint8_t cnt = st & 0x3F;
  d.overflow = (st & 0x80) != 0;
  if (d.overflow) sDet.gap();
  uint32_t credited = 0, motion = 0;
  int16_t px = sLastX, py = sLastY, pz = sLastZ;
  while (cnt > 0) {
    uint8_t n = cnt > 20 ? 20 : cnt;                    // Wire buffer is 128 bytes
    Wire.beginTransmission(sAddr);
    Wire.write(0x01);
    if (Wire.endTransmission(false) != 0) { d.ok = false; break; }
    if (Wire.requestFrom((int)sAddr, (int)(n * 6)) != n * 6) {
      while (Wire.available()) Wire.read();
      d.ok = false;
      break;
    }
    for (uint8_t i = 0; i < n; i++) {
      uint8_t b[6];
      for (int k = 0; k < 6; k++) b[k] = (uint8_t)Wire.read();
      int16_t x = (int16_t)((b[0] << 8) | b[1]) >> 2;
      int16_t y = (int16_t)((b[2] << 8) | b[3]) >> 2;
      int16_t z = (int16_t)((b[4] << 8) | b[5]) >> 2;
      credited += sDet.push(x, y, z);
      motion += (uint32_t)(abs(x - px) + abs(y - py) + abs(z - pz));
      px = x; py = y; pz = z;
      d.samples++;
    }
    cnt -= n;
  }
  if (d.samples) {
    sLastX = px; sLastY = py; sLastZ = pz;
    d.motion = motion / d.samples;
  }
  d.x = sLastX; d.y = sLastY; d.z = sLastZ;
  if (credited) {
    portENTER_CRITICAL(&sMux);
    rtcLive.steps += credited;
    sealLive();
    portEXIT_CRITICAL(&sMux);
  }
  return d;
}

void stepsHwClearMotionLatch() {
  if (!sAddr) return;
  uint8_t v;
  rd(0x1E, v);             // TRANSIENT_SRC: reading clears the latch
  rd(0x0C, v);             // INT_SOURCE, defensively
}

void stepsHwPrepareDeepSleep() {
  if (!sAddr) return;
  wr(0x2A, 0x00);          // standby
  wr(0x09, 0x00);          // FIFO off
  wr(0x1D, 0x1E);          // TRANSIENT: latch + X/Y/Z
  wr(0x1F, 0x04);          // ~0.25 g: a wrist moving, not a table knock
  wr(0x20, 0x02);
  wr(0x2C, 0x02);          // active-high
  wr(0x2D, 0x20);          // TRANSIENT interrupt only
  wr(0x2E, 0x20);          // -> INT1 (GPIO3, an RTC pin)
  wr(0x2A, 0x29);          // 12.5 Hz, active
  stepsHwClearMotionLatch();
}

// ---------------------------------------------------------------- counting
uint32_t stepsToday() { return rtcLive.steps; }
uint16_t stepsDay()   { return rtcLive.day; }
bool     stepsWalking() { return sDet.walking(); }
uint32_t stepsGoal()  { return sGoal; }
void     stepsSetGoal(uint32_t g) { sGoal = g; }

void stepsCheckpointNow() {
  RtcLive snap;
  portENTER_CRITICAL(&sMux);
  snap = rtcLive;
  portEXIT_CRITICAL(&sMux);
  if (snap.steps == sSavedSteps && snap.day == sSavedDay) return;
  Preferences p;
  if (!p.begin("generative-face", false)) return;
  p.putUShort("cDay", snap.day);
  p.putULong("cSteps", snap.steps);
  p.putUShort("cGoalDay", snap.celebrated);
  p.end();
  sSavedSteps = snap.steps;
  sSavedDay = snap.day;
  sSavedMs = millis();
}

void stepsService(uint16_t today, bool dateValid, bool force) {
  if (dateValid) {
    gf::LiveDay live;
    gf::DayRecord done;
    bool filed;
    portENTER_CRITICAL(&sMux);
    live.day = rtcLive.day;
    live.steps = rtcLive.steps;
    filed = gf::rolloverDay(live, today, gf::ALGO_VERSION, done);
    rtcLive.day = live.day;
    rtcLive.steps = live.steps;
    sealLive();
    portEXIT_CRITICAL(&sMux);
    if (filed) {
      gfLog("Steps  : day %u closed with %lu steps\n", done.day, (unsigned long)done.steps);
      fileDay(done);
      force = true;
    } else if (live.day != sSavedDay) {
      force = true;                       // clock set: carried-over count
    }
    // Parked days: try again every ten minutes (cheap when the store is
    // still unavailable: it doesn't remount within a boot).
    if (sPendN && (sPendTryMs == 0 || millis() - sPendTryMs > 10UL * 60UL * 1000UL)) {
      sPendTryMs = millis();
      pendRetry();
    }
  }
  // The goal buzz: once per day, the first time the count reaches it.
  uint32_t steps = rtcLive.steps;
  if (sGoal > 0 && steps >= sGoal && rtcLive.celebrated != rtcLive.day) {
    portENTER_CRITICAL(&sMux);
    rtcLive.celebrated = rtcLive.day;
    sealLive();
    portEXIT_CRITICAL(&sMux);
    hapticBuzz(170, 70);
    hapticBuzz(0, 90);
    hapticBuzz(255, 180);
    force = true;
  }
  bool due = (steps >= sSavedSteps + 300) ||
             (steps != sSavedSteps && millis() - sSavedMs > 5UL * 60UL * 1000UL);
  if (force || due) stepsCheckpointNow();
}

void stepsDebugPrint(Print &out) {
  out.printf("STEPS day=%u today=%lu goal=%lu walking=%d cadence=%.2fHz thr=%.3fg peaks=%lu\n",
             rtcLive.day, (unsigned long)rtcLive.steps, (unsigned long)sGoal,
             sDet.walking() ? 1 : 0, sDet.cadenceHz(), sDet.threshold(),
             (unsigned long)sDet.peaks());
}
