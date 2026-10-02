// Background power manager. Light-sleep pattern borrowed from EWatchOS2.1's
// power_mgr (same board): cooperative taskIO pause, interrupt hygiene before
// arming level-triggered GPIO wakes, simulated sleep while USB is attached.
#include "power_bg.h"
#include <Wire.h>
#include <esp_sleep.h>
#include <esp_task_wdt.h>
#include <esp_attr.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include "pins.h"
#include "power.h"
#include "display.h"
#include "touch.h"
#include "haptic.h"
#include "model.h"
#include "event.h"
#include "view.h"
#include "controller.h"
#include "steps_hw.h"
#include "gf_date.h"
#include "sleep_policy.h"

#ifndef GF_LOWBAT_CUTOFF_MV
#define GF_LOWBAT_CUTOFF_MV 3350
#endif

namespace {
const uint32_t kBatteryMs  = 60UL * 1000;           // battery check while dark
const uint32_t kDeepMarker = 0x44504453;            // "DPDS"
}  // namespace

RTC_DATA_ATTR static uint32_t gBgDeepMarker = 0;
RTC_DATA_ATTR static uint8_t  gBgDeepInt1 = 0;      // motion wake armed?
RTC_DATA_ATTR static uint8_t  gBgDeepLow = 0;       // consecutive low checks
static const uint64_t kDeepCheckUs = 4ULL * 3600ULL * 1000000ULL;
static bool sQuiet = false;

// Statistics for the PWR serial command.
static uint32_t sEntries = 0, sUserWakes = 0, sFifoWakes = 0, sMotionWakes = 0;
static uint32_t sTimerWakes = 0, sSpuriousTouch = 0, sSpuriousNone = 0, sStorms = 0;
static uint32_t sDrainErrors = 0, sStillEntries = 0, sStuckMotion = 0;
static uint64_t sScreenOffMs = 0;
static uint32_t sLastWakeLatencyMs = 0;

enum Wake : uint8_t { WK_NONE, WK_BUTTON, WK_TOUCH, WK_FIFO, WK_MOTION, WK_TIMER };

// Periodic battery check from the background deep sleep. Runs before
// setup() has touched anything: the LDO latch is still held from sleep.
static void deepCheckAndSleep() {
  analogSetPinAttenuation(PIN_BAT_MON_ADC, ADC_11db);
  pinMode(PIN_BAT_MON_EN, OUTPUT);
  digitalWrite(PIN_BAT_MON_EN, HIGH);
  delay(2);
  uint32_t acc = 0;
  for (int i = 0; i < 8; i++) acc += analogReadMilliVolts(PIN_BAT_MON_ADC);
  digitalWrite(PIN_BAT_MON_EN, LOW);
  float v = ((float)acc / 8.0f / 1000.0f) / 0.769f;
  bool plausible = acc > 0 && v > 2.9f && v < 4.6f;
#if GF_LOWBAT_CUTOFF_MV > 0
  if (plausible && v * 1000.0f < (float)GF_LOWBAT_CUTOFF_MV) {
    if (++gBgDeepLow >= 2) powerHardOff();  // the rail drops
  } else if (plausible) {
    gBgDeepLow = 0;
  }
#else
  (void)plausible;
#endif
  uint64_t mask = 1ULL << PIN_BTN;
  if (gBgDeepInt1) mask |= 1ULL << PIN_MMA_INT1;
  esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_HIGH);
  esp_sleep_enable_timer_wakeup(kDeepCheckUs);
  gpio_hold_en((gpio_num_t)PIN_LDO_LATCH);
  gpio_deep_sleep_hold_en();
  esp_deep_sleep_start();
}

void powerBgBoot(int wakeCause, uint64_t st) {
  bool marked = (gBgDeepMarker == kDeepMarker);
  if (marked && wakeCause == ESP_SLEEP_WAKEUP_TIMER) deepCheckAndSleep();   // no return
  bool ext1Wake = (wakeCause == ESP_SLEEP_WAKEUP_EXT1);
  gBgDeepMarker = 0;
  bool motion = (st & (1ULL << PIN_MMA_INT1)) != 0;
  bool button = (st & (1ULL << PIN_BTN)) != 0;
  sQuiet = ext1Wake && marked && motion && !button;
}

bool powerBgBootQuiet() { return sQuiet; }

static bool devMode() {
  return (bool)Serial;                 // USB host attached: keep serial alive
}

static Wake classify(bool fifoArmed, bool motionArmed, bool touchArmed) {
  if (digitalRead(PIN_BTN)) return WK_BUTTON;
  if (touchArmed && (touchPending || digitalRead(PIN_TOUCH_INT) == LOW)) {
    touchPending = false;
    return WK_TOUCH;
  }
  if (fifoArmed && digitalRead(PIN_MMA_INT2)) return WK_FIFO;
  if (motionArmed && digitalRead(PIN_MMA_INT1)) return WK_MOTION;
  return WK_NONE;
}

// Polling stand-in for light sleep while USB is attached.
static Wake fakeSleep(uint32_t maxMs, bool fifoArmed, bool motionArmed, bool touchArmed) {
  uint32_t t0 = millis();
  for (;;) {
    esp_task_wdt_reset();
    Wake w = classify(fifoArmed, motionArmed, touchArmed);
    if (w != WK_NONE) return w;
    if (millis() - t0 >= maxMs) return WK_TIMER;
    delay(20);
  }
}

static void refreshClock(uint16_t &today, bool &valid) {
  uint8_t h, m, s, wd, dy, mo;
  uint16_t yr;
  valid = false;
  if (readRTC(h, m, s, wd, dy, mo, yr)) {
    ModelLock lk;
    model.hour = h; model.minute = m; model.second = s;
    model.weekday = wd; model.day = dy; model.month = mo; model.year = yr;
    model.rtcOk = true;
    model.revision++;
    valid = gf::plausibleDate(yr, mo, dy);
    today = valid ? gf::dayIndex(yr, mo, dy) : 0;
  }
}

// gpio_wakeup_enable() rewrites a pin's interrupt type to the wake level and
// gpio_wakeup_disable() doesn't restore it. The touch pin carries the
// CST816S ISR (rising edge), so it sleeps with its interrupt disabled and
// gets its edge back after every wake; a stuck-low line can then never
// storm a level-triggered ISR.
static void touchIsrSleep() {
  gpio_intr_disable((gpio_num_t)PIN_TOUCH_INT);
}
static void touchIsrAwake() {
  gpio_set_intr_type((gpio_num_t)PIN_TOUCH_INT, GPIO_INTR_POSEDGE);
  gpio_intr_enable((gpio_num_t)PIN_TOUCH_INT);
}

static void armWakes(bool fifoArmed, bool motionArmed, bool touchArmed) {
  if (touchArmed) gpio_wakeup_enable((gpio_num_t)PIN_TOUCH_INT, GPIO_INTR_LOW_LEVEL);
  else            gpio_wakeup_disable((gpio_num_t)PIN_TOUCH_INT);
  gpio_wakeup_enable((gpio_num_t)PIN_BTN, GPIO_INTR_HIGH_LEVEL);    // always
  if (fifoArmed)   gpio_wakeup_enable((gpio_num_t)PIN_MMA_INT2, GPIO_INTR_HIGH_LEVEL);
  else             gpio_wakeup_disable((gpio_num_t)PIN_MMA_INT2);
  if (motionArmed) gpio_wakeup_enable((gpio_num_t)PIN_MMA_INT1, GPIO_INTR_HIGH_LEVEL);
  else             gpio_wakeup_disable((gpio_num_t)PIN_MMA_INT1);
  esp_sleep_enable_gpio_wakeup();
}

static void disarmWakes() {
  gpio_wakeup_disable((gpio_num_t)PIN_TOUCH_INT);
  gpio_wakeup_disable((gpio_num_t)PIN_BTN);
  gpio_wakeup_disable((gpio_num_t)PIN_MMA_INT2);
  gpio_wakeup_disable((gpio_num_t)PIN_MMA_INT1);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  touchIsrAwake();
}

void powerScreenOff() {
  sEntries++;
  uint32_t offStart = millis();
  bool quiet = sQuiet;

  // ---- 1. Dark: backlight down, panel asleep (its RAM keeps the frame).
  if (!quiet) {
    uint8_t br = backlightGet();
    for (int i = 6; i >= 0; i--) { backlightSet((uint8_t)(br * i / 7)); delay(18); }
  }
  backlightSet(0);
  panelOff();

  // ---- 2. Own the I2C bus; slow the CPU. If taskIO won't park (it never
  // should), stay awake rather than share the bus: light the face again and
  // let the idle timer try later.
  if (!controllerSuspendIo()) {
    panelWake();
    if (currentView) currentView->onWake();
    panelShow();
    uint8_t br;
    { ModelLock lk; br = model.brightness; }
    backlightSet(br);
    return;
  }
  setCpuFrequencyMhz(80);

  // ---- 3. Interrupt hygiene, so a stale assertion can't wake us at once.
  uint8_t g = 0, pts = 0;
  uint16_t tx = 0, ty = 0;
  touchReadEventDirect(g, pts, tx, ty);
  touchPending = false;
  stepsHwClearMotionLatch();
  stepsHwDrain();
  if (eventQueue) xQueueReset(eventQueue);
  for (int i = 0; i < 5; i++) { delay(50); esp_task_wdt_reset(); }   // finger lifts
  stepsHwDrain();
  touchReadEventDirect(g, pts, tx, ty);
  touchPending = false;
  stepsHwClearMotionLatch();
  pinMode(PIN_TOUCH_INT, INPUT_PULLUP);       // the INT pad floats between pulses

  // ---- 4. Sleep loop. The policy (sleep_policy.h, host-tested) decides
  // tiers, timers and storm handling; this loop does the hardware.
  gf::SleepPolicy pol;
  pol.begin(millis(), stepsHwPresent(), stepsToday());
  uint32_t lastBat = millis();

  for (;;) {
    esp_task_wdt_reset();
    // Yield one tick. esp_light_sleep_start() never blocks the task, so
    // without this the lower-priority tasks on this core (the loopTask,
    // which is subscribed to the task watchdog, and the haptic task) would
    // starve, and the watchdog would eventually panic. (The watchdog timer
    // only counts while awake, so it would take a long walk, but it would
    // come.)
    vTaskDelay(1);
    uint32_t now = millis();

    // Housekeeping: clock, midnight rollover, checkpoints, battery.
    if (pol.housekeepingDue(now)) {
      uint16_t today = 0;
      bool valid = false;
      refreshClock(today, valid);
      stepsService(today, valid);
      pol.housekeepingDone(now);
    }
    if (now - lastBat >= kBatteryMs) {
      lastBat = now;
      float v = 0; uint8_t pct = 0;
      bool ok = readBattery(v, pct);
      if (ok) { ModelLock lk; model.vbat = v; model.batPct = pct; model.batOk = true; }
      if (powerBgLowBatteryCheck(v, ok)) powerBgLowBatteryOff(false);
    }

    uint32_t horizon = 0;
    bool fifoBefore = pol.fifoArmed, motionBefore = pol.motionArmed;
    if (pol.plan(millis(), stepsToday(), horizon) == gf::SleepPolicy::kDeepSleep) {
      powerBgDeepSleep();                     // tier 3: does not return
    }
    if (pol.motionArmed && !motionBefore) stepsHwClearMotionLatch();    // entering tier 2
    (void)fifoBefore;

    // Never sleep with the motor running: the PWM would freeze on.
    for (int i = 0; i < 100 && !hapticIdle(); i++) { delay(10); esp_task_wdt_reset(); }

    bool fifoArmed = pol.fifoArmed, motionArmed = pol.motionArmed, touchArmed = pol.touchArmed;
    Wake wake;
    if (devMode()) {
      wake = fakeSleep(horizon, fifoArmed, motionArmed, touchArmed);
    } else {
      // The touch ISR goes quiet BEFORE gpio_wakeup_enable() turns its pin
      // level-triggered: with INT low at that moment the ISR would
      // otherwise storm on this core.
      touchIsrSleep();
      armWakes(fifoArmed, motionArmed, touchArmed);
      esp_sleep_enable_timer_wakeup((uint64_t)horizon * 1000ULL);
      gpio_hold_en((gpio_num_t)PIN_LDO_LATCH);
      esp_light_sleep_start();
      gpio_hold_dis((gpio_num_t)PIN_LDO_LATCH);
      touchIsrAwake();
      esp_sleep_wakeup_cause_t c = esp_sleep_get_wakeup_cause();
      wake = classify(fifoArmed, motionArmed, touchArmed);
      // The touch INT is a ~150 us pulse that may be over by now: a GPIO
      // wake with no other source asserted is a touch candidate, which the
      // register read below confirms or rejects.
      if (wake == WK_NONE && c == ESP_SLEEP_WAKEUP_GPIO && touchArmed) wake = WK_TOUCH;
      if (wake == WK_NONE && c == ESP_SLEEP_WAKEUP_TIMER) wake = WK_TIMER;
    }

    // While armed, the FIFO is drained after every wake: it feeds the step
    // detector and drops INT2 back below the watermark.
    bool drainOk = true;
    if (fifoArmed) {
      StepDrain d = stepsHwDrain();
      drainOk = d.ok;
      if (!d.ok) sDrainErrors++;
    }
    bool touchReal = false;
    if (wake == WK_TOUCH) {
      touchReal = touchReadEventDirect(g, pts, tx, ty) && (g != 0 || pts > 0);
    }
    // A motion wake: release INT1. If it is still high straight afterwards,
    // the line is stuck (the sensor needs two fresh samples, 40 ms, to
    // latch again), and the policy must not treat it as movement.
    bool motionStuck = false;
    if (wake == WK_MOTION) {
      stepsHwClearMotionLatch();
      motionStuck = digitalRead(PIN_MMA_INT1) == HIGH;
      if (motionStuck) sStuckMotion++;
    }
    gf::WakeCause cause = gf::WakeCause::None;
    switch (wake) {
      case WK_BUTTON: cause = gf::WakeCause::Button; break;
      case WK_TOUCH:  cause = gf::WakeCause::Touch;  break;
      case WK_FIFO:   cause = gf::WakeCause::Fifo;   sFifoWakes++; break;
      case WK_MOTION: cause = gf::WakeCause::Motion; sMotionWakes++; break;
      case WK_TIMER:  cause = gf::WakeCause::Timer;  sTimerWakes++; break;
      default:        cause = gf::WakeCause::None;   break;
    }
    gf::SleepPolicy::Outcome out =
        pol.onWake(millis(), cause, fifoArmed, drainOk, touchReal, motionStuck);
    if (wake == WK_MOTION && pol.fifoArmed) stepsHwDrain();   // the FIFO holds the last 0.64 s
    if (wake == WK_TOUCH && !touchReal) delay(30);   // let the INT line release
    if (out == gf::SleepPolicy::kDeepSleepNow) powerBgDeepSleep();
    if (out == gf::SleepPolicy::kUserWake) break;
  }
  sStillEntries += pol.stillEntries;
  sSpuriousTouch += pol.spuriousTouch;
  sSpuriousNone += pol.spuriousNone;
  sStorms += pol.storms;

  // ---- 5. Wake: clock first, then a fresh frame, then light.
  uint32_t tWake = millis();
  disarmWakes();
  setCpuFrequencyMhz(240);
  {
    uint16_t today = 0;
    bool valid = false;
    refreshClock(today, valid);
    stepsService(today, valid);
  }
  sUserWakes++;
  sScreenOffMs += (millis() - offStart);
  sQuiet = false;
  panelWake();
  Screen scr;
  { ModelLock lk; scr = model.screen; }
  // Re-enter the face so views that only paint deltas (the classic face)
  // redraw everything; then a fresh, complete frame before the light.
  if (scr != Screen::Watch) switchTo(Screen::Watch);
  else if (currentView) currentView->onEnter();
  if (currentView) currentView->onWake();
  panelShow();
  uint8_t br;
  { ModelLock lk; br = model.brightness; }
  for (int i = 1; i <= 5; i++) { backlightSet((uint8_t)(br * i / 5)); delay(12); }
  sLastWakeLatencyMs = millis() - tWake;
  hapticBuzz(90, 25);
  // The waking tap must not reach the face as a tap.
  touchReadEventDirect(g, pts, tx, ty);
  touchPending = false;
  if (eventQueue) xQueueReset(eventQueue);
  controllerResumeIo();
}

void powerBgDeepSleep() {
  stepsCheckpointNow();
  disarmWakes();
  backlightSet(0);
  panelOff();
  // Accelerometer: FIFO off, 12.5 Hz motion detection on INT1.
  stepsHwPrepareDeepSleep();
  delay(20);
  stepsHwClearMotionLatch();
  bool motionWake = stepsHwPresent() && digitalRead(PIN_MMA_INT1) == LOW;
  // Touch controller: power it down for the night (setup() pulses its RST
  // on every boot, which wakes it again).
  uint8_t g, pts; uint16_t tx, ty;
  touchReadEventDirect(g, pts, tx, ty);
  touchDeepSleepNow();
  uint64_t mask = 1ULL << PIN_BTN;
  if (motionWake) mask |= 1ULL << PIN_MMA_INT1;
  esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_HIGH);
  esp_sleep_enable_timer_wakeup(kDeepCheckUs);    // battery check (see powerBgBoot)
  gBgDeepInt1 = motionWake ? 1 : 0;
  gBgDeepMarker = kDeepMarker;
  Serial.flush();
  gpio_hold_en((gpio_num_t)PIN_LDO_LATCH);
  gpio_deep_sleep_hold_en();
  esp_deep_sleep_start();
}

static uint8_t  sLowCount = 0;
static uint32_t sLastBatMs = 0;

bool powerBgLowBatteryCheck(float volts, bool ok) {
#if GF_LOWBAT_CUTOFF_MV > 0
  uint32_t now = millis();
  if (sLastBatMs != 0 && now - sLastBatMs < 55000) return false;
  bool plausible = ok && volts > 2.9f && volts < 4.6f;
  if (!plausible) return false;               // bad readings neither count nor reset
  sLastBatMs = now;
  if (volts * 1000.0f < (float)GF_LOWBAT_CUTOFF_MV) sLowCount++;
  else sLowCount = 0;
  return sLowCount >= 3;
#else
  (void)volts; (void)ok;
  return false;
#endif
}

void powerBgLowBatteryOff(bool screenOn) {
  stepsCheckpointNow();
  if (screenOn && gfx) {
    gfx->fillScreen(BLACK);
    gfx->setTextColor(WHITE, BLACK);
    gfx->setTextSize(2);
    gfx->setCursor(48, 124);
    gfx->print("battery low");
    delay(1500);
  }
  backlightOff();
  powerHardOff();
}

void powerHardOff() {
  // A watchdog reset from here would latch the power straight back on.
  esp_task_wdt_delete(nullptr);               // harmless if not subscribed
  gBgDeepMarker = 0;
  gpio_hold_dis((gpio_num_t)PIN_LDO_LATCH);
  gpio_deep_sleep_hold_dis();
  digitalWrite(PIN_LDO_LATCH, LOW);
  pinMode(PIN_LDO_LATCH, OUTPUT);
  // SW2 keeps the LDO enabled while it is held (the 3 s power-off hold):
  // the rail drops when it is released.
  while (digitalRead(PIN_BTN)) delay(20);
  delay(1000);
  // Still running: something else holds the rail up. Sleep with only the
  // button armed and the latch held low, the nearest thing to off.
  Serial.flush();
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_ext1_wakeup(1ULL << PIN_BTN, ESP_EXT1_WAKEUP_ANY_HIGH);
  gpio_hold_en((gpio_num_t)PIN_LDO_LATCH);
  gpio_deep_sleep_hold_en();
  esp_deep_sleep_start();
  for (;;) delay(1000);
}

void powerBgStatsPrint(Print &out) {
  out.printf("PWR screen_off=%lu user_wakes=%lu fifo_wakes=%lu motion_wakes=%lu timer_wakes=%lu\n",
             (unsigned long)sEntries, (unsigned long)sUserWakes, (unsigned long)sFifoWakes,
             (unsigned long)sMotionWakes, (unsigned long)sTimerWakes);
  out.printf("PWR still_entries=%lu spurious_touch=%lu spurious_none=%lu storms=%lu drain_errors=%lu stuck_motion=%lu\n",
             (unsigned long)sStillEntries, (unsigned long)sSpuriousTouch,
             (unsigned long)sSpuriousNone, (unsigned long)sStorms, (unsigned long)sDrainErrors,
             (unsigned long)sStuckMotion);
  out.printf("PWR screen_off_ms=%llu last_wake_ms=%lu dev_mode=%d low_count=%u\n",
             (unsigned long long)sScreenOffMs, (unsigned long)sLastWakeLatencyMs,
             devMode() ? 1 : 0, (unsigned)sLowCount);
}
