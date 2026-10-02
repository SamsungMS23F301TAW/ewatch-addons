#include "fr_engine.h"
#include <string.h>

namespace fr {

void RadarEngine::begin(uint32_t myId) {
  myId_ = myId;
  hs_.reset(myId);
  hs_.setConfig(cfg_.hs);
  for (auto &p : peers_) p = Peer();
  for (auto &a : alerts_) a = MateAlertEntry();
  if (!name_[0]) defaultName(myId, name_);
  calPeer_ = 0; calN_ = 0; calLast_ = 0;
  lastClockAdjustMs_ = 0; clockAdjusted_ = false;
}

void RadarEngine::setName(const char *n) {
  if (sanitizeName(n, name_) == 0) defaultName(myId_, name_);
}

void RadarEngine::setRef1m(int8_t ref, bool calibrated) {
  if (ref < kMinRef1m) ref = kMinRef1m;
  if (ref > kMaxRef1m) ref = kMaxRef1m;
  ref1m_ = ref;
  calibrated_ = calibrated;
}

void RadarEngine::setBaseFlags(uint8_t f) { baseFlags_ = f & (kFlagBackground | kFlagAlerts); }

RadarEngine::Peer *RadarEngine::findPeer(uint32_t id) {
  for (auto &p : peers_) if (p.used && p.id == id) return &p;
  return nullptr;
}
const RadarEngine::Peer *RadarEngine::findPeer(uint32_t id) const {
  for (auto &p : peers_) if (p.used && p.id == id) return &p;
  return nullptr;
}

RadarEngine::Peer *RadarEngine::observe(const Beacon &b, int rssi, uint32_t nowMs) {
  Peer *p = findPeer(b.id);
  if (!p) {
    Peer *victim = nullptr;
    for (auto &q : peers_) {
      if (!q.used) { victim = &q; break; }
      // Prefer evicting non-mates, then the longest-unheard.
      bool qm = mates_.isMate(q.id);
      if (!victim) { victim = &q; continue; }
      bool vm = mates_.isMate(victim->id);
      if (vm && !qm) { victim = &q; continue; }
      if (vm == qm && (int32_t)(q.lastMs - victim->lastMs) < 0) victim = &q;
    }
    *victim = Peer();
    p = victim;
    p->used = true;
    p->id = b.id;
    p->firstMs = nowMs;
  }
  if (p->zone == Zone::Lost && p->zt.initialised()) {
    // Back in range after being lost: start the estimate afresh.
    p->filt.reset();
    p->zt.reset();
    p->firstMs = nowMs;
  }
  memcpy(p->name, b.name, sizeof(p->name));
  p->ref1m = b.ref1m;
  p->flags = b.flags;
  p->clk = b.clk;
  p->filt.add(rssi, nowMs);
  p->lastMs = nowMs;
  if (p->filt.valid() && (p->zt.initialised() || p->filt.count() >= cfg_.firstFixSamples))
    p->zone = p->zt.update(p->filt.value(), p->ref1m, nowMs, cfg_.zones);
  return p;
}

MateAlertState &RadarEngine::alertState(uint32_t id) {
  MateAlertEntry *freeE = nullptr;
  for (auto &a : alerts_) {
    if (a.id == id) return a.st;
    if (!freeE && (a.id == 0 || !mates_.isMate(a.id))) freeE = &a;
  }
  if (!freeE) freeE = &alerts_[0];
  *freeE = MateAlertEntry();
  freeE->id = id;
  return freeE->st;
}

bool RadarEngine::isClose(uint32_t id) const {
  const Peer *p = findPeer(id);
  if (!p || p->zone == Zone::Lost || !p->filt.valid()) return false;
  if (p->zone == Zone::RightHere) return true;
  if (p->zone != Zone::Near) return false;
  float b0 = boundaryDb(cfg_.zones.rightHereM, cfg_.zones.pathLossExp);
  return pathLossDb(p->filt.value(), p->ref1m) <= b0 + cfg_.closeSlackDb;
}

bool RadarEngine::helloThunk(void *ctx, uint32_t peerId) {
  RadarEngine *e = static_cast<RadarEngine *>(ctx);
  if (!e->cfg_.alertsEnabled) return false;
  return acceptWave(e->alertState(peerId), e->nowSecCache_, e->cfg_.alerts);
}

void RadarEngine::applyPlay(const PlayCmd &p, Effects &fx) {
  if (p.kind == AnimKind::None) return;
  fx.play = p;
  fx.beaconDirty = true;
  if (p.resync) return;                       // already buzzed for this one
  if (p.kind == AnimKind::Hello) { fx.buzzHello = true; fx.alertMate = p.peerId; }
  else fx.buzzCelebrate = true;
}

void RadarEngine::onBeacon(const Beacon &b, int rssi, uint32_t nowMs, uint32_t nowSec,
                           uint8_t myClk, Effects &fx) {
  if (!validId(b.id) || b.id == myId_) return;
  nowSecCache_ = nowSec;
  hs_.setConfig(cfg_.hs);
  Peer *p = observe(b, rssi, nowMs);
  const bool mate = mates_.isMate(b.id);

  if (calPeer_ == b.id && rssi < 0 && rssi >= -110) {
    if (calN_ < kCalCap) calBuf_[calN_++] = (int8_t)rssi;
    calLast_ = rssi;
  }

  if (mate) mates_.noteSeen(b.id, p->zone, nowSec);

  // 1. Events the sender is advertising (hello waves, shakes, celebrations).
  PlayCmd pc;
  if (hs_.onBeacon(b, nowMs, *this, &RadarEngine::helloThunk, this, pc)) applyPlay(pc, fx);

  // 2. Our own arrival alert for this mate.
  if (mate && cfg_.alertsEnabled && p->filt.count() >= cfg_.minSamplesForAlert &&
      fx.play.kind == AnimKind::None) {
    if (alertOnZone(alertState(b.id), p->zone, nowSec, cfg_.alerts)) {
      applyPlay(hs_.startHello(b.id, nowMs), fx);
    }
  }

  // 3. Rendezvous clock: follow mates with a lower id (rate limited).
  int d = clkDelta(myClk, b.clk);
  if (shouldAdoptClock(myId_, b.id, mate, d) &&
      (!clockAdjusted_ || (uint32_t)(nowMs - lastClockAdjustMs_) > 10000)) {
    fx.clockAdjust = true;
    fx.clockDeltaSec = d;
    clockAdjusted_ = true;
    lastClockAdjustMs_ = nowMs;
  }
}

void RadarEngine::tick(uint32_t nowMs, uint32_t nowSec, Effects &fx) {
  (void)fx;
  nowSecCache_ = nowSec;
  for (auto &p : peers_) {
    if (!p.used) continue;
    uint32_t age = nowMs - p.lastMs;
    if (age > cfg_.zones.lostAfterMs) p.zone = Zone::Lost;
    if (age > cfg_.forgetMs) p = Peer();
  }
}

void RadarEngine::onLocalShake(uint32_t nowMs, Effects &fx) {
  PlayCmd pc;
  bool started = hs_.localShake(nowMs, *this, pc);
  fx.beaconDirty = true;               // shake goes on air either way
  if (started) applyPlay(pc, fx);
}

void RadarEngine::buildBeacon(Beacon &out, uint32_t nowMs, uint8_t clk) const {
  out = Beacon();
  out.id = myId_;
  out.ref1m = ref1m_;
  out.flags = (uint8_t)(baseFlags_ | (calibrated_ ? kFlagCalibrated : 0));
  out.clk = (uint8_t)(clk % 120);
  memcpy(out.name, name_, sizeof(out.name));
  hs_.fillBeacon(out, nowMs);
}

float RadarEngine::bandPosition(Zone z, float pl, const ZoneConfig &zc) {
  float b0 = boundaryDb(zc.rightHereM, zc.pathLossExp);
  float b1 = boundaryDb(zc.nearM, zc.pathLossExp);
  float b2 = boundaryDb(zc.aroundM, zc.pathLossExp);
  float lo, hi;
  switch (z) {
    case Zone::RightHere: lo = b0 - 10.f; hi = b0; break;
    case Zone::Near:      lo = b0; hi = b1; break;
    case Zone::Around:    lo = b1; hi = b2; break;
    case Zone::Far:       lo = b2; hi = b2 + 12.f; break;
    default:              return 1.f;
  }
  float t = (pl - lo) / (hi - lo);
  if (t < 0.f) t = 0.f;
  if (t > 1.f) t = 1.f;
  return t;
}

int RadarEngine::snapshot(PeerView *out, int max, uint32_t nowMs) const {
  int n = 0;
  for (const auto &p : peers_) {
    if (!p.used || !p.filt.valid() || !p.zt.initialised() || n >= max) continue;
    PeerView v;
    v.id = p.id;
    memcpy(v.name, p.name, sizeof(v.name));
    const char *nick = mates_.nickOf(p.id);
    v.mate = nick != nullptr;
    memcpy(v.label, nick ? nick : p.name, sizeof(v.label));
    if (!v.label[0]) defaultName(p.id, v.label);
    v.zone = (nowMs - p.lastMs > cfg_.zones.lostAfterMs) ? Zone::Lost : p.zone;
    v.rssi = p.filt.value();
    v.plDb = pathLossDb(v.rssi, p.ref1m);
    v.distM = estimateDistanceM(v.rssi, p.ref1m, cfg_.zones.pathLossExp);
    v.bandPos = bandPosition(v.zone, v.plDb, cfg_.zones);
    v.ageMs = nowMs - p.lastMs;
    v.seenForMs = nowMs - p.firstMs;
    v.flags = p.flags;
    v.ref1m = p.ref1m;
    v.samples = p.filt.count();
    out[n++] = v;
  }
  // Closest zone first, mates before others, then stronger signal.
  for (int i = 1; i < n; ++i) {
    PeerView v = out[i];
    int j = i - 1;
    auto before = [](const PeerView &a, const PeerView &b) {
      if (a.zone != b.zone) return (int)a.zone < (int)b.zone;
      if (a.mate != b.mate) return a.mate;
      return a.rssi > b.rssi;
    };
    while (j >= 0 && before(v, out[j])) { out[j + 1] = out[j]; --j; }
    out[j + 1] = v;
  }
  return n;
}

int RadarEngine::liveCount(uint32_t nowMs) const {
  int n = 0;
  for (const auto &p : peers_)
    if (p.used && p.filt.valid() && nowMs - p.lastMs <= cfg_.zones.lostAfterMs) ++n;
  return n;
}

bool RadarEngine::strongestPeer(uint32_t nowMs, uint32_t &id) const {
  const Peer *best = nullptr;
  for (const auto &p : peers_) {
    if (!p.used || !p.filt.valid() || nowMs - p.lastMs > 2500) continue;
    if (!best || p.filt.value() > best->filt.value()) best = &p;
  }
  if (!best) return false;
  id = best->id;
  return true;
}

void RadarEngine::labelFor(uint32_t id, char *out) const {
  const char *nick = mates_.nickOf(id);
  if (nick) { memcpy(out, nick, kMaxName + 1); return; }
  const Peer *p = findPeer(id);
  if (p && p->name[0]) { memcpy(out, p->name, kMaxName + 1); return; }
  defaultName(id, out);
}

bool RadarEngine::addMate(uint32_t id, const char *nick, uint32_t nowSec) {
  const Peer *p = findPeer(id);
  const char *n = (nick && nick[0]) ? nick : (p ? p->name : "");
  if (!mates_.add(id, n, nowSec)) return false;
  // A freshly added mate who is already right next to us should not
  // immediately buzz: treat them as already greeted.
  MateAlertState &st = alertState(id);
  st = MateAlertState();
  if (p && p->zone != Zone::Lost && isNearZone(p->zone)) {
    st.nearSeen = true; st.lastNear = nowSec;
    st.alerted = true;  st.lastAlert = nowSec;
  }
  if (p) mates_.noteSeen(id, p->zone, nowSec);
  return true;
}

bool RadarEngine::removeMate(uint32_t id) {
  for (auto &a : alerts_) if (a.id == id) a = MateAlertEntry();
  return mates_.remove(id);
}

void RadarEngine::calStart(uint32_t peerId) {
  calPeer_ = peerId;
  calN_ = 0;
  calLast_ = 0;
}

int RadarEngine::exportAlerts(MateAlertEntry *out, int max) const {
  int n = 0;
  for (const auto &a : alerts_)
    if (a.id != 0 && mates_.isMate(a.id) && n < max) out[n++] = a;
  return n;
}

void RadarEngine::importAlerts(const MateAlertEntry *in, int n) {
  for (auto &a : alerts_) a = MateAlertEntry();
  int k = 0;
  for (int i = 0; i < n && k < kMaxMates; ++i)
    if (validId(in[i].id) && mates_.isMate(in[i].id)) alerts_[k++] = in[i];
}

}  // namespace fr
