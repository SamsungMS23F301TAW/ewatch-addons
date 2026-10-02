#include <Wire.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include "pins.h"
#include "power.h"
#include "i2c_bus.h"
#include "touch.h"
#include "display.h"
#include "haptic.h"
#include "controller.h"
#include "view.h"
#include "event.h"
#include "apptimer.h"
#include "accel.h"
#include "steps.h"
#include "bgpower.h"
#include "petsvc.h"

// ---------- I/O request queue (cross-task -> taskIO) ----------
struct RTCWrite {
  uint8_t  h, m, s;
  uint8_t  weekday;
  uint8_t  day, month;
  uint16_t year;
};
static QueueHandle_t rtcWriteQueue = nullptr;

void requestSetRTC(uint8_t h, uint8_t m, uint8_t s,
                   uint8_t weekday, uint8_t day, uint8_t month, uint16_t year) {
  if (!rtcWriteQueue) return;
  RTCWrite r{ h, m, s, weekday, day, month, year };
  xQueueSend(rtcWriteQueue, &r, 0);
}

// ---------- low-level I/O ----------
static uint8_t bcd2dec(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }
static uint8_t dec2bcd(uint8_t d) { return ((d / 10) << 4) | (d % 10); }

bool readRTC(uint8_t &h, uint8_t &m, uint8_t &s,
             uint8_t &weekday, uint8_t &day, uint8_t &month, uint16_t &year) {
  Wire.beginTransmission(I2C_ADDR_RV3028);
  Wire.write(0x00);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)I2C_ADDR_RV3028, 7) != 7) return false;
  uint8_t b[7];
  for (int i = 0; i < 7; i++) b[i] = Wire.read();
  s       = bcd2dec(b[0] & 0x7F);
  m       = bcd2dec(b[1] & 0x7F);
  h       = bcd2dec(b[2] & 0x3F);
  weekday = b[3] & 0x07;
  day     = bcd2dec(b[4] & 0x3F);
  month   = bcd2dec(b[5] & 0x1F);
  year    = 2000 + bcd2dec(b[6]);
  return true;
}

bool writeRTC(uint8_t h, uint8_t m, uint8_t s,
              uint8_t weekday, uint8_t day, uint8_t month, uint16_t year) {
  uint8_t yy = (year >= 2000) ? (year - 2000) : 0;
  if (yy > 99) yy = 99;
  Wire.beginTransmission(I2C_ADDR_RV3028);
  Wire.write(0x00);
  Wire.write(dec2bcd(s)     & 0x7F);
  Wire.write(dec2bcd(m)     & 0x7F);
  Wire.write(dec2bcd(h)     & 0x3F);
  Wire.write(weekday        & 0x07);
  Wire.write(dec2bcd(day)   & 0x3F);
  Wire.write(dec2bcd(month) & 0x1F);
  Wire.write(dec2bcd(yy));
  return Wire.endTransmission() == 0;
}

// Live "is finger on screen" poll. The CST816S library updates state only on
// ISR events, so we go straight to the chip to detect ongoing contact and
// release. Reads regs 0x02 (FingerNum) + 0x03..0x06 (XH/XL/YH/YL).
static bool readTouchHeld(uint16_t &x, uint16_t &y) {
  if (!touchPresent) return false;        // no controller — skip the I2C read
  Wire.beginTransmission(I2C_ADDR_TOUCH);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)I2C_ADDR_TOUCH, 5) != 5) return false;
  uint8_t pts = Wire.read();
  uint8_t xh  = Wire.read();
  uint8_t xl  = Wire.read();
  uint8_t yh  = Wire.read();
  uint8_t yl  = Wire.read();
  if (pts == 0) return false;
  x = ((uint16_t)(xh & 0x0F) << 8) | xl;
  y = ((uint16_t)(yh & 0x0F) << 8) | yl;
  return true;
}

bool readBattery(float &volts, uint8_t &pct) {
  digitalWrite(PIN_BAT_MON_EN, HIGH);
  delay(2);
  uint32_t acc = 0;
  for (int i = 0; i < 8; i++) acc += analogReadMilliVolts(PIN_BAT_MON_ADC);
  digitalWrite(PIN_BAT_MON_EN, LOW);
  uint32_t adc_mV = acc / 8;
  if (adc_mV == 0) return false;
  volts = (adc_mV / 1000.0f) / 0.769f;
  float p = (volts - 3.30f) * (100.0f / 0.90f);
  if (p < 0) p = 0;
  if (p > 100) p = 100;
  pct = (uint8_t)(p + 0.5f);
  return true;
}

// ---------- init ----------
void controllerInit() {
  pinMode(PIN_BTN,        INPUT);
  pinMode(PIN_RTC_INT,    INPUT_PULLUP);
  pinMode(PIN_MMA_INT1,   INPUT);
  pinMode(PIN_MMA_INT2,   INPUT);
  pinMode(PIN_BAT_MON_EN, OUTPUT);
  digitalWrite(PIN_BAT_MON_EN, LOW);
  analogSetPinAttenuation(PIN_BAT_MON_ADC, ADC_11db);

  // Accelerometer: FIFO-paced 12.5 Hz sampling for the step detector. (This
  // replaces BaseOS's free-running read of OUT_X: with the FIFO on, reading
  // the data registers pops samples, so only accel::drainFifo() may read them.)
  if (accel::begin()) accel::configSampling(false, 0);
  else Serial.println("accel: MMA8451 not found - no step counting");

  // CST816S IrqCtl (0xFA): EnTouch + EnChange + EnMotion. Without EnChange the
  // chip stops firing IRQs while a finger is held still, which broke the
  // tap-and-hold ramp logic in Settings. Polling reg 0x02 also has to work
  // reliably for the same flow. Skip when no controller answered at boot.
  if (touchPresent) {
    Wire.beginTransmission(I2C_ADDR_TOUCH);
    Wire.write(0xFA);
    Wire.write(0x70);
    Wire.endTransmission();
  }

  rtcWriteQueue = xQueueCreate(2, sizeof(RTCWrite));
}

// ---------- cross-task flags ----------
// Cooperative pause: the background-sleep loop asks taskIO to park at the top
// of its loop (never mid-I2C-transfer) before it takes the bus.
static volatile bool s_ioPauseReq = false;
// Wake guard: the button press / touch that woke the watch must not also be
// delivered as "back" or as a tap. taskIO swallows them until released.
static volatile bool s_ioWakeGuard = true;          // also covers a button-boot
static volatile bool s_screenOffReq = false;

void requestScreenOff() { s_screenOffReq = true; }

static uint32_t dayFromDate(uint16_t y, uint8_t mo, uint8_t d, bool ok) {
  if (!ok || y < 2024 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
  return rtcEpochSec(y, mo, d, 0, 0, 0) / 86400u;
}

// ---------- FreeRTOS tasks ----------

// Single I/O task — owns the I2C bus to avoid Wire-singleton races. Runs at
// ~50 Hz: button + touch every cycle, accelerometer FIFO every 8th cycle
// (~6 Hz, 1-3 samples each at 12.5 Hz ODR), RTC every 4th (~12 Hz), battery
// every 50th (~1 Hz). Drains pending RTC writes posted by other tasks.
static void taskIO(void *) {
  // Subscribe to the system task watchdog. If this loop ever stalls for more
  // than the WDT timeout the chip resets, and the panic-loop guard in main()
  // catches the repeat and drops the LDO latch.
  esp_task_wdt_add(nullptr);
  bool     lastBtn = false;
  uint32_t btnDownMs = 0;
  bool     veryLongFired = false;
  bool     suppressShort = false;     // button that woke us: no "back" on release
  bool     suppressTouch = false;     // finger that woke us: no tap until lifted

  uint32_t cycle = 0;
  bool     fingerDown = false;
  uint32_t lastHoldPostMs = 0;
  uint16_t lastTouchX = 0, lastTouchY = 0;
  uint32_t curDay = 0;
  uint32_t gestureMuteUntil = 0;      // swallow the gesture of the wake touch
  int16_t  fifo[accel::kFifoSize * 3];

  for (;;) {
    esp_task_wdt_reset();

    // ---- Cooperative pause point for the background-sleep loop ----
    if (s_ioPauseReq) {
      esp_task_wdt_delete(nullptr);
      vTaskSuspend(nullptr);
      esp_task_wdt_add(nullptr);
      esp_task_wdt_reset();
      continue;
    }

    // ---- Wake guard: adopt the current button/finger state silently ----
    if (s_ioWakeGuard) {
      s_ioWakeGuard = false;
      lastBtn = digitalRead(PIN_BTN);
      veryLongFired = false;
      suppressShort = lastBtn;
      if (lastBtn) btnDownMs = millis();     // a 3 s hold from here still powers off
      uint16_t hx, hy;
      fingerDown = readTouchHeld(hx, hy);
      suppressTouch = fingerDown;
      touchPending = false;
      // The CST816S reports a swipe's gesture around the lift, which may be
      // after this point: a swipe-right that woke the watch must not arrive
      // as "back" (= screen off again on the face).
      gestureMuteUntil = millis() + 400;
      { ModelLock lk; model.button = lastBtn; }
    }

    // ---- Drain pending RTC writes from other tasks ----
    RTCWrite r;
    while (rtcWriteQueue && xQueueReceive(rtcWriteQueue, &r, 0) == pdPASS) {
      writeRTC(r.h, r.m, r.s, r.weekday, r.day, r.month, r.year);
    }

    // ---- Touch state machine ----
    // We treat the polled FingerNum register as the source of truth for the
    // press/hold/release state; the ISR is only used to fish out gesture
    // codes (which are reported alongside the press frame). This dodges the
    // CST816S quirk where the chip stops emitting interrupts while a finger
    // is held stationary — the previous design relied on those.
    uint16_t hx = 0, hy = 0;
    bool live = readTouchHeld(hx, hy);

    if (live && !fingerDown) {
      fingerDown = true;
      lastTouchX = hx; lastTouchY = hy;
      Event e = makeEvent(EventType::Touch);
      e.x = hx; e.y = hy;
      postEvent(e);
      lastHoldPostMs = millis();
    } else if (live && fingerDown) {
      lastTouchX = hx; lastTouchY = hy;
      if (!suppressTouch && millis() - lastHoldPostMs >= 30) {
        lastHoldPostMs = millis();
        Event e = makeEvent(EventType::TouchHold);
        e.x = hx; e.y = hy;
        postEvent(e);
      }
    } else if (!live && fingerDown) {
      fingerDown = false;
      if (!suppressTouch) {
        Event e = makeEvent(EventType::TouchUp);
        e.x = lastTouchX; e.y = lastTouchY;
        postEvent(e);
      } else {
        gestureMuteUntil = millis() + 300;   // the wake touch's own gesture
      }
      suppressTouch = false;
    }

    // Drain ISR-reported gestures (only — Touch state already comes from poll).
    if (touchPending) {
      touchPending = false;
      if (touchpad.available()) {
        Gesture g = (Gesture)touchpad.data.gestureID;
        const bool muted = suppressTouch || (int32_t)(millis() - gestureMuteUntil) < 0;
        if (g != Gesture::None && !muted) {
          // Dedup: in continuous-report mode the chip can fire multiple
          // ISRs for one physical swipe. Suppress repeats within 400 ms.
          static Gesture  lastG = Gesture::None;
          static uint32_t lastGMs = 0;
          uint32_t now = millis();
          if (g != lastG || now - lastGMs > 400) {
            if (g == Gesture::SwipeRight) {
              // Global "back": every view already handles ButtonShort as the
              // back affordance, so route swipe-right through the same path
              // instead of inventing a new event type.
              Event back = makeEvent(EventType::ButtonShort);
              postEvent(back);
            } else {
              Event ge = makeEvent(EventType::Gesture);
              ge.x = touchpad.data.x;
              ge.y = touchpad.data.y;
              ge.gesture = g;
              postEvent(ge);
            }
          }
          lastG = g; lastGMs = now;
        }
      }
    }

    // ---- Button (GPIO, no Wire) ----
    bool btn = digitalRead(PIN_BTN);
    if (btn != lastBtn) {
      if (btn) {
        btnDownMs = millis();
        veryLongFired = false;
        suppressShort = false;
        Event e = makeEvent(EventType::ButtonDown);
        postEvent(e);
      } else {
        uint32_t held = millis() - btnDownMs;
        Event e = makeEvent(EventType::ButtonUp);
        postEvent(e);
        if (held < 1000 && !veryLongFired && !suppressShort) {
          Event s = makeEvent(EventType::ButtonShort);
          postEvent(s);
        }
        suppressShort = false;
      }
      { ModelLock lk; model.button = btn; model.revision++; }
      lastBtn = btn;
    }
    if (btn && !veryLongFired && (millis() - btnDownMs > 3000)) {
      Event e = makeEvent(EventType::ButtonVeryLong);
      postEvent(e);
      veryLongFired = true;
    }

    // ---- RTC + battery (lower rate) ----
    // The clock is read first: every FIFO cycle is also an RTC cycle, so the
    // step book is always rolled with this cycle's date (never a stale one
    // from before a dark period or from boot).
    uint8_t h = 0, mm = 0, s = 0;
    uint8_t wd = 0, dy = 0, mo = 1;
    uint16_t yr = 2025;
    bool rtcOk = false;
    bool ranRtc = (cycle % 4 == 0);
    if (ranRtc) {
      rtcOk = readRTC(h, mm, s, wd, dy, mo, yr);
      curDay = dayFromDate(yr, mo, dy, rtcOk);
    }

    // ---- Accelerometer FIFO -> step detector ----
    // (BaseOS's ImuMotion jolt heuristic is gone: walking would have kept the
    // screen awake. Raise-to-wake is the MMA's own TRANSIENT engine instead.)
    bool accNew = false;
    int16_t ax = 0, ay = 0, az = 0;
    if ((cycle % 8) == 0 && accel::present()) {
      bool ovf = false;
      int n = accel::drainFifo(fifo, accel::kFifoSize, &ovf);
      if (n > 0) {
        stepsSvcFeed(fifo, n, ovf, curDay);
        ax = fifo[(n - 1) * 3]; ay = fifo[(n - 1) * 3 + 1]; az = fifo[(n - 1) * 3 + 2];
        accNew = true;
      }
    }

    float vbat = 0; uint8_t pct = 0; bool batOk = false;
    bool ranBat = (cycle % 50 == 0);
    if (ranBat) batOk = readBattery(vbat, pct);

    // Bump `revision` only when something a view can show changed (the
    // accelerometer used to bump it every cycle, forcing ~50 Hz re-renders).
    // Animated views ask for frames via View::frameMs() instead.
    bool tickEvent = false;
    {
      ModelLock lk;
      bool changed = false;
      if (accNew) { model.ax = ax; model.ay = ay; model.az = az; changed = true; }
      if (model.imuOk != accel::present()) { model.imuOk = accel::present(); changed = true; }
      if (ranRtc) {
        if (rtcOk && s != model.second) tickEvent = true;
        if (model.hour != h || model.minute != mm || model.second != s || model.day != dy ||
            model.month != mo || model.year != yr || model.weekday != wd || model.rtcOk != rtcOk)
          changed = true;
        model.hour = h; model.minute = mm; model.second = s;
        model.weekday = wd; model.day = dy; model.month = mo; model.year = yr;
        model.rtcOk = rtcOk;
      }
      if (ranBat) {
        if (model.batPct != pct || model.batOk != batOk) changed = true;
        model.vbat = vbat; model.batPct = pct; model.batOk = batOk;
      }
      if (changed) model.revision++;
    }

    if (tickEvent) {
      Event e = makeEvent(EventType::Tick);
      postEvent(e);
    }

    // ---- Countdown timer expiry ----
    // Checked on every RTC read. timerArmed() is persistent (RTC memory), so
    // this also catches a timer that came due while the watch was asleep.
    if (ranRtc && rtcOk && timerArmed()) {
      uint32_t nowEpoch = rtcEpochSec(yr, mo, dy, h, mm, s);
      if (nowEpoch >= timerDeadlineEpoch()) {
        timerMarkFired();
        hapticBuzz(255, 220);                 // strong initial alarm buzz (was 400: wrapped to 144)
        Event e = makeEvent(EventType::TimerExpired);
        postEvent(e);
      }
    }

    cycle++;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

static TaskHandle_t ioTaskHandle = nullptr;

void controllerSuspendIo() {
  if (!ioTaskHandle) return;
  s_ioPauseReq = true;
  uint32_t t0 = millis();
  while (eTaskGetState(ioTaskHandle) != eSuspended && millis() - t0 < 500) {
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  if (eTaskGetState(ioTaskHandle) != eSuspended) {
    // Never seen in practice (a cycle is < 10 ms of work). Proceeding is
    // safe-ish: the bus has been idle for >= the 20 ms cycle delay.
    Serial.println("io: pause timeout");
  }
}

void controllerResumeIo() {
  if (!ioTaskHandle) return;
  s_ioWakeGuard = true;
  s_ioPauseReq = false;
  vTaskResume(ioTaskHandle);
}

// Persist the addon state, then drop the LDO latch and stop. Called from the
// render task in response to global power-off triggers (3 s button hold) or
// from PowerOffView.
void savePetAndSteps() {
  stepsSvcCheckpoint(true);
  petSvcSave(true);
}

static void driveQuietPinsLow() {
  digitalWrite(PIN_MOTOR_EN, LOW);
  pinMode(PIN_MOTOR_EN, OUTPUT);
  digitalWrite(PIN_BAT_MON_EN, LOW);
  pinMode(PIN_BAT_MON_EN, OUTPUT);
  digitalWrite(PIN_DISP_BL, LOW);            // plain GPIO low (detaches LEDC, which
  pinMode(PIN_DISP_BL, OUTPUT);              // backlightSet() re-attaches later)
}

void holdPinsForDeepSleep() {
  driveQuietPinsLow();
  // GPIO21 is an RTC pad, GPIO38 a digital one (both hold through deep sleep;
  // unheld digital pads are disconnected). GPIO33 sits on the flash supply,
  // which deep sleep switches off, so it is simply unpowered (low) anyway.
  gpio_hold_en((gpio_num_t)PIN_MOTOR_EN);
  gpio_hold_en((gpio_num_t)PIN_DISP_BL);
}

void releaseSleepPinHolds() {
  driveQuietPinsLow();                       // same level the holds kept: no float
  gpio_hold_dis((gpio_num_t)PIN_MOTOR_EN);
  gpio_hold_dis((gpio_num_t)PIN_DISP_BL);
}

void powerOffNow() {
  unlatchPower();
  uint32_t releasedAt = 0;
  for (;;) {
    esp_task_wdt_reset();
    if (digitalRead(PIN_BTN)) releasedAt = 0;          // SW2 still holds the rail up
    else if (!releasedAt) releasedAt = millis() | 1;
    else if (millis() - releasedAt > 3000) break;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  // Still running: something else (USB) keeps the rail up. Look switched off
  // until SW2 is pressed: deep sleep with the button as the only wake source
  // and the latch held low, so unplugging drops the rail for real.
  backlightOff();
  panelSleep(true);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_ext1_wakeup(1ULL << PIN_BTN, ESP_EXT1_WAKEUP_ANY_HIGH);
  holdPinsForDeepSleep();
  gpio_hold_en((gpio_num_t)PIN_LDO_LATCH);   // held LOW
  gpio_deep_sleep_hold_en();
  esp_deep_sleep_start();
}

static void shutdownNow() {
  if (gfx) {
    gfx->fillScreen(BLACK);
    gfx->setTextColor(WHITE, BLACK);
    gfx->setTextSize(2);
    gfx->setCursor(80, 130);
    gfx->print("bye");
  }
  savePetAndSteps();
  hapticBuzz(180, 200);
  vTaskDelay(pdMS_TO_TICKS(80));
  backlightOff();
  powerOffNow();                    // does not return
}

// Activity-flagging predicate: which event types count as "user is awake".
// (Motion is deliberately absent: walking must not keep the screen lit.)
static bool isActivity(EventType t) {
  return t == EventType::Touch       ||
         t == EventType::TouchHold   ||
         t == EventType::Gesture     ||
         t == EventType::ButtonDown  ||
         t == EventType::ButtonShort;
}

// Render task: drains events into the active view and re-renders on dirty.
// Touches the display from a single task; the I/O task never paints. Drains
// up to N events per cycle so a fast TouchHold stream doesn't queue up.
// Also enforces the screen timeout: with background steps on, the screen-off
// phase is the light-sleep sampler (bgpower); otherwise stock deep sleep.
static void taskRender(void *) {
  // Subscribe to the system task watchdog. A frozen view (infinite redraw,
  // stuck animation, deadlock) will fail to feed within the WDT window and
  // trigger a chip reset — covered by the panic-loop guard in main().
  esp_task_wdt_add(nullptr);
  uint32_t lastRender = 0;
  uint32_t lastSeenRev = 0;
  uint32_t lastActivity = millis();
  uint32_t lastHousekeep = 0;

  // A motion/timer wake from the deep tier: stay dark, go straight back to
  // background sampling.
  if (bgBootIsBackground()) {
    bgRunScreenOff(true);
    lastActivity = millis();
  }

  for (;;) {
    esp_task_wdt_reset();
    Event e;
    bool any = false;
    int drained = 0;
    while (drained < 8 &&
           xQueueReceive(eventQueue, &e, pdMS_TO_TICKS(any ? 0 : 30)) == pdPASS) {
      any = true; drained++;
      if (e.type == EventType::ButtonVeryLong) {
        shutdownNow();
      }
      if (e.type == EventType::TimerExpired) {
        // The timer-wake infra (apptimer.* + the expiry check above, + the
        // timer wake-up armed in enterDeepSleep) is kept and still raises
        // this event. Pixel Pet has no Timer app to show, so we only count it
        // as activity. An app that uses timerArm() should switchTo() its own
        // alarm screen here.
        lastActivity = millis();
        continue;
      }
      if (isActivity(e.type)) lastActivity = millis();
      if (currentView) currentView->onEvent(e);
    }

    // ---- Pet + step housekeeping while the screen is on (1 Hz) ----
    if (millis() - lastHousekeep >= 1000) {
      lastHousekeep = millis();
      petSvcUpdate(petTimeFromModel(), true);
      stepsSvcCheckpoint(false);
      petSvcSave(false);
      float vb; bool bok;
      { ModelLock lk; vb = model.vbat; bok = model.batOk; }
      bgCheckBattery(vb, bok);
    }

    // ---- Screen timeout / explicit "screen off" ----
    uint16_t timeoutSec;
    Screen   scr;
    { ModelLock lk;
      timeoutSec = model.sleepTimeoutSec;
      scr        = model.screen; }
    // Never auto-sleep mid-action. The power screen blocks it; add your own
    // screen to this OR-chain if it must stay lit.
    bool blockSleep = (scr == Screen::PowerOff);
    bool offNow = s_screenOffReq;
    s_screenOffReq = false;
    if (offNow || (timeoutSec > 0 && !blockSleep &&
                   (millis() - lastActivity) > (uint32_t)timeoutSec * 1000)) {
      if (bgActive()) {
        bgRunScreenOff(false);           // returns when the wearer wakes the watch
        lastActivity = millis();
        lastRender = millis();
        { ModelLock lk; lastSeenRev = model.revision; }
        continue;
      }
      savePetAndSteps();
      enterDeepSleep();                  // does not return
    }

    uint32_t rev;
    { ModelLock lk; rev = model.revision; }
    const uint16_t frameMs = currentView ? currentView->frameMs() : 0;
    const uint32_t sinceRender = millis() - lastRender;
    bool dirty = any || (rev != lastSeenRev) || (sinceRender > 1000) ||
                 (frameMs > 0 && sinceRender >= frameMs);
    if (dirty && currentView) {
      currentView->render();
      lastRender = millis();
      lastSeenRev = rev;
    }
  }
}

void controllerStartTasks() {
  // Stack sizes tuned from observed high-water marks:
  //   io:     peak ~2.2 KiB + the 192-byte FIFO buffer; 4.5 KiB leaves margin.
  //   render: view->render() recurses through Arduino_GFX + FreeFont code
  //           paths and the carousel's inline frame loop, and the background
  //           sleep loop runs here too (FIFO buffer, RTC/NVS/I2C calls) —
  //           8 KiB keeps a healthy margin.
  xTaskCreatePinnedToCore(taskIO,     "io",     4608, nullptr, 5, &ioTaskHandle, 0);
  xTaskCreatePinnedToCore(taskRender, "render", 8192, nullptr, 4, nullptr,       1);
}

// ---------- Deep sleep (stock path: background steps off) ----------
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>

void enterDeepSleep() {
  bool wkTouch, wkBtn, wkImu;
  uint16_t toOffSec;
  uint8_t  rtcH = 0, rtcM = 0, rtcS = 0, rtcDay = 1, rtcMon = 1;
  uint16_t rtcYear = 2025;
  bool     rtcOk = false;
  uint8_t  imuThs;
  { ModelLock lk;
    wkTouch  = model.wakeOnTouch;
    wkBtn    = model.wakeOnButton;
    wkImu    = model.wakeOnImu;
    toOffSec = model.sleepToOffSec;
    imuThs   = model.imuWakeThreshold;
    rtcH = model.hour; rtcM = model.minute; rtcS = model.second;
    rtcDay = model.day; rtcMon = model.month; rtcYear = model.year;
    rtcOk = model.rtcOk; }

  // Park the I/O task at the top of its loop so we own the I2C bus for the
  // pre-sleep drain. (BaseOS suspended it wherever it was, which could be
  // mid-transfer, holding the Wire lock.)
  controllerSuspendIo();

  // MMA8451 jolt wake exactly as BaseOS sets it up (TRANSIENT, high-passed
  // so gravity/tilt are ignored) on INT1 at the user's threshold; otherwise
  // the accelerometer goes to standby.
  if (wkImu) accel::configJoltWake(imuThs);
  else accel::standby();

  // ---- Drain pending interrupt sources so we don't immediately wake up ----
  // CST816S: read regs 0x01..0x06 to clear the current touch frame.
  {
    Wire.beginTransmission(I2C_ADDR_TOUCH);
    Wire.write(0x01);
    if (Wire.endTransmission(false) == 0) {
      Wire.requestFrom((int)I2C_ADDR_TOUCH, 6);
      while (Wire.available()) (void)Wire.read();
    }
  }
  accel::clearLatches();
  // Touch ISR flag: spurious if the chip raised INT during the screens we
  // just animated. Discard.
  touchPending = false;

  // Settle: wait for the user's finger to lift and the INT lines to deassert.
  vTaskDelay(pdMS_TO_TICKS(300));
  accel::clearLatches();            // a jolt during the settle must not wake us at once

  // CST816S touch INT is active-low -> ext0 with level=0.
  if (wkTouch) {
    rtc_gpio_pullup_en((gpio_num_t)PIN_TOUCH_INT);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_TOUCH_INT, 0);
  }

  // Button (active-high) and MMA INT1 (configured active-high above) -> ext1
  // with ANY_HIGH. ext0 + ext1 can both be enabled.
  uint64_t mask = 0;
  if (wkBtn) mask |= 1ULL << PIN_BTN;
  if (wkImu && accel::present()) mask |= 1ULL << PIN_MMA_INT1;
  // Never sleep with no way back. With auto power-off the button can always
  // power the watch back on afterwards; without it (or when neither touch,
  // with a controller that answered, nor the accelerometer can wake the
  // watch) the button wakes it, whatever the Sleep settings say.
  if (toOffSec == 0 || (!(wkTouch && touchPresent) && !(mask & (1ULL << PIN_MMA_INT1))))
    mask |= 1ULL << PIN_BTN;
  if (mask) esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_HIGH);

  // Timer wake-up. An armed countdown timer takes priority over the
  // auto-power-off interval: we wake exactly when it should alarm. Otherwise
  // the auto-power-off timer (sleepToOffSec) is armed as before. main() tells
  // the two apart on wake by checking timerArmed() + the RTC deadline.
  bool armedTimerWake = false;
  if (rtcOk && timerArmed()) {
    uint32_t nowEpoch = rtcEpochSec(rtcYear, rtcMon, rtcDay, rtcH, rtcM, rtcS);
    uint32_t remain   = timerRemainingSec(nowEpoch);
    if (remain == 0) remain = 1;          // fire almost immediately
    esp_sleep_enable_timer_wakeup((uint64_t)remain * 1000000ULL);
    armedTimerWake = true;
  }
  if (!armedTimerWake && toOffSec > 0) {
    esp_sleep_enable_timer_wakeup((uint64_t)toOffSec * 1000000ULL);
  }

  // Hold the LDO latch through sleep so the rail stays up (and the motor
  // enable and backlight low).
  holdPinsForDeepSleep();
  gpio_hold_en((gpio_num_t)PIN_LDO_LATCH);
  gpio_deep_sleep_hold_en();

  esp_deep_sleep_start();
  // Does not return.
}
