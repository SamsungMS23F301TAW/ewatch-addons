# EWatch_Dev bug report

**For:** Ewan Wills (and the agents working on EWatch_Dev)
**From:** Kyle Hanning
**Repo state checked:** `main` @ `dbe73c2` ("fix(ewatchos): release a leftover latch pad hold at boot")
**Date:** 2 October 2026

## How these were found

Eight AI agents each built a separate addon on a copy of **BaseOS**, and several
ran into the same problems independently; two of them also ran independent
code reviews of the boot and sleep paths. I then checked every item below
against the upstream source myself; file paths and line numbers are exact for
`dbe73c2`. Nothing was run on a watch. Items marked *needs hardware* still need a
current meter or a real watch to confirm their size.

Every app folder (`tamagotchi/`, `Tunnel_Racer/`, ...) is its own copy of
BaseOS, so **each fix has to be applied in every affected folder**. The lists
below name them all.

## Summary

| # | Severity | Issue | Folders | Confirm on hardware? |
|---|---|---|---|---|
| 1 | **High** | Crash-loop guard can never trigger | 15 | Recommended |
| 2 | **High** | Going to sleep can deadlock the I2C bus: the screen freezes for 20 s, then the watchdog reboots | 12 | Recommended |
| 3 | Medium | Deep sleep leaves the display panel and accelerometer running | All BaseOS-derived | Yes (current draw) |
| 4 | Medium | Every wake source can be switched off, leaving the watch unwakeable | 8 (+2 to check) | No |
| 5 | Low | Power-off doesn't stay off on USB: the watchdog reboots the watch | 11 | Yes |
| 6 | Low | A stuck touch-interrupt line causes an endless wake loop | 13 | Hard (needs a fault) |
| 7 | Low | The power-latch pin floats briefly on every wake | 12 | No |
| 8 | Low | Timer alarm buzz strength overflows (400 → 144) | 8 | No |
| 9 | Low | `pio run -e disptest` fails (test source missing) | 8 | No |
| 10 | Low | `taskIO` runs at ~45 Hz with jitter, not 50 Hz | All BaseOS-derived | No |
| 11 | Low | PlatformIO platform version not pinned | All | No |
| 12 | Low | Backlight turns on after a fixed 80 ms, possibly before the first frame is drawn | All BaseOS-derived | Yes (visual) |
| 13 | Docs | Stale licence note; six app READMEs are copies of BaseOS's | 7 | No |

---

## 1. Crash-loop guard can never trigger (High)

**What the code intends.** `main.cpp` counts consecutive crash or watchdog
boots. After `kPanicGiveUpLimit` (3) it drops the LDO latch, so a
crash-looping watch can't drain its battery.

**What actually happens.** The counter is declared `RTC_DATA_ATTR`. The ESP-IDF
startup code re-initialises `RTC_DATA_ATTR` variables on **every reset except a
deep-sleep wake**. Panics, task watchdog and interrupt watchdog resets are
exactly the resets the guard counts, and each one sets the counter back to 0.
It reaches 1 after every crash and never exceeds 3, so the watch can loop
forever.

The framework header in this project's own toolchain states the contract
(`framework-arduinoespressif32/tools/sdk/esp32s3/include/esp_common/include/esp_attr.h`):

```c
// Forces data into RTC slow memory. ...
// Any variable marked with this attribute will keep its value
// during a deep sleep / wake cycle.
#define RTC_DATA_ATTR ...

// Forces data into RTC slow memory of .noinit section.
// Any variable marked with this attribute will keep its value
// after restart or during a deep sleep / wake cycle.
#define RTC_NOINIT_ATTR ...
```

The comment in BaseOS (`main.cpp:89`, "a counter in RTC_DATA_ATTR memory
(survives soft reset ...)") describes `RTC_NOINIT_ATTR`, not `RTC_DATA_ATTR`.

**Affected (15 folders, 17 variables):**

| File | Line | Variable |
|---|---|---|
| `BaseOS/src/main.cpp` | 98 | `gConsecutivePanics` |
| `Tunnel_Racer/src/main.cpp` | 98 | `gConsecutivePanics` |
| `Starfox_shooter/src/main.cpp` | 98 | `gConsecutivePanics` |
| `3D_Watchface/src/main.cpp` | 98 | `gConsecutivePanics` |
| `tamagotchi/src/main.cpp` | 98 | `gConsecutivePanics` |
| `FirstOS/src/main.cpp` | 98 | `gConsecutivePanics` |
| `ScrubMarine/src/main.cpp` | 98 | `gConsecutivePanics` |
| `Spotify_Controller/src/main.cpp` | 99 | `gConsecutivePanics` |
| `BasicDigital/src/main.cpp` | 100 | `gConsecutivePanics` |
| `BasicAnalog/src/main.cpp` | 100 | `gConsecutivePanics` |
| `3D_Racer/src/main.cpp` | 96 | `gConsecutivePanics` |
| `Doom/src/main.cpp` | 54 | `gConsecutivePanics` |
| `EWatchOS2.1/src/main.cpp` | 105, 108 | `gConsecutivePanics`, `gSafeModeTried` |
| `FirstOS Fable/src/main.cpp` | 100, 103 | `gConsecutivePanics`, `gSafeModeTried` |
| `ClaudeFace/firmware/src/main.cpp` | 36 | `gCrashBoots` |

In EWatchOS2.1 and FirstOS Fable, `gSafeModeTried` has the same problem, so the
"try safe mode once, then power off" escalation never remembers that safe mode
was tried.

**Fix.** Move the variables to `RTC_NOINIT_ATTR`. That memory holds random
values after a cold power-on, so guard it with a magic word. For BaseOS:

```cpp
// RTC_NOINIT survives panic/WDT resets (RTC_DATA_ATTR does not; the startup
// code re-initialises it on every reset except a deep-sleep wake). Its
// contents are random after power-on, so a magic word marks them valid.
RTC_NOINIT_ATTR static uint32_t gConsecutivePanics;
RTC_NOINIT_ATTR static uint32_t gPanicMagic;
static const uint32_t kPanicMagic = 0x50414E43u;   // "PANC"

static void checkPanicLoopAndMaybeShutdown(esp_reset_reason_t r) {
  if (gPanicMagic != kPanicMagic) {     // first boot after power-on
    gPanicMagic = kPanicMagic;
    gConsecutivePanics = 0;
  }
  // ... the rest of the function is unchanged ...
}
```

For EWatchOS2.1 and FirstOS Fable, also move `gSafeModeTried` to
`RTC_NOINIT_ATTR` and zero it in the same magic-word block. Also fix the
comment at `main.cpp:89`. The stable-run timer that clears the counter after
`kStableSec` needs no change.

**Verify.**
- Without hardware: build and confirm the variables now sit in the
  `.rtc_noinit` section (`xtensa-esp32s3-elf-nm -S` on `firmware.elf`, or the
  map file).
- On a watch: add a temporary debug trigger (for example a serial command that
  calls `abort()`, which gives reset reason `ESP_RST_PANIC`). Fire it on each
  boot; the serial log should count 1, 2, 3, and the 4th crash boot should
  drop the latch. Remove the trigger afterwards.

---

## 2. Going to sleep can deadlock the I2C bus (High)

**What happens.** `enterDeepSleep()` stops the I/O task from the outside, then
uses `Wire` itself to drain the touch and accelerometer interrupts:

```cpp
// Stop the I/O task so we own the I2C bus for the pre-sleep drain.
if (ioTaskHandle) vTaskSuspend(ioTaskHandle);
vTaskDelay(pdMS_TO_TICKS(40));    // let any in-flight Wire xfer finish
```

`taskIO` runs on core 0 and the render task, which calls this, runs on core
1. The suspend lands at whatever point `taskIO` happens to be. In
Arduino-ESP32 2.0.17, `TwoWire::beginTransmission()` takes the bus mutex with
`portMAX_DELAY`. Only `endTransmission(true)` releases it, or `requestFrom()`
after an `endTransmission(false)` repeated start, which every register read
here uses. Check `framework-arduinoespressif32/libraries/Wire/src/Wire.cpp`,
lines 408–470.

If `taskIO` is suspended inside that window, the mutex stays held for good.
The 40 ms delay can't help, because a suspended task never runs. The next
`Wire.beginTransmission()` in `enterDeepSleep()` then blocks forever. The
render task stops feeding the watchdog, the screen stays lit and frozen
(nothing has turned the backlight off yet), and after `kTaskWdtSec` (20 s) the
chip resets.

**How often.** `taskIO` spends roughly 1 ms of each ~21 ms cycle inside `Wire`
transactions (touch poll, accelerometer read, and the RTC every 4th cycle). We
estimate around 5% of sleep entries are exposed. That's an estimate, not a
measurement; on a watch it would look like "occasionally freezes for 20 s when
going to sleep, then reboots". Bug #1 means these reboots are never caught by
the crash guard either.

**Affected (12 folders):**

| File | Line |
|---|---|
| `BaseOS/src/core/controller.cpp` | 499 |
| `3D_Watchface/src/core/controller.cpp` | 499 |
| `Tunnel_Racer/src/core/controller.cpp` | 499 |
| `tamagotchi/src/core/controller.cpp` | 499 |
| `FirstOS/src/core/controller.cpp` | 497 |
| `Starfox_shooter/src/core/controller.cpp` | 502 |
| `ScrubMarine/src/core/controller.cpp` | 509 |
| `Spotify_Controller/src/core/controller.cpp` | 509 |
| `BasicAnalog/src/core/controller.cpp` | 456 |
| `BasicDigital/src/core/controller.cpp` | 456 |
| `FirstOS Fable/src/core/controller.cpp` | 654 |
| `EWatchOS2.1/src/core/controller.cpp` | 668 |

**Fix.** EWatchOS2.1 already has the right mechanism for light sleep:
`controllerSuspendIo()` sets `gIoPauseReq`. `taskIO` parks itself at the top
of its loop, which is always between transactions, and the caller waits until
it's suspended (`EWatchOS2.1/src/core/controller.cpp`, lines 220–262 and
581–596). Its deep-sleep path just doesn't use it.

- **EWatchOS2.1 and FirstOS Fable:** replace the `vTaskSuspend(ioTaskHandle)`
  + `vTaskDelay(40)` pair in `enterDeepSleep()` with `controllerSuspendIo();`
  (FirstOS Fable likely has the same helper; if not, port it as for BaseOS).
- **BaseOS and its copies:** port `gIoPauseReq`, the loop-top pause point
  (remove the task from the watchdog before `vTaskSuspend(nullptr)` and re-add
  it after) and `controllerSuspendIo()` / `controllerResumeIo()` from
  EWatchOS2.1. Then use `controllerSuspendIo()` in `enterDeepSleep()`.
- Keep the rule that nothing other than a self-suspend ever stops `taskIO`.

**Verify.** On a watch, put the sleep timeout at its minimum and let the watch
sleep and wake 100+ times. Before the fix, expect occasional 20 s freezes
followed by a reboot (the serial log shows a task watchdog reset). After the
fix there should be none.

---

## 3. Deep sleep leaves the display panel and accelerometer running (Medium, *needs hardware*)

**What happens.** `enterDeepSleep()` (`BaseOS/src/core/controller.cpp`)
suspends `taskIO`, drains interrupts, arms wake sources and sleeps. It never:

- turns the backlight off, or puts the ST7789 into sleep-in (SLPIN). The panel
  rail stays latched, so the controller keeps running;
- puts the MMA8451 into standby. It stays active at **800 Hz** (`CTRL_REG1 =
  0x01` from `mmaActivate()` at boot) when wake-on-motion is off. When it's on,
  `configureMmaForJoltWake()` re-enables it, again at 800 Hz.

**Why it matters.** With stock defaults, deep sleep only lasts 30 s before
power-off, so the waste is small. With `sleepToOffSec = 0` ("sleep forever"),
or any app that stays in deep sleep for hours (alarms, background features),
these parts can dominate sleep current. The MMA8451 datasheet puts 800 Hz
normal mode around 165 µA against about 2 µA in standby, and an awake panel
controller draws far more than in sleep-in. The ESP32-S3 itself sleeps at
around 10 µA. **Measure before and after**; the exact numbers depend on the
board.

**Fix (sketch).** In `enterDeepSleep()`, after the I/O task is suspended:

```cpp
backlightOff();
if (gfx) gfx->displayOff();   // Arduino_ST7789::displayOff() sends SLPIN (verified in GFX 1.4.7)
                              // waking is a full reboot, and displayBegin() re-inits the panel
if (!wkImu && mmaAddr) {      // accelerometer isn't a wake source: put it in standby
  Wire.beginTransmission(mmaAddr);
  Wire.write(0x2A); Wire.write(0x00);   // CTRL_REG1: standby
  Wire.endTransmission();
}
```

- For wake-on-motion, run the transient detector at a lower data rate, for
  example 50 Hz in low-power mode as EWatchOS2.1's `controller.cpp` already
  does (`CTRL_REG2 = 0x03`, `CTRL_REG1 = 0x21`). `TRANSIENT_COUNT` is in
  samples, so re-tune it and the threshold.
- Apply the same to `reArmFromSpuriousWakeAndSleep()` in `main.cpp`.
- Also check that the backlight pin can't float high during deep sleep: drive
  it low and `gpio_hold_en()` it, or confirm there's a pull-down on the board.

Two of Kyle's addons already do this (Friend Radar puts the panel in SLPIN; Meeting
Countdown also puts the accelerometer in standby) and can serve as references.

---

## 4. Every wake source can be switched off, leaving the watch unwakeable (Medium)

**What happens.** Settings → Sleep toggles touch, button and motion wake
independently with no "at least one" rule (`BaseOS/src/apps/system/view.cpp`,
around lines 1628–1644). Auto power-off can also be set to "never"
(`toOffSec == 0`, around lines 1704 and 1742). `enterDeepSleep()` arms ext0
only for touch, ext1 only for the button or motion, and the timer only when
`toOffSec > 0` or an app timer is armed.

With all three wake sources off and power-off set to "never", the chip enters
deep sleep with **no wake source at all**. The LDO latch is held through sleep,
so the rail stays up, and SW2 isn't armed, so pressing it does nothing. The
watch can't be woken and sits in deep sleep until the battery is flat.

**Affected:** the same Sleep page in `BaseOS`, `3D_Watchface`, `FirstOS`,
`ScrubMarine`, `Spotify_Controller`, `Starfox_shooter`, `Tunnel_Racer` and
`tamagotchi` (`src/apps/system/view.cpp`). EWatchOS2.1 and FirstOS Fable have
the same toggles in `src/apps/system/settings_system.cpp`; check their sleep
paths too.

**Fix:** always arm SW2 as an ext1 wake source in `enterDeepSleep()` and
`reArmFromSpuriousWakeAndSleep()`, whatever the settings say. Optionally also
stop the UI from turning off the last remaining wake source.

---

## 5. Power-off doesn't stay off on USB: the watchdog reboots the watch (Low)

**What happens.** `shutdownNow()` runs on the render task, which is subscribed
to the task watchdog. It calls `unlatchPower()`, then spins in
`for (;;) vTaskDelay(1000)` without feeding the watchdog or removing itself
from it. On battery the rail drops and that's fine. But if anything keeps the
rail up, the watchdog fires after 20 s, the chip resets, and the watch boots
straight back on. That includes USB power, and possibly SW2 still being held
from the 3-second power-off hold, depending on the power circuit.

**Affected (11 folders, `src/core/controller.cpp`):**

| Folder | Line |
|---|---|
| `BaseOS` | 352 |
| `3D_Watchface` | 352 |
| `ScrubMarine` | 352 |
| `Starfox_shooter` | 352 |
| `Tunnel_Racer` | 352 |
| `tamagotchi` | 352 |
| `FirstOS` | 353 |
| `Spotify_Controller` | 357 |
| `3D_Racer` | 193 |
| `FirstOS Fable` | 483 |
| `EWatchOS2.1` | 491 |

**Fix:** ClaudeFace already does this correctly
(`ClaudeFace/firmware/src/main.cpp`, `powerOffNow()`):
1. Unlatch and wait about 300 ms.
2. If the code is still running, the board is externally powered. Call
   `esp_task_wdt_delete(nullptr)` before waiting, and ideally go into deep
   sleep with only SW2 as a wake source, so the watch looks and behaves as off.

---

## 6. A stuck touch-interrupt line causes an endless wake loop (Low)

**What happens.** On a touch (ext0) wake, `setup()` checks
`touchReallyPresent()`. If no finger is present, it calls
`reArmFromSpuriousWakeAndSleep()` (`BaseOS/src/main.cpp:217`), which re-arms
the same wake sources and sleeps again, with no limit.

If the CST816S interrupt line is stuck low (a wedged controller, moisture or
EMI), ext0 fires immediately every time. The watch cycles boot → check → sleep
forever. Because the power-off timer is re-armed each cycle, auto power-off
never happens either, and the battery drains at close to active current.

**Affected:** `reArmFromSpuriousWakeAndSleep()` exists in 13 folders: `BaseOS`,
`3D_Racer`, `3D_Watchface`, `BasicAnalog`, `BasicDigital`, `EWatchOS2.1`,
`FirstOS`, `FirstOS Fable`, `ScrubMarine`, `Spotify_Controller`,
`Starfox_shooter`, `Tunnel_Racer` and `tamagotchi` (all in `src/main.cpp`).

**Fix:** count consecutive rejected touch wakes in an `RTC_DATA_ATTR`
variable. That's fine here, since these are deep-sleep wakes, unlike bug #1.
After about 5, sleep with touch wake disabled (button only) until a real wake
happens, and reset the count on any accepted wake.

---

## 7. The power-latch pin floats briefly on every wake (Low)

**What happens.** On a deep-sleep wake, `setup()` calls
`gpio_hold_dis(PIN_LDO_LATCH)` **before** `latchPower()` drives the pin HIGH
(`BaseOS/src/main.cpp:259`). After the wake reset, the pin's configuration is
back to its default, so releasing the hold first leaves the latch undriven for
a few microseconds. Whether that can ever drop the rail depends on the enable
circuit's pull-down and time constant.

EWatchOS2.1 has already fixed this in `dbe73c2`: `latchPower()` runs first,
then the hold is released on every boot. The comment there explains why.

**Affected (12 folders, `src/main.cpp`):**

| Folder | Line |
|---|---|
| `BaseOS` | 259 |
| `3D_Watchface` | 259 |
| `FirstOS` | 259 |
| `ScrubMarine` | 259 |
| `Starfox_shooter` | 259 |
| `Tunnel_Racer` | 259 |
| `tamagotchi` | 259 |
| `Spotify_Controller` | 260 |
| `3D_Racer` | 257 |
| `BasicAnalog` | 263 |
| `BasicDigital` | 263 |
| `FirstOS Fable` | 292 |

**Fix:** port the EWatchOS2.1 ordering: drive the latch HIGH first, then
release the hold.

---

## 8. Timer alarm buzz strength overflows: 400 → 144 (Low)

`hapticBuzz(uint8_t intensity, uint16_t duration_ms)` takes 0–255. The timer
expiry path calls `hapticBuzz(400, 220)` with the comment "strong initial alarm
buzz". 400 truncates to 144 (400 − 256), about 56% strength, which is the
opposite of what's intended. GCC also warns about it (`-Woverflow`).

**Affected:**
- `BaseOS/src/core/controller.cpp:339`
- `Tunnel_Racer/src/core/controller.cpp:339`
- `Starfox_shooter/src/core/controller.cpp:339`
- `3D_Watchface/src/core/controller.cpp:339`
- `tamagotchi/src/core/controller.cpp:339`
- `ScrubMarine/src/core/controller.cpp:339`
- `FirstOS/src/core/controller.cpp:340`
- `Spotify_Controller/src/core/controller.cpp:344`

**Fix:** `hapticBuzz(255, 220);`

---

## 9. `pio run -e disptest` fails: test source missing (Low)

`platformio.ini` defines `[env:disptest]` to build
`src/test/disp_touch_test.cpp`, and BaseOS's `README.md` and `CLAUDE.md`
document it. The file only exists in `FirstOS/`. In every folder below the env
has no sources, so it can't build:

`BaseOS`, `3D_Watchface`, `Doom`, `ScrubMarine`, `Spotify_Controller`,
`Starfox_shooter`, `Tunnel_Racer`, `tamagotchi`

**Fix:** copy `FirstOS/src/test/disp_touch_test.cpp` into each folder's
`src/test/`. The `ewatch` env already excludes `test/` via `build_src_filter`,
so this is safe. Alternatively, delete the env and its documentation.

---

## 10. `taskIO` runs at ~45 Hz with jitter, not 50 Hz (Low)

`taskIO` ends each cycle with `vTaskDelay(pdMS_TO_TICKS(20))`
(`BaseOS/src/core/controller.cpp:346`). The delay starts **after** the touch,
accelerometer, RTC and battery work, so the real period is 20 ms plus the work
time: about 45 Hz, varying cycle to cycle. Anything that assumes evenly spaced
samples (shake heuristics, step or rep detection, motion filters) sees a
skewed rate.

**Fix:**

```cpp
TickType_t last = xTaskGetTickCount();
for (;;) {
  // ... existing body ...
  vTaskDelayUntil(&last, pdMS_TO_TICKS(20));
}
```

For exact timing, use the MMA8451's FIFO at a fixed data rate instead.

---

## 11. PlatformIO platform version not pinned (Low)

`platformio.ini` uses `platform = espressif32` with no version. The code
depends on Arduino-ESP32 2.x (the file's own comment says so, and GFX is pinned
below 1.5 because 1.5+ needs 3.x headers). If the platform's default ever moves
to Arduino 3.x, every project stops building.

Today this resolves to **Espressif 32 platform 7.1.3**, Arduino-ESP32 2.0.17
(`framework-arduinoespressif32 @ 4.20017.x`). Stock BaseOS builds cleanly
against it.

**Fix:** `platform = espressif32 @ 7.1.3` in each `platformio.ini`.

---

## 12. Backlight turns on after a fixed 80 ms (Low, *needs hardware*)

`setup()` launches the tasks, waits `delay(80)`, then calls
`backlightSet(model.brightness)` (`BaseOS/src/main.cpp:346-347`). Every wake
from deep sleep is a full reboot. If the first frame takes longer than 80 ms
(heavier faces and apps), the user sees a black or half-drawn screen as the
backlight comes on.

**Suggested fix:** have the render task signal "first frame flushed" and wait
for it, with a cap of about 1 s, before turning the backlight on. Kyle's Tilt
Parallax addon does this.

---

## 13. Documentation (Docs)

- `BaseOS/README.md:81-84` says "No license file is currently included". The
  repo root has had an MIT `LICENSE` since `12d8e77`. Update the section and
  point it to the root licence.
- These READMEs are byte-for-byte copies of `BaseOS/README.md`. They describe
  "no user apps are installed" instead of the app in the folder:
  - `3D_Watchface/README.md`
  - `ScrubMarine/README.md`
  - `Spotify_Controller/README.md`
  - `Starfox_shooter/README.md`
  - `Tunnel_Racer/README.md`
  - `tamagotchi/README.md`

---

## Suggested order of work

1. **#1 crash guard** and **#2 sleep deadlock**. Both are high impact and
   small changes, and #2's fix already exists in EWatchOS2.1.
2. **#4 wake sources**, **#8 haptic overflow** and **#11 platform pin** (all
   one-to-three-line fixes).
3. **#5 power-off on USB**, **#6 wake loop** and **#7 latch order**. ClaudeFace
   and EWatchOS2.1 already contain the fixes for #5 and #7.
4. **#9 disptest**, then the **#13 docs**.
5. **#3 sleep power**: measure stock sleep current first, apply the fix,
   measure again.
6. **#10** and **#12**, when convenient.

Fix BaseOS first and verify it, then copy the same changes into the other app
folders; they all share BaseOS's `main.cpp` and `controller.cpp`.
