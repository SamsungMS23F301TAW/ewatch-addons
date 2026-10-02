// EWatch Companion wire protocol. See ble_proto.h and docs/PROTOCOL.md.
#include "ble_proto.h"

#include <string.h>

namespace bleproto {

// ---------------------------------------------------------------------------
// Colours
// ---------------------------------------------------------------------------
uint16_t rgb565FromRgb888(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((uint16_t)(r & 0xF8) << 8) |
                    ((uint16_t)(g & 0xFC) << 3) |
                    ((uint16_t)b >> 3));
}

void rgb888FromRgb565(uint16_t c, uint8_t &r, uint8_t &g, uint8_t &b) {
  uint8_t r5 = (uint8_t)((c >> 11) & 0x1F);
  uint8_t g6 = (uint8_t)((c >> 5) & 0x3F);
  uint8_t b5 = (uint8_t)(c & 0x1F);
  r = (uint8_t)((r5 << 3) | (r5 >> 2));
  g = (uint8_t)((g6 << 2) | (g6 >> 4));
  b = (uint8_t)((b5 << 3) | (b5 >> 2));
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static bool hasNul(const uint8_t *p, size_t n) {
  return n > 0 && memchr(p, 0, n) != nullptr;
}

static void copyText(char *dst, size_t cap, const uint8_t *src, size_t n) {
  if (n >= cap) n = cap - 1;
  if (n) memcpy(dst, src, n);
  dst[n] = '\0';
}

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------
size_t encodeTheme(const Theme &t, uint8_t *out) {
  putU16(out + 0, t.bg);
  putU16(out + 2, t.fg);
  putU16(out + 4, t.accent);
  putU16(out + 6, t.line);
  return kThemeLen;
}

Result decodeTheme(const uint8_t *in, size_t len, Theme &out) {
  if (len != kThemeLen) return RES_BAD_LENGTH;
  out.bg     = getU16(in + 0);
  out.fg     = getU16(in + 2);
  out.accent = getU16(in + 4);
  out.line   = getU16(in + 6);
  return RES_OK;
}

// ---------------------------------------------------------------------------
// Brightness
// ---------------------------------------------------------------------------
size_t encodeBrightness(uint8_t level, uint8_t *out) {
  out[0] = level;
  return kBrightnessLen;
}

Result decodeBrightness(const uint8_t *in, size_t len, uint8_t &out) {
  if (len != kBrightnessLen) return RES_BAD_LENGTH;
  out = in[0] < kBrightnessMin ? kBrightnessMin : in[0];
  return RES_OK;
}

// ---------------------------------------------------------------------------
// Face
// ---------------------------------------------------------------------------
size_t encodeFace(const FaceCfg &f, uint8_t *out) {
  out[0] = f.style;
  out[1] = f.options;
  return kFaceLen;
}

Result decodeFace(const uint8_t *in, size_t len, uint8_t styleCount, FaceCfg &out) {
  if (len != kFaceLen) return RES_BAD_LENGTH;
  if (in[0] >= styleCount) return RES_BAD_VALUE;
  out.style   = in[0];
  out.options = (uint8_t)(in[1] & kFaceOptMask);   // unknown option bits are dropped
  return RES_OK;
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------
size_t encodeSlots(const SlotCfg *slots, uint8_t *out) {
  for (uint8_t i = 0; i < kSlotCount; i++) {
    out[i * 4 + 0] = slots[i].source;
    out[i * 4 + 1] = slots[i].arg;
    out[i * 4 + 2] = slots[i].color;
    out[i * 4 + 3] = slots[i].flags;
  }
  return kSlotsLen;
}

Result decodeSlots(const uint8_t *in, size_t len, SlotCfg *out) {
  if (len != kSlotsLen) return RES_BAD_LENGTH;
  SlotCfg tmp[kSlotCount];
  for (uint8_t i = 0; i < kSlotCount; i++) {
    const uint8_t *p = in + i * 4;
    uint8_t src = p[0], arg = p[1], col = p[2], flags = p[3];
    if (src > kSourceMax || col > kSlotColorMax) return RES_BAD_VALUE;
    if (src == SRC_TEXT) {
      if (arg >= kTextCount) return RES_BAD_VALUE;
    } else if (src == SRC_FEED) {
      if (arg >= kFeedCount) return RES_BAD_VALUE;
    } else {
      arg = 0;                          // arg is meaningless for other sources
    }
    tmp[i].source = src;
    tmp[i].arg    = arg;
    tmp[i].color  = col;
    tmp[i].flags  = (uint8_t)(flags & kSlotFlagsMask);
  }
  memcpy(out, tmp, sizeof(tmp));
  return RES_OK;
}

// ---------------------------------------------------------------------------
// Texts
// ---------------------------------------------------------------------------
size_t encodeTextWrite(uint8_t index, const char *utf8, size_t len, uint8_t *out) {
  if (len > kTextMax) len = kTextMax;
  out[0] = index;
  if (len) memcpy(out + 1, utf8, len);
  return 1 + len;
}

Result decodeTextWrite(const uint8_t *in, size_t len, uint8_t &index, TextEntry &out) {
  if (len < 1 || len > kTextWriteMax) return RES_BAD_LENGTH;
  if (in[0] >= kTextCount) return RES_BAD_VALUE;
  size_t n = len - 1;
  if (hasNul(in + 1, n)) return RES_BAD_VALUE;
  index = in[0];
  out.len = (uint8_t)n;
  copyText(out.text, sizeof(out.text), in + 1, n);
  return RES_OK;
}

size_t encodeTextsRead(const TextEntry *texts, uint8_t *out) {
  size_t o = 0;
  for (uint8_t i = 0; i < kTextCount; i++) {
    uint8_t n = texts[i].len > kTextMax ? kTextMax : texts[i].len;
    out[o++] = n;
    if (n) memcpy(out + o, texts[i].text, n);
    o += n;
  }
  return o;
}

// ---------------------------------------------------------------------------
// Feeds
// ---------------------------------------------------------------------------
static void putFeedHeader(const FeedEntry &f, uint8_t *p) {
  p[0] = f.icon;
  p[1] = f.kind;
  putU32(p + 2, f.expiresAt);
  putU32(p + 6, f.targetAt);
}

size_t encodeFeedWrite(uint8_t index, const FeedEntry &f, uint8_t *out) {
  uint8_t n = f.len > kFeedTextMax ? kFeedTextMax : f.len;
  out[0] = index;
  putFeedHeader(f, out + 1);
  if (n) memcpy(out + kFeedWriteMin, f.text, n);
  return kFeedWriteMin + n;
}

Result decodeFeedWrite(const uint8_t *in, size_t len, uint8_t &index, FeedEntry &out) {
  if (len < kFeedWriteMin || len > kFeedWriteMax) return RES_BAD_LENGTH;
  uint8_t idx  = in[0];
  uint8_t icon = in[1];
  uint8_t kind = in[2];
  uint32_t exp = getU32(in + 3);
  uint32_t tgt = getU32(in + 7);
  size_t n = len - kFeedWriteMin;
  if (idx >= kFeedCount || icon > kIconMax || kind > kFeedKindMax) return RES_BAD_VALUE;
  if (kind == FEED_COUNTDOWN && tgt == 0) return RES_BAD_VALUE;
  if (hasNul(in + kFeedWriteMin, n)) return RES_BAD_VALUE;
  index = idx;
  out.icon = icon;
  out.kind = kind;
  out.expiresAt = exp;
  out.targetAt = (kind == FEED_COUNTDOWN) ? tgt : 0;
  out.len = (uint8_t)n;
  copyText(out.text, sizeof(out.text), in + kFeedWriteMin, n);
  return RES_OK;
}

size_t encodeFeedsRead(const FeedEntry *feeds, uint8_t *out) {
  size_t o = 0;
  for (uint8_t i = 0; i < kFeedCount; i++) {
    uint8_t n = feeds[i].len > kFeedTextMax ? kFeedTextMax : feeds[i].len;
    out[o++] = n;
    putFeedHeader(feeds[i], out + o);
    o += kFeedHeaderLen;
    if (n) memcpy(out + o, feeds[i].text, n);
    o += n;
  }
  return o;
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
size_t encodeTimeWrite(const TimeSync &t, uint8_t *out) {
  putU32(out + 0, t.unix);
  putU16(out + 4, (uint16_t)t.tzOffsetMin);
  putU16(out + 6, t.millis);
  return kTimeWriteLen;
}

Result decodeTimeWrite(const uint8_t *in, size_t len, TimeSync &out) {
  if (len != kTimeWriteLen) return RES_BAD_LENGTH;
  uint32_t unix = getU32(in + 0);
  int16_t  tz   = (int16_t)getU16(in + 4);
  uint16_t ms   = getU16(in + 6);
  if (tz < kTzMinMinutes || tz > kTzMaxMinutes || ms > 999) return RES_BAD_VALUE;
  // The LOCAL time must fit the RTC's 2000..2099 range.
  int64_t local = (int64_t)unix + (int64_t)tz * 60;
  if (local < (int64_t)kUnixMin || local > (int64_t)kUnixMax) return RES_BAD_VALUE;
  out.unix = unix;
  out.tzOffsetMin = tz;
  out.millis = ms;
  return RES_OK;
}

size_t encodeTimeRead(uint32_t unix, int16_t tzOffsetMin, bool rtcOk, uint8_t *out) {
  putU32(out + 0, unix);
  putU16(out + 4, (uint16_t)tzOffsetMin);
  out[6] = rtcOk ? 1 : 0;
  return kTimeReadLen;
}

// ---------------------------------------------------------------------------
// Auth
// ---------------------------------------------------------------------------
size_t encodeAuthWrite(uint32_t code, uint8_t *out) {
  putU32(out, code);
  return kAuthWriteLen;
}

Result decodeAuthWrite(const uint8_t *in, size_t len, uint32_t &code) {
  if (len != kAuthWriteLen) return RES_BAD_LENGTH;
  uint32_t c = getU32(in);
  if (c > 999999UL) return RES_BAD_VALUE;
  code = c;
  return RES_OK;
}

size_t encodeAuthRead(uint8_t state, uint8_t attemptsLeft, uint8_t secondsLeft, uint8_t *out) {
  out[0] = state;
  out[1] = attemptsLeft;
  out[2] = secondsLeft;
  return kAuthReadLen;
}

// ---------------------------------------------------------------------------
// Revision
// ---------------------------------------------------------------------------
size_t encodeRevision(const Revision &r, uint8_t *out) {
  putU32(out + 0, r.revision);
  putU16(out + 4, r.changedMask);
  out[6] = r.source;
  out[7] = r.result;
  return kRevisionLen;
}

Result decodeRevision(const uint8_t *in, size_t len, Revision &out) {
  if (len != kRevisionLen) return RES_BAD_LENGTH;
  out.revision    = getU32(in + 0);
  out.changedMask = getU16(in + 4);
  out.source      = in[6];
  out.result      = in[7];
  return RES_OK;
}

// ---------------------------------------------------------------------------
// Message
// ---------------------------------------------------------------------------
size_t encodeMessage(const Message &m, uint8_t *out) {
  uint8_t n = m.len > kMessageMax ? kMessageMax : m.len;
  out[0] = m.icon;
  out[1] = m.flags;
  if (n) memcpy(out + 2, m.text, n);
  return 2 + (size_t)n;
}

Result decodeMessage(const uint8_t *in, size_t len, Message &out) {
  if (len < 3 || len > kMessageWriteMax) return RES_BAD_LENGTH;
  if (in[0] > kIconMax) return RES_BAD_VALUE;
  size_t n = len - 2;
  if (hasNul(in + 2, n)) return RES_BAD_VALUE;
  out.icon  = in[0];
  out.flags = (uint8_t)(in[1] & kMsgFlagsMask);
  out.len   = (uint8_t)n;
  copyText(out.text, sizeof(out.text), in + 2, n);
  return RES_OK;
}

// ---------------------------------------------------------------------------
// Control
// ---------------------------------------------------------------------------
Result decodeControl(const uint8_t *in, size_t len, uint8_t &op) {
  if (len < 1 || len > kControlWriteMax) return RES_BAD_LENGTH;
  switch (in[0]) {
    case CTRL_IDENTIFY:
    case CTRL_RESET_FACE:
    case CTRL_RESET_THEME:
    case CTRL_SHOW_FACE:
    case CTRL_SHOW_COMPANION:
      op = in[0];
      return RES_OK;
    default:
      return RES_UNSUPPORTED;
  }
}

// ---------------------------------------------------------------------------
// Info
// ---------------------------------------------------------------------------
static size_t putStr(uint8_t *out, size_t o, size_t cap, const char *s, size_t maxLen) {
  size_t n = s ? strlen(s) : 0;
  if (n > maxLen) n = maxLen;
  if (o + 1 + n > cap) n = (o + 1 < cap) ? cap - o - 1 : 0;
  if (o >= cap) return o;
  out[o++] = (uint8_t)n;
  if (n) memcpy(out + o, s, n);
  return o + n;
}

size_t encodeInfo(const InfoParams &p, uint8_t *out, size_t cap) {
  if (cap < kInfoHeaderLen) return 0;
  out[0] = kProtoMajor;
  out[1] = kProtoMinor;
  putU16(out + 2, p.capabilities);
  out[4] = kSlotCount;
  out[5] = kTextCount;
  out[6] = kTextMax;
  out[7] = kFeedCount;
  out[8] = kFeedTextMax;
  out[9] = kMessageMax;
  out[10] = kSourceMax;
  out[11] = kIconMax;
  out[12] = kBrightnessMin;
  out[13] = p.styleCount;
  size_t o = kInfoHeaderLen;
  for (uint8_t i = 0; i < p.styleCount; i++) {
    o = putStr(out, o, cap, p.styleNames ? p.styleNames[i] : "", 16);
  }
  o = putStr(out, o, cap, p.firmware, 16);
  o = putStr(out, o, cap, p.deviceName, 24);
  return o;
}

// ---------------------------------------------------------------------------
// Civil time (Howard Hinnant's public-domain algorithms)
// ---------------------------------------------------------------------------
int32_t daysFromCivil(int32_t y, uint32_t m, uint32_t d) {
  y -= m <= 2 ? 1 : 0;
  const int32_t era = (y >= 0 ? y : y - 399) / 400;
  const uint32_t yoe = (uint32_t)(y - era * 400);
  const uint32_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t)doe - 719468;
}

void civilFromDays(int32_t z, int32_t &y, uint32_t &m, uint32_t &d) {
  z += 719468;
  const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  const uint32_t doe = (uint32_t)(z - era * 146097);
  const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int32_t yy = (int32_t)yoe + era * 400;
  const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const uint32_t mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = yy + (m <= 2 ? 1 : 0);
}

uint8_t weekdayFromDays(int32_t z) {
  return (uint8_t)(z >= -4 ? (z + 4) % 7 : (z + 5) % 7 + 6);
}

void unixToCivil(uint32_t unix, int16_t tzOffsetMin, CivilTime &out) {
  int64_t local = (int64_t)unix + (int64_t)tzOffsetMin * 60;
  int64_t days = local / 86400;
  int64_t secs = local % 86400;
  if (secs < 0) { secs += 86400; days -= 1; }
  int32_t y; uint32_t m, d;
  civilFromDays((int32_t)days, y, m, d);
  out.year    = (uint16_t)y;
  out.month   = (uint8_t)m;
  out.day     = (uint8_t)d;
  out.hour    = (uint8_t)(secs / 3600);
  out.minute  = (uint8_t)((secs % 3600) / 60);
  out.second  = (uint8_t)(secs % 60);
  out.weekday = weekdayFromDays((int32_t)days);
}

uint32_t civilToUnix(const CivilTime &t, int16_t tzOffsetMin) {
  int64_t days = daysFromCivil(t.year, t.month, t.day);
  int64_t v = days * 86400 + (int64_t)t.hour * 3600 + (int64_t)t.minute * 60 +
              (int64_t)t.second - (int64_t)tzOffsetMin * 60;
  if (v < 0) return 0;
  if (v > (int64_t)0xFFFFFFFFLL) return 0xFFFFFFFFUL;
  return (uint32_t)v;
}

}  // namespace bleproto
