#include "radar_store.h"
#include <Arduino.h>
#include <Preferences.h>
#include <esp_random.h>
#include <string.h>
#include "fr_beacon.h"

static const char *kNs = "friend-radar";   // 12 chars (NVS limit is 15)
static const uint8_t kSchema = 1;

uint32_t RadarStore::newId() {
  uint32_t id;
  do {
    id = esp_random() ^ (uint32_t)(micros() * 2654435761u);
  } while (!fr::validId(id));
  return id;
}

void RadarStore::load(RadarSettings &s, fr::MateBook &mates) {
  Preferences p;
  bool fresh = false;
  if (!p.begin(kNs, /*readOnly=*/false)) {
    // NVS unavailable: run with an in-memory identity for this boot.
    s.id = newId();
    fr::defaultName(s.id, s.name);
    return;
  }
  s.id = p.getULong("id", 0);
  if (!fr::validId(s.id)) {
    s.id = newId();
    fresh = true;
  }
  String n = p.getString("name", "");
  if (fr::sanitizeName(n.c_str(), s.name) == 0) fr::defaultName(s.id, s.name);
  s.ref1m = p.getChar("ref1m", fr::kDefaultRef1m);
  if (s.ref1m < fr::kMinRef1m || s.ref1m > fr::kMaxRef1m) s.ref1m = fr::kDefaultRef1m;
  s.calibrated = p.getUChar("calib", 0) != 0;
  s.alerts = p.getUChar("alerts", 1) != 0;
  s.bgAlerts = p.getUChar("bgOn", 0) != 0;
  s.bgPeriodSec = p.getUChar("bgPer", 60);
  if (s.bgPeriodSec != 30 && s.bgPeriodSec != 60 && s.bgPeriodSec != 120) s.bgPeriodSec = 60;
  s.clkOffsetSec = p.getInt("clkOff", 0);

  size_t len = p.getBytesLength("mates");
  if (len > 0 && len <= fr::MateBook::maxBlobSize()) {
    static uint8_t buf[2 + fr::kMaxMates * 128];
    if (len <= sizeof buf && p.getBytes("mates", buf, len) == len) {
      if (!mates.deserialize(buf, len)) Serial.println("radar: mates blob rejected");
    }
  }
  if (fresh) {
    p.putUChar("ver", kSchema);
    p.putULong("id", s.id);
  }
  p.end();
}

void RadarStore::saveSettings(const RadarSettings &s) {
  Preferences p;
  if (!p.begin(kNs, false)) return;
  p.putUChar("ver", kSchema);
  p.putULong("id", s.id);
  p.putString("name", s.name);
  p.putChar("ref1m", s.ref1m);
  p.putUChar("calib", s.calibrated ? 1 : 0);
  p.putUChar("alerts", s.alerts ? 1 : 0);
  p.putUChar("bgOn", s.bgAlerts ? 1 : 0);
  p.putUChar("bgPer", s.bgPeriodSec);
  p.putInt("clkOff", s.clkOffsetSec);
  p.end();
}

void RadarStore::saveMates(const fr::MateBook &mates) {
  static uint8_t buf[2 + fr::kMaxMates * 128];
  size_t n = mates.serialize(buf, sizeof buf);
  if (!n) return;
  Preferences p;
  if (!p.begin(kNs, false)) return;
  p.putBytes("mates", buf, n);
  p.end();
}
