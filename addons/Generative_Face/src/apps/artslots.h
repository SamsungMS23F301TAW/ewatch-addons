// PSRAM buffers for rendering art on the watch.
//   * the FACE slot holds today's piece for the watch face (kept between
//     screen-offs, so waking is instant);
//   * the SHARED slot renders any other day, for the Gallery app and the
//     serial SNAP command; it is guarded by a mutex.
// Each slot: a 240x280 0x00RRGGBB canvas (263 KB), the generator scratch
// (160 KB) and two 8-bit text masks (131 KB). Both are allocated once, in
// artSlotsInit() during setup.
#pragma once
#include <stdint.h>
#include "gf_art.h"
#include "gf_face.h"

struct ArtSlot {
  gf::Canvas canvas;
  uint8_t   *scratch = nullptr;
  uint8_t   *maskA = nullptr, *maskB = nullptr;
  gf::ArtJob job;
  gf::FaceLayer layer;
  uint8_t    owner = 0;           // who began `job` (kOwner*), see below
  bool       ready = false;
};

// Owners of the shared slot's current job. A user that finds another owner
// in the slot must begin its job again (the art was overwritten).
static const uint8_t kOwnerNone = 0, kOwnerGallery = 1, kOwnerSnap = 2;

void     artSlotsInit();            // allocate both slots + the mutex (call from setup)

ArtSlot *artSlotFace();             // nullptr if PSRAM is exhausted
ArtSlot *artSlotShared();
bool     artSharedLock(uint32_t waitMs);
void     artSharedUnlock();

// Runs `job` for at most `budgetUs` microseconds; returns true when done.
bool     artRunFor(gf::ArtJob &job, uint32_t budgetUs);
