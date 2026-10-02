# Stepmunk (EWatch addon `pixel-pet`) — read this first

A Tamagotchi-style pixel pet that only eats when you walk, built on EWatch
BaseOS @ dbe73c2. The BaseOS app-building guide follows below this section;
everything here is what this addon adds or changes. User-facing docs:
`README.md`.

## Architecture

```
            ┌────────── screen ON ──────────┐        ┌──────── screen OFF (bgpower) ────────┐
MMA8451 ──> taskIO drains FIFO every ~160 ms │        │ render task: light-sleep loop        │
  12.5 Hz   (core 0, owns I2C)               │  or    │ INT2 watermark wake every ~1.6 s     │
  FIFO      └─> stepsSvcFeed() ──────────────┼──>  steps::Detector + StepBook (steps.cpp) │
                                             │        │   └─> petSvcUpdate() (+haptics)      │
taskRender (core 1): 1 Hz petSvcUpdate,      │        │ still 12 min -> deep tier (INT1      │
  views render (PetFaceView / PetAppView)    │        │ motion wake boots dark, back here)   │
```

* **Pure, host-tested cores** (no Arduino): `src/core/steps.{h,cpp}` (detector
  + day book), `src/apps/pet/petmodel.{h,cpp}` (rules), `px.*` (renderer +
  fonts), `petart.*` (sprites), `scenes.*` (layouts). Keep them free of
  Arduino/FreeRTOS: `pio test -e native` and `tools/preview/build.sh` compile
  them with the host compiler.
* **Device glue**: `steps_service.cpp` (mutex, RTC-memory mirror, NVS
  checkpoints, PSRAM recorder), `petsvc.*` (pet persistence, options, haptic
  vocabulary, UI event queue), `accel.*` (MMA8451 FIFO / motion-wake
  registers), `bgpower.*` (screen-off light-sleep sampler, deep tier,
  background boots, low-battery cutoff), `addonstore.*` (NVS namespace
  `pixelpet`), `console.*` (serial commands), `petviews.*` (the two views).

## Files

| File | Role |
|---|---|
| `src/core/steps.h/.cpp` | Detector (magnitude, gravity EMA, 3 Hz Butterworth, adaptive peaks, regularity gate) + StepBook (today/lifetime/30-day history, midnight rollover). Pure. |
| `src/core/steps_service.cpp` | `stepsSvc*()`: thread-safe service, RTC + NVS persistence, `rec`/`dump` recorder. |
| `src/core/accel.h/.cpp` | MMA8451: 12.5 Hz FIFO (watermark 20 → INT2), TRANSIENT jolt (INT1), deep-sleep motion wake. Bus owner only. |
| `src/core/bgpower.h/.cpp` | Screen-off loop, wake classification, INT2/touch fault fallbacks, deep tier (settled INT1 check, phantom-touch re-sleep to the planned wake), boot classification + boot-press latch, crash safe mode, low battery. |
| `src/core/controller.cpp` | taskIO now drains the FIFO (no more `readAccel`/`ImuMotion`), cooperative pause + wake guard; taskRender routes screen-off to bgpower, 1 Hz pet housekeeping, `View::frameMs()` pacing. |
| `src/main.cpp` | Deep-tier wakes don't power off; background boots stay dark/silent with a button-press latch; panic counter in `RTC_NOINIT_ATTR`; latch-then-unhold on wake; services init before `viewsInit()`; console in `loop()`. |
| `src/apps/pet/petmodel.*` | Economy (400 steps/snack, 8.5 pts, drain 7/h day 1.5/h night, bowl 3, bedtime feast), moods, sulk, evolution, streaks, nudges, exact time replay. Pure. |
| `src/apps/pet/petart.*` | Procedural body + ASCII stamps (compile-time size checked), per-mood idle loops and one-shot acts. Pure. |
| `src/apps/pet/scenes.*` | Watch face bands (top/pet/stats) and the four app pages. Pure. |
| `src/apps/pet/px.*` | Canvas primitives, dithering, sprite blits, original 5x7 + 6x9 pixel fonts (icons on `^ ~ @ # $ & { } | \``). Pure. |
| `src/apps/pet/petsvc.*` | Pet service on the watch. |
| `src/apps/pet/petviews.*` | `PetFaceView` (default `Screen::Watch`), `PetAppView` (`Screen::PetApp`), launcher tile art. |
| `test/test_steps`, `test/test_pet`, `test/test_render` | Unity tests (synthetic signals; simulated days; canary-guarded render safety). |
| `tools/preview/*` | Host renderer → PNG sheets in `docs/`, generates `icon.svg`. |

## Extension points

* **Tuning the game**: `pet::Tuning` in `petmodel.h` (steps per snack, drain
  rates, night window, thresholds, nudge rules). Re-run `pio test -e native`;
  the simulated-day tests encode the design targets (6–8k keeps it happy, a
  lazy day is grumpy by evening).
* **Tuning the detector**: `steps::DetectorConfig`. Capture real data with the
  console (`rec 300`, walk, `dump`) and add it as a test case.
* **New art**: add stamps with `DEF_STAMP(name, w, h, "...")` (sizes are
  checked at compile time), compose them in a `compose*()` function, then look
  at `tools/preview/build.sh .scratch/prev` output with an image viewer.
* **New pet reactions**: add a `PetUi` kind in `petsvc.h`, push it in
  `petSvcUpdate()`, handle it in `PetStageAnim::consumeServiceEvents()`.
* **Power**: flags in `platformio.ini` (`PIXELPET_BG_STEPS`, `_DEEP_TIER`,
  `_STILL_MIN`, `_PANEL_SLPIN`, `_LOWBAT_CUTOFF`, `_CONSOLE`).

## Rules this addon adds

* Only `accel::drainFifo()` may read the MMA8451 data registers (with the FIFO
  on, reading OUT_X pops samples).
* While the screen is off, the render task owns the I2C bus (taskIO is
  parked). Never call I2C from views; the sleep loop is the exception.
* `taskIO` bumps `model.revision` only on real changes. Animated views return
  a non-zero `frameMs()` to get frames.
* Persist addon data only through `addonstore` (namespace `pixelpet`); never
  write new keys to `ewatch`.
* Never sleep while `hapticBusy()`; the PWM would freeze on.
* Bring-up safety: two crashes in a row inside the dark loop put the watch in
  background-steps safe mode until a power cycle (`bgNoteResetReason()` /
  `bgSafeMode()`).
* RTC memory: `RTC_DATA_ATTR` is re-initialised by the bootloader on every
  reset except a deep-sleep wake. Anything that must survive a crash (crash
  counters, the step and pet mirrors) is `RTC_NOINIT_ATTR` with a magic word
  or checksum, and must stay trivially constructible (`static_assert`s guard
  the mirrors). Keep `RTC_DATA_ATTR` for "did we put it to sleep" flags.
* No CPU interrupt may stay enabled on a pin while it is armed as a
  level-triggered light-sleep wake (interrupt storm): `sleepOnce()` masks the
  touch ISR around the sleep and puts the other wake pins back to "no
  interrupt" afterwards, and the boot-press latch on the button is detached
  before the dark loop starts.
* The MMA8451's high-pass filter restarts on every standby -> active switch.
  After reconfiguring, INT1 (TRANSIENT) only means motion once it has stayed
  quiet for a few samples: `enterDeepTier()` and `settleJolt()` wait for that
  (watching the button meanwhile) instead of using a fixed delay.
* Anything that powers off goes through `powerOffNow()`; anything that deep
  sleeps calls `holdPinsForDeepSleep()` last (setup() releases the holds).

## Differences from stock BaseOS (the guide below predates them)

* WiFi compiled out (`EWATCH_ENABLE_WIFI=0`); the `disptest` env was removed
  (its source file isn't in BaseOS); `default_envs = ewatch`; `env:native`
  added for tests.
* `EventType::ImuMotion` is no longer generated (walking would have kept the
  screen on); the IMU Gestures diagnostic was removed.
* Screen timeout → light-sleep sampler (no reboot, no auto power-off) when
  "Steps asleep" is on; `enterDeepSleep()` remains the path when it's off.
* Settings → "Font" is now "Face" and has a Stepmunk cell; the stock face also
  opens the launcher on swipe-up; the stock face's power icon and Power Off →
  Sleep go through `requestScreenOff()`.
* Haptics: `hapticPattern()` sequencer + `hapticBusy()`; display:
  `displayFlushRows()`, `panelSleep()/panelWake()/panelDispOn()`,
  `backlightFadeTo()`; touch: INT pull-up + `touchReadEventDirect()`.
* Boot/sleep fixes (README "Fixes to BaseOS"): the panic counter is
  `RTC_NOINIT_ATTR` with a check word (it never survived a panic before); on
  every boot the LDO latch is driven before its hold is released, ext1 pins
  are un-held, and the motor/backlight sleep holds are released;
  `enterDeepSleep()` parks taskIO with `controllerSuspendIo()` instead of
  `vTaskSuspend()`, keeps BaseOS's jolt setup (`accel::configJoltWake()`),
  puts the accelerometer in standby otherwise, holds the quiet pins and
  always keeps a wake source; rejected stock touch wakes back off after 6;
  power-off (and auto power-off) deep-sleeps until SW2 if USB keeps the rail
  up; the countdown alarm buzz is 255 (was 400 in a `uint8_t`).
* taskIO reads the RTC before draining the FIFO (same cycle), so steps are
  never booked to a stale day; after any wake guard, gestures are muted for
  ~0.4 s so the swipe that woke the watch isn't delivered.

---

# EWatch BaseOS — app-building guide

This is the **bare-bones EWatch slate**: every driver, core subsystem, and the
full system shell (watch face, app launcher, settings, diagnostics, power
management, sleep/wake) is present and working, but **no user apps are
installed**. The app launcher shows only `System`.

The purpose of this file is to let you (a future Claude Code session) add a new
app **first try**. Read the "Adding an app" recipe, copy the pattern, and reuse
the primitives documented below. Don't reinvent drawing, input, persistence, or
power handling — it already exists.

---

## 0. Golden rule: keep only what your app needs

This slate keeps **all** subsystems available so any app is buildable:
animation, timer-wake, WiFi/web service, diagnostics. Most apps need a small
subset. When you build a specific app:

- **Comment out or delete the subsystems your app doesn't use** so the firmware
  stays small and boot stays fast. Each subsystem below notes how to drop it.
- Strip the diagnostics screens (`Sensor Test`, `Touch Gestures`, `IMU
  Gestures`) from `kSystemApps[]` if the app doesn't need them.
- Flip `-DEWATCH_ENABLE_WIFI=0` in `platformio.ini` if there's no networking —
  that alone drops WiFi/WebServer/DNSServer and a chunk of flash.

Leave the **drivers** (display, touch, haptic, i2c, power, pins) and the **core
loop** (model, event, view, controller, storage) — those are the platform.

---

## 1. Hardware (fixed — do not assume otherwise)

| Item | Value |
|---|---|
| MCU | ESP32-S3FH4R2 — 4 MB flash, 2 MB QSPI PSRAM, native USB |
| Display | ST7789 240×280 IPS, RGB565, FSPI @ 60 MHz, col offset 0 / row offset 20 |
| Touch | CST816S capacitive, shared I2C @ 400 kHz, addr `0x15` |
| IMU | MMA8451 accelerometer (`0x1C`/`0x1D`) |
| RTC | RV-3028 (`0x52`) |
| Haptic | DRV2603 motor driver |
| Power | soft-latch LDO (no hard switch) + SW2 pushbutton |

Screen is **240 wide × 280 tall**. Pin map is `src/drivers/pins.h`; the
human-readable version is `PINOUT.md`. **Never** hardcode a pin — include
`pins.h` and use the `PIN_*` macros.

---

## 2. Build & flash

PlatformIO. `pio` may not be on PATH — use the full path:

```sh
~/.platformio/penv/bin/pio run                       # build (env:ewatch)
~/.platformio/penv/bin/pio run -t upload -t monitor  # flash + serial @115200
~/.platformio/penv/bin/pio run -e disptest -t upload -t monitor  # display+touch bring-up test
```

The `disptest` env compiles only `src/test/disp_touch_test.cpp` (its own
`setup()`/`loop()`) — a hardware sanity check that links none of the app code.

---

## 3. Architecture in one breath

`src/main.cpp` does boot only: latch power, crash-loop guard, hardware
bring-up, then launches FreeRTOS tasks. All runtime logic lives in two tasks:

- **`taskIO`** (core 0, 50 Hz) — owns the I2C bus. Reads RTC/IMU/battery,
  polls touch + button, posts `Event`s to a queue, drains cross-task RTC
  writes. **It is the only task allowed to touch `Wire`.**
- **`taskRender`** (core 1) — drains the event queue into the active `View`,
  re-renders when the model changed, and enforces auto-sleep. **It is the only
  task that paints the display.**

State flows one way: `taskIO` mutates the shared **`model`** (under a mutex) and
posts events → `taskRender` feeds events to `currentView` → the view reads the
model and draws. Views switch screens with `switchTo(Screen)`.

```
hardware → taskIO → [model + eventQueue] → taskRender → currentView.render()/onEvent()
```

---

## 4. Adding an app — the recipe

An app is a `View` subclass plus three registrations. Concretely, to add an app
called "Steps":

**Step 1 — write the view.** Create `src/apps/steps.h` + `src/apps/steps.cpp`:

```cpp
// src/apps/steps.h
#pragma once
#include "view.h"

class StepsView : public View {
public:
  void onEnter() override;                 // called once when the screen opens
  void render()  override;                 // called when the model is dirty
  void onEvent(const Event &e) override;   // touch / button / gesture / tick
  void onExit()  override {}               // optional cleanup
private:
  bool firstDraw = true;
};
```

```cpp
// src/apps/steps.cpp
#include <Arduino_GFX_Library.h>
#include "steps.h"
#include "display.h"   // gfx, frameCanvas, backlight
#include "haptic.h"    // hapticBuzz
#include "model.h"     // model + ModelLock

void StepsView::onEnter() {
  if (gfx) { ThemeColors t = theme(); gfx->fillScreen(t.bg); }
  firstDraw = true;
}

void StepsView::render() {
  if (!gfx) return;
  if (firstDraw) { drawTitleBar("Steps"); firstDraw = false; }
  // ... draw only what changed; render() is called frequently.
}

void StepsView::onEvent(const Event &e) {
  // Back affordance: both the hardware button and a right-swipe arrive as
  // ButtonShort. Always handle it so the user can leave.
  if (e.type == EventType::ButtonShort) { switchTo(Screen::AppList); return; }
  if (e.type == EventType::Touch && tappedBack(e.x, e.y)) {
    switchTo(Screen::AppList); return;
  }
  // ... your taps / gestures here.
}
```

**Step 2 — add the screen id.** In `src/core/model.h`, add an enumerator to the
`Screen` enum in the "user apps" region:

```cpp
  SensorTest, TouchGestures, ImuGestures,
  Steps,                       // <- your app
  PowerOff
```

**Step 3 — wire it into the registry.** In `src/apps/system/view.cpp`:

1. `#include "steps.h"` in the "user app headers" block near the top.
2. Add an instance with the other view instances: `static StepsView vSteps;`
3. Add a `viewFor()` case: `case Screen::Steps: return &vSteps;`
4. Add a launcher tile to `kTopApps[]` (above `System`):
   `{ "Steps", 0, Screen::Steps },`

That's it. Build, flash, swipe up from the watch face → your tile is there.

> The four edit sites in `view.cpp` and the enum each carry a `// <- add ...`
> marker comment showing exactly where to insert.

---

## 5. The `View` contract (`src/core/view.h`)

```cpp
class View {
  virtual void onEnter();                 // screen became active (set up, clear bg)
  virtual void onExit();                  // leaving — release the frame canvas, etc.
  virtual void render() = 0;              // paint; called when model.revision changed
  virtual void onEvent(const Event &);    // one input event
};
```

- `render()` is called when the model is dirty (any input, any sensor change)
  and at least once per second. **Redraw only changed regions** — full-screen
  fills every frame flicker and waste CPU. Track "last shown" values like the
  diagnostics views do.
- `onEvent()` runs on the render task too, so it's safe to draw from it.
- To leave a screen, call `switchTo(Screen::AppList)` (or `Screen::Watch`).
  `switchTo` calls the old view's `onExit()` then the new view's `onEnter()`.

---

## 6. Drawing

The display object is the global `Arduino_GFX *gfx` (from `display.h`). It's the
Arduino_GFX API: `fillScreen`, `fillRect`, `drawRect`, `fillRoundRect`,
`fillCircle`, `drawLine`, `setCursor`, `setTextSize`, `setTextColor(fg,bg)`,
`print`, `setFont`, `drawRGBBitmap`, etc. **Always null-check `gfx`** — it's
`nullptr` until `displayBegin()` succeeds.

**Colors are RGB565 `uint16_t`.** Named constants from Arduino_GFX exist
(`BLACK WHITE RED GREEN BLUE NAVY MAROON DARKGREY` …). Don't hardcode a palette
— read the user's theme:

```cpp
ThemeColors t = theme();   // { bg, fg, accent, line }
gfx->fillScreen(t.bg);
gfx->setTextColor(t.fg, t.bg);
uint16_t txt = contrastFor(t.accent);   // WHITE or BLACK, whichever reads better
```

Shared chrome helpers (declared in `view.h`, defined in `view.cpp`):

- `drawTitleBar("Title")` — title text + underline at the top.
- `drawBackButton()` / `bool tappedBack(x, y)` — top-left chevron + hit test.
- `theme()`, `contrastFor(bg)` — re-read every `render()` so a Settings → Display
  palette change repaints your app automatically.

**Double-buffering.** For full-screen animation, get the shared canvas instead
of drawing straight to `gfx`:

```cpp
Arduino_Canvas *cv = frameCanvas();   // lazy 240×280 buffer in PSRAM (~134 KiB)
if (cv) { /* draw into cv ... */ cv->flush(); }   // flush blits to the panel
```

Only one view is active at a time, so the single shared canvas is fine. Returns
`nullptr` if PSRAM is exhausted — handle it.

**Fonts.** GFX FreeFonts live in `src/apps/assets/fonts/` (Sans/Serif/Mono,
Bold, 12pt + 24pt) and are on the include path. `gfx->setFont(&FreeSans24pt7b)`;
`gfx->setFont(nullptr)` returns to the built-in 6×8 bitmap font that
`setTextSize(n)` scales.

---

## 7. Input — `Event` (`src/core/event.h`)

Events arrive via `onEvent(const Event &e)`. `e.x`/`e.y` are screen coords;
`e.gesture` is set for `Gesture` events.

| `EventType` | When |
|---|---|
| `Touch` | finger first contacts (`x`,`y` = down point) |
| `TouchHold` | finger still down, ~30 Hz |
| `TouchUp` | finger released |
| `Gesture` | swipe / double-tap / long-press; read `e.gesture` |
| `ButtonDown` / `ButtonUp` | raw SW2 transitions |
| `ButtonShort` | SW2 released < 1 s **— this is "back"; right-swipe maps here too** |
| `ButtonVeryLong` | SW2 held > 3 s → global power-off (handled by the render task) |
| `ImuMotion` | jolt detected from the accel delta heuristic |
| `Tick` | RTC seconds rolled over (1 Hz) — drive clocks/timers off this |
| `TimerExpired` | the timer-wake infra fired (see §9) |

`Gesture` values: `SwipeUp/Down/Left/Right`, `SingleTap`, `DoubleTap`,
`LongPress`. The watch face uses swipe-up → app list; the carousels use
swipe-up/down to scroll.

**Hit-testing** is manual: compare `e.x`/`e.y` against your button rects (see
how the diagnostics views do `inRect(...)`).

---

## 8. Shared state — `model` + `ModelLock` (`src/core/model.h`)

`model` is the single global struct of live state (time, date, accel, battery,
button, active screen, all user settings/colors). **Every access — read or
write — must hold the mutex:**

```cpp
{ ModelLock lk;                 // RAII; releases at end of scope
  uint8_t h = model.hour, m = model.minute; }
```

Keep the lock scope tiny — copy out what you need, then release before drawing.
Never call `switchTo()` or draw while holding the lock. To nudge a redraw from
outside the normal flow, bump `model.revision++` under the lock.

**Don't write the RTC directly.** Queue it cross-task:
`requestSetRTC(h,m,s, weekday, day, month, year)` — `taskIO` performs the I2C
write on its next cycle (`controller.h`).

---

## 9. Available subsystems (drop the ones you don't use)

### Drivers (`src/drivers/`) — keep these

- **`display.h`** — `gfx`, `displayBegin()`, `backlightSet(0..255)`,
  `backlightOn/Off()`, `frameCanvas()`.
- **`touch.h`** — CST816S; the I/O task already feeds touch into events. You
  rarely call this directly.
- **`haptic.h`** — `hapticBuzz(intensity 0..255, duration_ms)` is fire-and-forget
  and safe from any task. `hapticSetStrengthPct()` is the global scaler (Settings
  → Haptics). Buzz on every meaningful tap — it's the platform's feel.
- **`i2c_bus.h`** — `i2cPing/i2cReadReg/i2cScan`. **Only call from `taskIO`.**
- **`power.h`** — `latchPower()` (first line of `setup()`, already there),
  `unlatchPower()` (cut power), `buttonPressed()`.
- **`pins.h`** — `PIN_*` and `I2C_ADDR_*` macros.

### Core (`src/core/`)

- **`controller.*`** — sensor reads + tasks + `enterDeepSleep()`. Keep.
- **`storage.*`** — NVS-backed settings. **To persist a new setting:** add the
  field to `model`, then add a `prefs.get*`/`prefs.put*` line in `Storage::load()`
  and `Storage::save()`. Also holds the known-WiFi-networks store.
- **`anim.*`** — *optional.* Retained-mode scene graph + tween/easing engine
  (`UI ui(canvas); ui.rect(...); ui.animate(id, Prop::X, to, ms, Easing::...)`).
  Great for animated apps; **delete `anim.cpp/.h` if you don't animate** —
  nothing in the base shell uses it.
- **`apptimer.*`** — *optional.* RTC-epoch time base + countdown-timer and
  stopwatch state in `RTC_DATA_ATTR` (survives deep sleep). The **timer-wake
  infra is wired through** `main.cpp` + `controller.cpp` + `enterDeepSleep()`:
  call `timerArm(nowEpoch, durationSec)` and the watch wakes from deep sleep to
  fire `EventType::TimerExpired`. Route that event to your screen in
  `taskRender` (there's a marked spot). If your app has no scheduled wake,
  delete `apptimer.*` and the three small `timer*()` blocks it feeds.
- **`wifi_svc.*`** — *optional, gated by `-DEWATCH_ENABLE_WIFI`.* Background
  radio + HTTP settings server. AP mode = SoftAP `EWATCH_SETUP` →
  `http://192.168.4.1/`; client mode connects the strongest known SSID; NTP sync
  writes the RTC. Status mirrors into `model.wifi*`. Set the gate to `0` to drop
  it entirely.

### System shell (`src/apps/system/view.cpp`)

Watch face (multiple styles + 7-seg renderer), the carousel launcher, all
Settings pages, the diagnostics screens, and Power Off. Keep the watch face and
the launcher/settings; trim diagnostics from `kSystemApps[]` if unneeded.

---

## 10. Gotchas (the stuff that bricks a watch)

- **Power latch.** The watch has no hard switch — the firmware holds its own
  rail via `PIN_LDO_LATCH`. `latchPower()` is already the first line of
  `setup()`; never delay or remove it. Calling `unlatchPower()` powers the watch
  **off** instantly.
- **One task owns `Wire`.** All I2C goes through `taskIO`. Don't read sensors or
  touch the bus from a view — read `model` instead, and queue RTC writes with
  `requestSetRTC()`.
- **Watchdog.** Every task feeds a 20 s Task WDT. If your `render()`/`onEvent()`
  blocks (busy-loop, long `delay`, deadlock on `ModelLock`) the chip resets, and
  3 crash-boots in a row drop the rail (panic-loop guard in `main.cpp`). Keep
  per-frame work short; never hold `ModelLock` across a draw.
- **Auto-sleep.** After `model.sleepTimeoutSec` of no input the watch deep-sleeps.
  If your screen must stay lit (showing a code, a photo), add it to the
  `blockSleep` OR-chain in `taskRender` (marked).
- **`millis()` resets across deep sleep.** For anything that must survive sleep,
  anchor to the RTC epoch via `apptimer`'s `rtcEpochSec(...)`, not `millis()`.
- **Redraw discipline.** `render()` runs often. Cache last-shown values and paint
  deltas, or you'll flicker and pin the CPU.
