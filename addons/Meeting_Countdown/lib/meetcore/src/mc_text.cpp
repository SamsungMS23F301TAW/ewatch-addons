#include "mc_text.h"
#include <string.h>

namespace mc {

uint32_t fnv1a(const char *s, size_t n, uint32_t h) {
  for (size_t i = 0; i < n; i++) h = fnv1aByte((uint8_t)s[i], h);
  return h;
}

size_t icsUnescape(char *s, size_t n) {
  size_t w = 0;
  for (size_t r = 0; r < n; r++) {
    char c = s[r];
    if (c == '\\' && r + 1 < n) {
      char e = s[++r];
      if (e == 'n' || e == 'N') c = ' ';
      else c = e;                   // \, \; \\ and anything else -> literal
    }
    s[w++] = c;
  }
  if (w < n) s[w] = '\0';
  return w;
}

void copyStr(char *dst, size_t cap, const char *src) {
  if (!cap) return;
  size_t i = 0;
  if (src) for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
  dst[i] = '\0';
}

bool equalsNoCase(const char *a, const char *b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    char x = a[i], y = b[i];
    if (x >= 'a' && x <= 'z') x = (char)(x - 32);
    if (y >= 'a' && y <= 'z') y = (char)(y - 32);
    if (x != y) return false;
    if (!x) return true;
  }
  return true;
}

// ---- UTF-8 -> ASCII folding ------------------------------------------------
// U+00C0..U+00FF. Two chars per entry; the second is '\0' for single letters.
static const char kLatin1[64][3] = {
  "A","A","A","A","A","A","AE","C","E","E","E","E","I","I","I","I",
  "D","N","O","O","O","O","O","x","O","U","U","U","U","Y","Th","ss",
  "a","a","a","a","a","a","ae","c","e","e","e","e","i","i","i","i",
  "d","n","o","o","o","o","o","/","o","u","u","u","u","y","th","y",
};
// U+0100..U+017F (Latin Extended-A), base letters.
static const char kLatinExtA[] =
  "AaAaAaCcCcCcCcDd"
  "DdEeEeEeEeEeGgGg"
  "GgGgHhHhIiIiIiIi"
  "IiIiJjKkkLlLlLlL"
  "lLlNnNnNnnNnOoOo"
  "OoOoRrRrRrSsSsSs"
  "SsTtTtTtUuUuUuUu"
  "UuUuWwYyYZzZzZzs";

// Returns the ASCII replacement for code point cp (may be empty).
static const char *foldCodePoint(uint32_t cp, char tmp[3]) {
  tmp[0] = tmp[1] = tmp[2] = '\0';
  if (cp < 0x80) {
    if (cp < 0x20 || cp == 0x7F) { tmp[0] = ' '; return tmp; }
    tmp[0] = (char)cp;
    return tmp;
  }
  if (cp >= 0xC0 && cp <= 0xFF) return kLatin1[cp - 0xC0];
  if (cp >= 0x100 && cp <= 0x17F) {
    if (cp == 0x132) return "IJ";
    if (cp == 0x133) return "ij";
    if (cp == 0x152) return "OE";
    if (cp == 0x153) return "oe";
    tmp[0] = kLatinExtA[cp - 0x100];
    return tmp;
  }
  switch (cp) {
    case 0xA0: case 0x2002: case 0x2003: case 0x2009: case 0x200A: case 0x202F:
    case 0x3000:
      return " ";
    case 0xA1: return "!";
    case 0xBF: return "?";
    case 0xAB: case 0xBB: case 0x201C: case 0x201D: case 0x201E: case 0x201F:
      return "\"";
    case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032:
    case 0x2039: case 0x203A: case 0xB4:
      return "'";
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014:
    case 0x2015: case 0x2212:
      return "-";
    case 0x2026: return "...";
    case 0x2022: case 0xB7: case 0x2027: case 0x2219: return "-";
    case 0xD7: return "x";
    case 0x2192: return "->";
    case 0x2190: return "<-";
    case 0x20AC: return "EUR";
    case 0xA3: return "GBP";
    case 0xA9: return "(c)";
    case 0xAE: return "(R)";
    case 0x2122: return "TM";
    case 0x218: return "S";
    case 0x219: return "s";
    case 0x21A: return "T";
    case 0x21B: return "t";
    default: break;
  }
  return "";                                       // emoji, CJK, ...: dropped
}

size_t foldToAscii(const char *in, size_t n, char *out, size_t cap) {
  if (!cap) return 0;
  size_t w = 0;
  bool lastSpace = true;                           // trims leading spaces
  size_t i = 0;
  while (i < n && in[i]) {
    uint8_t b = (uint8_t)in[i];
    uint32_t cp;
    size_t len = 1;
    if (b < 0x80) {
      cp = b;
    } else if ((b & 0xE0) == 0xC0 && i + 1 < n && ((uint8_t)in[i + 1] & 0xC0) == 0x80) {
      cp = ((b & 0x1Fu) << 6) | ((uint8_t)in[i + 1] & 0x3Fu);
      len = 2;
    } else if ((b & 0xF0) == 0xE0 && i + 2 < n && ((uint8_t)in[i + 1] & 0xC0) == 0x80 &&
               ((uint8_t)in[i + 2] & 0xC0) == 0x80) {
      cp = ((b & 0x0Fu) << 12) | (((uint8_t)in[i + 1] & 0x3Fu) << 6) |
           ((uint8_t)in[i + 2] & 0x3Fu);
      len = 3;
    } else if ((b & 0xF8) == 0xF0 && i + 3 < n && ((uint8_t)in[i + 1] & 0xC0) == 0x80 &&
               ((uint8_t)in[i + 2] & 0xC0) == 0x80 && ((uint8_t)in[i + 3] & 0xC0) == 0x80) {
      cp = 0x10000;                                // astral plane: emoji etc. -> dropped
      len = 4;
    } else {
      cp = b;                                      // stray byte: treat as Latin-1
    }
    i += len;
    if (cp == 0xFE0F || cp == 0x200D || cp == 0x200B || cp == 0xFEFF) continue;
    char tmp[3];
    const char *rep = foldCodePoint(cp, tmp);
    for (const char *p = rep; *p; p++) {
      char c = *p;
      if (c == ' ' || c == '\t') {
        if (lastSpace) continue;
        lastSpace = true;
        c = ' ';
      } else {
        lastSpace = false;
      }
      if (w + 1 >= cap) goto done;
      out[w++] = c;
    }
  }
done:
  while (w > 0 && out[w - 1] == ' ') w--;
  out[w] = '\0';
  return w;
}

}  // namespace mc
