#include "parallax_store.h"
#include <Arduino.h>
#include <Preferences.h>
#include "tp_config.h"
#include "tp_scene.h"

static const char *kNs = "tilt-parallax";   // 13 chars (NVS limit is 15)
static const uint8_t kSchema = 1;

static ParallaxSettings sCur;
static ParallaxSettings sSaved;
static bool sLoaded = false;

ParallaxSettings &parallaxSettings() { return sCur; }

static void sanitise(ParallaxSettings &s) {
  if (s.scene >= tp::sceneCount()) s.scene = 0;
  if (s.depth >= tp::kStrengthCount) s.depth = tp::kStrengthDefault;
}

void parallaxSettingsLoad() {
  Preferences p;
  ParallaxSettings d;
  if (p.begin(kNs, /*readOnly=*/true)) {
    uint8_t ver = p.getUChar("ver", 0);
    if (ver == kSchema) {
      d.scene = p.getUChar("scene", d.scene);
      d.depth = p.getUChar("depth", d.depth);
      d.h12 = p.getBool("h12", d.h12);
      d.ambient = p.getBool("ambient", d.ambient);
      d.classicFace = p.getBool("classic", d.classicFace);
      d.raiseToWake = p.getBool("raise", d.raiseToWake);
    }
    p.end();
  }
  sanitise(d);
  sCur = d;
  sSaved = d;
  sLoaded = true;
}

void parallaxSettingsSave() {
  sanitise(sCur);
  if (sLoaded && sCur.scene == sSaved.scene && sCur.depth == sSaved.depth &&
      sCur.h12 == sSaved.h12 && sCur.ambient == sSaved.ambient &&
      sCur.classicFace == sSaved.classicFace && sCur.raiseToWake == sSaved.raiseToWake)
    return;                                  // nothing changed: no flash write
  Preferences p;
  if (!p.begin(kNs, /*readOnly=*/false)) return;
  p.putUChar("ver", kSchema);
  if (sCur.scene != sSaved.scene) p.putUChar("scene", sCur.scene);
  if (sCur.depth != sSaved.depth) p.putUChar("depth", sCur.depth);
  if (sCur.h12 != sSaved.h12) p.putBool("h12", sCur.h12);
  if (sCur.ambient != sSaved.ambient) p.putBool("ambient", sCur.ambient);
  if (sCur.classicFace != sSaved.classicFace) p.putBool("classic", sCur.classicFace);
  if (sCur.raiseToWake != sSaved.raiseToWake) p.putBool("raise", sCur.raiseToWake);
  p.end();
  sSaved = sCur;
}
