#include "tp_platform.h"
#include <stdlib.h>

#if TP_ON_DEVICE
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace tp {

void *allocBig(size_t bytes) {
#if TP_ON_DEVICE
  void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
  return p;
#else
  return malloc(bytes);
#endif
}

void *allocFast(size_t bytes) {
#if TP_ON_DEVICE
  return heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
  return malloc(bytes);
#endif
}

void freeMem(void *p) {
#if TP_ON_DEVICE
  heap_caps_free(p);
#else
  free(p);
#endif
}

void yieldBriefly() {
#if TP_ON_DEVICE
  vTaskDelay(1);
#endif
}

}  // namespace tp
