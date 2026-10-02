// Addon persistence: Pixel Pet's own NVS namespace ("pixelpet"). The shared
// BaseOS namespace "ewatch" is never touched from here, so a user's theme,
// WiFi list and haptics survive installing or removing this addon.
//
// Thread-safe (internal mutex). Writes hit flash: call on state changes and
// checkpoints, never per frame. Prefer the render task (flash writes can take
// tens of ms when a page needs erasing).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace addonstore {

void   begin();
// Blob get: true only if the stored length matches `len` exactly.
bool   getBlob(const char *key, void *buf, size_t len);
bool   putBlob(const char *key, const void *buf, size_t len);
bool   getBool(const char *key, bool def);
void   putBool(const char *key, bool v);
uint32_t getU32(const char *key, uint32_t def);
void   putU32(const char *key, uint32_t v);
void   remove(const char *key);

// FNV-1a — integrity check for RTC-memory mirrors.
uint32_t fnv1a(const void *data, size_t len);

}  // namespace addonstore
