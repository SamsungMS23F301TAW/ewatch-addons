// Dayprint background power manager: keeps counting steps with the screen
// off. Compiled in with -DGF_BG_STEPS=1 and switchable in Settings -> Face.
//
// Tiers (see README "Power"):
//   1. MOVING   screen off, panel asleep, CPU in light sleep at 80 MHz. The
//               accelerometer FIFO wakes it every ~0.6 s (INT2) to feed the
//               step detector. Touch and button wake the screen instantly.
//   2. STILL    after 90 s without steps (25 s after a motion wake) the FIFO
//               wake is disarmed; only the accelerometer's motion interrupt
//               (INT1), touch or the button wake the CPU (plus a 15 s
//               housekeeping timer).
//   3. DEEP     after 15 min without motion: deep sleep, touch controller
//               powered down; motion (INT1) or the button wakes. A motion
//               wake reboots quietly straight back into tier 1, screen dark.
//               Every 4 h a timer wake checks the battery (a few hundred ms)
//               so a watch left in a drawer powers off before it runs flat.
// Safety: wake-storm guards on every wake source, a low-battery cutoff, and
// a simulated (polling) sleep while USB is attached so serial stays alive.
#pragma once
#include <Arduino.h>
#include <stdint.h>

// Call first thing in setup() with the wake cause (esp_sleep_wakeup_cause_t)
// and, for ext1 wakes, esp_sleep_get_ext1_wakeup_status(). Reads and clears
// the "background deep sleep" marker and works out whether this boot is a
// quiet motion wake. A TIMER wake from the background deep sleep is the
// periodic battery check: it checks, then goes straight back to sleep or
// powers off, and never returns.
void powerBgBoot(int wakeCause, uint64_t ext1Status);
bool powerBgBootQuiet();          // keep the screen dark on this boot

// Screen-off light sleep. Blocks until the user wakes the watch (returns),
// or drops to deep sleep / powers off (never returns).
void powerScreenOff();

// Tier 3. Does not return.
void powerBgDeepSleep();

// Low-battery cutoff: feed it battery readings; true = power off now.
// Needs three plausible readings below GF_LOWBAT_CUTOFF_MV, a minute apart.
bool powerBgLowBatteryCheck(float volts, bool ok);
void powerBgLowBatteryOff(bool screenOn);

// Cut the rail, from any task. Leaves the task watchdog first (a reset
// would latch the power back on), waits for a held SW2 to be released (it
// keeps the LDO enabled), and if the chip still keeps running, deep-sleeps
// with only the button armed and the latch held low. Does not return.
void powerHardOff();

void powerBgStatsPrint(Print &out);
