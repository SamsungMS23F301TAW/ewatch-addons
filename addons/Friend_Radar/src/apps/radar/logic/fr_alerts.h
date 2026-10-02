// Mate arrival alerts.
//
// A mate "arrives" when we see them in Near or Right here after they have
// been away (not seen near) for at least awayGraceSec. An arrival fires an
// alert only if this mate's cooldown has expired, so a friend sitting next to
// you, or hovering on a zone edge, buzzes you at most once per cooldown.
// The same rules run in the foreground radar and in background windows, which
// is why time here is RTC seconds (it survives deep sleep).
#pragma once
#include "fr_types.h"

namespace fr {

struct AlertConfig {
  uint32_t cooldownSec   = 600;   // per mate, ~10 minutes
  uint32_t awayGraceSec  = 90;    // must be away this long to "arrive" again
  uint32_t waveAcceptSec = 120;   // accept at most one incoming hello per mate per 2 min
};

struct MateAlertState {
  uint32_t lastAlert  = 0;  bool alerted = false;
  uint32_t lastNear   = 0;  bool nearSeen = false;
  uint32_t lastWaveRx = 0;  bool waveRx = false;
};

static inline bool isNearZone(Zone z) { return z == Zone::RightHere || z == Zone::Near; }

// Feed every committed zone observation for a mate. Returns true when this
// observation is an arrival that should alert now (and records the alert).
bool alertOnZone(MateAlertState &s, Zone z, uint32_t nowSec, const AlertConfig &c);

// An incoming hello wave from this mate (addressed to us). Returns true if we
// should play along. Accepting counts as an alert: it starts the cooldown and
// marks the mate as near, so we do not fire a second alert of our own.
bool acceptWave(MateAlertState &s, uint32_t nowSec, const AlertConfig &c);

// Seconds of cooldown remaining (0 = ready).
uint32_t cooldownLeft(const MateAlertState &s, uint32_t nowSec, const AlertConfig &c);

}  // namespace fr
