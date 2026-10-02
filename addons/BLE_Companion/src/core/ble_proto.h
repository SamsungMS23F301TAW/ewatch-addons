// EWatch Companion wire protocol: UUIDs, limits, byte layouts and the
// encode / decode / validate helpers for every characteristic of the
// "EWatch Config Service".
//
// Pure C++11 with no Arduino, FreeRTOS or NimBLE dependency, so it compiles
// into the firmware AND into the host unit tests (`pio test -e native`). The
// web page (web/index.html) carries a JavaScript mirror of this file; both are
// checked against the shared vectors in test/vectors/protocol_vectors.json.
//
// docs/PROTOCOL.md is the human-readable specification of this file. If you
// change a layout here, change the JS codec, the vectors and the doc together
// and bump kProtoMinor (compatible addition) or kProtoMajor (breaking change).
//
// Conventions
//   * Every multi-byte integer is little-endian.
//   * Colours are RGB565 (5-6-5) in a uint16, converted from 8-bit channels by
//     truncation (rgb565FromRgb888) exactly like BaseOS's web settings page.
//   * Times on the wire are Unix seconds (UTC). The watch's RTC keeps LOCAL
//     time; model.tzOffsetMin converts between the two.
//   * Text on the wire is UTF-8. The watch renders it with the 6x8 CP437
//     bitmap font after face_slots' glyph mapping.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace bleproto {

// ---------------------------------------------------------------------------
// Versioning
// ---------------------------------------------------------------------------
static const uint8_t kProtoMajor = 1;   // breaking layout changes
static const uint8_t kProtoMinor = 1;   // backwards-compatible additions (1: Halo face style 8)

// ---------------------------------------------------------------------------
// UUIDs. Custom 128-bit base e128xxxx-ca10-4c2f-babd-3d3dd6094bbc.
// ---------------------------------------------------------------------------
#define BLEPROTO_UUID(x) "e128" x "-ca10-4c2f-babd-3d3dd6094bbc"
static const char *const kServiceUuid    = BLEPROTO_UUID("0001");
static const char *const kInfoUuid       = BLEPROTO_UUID("0002");  // R         (open)
static const char *const kAuthUuid       = BLEPROTO_UUID("0003");  // R W N     (open)
static const char *const kRevisionUuid   = BLEPROTO_UUID("0004");  // R N       (auth)
static const char *const kThemeUuid      = BLEPROTO_UUID("0010");  // R W N     (auth)
static const char *const kBrightnessUuid = BLEPROTO_UUID("0011");  // R W N     (auth)
static const char *const kFaceUuid       = BLEPROTO_UUID("0012");  // R W N     (auth)
static const char *const kSlotsUuid      = BLEPROTO_UUID("0013");  // R W N     (auth)
static const char *const kTextsUuid      = BLEPROTO_UUID("0014");  // R W       (auth)
static const char *const kFeedsUuid      = BLEPROTO_UUID("0015");  // R W       (auth)
static const char *const kTimeUuid       = BLEPROTO_UUID("0016");  // R W N     (auth)
static const char *const kMessageUuid    = BLEPROTO_UUID("0017");  // W         (auth)
static const char *const kControlUuid    = BLEPROTO_UUID("0018");  // W         (auth)

// ---------------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------------
static const uint8_t kSlotCount     = 4;    // Top, Upper, Lower, Bottom
static const uint8_t kTextCount     = 4;
static const uint8_t kTextMax       = 20;   // UTF-8 bytes per custom text
static const uint8_t kFeedCount     = 4;
static const uint8_t kFeedTextMax   = 20;   // UTF-8 bytes per feed text/label
static const uint8_t kMessageMax    = 64;   // UTF-8 bytes per pushed message
static const uint8_t kBrightnessMin = 16;   // lower values are clamped (panel stays visible)
static const uint8_t kIconMax       = 15;   // highest valid icon id
static const int16_t kTzMinMinutes  = -720; // UTC-12:00
static const int16_t kTzMaxMinutes  = 840;  // UTC+14:00
// RV-3028 stores a two-digit year, so the watch can represent 2000..2099.
static const uint32_t kUnixMin = 946684800UL;   // 2000-01-01T00:00:00Z
static const uint32_t kUnixMax = 4102444799UL;  // 2099-12-31T23:59:59Z

// Payload sizes (bytes)
static const size_t kThemeLen       = 8;
static const size_t kBrightnessLen  = 1;
static const size_t kFaceLen        = 2;
static const size_t kSlotsLen       = kSlotCount * 4;           // 16
static const size_t kTimeWriteLen   = 8;
static const size_t kTimeReadLen    = 7;
static const size_t kAuthWriteLen   = 4;
static const size_t kAuthReadLen    = 3;
static const size_t kRevisionLen    = 8;
static const size_t kFeedHeaderLen  = 10;                       // icon kind exp tgt
static const size_t kTextWriteMax   = 1 + kTextMax;             // 21
static const size_t kFeedWriteMin   = 1 + kFeedHeaderLen;       // 11
static const size_t kFeedWriteMax   = kFeedWriteMin + kFeedTextMax;   // 31
static const size_t kTextsReadMax   = kTextCount * (1 + kTextMax);    // 84
static const size_t kFeedsReadMax   = kFeedCount * (1 + kFeedHeaderLen + kFeedTextMax); // 124
static const size_t kMessageWriteMax = 2 + kMessageMax;         // 66
static const size_t kControlWriteMax = 4;
static const size_t kInfoMax        = 160;

// ---------------------------------------------------------------------------
// Enumerations
// ---------------------------------------------------------------------------
// Face data-source slot sources.
enum Source : uint8_t {
  SRC_NONE       = 0,
  SRC_SECONDS    = 1,   // ":SS"
  SRC_DATE       = 2,   // "Wed 1 Oct 2026" (the stock BaseOS date line)
  SRC_SHORT_DATE = 3,   // "Wed 1 Oct"
  SRC_BATTERY    = 4,   // battery glyph + "87%"
  SRC_TEXT       = 5,   // custom text #arg
  SRC_FEED       = 6,   // pushed value #arg (weather, next meeting, ...)
};
static const uint8_t kSourceMax = SRC_FEED;

enum SlotColor : uint8_t { COLOR_FG = 0, COLOR_ACCENT = 1, COLOR_DIM = 2 };
static const uint8_t kSlotColorMax = COLOR_DIM;
static const uint8_t kSlotFlagNoIcon = 0x01;   // hide the icon of battery/feed slots
static const uint8_t kSlotFlagsMask  = 0x01;   // flags outside the mask are dropped

// Slot positions on the face, top to bottom.
enum SlotPos : uint8_t { SLOT_TOP = 0, SLOT_UPPER = 1, SLOT_LOWER = 2, SLOT_BOTTOM = 3 };

// Face options bitfield.
static const uint8_t kFaceOpt12h  = 0x01;      // 12-hour clock
static const uint8_t kFaceOptMask = 0x01;

enum FeedKind : uint8_t {
  FEED_VALUE     = 0,   // show text as-is ("14°C Cloudy")
  FEED_COUNTDOWN = 1,   // show "<label> in 25m" counting down to targetAt
};
static const uint8_t kFeedKindMax = FEED_COUNTDOWN;

// Icon ids shared by feeds and messages. Drawn as vector glyphs on the watch
// (face_icons.cpp) and in the web preview. 0 = no icon.
enum Icon : uint8_t {
  ICON_NONE = 0, ICON_SUN, ICON_PARTLY, ICON_CLOUD, ICON_RAIN, ICON_STORM,
  ICON_SNOW, ICON_FOG, ICON_MOON, ICON_CALENDAR, ICON_BELL, ICON_HEART,
  ICON_STAR, ICON_CHAT, ICON_CHECK, ICON_ALERT,
};

// Auth characteristic state.
enum AuthState : uint8_t {
  AUTH_CODE_REQUIRED = 0,   // a 6-digit code is on the watch screen
  AUTH_AUTHORIZED    = 1,   // config writes are accepted on this connection
  AUTH_LOCKED        = 2,   // too many wrong codes; the watch will disconnect
};

// Result codes reported in the Revision characteristic (NimBLE 1.4.x cannot
// return ATT errors from write callbacks, so results travel as a notify).
enum Result : uint8_t {
  RES_OK          = 0,
  RES_NOT_AUTH    = 1,   // write before a successful code entry
  RES_BAD_LENGTH  = 2,
  RES_BAD_VALUE   = 3,
  RES_BUSY        = 4,
  RES_UNSUPPORTED = 5,
};

// Change bits (Revision.changedMask).
enum ChangeBit : uint16_t {
  CH_THEME      = 1u << 0,
  CH_BRIGHTNESS = 1u << 1,
  CH_FACE       = 1u << 2,
  CH_SLOTS      = 1u << 3,
  CH_TEXTS      = 1u << 4,
  CH_FEEDS      = 1u << 5,
  CH_TIME       = 1u << 6,
  CH_MESSAGE    = 1u << 7,
  CH_CONTROL    = 1u << 8,
};

// Who caused a change (Revision.source).
enum ChangeSource : uint8_t {
  BY_CLIENT = 0,    // this BLE connection
  BY_WATCH  = 1,    // the watch's own UI (Settings pages, WiFi web page)
  BY_SYSTEM = 2,    // defaults restored, feed expiry, boot
};

// Control opcodes.
enum ControlOp : uint8_t {
  CTRL_IDENTIFY       = 0x01,  // buzz + full brightness so you know which watch
  CTRL_RESET_FACE     = 0x02,  // slots, texts, feeds, face options -> defaults
  CTRL_RESET_THEME    = 0x03,  // BaseOS colours, brightness 200, style 0
  CTRL_SHOW_FACE      = 0x04,  // switch the watch to its face (live preview)
  CTRL_SHOW_COMPANION = 0x05,  // switch the watch to the Companion screen
};

// Message flags.
static const uint8_t kMsgFlagSilent = 0x01;   // no buzz
static const uint8_t kMsgFlagsMask  = 0x01;

// Capability flags (Info.capabilities).
static const uint16_t kCapMessages   = 1u << 0;
static const uint16_t kCapFeeds      = 1u << 1;
static const uint16_t kCap12h        = 1u << 2;
static const uint16_t kCapEncrypted  = 1u << 3;   // link encryption required (build option)
static const uint16_t kCapControl    = 1u << 4;

// ---------------------------------------------------------------------------
// Value types
// ---------------------------------------------------------------------------
struct Theme   { uint16_t bg, fg, accent, line; };
struct FaceCfg { uint8_t style, options; };
struct SlotCfg { uint8_t source, arg, color, flags; };

struct TextEntry {
  uint8_t len;                       // bytes used in `text`
  char    text[kTextMax + 1];        // UTF-8, NUL-terminated
};

struct FeedEntry {
  uint8_t  icon;
  uint8_t  kind;                     // FeedKind
  uint32_t expiresAt;                // Unix UTC seconds, 0 = never
  uint32_t targetAt;                 // Unix UTC seconds (countdown target)
  uint8_t  len;
  char     text[kFeedTextMax + 1];   // UTF-8, NUL-terminated
};

struct TimeSync {
  uint32_t unix;                     // UTC seconds
  int16_t  tzOffsetMin;              // local = UTC + offset
  uint16_t millis;                   // 0..999 sub-second part of `unix`
};

struct Revision {
  uint32_t revision;
  uint16_t changedMask;
  uint8_t  source;                   // ChangeSource
  uint8_t  result;                   // Result
};

struct Message {
  uint8_t icon;
  uint8_t flags;
  uint8_t len;
  char    text[kMessageMax + 1];
};

// Broken-down local civil time, as the RTC stores it.
struct CivilTime {
  uint16_t year;     // 4-digit
  uint8_t  month;    // 1..12
  uint8_t  day;      // 1..31
  uint8_t  hour, minute, second;
  uint8_t  weekday;  // 0 = Sunday .. 6 = Saturday (BaseOS / RV-3028 convention)
};

// ---------------------------------------------------------------------------
// Little-endian helpers
// ---------------------------------------------------------------------------
inline void     putU16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
inline uint16_t getU16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline void     putU32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
inline uint32_t getU32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ---------------------------------------------------------------------------
// Colour conversion (must match web/index.html exactly)
// ---------------------------------------------------------------------------
// 8-bit channels -> RGB565 by truncating the low bits (BaseOS convention).
uint16_t rgb565FromRgb888(uint8_t r, uint8_t g, uint8_t b);
// RGB565 -> 8-bit channels by bit replication (0x1F -> 0xFF, 0x00 -> 0x00).
void     rgb888FromRgb565(uint16_t c, uint8_t &r, uint8_t &g, uint8_t &b);

// ---------------------------------------------------------------------------
// Encoders return the number of bytes written. Decoders validate fully and
// return RES_OK, or an error without touching `out`.
// ---------------------------------------------------------------------------
size_t encodeTheme(const Theme &t, uint8_t *out);
Result decodeTheme(const uint8_t *in, size_t len, Theme &out);

size_t encodeBrightness(uint8_t level, uint8_t *out);
// Values below kBrightnessMin are clamped up (not an error).
Result decodeBrightness(const uint8_t *in, size_t len, uint8_t &out);

size_t encodeFace(const FaceCfg &f, uint8_t *out);
// `styleCount` comes from the firmware's face style table.
Result decodeFace(const uint8_t *in, size_t len, uint8_t styleCount, FaceCfg &out);

size_t encodeSlots(const SlotCfg *slots, uint8_t *out);       // kSlotCount entries
Result decodeSlots(const uint8_t *in, size_t len, SlotCfg *out); // normalises arg/flags

// Texts: write = [index][utf8...], read = kTextCount x [len][utf8...]
size_t encodeTextWrite(uint8_t index, const char *utf8, size_t len, uint8_t *out);
Result decodeTextWrite(const uint8_t *in, size_t len, uint8_t &index, TextEntry &out);
size_t encodeTextsRead(const TextEntry *texts, uint8_t *out);

// Feeds: write = [index][icon][kind][u32 expiresAt][u32 targetAt][utf8...],
//        read  = kFeedCount x [len][icon][kind][u32 exp][u32 tgt][utf8...]
size_t encodeFeedWrite(uint8_t index, const FeedEntry &f, uint8_t *out);
Result decodeFeedWrite(const uint8_t *in, size_t len, uint8_t &index, FeedEntry &out);
size_t encodeFeedsRead(const FeedEntry *feeds, uint8_t *out);

// Time: write = [u32 unix][i16 tz][u16 ms], read = [u32 unix][i16 tz][u8 rtcOk]
size_t encodeTimeWrite(const TimeSync &t, uint8_t *out);
Result decodeTimeWrite(const uint8_t *in, size_t len, TimeSync &out);
size_t encodeTimeRead(uint32_t unix, int16_t tzOffsetMin, bool rtcOk, uint8_t *out);

// Auth: write = [u32 code], read = [state][attemptsLeft][secondsLeft]
size_t encodeAuthWrite(uint32_t code, uint8_t *out);
Result decodeAuthWrite(const uint8_t *in, size_t len, uint32_t &code);
size_t encodeAuthRead(uint8_t state, uint8_t attemptsLeft, uint8_t secondsLeft, uint8_t *out);

size_t encodeRevision(const Revision &r, uint8_t *out);
Result decodeRevision(const uint8_t *in, size_t len, Revision &out);

// Message: [icon][flags][utf8 1..kMessageMax]
size_t encodeMessage(const Message &m, uint8_t *out);
Result decodeMessage(const uint8_t *in, size_t len, Message &out);

// Control: [opcode][args...]. Only the opcode is defined in v1.0.
Result decodeControl(const uint8_t *in, size_t len, uint8_t &op);

// Info (read-only): fixed 14-byte header then length-prefixed strings:
//   [major][minor][u16 caps][slotCount][textCount][textMax][feedCount]
//   [feedTextMax][messageMax][sourceMax][iconMax][brightnessMin][styleCount]
//   styleCount x ([len][ascii name]), [len][firmware], [len][device name]
static const size_t kInfoHeaderLen = 14;
struct InfoParams {
  uint16_t    capabilities;
  uint8_t     styleCount;
  const char *const *styleNames;   // styleCount entries
  const char *firmware;            // e.g. "1.0.0"
  const char *deviceName;          // e.g. "EWatch-1A2B"
};
size_t encodeInfo(const InfoParams &p, uint8_t *out, size_t cap);

// ---------------------------------------------------------------------------
// Civil time helpers (proleptic Gregorian, valid for 1970..2099)
// ---------------------------------------------------------------------------
int32_t  daysFromCivil(int32_t y, uint32_t m, uint32_t d);   // days since 1970-01-01
void     civilFromDays(int32_t z, int32_t &y, uint32_t &m, uint32_t &d);
uint8_t  weekdayFromDays(int32_t z);                          // 0 = Sunday
// Unix UTC + offset -> local broken-down time.
void     unixToCivil(uint32_t unix, int16_t tzOffsetMin, CivilTime &out);
// Local broken-down time - offset -> Unix UTC.
uint32_t civilToUnix(const CivilTime &t, int16_t tzOffsetMin);

}  // namespace bleproto
