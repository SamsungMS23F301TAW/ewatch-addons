// The scope: a lit sonar instrument (v2).
//
//   static layer   room, drop shadow, lacquered gunmetal bezel with engraved
//                  ticks and lettering, deep phosphor glass with grain
//   content        etched zone grooves, zone names set along the bottom arc,
//                  the phosphor sweep, you, the blips (mates are glowing orbs)
//                  -- all drawn through a camera so the scope can zoom in and
//                  lock onto a mate
//   glass overlay  inner rim shadow + the glass reflection, applied on top
//   chrome         bezel arc lettering + on-air lamp, corner buttons, footer,
//                  and the detail card (a glass sheet that springs up)
//
// RSSI carries no direction, so a blip's angle is a fixed decorative
// position derived from its id (kept clear of the label sector at the
// bottom); only distance from the centre means anything.
#pragma once
#include "fr_anim.h"
#include "fr_engine.h"
#include "fr_ui.h"

namespace fr {

enum class RadioState : uint8_t { Off, Starting, Live, Error };

struct Blip {
  uint32_t id = 0;
  char     label[kMaxName + 1] = {0};
  bool     mate = false;
  Zone     zone = Zone::Lost;
  float    angleDeg = 0.f;   // 0 = up, clockwise; decorative only
  float    radius = 0.f;     // animated, px from the centre (overview)
  uint8_t  alpha = 255;      // fade in / out
  float    scale = 1.f;      // pop-in
  bool     selected = false;
  bool     asleep = false;   // heard from a background window
  uint16_t color = 0;        // personal colour (mates) or phosphor (others)
  float    signal = 0.f;     // 0..1, drives the breathing halo
};

struct RadarHud {
  RadioState radio = RadioState::Starting;
  char myName[kMaxName + 1] = {0};
  bool bgAlerts = false;
  char footer[56] = {0};
  bool searching = false;
};

struct CardInfo {
  bool     show = false;
  uint32_t id = 0;
  char     name[kMaxName + 1] = {0};
  char     advertised[kMaxName + 1] = {0};
  bool     mate = false;
  Zone     zone = Zone::Lost;
  float    distM = 0.f;
  uint32_t agoSec = 0;
  int      bars = 0;          // 0..5 signal bars
  bool     asleep = false;
  bool     confirmRemove = false;
  uint8_t  pressed = 0;       // 0 none, 1 left/wide, 2 right
  uint16_t color = 0;
  float    signal = 0.f;
};

// Camera: screen = t + s * (overviewPosition - f).
struct RadarCamera {
  float fx = (float)layout::cx, fy = (float)layout::cy;
  float tx = (float)layout::cx, ty = (float)layout::cy;
  float s = 1.f;
  float focus = 0.f;          // 0 overview .. 1 locked on
  static RadarCamera overview() { return RadarCamera(); }
  // Lock onto an overview-space point; u is the spring position (may overshoot).
  static RadarCamera lockOn(float bx, float by, float u);
  void apply(float x, float y, float &sx, float &sy) const { sx = tx + s * (x - fx); sy = ty + s * (y - fy); }
};

struct RadarFrame {
  uint32_t tMs = 0;
  float    beamDeg = 0.f;
  const Blip *blips = nullptr;
  int      nBlips = 0;
  RadarHud hud;
  CardInfo card;
  float    cardY = (float)layout::H;   // card top edge (springs); >= H = hidden
  RadarCamera cam;
  uint32_t focusId = 0;
  float    boot = 1.f;                 // 0..1 power-on sequence
  bool     backPressed = false, menuPressed = false;
  const PairAnimSpec *anim = nullptr;  // shared animation playing inside the glass
  int32_t  animT = 0;
};

namespace dial {
constexpr int D = 2 * layout::Rg + 1;                  // polar / overlay LUT side
constexpr size_t kBgPixels = (size_t)layout::W * layout::H;
constexpr size_t kPolarEntries = (size_t)D * D;
constexpr float kFocusScale = 1.55f;
constexpr float kFocusX = 120.f, kFocusY = 90.f;      // where a locked-on mate sits
}

class RadarScene {
public:
  // Caller-owned buffers (PSRAM on the watch): staticLayer dial::kBgPixels;
  // polar, overlay and cleanGlass dial::kPolarEntries uint16 each. The static
  // layer carries the overview's grooves and zone names; cleanGlass is the
  // bare glass, used while the camera zooms or the scope powers on.
  void attach(uint16_t *staticLayer, uint16_t *polar, uint16_t *overlay, uint16_t *cleanGlass) {
    bg_ = staticLayer; polar_ = polar; overlay_ = overlay; clean_ = cleanGlass; built_ = false;
  }
  bool ready() const { return built_; }
  void build();
  void drawFrame(Canvas &out, const RadarFrame &f) const;

  static void blipScreenXY(const Blip &b, const RadarCamera &cam, float &x, float &y);
  static void blipOverviewXY(const Blip &b, float &x, float &y);
  static int  hitBlip(const Blip *blips, int n, const RadarCamera &cam, int x, int y);

private:
  void drawContent(Canvas &out, const RadarFrame &f, bool grooves) const;
  static void drawGrooves(Canvas &out, float ccx, float ccy, float s, float boot, uint8_t contentA,
                          uint8_t labelA);
  void drawBeam(Canvas &out, float beamDeg, uint8_t a) const;
  void drawBlips(Canvas &out, const RadarFrame &f) const;
  void restoreOutsideGlass(Canvas &out) const;
  void applyGlass(Canvas &out) const;
  void drawChrome(Canvas &out, const RadarFrame &f) const;
  void drawCard(Canvas &out, const RadarFrame &f) const;

  uint16_t *bg_ = nullptr, *polar_ = nullptr, *overlay_ = nullptr, *clean_ = nullptr;
  bool built_ = false;
  // Per-frame shortcuts computed by build(): the sweep skips 16 px tiles its
  // trail cannot reach, the glass pass skips the clear middle of each row.
  static constexpr int kTile = 16, kTiles = (dial::D + kTile - 1) / kTile, kTrail = 96;
  uint8_t tileA0_[kTiles * kTiles] = {0}, tileSpan_[kTiles * kTiles] = {0};
  uint8_t ovFirst_[dial::D] = {0}, ovZ0_[dial::D] = {0}, ovZ1_[dial::D] = {0}, ovLast_[dial::D] = {0};
  uint8_t trail_[kTrail] = {0}, radial_[256] = {0};
};

// Turns engine snapshots into smoothly moving blips.
class BlipAnimator {
public:
  void reset() { for (auto &t : tr_) t = Track(); lastMs_ = 0; }
  void update(const PeerView *peers, int n, uint32_t nowMs);
  void step(uint32_t nowMs);
  int  blips(Blip *out, int max, uint32_t selectedId) const;
  bool find(uint32_t id, Blip &out) const;

  static float targetRadius(Zone z, float bandPos);
  static float signalFor(float plDb);

private:
  struct Track {
    bool     used = false;
    uint32_t id = 0;
    char     label[kMaxName + 1] = {0};
    bool     mate = false, asleep = false;
    Zone     zone = Zone::Lost;
    float    r = 0.f, target = 0.f;
    float    alpha = 0.f, alphaTarget = 255.f;
    float    signal = 0.f;
    uint32_t bornMs = 0;
    bool     present = false;
  };
  Track    tr_[kMaxPeers];
  uint32_t lastMs_ = 0;
};

}  // namespace fr
