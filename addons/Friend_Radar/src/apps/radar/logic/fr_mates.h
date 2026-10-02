// Saved mates: id, local nickname, and a short per-mate encounter log.
//
// An encounter starts when a mate is heard after being unheard for at least
// kEncounterGapSec, and grows while they keep being heard. Each mate keeps its
// last kLogLen encounters (newest first) with the closest zone reached.
// The whole book serialises to a small versioned blob for the addon's own NVS
// namespace; times are RTC seconds since 2000-01-01.
#pragma once
#include "fr_types.h"

namespace fr {

constexpr int      kLogLen          = 6;
constexpr uint32_t kEncounterGapSec = 300;   // 5 min unheard -> next sighting is a new encounter

struct Encounter {
  uint32_t startSec = 0;
  uint16_t minutes  = 0;       // duration so far
  uint8_t  closest  = (uint8_t)Zone::Far;
  uint8_t  pad      = 0;
};

struct Mate {
  uint32_t  id = 0;
  char      nick[kMaxName + 1] = {0};
  uint32_t  addedSec = 0;
  uint32_t  lastSeenSec = 0;    // 0 = never heard since being added
  uint8_t   lastZone = (uint8_t)Zone::Lost;
  uint8_t   logN = 0;
  Encounter log[kLogLen];
};

class MateBook {
public:
  // Dirty levels: structural changes should be saved promptly, sighting
  // updates can be saved lazily (on exit / every few minutes).
  enum Dirty : uint8_t { Clean = 0, Sightings = 1, Structure = 2 };

  int  count() const { return n_; }
  const Mate &at(int i) const { return m_[i]; }
  int  find(uint32_t id) const;
  bool isMate(uint32_t id) const { return find(id) >= 0; }
  const char *nickOf(uint32_t id) const;   // nullptr if not a mate

  bool add(uint32_t id, const char *nick, uint32_t nowSec);
  bool remove(uint32_t id);
  bool rename(uint32_t id, const char *nick);
  void clear();

  // A mate was heard at `nowSec` in zone `z` (Lost is ignored).
  void noteSeen(uint32_t id, Zone z, uint32_t nowSec);

  uint8_t dirty() const { return dirty_; }
  void    clearDirty() { dirty_ = Clean; }

  // Versioned binary blob. Returns bytes written (0 if cap is too small).
  size_t serialize(uint8_t *out, size_t cap) const;
  bool   deserialize(const uint8_t *in, size_t len);
  static size_t maxBlobSize();

private:
  Mate    m_[kMaxMates];
  int     n_ = 0;
  uint8_t dirty_ = Clean;
};

}  // namespace fr
