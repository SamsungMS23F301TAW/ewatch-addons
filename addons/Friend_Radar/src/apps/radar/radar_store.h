// Friend Radar persistence: the addon's own NVS namespace "friend-radar".
// BaseOS settings in "ewatch" are never touched. Saves happen on change and
// on exit, never per frame.
#pragma once
#include <stdint.h>
#include "fr_mates.h"

struct RadarSettings {
  uint32_t id = 0;              // random persistent watch id
  char     name[fr::kMaxName + 1] = {0};
  int8_t   ref1m = -60;         // calibrated RSSI at 1 m
  bool     calibrated = false;
  bool     alerts = true;       // mate arrival alerts (buzz + animation)
  bool     bgAlerts = false;    // background mate alerts (stretch, default off)
  uint8_t  bgPeriodSec = 60;    // 30, 60 or 120
  int32_t  clkOffsetSec = 0;    // rendezvous clock offset (learned from mates)
};

namespace RadarStore {
// Loads settings (creating a fresh random id on first run) and the mate book.
void load(RadarSettings &s, fr::MateBook &mates);
void saveSettings(const RadarSettings &s);
void saveMates(const fr::MateBook &mates);
uint32_t newId();               // a fresh random id (not saved)
}
