// Tilt Parallax — the only place that knows whether we're on the watch or on
// the host. Big art buffers go to PSRAM on the watch; small hot buffers (the
// composition strip, LUTs) go to internal SRAM.
#pragma once
#include <stddef.h>
#include <stdint.h>

#if defined(ESP_PLATFORM) || defined(ARDUINO)
#define TP_ON_DEVICE 1
#else
#define TP_ON_DEVICE 0
#endif

namespace tp {

void *allocBig(size_t bytes);    // PSRAM when available, else heap
void *allocFast(size_t bytes);   // internal SRAM (fast, DMA-capable); may fail
void  freeMem(void *p);

// Cooperative yield for long-running generators so the IDLE task (and its
// watchdog) gets CPU. No-op on the host.
void  yieldBriefly();

}  // namespace tp
