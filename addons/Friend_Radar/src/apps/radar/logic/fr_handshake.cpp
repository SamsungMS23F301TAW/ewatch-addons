#include "fr_handshake.h"

namespace fr {

void Handshake::reset(uint32_t myId) {
  myId_ = myId;
  myTag_ = targetTag(myId);
  seq_ = (uint8_t)(mix32(myId) & 0x3F);   // start somewhere arbitrary
  wave_ = Slot(); shake_ = Slot(); celeb_ = Slot();
  haveShake_ = false; shakeMs_ = 0;
  for (auto &p : peers_) p = PeerRec();
}

uint8_t Handshake::nextSeq() {
  seq_ = (uint8_t)((seq_ + 1) & 0x3F);
  return seq_;
}

Handshake::PeerRec *Handshake::rec(uint32_t id, uint32_t nowMs) {
  PeerRec *freeSlot = nullptr, *oldest = nullptr;
  for (auto &p : peers_) {
    if (p.used && p.id == id) { p.lastSeenMs = nowMs; return &p; }
    if (!p.used && !freeSlot) freeSlot = &p;
    if (p.used && (!oldest || (int32_t)(p.lastSeenMs - oldest->lastSeenMs) < 0)) oldest = &p;
  }
  PeerRec *r = freeSlot ? freeSlot : oldest;
  *r = PeerRec();
  r->used = true;
  r->id = id;
  r->lastSeenMs = nowMs;
  return r;
}

PlayCmd Handshake::makePlay(AnimKind k, uint32_t peer, uint32_t startMs, bool initiator) const {
  PlayCmd p;
  p.kind = k;
  p.peerId = peer;
  p.seed = pairSeed(myId_, peer, k == AnimKind::Hello ? kSaltHello : kSaltCelebrate);
  p.startMs = startMs;
  p.initiator = initiator;
  return p;
}

PlayCmd Handshake::startHello(uint32_t mateId, uint32_t nowMs) {
  wave_.on = true;
  wave_.seq = nextSeq();
  wave_.target = targetTag(mateId);
  wave_.sinceMs = nowMs;
  PeerRec *r = rec(mateId, nowMs);
  r->helloPlayed = true;
  r->helloMs = nowMs;
  r->helloStartMs = nowMs + cfg_.leadMs;
  return makePlay(AnimKind::Hello, mateId, r->helloStartMs, true);
}

bool Handshake::startCelebrate(PeerRec &r, uint32_t nowMs, uint32_t startMs,
                               bool initiator, PlayCmd &play) {
  if (r.celebPlayed && (uint32_t)(nowMs - r.celebMs) < cfg_.dedupeMs) return false;
  r.celebPlayed = true;
  r.celebMs = nowMs;
  celeb_.on = true;
  celeb_.seq = nextSeq();
  celeb_.target = targetTag(r.id);
  celeb_.sinceMs = nowMs;
  play = makePlay(AnimKind::Celebrate, r.id, startMs, initiator);
  return true;
}

bool Handshake::localShake(uint32_t nowMs, const PeerFacts &facts, PlayCmd &play) {
  haveShake_ = true;
  shakeMs_ = nowMs;
  shake_.on = true;
  shake_.seq = nextSeq();
  shake_.target = 0;
  shake_.sinceMs = nowMs;
  // Pair with a close mate who shook within the window just before us.
  PeerRec *best = nullptr;
  for (auto &p : peers_) {
    if (!p.used || !p.haveShake) continue;
    if ((uint32_t)(nowMs - p.shakeMs) > cfg_.shakePairMs) continue;
    if (!facts.isMate(p.id) || !facts.isClose(p.id)) continue;
    if (!best || (int32_t)(p.shakeMs - best->shakeMs) > 0) best = &p;
  }
  if (!best) return false;
  return startCelebrate(*best, nowMs, nowMs + cfg_.leadMs, true, play);
}

bool Handshake::onBeacon(const Beacon &b, uint32_t nowMs, const PeerFacts &facts,
                         bool (*helloAccepted)(void *, uint32_t), void *ctx,
                         PlayCmd &play) {
  PeerRec *r = rec(b.id, nowMs);
  uint8_t k = (uint8_t)b.evKind;
  if (b.evKind == EventKind::None) return false;
  bool firstSight = !(r->seenMask & (1u << k));
  bool fresh = firstSight || r->lastSeq[k] != b.evSeq;
  r->seenMask |= (uint8_t)(1u << k);
  r->lastSeq[k] = b.evSeq;
  if (!fresh) return false;

  uint32_t ageMs = (uint32_t)b.evAge * 100u;
  bool forMe = (b.evTarget == myTag_);

  switch (b.evKind) {
    case EventKind::Shake: {
      if (ageMs > cfg_.shakeTtlMs) return false;
      r->haveShake = true;
      r->shakeMs = nowMs - ageMs;
      // Did we shake just before (or at the same moment as) them?
      if (haveShake_ && (uint32_t)(r->shakeMs - shakeMs_ + cfg_.shakePairMs) <= 2 * cfg_.shakePairMs &&
          (uint32_t)(nowMs - shakeMs_) <= cfg_.recentShakeMs &&
          facts.isMate(b.id) && facts.isClose(b.id)) {
        return startCelebrate(*r, nowMs, nowMs, false, play);
      }
      return false;
    }
    case EventKind::Wave: {
      if (!forMe || !facts.isMate(b.id)) return false;
      if (ageMs > cfg_.waveTtlMs) return false;
      // The sender's t=0 was (event start + lead); join at that frame if late.
      uint32_t start = (ageMs < cfg_.joinLateMs) ? nowMs : nowMs - ageMs + cfg_.leadMs;
      if (r->helloPlayed && (uint32_t)(nowMs - r->helloMs) < cfg_.dedupeMs) {
        // Both of us started a hello on our own (we noticed each other at
        // about the same time). Converge on whichever started first.
        if ((int32_t)(r->helloStartMs - start) > (int32_t)cfg_.resyncMinMs) {
          r->helloStartMs = start;
          play = makePlay(AnimKind::Hello, b.id, start, false);
          play.resync = true;
          return true;
        }
        return false;
      }
      if (helloAccepted && !helloAccepted(ctx, b.id)) return false;
      r->helloPlayed = true;
      r->helloMs = nowMs;
      r->helloStartMs = start;
      play = makePlay(AnimKind::Hello, b.id, start, false);
      return true;
    }
    case EventKind::Celebrate: {
      if (!forMe || !facts.isMate(b.id)) return false;
      if (ageMs > cfg_.celebrateDurMs) return false;            // moment has passed
      if (!haveShake_ || (uint32_t)(nowMs - shakeMs_) > cfg_.recentShakeMs) return false;
      uint32_t start = (ageMs < cfg_.joinLateMs) ? nowMs : nowMs - ageMs + cfg_.leadMs;
      return startCelebrate(*r, nowMs, start, false, play);
    }
    default:
      return false;
  }
}

void Handshake::fillBeacon(Beacon &b, uint32_t nowMs) const {
  const Slot *s = nullptr;
  EventKind kind = EventKind::None;
  if (celeb_.on && (uint32_t)(nowMs - celeb_.sinceMs) < cfg_.celebrateTtlMs) {
    s = &celeb_; kind = EventKind::Celebrate;
  } else if (shake_.on && (uint32_t)(nowMs - shake_.sinceMs) < cfg_.shakeTtlMs) {
    s = &shake_; kind = EventKind::Shake;
  } else if (wave_.on && (uint32_t)(nowMs - wave_.sinceMs) < cfg_.waveTtlMs) {
    s = &wave_; kind = EventKind::Wave;
  }
  if (!s) {
    b.evKind = EventKind::None; b.evSeq = 0; b.evTarget = 0; b.evAge = 0;
    return;
  }
  uint32_t age = (uint32_t)(nowMs - s->sinceMs) / 100u;
  b.evKind = kind;
  b.evSeq = s->seq;
  b.evTarget = s->target;
  b.evAge = (uint8_t)(age > 255 ? 255 : age);
}

bool Handshake::eventActive(uint32_t nowMs) const {
  return (celeb_.on && (uint32_t)(nowMs - celeb_.sinceMs) < cfg_.celebrateTtlMs) ||
         (shake_.on && (uint32_t)(nowMs - shake_.sinceMs) < cfg_.shakeTtlMs) ||
         (wave_.on  && (uint32_t)(nowMs - wave_.sinceMs)  < cfg_.waveTtlMs);
}

}  // namespace fr
