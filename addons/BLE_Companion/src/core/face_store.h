// FaceStore: the companion's own settings (face data-source slots, custom
// texts, pushed feeds, face options) plus the persisted config revision.
//
// Lives in NVS namespace "ble-companion" (never inside BaseOS's "ewatch"
// namespace). Keep that name if you reuse this module in another addon so a
// user's slot setup carries across installs.
//
// Thread-safe: every function may be called from any task. Mutators bump
// generation() so renderers can cheaply detect changes, bump model.revision so
// the face repaints, and schedule a debounced NVS save. Something must call
// flushIfDue() periodically (ble_config's service task does) and flushNow()
// before deep sleep / power-off.
#pragma once
#include <stdint.h>
#include "ble_proto.h"

namespace FaceStore {

struct Snapshot {
  bleproto::SlotCfg   slots[bleproto::kSlotCount];
  uint8_t             faceOptions;                 // bleproto::kFaceOpt*
  bleproto::TextEntry texts[bleproto::kTextCount];
  bleproto::FeedEntry feeds[bleproto::kFeedCount];
};

// Stock layout: Upper row = seconds, Lower row = date, Top/Bottom empty. With
// these defaults the face is pixel-identical to BaseOS.
void defaults(Snapshot &out);

void begin();                          // load from NVS; call once after Storage::load()
void snapshot(Snapshot &out);          // consistent copy of everything
uint32_t generation();                 // increments on every change (not persisted)

void setSlots(const bleproto::SlotCfg *slots);
void setFaceOptions(uint8_t options);
uint8_t faceOptions();
void setText(uint8_t index, const bleproto::TextEntry &t);
void setFeed(uint8_t index, const bleproto::FeedEntry &f);
void resetToDefaults();

// Monotonic count of applied config changes (any source), persisted.
uint32_t configRevision();
uint32_t bumpConfigRevision();         // returns the new value; schedules a save

// Persistence.
void flushIfDue(uint32_t nowMs);       // save if dirty and quiet for >= 1.5 s
void flushNow();

}  // namespace FaceStore
