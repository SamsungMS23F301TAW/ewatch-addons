# EWatch Companion (addon `ble-companion`): read this first

BaseOS plus a **BLE GATT "EWatch Config Service"** that lets a Web Bluetooth
page (`web/index.html`) restyle the watch live: theme, brightness, clock style,
12-hour time, four face **data-source slots**, custom texts, pushed **live
data** (values and countdowns), time sync, messages and control actions.
Version 1.1 adds **Halo**, an anti-aliased default face (style 8), and
redraws the Companion / Message screens and the web page in the same visual
language. The BaseOS app-building guide follows below this section and is
still accurate, except where noted here.

## Architecture

| Layer | Files | Notes |
|---|---|---|
| Wire protocol (pure C++11) | `src/core/ble_proto.{h,cpp}` | UUIDs, limits, LE layouts, validation. **Source of truth**, specified in `docs/PROTOCOL.md` and mirrored in the page's `CORE` block |
| Slot formatting (pure C++11) | `src/core/face_slots.{h,cpp}` | `slotRaw()` (content) and `layoutSlot()` (fitting for the 6×8 font), UTF-8 → CP437 glyphs, countdown and date text. Mirrored in JS |
| Slot store (Arduino) | `src/core/face_store.{h,cpp}` | Slots, texts, feeds and options in RAM; NVS namespace `ble-companion`; `generation()`; debounced `flushIfDue()` / `flushNow()` |
| GATT server (NimBLE 1.4.3) | `src/core/ble_config.{h,cpp}` | Auth (6-digit code), advertising window, timeouts, the `blecfg` service task (notifications, saves, backlight). Gated by `-DEWATCH_ENABLE_BLE` |
| Watch face | `src/apps/face/face_render.{h,cpp}`, `face_icons.{h,cpp}` | Style table + BaseOS's face **moved here from view.cpp** (7-seg, corner icons, timer lines) plus slots, 12-hour time and the BLE glyph. `Renderer::draw(g, frame, fb)` dispatches style 8 to Halo. Depends only on Arduino_GFX, so it compiles on the host |
| AA engine (pure C++11) | `src/apps/face/aa_gfx.{h,cpp}`, `aa_assets.{h,cpp}` (generated) | Integer-only anti-aliased primitives into an RGB565 buffer, plus 4-bit type/icon atlases. Mirrored line for line in the page ("AA MIRROR") |
| Halo face (pure C++11) | `src/apps/face/face_halo.{h,cpp}` | `palette()`, `backdrop()`, `card()`, `buildScene()`, `paint()` honouring the clip, row regions. Mirrored in the page ("HALO MIRROR") |
| Screens | `src/apps/companion_ui.{h,cpp}` (pure painter), `src/apps/companion.{h,cpp}` (views) | `CompanionView` (visible / code / connected / off / lockout, reset safety nets), `MessageView`. Not mirrored in JS |
| Glue | `src/core/controller.cpp` (`taskRender`), `src/main.cpp`, `src/apps/system/view.cpp` | Routes `EventType::BleState/BleMessage/BleRequest`, keep-awake and dimming, `prepareForSleep()`, serial console, registrations |
| Web page | `web/index.html` | Single static file. `// @@CORE:BEGIN … // @@CORE:END` is pure and is loaded by the Node tests. Classic fonts are injected between `@@FONT-DATA` markers, the AA atlases between `@@AA-DATA` markers |

Threading rules added by this addon:
- GATT callbacks run on the **NimBLE host task**. They validate and apply
  under short locks only. Lock order is companion mutex → FaceStore mutex →
  `ModelLock`; never take the companion mutex while holding `ModelLock`.
- The UI learns about BLE through **events**; only `taskRender` calls
  `switchTo()` or draws. NVS writes and `backlightSet()` for BLE changes
  happen on the `blecfg` task.
- `WatchFaceView` renders into `frameCanvas()` and pushes only the dirty row
  spans the renderer reports (`Renderer::dirtySpan`). Full repaints happen on
  theme, style or option changes.

Rules for the anti-aliased code (break them and the page stops matching the
watch):
- **No floating point** in `aa_gfx`, `face_halo` or anything they call. Every
  divide must have a non-negative numerator (C++ truncation == JS floor),
  intermediates must stay below 2^31, and `>>` of negatives is relied on to be
  arithmetic (GCC, clang and JS all agree).
- A pixel's value may depend only on its position and the call's arguments,
  **never on the clip** (no error diffusion, no "draw if changed"). Partial
  repaints through `aa::setClip` rely on it.
- Every Halo element must stay inside its region's rows (`halo::regionRows`);
  shadows and glows included. `render.py`'s incremental pairs catch leaks.
- Change C++ and the page's mirror together, then run `render.py` (golden
  frames) and the Node tests. Atlases are regenerated with
  `tools/gen_aa_assets.py` (Pillow + numpy + Inter Display; writes both
  sides).

## Extension points

- **New slot source:** add it to `bleproto::Source` (and `kSourceMax`), format
  it in `faceslots::layoutSlot()`, mirror it in the page's `layoutSlot`, add it
  to `sourceOptions()` in the page, add vectors in `tools/gen_vectors.py`, and
  bump `kProtoMinor`.
- **New characteristic:** a UUID in `ble_proto.h`, an encoder and decoder,
  then `addChr()` plus `buildValue()` plus a `handleWrite()` case in
  `ble_config.cpp`, the JS codec, `docs/PROTOCOL.md`, and vectors.
- **New icon:** `face_icons.cpp` on the 16-grid using integer `P()` scaling
  (Classic styles), the identical code in the page's `drawIcon`, `kIconMax`;
  plus a draw function in `tools/gen_aa_assets.py` (`FEED_ICONS`, both sizes)
  for Halo and the screens. Then re-run the generator,
  `tools/preview/render.py` (golden frames) and the Node tests.
- **New face style:** append it to `kStyles` in `face_render.cpp` (never
  insert: the index is stored in NVS and on the wire), mirror it in the page's
  `STYLES`, bump `kProtoMinor`, and update Settings → Font's grid in
  `view.cpp` if the count no longer fits.
- **Reusing the BLE module in another addon:** see the header comment in
  `src/core/ble_config.h`. Keep the NVS namespace `ble-companion` so users keep
  their slots.

## Verify (no watch needed)

```sh
~/.platformio/penv/bin/pio run                   # 0 warnings expected
~/.platformio/penv/bin/pio test -e native        # protocol + slot vectors
node --test test/web/*.test.mjs                  # JS codec + pixel-exact preview vs golden frames
python3 tools/preview/render.py                  # host render (real Arduino_GFX) -> docs/*.png, golden frames,
                                                 # plus incremental-redraw self-checks for the face
                                                 # (dirty spans) and the Companion screens (pushed rects)
```

After changing the renderer or icons, run `render.py` **before** the Node
tests (it rewrites the golden frames), then check the PNGs in `docs/` by eye.

---

# EWatch BaseOS — app-building guide

This is the **bare-bones EWatch slate**: every driver, core subsystem, and the
full system shell (watch face, app launcher, settings, diagnostics, power
management, sleep/wake) is present and working, but **no user apps are
installed**. The app launcher shows only `System` (EWatch Companion adds a
`Companion` tile first).

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
- **`anim.*`** — *(removed from EWatch Companion, whose screens animate with `aa_gfx` instead; copy it back from BaseOS if needed)* *optional.* Retained-mode scene graph + tween/easing engine
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

The carousel launcher, all Settings pages, the diagnostics screens, Power Off
and the `WatchFaceView` adapter. (EWatch Companion: the face's drawing code and
style table now live in `src/apps/face/face_render.cpp`.) Keep the watch face
and the launcher/settings; trim diagnostics from `kSystemApps[]` if unneeded.

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
