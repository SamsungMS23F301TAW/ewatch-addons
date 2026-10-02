// Meeting Countdown — small text helpers shared by the parser, the serial
// protocol and the renderer. Pure C++.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace mc {

// FNV-1a, usable incrementally: h = fnv1a(chunk, n, h).
constexpr uint32_t kFnvSeed = 2166136261u;
uint32_t fnv1a(const char *s, size_t n, uint32_t h = kFnvSeed);
inline uint32_t fnv1aByte(uint8_t c, uint32_t h) { return (h ^ c) * 16777619u; }

// iCalendar TEXT unescape in place: "\\," -> ",", "\\;" -> ";",
// "\\n"/"\\N" -> ' ', "\\\\" -> "\\". Returns the new length.
size_t icsUnescape(char *s, size_t n);

// Fold UTF-8 (or stray Latin-1) into printable ASCII for the watch's GFX
// fonts: accents are stripped (é -> e, ß -> ss, Ł -> L), typographic quotes
// and dashes become ASCII, everything else non-ASCII is dropped. Runs of
// whitespace collapse to one space and the result is trimmed. Writes at most
// cap-1 bytes plus a NUL; returns the length written.
size_t foldToAscii(const char *in, size_t n, char *out, size_t cap);

// strncpy that always terminates and never splits... ASCII only, so plain.
void copyStr(char *dst, size_t cap, const char *src);

// Case-insensitive ASCII compare of the first n bytes.
bool equalsNoCase(const char *a, const char *b, size_t n);

}  // namespace mc
