// Rep Counter: NVS persistence. See rep_store.h.
#include "rep_store.h"
#include <Arduino.h>
#include <Preferences.h>

namespace repstore {

static bool readBlob(Preferences &p, const char *key, reps::BlobKind kind, void *out, size_t size) {
  size_t len = p.getBytesLength(key);
  if (len != reps::kBlobHeader + size) return false;
  uint8_t buf[reps::kBlobHeader + sizeof(reps::DayLog)];
  if (len > sizeof(buf)) return false;
  if (p.getBytes(key, buf, len) != len) return false;
  return reps::unpackBlob(kind, buf, len, out, size);
}

static void writeBlob(const char *key, reps::BlobKind kind, const void *in, size_t size) {
  // Static: only the render task saves, and it keeps ~0.5 KB off its stack.
  static uint8_t buf[reps::kBlobHeader + sizeof(reps::DayLog)];
  size_t n = reps::packBlob(kind, in, size, buf, sizeof(buf));
  if (!n) return;
  Preferences p;
  if (!p.begin(kNamespace, false)) return;
  p.putBytes(key, buf, n);
  p.end();
}

void load(reps::Settings &s, reps::DayLog &log, reps::History &hist) {
  Preferences p;
  if (!p.begin(kNamespace, true)) {      // namespace doesn't exist yet
    s = reps::Settings();
    log = reps::DayLog();
    hist = reps::History();
    return;
  }
  if (!readBlob(p, "cfg", reps::kBlobSettings, &s, sizeof(s))) s = reps::Settings();
  if (!readBlob(p, "log", reps::kBlobLog, &log, sizeof(log))) log = reps::DayLog();
  if (!readBlob(p, "hist", reps::kBlobHist, &hist, sizeof(hist))) hist = reps::History();
  p.end();
  s.sanitize();
}

bool loadLog(reps::DayLog &log) {
  Preferences p;
  if (!p.begin(kNamespace, true)) return false;
  bool ok = readBlob(p, "log", reps::kBlobLog, &log, sizeof(log));
  p.end();
  return ok;
}

void saveSettings(const reps::Settings &s) { writeBlob("cfg", reps::kBlobSettings, &s, sizeof(s)); }
void saveLog(const reps::DayLog &log) { writeBlob("log", reps::kBlobLog, &log, sizeof(log)); }
void saveHist(const reps::History &hist) { writeBlob("hist", reps::kBlobHist, &hist, sizeof(hist)); }

}  // namespace repstore
