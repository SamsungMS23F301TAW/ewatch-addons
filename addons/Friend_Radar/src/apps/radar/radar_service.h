// Friend Radar service (device only): owns the RadarEngine, the radio and
// the radar task. Views talk to it through these thread-safe calls; nothing
// here draws or blocks the render task.
//
//   radar task (core 0, prio 3)
//     - radio state machine: NimBLE up when a session wants it, down after
//       ~20 s idle; advertising + scanning only while a session is open
//     - feeds decoded beacons to the engine, refreshes our advertisement
//     - carries out engine effects: haptic patterns, pending animations,
//       rendezvous clock adjustments, lazy saves
//
// Background mate alerts (FR_BACKGROUND_ALERTS) live here too: the headless
// radio window run from setup() after a timer wake, and the sleep planner
// enterDeepSleep() consults.
#pragma once
#include <stdint.h>
#include "fr_engine.h"
#include "fr_radar.h"
#include "radar_store.h"

#ifndef FR_BACKGROUND_ALERTS
#define FR_BACKGROUND_ALERTS 0
#endif

namespace radar {

// ---- lifecycle --------------------------------------------------------------
void init();          // load settings + mates (NVS). Idempotent; safe before tasks.
void startTask();     // launch the radar task (after controllerStartTasks()).

// ---- foreground session (the app is open) ----------------------------------------
void open();
void close();         // stops the radio, saves state
fr::RadioState radioState();

// ---- data for the UI ------------------------------------------------------------
int  snapshot(fr::PeerView *out, int max);
// An animation the UI should start (hello / celebrate). Returns false if none.
bool takePlay(fr::PlayCmd &cmd, char *peerName);
void localShake();    // wearer shook their wrist (ImuMotion)
uint32_t myId();

// ---- mates ------------------------------------------------------------------------
int  mates(fr::Mate *out, int max);
bool mateById(uint32_t id, fr::Mate &out);
bool isMate(uint32_t id);
bool addMate(uint32_t id);
bool removeMate(uint32_t id);
bool renameMate(uint32_t id, const char *nick);

// ---- settings -----------------------------------------------------------------------
RadarSettings settings();
void setName(const char *name);
void setAlerts(bool on);
void setBackground(bool on, uint8_t periodSec);
void setCalibration(int8_t ref1m, bool calibrated);
void resetId();

// ---- calibration --------------------------------------------------------------------
bool calStart(char *peerName);        // false if nobody is in range
uint16_t calCount();
int  calLastRssi();
fr::CalResult calResult();
void calStop();

// ---- time ---------------------------------------------------------------------------
uint32_t nowSec();                    // RTC local seconds since 2000 (model-based)

// ---- diagnostics (serial) ---------------------------------------------------------
void serialCommand(const char *line);

// ---- background mate alerts ----------------------------------------------------------
bool backgroundActive();              // setting on, mates saved, battery fine (and compiled)
#if FR_BACKGROUND_ALERTS
enum class BgOutcome : uint8_t { Sleep, Alert, UserWake };
bool      bgArmed();                  // the pending timer wake is a radar window (RTC memory)
void      bgSetArmed(bool armed);
// Milliseconds until the next rendezvous wake, or 0 if background mode should
// not run (off, no mates, low battery). rtcEpochSec = current RTC local time.
uint32_t  bgPlanSleepMs(uint32_t rtcEpochSec, uint8_t batteryPct, bool batteryKnown);
BgOutcome bgRunWindow();              // headless radio window, from setup()
void      bgSleep();                  // re-arm wakes + deep sleep; never returns
bool      takeAlertBoot();            // this boot was woken for a mate (consumes flag)
#endif

}  // namespace radar
