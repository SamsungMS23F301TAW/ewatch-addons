#include "fr_beacon.h"
#include <string.h>

namespace fr {

const char *zoneName(Zone z) {
  switch (z) {
    case Zone::RightHere: return "Right here";
    case Zone::Near:      return "Near";
    case Zone::Around:    return "Around";
    case Zone::Far:       return "Far";
    case Zone::Lost:      return "Lost";
  }
  return "?";
}

const char *zoneShort(Zone z) {
  switch (z) {
    case Zone::RightHere: return "here";
    case Zone::Near:      return "near";
    case Zone::Around:    return "around";
    case Zone::Far:       return "far";
    case Zone::Lost:      return "lost";
  }
  return "?";
}

size_t sanitizeName(const char *in, char *out) {
  size_t n = 0;
  bool pendingSpace = false;
  if (in) {
    for (const char *p = in; *p && n < kMaxName; ++p) {
      unsigned char c = (unsigned char)*p;
      if (c < 0x20 || c > 0x7E) continue;          // drop control / non-ASCII
      if (c == ' ') { pendingSpace = (n > 0); continue; }
      if (pendingSpace) {
        if (n + 1 >= kMaxName) break;               // no room for space + char
        out[n++] = ' ';
        pendingSpace = false;
      }
      out[n++] = (char)c;
    }
  }
  out[n] = '\0';
  return n;
}

void defaultName(uint32_t id, char *out) {
  static const char kHex[] = "0123456789ABCDEF";
  const char *prefix = "EWatch ";
  size_t n = 0;
  while (prefix[n]) { out[n] = prefix[n]; ++n; }
  uint16_t tag = (uint16_t)(id & 0xFFFF);
  for (int s = 12; s >= 0; s -= 4) out[n++] = kHex[(tag >> s) & 0xF];
  out[n] = '\0';
}

size_t encodeAdv(const Beacon &b, uint8_t *out, size_t cap) {
  if (!out || !validId(b.id)) return 0;
  char name[kMaxName + 1];
  size_t nameLen = sanitizeName(b.name, name);
  size_t payload = kFixedPayload + nameLen;     // bytes after the company id
  size_t adLen   = 1 /*type*/ + 2 /*company*/ + payload;
  size_t total   = 1 /*len*/ + adLen;
  if (total > kMaxAdvData || total > cap) return 0;

  uint8_t *p = out;
  *p++ = (uint8_t)adLen;
  *p++ = 0xFF;                                  // Manufacturer Specific Data
  *p++ = (uint8_t)(kCompanyId & 0xFF);
  *p++ = (uint8_t)(kCompanyId >> 8);
  *p++ = kMagic0; *p++ = kMagic1; *p++ = kMagic2;
  *p++ = kProtoVersion;
  *p++ = (uint8_t)(b.id);
  *p++ = (uint8_t)(b.id >> 8);
  *p++ = (uint8_t)(b.id >> 16);
  *p++ = (uint8_t)(b.id >> 24);
  *p++ = (uint8_t)b.ref1m;
  *p++ = b.flags;
  *p++ = (uint8_t)(b.clk % 120);
  *p++ = (uint8_t)(((b.evSeq & 0x3F) << 2) | ((uint8_t)b.evKind & 0x03));
  *p++ = (uint8_t)(b.evTarget);
  *p++ = (uint8_t)(b.evTarget >> 8);
  *p++ = b.evAge;
  memcpy(p, name, nameLen);
  p += nameLen;
  return (size_t)(p - out);
}

// Parse one Manufacturer Specific Data body (company id included).
static bool parseMsd(const uint8_t *d, size_t n, Beacon &out) {
  if (n < 2 + kFixedPayload) return false;
  uint16_t company = (uint16_t)(d[0] | (d[1] << 8));
  if (company != kCompanyId) return false;
  const uint8_t *p = d + 2;
  size_t len = n - 2;
  if (p[0] != kMagic0 || p[1] != kMagic1 || p[2] != kMagic2) return false;
  if ((p[3] >> 4) != (kProtoVersion >> 4)) return false;    // other major version
  uint32_t id = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
                ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
  if (!validId(id)) return false;
  int8_t ref = (int8_t)p[8];
  if (ref < kMinRef1m || ref > kMaxRef1m) return false;
  if (p[10] >= 120) return false;

  Beacon b;
  b.id       = id;
  b.ref1m    = ref;
  b.flags    = p[9];
  b.clk      = p[10];
  b.evKind   = (EventKind)(p[11] & 0x03);
  b.evSeq    = (uint8_t)(p[11] >> 2);
  b.evTarget = (uint16_t)(p[12] | (p[13] << 8));
  b.evAge    = p[14];
  // Name: the remaining bytes, up to an optional NUL, printable ASCII only.
  char raw[kMaxName + 1];
  size_t nn = 0;
  for (size_t i = kFixedPayload; i < len && nn < kMaxName; ++i) {
    if (p[i] == 0) break;
    raw[nn++] = (char)p[i];
  }
  raw[nn] = '\0';
  sanitizeName(raw, b.name);
  out = b;
  return true;
}

bool decodeAdv(const uint8_t *data, size_t len, Beacon &out) {
  if (!data) return false;
  size_t i = 0;
  while (i < len) {
    uint8_t adLen = data[i];
    if (adLen == 0) break;                      // early terminator / padding
    if (i + 1 + adLen > len) return false;      // truncated structure
    uint8_t type = data[i + 1];
    if (type == 0xFF && parseMsd(data + i + 2, adLen - 1, out)) return true;
    i += 1 + (size_t)adLen;
  }
  return false;
}

}  // namespace fr
