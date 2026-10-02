// Background power for Pixel Pet: steps keep counting with the screen off.
//
//   SCREEN ON    stock behaviour; taskIO drains the accelerometer FIFO.
//   SCREEN OFF   (after the idle timeout or a button press) the render task
//                runs bgRunScreenOff(): backlight off, panel SLPIN, taskIO
//                parked, then a light-sleep loop. The MMA8451 fills its FIFO
//                at 12.5 Hz and raises INT2 at the watermark (~every 1.6 s);
//                each wake drains it, runs the step detector and the pet, and
//                sleeps again. Button / touch / (optional) wrist-jolt wake the
//                screen through the fast path (no reboot).
//   DEEP         after PIXELPET_STILL_MIN minutes without movement the loop
//                drops into deep sleep with a low-threshold accelerometer
//                motion wake on INT1 (plus button, touch, and a timer for the
//                next nudge / housekeeping). A motion or timer wake boots in
//                the background (screen stays dark) and resumes the loop.
//
// There is no idle power-off in this mode; the low-battery cutoff (sustained,
// plausible readings below 3.40 V) powers the watch off instead.
//
// Safety: the button always wakes the watch (a press during a dark boot is
// latched); INT2 trouble (stuck high or silent) falls back to timer polling;
// touch-wake storms disable touch wake for that dark period (and repeated
// phantom touch wakes for the rest of a deep sleep); two crashes while dark
// switch background stepping off until the next power cycle; USB attached =
// simulated sleep so serial survives.
// Everything here is compiled out with -DPIXELPET_BG_STEPS=0.
#pragma once
#include <stdint.h>

bool bgCompiled();          // PIXELPET_BG_STEPS
bool bgActive();            // compiled + enabled in Pixel Pet options + accel present

// Boot classification (setup()): returns true when this boot should stay dark
// and go straight back to background sampling (motion / timer wake from the
// deep tier). May not return: a spurious touch wake from the deep tier
// re-arms deep sleep immediately.
bool bgClassifyBoot(int wakeCause);
bool bgBootIsBackground();
// Cheap early check (RTC memory only, no I2C): was the last sleep ours?
bool bgWokeFromDeepTier();
// Button-press latch for dark boots: armed first thing in setup() after a
// deep-tier wake, read (and disarmed) by bgRunScreenOff(). Disarm it when the
// boot turns out not to be a background one.
void bgArmBootPressLatch();
void bgDisarmBootPressLatch();
// Call once at boot with whether the last reset was a crash. Two crashes
// inside the dark loop in a row switch background stepping off until the
// next power cycle (crash tracking lives in RTC_NOINIT memory).
void bgNoteResetReason(bool crashed);
bool bgSafeMode();

// Render task only. Blocks for the whole dark period; returns when the user
// wakes the watch (the view has been repainted and the backlight is up).
// May not return (deep tier / low battery).
void bgRunScreenOff(bool fromBoot);

// Low-battery cutoff, fed with readings while the screen is on (render task).
void bgCheckBattery(float volts, bool ok);

struct BgStats {
  uint32_t screenOffs, lightSleeps, wakeFifo, wakeTimer, wakeButton, wakeTouch, wakeJolt;
  uint32_t spuriousTouch, int2Faults, deepEntries, bgBoots, samples, simulated;
  uint32_t deepNoMotion;     // deep sleeps taken without the motion wake armed
  uint32_t deepBusy;         // deep-tier entries where INT1 wouldn't settle
  bool     int2Mode;
};
BgStats bgStats();
