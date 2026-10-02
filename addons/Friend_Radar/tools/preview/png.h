// Minimal PNG writer for host previews (RGB565 frame -> 8-bit RGB PNG).
// Uses the system zlib; host-only, never compiled into the firmware.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include <zlib.h>

namespace png {

static void put32(std::vector<uint8_t> &v, uint32_t x) {
  v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16));
  v.push_back((uint8_t)(x >> 8));  v.push_back((uint8_t)x);
}

static void chunk(FILE *f, const char *type, const std::vector<uint8_t> &data) {
  std::vector<uint8_t> buf;
  put32(buf, (uint32_t)data.size());
  buf.insert(buf.end(), type, type + 4);
  buf.insert(buf.end(), data.begin(), data.end());
  uint32_t crc = (uint32_t)crc32(0, buf.data() + 4, (uInt)(buf.size() - 4));
  put32(buf, crc);
  fwrite(buf.data(), 1, buf.size(), f);
}

// Writes `px` (w*h RGB565) scaled up by an integer `zoom` (nearest neighbour).
static bool writeRgb565(const char *path, const uint16_t *px, int w, int h, int zoom = 1) {
  int W = w * zoom, H = h * zoom;
  std::vector<uint8_t> raw;
  raw.reserve((size_t)(W * 3 + 1) * H);
  for (int y = 0; y < H; ++y) {
    raw.push_back(0);
    for (int x = 0; x < W; ++x) {
      uint16_t c = px[(y / zoom) * w + (x / zoom)];
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
  const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
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

}  // namespace png
