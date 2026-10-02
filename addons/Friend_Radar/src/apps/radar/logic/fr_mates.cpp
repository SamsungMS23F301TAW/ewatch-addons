#include "fr_mates.h"
#include "fr_beacon.h"
#include <string.h>

namespace fr {

static constexpr uint8_t kBlobVersion = 1;
// Per-mate record on disk: id4 nick13 added4 seen4 zone1 logN1 + log*(4+2+1+1)
static constexpr size_t kRecBytes = 4 + (kMaxName + 1) + 4 + 4 + 1 + 1 + kLogLen * 8;

int MateBook::find(uint32_t id) const {
  for (int i = 0; i < n_; ++i) if (m_[i].id == id) return i;
  return -1;
}

const char *MateBook::nickOf(uint32_t id) const {
  int i = find(id);
  return i < 0 ? nullptr : m_[i].nick;
}

bool MateBook::add(uint32_t id, const char *nick, uint32_t nowSec) {
  if (!validId(id)) return false;
  if (find(id) >= 0) return true;
  if (n_ >= kMaxMates) return false;
  Mate m;
  m.id = id;
  if (sanitizeName(nick, m.nick) == 0) defaultName(id, m.nick);
  m.addedSec = nowSec;
  m_[n_++] = m;
  dirty_ = Structure;
  return true;
}

bool MateBook::remove(uint32_t id) {
  int i = find(id);
  if (i < 0) return false;
  for (int j = i; j + 1 < n_; ++j) m_[j] = m_[j + 1];
  --n_;
  m_[n_] = Mate();
  dirty_ = Structure;
  return true;
}

bool MateBook::rename(uint32_t id, const char *nick) {
  int i = find(id);
  if (i < 0) return false;
  char clean[kMaxName + 1];
  if (sanitizeName(nick, clean) == 0) return false;
  if (strcmp(clean, m_[i].nick) == 0) return true;
  memcpy(m_[i].nick, clean, sizeof(clean));
  dirty_ = Structure;
  return true;
}

void MateBook::clear() {
  for (int i = 0; i < n_; ++i) m_[i] = Mate();
  n_ = 0;
  dirty_ = Structure;
}

void MateBook::noteSeen(uint32_t id, Zone z, uint32_t nowSec) {
  if (z == Zone::Lost) return;
  int i = find(id);
  if (i < 0) return;
  Mate &m = m_[i];
  bool fresh = m.logN == 0 || m.lastSeenSec == 0 ||
               (uint32_t)(nowSec - m.lastSeenSec) >= kEncounterGapSec ||
               nowSec < m.log[0].startSec;      // clock went backwards
  if (fresh) {
    for (int k = kLogLen - 1; k > 0; --k) m.log[k] = m.log[k - 1];
    m.log[0] = Encounter();
    m.log[0].startSec = nowSec;
    m.log[0].closest = (uint8_t)z;
    if (m.logN < kLogLen) ++m.logN;
  } else {
    uint32_t mins = (nowSec - m.log[0].startSec) / 60u;
    m.log[0].minutes = (uint16_t)(mins > 0xFFFF ? 0xFFFF : mins);
    if ((uint8_t)z < m.log[0].closest) m.log[0].closest = (uint8_t)z;
  }
  m.lastSeenSec = nowSec;
  m.lastZone = (uint8_t)z;
  if (dirty_ < Sightings) dirty_ = Sightings;
}

size_t MateBook::maxBlobSize() { return 2 + (size_t)kMaxMates * kRecBytes; }

static void put32(uint8_t *&p, uint32_t v) {
  *p++ = (uint8_t)v; *p++ = (uint8_t)(v >> 8); *p++ = (uint8_t)(v >> 16); *p++ = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t *&p) {
  uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
  p += 4;
  return v;
}

size_t MateBook::serialize(uint8_t *out, size_t cap) const {
  size_t need = 2 + (size_t)n_ * kRecBytes;
  if (!out || cap < need) return 0;
  uint8_t *p = out;
  *p++ = kBlobVersion;
  *p++ = (uint8_t)n_;
  for (int i = 0; i < n_; ++i) {
    const Mate &m = m_[i];
    put32(p, m.id);
    memcpy(p, m.nick, kMaxName + 1); p += kMaxName + 1;
    put32(p, m.addedSec);
    put32(p, m.lastSeenSec);
    *p++ = m.lastZone;
    *p++ = m.logN;
    for (int k = 0; k < kLogLen; ++k) {
      put32(p, m.log[k].startSec);
      *p++ = (uint8_t)m.log[k].minutes;
      *p++ = (uint8_t)(m.log[k].minutes >> 8);
      *p++ = m.log[k].closest;
      *p++ = 0;
    }
  }
  return (size_t)(p - out);
}

bool MateBook::deserialize(const uint8_t *in, size_t len) {
  if (!in || len < 2 || in[0] != kBlobVersion) return false;
  int n = in[1];
  if (n > kMaxMates || len != 2 + (size_t)n * kRecBytes) return false;
  Mate tmp[kMaxMates];
  int kept = 0;
  const uint8_t *p = in + 2;
  for (int i = 0; i < n; ++i) {
    Mate m;
    m.id = get32(p);
    char raw[kMaxName + 1];
    memcpy(raw, p, kMaxName + 1); p += kMaxName + 1;
    raw[kMaxName] = '\0';
    sanitizeName(raw, m.nick);
    m.addedSec = get32(p);
    m.lastSeenSec = get32(p);
    m.lastZone = *p++;
    m.logN = *p++;
    if (m.logN > kLogLen) m.logN = kLogLen;
    if (m.lastZone > (uint8_t)Zone::Lost) m.lastZone = (uint8_t)Zone::Lost;
    for (int k = 0; k < kLogLen; ++k) {
      m.log[k].startSec = get32(p);
      m.log[k].minutes = (uint16_t)(p[0] | (p[1] << 8)); p += 2;
      m.log[k].closest = *p++;
      if (m.log[k].closest > (uint8_t)Zone::Far) m.log[k].closest = (uint8_t)Zone::Far;
      ++p;
    }
    if (!validId(m.id)) continue;                       // drop corrupt records
    bool dup = false;
    for (int j = 0; j < kept; ++j) if (tmp[j].id == m.id) dup = true;
    if (dup) continue;
    if (m.nick[0] == '\0') defaultName(m.id, m.nick);
    tmp[kept++] = m;
  }
  for (int i = 0; i < kMaxMates; ++i) m_[i] = (i < kept) ? tmp[i] : Mate();
  n_ = kept;
  dirty_ = Clean;
  return true;
}

}  // namespace fr
