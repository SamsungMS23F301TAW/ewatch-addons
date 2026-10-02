// Face data-source slots. See face_slots.h. Mirrored in web/index.html
// (search for "FACE SLOTS MIRROR"); keep the two in step.
#include "face_slots.h"

#include <stdio.h>
#include <string.h>

namespace faceslots {

using namespace bleproto;

// ---------------------------------------------------------------------------
// UTF-8 -> CP437 glyphs
// ---------------------------------------------------------------------------
namespace {

struct GlyphMap { uint16_t cp; uint8_t glyph; };

// Sorted by code point for binary search. Accented capitals the CP437 font
// lacks fall back to their base letter.
const GlyphMap kMap[] = {
  {0x00A0, ' '},  {0x00A1, 0xAD}, {0x00A2, 0x9B}, {0x00A3, 0x9C}, {0x00A5, 0x9D},
  {0x00A7, 0x15}, {0x00AA, 0xA6}, {0x00AB, 0xAE}, {0x00AC, 0xAA}, {0x00B0, 0xF8},
  {0x00B1, 0xF1}, {0x00B2, 0xFD}, {0x00B5, 0xE6}, {0x00B6, 0x14}, {0x00B7, 0xFA},
  {0x00BA, 0xA7}, {0x00BB, 0xAF}, {0x00BC, 0xAC}, {0x00BD, 0xAB}, {0x00BF, 0xA8},
  {0x00C0, 'A'},  {0x00C1, 'A'},  {0x00C2, 'A'},  {0x00C3, 'A'},  {0x00C4, 0x8E},
  {0x00C5, 0x8F}, {0x00C6, 0x92}, {0x00C7, 0x80}, {0x00C8, 'E'},  {0x00C9, 0x90},
  {0x00CA, 'E'},  {0x00CB, 'E'},  {0x00CC, 'I'},  {0x00CD, 'I'},  {0x00CE, 'I'},
  {0x00CF, 'I'},  {0x00D0, 'D'},  {0x00D1, 0xA5}, {0x00D2, 'O'},  {0x00D3, 'O'},
  {0x00D4, 'O'},  {0x00D5, 'O'},  {0x00D6, 0x99}, {0x00D7, 'x'},  {0x00D8, 'O'},
  {0x00D9, 'U'},  {0x00DA, 'U'},  {0x00DB, 'U'},  {0x00DC, 0x9A}, {0x00DD, 'Y'},
  {0x00DF, 0xE1}, {0x00E0, 0x85}, {0x00E1, 0xA0}, {0x00E2, 0x83}, {0x00E3, 'a'},
  {0x00E4, 0x84}, {0x00E5, 0x86}, {0x00E6, 0x91}, {0x00E7, 0x87}, {0x00E8, 0x8A},
  {0x00E9, 0x82}, {0x00EA, 0x88}, {0x00EB, 0x89}, {0x00EC, 0x8D}, {0x00ED, 0xA1},
  {0x00EE, 0x8C}, {0x00EF, 0x8B}, {0x00F0, 'd'},  {0x00F1, 0xA4}, {0x00F2, 0x95},
  {0x00F3, 0xA2}, {0x00F4, 0x93}, {0x00F5, 'o'},  {0x00F6, 0x94}, {0x00F7, 0xF6},
  {0x00F8, 'o'},  {0x00F9, 0x97}, {0x00FA, 0xA3}, {0x00FB, 0x96}, {0x00FC, 0x81},
  {0x00FD, 'y'},  {0x00FF, 0x98},
  {0x2013, '-'},  {0x2014, '-'},  {0x2018, '\''}, {0x2019, '\''}, {0x201C, '"'},
  {0x201D, '"'},  {0x2022, 0x07}, {0x20AC, 'E'},  {0x2190, 0x1B}, {0x2191, 0x18},
  {0x2192, 0x1A}, {0x2193, 0x19}, {0x2194, 0x1D}, {0x2195, 0x12}, {0x221A, 0xFB},
  {0x221E, 0xEC}, {0x2248, 0xF7}, {0x2264, 0xF3}, {0x2265, 0xF2}, {0x2302, 0x7F},
  {0x2588, 0xDB}, {0x2591, 0xB0}, {0x2592, 0xB1}, {0x2593, 0xB2}, {0x25A0, 0xFE},
  {0x25B2, 0x1E}, {0x25B6, 0x10}, {0x25BA, 0x10}, {0x25BC, 0x1F}, {0x25C0, 0x11},
  {0x25C4, 0x11}, {0x25CB, 0x09}, {0x25CF, 0x07}, {0x2600, 0x0F}, {0x263A, 0x01},
  {0x263B, 0x02}, {0x263C, 0x0F}, {0x2640, 0x0C}, {0x2642, 0x0B}, {0x2660, 0x06},
  {0x2663, 0x05}, {0x2665, 0x03}, {0x2666, 0x04}, {0x266A, 0x0E}, {0x266B, 0x0E},
  {0x2713, 0xFB}, {0x2714, 0xFB}, {0x2764, 0x03},
};
const size_t kMapCount = sizeof(kMap) / sizeof(kMap[0]);

const int kDrop = -1;       // emit nothing
const int kEllipsis = -2;   // emit "..."

int mapCodepoint(uint32_t cp) {
  if (cp == '\t') return ' ';
  if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0)) return kDrop;   // controls
  if (cp < 0x7F) return (int)cp;                                             // ASCII
  if (cp == 0x2026) return kEllipsis;
  if (cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0x2060 || cp == 0xFEFF ||
      (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0x1F3FB && cp <= 0x1F3FF)) {
    return kDrop;                                     // zero-width / modifiers
  }
  if (cp <= 0xFFFF) {
    size_t lo = 0, hi = kMapCount;
    while (lo < hi) {
      size_t mid = (lo + hi) / 2;
      if (kMap[mid].cp == cp) return kMap[mid].glyph;
      if (kMap[mid].cp < cp) lo = mid + 1; else hi = mid;
    }
  }
  return '?';
}

}  // namespace

size_t utf8ToGlyphs(const char *utf8, size_t len, uint8_t *out, size_t cap) {
  const uint8_t *s = (const uint8_t *)utf8;
  size_t i = 0, n = 0;
  while (i < len && n < cap) {
    uint8_t c = s[i];
    uint32_t cp = 0;
    size_t need = 0;
    uint32_t minCp = 0;
    if (c < 0x80)                { cp = c;        need = 0; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; need = 1; minCp = 0x80; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; need = 2; minCp = 0x800; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; need = 3; minCp = 0x10000; }
    else                         { out[n++] = '?'; i++; continue; }   // stray byte
    bool ok = (i + need < len) || need == 0;
    for (size_t k = 1; ok && k <= need; k++) {
      if (i + k >= len || (s[i + k] & 0xC0) != 0x80) ok = false;
      else cp = (cp << 6) | (s[i + k] & 0x3F);
    }
    if (ok && need > 0 &&
        (cp < minCp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))) {
      ok = false;                                     // overlong / surrogate
    }
    if (!ok) { out[n++] = '?'; i++; continue; }
    i += need + 1;
    int g = mapCodepoint(cp);
    if (g == kDrop) continue;
    if (g == kEllipsis) {
      for (int k = 0; k < 3 && n < cap; k++) out[n++] = '.';
      continue;
    }
    out[n++] = (uint8_t)g;
  }
  out[n] = 0;
  return n;
}

// ---------------------------------------------------------------------------
// Formatters
// ---------------------------------------------------------------------------
static const char *const kWday[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const kMon[]  = { "???", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

size_t formatDate(const CivilTime &t, bool shortForm, char *out, size_t cap) {
  const char *wd = (t.weekday < 7) ? kWday[t.weekday] : "---";
  const char *mn = (t.month >= 1 && t.month <= 12) ? kMon[t.month] : "???";
  int n = shortForm
      ? snprintf(out, cap, "%s %u %s", wd, (unsigned)t.day, mn)
      : snprintf(out, cap, "%s %u %s %u", wd, (unsigned)t.day, mn, (unsigned)t.year);
  if (n < 0) { out[0] = 0; return 0; }
  return (size_t)n < cap ? (size_t)n : cap - 1;
}

size_t formatCountdown(int64_t deltaSec, char *out, size_t cap) {
  int n;
  if (deltaSec <= 0) {
    n = snprintf(out, cap, "now");
  } else {
    int64_t mins = (deltaSec + 59) / 60;               // round up: "in 1m" until "now"
    if (mins < 60) {
      n = snprintf(out, cap, "in %um", (unsigned)mins);
    } else if (mins < 48 * 60) {
      unsigned h = (unsigned)(mins / 60), m = (unsigned)(mins % 60);
      n = m ? snprintf(out, cap, "in %uh%um", h, m) : snprintf(out, cap, "in %uh", h);
    } else {
      n = snprintf(out, cap, "in %ud", (unsigned)(mins / (24 * 60)));
    }
  }
  if (n < 0) { out[0] = 0; return 0; }
  return (size_t)n < cap ? (size_t)n : cap - 1;
}

bool feedVisible(const FeedEntry &f, uint32_t unixNow) {
  if (f.kind == FEED_VALUE && f.len == 0) return false;
  if (unixNow == 0) return f.kind == FEED_VALUE;       // no clock: hide countdowns
  uint32_t exp = f.expiresAt;
  if (f.kind == FEED_COUNTDOWN && exp == 0) exp = f.targetAt + 15u * 60u;
  if (exp != 0 && unixNow >= exp) return false;
  return true;
}

// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------
int16_t iconWidth(uint8_t icon, uint8_t size) {
  if (icon == 0) return 0;
  if (icon == kIconBattery) return (int16_t)(11 * size);
  return (int16_t)(8 * size);
}

int16_t iconGap(uint8_t size) { return (int16_t)(3 * size); }

int16_t contentWidth(const SlotContent &c) {
  if (c.len == 0 && c.icon == 0) return 0;
  int16_t w = (int16_t)(c.len * 6 * c.size);
  if (c.icon) w = (int16_t)(w + iconWidth(c.icon, c.size) + iconGap(c.size));
  return w;
}

bool sameContent(const SlotContent &a, const SlotContent &b) {
  return a.len == b.len && a.icon == b.icon && a.iconArg == b.iconArg &&
         a.color == b.color && a.size == b.size &&
         memcmp(a.glyphs, b.glyphs, a.len) == 0;
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
static size_t asciiToGlyphs(const char *s, uint8_t *out, size_t cap) {
  size_t n = 0;
  while (s[n] && n < cap) { out[n] = (uint8_t)s[n]; n++; }
  out[n] = 0;
  return n;
}

// Copy the first `keep` glyphs of `src`, drop trailing spaces, append "..".
// Returns the number of glyphs written (at most keep + 2).
static size_t truncateInto(const uint8_t *src, size_t keep, uint8_t *dst) {
  while (keep > 0 && src[keep - 1] == ' ') keep--;
  memcpy(dst, src, keep);
  dst[keep] = '.';
  dst[keep + 1] = '.';
  return keep + 2;
}

static int capacityFor(int16_t width, uint8_t icon, uint8_t size) {
  int16_t w = width;
  if (icon) w = (int16_t)(w - iconWidth(icon, size) - iconGap(size));
  if (w <= 0) return 0;
  int cap = w / (6 * size);
  return cap > kMaxGlyphs ? kMaxGlyphs : cap;
}

bool slotRaw(const SlotCfg &cfg, const FaceData &d, SlotRaw &out) {
  memset(&out, 0, sizeof(out));
  out.iconArg = 255;
  out.color = cfg.color <= kSlotColorMax ? cfg.color : (uint8_t)COLOR_FG;
  bool showIcon = (cfg.flags & kSlotFlagNoIcon) == 0;
  char tmp[32];

  switch (cfg.source) {
    case SRC_SECONDS:
      snprintf(tmp, sizeof(tmp), ":%02u", (unsigned)(d.now.second % 60));
      out.bodyLen = (uint8_t)asciiToGlyphs(tmp, out.body, kMaxGlyphs);
      break;
    case SRC_DATE:
    case SRC_SHORT_DATE:
      if (!d.rtcOk) return false;                 // stock face leaves the row blank
      formatDate(d.now, cfg.source == SRC_SHORT_DATE, tmp, sizeof(tmp));
      out.bodyLen = (uint8_t)asciiToGlyphs(tmp, out.body, kMaxGlyphs);
      break;
    case SRC_BATTERY:
      if (d.batOk) snprintf(tmp, sizeof(tmp), "%u%%", (unsigned)d.batPct);
      else         snprintf(tmp, sizeof(tmp), "--%%");
      out.bodyLen = (uint8_t)asciiToGlyphs(tmp, out.body, kMaxGlyphs);
      if (showIcon) { out.icon = kIconBattery; out.iconArg = d.batOk ? d.batPct : 255; }
      break;
    case SRC_TEXT: {
      if (!d.texts || cfg.arg >= kTextCount) return false;
      const TextEntry &t = d.texts[cfg.arg];
      out.bodyLen = (uint8_t)utf8ToGlyphs(t.text, t.len, out.body, kMaxGlyphs);
      break;
    }
    case SRC_FEED: {
      if (!d.feeds || cfg.arg >= kFeedCount) return false;
      const FeedEntry &f = d.feeds[cfg.arg];
      if (!feedVisible(f, d.unixNow)) return false;
      out.bodyLen = (uint8_t)utf8ToGlyphs(f.text, f.len, out.body, kMaxGlyphs);
      if (f.kind == FEED_COUNTDOWN) {
        out.countdown = true;
        int64_t delta = (int64_t)f.targetAt - (int64_t)d.unixNow;
        out.suffixLen = (uint8_t)formatCountdown(delta, out.suffix, sizeof(out.suffix));
      }
      if (showIcon && f.icon >= 1 && f.icon <= kIconMax) out.icon = f.icon;
      break;
    }
    default:
      return false;                               // SRC_NONE / unknown
  }
  if (out.bodyLen == 0 && out.suffixLen == 0) return false;
  out.source = cfg.source;
  return true;
}

void layoutSlot(const SlotCfg &cfg, const FaceData &d, uint8_t rowSize, int16_t width,
                SlotContent &out) {
  memset(&out, 0, sizeof(out));
  out.iconArg = 255;
  out.color = cfg.color <= kSlotColorMax ? cfg.color : (uint8_t)COLOR_FG;

  SlotRaw raw;
  if (!slotRaw(cfg, d, raw)) return;
  const uint8_t *body = raw.body;
  size_t bodyLen = raw.bodyLen;
  const char *suffix = raw.suffix;
  size_t suffixLen = raw.suffixLen;
  bool countdown = raw.countdown;
  uint8_t icon = raw.icon;
  if (icon == kIconBattery) out.iconArg = raw.iconArg;

  size_t total = countdown ? bodyLen + (bodyLen ? 1 : 0) + suffixLen : bodyLen;
  if (total == 0) return;

  // Largest size (natural size first, then down to kMinSize) that fits whole.
  uint8_t minSize = rowSize < kMinSize ? rowSize : kMinSize;
  uint8_t size = 0;
  for (uint8_t s = rowSize; s >= minSize && s > 0; s--) {
    if ((int)total <= capacityFor(width, icon, s)) { size = s; break; }
  }
  bool truncated = false;
  if (size == 0) { size = minSize; truncated = true; }
  int cap = capacityFor(width, icon, size);
  if (cap <= 0) return;

  size_t n = 0;
  if (!countdown) {
    if (!truncated) {
      memcpy(out.glyphs, body, bodyLen);
      n = bodyLen;
    } else if (cap >= 3) {
      n = truncateInto(body, (size_t)cap - 2, out.glyphs);
    } else {
      memcpy(out.glyphs, body, (size_t)cap);
      n = (size_t)cap;
    }
  } else {
    // "<label> <suffix>", shortening the label first and keeping the suffix.
    size_t labelCap = 0;
    if (!truncated) {
      labelCap = bodyLen;
    } else if ((size_t)cap > suffixLen + 1) {
      labelCap = (size_t)cap - suffixLen - 1;
      if (labelCap < 3) labelCap = 0;             // not worth a stub; suffix only
    }
    if (labelCap > 0 && bodyLen > 0) {
      if (labelCap >= bodyLen) {
        memcpy(out.glyphs, body, bodyLen);
        n = bodyLen;
      } else {
        n = truncateInto(body, labelCap - 2, out.glyphs);
      }
      out.glyphs[n++] = ' ';
    }
    for (size_t k = 0; k < suffixLen && n < (size_t)cap; k++) {
      out.glyphs[n++] = (uint8_t)suffix[k];
    }
  }
  out.glyphs[n] = 0;
  out.len = (uint8_t)n;
  out.icon = icon;
  out.size = size;
  out.truncated = truncated;
  if (icon != kIconBattery) out.iconArg = 255;
}

}  // namespace faceslots
