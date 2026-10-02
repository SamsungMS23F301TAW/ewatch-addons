#include "artslots.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "console.h"

static ArtSlot sFace, sShared;
static SemaphoreHandle_t sSharedMutex = nullptr;

static void *psram(size_t n) {
  return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static bool init(ArtSlot &s, const char *name) {
  if (s.ready) return true;
  size_t px = (size_t)gf::kW * gf::kH;
  if (!s.canvas.px) s.canvas.px = (uint32_t *)psram(px * 4);
  if (!s.scratch) s.scratch = (uint8_t *)psram(gf::kScratchBytes);
  if (!s.maskA) s.maskA = (uint8_t *)psram(px);
  if (!s.maskB) s.maskB = (uint8_t *)psram(px);
  if (!s.canvas.px || !s.scratch || !s.maskA || !s.maskB) {
    gfLog("Art    : %s slot allocation failed (PSRAM free %u)\n", name,
          (unsigned)ESP.getFreePsram());
    return false;
  }
  s.canvas.w = gf::kW;
  s.canvas.h = gf::kH;
  s.layer.maskText = s.maskA;
  s.layer.maskHalo = s.maskB;
  s.layer.valid = false;
  s.ready = true;
  gfLog("Art    : %s slot ready (PSRAM free %u)\n", name, (unsigned)ESP.getFreePsram());
  return true;
}

// Both slots are allocated in setup(), before any task can race to do it
// (the shared slot is used from the render task and the console's loop).
ArtSlot *artSlotFace()   { return sFace.ready ? &sFace : nullptr; }
ArtSlot *artSlotShared() { return sShared.ready ? &sShared : nullptr; }

void artSlotsInit() {
  if (!sSharedMutex) sSharedMutex = xSemaphoreCreateMutex();
  init(sFace, "face");
  init(sShared, "shared");
}

bool artSharedLock(uint32_t waitMs) {
  if (!sSharedMutex) return false;
  return xSemaphoreTake(sSharedMutex, pdMS_TO_TICKS(waitMs)) == pdTRUE;
}

void artSharedUnlock() {
  if (sSharedMutex) xSemaphoreGive(sSharedMutex);
}

bool artRunFor(gf::ArtJob &job, uint32_t budgetUs) {
  uint32_t t0 = micros();
  for (;;) {
    if (job.run(2)) return true;
    if (micros() - t0 >= budgetUs) return false;
  }
}
