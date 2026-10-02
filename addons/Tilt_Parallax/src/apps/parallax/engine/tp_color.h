// Tilt Parallax — colour helpers shared by the art generators, the LUT
// builder and the compositor. Pure C++ (no Arduino), so the same code runs in
// the host preview tool and the unit tests.
#pragma once
#include <stdint.h>
#include <math.h>

namespace tp {

// Floating-point RGB in 0..255 sRGB-ish units. The art is stylised, so we mix
// in this space directly rather than converting to linear light.
struct RGB {
  float r, g, b;
};

static inline RGB rgb(float r, float g, float b) { return RGB{ r, g, b }; }
static inline RGB rgbHex(uint32_t hex) {
  return RGB{ (float)((hex >> 16) & 0xFF), (float)((hex >> 8) & 0xFF),
              (float)(hex & 0xFF) };
}
static inline float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}
static inline float sat(float v) { return clampf(v, 0.0f, 1.0f); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float smoothstepf(float e0, float e1, float x) {
  float t = sat((x - e0) / (e1 - e0));
  return t * t * (3.0f - 2.0f * t);
}
static inline RGB mix(RGB a, RGB b, float t) {
  return RGB{ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t };
}
static inline RGB mul(RGB a, RGB b) {   // modulate, b in 0..255 = 0..1
  return RGB{ a.r * b.r / 255.0f, a.g * b.g / 255.0f, a.b * b.b / 255.0f };
}
static inline RGB scale(RGB a, float s) { return RGB{ a.r * s, a.g * s, a.b * s }; }
static inline RGB add(RGB a, RGB b) { return RGB{ a.r + b.r, a.g + b.g, a.b + b.b }; }
static inline float luma(RGB c) { return 0.299f * c.r + 0.587f * c.g + 0.114f * c.b; }
// Pull saturation toward grey (s < 1) or push it (s > 1).
static inline RGB saturate(RGB c, float s) {
  float l = luma(c);
  return RGB{ l + (c.r - l) * s, l + (c.g - l) * s, l + (c.b - l) * s };
}
// Screen blend: brightens, never exceeds 255. Good for glows.
static inline RGB screen(RGB a, RGB b, float t) {
  RGB s{ 255.0f - (255.0f - a.r) * (255.0f - b.r) / 255.0f,
         255.0f - (255.0f - a.g) * (255.0f - b.g) / 255.0f,
         255.0f - (255.0f - a.b) * (255.0f - b.b) / 255.0f };
  return mix(a, s, t);
}

static inline uint16_t to565(RGB c) {
  int r = (int)(clampf(c.r, 0, 255) + 0.5f);
  int g = (int)(clampf(c.g, 0, 255) + 0.5f);
  int b = (int)(clampf(c.b, 0, 255) + 0.5f);
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// 4x4 Bayer matrix, values 0..15.
static inline int bayer4(int x, int y) {
  static const uint8_t m[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 },
                                   { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
  return m[y & 3][x & 3];
}

// Ordered-dithered RGB565 conversion. Removes the banding that a smooth sky
// gradient otherwise shows at 5/6 bits per channel.
static inline uint16_t to565Dither(RGB c, int x, int y) {
  float t = (bayer4(x, y) + 0.5f) / 16.0f;            // 0..1
  int r = (int)(clampf(c.r, 0, 255) * (31.0f / 255.0f) + t);
  int g = (int)(clampf(c.g, 0, 255) * (63.0f / 255.0f) + t);
  int b = (int)(clampf(c.b, 0, 255) * (31.0f / 255.0f) + t);
  if (r > 31) r = 31;
  if (g > 63) g = 63;
  if (b > 31) b = 31;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

static inline RGB from565(uint16_t c) {
  int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
  return RGB{ (float)((r << 3) | (r >> 2)), (float)((g << 2) | (g >> 4)),
              (float)((b << 3) | (b >> 2)) };
}

// Alpha blend two RGB565 colours, a8 = 0..255 weight of `fg`. Uses the
// "spread the fields across a 32-bit word" trick: one multiply per channel
// group instead of three.
static inline uint16_t blend565(uint16_t fg, uint16_t bg, uint32_t a8) {
  uint32_t a = (a8 + 4) >> 3;                         // 0..32
  uint32_t f = ((uint32_t)fg | ((uint32_t)fg << 16)) & 0x07E0F81Fu;
  uint32_t b = ((uint32_t)bg | ((uint32_t)bg << 16)) & 0x07E0F81Fu;
  uint32_t r = ((f * a + b * (32u - a)) >> 5) & 0x07E0F81Fu;
  return (uint16_t)(r | (r >> 16));
}

// Darken an RGB565 colour toward black; a8 = 0..255 strength.
static inline uint16_t darken565(uint16_t bg, uint32_t a8) {
  uint32_t k = 32u - ((a8 + 4) >> 3);                 // keep fraction, 0..32
  uint32_t b = ((uint32_t)bg | ((uint32_t)bg << 16)) & 0x07E0F81Fu;
  uint32_t r = ((b * k) >> 5) & 0x07E0F81Fu;
  return (uint16_t)(r | (r >> 16));
}

}  // namespace tp
