// Tilt Parallax — background scene generation on core 0.
//
// Waking from deep sleep is a full reboot, and setup() spends most of its
// first second waiting on hardware (touch reset and drain, panel init). The
// art is pure computation into PSRAM, so parallaxBootPrewarm() starts this
// task at the top of setup() and the scene is usually finished before the
// display is even initialised. Later scene changes reuse the same task while
// the face keeps animating on core 1.
#pragma once
#include <stdint.h>
#include "tp_scene.h"

// Loads settings and starts generating the saved scene. Call early in
// setup(), after Serial.begin().
void parallaxBootPrewarm();

// Asks for scene `id`. Newer requests supersede older ones.
void parallaxGenRequest(int id);

// If a requested scene is ready, hands it over (caller owns it: call
// parallaxGenRecycle() when done). Returns nullptr otherwise.
tp::Scene *parallaxGenTake();

// Returns a scene's memory. Safe from the render task.
void parallaxGenRecycle(tp::Scene *s);

bool parallaxGenBusy();
uint32_t parallaxGenLastMs();     // duration of the last build
