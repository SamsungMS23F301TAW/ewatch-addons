#include "fr_zones.h"
#include <math.h>
#include <stdio.h>

namespace fr {

float boundaryDb(float meters, float n) {
  if (meters <= 0.f) meters = 0.01f;
  return 10.f * n * log10f(meters);
}

float estimateDistanceM(float rssi, int ref1m, float n) {
  if (n < 1.f) n = 1.f;
  float d = powf(10.f, pathLossDb(rssi, ref1m) / (10.f * n));
  if (d < 0.1f) d = 0.1f;
  if (d > 99.f) d = 99.f;
  return d;
}

static void boundaries(const ZoneConfig &c, float b[3]) {
  b[0] = boundaryDb(c.rightHereM, c.pathLossExp);   // RightHere | Near
  b[1] = boundaryDb(c.nearM, c.pathLossExp);        // Near | Around
  b[2] = boundaryDb(c.aroundM, c.pathLossExp);      // Around | Far
}

Zone classifyPathLoss(float pl, const ZoneConfig &cfg) {
  float b[3];
  boundaries(cfg, b);
  if (pl < b[0]) return Zone::RightHere;
  if (pl < b[1]) return Zone::Near;
  if (pl < b[2]) return Zone::Around;
  return Zone::Far;
}

void formatApproxMeters(float m, char *out, size_t cap) {
  if (m < 1.f)       snprintf(out, cap, "<1 m");
  else if (m >= 10.f) snprintf(out, cap, "10+ m");
  else               snprintf(out, cap, "~%d m", (int)(m + 0.5f));
}

Zone ZoneTracker::update(float rssi, int ref1m, uint32_t nowMs, const ZoneConfig &cfg) {
  float pl = pathLossDb(rssi, ref1m);
  if (!init_) {
    // First fix errs on the far side by one hysteresis step: a genuinely
    // close friend is confirmed within dwellCloserMs, while a noisy first
    // reading cannot fake proximity (which would raise a false alert).
    zone_ = pending_ = classifyPathLoss(pl + cfg.hysteresisDb, cfg);
    pendingSince_ = nowMs;
    init_ = true;
    return zone_;
  }
  float b[3];
  boundaries(cfg, b);
  const float h = cfg.hysteresisDb;

  int cand = (int)zone_;
  while (cand > (int)Zone::RightHere && pl < b[cand - 1] - h) --cand;
  while (cand < (int)Zone::Far && pl > b[cand] + h) ++cand;

  if ((Zone)cand == zone_) {
    pending_ = zone_;
    return zone_;
  }
  if ((Zone)cand != pending_) {
    pending_ = (Zone)cand;
    pendingSince_ = nowMs;
  }
  uint32_t dwell = (cand < (int)zone_) ? cfg.dwellCloserMs : cfg.dwellFartherMs;
  if ((uint32_t)(nowMs - pendingSince_) >= dwell) zone_ = pending_;
  return zone_;
}

}  // namespace fr
