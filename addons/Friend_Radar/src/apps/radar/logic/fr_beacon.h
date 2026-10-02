// Friend Radar beacon: the BLE advertisement every radar-enabled EWatch sends.
//
// One legacy, non-connectable advertisement holding a single Manufacturer
// Specific Data AD structure under company ID 0xFFFF (reserved for testing).
// The full byte layout, with an annotated example, is in docs/PROTOCOL.md.
// This module is pure: it turns a Beacon struct into advertising-data bytes
// and back, validating everything it reads off the air.
#pragma once
#include "fr_types.h"

namespace fr {

constexpr uint16_t kCompanyId     = 0xFFFF;
constexpr uint8_t  kMagic0        = 'E';
constexpr uint8_t  kMagic1        = 'W';
constexpr uint8_t  kMagic2        = 'R';
constexpr uint8_t  kProtoVersion  = 0x10;   // major 1 (high nibble), minor 0
constexpr size_t   kFixedPayload  = 15;     // bytes before the name
constexpr size_t   kMaxAdvData    = 31;     // legacy advertising data limit

// Beacon flags (byte 9).
constexpr uint8_t kFlagBackground = 0x01;   // sent from a background window
constexpr uint8_t kFlagCalibrated = 0x02;   // ref1m was measured, not default
constexpr uint8_t kFlagAlerts     = 0x04;   // sender has background mate alerts on

constexpr int8_t  kDefaultRef1m   = -60;    // uncalibrated RSSI at 1 m (dBm)
constexpr int8_t  kMinRef1m       = -90;
constexpr int8_t  kMaxRef1m       = -25;

struct Beacon {
  uint32_t  id       = 0;              // random persistent watch id (never 0 / 0xFFFFFFFF)
  int8_t    ref1m    = kDefaultRef1m;  // calibrated RSSI at 1 m, dBm
  uint8_t   flags    = 0;              // kFlag*
  uint8_t   clk      = 0;              // radar clock, seconds mod 120
  EventKind evKind   = EventKind::None;
  uint8_t   evSeq    = 0;              // 6-bit event sequence number
  uint16_t  evTarget = 0;              // targetTag() of the addressee, 0 = everyone
  uint8_t   evAge    = 0;              // event age, 100 ms units, saturates at 255
  char      name[kMaxName + 1] = {0};  // printable ASCII, NUL-terminated
};

// Encode `b` as complete advertising data (AD structures). Returns the number
// of bytes written, or 0 if `cap` is too small or the beacon is invalid.
size_t encodeAdv(const Beacon &b, uint8_t *out, size_t cap);

// Scan raw advertising data for an EWatch Radar beacon. Returns true and fills
// `out` only for a well-formed beacon of a supported major version.
bool decodeAdv(const uint8_t *data, size_t len, Beacon &out);

// Copy `in` into `out` keeping printable ASCII only, trimming surrounding
// spaces and collapsing runs of spaces, truncated to kMaxName characters.
// `out` must hold kMaxName + 1 bytes. Returns the resulting length.
size_t sanitizeName(const char *in, char *out);

// A friendly default name for a fresh install, e.g. "EWatch 3F2A".
void defaultName(uint32_t id, char *out);

// True for ids we never use on the air.
static inline bool validId(uint32_t id) { return id != 0 && id != 0xFFFFFFFFu; }

}  // namespace fr
