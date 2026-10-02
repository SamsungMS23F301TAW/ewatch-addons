# Shake Oracle (addon) — read this first

This folder is the **Shake Oracle** addon: EWatch BaseOS (`dbe73c2`) plus one
app, a shake-to-answer oracle ball. Marketplace id `shake-oracle` (see
`addon.json`); the folder keeps its working name `Magic_8_Ball`. The BaseOS
guide follows below, updated where this addon changed the platform.

## Architecture

```
taskIO (core 0) --imuStreamPush--> core/imu_stream (lock-free SPSC ring, 64 samples)
                                         |
taskRender (core 1) -> OracleView::render()  (src/apps/oracle/oracle_view.cpp)
     drains the ring -> OracleApp::imuSample()      ShakeDetector edges
     touch / swipes  -> OracleApp::tap()/swipePack()
     OracleApp::frame(fb) -> Scene::update() -> Renderer::drawWindow(fb)
     pushes rows kWinTop..kWinBottom (+ label strip) with gfx->draw16bitRGBBitmap
     FrameOut.buzz[] -> hapticBuzz(), activity -> noteActivity(), NVS/RTC saves
```

Everything in `src/apps/oracle/core/` is **portable C++11 with no Arduino
dependency**. It builds on the watch (with GFX's `gfxfont.h`) and on the host
(with `tools/host_include/gfxfont.h`). The unit tests and the preview tool run
the exact code the watch runs. Keep it that way. Don't include Arduino
headers there (the watch's `-std=gnu++11` also rules out C++14 features: give
multi-statement lambdas explicit return types).

| File | Role |
|---|---|
| `src/apps/oracle/core/oracle_config.h` | **Every tunable**: geometry, die size, text sizes, frame periods, awake time, haptic levels, tilt signs, golden odds. Start here. |
| `core/shake_detector.*` | Gravity low-pass → lobes → direction reversals; Started/Stopped edges, intensity. Parameters in `ShakeConfig`. |
| `core/answers.*` | The four packs (text, prompt, golden), PCG32, weighted picker that never repeats the last text. |
| `core/text_layout.*` | Fit text into a `FitRegion` (triangle, disc), shrink-to-fit by binary search over cap height, word wrap over every split, forced `\n` breaks, area-weighted anti-aliased raster of GFX fonts. |
| `core/oracle_scene.*` | Phases Idle/Churning/Rising/Showing/Sinking, die physics (buoyancy + floor + springs), bubbles, motes, swirl. Produces a `FrameState`. |
| `core/oracle_render.*` | Pixels: cached ball (+ label strip), per-frame window pass (row buffer, 8.8 fixed point, Bayer-dithered RGB565), die sprite (coverage/shade/text/glow), prompt mask, launcher tile icon. |
| `core/oracle_app.*` | `OracleApp` glues it all and `HapticPlanner` schedules buzzes (never zero-intensity pauses, at most one ahead of the driver queue). |
| `src/apps/oracle/oracle_view.*` | The `View`: canvas, row flushes, haptics, dimming, NVS (`"shake-oracle"` / `pack`), RTC (last answer + RNG), serial console, text-mode fallback, tile icon blit. |
| `src/core/imu_stream.*` | Timestamped sample ring; enabled only while a consumer view is active. |
| `test/test_*` | Unity tests: `pio test -e native` (54 cases). |
| `tools/oracle_preview.cpp`, `tools/make_previews.sh` | Host previews → `docs/*.png`, `docs/sequence.png`, `docs/oracle.gif`. |

## Extension points

- **New answers or packs**: edit `core/answers.cpp` (each `Pack` has a
  name, a prompt with one `\n`, answers and a golden answer). Run
  `pio test -e native`: `test_text_fit` proves every answer fits the die at
  `kDieCapMin` or larger with all ink on the face, and `test_answers` checks
  ASCII, lengths, duplicates and balance.
- **Look and feel**: palette constants at the top of `core/oracle_render.cpp`,
  geometry in `oracle_config.h`. Run `tools/make_previews.sh` and *look* at
  `docs/` before shipping.
- **Feel of the physics**: spring and buoyancy constants in
  `Scene::stepDie()`, haptic levels in `oracle_config.h`.
- **Shake sensitivity**: `ShakeConfig` defaults. Re-run `test_shake`, whose
  walking, running and jolt rejections are the guardrails.
- **Another animated app**: override `View::framePeriodMs()` for a frame
  cadence and `View::minAwakeSec()` to stay lit; consume `imu_stream` if you
  need every IMU sample.

## Build / test / package

```sh
~/.platformio/penv/bin/pio run -d addons/Magic_8_Ball              # env:ewatch (default)
~/.platformio/penv/bin/pio test -d addons/Magic_8_Ball -e native   # host tests
python3 tools/package_addon.py addons/Magic_8_Ball                 # dist/
addons/Magic_8_Ball/tools/make_previews.sh                         # docs/
```

No watch connected? Verify with the tests and previews; never flash from an
agent session. Hardware-only checks are listed in `README.md`.

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
~/.platformio/penv/bin/pio run                       # build (env:ewatch, the default)
~/.platformio/penv/bin/pio run -t upload -t monitor  # flash + serial @115200
~/.platformio/penv/bin/pio test -e native            # host unit tests (no watch needed)
```

(Upstream BaseOS also lists a `disptest` env for `src/test/disp_touch_test.cpp`;
that file isn't in BaseOS, so this addon removed the env.)

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
   An optional fourth field draws artwork above the name:
   `void icon(Arduino_GFX *g, int16_t cx, int16_t cy, int16_t r, uint16_t tileBg)`
   (added by Shake Oracle; see `oracleDrawTileIcon`, which blits a cached
   anti-aliased sprite). Names longer than 11 characters drop to text size 2
   in the bitmap-font styles so they fit the tile.

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
  virtual uint16_t framePeriodMs() const; // added: >0 = call render() at this cadence
  virtual uint16_t minAwakeSec() const;   // added: raise the idle-sleep timeout here
};
```

- `framePeriodMs()` (added by Shake Oracle): return e.g. 28 for ~35 fps
  continuous animation. `taskRender` then waits for events only until the next
  frame is due, and always blocks at least one tick so lower-priority tasks
  run. 0 keeps the classic behaviour.
- `minAwakeSec()` (added): while this view is active the auto-sleep timeout is
  at least this long (the user's longer setting, or 0 = never, still wins).
  Views can call `noteActivity()` (controller.h) for activity that isn't an
  input event, and `idleMs()` / `effectiveSleepTimeoutSec()` to dim ahead of
  sleep.

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
- **`anim.*`** — *removed in this addon* (Shake Oracle animates with its own
  physics in `src/apps/oracle/core/`). Upstream BaseOS has a retained-mode
  scene graph + tween/easing engine there if a future app wants it.
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
  `blockSleep` OR-chain in `taskRender` (marked), or override
  `View::minAwakeSec()` for a longer per-screen timeout.
- **`millis()` resets across deep sleep.** For anything that must survive sleep,
  anchor to the RTC epoch via `apptimer`'s `rtcEpochSec(...)`, not `millis()`.
- **Redraw discipline.** `render()` runs often. Cache last-shown values and paint
  deltas, or you'll flicker and pin the CPU.
