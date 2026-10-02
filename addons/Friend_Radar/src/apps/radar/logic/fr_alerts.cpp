#include "fr_alerts.h"

namespace fr {

bool alertOnZone(MateAlertState &s, Zone z, uint32_t now, const AlertConfig &c) {
  if (!isNearZone(z)) return false;
  bool arrival = !s.nearSeen || (uint32_t)(now - s.lastNear) >= c.awayGraceSec;
  s.nearSeen = true;
  s.lastNear = now;
  if (!arrival) return false;
  if (s.alerted && (uint32_t)(now - s.lastAlert) < c.cooldownSec) return false;
  s.alerted = true;
  s.lastAlert = now;
  return true;
}

bool acceptWave(MateAlertState &s, uint32_t now, const AlertConfig &c) {
  if (s.waveRx && (uint32_t)(now - s.lastWaveRx) < c.waveAcceptSec) return false;
  s.waveRx = true;     s.lastWaveRx = now;
  s.alerted = true;    s.lastAlert = now;
  s.nearSeen = true;   s.lastNear = now;
  return true;
}

uint32_t cooldownLeft(const MateAlertState &s, uint32_t now, const AlertConfig &c) {
  if (!s.alerted) return 0;
  uint32_t el = (uint32_t)(now - s.lastAlert);
  return el >= c.cooldownSec ? 0 : c.cooldownSec - el;
}

}  // namespace fr
