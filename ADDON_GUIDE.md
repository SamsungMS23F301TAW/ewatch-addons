# EWatch addon guide

Shared conventions for every addon in `addons/`. If you're an agent building one,
read this whole file first, then `EWatch_Dev/BaseOS/CLAUDE.md`.

## What an addon is

The EWatch is Ewan Wills' custom ESP32-S3 smartwatch. Upstream source is cloned
at `EWatch_Dev/` (github.com/Ewan-Wills/EWatch_Dev @ `dbe73c2`, MIT licence).
**Treat `EWatch_Dev/` as read-only.**

EWatch Cloud is the online marketplace that installs addons onto the watch over
a USB cable. ESP32 firmware is one monolithic image, so **an addon is a complete
standalone firmware**: BaseOS plus one app or face. That's the same pattern as
upstream's `tamagotchi/`, `Tunnel_Racer/` and `Starfox_shooter/` folders.
Installing an addon replaces whatever was on the watch, so every addon must be a
fully working watch on its own (watch face, launcher, settings, sleep) with its
feature added.

We don't yet know EWatch Cloud's exact upload format. Each addon ships a
PlatformIO project, an `addon.json`, and a `dist/` folder made by the shared
packager. The packager outputs flash parts, an ESP Web Tools `manifest.json`,
a merged image and a zip, which covers how browser-based cable flashers work.

## Getting started

1. Read `EWatch_Dev/BaseOS/CLAUDE.md` end to end (app recipe, `View` contract,
   `model` + `ModelLock`, events, drawing, gotchas) and `EWatch_Dev/BaseOS/PINOUT.md`.
2. Create your addon from BaseOS:
   `rsync -a --exclude .pio EWatch_Dev/BaseOS/ addons/<Folder>/`
   Copy `EWatch_Dev/LICENSE` in, and delete the stray `package.json` and
   `package-lock.json` (an unused npm stub).
3. Build it unmodified once, then add your feature using the CLAUDE.md recipe.

## Build and test

```sh
~/.platformio/penv/bin/pio run -d addons/<Folder>            # firmware (env:ewatch)
~/.platformio/penv/bin/pio test -d addons/<Folder> -e native # host unit tests, if you add env:native
python3 tools/package_addon.py addons/<Folder>               # build + package into dist/
```

- Pre-installed and verified on this machine: Espressif32 platform 7.1.3,
  Arduino-ESP32 2.0.17 (ESP-IDF 4.4), GFX Library for Arduino 1.4.7, the
  `native` platform with Unity, and NimBLE-Arduino 1.4.3 (builds on this core
  with server, advertising and scan together). Stock BaseOS builds in about 70 s:
  RAM 15.7%, flash 30.9% of the 3 MB app partition.
- Don't change the platform version or move to Arduino 3.x / pioarduino.
- **No watch is connected to this machine.** Never upload, flash, or open serial
  ports. Verification means a clean compile, host-side tests of your algorithms,
  host-rendered previews of your visuals where practical, and careful review.
  Everything that needs real hardware goes in your README's hardware test
  checklist.
- Host previews: keep pixel-generating code (art, sprites, layouts) separable
  from Arduino where practical. That lets you compile it with `clang++` on
  macOS, dump frames as PNG, and **look at them with the Read tool**. Python's
  stdlib `zlib` + `struct` can write PNGs, and macOS `sips` converts BMP to PNG.
  This is the only way to see your UI before it reaches a real watch, so use it.
  Put the best previews in `docs/`; they double as marketplace screenshots.
- Host tools available: `clang++`, Node 24, and `python3` with Pillow and numpy.
  pyserial lives in `~/.platformio/penv/bin/python`, so serial helper scripts
  should say to run them with that interpreter (or `pip install pyserial`).
- Other agents are building other addons in parallel on the same machine. Only
  write inside your own `addons/<Folder>/` (scratch files can go in your
  `addons/<Folder>/.scratch/`, which you delete at the end). Don't touch other
  addons, `EWatch_Dev/`, `tools/`, this guide, or global config. Don't install
  global software (pip, brew, npm -g). Project-local PlatformIO `lib_deps` are
  fine.

## Hardware facts that shape designs

| Part | What matters |
|---|---|
| SoC | ESP32-S3FH4R2: dual core 240 MHz, 4 MB flash, 2 MB QSPI PSRAM, native USB-CDC serial at 115200 |
| Display | ST7789 240×280 portrait, RGB565, SPI at 60 MHz. A full-frame flush takes about 18 ms, so expect 30–40 fps for full-screen animation. `frameCanvas()` returns a lazily allocated 240×280 PSRAM canvas (134 KB); call `flush()` to push it. |
| IMU | **MMA8451Q accelerometer only**: no gyro, no magnetometer, no hardware step counter. BaseOS sets it to ±2 g, 14-bit (4096 counts/g), `CTRL_REG1=0x01` (800 Hz ODR). `taskIO` reads it every cycle into `model.ax/ay/az`. That cycle is "50 Hz" but implemented as work + `vTaskDelay(20 ms)`, so the true rate is about 45 Hz with jitter. Timestamp your samples, or use the chip's FIFO at a fixed ODR. The chip also has a 32-sample FIFO and transient, motion, pulse and orientation engines. INT1 = GPIO3 (also a strap pin, used for jolt-wake), INT2 = GPIO4 (free). With FIFO mode on (`F_SETUP` 0x09), reading `OUT_X_MSB` (0x01) pops the FIFO, so `readAccel()` must change to match. `EventType::ImuMotion` is a coarse jolt heuristic (sum of \|Δ\| over 300 ms > 18000). |
| Touch | CST816S, gestures included. Right-swipe is turned into `ButtonShort` (back) by `taskIO`. |
| Haptic | `hapticBuzz(intensity 0..255, ms)` is fire-and-forget and already scaled by the user's strength setting. Patterns need a small non-blocking sequencer. |
| RTC | RV-3028 keeps **local** time. `rtcEpochSec()` (apptimer) gives seconds since 2000 and is the time base for anything that must survive sleep. `model.tzOffsetMin` is the UTC offset (fixed, no DST rules). Write time only via `requestSetRTC()`. |
| Radio | WiFi via `core/wifi_svc` (gate `-DEWATCH_ENABLE_WIFI`; set it to 0 if unused). BLE isn't used upstream yet. Prefer NimBLE-Arduino `@ ^1.4.3` over the built-in Bluedroid because it's far smaller. |
| Flash | `huge_app.csv`: app 3 MB at 0x10000, NVS 20 KB at 0x9000, and a free 896 KB data partition labelled `spiffs` at 0x310000 (LittleFS: `LittleFS.begin(true, "/littlefs", 10, "spiffs")`). |

Rules carried over from BaseOS:
- `taskIO` (core 0) is the only task that touches `Wire`.
- `taskRender` (core 1) is the only task that draws.
- Never draw or call `switchTo()` while holding `ModelLock`.
- Never block in `render()` or `onEvent()`. There's a 20 s task watchdog, and 3 crash-boots in a row make the watch power itself off.

**If your algorithm needs every sample** (steps, reps, shakes, tilt smoothing),
don't poll `model` from `render()`. Add a small sample hook or lock-free ring
buffer that `taskIO` feeds, and consume it on the render task or your own task.

### MMA8451Q register cheat-sheet

Put the chip in standby (`CTRL_REG1` bit 0 = 0) before changing configuration.

| Reg | Name | Notes |
|---|---|---|
| 0x00 | STATUS / F_STATUS | When the FIFO is on: F_OVF bit 7, F_WMRK_FLAG bit 6, F_CNT bits 5:0 |
| 0x01–0x06 | OUT_X/Y/Z MSB, LSB | 14-bit, left-justified: `(int16_t)(msb<<8 \| lsb) >> 2`. With the FIFO on, burst reads from 0x01 pop samples, 6 bytes each, and the address wraps 0x06 → 0x01. |
| 0x09 | F_SETUP | F_MODE bits 7:6 (00 off, 01 circular, 10 fill/stop, 11 trigger), F_WMRK bits 5:0 |
| 0x0C | INT_SOURCE | SRC_FIFO bit 6, SRC_TRANS bit 5, SRC_LNDPRT bit 4, SRC_PULSE bit 3, SRC_FF_MT bit 2, SRC_DRDY bit 0 |
| 0x0D | WHO_AM_I | 0x1A |
| 0x0E | XYZ_DATA_CFG | HPF_OUT bit 4, FS bits 1:0 (00 ±2 g, 01 ±4 g, 10 ±8 g) |
| 0x15–0x18 | FF_MT_CFG / SRC / THS / COUNT | motion and freefall |
| 0x1D–0x20 | TRANSIENT_CFG / SRC / THS / COUNT | high-pass jolt detection (used for wake) |
| 0x21–0x28 | PULSE_* | tap and double-tap |
| 0x2A | CTRL_REG1 | ASLP_RATE bits 7:6, DR bits 5:3 (000 800 Hz, 001 400, 010 200, 011 100, 100 50, 101 12.5, 110 6.25, 111 1.56), LNOISE bit 2 (±2 g and ±4 g only), F_READ bit 1, ACTIVE bit 0 |
| 0x2B | CTRL_REG2 | RST bit 6, SLPE bit 2, MODS bits 1:0 (00 normal, 01 low-noise low-power, 10 high-res, 11 low-power) |
| 0x2C | CTRL_REG3 | WAKE_* bits 6:3, IPOL bit 1 (1 = active-high), PP_OD bit 0 |
| 0x2D | CTRL_REG4 | interrupt enables: ASLP 7, FIFO 6, TRANS 5, LNDPRT 4, PULSE 3, FF_MT 2, DRDY 0 |
| 0x2E | CTRL_REG5 | interrupt routing, same bit layout as CTRL_REG4 (1 = INT1, 0 = INT2) |

On Arduino-ESP32 2.x, `Wire`'s default buffer is 128 bytes. Drain the FIFO in
chunks of at most 120 bytes (20 samples) per `requestFrom`; a full 32-sample
FIFO is 192 bytes.

## Power model: read `main.cpp` and `controller.cpp` before designing

- After `model.sleepTimeoutSec` with no activity (default **5 s**), `taskRender`
  calls `enterDeepSleep()`. Waking from deep sleep is a **full reboot**:
  `setup()` runs again and `millis()` restarts. `RTC_DATA_ATTR` variables survive.
- After `model.sleepToOffSec` asleep with no wake (default **30 s**), a timer
  wake drops the LDO latch and the watch is **fully off**. RTC memory is lost and
  only the button turns it back on. The exception is when `timerArmed()`
  (apptimer): then the timer wake boots normally and fires `TimerExpired`. Route
  that event to your screen in `taskRender`, where the spot is marked.
- So with stock BaseOS, **nothing happens in the background**: no step counting,
  no BLE scanning, no reminders unless a timer is armed. If your addon needs
  background behaviour, design an explicit power strategy, put the risky part
  behind a build flag so it can be switched off if hardware testing finds
  problems, and estimate the battery cost in your README.
- Interactive screens that must stay lit (a workout, the radar, a long
  animation) should extend the `blockSleep` OR-chain in `taskRender`, or use a
  longer per-screen timeout. The user must still be able to leave and sleep.
- `EWatch_Dev/EWatchOS2.1/src/core/power_mgr.cpp` is a working tiered power
  manager for this exact board. It dims, then turns the panel off and enters
  light sleep with GPIO wake on touch, button, accel INT2 and RTC INT, then
  deep-sleeps. `EWatchOS2.1/src/core/controller.cpp` runs the MMA8451 at 50 Hz in
  low-power mode with TRANSIENT on INT2, and has a cooperative `taskIO` pause for
  light sleep. Borrow from these rather than re-deriving light sleep. Note that
  light sleep kills USB-CDC.

## Background step counting (shared design, used by Pixel Pet and Generative Face)

Implement it in `src/core/steps.{h,cpp}`. Keep the detector itself free of
Arduino dependencies so it can be unit-tested on the host.

- **Detector.** Acceleration magnitude in g. Remove gravity with a moving average
  or high-pass, low-pass at about 3 Hz, then detect peaks with an adaptive
  threshold. Accept step intervals of 0.25–2.0 s. Add a regularity gate: after
  at least 4–6 consistent steps, count those and every step that follows. That
  rejects arm waving, typing and single jolts. Test it against synthetic walking,
  running, wrist noise, typing and car-ride signals.
- **Sampling while the screen is off** (the hard part). Recommended strategy;
  adapt it and document the trade-offs:
  1. Screen-off is light sleep, not deep sleep. Run the MMA8451 at 12.5–25 Hz
     with the FIFO in watermark mode routed to INT2 (GPIO4), wake on that pin,
     drain the FIFO over I2C, run the detector and sleep again. That's about one
     short wake per second.
  2. After a long stillness (10–15 min with no motion), deep-sleep with the
     MMA8451 motion/transient interrupt as a wake source. A walk then resumes
     counting, losing only its first few steps.
  3. Touch or button wake turns the screen on immediately, as it does today.
  4. Replace stock idle power-off for this addon, but keep a low-battery cutoff.
- **Persistence.** Keep today's count in RTC memory, and checkpoint it to NVS
  every few minutes or few hundred steps. Roll over at local midnight using the
  RTC date, and keep a daily history.

## Persistence

- Base settings live in NVS namespace `"ewatch"` (`storage.cpp`). Keep that
  schema unchanged so a user's theme, WiFi networks and haptics carry across
  addon installs. The packager's web-flasher manifest deliberately skips the NVS
  region for this reason.
- Addon data goes in **its own NVS namespace** (max 15 chars, for example the
  addon id) or a directory named after your addon id on the LittleFS `spiffs`
  partition. Never reinterpret keys in `"ewatch"`.
- Save on state changes and on exit, never per frame (flash wear).

## UX conventions

- Back is `ButtonShort`, which right-swipe also sends; always handle it. Holding
  the button for 3 s is the global power-off, which BaseOS handles.
- Use `theme()` colours for chrome. Faces, games and artwork may own their palette.
- Buzz on meaningful taps; that's the platform feel.
- Redraw only what changed, or render into `frameCanvas()` and flush. No
  full-screen `fillScreen` flicker every frame.
- **Watch-face addons**: make your face the default `Screen::Watch` presentation.
  The time must stay readable at a glance and swipe-up must still open the
  launcher. Keep the stock BaseOS face styles reachable from Settings if
  practical.
- Interactive apps: put your tile first in `kTopApps[]`.
- Serial debug commands (optional but encouraged): BaseOS's `loop()` is idle, so
  a tiny line reader there is fine. `EWatchOS2.1/src/core/ewlog.*` has a console
  pattern to borrow. Never require the serial port for normal use.

## Required deliverables in `addons/<Folder>/`

1. A full PlatformIO project (BaseOS + your feature) that **compiles with zero
   errors** and no new warnings from your code.
2. `addon.json`:
   ```json
   {
     "id": "kebab-case-id",
     "name": "Display Name",
     "version": "1.0.0",
     "type": "watchface | app | game | tool",
     "summary": "One line, at most 120 characters",
     "description": "A short paragraph for the marketplace listing.",
     "author": "Kyle Hanning",
     "license": "MIT",
     "base": "EWatch BaseOS @ dbe73c2",
     "capabilities": ["imu", "haptic", "touch", "ble", "wifi", "rtc-wake", "background-steps", "background-ble", "serial", "storage"],
     "icon": "icon.svg"
   }
   ```
   List only the capabilities you actually use. The packager rejects unknown ones.
3. `icon.svg`: an original, square, flat icon that reads at 64 px. Upstream
   precedent is `EWatch_Dev/ScrubMarine/SMIcon.svg`.
4. `README.md`, rewritten rather than the stale BaseOS copy. Cover:
   - what it does, and how to use it on the watch (gestures, buttons)
   - design and algorithm notes
   - power strategy with a battery estimate
   - limitations
   - build, test and package commands
   - a **hardware test checklist** for Kyle and Ewan
5. `CLAUDE.md`: keep BaseOS's guide, but add a section at the top describing
   your addon's architecture, files and extension points for future agents.
6. `LICENSE`: upstream MIT, copied in.
7. Tests for the algorithmic core (detectors, parsers, models, determinism), run
   with `pio test -e native` or plain `clang++`, and passing.
8. `python3 tools/package_addon.py addons/<Folder>` run successfully, so `dist/`
   is up to date.
9. No leftover scratch files and no `.scratch/` directory. `.pio/` and `dist/`
   may stay.

## Originality

Everything must be original: code, art, names and text. No copyrighted
characters or artwork, no trademarked product names as the addon's name, and
no copied text such as a toy's answer list.

## Final report (your last message)

Plain text with these sections:
- **Built**: what the addon does, and its folder.
- **Files**: the key files you added or changed, one line each.
- **Build**: the result with RAM and flash percentages.
- **Tests**: what you ran and the pass/fail counts.
- **Package**: dist files and app partition usage.
- **Design decisions**: including the power strategy and battery estimate.
- **Risks and limitations**: be candid about what's unverified without hardware.
- **Hardware test checklist**: the top items.
- **Questions for Kyle**: things only the user can decide.
