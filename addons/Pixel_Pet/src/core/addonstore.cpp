#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "addonstore.h"

namespace addonstore {

static const char *NS = "pixelpet";       // <= 15 chars
static Preferences s_prefs;
static SemaphoreHandle_t s_mutex = nullptr;
static bool s_open = false;

namespace {
struct Lock {
  Lock()  { if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY); }
  ~Lock() { if (s_mutex) xSemaphoreGive(s_mutex); }
};
}

void begin() {
  if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
  Lock lk;
  if (!s_open) s_open = s_prefs.begin(NS, /*readOnly=*/false);
}

bool getBlob(const char *key, void *buf, size_t len) {
  Lock lk;
  if (!s_open) return false;
  if (s_prefs.getBytesLength(key) != len) return false;
  return s_prefs.getBytes(key, buf, len) == len;
}

bool putBlob(const char *key, const void *buf, size_t len) {
  Lock lk;
  if (!s_open) return false;
  return s_prefs.putBytes(key, buf, len) == len;
}

bool getBool(const char *key, bool def) {
  Lock lk;
  return s_open ? s_prefs.getBool(key, def) : def;
}

void putBool(const char *key, bool v) {
  Lock lk;
  if (s_open) s_prefs.putBool(key, v);
}

uint32_t getU32(const char *key, uint32_t def) {
  Lock lk;
  return s_open ? s_prefs.getUInt(key, def) : def;
}

void putU32(const char *key, uint32_t v) {
  Lock lk;
  if (s_open) s_prefs.putUInt(key, v);
}

void remove(const char *key) {
  Lock lk;
  if (s_open) s_prefs.remove(key);
}

uint32_t fnv1a(const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= 16777619u; }
  return h;
}

}  // namespace addonstore
