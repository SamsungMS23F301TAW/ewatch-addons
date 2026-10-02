// Span compositor: the fast path must be pixel-identical to a naive per-pixel
// reference for every layer type, offset, clip, clamp and wrap combination.
#include <unity.h>
#include <string.h>
#include <stdlib.h>
#include <vector>
#include "tp_layer.h"
#include "tp_compose.h"
#include "tp_color.h"

using namespace tp;

void setUp() {}
void tearDown() {}

static uint32_t rnd(uint32_t &s) { s = s * 1664525u + 1013904223u; return s >> 8; }

// Random but "art-like" content: runs of indices, transparent gaps and
// anti-aliased edge pixels.
static void fillIndexed(Layer &L, uint32_t &seed, bool full) {
  for (int y = 0; y < L.h; y++) {
    int x = 0;
    while (x < L.w) {
      int run = 1 + (int)(rnd(seed) % 24);
      int kind = (int)(rnd(seed) % 5);
      uint8_t ix = (uint8_t)(1 + rnd(seed) % 250);
      for (int k = 0; k < run && x < L.w; k++, x++) {
        size_t i = (size_t)y * L.w + x;
        if (full || kind != 0) {
          L.idx[i] = (kind == 4) ? (uint8_t)(1 + rnd(seed) % 250) : ix;   // varied run
          if (L.alpha) L.alpha[i] = (kind == 3) ? (uint8_t)(1 + rnd(seed) % 254) : 255;
        } else {
          L.idx[i] = 0;
          if (L.alpha) L.alpha[i] = 0;
        }
      }
    }
  }
  if (full) {   // a fully opaque row exercises the occlusion start
    for (int x = 0; x < L.w; x++) {
      L.idx[(size_t)(L.h - 1) * L.w + x] = 7;
      if (L.alpha) L.alpha[(size_t)(L.h - 1) * L.w + x] = 255;
    }
  }
  for (int b = 0; b < L.nBands; b++)
    for (int i = 0; i < 256; i++) L.luts[b * 256 + i] = (uint16_t)(rnd(seed) & 0xFFFF);
}

static void fillAlpha(Layer &L, uint32_t &seed) {
  for (size_t i = 0; i < (size_t)L.w * L.h; i++) {
    uint32_t r = rnd(seed) % 10;
    L.alpha[i] = r < 4 ? 0 : (r < 7 ? 255 : (uint8_t)(rnd(seed) & 0xFF));
  }
  L.color = (uint16_t)(rnd(seed) & 0xFFFF);
  L.opacity = (rnd(seed) % 2) ? 255 : (uint8_t)(rnd(seed) & 0xFF);
}

static void runTrial(uint32_t seed) {
  const int W = 240, H = 120;
  std::vector<Layer> layers(6);
  // 0: RGB sky, clamped top and bottom, sometimes wrapped
  {
    Layer &L = layers[0];
    L.allocRGB((int16_t)(W + 30 + rnd(seed) % 40), (int16_t)(40 + rnd(seed) % 60));
    for (size_t i = 0; i < (size_t)L.w * L.h; i++) L.rgb[i] = (uint16_t)(rnd(seed) & 0xFFFF);
    L.x0 = (int16_t)(-(int)(rnd(seed) % 30));
    L.y0 = (int16_t)(-(int)(rnd(seed) % 20));
    L.clampTop = L.clampBottom = true;
    L.wrapX = (rnd(seed) % 3) == 0;
    L.encodeSpans();
  }
  // 1..3: indexed terrain-like layers, some with alpha, some wrapped
  for (int k = 1; k <= 3; k++) {
    Layer &L = layers[k];
    bool alpha = (rnd(seed) % 3) != 0;
    L.allocIndexed((int16_t)(W + rnd(seed) % 60), (int16_t)(20 + rnd(seed) % 80), alpha,
                   (uint8_t)(1 + rnd(seed) % 8));
    fillIndexed(L, seed, (rnd(seed) % 2) == 0);
    L.x0 = (int16_t)(-(int)(rnd(seed) % 30));
    L.y0 = (int16_t)(rnd(seed) % 90);
    L.clampBottom = (rnd(seed) % 2) == 0;
    L.wrapX = (rnd(seed) % 4) == 0;
    L.encodeSpans();
  }
  // 4..5: alpha sprites (clock, shadow)
  for (int k = 4; k <= 5; k++) {
    Layer &L = layers[k];
    L.allocAlpha((int16_t)(60 + rnd(seed) % 200), (int16_t)(10 + rnd(seed) % 40));
    fillAlpha(L, seed);
    L.x0 = (int16_t)((int)(rnd(seed) % 120) - 30);
    L.y0 = (int16_t)(rnd(seed) % 100);
    L.encodeSpans();
  }
  Layer *stack[6];
  for (int i = 0; i < 6; i++) stack[i] = &layers[i];
  std::vector<uint16_t> a((size_t)W * H), b((size_t)W * H);
  for (int frame = 0; frame < 12; frame++) {
    for (int i = 0; i < 6; i++) {
      layers[i].dx = (int16_t)((int)(rnd(seed) % 61) - 30);
      layers[i].dy = (int16_t)((int)(rnd(seed) % 41) - 20);
      layers[i].animDx = (int16_t)((rnd(seed) % 5) == 0 ? (int)(rnd(seed) % 500) : 0);
      layers[i].animDy = (int16_t)((rnd(seed) % 7) == 0 ? (int)(rnd(seed) % 150) : 0);
      layers[i].visible = (rnd(seed) % 9) != 0;
    }
    memset(a.data(), 0xAB, a.size() * 2);
    memset(b.data(), 0xCD, b.size() * 2);
    // compose in uneven strips like the watch does
    int y = 0;
    while (y < H) {
      int n = 1 + (int)(rnd(seed) % 40);
      if (y + n > H) n = H - y;
      composeRows(stack, 6, y, y + n, W, a.data() + (size_t)y * W, W);
      y += n;
    }
    composeRowsReference(stack, 6, 0, H, W, b.data(), W);
    for (size_t i = 0; i < a.size(); i++) {
      if (a[i] != b[i]) {
        char msg[96];
        snprintf(msg, sizeof(msg), "seed %u frame %d pixel (%d,%d): fast %04x ref %04x", seed,
                 frame, (int)(i % W), (int)(i / W), a[i], b[i]);
        TEST_FAIL_MESSAGE(msg);
      }
    }
  }
  for (auto &L : layers) L.release();
}

static void test_fast_path_matches_reference() {
  for (uint32_t seed = 1; seed <= 60; seed++) runTrial(seed * 2654435761u);
}

static void test_span_encoding_counts() {
  Layer L;
  L.allocIndexed(32, 3, true, 1);
  // row 0: transparent | 10 solid | 2 blend | 5 varied
  for (int x = 4; x < 14; x++) { L.idx[x] = 9; L.alpha[x] = 255; }
  L.idx[14] = 9; L.alpha[14] = 100;
  L.idx[15] = 9; L.alpha[15] = 30;
  for (int x = 16; x < 21; x++) { L.idx[x] = (uint8_t)(x); L.alpha[x] = 255; }
  // row 1: fully opaque single colour
  for (int x = 0; x < 32; x++) { L.idx[32 + x] = 3; L.alpha[32 + x] = 255; }
  // row 2: empty
  TEST_ASSERT_TRUE(L.encodeSpans());
  TEST_ASSERT_EQUAL_UINT32(3, L.rowStart[1] - L.rowStart[0]);
  const Span *s = L.spans + L.rowStart[0];
  TEST_ASSERT_EQUAL_UINT8(SPAN_SOLID, s[0].kind);
  TEST_ASSERT_EQUAL_INT16(4, s[0].x);
  TEST_ASSERT_EQUAL_INT16(10, s[0].len);
  TEST_ASSERT_EQUAL_UINT8(SPAN_BLEND, s[1].kind);
  TEST_ASSERT_EQUAL_INT16(2, s[1].len);
  TEST_ASSERT_EQUAL_UINT8(SPAN_LUT, s[2].kind);
  TEST_ASSERT_EQUAL_INT16(5, s[2].len);
  TEST_ASSERT_EQUAL_UINT32(1, L.rowStart[2] - L.rowStart[1]);
  TEST_ASSERT_TRUE(L.rowFlags[1] & ROW_OPAQUE);
  TEST_ASSERT_FALSE(L.rowFlags[0] & ROW_OPAQUE);
  TEST_ASSERT_EQUAL_UINT32(0, L.rowStart[3] - L.rowStart[2]);
  L.release();
}

static void test_blend565_endpoints() {
  TEST_ASSERT_EQUAL_HEX16(0xF800, blend565(0xF800, 0x001F, 255));
  TEST_ASSERT_EQUAL_HEX16(0x001F, blend565(0xF800, 0x001F, 0));
  uint16_t mid = blend565(0xFFFF, 0x0000, 128);
  RGB c = from565(mid);
  TEST_ASSERT_TRUE(c.r > 110 && c.r < 145);
  TEST_ASSERT_EQUAL_HEX16(0x0000, darken565(0xFFFF, 255));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_span_encoding_counts);
  RUN_TEST(test_blend565_endpoints);
  RUN_TEST(test_fast_path_matches_reference);
  return UNITY_END();
}
