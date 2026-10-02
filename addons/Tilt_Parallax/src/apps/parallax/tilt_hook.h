// Tilt Parallax — accelerometer sample hook.
//
// taskIO calls tiltHookPush() right after every successful readAccel(), so
// the tilt filter sees every sample with its own timestamp instead of
// whatever happens to be in `model` when the face renders. The push is a few
// stores into a lock-free ring and returns immediately; it does nothing
// unless the parallax face is on screen.
#pragma once
#include <stdint.h>
#include "tp_ring.h"

void tiltHookPush(int16_t ax, int16_t ay, int16_t az, uint32_t tMs);   // taskIO only
void tiltHookEnable(bool on);                                          // face only
bool tiltHookPop(tp::ImuSample &out);                                  // face only
void tiltHookClear();                                                  // face only
uint32_t tiltHookDropped();
