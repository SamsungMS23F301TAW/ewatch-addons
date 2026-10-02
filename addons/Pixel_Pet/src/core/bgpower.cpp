// Background power for Pixel Pet. See bgpower.h for the state machine.
// Light-sleep mechanics follow EWatch_Dev/EWatchOS2.1/src/core/power_mgr.cpp
// (cooperative taskIO pause, level-triggered GPIO wakes, simulated sleep while
// a USB host is attached), adapted to FIFO-paced step sampling.
#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <esp_attr.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <math.h>
#include "pins.h"
#include "power.h"
#include "display.h"
#include "touch.h"
#include "haptic.h"
#include "model.h"
#include "event.h"
#include "view.h"
#include "controller.h"
#include "accel.h"
#include "steps.h"
#include "petsvc.h"
#include "bgpower.h"

#ifndef PIXELPET_BG_STEPS
#define PIXELPET_BG_STEPS 1
#endif
#ifndef PIXELPET_DEEP_TIER
#define PIXELPET_DEEP_TIER 1
#endif
#ifndef PIXELPET_STILL_MIN
#define PIXELPET_STILL_MIN 12
#endif
#ifndef PIXELPET_PANEL_SLPIN
#define PIXELPET_PANEL_SLPIN 1
#endif
#ifndef PIXELPET_LOWBAT_CUTOFF
#define PIXELPET_LOWBAT_CUTOFF 1
#endif

// Survive deep sleep: did *we* put the chip into the deep tier? These are
// RTC_DATA_ATTR on purpose: the bootloader re-initialises them on any reset
// other than a deep-sleep wake, so a crash never looks like a deep-tier wake.
RTC_DATA_ATTR static bool     s_inDeepTier = false;
RTC_DATA_ATTR static uint32_t s_rtcDeepEntries = 0;
RTC_DATA_ATTR static uint32_t s_rtcBgBoots = 0;

static bool    s_bootBackground = false;
static BgStats s_stats = {};

// Crash safety net for hardware bring-up: if the chip crashes in the dark
// loop twice in a row, background stepping is switched off (stock deep sleep
// is used instead) rather than crash-looping. The record must survive a panic
// reset, which re-initialises RTC_DATA_ATTR variables, so it lives in
// RTC_NOINIT_ATTR memory (left alone by the bootloader; garbage after a power
// cycle, which the check word rejects). A power cycle therefore clears it.
static const uint32_t kDarkMagic = 0xDA4C0002u;
static const uint32_t kInDark    = 0x0D0A7C11u;
RTC_NOINIT_ATTR static uint32_t s_darkCheck;
RTC_NOINIT_ATTR static uint32_t s_inDarkLoop;
RTC_NOINIT_ATTR static uint32_t s_darkCrashes;
static bool s_safeMode = false;

static uint32_t darkCheckFor(uint32_t inDark, uint32_t crashes) {
  return kDarkMagic ^ inDark ^ (crashes * 0x9E3779B1u);
}

static void darkSet(uint32_t inDark, uint32_t crashes) {
  s_inDarkLoop = inDark;
  s_darkCrashes = crashes;
  s_darkCheck = darkCheckFor(inDark, crashes);
}

void bgNoteResetReason(bool crashed) {
  uint32_t crashes = 0;
  bool wasDark = false;
  if (esp_reset_reason() != ESP_RST_POWERON &&
      s_darkCheck == darkCheckFor(s_inDarkLoop, s_darkCrashes)) {
    crashes = s_darkCrashes;
    wasDark = s_inDarkLoop == kInDark;
  }
  if (crashed && wasDark) {
    if (crashes < 255) crashes++;
  } else if (!crashed && crashes < 2) {
    crashes = 0;                             // a clean boot forgives a single crash
  }
  darkSet(0, crashes);
  s_safeMode = crashes >= 2;
}
bool bgSafeMode() { return s_safeMode; }

// ---------------------------------------------------------------------------
// Button latch for dark boots
// ---------------------------------------------------------------------------
// A background boot takes ~0.5-1 s before the dark loop starts watching the
// button, and the wearer often raises the wrist (motion wake) and then
// presses. An edge ISR remembers that press.
#if PIXELPET_BG_STEPS
static volatile bool s_bootPress = false;
static bool s_bootLatch = false;

static void IRAM_ATTR onBootPress() { s_bootPress = true; }

void bgArmBootPressLatch() {
  if (s_bootLatch) return;
  pinMode(PIN_BTN, INPUT);
  s_bootPress = digitalRead(PIN_BTN) == HIGH;
  attachInterrupt(digitalPinToInterrupt(PIN_BTN), onBootPress, RISING);
  s_bootLatch = true;
}

void bgDisarmBootPressLatch() {
  if (!s_bootLatch) return;
  // Must be gone before the pin is armed as a level-triggered light-sleep
  // wake: with the ISR still enabled that would be an interrupt storm.
  detachInterrupt(digitalPinToInterrupt(PIN_BTN));
  s_bootLatch = false;
}

// Disarms the latch. True if the button went down while it was armed, or is
// down right now.
static bool bgTakeBootPress() {
  const bool armed = s_bootLatch;
  bgDisarmBootPressLatch();
  const bool pressed = (armed && s_bootPress) || digitalRead(PIN_BTN) == HIGH;
  s_bootPress = false;
  return pressed;
}
#else
void bgArmBootPressLatch() {}
void bgDisarmBootPressLatch() {}
#endif

bool bgCompiled() { return PIXELPET_BG_STEPS != 0; }

bool bgActive() {
#if PIXELPET_BG_STEPS
  return !s_safeMode && accel::present() && petSvcOptions().bgSteps;
#else
  return false;
#endif
}

bool bgBootIsBackground() { return s_bootBackground; }
bool bgWokeFromDeepTier() { return s_inDeepTier; }

#if PIXELPET_BG_STEPS
static uint32_t deepNoMotionCount();
static uint32_t deepBusyCount();
#endif

BgStats bgStats() {
  BgStats s = s_stats;
  s.deepEntries = s_rtcDeepEntries;
  s.bgBoots = s_rtcBgBoots;
#if PIXELPET_BG_STEPS
  s.deepNoMotion = deepNoMotionCount();
  s.deepBusy = deepBusyCount();
#endif
  return s;
}

// ---------------------------------------------------------------------------
// Low battery
// ---------------------------------------------------------------------------
#if PIXELPET_LOWBAT_CUTOFF
// The count survives deep sleep: a still watch spends most of its time in
// the deep tier and takes one reading per background boot, so a RAM counter
// would never reach three. The first reading of each boot always counts (a
// deep sleep separates it from the previous one).
RTC_DATA_ATTR static uint8_t s_lowCount = 0;
static uint32_t s_lastBatMs = 0;
#endif

static void lowBatteryCheck(float v, bool ok, bool dark) {
#if PIXELPET_LOWBAT_CUTOFF
  // Ignore implausible readings: a board without the divider bodge (or a
  // flaky ADC) must never be switched off by noise. (No reading yet: skip.)
  if (!ok) return;
  if (v < 2.9f || v > 4.6f) { s_lowCount = 0; return; }
  if (s_lastBatMs && millis() - s_lastBatMs < 55000UL) return;
  s_lastBatMs = millis() | 1;
  if (v < 3.40f) s_lowCount++;
  else s_lowCount = 0;
  if (s_lowCount < 3) return;
  Serial.printf("power: battery %.2f V for 3 checks - powering off\n", v);
  s_lowCount = 0;                            // (if USB keeps us up, start afresh after SW2)
  stepsSvcCheckpoint(true);
  petSvcSave(true);
  if (!dark && gfx) {
    gfx->fillScreen(BLACK);
    gfx->setTextColor(WHITE, BLACK);
    gfx->setTextSize(2);
    gfx->setCursor(30, 120);
    gfx->print("Battery low");
    gfx->setCursor(30, 146);
    gfx->print("Charge me!");
    vTaskDelay(pdMS_TO_TICKS(1500));
  }
  backlightOff();
  powerOffNow();                             // does not return
#else
  (void)v; (void)ok; (void)dark;
#endif
}

void bgCheckBattery(float volts, bool ok) { lowBatteryCheck(volts, ok, false); }

#if PIXELPET_BG_STEPS

// ---------------------------------------------------------------------------
// Deep tier
// ---------------------------------------------------------------------------
RTC_DATA_ATTR static uint32_t s_deepUntil = 0;      // planned wake (RTC epoch), 0 = unknown
RTC_DATA_ATTR static bool     s_deepInt1 = false;   // motion wake armed for this deep sleep
RTC_DATA_ATTR static uint8_t  s_deepPhantoms = 0;   // phantom touch wakes this deep sleep
// Diagnostics for `status` (survive deep sleep, cleared by a reset):
RTC_DATA_ATTR static uint32_t s_rtcDeepNoMotion = 0;   // deep sleeps without motion wake
RTC_DATA_ATTR static uint32_t s_rtcDeepBusy = 0;       // entries put off: INT1 kept firing

static uint32_t deepNoMotionCount() { return s_rtcDeepNoMotion; }
static uint32_t deepBusyCount() { return s_rtcDeepBusy; }

// ~0.19 g, high-passed. Errs sensitive: missing the start of a walk costs
// steps; a spurious background boot costs ~1 s at ~50 mA.
static const uint8_t  kMotionWakeThs = 0x03;
static const uint32_t kDeepMaxSec    = 6UL * 3600UL;       // housekeeping wake
static const uint32_t kNoMotionWakeMaxSec = 30UL * 60UL;   // if INT1 can't be armed
static const uint8_t  kMaxPhantoms   = 6;                  // then touch wake off for this sleep
static const uint32_t kQuietMs       = 600;                // INT1 silent this long = settled
static const uint32_t kSettleMaxMs   = 4000;               // still firing after this = moving

static void drainTouchRaw() {
  Wire.beginTransmission(I2C_ADDR_TOUCH);
  Wire.write(0x01);
  if (Wire.endTransmission(false) == 0) {
    Wire.requestFrom((int)I2C_ADDR_TOUCH, 6);
    while (Wire.available()) (void)Wire.read();
  }
  touchPending = false;
}

static bool touchValid() {
  uint8_t g = 0, pts = 0;
  uint16_t x = 0, y = 0;
  const bool got = touchReadEventDirect(g, pts, x, y);
  touchPending = false;
  return got && (g != 0 || pts > 0);
}

static bool readClock(PetTime &t) {
  uint8_t h = 0, m = 0, s = 0, wd = 0, d = 1, mo = 1;
  uint16_t y = 2000;
  const bool ok = readRTC(h, m, s, wd, d, mo, y);
  t = petTimeFrom(h, m, s, wd, d, mo, y, ok);
  return ok;
}

static void armDeepAndSleep(uint32_t sleepSec, bool wkTouch, bool int1Ok) {
  drainTouchRaw();
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  if (wkTouch) {
    rtc_gpio_pullup_en((gpio_num_t)PIN_TOUCH_INT);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_TOUCH_INT, 0);
  }
  // The button always wakes the watch in this mode, whatever the Sleep
  // settings say: it is the one wake source that cannot misfire.
  uint64_t mask = 1ULL << PIN_BTN;
  if (int1Ok) mask |= 1ULL << PIN_MMA_INT1;
  esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_HIGH);
  if (sleepSec < 30) sleepSec = 30;
  esp_sleep_enable_timer_wakeup((uint64_t)sleepSec * 1000000ULL);
  s_inDeepTier = true;
  s_rtcDeepEntries++;
  backlightSet(0);
  holdPinsForDeepSleep();                    // motor enable + backlight held low
  gpio_hold_en((gpio_num_t)PIN_LDO_LATCH);
  gpio_deep_sleep_hold_en();
  Serial.flush();
  esp_deep_sleep_start();
}

// Why enterDeepTier() came back instead of sleeping.
enum DeepAbort : uint8_t { DEEP_MOVING, DEEP_BUTTON, DEEP_TOUCH };

// Saves everything and drops into deep sleep (no return) -- unless the wearer
// wakes the watch meanwhile (button: DEEP_BUTTON, a real touch: DEEP_TOUCH)
// or INT1 keeps firing while the motion wake settles, i.e. the wrist is
// moving (DEEP_MOVING). With `forceSleep` (INT1 also kept firing on earlier
// attempts: maybe stuck high) it sleeps anyway, without motion wake and with
// a short timer. Phantom touches are filtered here (several of them arm the
// sleep without touch wake).
static DeepAbort enterDeepTier(const PetTime &t, bool touchWake, bool forceSleep) {
  uint32_t sleepSec = kDeepMaxSec;
  const uint32_t nudge = petSvcNextNudge(t);
  if (t.ok && nudge > t.epoch && nudge - t.epoch < sleepSec) sleepSec = nudge - t.epoch;
  uint8_t phantoms = 0;
  // Button or a real finger? (Phantoms are counted and dropped.)
  auto wearer = [&]() -> int {
    if (digitalRead(PIN_BTN)) return DEEP_BUTTON;
    if (touchWake && (touchPending || digitalRead(PIN_TOUCH_INT) == LOW)) {
      if (touchValid()) return DEEP_TOUCH;
      s_stats.spuriousTouch++;
      if (++phantoms >= kMaxPhantoms) touchWake = false;   // noisy line: no touch wake
    }
    return -1;
  };
  bool int1Ok = false;
  if (accel::present()) {
    accel::configMotionWake(kMotionWakeThs);
    // INT1 means "moving" only once the chip is up (2/ODR + 1 ms) and its
    // high-pass filter has settled after the switch from standby. Wait until
    // it stays quiet for kQuietMs (re-clearing it), watching the wearer.
    const uint32_t t0 = millis();
    bool started = false;
    uint32_t quietSince = t0;
    for (;;) {
      esp_task_wdt_reset();
      const int w = wearer();
      if (w >= 0) return (DeepAbort)w;
      const uint32_t now = millis();
      if (now - t0 >= 180) {
        if (!started || accel::int1High()) {
          accel::clearLatches();
          started = true;
          quietSince = now;
        } else if (now - quietSince >= kQuietMs) {
          int1Ok = true;
          break;
        }
      }
      if (now - t0 >= kSettleMaxMs) break;
      vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!int1Ok) {
      s_rtcDeepBusy++;
      if (!forceSleep) return DEEP_MOVING;
    }
  }
  if (!int1Ok && sleepSec > kNoMotionWakeMaxSec) sleepSec = kNoMotionWakeMaxSec;
  stepsSvcCheckpoint(true);
  petSvcSave(true);
  const int w = wearer();                    // anything while saving?
  if (w >= 0) return (DeepAbort)w;
  s_deepUntil = t.ok ? t.epoch + sleepSec : 0;
  s_deepInt1 = int1Ok;
  s_deepPhantoms = 0;
  if (!int1Ok) s_rtcDeepNoMotion++;
  Serial.printf("power: still for %u min - deep sleep (%lus, motion wake %d)\n",
                (unsigned)PIXELPET_STILL_MIN, (unsigned long)sleepSec, int1Ok ? 1 : 0);
  armDeepAndSleep(sleepSec, touchWake, int1Ok);
  return DEEP_MOVING;                        // not reached
}

static bool backgroundBoot() {
  s_bootBackground = true;
  s_rtcBgBoots++;
  return true;
}

bool bgClassifyBoot(int wakeCause) {
  if (s_safeMode) Serial.println("power: crashed twice while dark - background steps off until power-off");
  const bool deep = s_inDeepTier;
  s_inDeepTier = false;
  if (!deep) return false;
  const esp_sleep_wakeup_cause_t w = (esp_sleep_wakeup_cause_t)wakeCause;
  if (w == ESP_SLEEP_WAKEUP_EXT0) {
    // Touch wake from the deep tier: real finger -> normal boot. Phantom
    // (EMI, charging) -> straight back to sleep without lighting anything.
    bool real = false;
    for (int i = 0; i < 5 && !real; i++) {
      Wire.beginTransmission(I2C_ADDR_TOUCH);
      Wire.write(0x01);
      if (Wire.endTransmission(false) == 0 && Wire.requestFrom((int)I2C_ADDR_TOUCH, 2) == 2) {
        uint8_t g = (uint8_t)Wire.read(), pts = (uint8_t)Wire.read();
        real = g != 0 || pts > 0;
      }
      if (!real) delay(30);
    }
    if (real || s_bootPress) { s_deepPhantoms = 0; return false; }
    // The accelerometer is still in motion-wake mode from before this boot:
    // a latched INT1 means the wrist moved too, so this is a motion boot.
    pinMode(PIN_MMA_INT1, INPUT);
    if (s_deepInt1 && digitalRead(PIN_MMA_INT1) == HIGH) { s_deepPhantoms = 0; return backgroundBoot(); }
    // Otherwise sleep again until the wake that was planned (nudge or
    // housekeeping), with the same motion wake as before. Repeated phantoms
    // (a noisy charger) turn touch wake off until the next real wake; the
    // button still works.
    // Without a known planned wake (clock invalid at entry), use the short
    // timer: each phantom would otherwise restart a 6 h one.
    uint32_t remain = kNoMotionWakeMaxSec;
    if (s_deepUntil) {
      PetTime now;
      if (readClock(now) && now.ok) {
        if (now.epoch + 30 >= s_deepUntil) { s_deepPhantoms = 0; return backgroundBoot(); }
        remain = s_deepUntil - now.epoch;
      }
    }
    if (remain > kDeepMaxSec) remain = kDeepMaxSec;
    if (!s_deepInt1 && remain > kNoMotionWakeMaxSec) remain = kNoMotionWakeMaxSec;
    if (s_deepPhantoms < 255) s_deepPhantoms++;
    const bool touchAgain = s_deepPhantoms < kMaxPhantoms;
    if (s_bootPress || digitalRead(PIN_BTN) == HIGH) { s_deepPhantoms = 0; return false; }
    Serial.printf("power: phantom touch wake %u - back to sleep (%lus%s)\n", (unsigned)s_deepPhantoms,
                  (unsigned long)remain, touchAgain ? "" : ", touch wake off");
    armDeepAndSleep(remain, touchAgain, s_deepInt1);
  }
  s_deepPhantoms = 0;
  if (w == ESP_SLEEP_WAKEUP_EXT1) {
    const uint64_t st = esp_sleep_get_ext1_wakeup_status();
    if (st & (1ULL << PIN_BTN)) return false;     // the wearer pressed the button
    return backgroundBoot();                       // motion: a walk may be starting
  }
  if (w == ESP_SLEEP_WAKEUP_TIMER) return backgroundBoot();   // nudge / housekeeping check
  return false;
}

// ---------------------------------------------------------------------------
// Light-sleep loop
// ---------------------------------------------------------------------------
enum Wake : uint8_t { W_NONE, W_FIFO, W_TIMER, W_BUTTON, W_TOUCH, W_JOLT };

static const uint32_t kStillMs          = (uint32_t)PIXELPET_STILL_MIN * 60000UL;
static const uint32_t kStillAfterBootMs = 3UL * 60000UL;
static const uint32_t kHousekeepMs      = 5UL * 60000UL;

// Wait for the motor to stop. Returns true if the button was pressed
// meanwhile (a quick press during a buzz must still wake the screen).
static bool waitHaptic(uint32_t maxMs) {
  const uint32_t t0 = millis();
  bool pressed = false;
  while (hapticBusy() && millis() - t0 < maxMs) {
    esp_task_wdt_reset();
    if (digitalRead(PIN_BTN)) pressed = true;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return pressed || digitalRead(PIN_BTN);
}

static int simulatedSleep(uint32_t ms, bool touchWake, bool int2Wake, bool joltWake) {
  s_stats.simulated++;
  const uint32_t t0 = millis();
  for (;;) {
    esp_task_wdt_reset();
    if (digitalRead(PIN_BTN)) return W_BUTTON;
    if (touchWake && (touchPending || digitalRead(PIN_TOUCH_INT) == LOW)) return W_TOUCH;
    if (joltWake && accel::int1High()) return W_JOLT;
    if (int2Wake && accel::int2High()) return W_FIFO;
    if (millis() - t0 >= ms) return W_TIMER;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

static int sleepOnce(uint32_t ms, bool touchWake, bool int2Wake, bool joltWake, bool dev) {
  // Already-asserted sources: don't even try to sleep.
  if (digitalRead(PIN_BTN)) return W_BUTTON;
  if (touchWake && (touchPending || digitalRead(PIN_TOUCH_INT) == LOW)) return W_TOUCH;
  if (joltWake && accel::int1High()) return W_JOLT;
  if (int2Wake && accel::int2High()) return W_FIFO;
  if (dev) return simulatedSleep(ms, touchWake, int2Wake, joltWake);

  // Level-triggered wakes change the pins' interrupt type. The touch pin has
  // the CST816S edge ISR on it: mask that interrupt while the pin is armed
  // at a level, or a held-low line would storm the CPU. (No ISR is attached
  // to the button or the accelerometer lines while this loop runs.)
  gpio_wakeup_enable((gpio_num_t)PIN_BTN, GPIO_INTR_HIGH_LEVEL);
  if (touchWake) {
    gpio_intr_disable((gpio_num_t)PIN_TOUCH_INT);
    gpio_wakeup_enable((gpio_num_t)PIN_TOUCH_INT, GPIO_INTR_LOW_LEVEL);
  }
  if (int2Wake)  gpio_wakeup_enable((gpio_num_t)PIN_MMA_INT2, GPIO_INTR_HIGH_LEVEL);
  if (joltWake)  gpio_wakeup_enable((gpio_num_t)PIN_MMA_INT1, GPIO_INTR_HIGH_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000ULL);
  esp_task_wdt_reset();
  s_stats.lightSleeps++;
  const esp_err_t err = esp_light_sleep_start();
  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  // Restore: wakeup_disable leaves the pin's interrupt type at the wake
  // level, so put the touch ISR back on its falling edge, then unmask it.
  if (touchWake) {
    gpio_wakeup_disable((gpio_num_t)PIN_TOUCH_INT);
    gpio_set_intr_type((gpio_num_t)PIN_TOUCH_INT, GPIO_INTR_NEGEDGE);
    gpio_intr_enable((gpio_num_t)PIN_TOUCH_INT);
  }
  // The other three have no ISR: leave them with no interrupt type rather
  // than a level one, so a later pinMode() (which re-enables the interrupt
  // of any typed pin) can't start a storm.
  gpio_wakeup_disable((gpio_num_t)PIN_BTN);
  gpio_wakeup_disable((gpio_num_t)PIN_MMA_INT2);
  gpio_wakeup_disable((gpio_num_t)PIN_MMA_INT1);
  gpio_set_intr_type((gpio_num_t)PIN_BTN, GPIO_INTR_DISABLE);
  gpio_set_intr_type((gpio_num_t)PIN_MMA_INT2, GPIO_INTR_DISABLE);
  gpio_set_intr_type((gpio_num_t)PIN_MMA_INT1, GPIO_INTR_DISABLE);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_task_wdt_reset();

  if (digitalRead(PIN_BTN)) return W_BUTTON;
  if (touchWake && (touchPending || digitalRead(PIN_TOUCH_INT) == LOW)) return W_TOUCH;
  if (joltWake && accel::int1High()) return W_JOLT;
  if (int2Wake && accel::int2High()) return W_FIFO;
  if (err != ESP_OK) return W_TIMER;                 // rejected: just service and retry
  if (cause == ESP_SLEEP_WAKEUP_TIMER) return W_TIMER;
  // A GPIO wake whose pin has already released is almost always the touch
  // controller's short INT pulse.
  if (cause == ESP_SLEEP_WAKEUP_GPIO && touchWake) return W_TOUCH;
  return W_NONE;
}

// The TRANSIENT (jolt) engine is high-passed, and its filter restarts at
// each standby -> active switch: until it settles, the switch itself can read
// as a jolt and light the screen straight back up. Wait (at most 1.5 s) until
// INT1 has stayed quiet for three samples, clearing it meanwhile. Returns
// true if the button was pressed during the wait.
static bool settleJolt() {
  const uint32_t t0 = millis();
  uint32_t quietSince = t0;
  accel::clearLatches();
  while (millis() - t0 < 1500) {
    esp_task_wdt_reset();
    if (digitalRead(PIN_BTN)) return true;
    if (accel::int1High()) {
      accel::clearLatches();
      quietSince = millis();
    } else if (millis() - quietSince >= 250) {
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  accel::clearLatches();
  return false;
}

// Did the wrist move during this batch? (Spread of |a| above a small floor.)
static bool batchMoved(const int16_t *xyz, int n) {
  float lo = 99.0f, hi = 0.0f;
  for (int i = 0; i < n; i++) {
    const float x = xyz[i * 3] / 4096.0f, y = xyz[i * 3 + 1] / 4096.0f, z = xyz[i * 3 + 2] / 4096.0f;
    const float m = sqrtf(x * x + y * y + z * z);
    if (m < lo) lo = m;
    if (m > hi) hi = m;
  }
  return n > 1 && hi - lo > 0.045f;
}


void bgRunScreenOff(bool fromBoot) {
  s_stats.screenOffs++;
  // ---- go dark ----------------------------------------------------------
  if (fromBoot) backlightSet(0);
  else backlightFadeTo(0, 140);
  panelSleep(PIXELPET_PANEL_SLPIN != 0);
  // A button press while going dark cancels it (never sleep with the motor
  // running, either).
  bool pressedEarly = waitHaptic(1500);
  controllerSuspendIo();                     // the I2C bus is ours from here on

  bool wkTouch, wkImu;
  uint8_t imuThs;
  { ModelLock lk; wkTouch = model.wakeOnTouch; wkImu = model.wakeOnImu; imuThs = model.imuWakeThreshold; }
  const bool haveAccel = accel::present();
  if (haveAccel) accel::configSampling(wkImu, imuThs);   // fresh FIFO, latches released
  drainTouchRaw();
  // Discard what taskIO queued while the screen faded, but notice a fresh
  // button press among it.
  if (eventQueue) {
    Event e;
    while (xQueueReceive(eventQueue, &e, 0) == pdPASS) {
      if (e.type == EventType::ButtonDown) pressedEarly = true;
    }
  }
  vTaskDelay(pdMS_TO_TICKS(60));             // let the INT lines settle
  if (haveAccel && wkImu && settleJolt()) pressedEarly = true;
  touchPending = false;
  // Dark boot: was the button pressed while it was starting up? (Always
  // disarm the latch here: its edge ISR must not be on the pin from now on.)
  if (bgTakeBootPress() && fromBoot) pressedEarly = true;
  if (pressedEarly) s_stats.wakeButton++;

  darkSet(kInDark, s_darkCrashes);           // crash-tracking (see bgNoteResetReason)
  const uint32_t stillMs = fromBoot ? kStillAfterBootMs : kStillMs;
  uint32_t lastMotion = millis();
  uint32_t lastHousekeep = 0;
  bool touchOk = wkTouch && touchPresent;    // no controller: nothing to wake us
  bool needGap = true;                       // FIFO restarted: break the detector chain
  bool int2Mode = haveAccel;                 // re-try INT2 at every dark period
  uint8_t int2Stuck = 0, int2Missed = 0;
  uint8_t deepAborts = 0;
  uint32_t spurWindow = millis();
  uint8_t spurCount = 0;
  int wake = W_NONE;

  while (!pressedEarly) {
    esp_task_wdt_reset();
    const bool dev = Serial.isPlugged();     // USB host: simulated sleep keeps serial
    uint32_t horizon = haveAccel ? (int2Mode ? 5000UL : 1400UL) : 60000UL;
#if PIXELPET_DEEP_TIER
    const uint32_t idle = millis() - lastMotion;
    if (!dev && idle < stillMs && stillMs - idle < horizon) horizon = stillMs - idle + 20;
#endif
    wake = sleepOnce(horizon, touchOk, haveAccel && int2Mode, wkImu && haveAccel, dev);
    switch (wake) {
      case W_FIFO:   s_stats.wakeFifo++; break;
      case W_TIMER:  s_stats.wakeTimer++; break;
      case W_BUTTON: s_stats.wakeButton++; break;
      case W_TOUCH:  s_stats.wakeTouch++; break;
      case W_JOLT:   s_stats.wakeJolt++; break;
      default: break;
    }

    // ---- service: accelerometer, clock, pet --------------------------------
    PetTime t;
    readClock(t);
    if (haveAccel) {
      int16_t buf[accel::kFifoSize * 3];
      bool ovf = false;
      const int n = accel::drainFifo(buf, accel::kFifoSize, &ovf);
      if (n > 0) {
        s_stats.samples += (uint32_t)n;
        const uint32_t added = stepsSvcFeed(buf, n, ovf || needGap, t.day);
        needGap = false;
        if (added || batchMoved(buf, n)) lastMotion = millis();
        if (added) deepAborts = 0;
        if (int2Mode) {
          // A timer wake that finds a full watermark means INT2 isn't waking
          // us: after a few in a row, poll on a timer instead.
          if (wake == W_TIMER && n >= accel::kWatermark) {
            if (++int2Missed >= 3) { int2Mode = false; s_stats.int2Faults++; }
          } else if (wake == W_FIFO) {
            int2Missed = 0;
          }
        }
      }
      if (int2Mode) {
        // INT2 still high after a drain would wake us forever: give up on it.
        if (accel::int2High()) {
          if (++int2Stuck >= 4) { int2Mode = false; s_stats.int2Faults++; }
        } else {
          int2Stuck = 0;
        }
      }
    }
    petSvcUpdate(t, false);                  // meals and nudges (may buzz)
    if (lastHousekeep == 0 || millis() - lastHousekeep >= kHousekeepMs) {
      lastHousekeep = millis();
      stepsSvcCheckpoint(false);
      petSvcSave(false);
      float v = 0;
      uint8_t pct = 0;
      const bool ok = readBattery(v, pct);
      if (ok) { ModelLock lk; model.vbat = v; model.batPct = pct; model.batOk = true; }
      lowBatteryCheck(v, ok, true);          // may power off
    }
    if (waitHaptic(1500)) wake = W_BUTTON;

    // ---- did the wearer wake us? -------------------------------------------
    if (wake == W_BUTTON) break;
    if (wake == W_JOLT) { accel::clearLatches(); break; }
    if (wake == W_TOUCH) {
      if (touchValid()) break;
      s_stats.spuriousTouch++;
      if (millis() - spurWindow > 10000UL) { spurWindow = millis(); spurCount = 0; }
      if (++spurCount > 20) {
        touchOk = false;                     // phantom-touch storm: button-only
        Serial.println("power: touch wake storm - touch wake off until next wake");
      }
      vTaskDelay(pdMS_TO_TICKS(30));
      touchPending = false;
    }
#if PIXELPET_DEEP_TIER
    if (!dev && millis() - lastMotion >= stillMs) {
      // Returns only if it didn't sleep. INT1 that won't settle twice in a
      // row (without steps in between) is suspect: the third attempt sleeps
      // regardless, on a short timer.
      const DeepAbort r = enterDeepTier(t, touchOk, deepAborts >= 2);
      bool woke = r != DEEP_MOVING;          // button or a real touch
      if (haveAccel) {                       // back to FIFO sampling
        accel::configSampling(wkImu, imuThs);
        if (wkImu && !woke && settleJolt()) { woke = true; s_stats.wakeButton++; }
      }
      needGap = true;
      if (r == DEEP_BUTTON) s_stats.wakeButton++;
      if (r == DEEP_TOUCH) s_stats.wakeTouch++;
      if (woke) break;
      if (deepAborts < 255) deepAborts++;    // DEEP_MOVING: sample on
      lastMotion = millis();
    }
#endif
  }

  // ---- fast wake: no reboot, panel RAM intact ----------------------------
  darkSet(0, pressedEarly ? s_darkCrashes : 0);   // a dark period went fine
  s_stats.int2Mode = int2Mode;
  {
    uint8_t h, m, s, wd, d, mo;
    uint16_t y;
    if (readRTC(h, m, s, wd, d, mo, y)) {
      ModelLock lk;
      model.hour = h; model.minute = m; model.second = s;
      model.weekday = wd; model.day = d; model.month = mo; model.year = y;
      model.rtcOk = true;
      model.revision++;
    }
  }
  uint8_t br, haptStr;
  { ModelLock lk; br = model.brightness; haptStr = model.hapticStrength; }
  hapticSetStrengthPct(haptStr);             // undo any live preview left behind
  panelWake();
  controllerResumeIo();                      // with a wake guard (see taskIO)
  Screen scr;
  { ModelLock lk; scr = model.screen; }
  if (scr != Screen::Watch) switchTo(Screen::Watch);
  else if (currentView) currentView->onEnter();   // force a full repaint
  if (currentView) currentView->render();
  panelDispOn();
  backlightSet(0);
  backlightFadeTo(br, 90);
}

#else  // !PIXELPET_BG_STEPS

bool bgClassifyBoot(int) { return false; }
void bgRunScreenOff(bool) {}

#endif
