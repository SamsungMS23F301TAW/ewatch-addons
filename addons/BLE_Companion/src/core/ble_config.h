// BleConfig — the "EWatch Config Service": a self-contained BLE GATT server
// (NimBLE-Arduino 1.4.3) that lets a Web Bluetooth page restyle the watch
// live: theme colours, brightness, face style, face data-source slots, custom
// texts, pushed feeds (weather, next meeting), time sync, messages.
//
// Wire format: src/core/ble_proto.h + docs/PROTOCOL.md.
//
// Lifecycle (all timing is millis()-based; deep sleep reboots the chip, which
// also resets BLE to Off):
//   Off ──startAdvertising(window)──▶ Advertising ──central connects──▶ Pairing
//   Pairing: a fresh 6-digit code is shown on the watch; the page must write
//            it to the Auth characteristic within 60 s. 3 wrong codes drop the
//            link and pause advertising for 30 s.
//   Pairing ──correct code──▶ Connected: config writes are accepted, applied
//            live (model.revision++) and saved (debounced) to NVS.
//   Disconnect ──▶ Advertising while the window is open, else Off.
//   Connected with no client write and no touch for 5 min ──▶ dropped.
// The radio stack is brought up lazily the first time advertising is asked
// for and then stays up until the next deep sleep (no deinit/reinit churn).
//
// Threading: NimBLE callbacks run on the NimBLE host task; a small service
// task ("blecfg", core 0) owns advertising, timeouts, notifications, NVS
// flushes and the backlight. The UI learns about changes through events
// (EventType::BleState / BleMessage / BleRequest) posted to the render task.
//
// Reusing this module in another addon:
//   1. copy src/core/ble_proto.*, face_slots.*, face_store.*, ble_config.*
//      and add `h2zero/NimBLE-Arduino @ 1.4.3` + -DEWATCH_ENABLE_BLE=1;
//   2. call FaceStore::begin() then BleConfig::begin() in setup();
//   3. add BleConfig::clientConnected() to taskRender's blockSleep chain, call
//      BleConfig::noteUserActivity() on input, and BleConfig::prepareForSleep()
//      before deep sleep / power-off;
//   4. give the user a way to call startAdvertising() and to see status().code.
#pragma once
#include <stdint.h>
#include "ble_proto.h"

namespace BleConfig {

enum class State : uint8_t {
  Off         = 0,   // not advertising, no client
  Advertising = 1,   // visible to Web Bluetooth choosers
  Pairing     = 2,   // client connected, waiting for the 6-digit code
  Connected   = 3,   // client authorised; writes apply live
};

// Why the most recent state change happened (shown on the Companion screen).
enum class Reason : uint8_t {
  None = 0, Started, WindowEnded, Stopped, ClientConnected, Paired,
  WrongCode, LockedOut, PairTimeout, IdleTimeout, ClientLeft, Kicked,
};

// EventType::BleRequest payloads (Event.x).
enum Request : uint8_t {
  REQ_SHOW_FACE      = 1,
  REQ_SHOW_COMPANION = 2,
  REQ_IDENTIFY       = 3,
};

static const uint32_t kDefaultWindowMs = 3UL * 60UL * 1000UL;  // visible for 3 min
static const uint32_t kPairTimeoutMs   = 60UL * 1000UL;
static const uint32_t kIdleTimeoutMs   = 5UL * 60UL * 1000UL;
static const uint32_t kLockoutMs       = 30UL * 1000UL;
static const uint8_t  kMaxAttempts     = 3;

struct Status {
  State    state;
  Reason   reason;            // why we entered `state`
  bool     radioUp;           // NimBLE initialised
  uint32_t code;              // 6-digit pairing code (valid while Pairing)
  uint8_t  attemptsLeft;
  uint32_t pairRemainingMs;   // time left to enter the code
  uint32_t advRemainingMs;    // advertising window left
  uint32_t lockoutRemainingMs;
  uint32_t sessionMs;         // time since the code was accepted
  uint32_t writes;            // config writes applied this session
  uint32_t lastWriteMs;       // millis() of the last applied write (0 = none)
  uint16_t mtu;
  char     deviceName[16];    // "EWatch-1A2B"
};

// Call once from setup() after Storage::load() and FaceStore::begin(). Cheap:
// only creates the service task; the radio stays off until advertising.
void begin();

// Become visible for `windowMs` (restarts the window if already advertising).
void startAdvertising(uint32_t windowMs = kDefaultWindowMs);
// Stop advertising now (an existing connection is kept).
void stopAdvertising();
// Drop the current client (the watch's "Disconnect" button).
void disconnect();

Status status();
bool     clientConnected();       // Pairing or Connected: keep the watch awake
uint32_t lastActivityMs();        // millis() of the last client write, 0 = none
void     noteUserActivity();      // render task: touch / button happened

// The most recent pushed message (EventType::BleMessage). Returns false if
// there is none. The message stays available until the next one arrives.
bool takeMessage(bleproto::Message &out);

// Restore the stock face layout and/or theme from the watch itself (the
// Companion screen's safety net). Notifies a connected page (source = watch).
void resetFaceFromWatch();
void resetThemeFromWatch();

// Flush pending NVS saves and drop the link. Call before deep sleep or
// power-off. Safe to call when BLE never started.
void prepareForSleep();

}  // namespace BleConfig
