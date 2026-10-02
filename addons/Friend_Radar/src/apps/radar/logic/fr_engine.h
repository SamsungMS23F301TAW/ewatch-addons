// RadarEngine: the whole radar brain, free of any radio or display code.
//
// The device layer feeds it decoded beacons with their RSSI (onBeacon), the
// passage of time (tick) and local shakes (onLocalShake). It answers with
// Effects to carry out (start an animation, buzz, re-advertise, adjust the
// rendezvous clock) and with snapshots for the UI. The same engine runs the
// foreground radar and the short background windows.
#pragma once
#include "fr_alerts.h"
#include "fr_beacon.h"
#include "fr_clock.h"
#include "fr_handshake.h"
#include "fr_mates.h"
#include "fr_rssi.h"
#include "fr_zones.h"

namespace fr {

struct EngineConfig {
  ZoneConfig      zones;
  AlertConfig     alerts;
  HandshakeConfig hs;
  bool     alertsEnabled      = true;    // arrival alerts + accepting hello waves
  uint16_t firstFixSamples    = 3;       // first zone fix from a median of 3, not one packet
  uint16_t minSamplesForAlert = 5;       // never alert on a handful of packets
  uint32_t forgetMs           = 60000;   // lost peers fade from the dial after this
  float    closeSlackDb       = 4.0f;    // "close enough to pair a shake" leniency
};

// A live EWatch as the UI sees it.
struct PeerView {
  uint32_t id = 0;
  char     label[kMaxName + 1] = {0};  // nickname for mates, else advertised name
  char     name[kMaxName + 1] = {0};   // advertised name
  bool     mate = false;
  Zone     zone = Zone::Lost;
  float    rssi = 0.f;       // filtered dBm
  float    plDb = 0.f;       // path loss vs the sender's 1 m reference
  float    distM = 0.f;      // secondary display only
  float    bandPos = 0.5f;   // 0 = inner edge of the zone band, 1 = outer edge
  uint32_t ageMs = 0;        // since the last packet
  uint32_t seenForMs = 0;    // since first heard this session
  uint8_t  flags = 0;
  int8_t   ref1m = 0;
  uint16_t samples = 0;
};

// What the caller must do after an engine call.
struct Effects {
  PlayCmd play;                  // kind != None: start this animation
  bool    buzzHello = false;
  bool    buzzCelebrate = false;
  bool    beaconDirty = false;   // our advertisement content changed
  bool    clockAdjust = false;   // shift our radar clock by clockDeltaSec
  int     clockDeltaSec = 0;
  uint32_t alertMate = 0;        // id of the mate we alerted for (0 = none)
};

struct MateAlertEntry {
  uint32_t       id = 0;
  MateAlertState st;
};

class RadarEngine : private PeerFacts {
public:
  RadarEngine() { begin(0); }
  void begin(uint32_t myId);

  EngineConfig       &config()       { return cfg_; }
  const EngineConfig &config() const { return cfg_; }
  MateBook           &mates()        { return mates_; }
  const MateBook     &mates()  const { return mates_; }
  uint32_t            myId()   const { return myId_; }

  // Our advertised identity.
  void setName(const char *name);
  void setRef1m(int8_t ref1m, bool calibrated);
  void setBaseFlags(uint8_t flags);     // kFlagBackground / kFlagAlerts
  const char *name() const { return name_; }
  int8_t ref1m() const { return ref1m_; }

  void onBeacon(const Beacon &b, int rssi, uint32_t nowMs, uint32_t nowSec,
                uint8_t myClk, Effects &fx);
  void tick(uint32_t nowMs, uint32_t nowSec, Effects &fx);
  void onLocalShake(uint32_t nowMs, Effects &fx);

  void buildBeacon(Beacon &out, uint32_t nowMs, uint8_t clk) const;
  bool eventOnAir(uint32_t nowMs) const { return hs_.eventActive(nowMs); }

  // UI snapshot, closest first. Returns the number written.
  int  snapshot(PeerView *out, int max, uint32_t nowMs) const;
  int  liveCount(uint32_t nowMs) const;
  bool strongestPeer(uint32_t nowMs, uint32_t &id) const;
  // Display label for a watch: mate nickname, else its advertised name.
  void labelFor(uint32_t id, char *out) const;

  // Mate management (keeps alert state consistent).
  bool addMate(uint32_t id, const char *nick, uint32_t nowSec);
  bool removeMate(uint32_t id);

  // Calibration capture: raw RSSI from one peer.
  void      calStart(uint32_t peerId);
  void      calStop() { calPeer_ = 0; }
  uint32_t  calPeer() const { return calPeer_; }
  uint16_t  calCount() const { return calN_; }
  const int8_t *calSamples() const { return calBuf_; }
  int       calLastRaw() const { return calLast_; }

  // Alert state survives deep sleep via the device layer.
  int  exportAlerts(MateAlertEntry *out, int max) const;
  void importAlerts(const MateAlertEntry *in, int n);

  // Zone band position helper (also used by the renderer's previews).
  static float bandPosition(Zone z, float plDb, const ZoneConfig &zc);

private:
  struct Peer {
    bool       used = false;
    uint32_t   id = 0;
    char       name[kMaxName + 1] = {0};
    int8_t     ref1m = kDefaultRef1m;
    uint8_t    flags = 0;
    uint8_t    clk = 0;
    RssiFilter filt;
    ZoneTracker zt;
    Zone       zone = Zone::Lost;
    uint32_t   firstMs = 0, lastMs = 0;
  };

  bool isMate(uint32_t id) const override { return mates_.isMate(id); }
  bool isClose(uint32_t id) const override;
  static bool helloThunk(void *ctx, uint32_t peerId);

  Peer       *findPeer(uint32_t id);
  const Peer *findPeer(uint32_t id) const;
  Peer       *observe(const Beacon &b, int rssi, uint32_t nowMs);
  MateAlertState &alertState(uint32_t id);
  void applyPlay(const PlayCmd &p, Effects &fx);

  EngineConfig cfg_;
  uint32_t     myId_ = 0;
  char         name_[kMaxName + 1] = {0};
  int8_t       ref1m_ = kDefaultRef1m;
  bool         calibrated_ = false;
  uint8_t      baseFlags_ = 0;
  MateBook     mates_;
  Handshake    hs_;
  Peer         peers_[kMaxPeers];
  MateAlertEntry alerts_[kMaxMates];
  uint32_t     nowSecCache_ = 0;
  uint32_t     lastClockAdjustMs_ = 0;
  bool         clockAdjusted_ = false;

  static constexpr uint16_t kCalCap = 200;
  uint32_t calPeer_ = 0;
  uint16_t calN_ = 0;
  int8_t   calBuf_[kCalCap];
  int      calLast_ = 0;
};

}  // namespace fr
