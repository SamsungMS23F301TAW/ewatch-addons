#include "parallax_gen.h"
#include <Arduino.h>
#include <atomic>
#include <new>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include "parallax_store.h"
#include "tp_platform.h"

static TaskHandle_t sTask = nullptr;
static QueueHandle_t sReq = nullptr;
static std::atomic<tp::Scene *> sReady{ nullptr };
static std::atomic<int> sLatest{ -1 };        // newest requested id
static std::atomic<bool> sBusy{ false };
static std::atomic<uint32_t> sLastMs{ 0 };

static tp::Scene *newScene() {
  void *mem = tp::allocBig(sizeof(tp::Scene));
  if (!mem) return nullptr;
  return new (mem) tp::Scene();
}

void parallaxGenRecycle(tp::Scene *s) {
  if (!s) return;
  s->release();
  s->~Scene();
  tp::freeMem(s);
}

static void genTask(void *) {
  for (;;) {
    int id;
    if (xQueueReceive(sReq, &id, portMAX_DELAY) != pdPASS) continue;
    // Coalesce: only build the newest request.
    int newer;
    while (xQueueReceive(sReq, &newer, 0) == pdPASS) id = newer;
    sBusy.store(true);
    uint32_t t0 = millis();
    tp::Scene *s = newScene();
    bool ok = s && tp::buildScene(id, *s);
    uint32_t dt = millis() - t0;
    sLastMs.store(dt);
    if (!ok) {
      Serial.printf("tp: scene %d build FAILED (free PSRAM %u)\n", id,
                    (unsigned)ESP.getFreePsram());
      parallaxGenRecycle(s);
      sBusy.store(false);
      continue;
    }
    s->buildMs = dt;
    Serial.printf("tp: scene %d (%s) built in %lu ms, %u KB\n", id, tp::sceneName(id),
                  (unsigned long)dt, (unsigned)(s->bytes() / 1024));
    if (sLatest.load() != id) {               // superseded while we worked
      parallaxGenRecycle(s);
      sBusy.store(false);
      continue;
    }
    tp::Scene *old = sReady.exchange(s);
    if (old) parallaxGenRecycle(old);         // never collected; drop it
    sBusy.store(false);
  }
}

static void ensureTask() {
  if (sTask) return;
  sReq = xQueueCreate(4, sizeof(int));
  // Core 0, below taskIO (5): taskIO preempts us whenever it has work, and
  // the generator yields between layers so IDLE0 keeps the watchdog happy.
  xTaskCreatePinnedToCore(genTask, "tp_gen", 8192, nullptr, 2, &sTask, 0);
}

void parallaxGenRequest(int id) {
  ensureTask();
  sLatest.store(id);
  xQueueSend(sReq, &id, 0);
}

tp::Scene *parallaxGenTake() {
  tp::Scene *s = sReady.exchange(nullptr);
  if (s && s->id != sLatest.load()) {         // stale result
    parallaxGenRecycle(s);
    return nullptr;
  }
  return s;
}

bool parallaxGenBusy() { return sBusy.load() || sReady.load() != nullptr; }
uint32_t parallaxGenLastMs() { return sLastMs.load(); }

void parallaxBootPrewarm() {
  parallaxSettingsLoad();
  parallaxGenRequest(parallaxSettings().scene);
}
