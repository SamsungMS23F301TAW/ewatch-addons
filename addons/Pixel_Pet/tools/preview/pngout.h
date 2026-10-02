// Minimal PNG writer for the host previews (zlib from the macOS SDK).
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include <zlib.h>

namespace pngout {

inline void put32(std::vector<uint8_t> &v, uint32_t x) {
  v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16));
  v.push_back((uint8_t)(x >> 8));  v.push_back((uint8_t)x);
}

inline void chunk(FILE *f, const char *type, const std::vector<uint8_t> &data) {
  std::vector<uint8_t> c;
  put32(c, (uint32_t)data.size());
  c.insert(c.end(), type, type + 4);
  c.insert(c.end(), data.begin(), data.end());
  uint32_t crc = crc32(0, c.data() + 4, (uInt)(c.size() - 4));
  put32(c, crc);
  fwrite(c.data(), 1, c.size(), f);
}

// RGB565 buffer -> PNG, each source pixel repeated `up` times in x and y.
inline bool write565(const char *path, const uint16_t *buf, int w, int h, int up = 1) {
  const int W = w * up, H = h * up;
  std::vector<uint8_t> raw;
  raw.reserve((size_t)(W * 3 + 1) * H);
  for (int y = 0; y < H; y++) {
    raw.push_back(0);
    const uint16_t *row = buf + (y / up) * w;
    for (int x = 0; x < W; x++) {
      uint16_t c = row[x / up];
      uint8_t r = (uint8_t)(((c >> 11) & 31) * 255 / 31);
      uint8_t g = (uint8_t)(((c >> 5) & 63) * 255 / 63);
      uint8_t b = (uint8_t)((c & 31) * 255 / 31);
      raw.push_back(r); raw.push_back(g); raw.push_back(b);
    }
  }
  uLongf zlen = compressBound((uLong)raw.size());
  std::vector<uint8_t> z(zlen);
  if (compress2(z.data(), &zlen, raw.data(), (uLong)raw.size(), 9) != Z_OK) return false;
  z.resize(zlen);
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  fwrite(sig, 1, 8, f);
  std::vector<uint8_t> ihdr;
  put32(ihdr, (uint32_t)W); put32(ihdr, (uint32_t)H);
  ihdr.push_back(8); ihdr.push_back(2); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
  chunk(f, "IHDR", ihdr);
  chunk(f, "IDAT", z);
  chunk(f, "IEND", {});
  fclose(f);
  return true;
}

}  // namespace pngout
