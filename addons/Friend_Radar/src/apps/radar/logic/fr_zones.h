// Distance model and zone tracking.
//
// Log-distance path loss with the sender's advertised 1 m reference:
//     rssi(d) = ref1m - 10 n log10(d)   =>   d = 10 ^ ((ref1m - rssi) / (10 n))
// Rather than show falsely precise metres, the radar reports coarse zones.
// Boundaries are compared in the path-loss domain (ref1m - rssi, dB) with a
// symmetric hysteresis band, and a candidate zone must persist for a dwell
// time before it is committed (shorter when moving closer, so alerts are
// prompt; longer when moving away, so the display does not flap).
#pragma once
#include "fr_types.h"

namespace fr {

struct ZoneConfig {
  float    pathLossExp   = 2.5f;   // n: ~2 free space, 2.5-3 indoors / on the body
  float    rightHereM    = 0.7f;   // closer than this -> Right here
  float    nearM         = 3.0f;   // -> Near
  float    aroundM       = 10.0f;  // -> Around; beyond -> Far
  float    hysteresisDb  = 3.0f;   // must cross a boundary by this much
  uint32_t dwellCloserMs = 500;    // tuned in test_filter_zones: Near ~2.4 s after
  uint32_t dwellFartherMs = 2000;  // crossing 3 m on foot, no flapping at rest
  uint32_t lostAfterMs   = 10000;  // no packets for this long -> Lost
};

// Path loss in dB relative to the 1 m reference (negative when closer than 1 m).
static inline float pathLossDb(float rssi, int ref1m) { return (float)ref1m - rssi; }

// Path-loss boundary (dB) corresponding to `meters` for exponent n.
float boundaryDb(float meters, float n);

// Estimated distance in metres (secondary display only; clamped to 0.1..99).
float estimateDistanceM(float rssi, int ref1m, float n);

// Zone from a path-loss value with no hysteresis (used for first fixes and
// one-shot background classification).
Zone classifyPathLoss(float plDb, const ZoneConfig &cfg);

// Human "~2 m" style text for a distance estimate, e.g. "<1 m", "~4 m", "10+ m".
void formatApproxMeters(float meters, char *out, size_t cap);

class ZoneTracker {
public:
  ZoneTracker() { reset(); }
  void reset() { init_ = false; zone_ = pending_ = Zone::Lost; pendingSince_ = 0; }

  // Feed a filtered RSSI estimate; returns the committed zone (never Lost).
  // The first fix is classified one hysteresis step farther than measured.
  Zone update(float rssi, int ref1m, uint32_t nowMs, const ZoneConfig &cfg);

  Zone zone()        const { return zone_; }
  Zone pending()     const { return pending_; }
  bool initialised() const { return init_; }

private:
  bool     init_;
  Zone     zone_, pending_;
  uint32_t pendingSince_;
};

}  // namespace fr
