#include "daystore.h"
#include <new>
#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "console.h"

static gf::DayLog *sLog = nullptr;           // ~9 KB, allocated on first use
static SemaphoreHandle_t sLock = nullptr;
static bool sMounted = false;                // LittleFS is mounted
static bool sReady = false;                  // the collection is loaded; saves allowed
static bool sTried = false;
static const char *kDir  = "/generative-face";
static const char *kFile = "/generative-face/days.bin";
static const char *kTmp  = "/generative-face/days.tmp";
static const char *kBad  = "/generative-face/days.bad";
static const char *kNs   = "generative-face";
static const uint8_t kFormatAfterFails = 3;

namespace {
struct Lock {
  Lock()  { if (sLock) xSemaphoreTake(sLock, portMAX_DELAY); }
  ~Lock() { if (sLock) xSemaphoreGive(sLock); }
};

void *psram(size_t n) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : malloc(n);
}

void noteMounted() {
  Preferences p;
  if (!p.begin(kNs, false)) return;
  if (p.getUChar("fsSeen", 0) != 1) p.putUChar("fsSeen", 1);
  if (p.getUChar("fsFail", 0) != 0) p.putUChar("fsFail", 0);
  p.end();
}

// A partition that has never mounted (first use, or one another firmware
// left behind) is formatted. One that has mounted before is formatted only
// after failing on kFormatAfterFails separate boots: a one-off glitch must
// not wipe a year of days.
bool mount() {
  if (LittleFS.begin(false, "/littlefs", 10, "spiffs")) {
    noteMounted();
    return true;
  }
  uint8_t seen = 0, fails = 0;
  Preferences p;
  if (p.begin(kNs, true)) {
    seen = p.getUChar("fsSeen", 0);
    fails = p.getUChar("fsFail", 0);
    p.end();
  }
  if (seen && fails + 1 < kFormatAfterFails) {
    if (p.begin(kNs, false)) { p.putUChar("fsFail", (uint8_t)(fails + 1)); p.end(); }
    gfLog("Days   : storage did not mount (%u of %u); not formatting, collection offline this boot\n",
          (unsigned)(fails + 1), (unsigned)kFormatAfterFails);
    return false;
  }
  if (seen) gfLog("Days   : storage unreadable on %u boots; formatting it\n", (unsigned)kFormatAfterFails);
  else      gfLog("Days   : preparing storage (first use)\n");
  if (!LittleFS.format() || !LittleFS.begin(false, "/littlefs", 10, "spiffs")) {
    gfLog("Days   : storage format failed; collection offline\n");
    return false;
  }
  noteMounted();
  return true;
}

// Reads a whole file. Returns false only for I/O or memory trouble; an
// empty file reads as zero bytes.
bool readAll(const char *path, uint8_t *&buf, size_t &n) {
  buf = nullptr;
  n = 0;
  File f = LittleFS.open(path, FILE_READ);
  if (!f) return false;
  n = f.size();
  if (n == 0) { f.close(); return true; }
  buf = (uint8_t *)psram(n);
  bool ok = buf && f.read(buf, n) == n;
  f.close();
  if (!ok && buf) { free(buf); buf = nullptr; }
  return ok;
}

// Loads days.bin, plus days.tmp if a save was interrupted. A file that
// fails its checks is salvaged record by record and kept as days.bad; it
// is never saved over. False means the files couldn't be read at all.
bool load() {
  bool hasFile = LittleFS.exists(kFile), hasTmp = LittleFS.exists(kTmp);
  bool fileDamaged = false;
  int32_t salvaged = 0;
  const char *paths[2] = { kFile, kTmp };
  bool present[2] = { hasFile, hasTmp };
  for (int i = 0; i < 2; i++) {
    if (!present[i]) continue;
    uint8_t *buf;
    size_t n;
    if (!readAll(paths[i], buf, n)) return false;
    if (gf::DayLog::intact(buf, n)) {
      sLog->merge(buf, n);
    } else {
      salvaged += sLog->merge(buf, n);
      if (i == 0) fileDamaged = true;
    }
    free(buf);                                // free(nullptr) is fine
  }
  if (fileDamaged) {
    LittleFS.remove(kBad);
    LittleFS.rename(kFile, kBad);
    gfLog("Days   : days.bin failed its checks; %ld record(s) recovered, original kept as days.bad\n",
          (long)salvaged);
  }
  if (hasFile || hasTmp) gfLog("Days   : %ld recorded days loaded\n", (long)sLog->count());
  return true;
}

// Mount + load once per boot. Caller holds the lock.
bool ensure() {
  if (sReady) return true;
  if (sTried) return false;
  sTried = true;
  if (!sLog) {
    void *mem = psram(sizeof(gf::DayLog));
    if (!mem) { gfLog("Days   : no memory for the collection\n"); return false; }
    sLog = new (mem) gf::DayLog();
  }
  sLog->clear();
  if (!mount()) return false;
  sMounted = true;
  if (!LittleFS.exists(kDir)) LittleFS.mkdir(kDir);
  if (!load()) {
    // Never save an empty collection over one we merely failed to read.
    sLog->clear();
    gfLog("Days   : collection could not be read; offline this boot\n");
    return false;
  }
  sReady = true;
  return true;
}

bool save() {
  size_t n = sLog->serializedSize();
  uint8_t *buf = (uint8_t *)psram(n);
  if (!buf) return false;
  bool ok = false;
  if (sLog->serialize(buf, n) == n) {
    File f = LittleFS.open(kTmp, FILE_WRITE);
    if (f) {
      ok = f.write(buf, n) == n;
      f.close();
      if (ok) {
        // If power fails between these two, load() finds days.tmp.
        LittleFS.remove(kFile);
        ok = LittleFS.rename(kTmp, kFile);
      }
    }
  }
  free(buf);
  if (!ok) gfLog("Days   : save failed\n");
  return ok;
}
}  // namespace

void daystoreInit() {
  if (!sLock) sLock = xSemaphoreCreateMutex();
}

bool daystoreAdd(const gf::DayRecord &r) {
  Lock lk;
  if (!ensure()) return false;
  sLog->upsert(r);
  return save();
}

int32_t daystoreCount() {
  Lock lk;
  return ensure() ? sLog->count() : 0;
}

bool daystoreAt(int32_t i, gf::DayRecord &out) {
  Lock lk;
  if (!ensure() || i < 0 || i >= sLog->count()) return false;
  out = sLog->at(i);
  return true;
}

bool daystoreFind(uint16_t day, gf::DayRecord &out) {
  Lock lk;
  if (!ensure()) return false;
  int32_t i = sLog->find(day);
  if (i < 0) return false;
  out = sLog->at(i);
  return true;
}

bool daystoreMounted() { return sMounted; }
bool daystoreAttempted() { return sTried; }
bool daystoreAvailable() { return sReady; }
