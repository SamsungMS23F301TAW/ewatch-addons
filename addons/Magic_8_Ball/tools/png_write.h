// Minimal PNG writer for host tools (RGB565 frame buffer -> 8-bit RGB PNG).
// Uses the system zlib (link with -lz).
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>

#include <string>
#include <vector>

namespace pngw {

inline void put32(std::vector<uint8_t> &v, uint32_t x) {
  v.push_back((uint8_t)(x >> 24));
  v.push_back((uint8_t)(x >> 16));
  v.push_back((uint8_t)(x >> 8));
  v.push_back((uint8_t)x);
}

inline void chunk(FILE *f, const char *type, const std::vector<uint8_t> &data) {
  std::vector<uint8_t> head;
  put32(head, (uint32_t)data.size());
  fwrite(head.data(), 1, 4, f);
  std::vector<uint8_t> body(type, type + 4);
  body.insert(body.end(), data.begin(), data.end());
  fwrite(body.data(), 1, body.size(), f);
  uint32_t crc = (uint32_t)crc32(0L, body.data(), (uInt)body.size());
  std::vector<uint8_t> tail;
  put32(tail, crc);
  fwrite(tail.data(), 1, 4, f);
}

// Write an RGB image (w*h*3 bytes). `scale` > 1 does nearest-neighbour zoom.
inline bool writeRGB(const std::string &path, const uint8_t *rgb, int w, int h, int scale = 1) {
  int W = w * scale, H = h * scale;
  std::vector<uint8_t> raw;
  raw.reserve((size_t)(W * 3 + 1) * H);
  for (int y = 0; y < H; y++) {
    raw.push_back(0);
    const uint8_t *row = rgb + (size_t)(y / scale) * w * 3;
    for (int x = 0; x < W; x++) {
      const uint8_t *p = row + (x / scale) * 3;
      raw.push_back(p[0]);
      raw.push_back(p[1]);
      raw.push_back(p[2]);
    }
  }
  uLongf cap = compressBound((uLong)raw.size());
  std::vector<uint8_t> z(cap);
  if (compress2(z.data(), &cap, raw.data(), (uLong)raw.size(), 9) != Z_OK) return false;
  z.resize(cap);
  FILE *f = fopen(path.c_str(), "wb");
  if (!f) return false;
  const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  fwrite(sig, 1, 8, f);
  std::vector<uint8_t> ihdr;
  put32(ihdr, (uint32_t)W);
  put32(ihdr, (uint32_t)H);
  ihdr.push_back(8);   // bit depth
  ihdr.push_back(2);   // colour type RGB
  ihdr.push_back(0);
  ihdr.push_back(0);
  ihdr.push_back(0);
  chunk(f, "IHDR", ihdr);
  chunk(f, "IDAT", z);
  chunk(f, "IEND", {});
  fclose(f);
  return true;
}

// Expand an RGB565 frame buffer to RGB888 the way the panel shows it.
inline void rgb565ToRgb(const uint16_t *fb, int n, uint8_t *out) {
  for (int i = 0; i < n; i++) {
    uint16_t c = fb[i];
    int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    out[i * 3 + 0] = (uint8_t)((r << 3) | (r >> 2));
    out[i * 3 + 1] = (uint8_t)((g << 2) | (g >> 4));
    out[i * 3 + 2] = (uint8_t)((b << 3) | (b >> 2));
  }
}

}  // namespace pngw
