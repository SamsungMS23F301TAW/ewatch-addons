// Tilt Parallax — FaceRenderer: everything between "here is a scene, a time
// and a tilt" and "here are the pixels for rows y0..y1".
//
// Owns the time-dependent layers (sky, clock, its shadow, the status line)
// and interleaves them with the scene's terrain layers by depth. Tracks which
// screen rows changed since the last frame so the watch only recomposes and
// re-sends those. Pure C++: the host preview tool drives the same object.
#pragma once
#include <stdint.h>
#include "tp_config.h"
#include "tp_layer.h"
#include "tp_scene.h"
#include "tp_sky.h"
#include "tp_text.h"

namespace tp {

struct StatusInfo {
  char date[24];       // e.g. "WED 1 OCT"; empty = hidden
  int  battery;        // 0..100, < 0 = unknown (hidden)
  StatusInfo() : battery(-1) { date[0] = 0; }
};

class FaceRenderer {
public:
  bool begin();                 // allocate own layers; false on OOM
  void end();

  // Adopts `s` for rendering (not owned). nullptr shows sky + clock only.
  void setScene(Scene *s);
  Scene *scene() const { return scene_; }

  // Recomputes the sky state; re-renders the sky and every LUT when the
  // time-of-day bucket changed (or force). Returns true when it re-rendered.
  bool setTimeOfDay(const LocalTime &t, bool force = false);
  const SkyState &sky() const { return sky_; }

  // Clock text. valid=false shows "--:--".
  void setClock(int hour, int minute, bool valid, bool h12);
  void setStatus(const StatusInfo &st);

  // Normalised tilt in screen orientation (x right, y down), each ~[-1,1],
  // and the strength scale (0..1.5). Applies per-layer integer offsets with
  // hysteresis; returns true if any offset changed.
  bool setParallax(float tx, float ty, float strength);

  // Extra offset for one terrain layer (scene switch animation).
  void setLayerAnim(int sceneLayer, int dx, int dy);
  // Extra offset for the cloud layer (ambient drift).
  void setCloudDrift(int dx);
  // Ambient: re-blend a few bright stars by time (ms); marks only their rows.
  // Returns true while there are visible stars to animate.
  bool twinkle(uint32_t ms);
  bool starsVisible() const { return twinkle_.n > 0 && sky_.starAmt > 0.15f; }
  // True when the current scene shows the drifting cloud band.
  bool hasClouds() const { return cloudsActive_; }
  int  cloudPeriod() const { return clouds_.w > 0 ? clouds_.w : 1; }
  // Current extra (animation) offset of a terrain layer.
  int  layerAnimDy(int sceneLayer) const;

  // Dirty rows.
  void markAllDirty();
  void markRows(int y0, int y1);
  bool anyDirty() const;
  // Pops the next contiguous dirty run (at most maxRows tall). False if none.
  bool popDirtyRun(int &y0, int &y1, int maxRows = kScreenH);
  int  dirtyCount() const;

  void composeRows(int y0, int y1, uint16_t *dst, int stride);

  // Diagnostics.
  size_t bytes() const;
  int    stackSize() const { return stackN_; }

private:
  void rebuildStack();
  void layerExtent(const Layer &L, int &y0, int &y1) const;
  void applyOffset(Layer &L, int dx, int dy);
  void renderClock();
  void renderStatus();
  void buildClouds();
  void buildGlows();
  void relightScene();

  Scene   *scene_ = nullptr;
  SkyState sky_;
  SkyStyle skyStyle_;
  int      todBucket_ = -1;
  int      sceneIdLit_ = -2;

  Layer    skyL_, clouds_, shadow_, clock_, statusShadow_, status_, glow_;
  LayerLook cloudLook_;
  bool     cloudsActive_ = false;
  TwinkleSet twinkle_;
  Layer   *stack_[Scene::kMaxLayers + 8];
  int      stackN_ = 0;

  char     clockText_[8] = { 0 };
  StatusInfo status_info_;
  bool     clockDirty_ = true, statusDirty_ = true;

  float    tiltX_ = 0, tiltY_ = 0, strength_ = 1;

  uint8_t  dirty_[kScreenH];
};

}  // namespace tp
