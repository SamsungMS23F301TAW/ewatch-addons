// Shared-animation handshake.
//
// Hello ("a mate is near"):
//   A notices mate B arriving and starts advertising a Wave event targeted at
//   B (a 6-bit sequence number makes it a fresh event). A starts its own
//   animation `leadMs` after the wave goes on air, which is roughly how long B
//   takes to hear it. B, on hearing a fresh wave addressed to it from one of
//   its own mates, starts the same animation immediately. Both seed the
//   animation from both watch ids (order-independent), so the frames match.
//   If B hears the wave late (missed packets, or a background window), the
//   advertised event age lets B join mid-animation at the matching frame.
//
// Bump to celebrate:
//   Each shake is advertised as an untargeted Shake event. A watch that sees
//   its own shake and a close mate's shake within shakePairMs of each other
//   starts a celebration and advertises a Celebrate event targeted at that
//   mate. A watch that receives a Celebrate addressed to it joins if its own
//   wearer shook recently, even if its own distance estimate disagreed, so
//   the outcome stays symmetric.
//
// All decisions are pure functions of the inputs, so the whole exchange is
// simulated end to end in the host tests.
#pragma once
#include "fr_beacon.h"

namespace fr {

struct HandshakeConfig {
  uint32_t leadMs         = 80;     // initiator delay ~= typical reception latency
  uint32_t waveTtlMs      = 30000;  // a wave stays on air this long (reaches background windows)
  uint32_t shakeTtlMs     = 3000;
  uint32_t celebrateTtlMs = 4000;
  uint32_t shakePairMs    = 1500;   // both wrists shaken within this window
  uint32_t recentShakeMs  = 3000;   // to join a partner's celebration
  uint32_t joinLateMs     = 300;    // events older than this are joined mid-animation
  uint32_t helloDurMs     = 3600;   // animation lengths (must match the renderer)
  uint32_t celebrateDurMs = 3000;
  uint32_t dedupeMs       = 8000;   // one hello / celebration per peer per this window
  uint32_t resyncMinMs    = 120;    // re-align a playing hello if the mate started this much earlier
};

struct PlayCmd {
  AnimKind kind      = AnimKind::None;
  uint32_t peerId    = 0;
  uint32_t seed      = 0;
  uint32_t startMs   = 0;     // local ms at animation t = 0 (may be in the past)
  bool     initiator = false;
  bool     resync    = false; // same animation already playing: just move t = 0 earlier
};

constexpr uint32_t kSaltHello     = 0x48454c4fu;   // "HELO"
constexpr uint32_t kSaltCelebrate = 0x43454c42u;   // "CELB"

// What the handshake needs to know about peers, supplied by the engine.
class PeerFacts {
public:
  virtual ~PeerFacts() = default;
  virtual bool isMate(uint32_t id) const = 0;
  // Close enough to pair a shake: Right here by our own (lenient) estimate.
  virtual bool isClose(uint32_t id) const = 0;
};

class Handshake {
public:
  Handshake() { reset(0); }
  void reset(uint32_t myId);
  void setConfig(const HandshakeConfig &c) { cfg_ = c; }
  const HandshakeConfig &config() const { return cfg_; }

  // We fired an arrival alert for `mateId`: advertise a wave and play.
  PlayCmd startHello(uint32_t mateId, uint32_t nowMs);

  // Our wearer shook. May start a celebration with a close mate whose shake
  // we already heard. Returns true and fills `play` if so.
  bool localShake(uint32_t nowMs, const PeerFacts &facts, PlayCmd &play);

  // A beacon from `b.id` arrived. Returns true and fills `play` if an
  // animation should start. `helloAccepted` is consulted for incoming waves
  // (lets the alert policy apply its per-mate rate limit).
  bool onBeacon(const Beacon &b, uint32_t nowMs, const PeerFacts &facts,
                bool (*helloAccepted)(void *ctx, uint32_t peerId), void *ctx,
                PlayCmd &play);

  // Event fields for our own beacon at `nowMs`.
  void fillBeacon(Beacon &b, uint32_t nowMs) const;

  // True while some event is on air (the beacon age byte needs refreshing).
  bool eventActive(uint32_t nowMs) const;

  uint32_t lastLocalShakeMs() const { return haveShake_ ? shakeMs_ : 0; }

private:
  struct Slot { bool on = false; uint8_t seq = 0; uint16_t target = 0; uint32_t sinceMs = 0; };
  struct PeerRec {
    bool     used = false;
    uint32_t id = 0;
    uint8_t  seenMask = 0;          // bit k: lastSeq[k] valid for EventKind k
    uint8_t  lastSeq[4] = {0, 0, 0, 0};
    bool     haveShake = false;
    uint32_t shakeMs = 0;           // when the peer shook (our clock)
    bool     helloPlayed = false;   uint32_t helloMs = 0;   uint32_t helloStartMs = 0;
    bool     celebPlayed = false;   uint32_t celebMs = 0;
    uint32_t lastSeenMs = 0;
  };

  PeerRec *rec(uint32_t id, uint32_t nowMs);
  uint8_t nextSeq();
  PlayCmd makePlay(AnimKind k, uint32_t peer, uint32_t startMs, bool initiator) const;
  bool startCelebrate(PeerRec &r, uint32_t nowMs, uint32_t startMs, bool initiator, PlayCmd &play);

  HandshakeConfig cfg_;
  uint32_t myId_ = 0;
  uint16_t myTag_ = 1;
  uint8_t  seq_ = 0;
  Slot     wave_, shake_, celeb_;
  bool     haveShake_ = false;
  uint32_t shakeMs_ = 0;
  PeerRec  peers_[kMaxPeers];
};

}  // namespace fr
