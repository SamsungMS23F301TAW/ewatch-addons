// FaceStore — see face_store.h.
#include "face_store.h"

#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

#include "model.h"

using namespace bleproto;

namespace FaceStore {

static const char *kNs = "ble-companion";   // <= 15 chars (NVS limit)
static const uint8_t kSchema = 1;            // bump + migrate if blob layouts change
static const uint32_t kSaveQuietMs = 1500;

static SemaphoreHandle_t sMutex = nullptr;
static Snapshot sState;
static volatile uint32_t sGeneration = 1;
static uint32_t sConfigRev = 0;
static bool sDirty = false;
static uint32_t sDirtyAtMs = 0;

namespace {
struct Lock {
  Lock()  { if (sMutex) xSemaphoreTake(sMutex, portMAX_DELAY); }
  ~Lock() { if (sMutex) xSemaphoreGive(sMutex); }
};

// Packed on-flash layouts (independent of struct padding).
const size_t kTextRec = 1 + kTextMax;                         // len + bytes
const size_t kFeedRec = 1 + kFeedHeaderLen + kFeedTextMax;    // len + hdr + bytes

void packTexts(const TextEntry *t, uint8_t *out) {
  memset(out, 0, kTextCount * kTextRec);
  for (uint8_t i = 0; i < kTextCount; i++) {
    uint8_t *p = out + i * kTextRec;
    p[0] = t[i].len > kTextMax ? kTextMax : t[i].len;
    memcpy(p + 1, t[i].text, p[0]);
  }
}

void unpackTexts(const uint8_t *in, TextEntry *t) {
  for (uint8_t i = 0; i < kTextCount; i++) {
    const uint8_t *p = in + i * kTextRec;
    uint8_t n = p[0] > kTextMax ? kTextMax : p[0];
    t[i].len = n;
    memcpy(t[i].text, p + 1, n);
    t[i].text[n] = 0;
    // A NUL inside the stored bytes would desync len and strlen; trim to it.
    size_t real = strlen(t[i].text);
    if (real < n) t[i].len = (uint8_t)real;
  }
}

void packFeeds(const FeedEntry *f, uint8_t *out) {
  memset(out, 0, kFeedCount * kFeedRec);
  for (uint8_t i = 0; i < kFeedCount; i++) {
    uint8_t *p = out + i * kFeedRec;
    p[0] = f[i].len > kFeedTextMax ? kFeedTextMax : f[i].len;
    p[1] = f[i].icon;
    p[2] = f[i].kind;
    putU32(p + 3, f[i].expiresAt);
    putU32(p + 7, f[i].targetAt);
    memcpy(p + 1 + kFeedHeaderLen, f[i].text, p[0]);
  }
}

void unpackFeeds(const uint8_t *in, FeedEntry *f) {
  for (uint8_t i = 0; i < kFeedCount; i++) {
    const uint8_t *p = in + i * kFeedRec;
    uint8_t n = p[0] > kFeedTextMax ? kFeedTextMax : p[0];
    f[i].len = n;
    f[i].icon = p[1] <= kIconMax ? p[1] : 0;
    f[i].kind = p[2] <= kFeedKindMax ? p[2] : (uint8_t)FEED_VALUE;
    f[i].expiresAt = getU32(p + 3);
    f[i].targetAt = getU32(p + 7);
    memcpy(f[i].text, p + 1 + kFeedHeaderLen, n);
    f[i].text[n] = 0;
    size_t real = strlen(f[i].text);
    if (real < n) f[i].len = (uint8_t)real;
  }
}

// Caller holds the lock.
void markChangedLocked() {
  sGeneration = sGeneration + 1;
  sDirty = true;
  sDirtyAtMs = millis();
}

void nudgeRenderer() {
  ModelLock lk;
  model.revision++;
}
}  // namespace

void defaults(Snapshot &out) {
  memset(&out, 0, sizeof(out));
  out.slots[SLOT_TOP]    = SlotCfg{ SRC_NONE,    0, COLOR_FG, 0 };
  out.slots[SLOT_UPPER]  = SlotCfg{ SRC_SECONDS, 0, COLOR_FG, 0 };
  out.slots[SLOT_LOWER]  = SlotCfg{ SRC_DATE,    0, COLOR_FG, 0 };
  out.slots[SLOT_BOTTOM] = SlotCfg{ SRC_NONE,    0, COLOR_FG, 0 };
  out.faceOptions = 0;
}

void begin() {
  if (!sMutex) sMutex = xSemaphoreCreateMutex();
  Snapshot s;
  defaults(s);
  uint32_t rev = 0;

  Preferences p;
  if (p.begin(kNs, /*readOnly=*/true)) {
    uint8_t schema = p.getUChar("schema", 0);
    if (schema == kSchema) {
      uint8_t slots[kSlotsLen];
      if (p.getBytesLength("slots") == kSlotsLen && p.getBytes("slots", slots, kSlotsLen) == kSlotsLen) {
        SlotCfg tmp[kSlotCount];
        if (decodeSlots(slots, kSlotsLen, tmp) == RES_OK) memcpy(s.slots, tmp, sizeof(tmp));
      }
      s.faceOptions = (uint8_t)(p.getUChar("faceOpt", 0) & kFaceOptMask);
      uint8_t texts[kTextCount * kTextRec];
      if (p.getBytesLength("texts") == sizeof(texts) && p.getBytes("texts", texts, sizeof(texts)) == sizeof(texts)) {
        unpackTexts(texts, s.texts);
      }
      uint8_t feeds[kFeedCount * kFeedRec];
      if (p.getBytesLength("feeds") == sizeof(feeds) && p.getBytes("feeds", feeds, sizeof(feeds)) == sizeof(feeds)) {
        unpackFeeds(feeds, s.feeds);
      }
      rev = p.getULong("rev", 0);
    }
    p.end();
  }
  Lock lk;
  sState = s;
  sConfigRev = rev;
  sDirty = false;
}

void snapshot(Snapshot &out) {
  Lock lk;
  out = sState;
}

uint32_t generation() { return sGeneration; }

void setSlots(const SlotCfg *slots) {
  { Lock lk; memcpy(sState.slots, slots, sizeof(sState.slots)); markChangedLocked(); }
  nudgeRenderer();
}

void setFaceOptions(uint8_t options) {
  { Lock lk; sState.faceOptions = (uint8_t)(options & kFaceOptMask); markChangedLocked(); }
  nudgeRenderer();
}

uint8_t faceOptions() {
  Lock lk;
  return sState.faceOptions;
}

void setText(uint8_t index, const TextEntry &t) {
  if (index >= kTextCount) return;
  { Lock lk; sState.texts[index] = t; markChangedLocked(); }
  nudgeRenderer();
}

void setFeed(uint8_t index, const FeedEntry &f) {
  if (index >= kFeedCount) return;
  { Lock lk; sState.feeds[index] = f; markChangedLocked(); }
  nudgeRenderer();
}

void resetToDefaults() {
  Snapshot d;
  defaults(d);
  { Lock lk; sState = d; markChangedLocked(); }
  nudgeRenderer();
}

uint32_t configRevision() {
  Lock lk;
  return sConfigRev;
}

uint32_t bumpConfigRevision() {
  Lock lk;
  sConfigRev++;
  if (!sDirty) { sDirty = true; sDirtyAtMs = millis(); }
  return sConfigRev;
}

static void saveLocked() {
  uint8_t slots[kSlotsLen];
  uint8_t texts[kTextCount * kTextRec];
  uint8_t feeds[kFeedCount * kFeedRec];
  encodeSlots(sState.slots, slots);
  packTexts(sState.texts, texts);
  packFeeds(sState.feeds, feeds);
  uint8_t opts = sState.faceOptions;
  uint32_t rev = sConfigRev;
  sDirty = false;

  Preferences p;
  if (!p.begin(kNs, /*readOnly=*/false)) return;
  p.putUChar("schema", kSchema);
  p.putBytes("slots", slots, sizeof(slots));
  p.putUChar("faceOpt", opts);
  p.putBytes("texts", texts, sizeof(texts));
  p.putBytes("feeds", feeds, sizeof(feeds));
  p.putULong("rev", rev);
  p.end();
}

void flushIfDue(uint32_t nowMs) {
  Lock lk;
  if (!sDirty || (nowMs - sDirtyAtMs) < kSaveQuietMs) return;
  saveLocked();
}

void flushNow() {
  Lock lk;
  if (sDirty) saveLocked();
}

}  // namespace FaceStore
