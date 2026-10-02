// Determinism and variety tests for the generative art (ALGO_VERSION 1).
#include <stdio.h>
#include <string.h>
#include <vector>
#include <unity.h>
#include "gf_art.h"
#include "gf_face.h"

using namespace gf;

static std::vector<uint32_t> gArt(kW * kH);
static std::vector<uint8_t> gScratch(kScratchBytes);

static uint32_t render(uint16_t day, uint16_t bucket, uint32_t chunk = 1u << 20,
                       uint32_t prefill = 0) {
  for (auto &p : gArt) p = prefill;
  Canvas cv; cv.px = gArt.data();
  ArtJob j;
  j.begin(&cv, gScratch.data(), day, bucket);
  int guard = 0;
  while (!j.run(chunk)) { TEST_ASSERT_TRUE(++guard < 2000000); }
  return canvasHash(cv);
}

static uint16_t D(int y, int m, int d) { return dayIndex((uint16_t)y, (uint8_t)m, (uint8_t)d); }

// Same inputs, same pixels — and the pre-existing canvas or scratch contents
// must not leak into the image.
void test_same_inputs_same_hash() {
  for (int i = 0; i < 21; i++) {
    uint16_t day = (uint16_t)(D(2026, 10, 1) + i);
    uint32_t a = render(day, 30);
    memset(gScratch.data(), 0xA5, gScratch.size());
    uint32_t b = render(day, 30, 1u << 20, 0x00123456);
    TEST_ASSERT_EQUAL_HEX32(a, b);
  }
}

// Rendering in tiny chunks across "frames" must equal a one-shot render.
void test_chunking_does_not_change_the_image() {
  for (int i = 0; i < 14; i++) {
    uint16_t day = (uint16_t)(D(2026, 3, 9) + i);
    uint32_t whole = render(day, 44);
    TEST_ASSERT_EQUAL_HEX32(whole, render(day, 44, 1));
    TEST_ASSERT_EQUAL_HEX32(whole, render(day, 44, 7));
  }
}

// Growing the piece step bucket by bucket (as the watch does while you walk)
// must land on exactly the image a fresh render at the final bucket gives.
void test_growth_is_incremental() {
  for (int i = 0; i < 14; i++) {
    uint16_t day = (uint16_t)(D(2026, 7, 13) + i);
    Canvas cv; cv.px = gArt.data();
    ArtJob j;
    j.begin(&cv, gScratch.data(), day, 0);
    while (!j.run(5)) {}
    const uint16_t path[] = { 3, 4, 20, 63, 64, 65, 90, 160 };
    for (uint16_t b : path) {
      TEST_ASSERT_TRUE(j.extendTo(b));
      while (!j.run(3)) {}
      uint32_t grown = canvasHash(cv);
      std::vector<uint32_t> keep(gArt);
      uint32_t fresh = render(day, b);
      TEST_ASSERT_EQUAL_HEX32_MESSAGE(fresh, grown, familyName(j.spec().family));
      gArt = keep;
    }
    TEST_ASSERT_FALSE(j.extendTo(10));       // shrinking needs a fresh begin()
  }
}

// No two days of a year look alike, and more steps change the picture.
void test_every_day_of_the_year_is_different() {
  static uint32_t hashes[366];
  int n = 0;
  for (uint16_t d = D(2026, 1, 1); d <= D(2026, 12, 31); d++) hashes[n++] = render(d, 32);
  for (int i = 0; i < n; i++)
    for (int k = i + 1; k < n; k++) TEST_ASSERT_NOT_EQUAL(hashes[i], hashes[k]);
  uint16_t day = D(2026, 10, 1);
  TEST_ASSERT_NOT_EQUAL(render(day, 0), render(day, 8));
  TEST_ASSERT_NOT_EQUAL(render(day, 8), render(day, 40));
  TEST_ASSERT_NOT_EQUAL(render(day, 64), render(day, 100));    // flourish
}

// Every Monday-based week shows all seven families; consecutive days never
// repeat a family (special days excepted).
void test_weekly_family_schedule() {
  for (uint16_t monday = D(2026, 1, 5); monday < D(2027, 12, 27); monday += 7) {
    bool seen[kFamilyCount] = { false };
    for (int k = 0; k < 7; k++) {
      ArtSpec s; describeDay((uint16_t)(monday + k), ALGO_VERSION, s);
      if (s.special == kOrdinary) seen[s.family] = true;
    }
    int count = 0;
    for (bool b : seen) count += b ? 1 : 0;
    TEST_ASSERT_TRUE(count >= 6);            // a solstice may replace one
  }
  for (uint16_t d = D(2026, 1, 1); d < D(2028, 1, 1); d++) {
    ArtSpec a, b;
    describeDay(d, ALGO_VERSION, a);
    describeDay((uint16_t)(d + 1), ALGO_VERSION, b);
    if (a.special == kOrdinary && b.special == kOrdinary) TEST_ASSERT_NOT_EQUAL(a.family, b.family);
  }
}

// Palette inks keep a real value contrast against their background.
void test_palettes_have_value_contrast() {
  for (uint16_t d = D(2026, 1, 1); d < D(2027, 1, 1); d++) {
    ArtSpec s; describeDay(d, ALGO_VERSION, s);
    if (s.special != kOrdinary) continue;
    int32_t bg = (luma(s.pal.bg0) + luma(s.pal.bg1)) / 2;
    int good = 0;
    for (int k = 0; k < 5; k++) {
      int32_t gap = s.pal.darkBg ? luma(s.pal.ink[k]) - bg : bg - luma(s.pal.ink[k]);
      if (gap >= 60) good++;
    }
    TEST_ASSERT_TRUE_MESSAGE(good >= 4, "palette lacks contrast");
    // Clock text is far from its halo.
    int32_t tg = luma(s.pal.text) - luma(s.pal.halo);
    if (tg < 0) tg = -tg;
    TEST_ASSERT_TRUE(tg >= 150);
  }
}

void test_special_days() {
  ArtSpec s;
  describeDay(D(2027, 1, 1), ALGO_VERSION, s);
  TEST_ASSERT_EQUAL(kNewYear, s.special);
  describeDay(D(2026, 6, 21), ALGO_VERSION, s);
  TEST_ASSERT_EQUAL(kJuneSolstice, s.special);
  TEST_ASSERT_EQUAL(kDunes, s.family);
  describeDay(D(2026, 12, 21), ALGO_VERSION, s);
  TEST_ASSERT_EQUAL(kDecemberSolstice, s.special);
  describeDay(D(2026, 3, 20), ALGO_VERSION, s);
  TEST_ASSERT_EQUAL(kMarchEquinox, s.special);
  describeDay(D(2026, 9, 23), ALGO_VERSION, s);
  TEST_ASSERT_EQUAL(kSeptemberEquinox, s.special);
  describeDay(D(2026, 9, 22), ALGO_VERSION, s);
  TEST_ASSERT_EQUAL(kOrdinary, s.special);
}

void test_dates_round_trip() {
  for (uint16_t d = 0; d < 40000; d += 7) {
    uint16_t y; uint8_t m, dd;
    dayToDate(d, y, m, dd);
    TEST_ASSERT_EQUAL_UINT16(d, dayIndex(y, m, dd));
  }
  TEST_ASSERT_EQUAL_UINT8(6, weekdayOf(0));                    // 2000-01-01, Saturday
  TEST_ASSERT_EQUAL_UINT8(4, weekdayOf(D(2026, 10, 1)));       // Thursday
  uint16_t idx;
  TEST_ASSERT_TRUE(parseIsoDate("2026-10-01", idx));
  TEST_ASSERT_EQUAL_UINT16(D(2026, 10, 1), idx);
  TEST_ASSERT_FALSE(parseIsoDate("2026-02-30", idx));
  TEST_ASSERT_FALSE(parseIsoDate("26-10-01", idx));
  char buf[11];
  formatIsoDate(D(2028, 2, 29), buf);
  TEST_ASSERT_EQUAL_STRING("2028-02-29", buf);
}

void test_growth_curve() {
  TEST_ASSERT_EQUAL_INT32(0, growthPermille(0));
  TEST_ASSERT_EQUAL_INT32(1000, growthPermille(FULL_BUCKET));
  TEST_ASSERT_EQUAL_INT32(1000, growthPermille(MAX_BUCKET));
  // Never shrinks; visibly grows with every bucket until the piece is
  // nearly complete (the ease-out curve flattens in the last few buckets).
  for (uint16_t b = 1; b <= MAX_BUCKET; b++) TEST_ASSERT_TRUE(growthPermille(b) >= growthPermille(b - 1));
  for (uint16_t b = 1; b <= 56; b++) TEST_ASSERT_TRUE(growthPermille(b) > growthPermille(b - 1));
  TEST_ASSERT_EQUAL_UINT16(0, bucketForSteps(249));
  TEST_ASSERT_EQUAL_UINT16(1, bucketForSteps(250));
  TEST_ASSERT_EQUAL_UINT16(MAX_BUCKET, bucketForSteps(10000000));
}

// The face overlay is deterministic too, and redraws when the minute changes.
void test_face_compositor() {
  static std::vector<uint8_t> mA(kW * kH), mB(kW * kH);
  static std::vector<uint16_t> o1(kW * kH), o2(kW * kH);
  uint16_t day = D(2026, 10, 1);
  render(day, 24);
  Canvas cv; cv.px = gArt.data();
  ArtSpec s; describeDay(day, ALGO_VERSION, s);
  FaceInputs in; in.day = day; in.hour = 10; in.minute = 42; in.steps = 6123; in.goal = 8000;
  FaceLayer L; L.maskText = mA.data(); L.maskHalo = mB.data();
  buildFaceLayer(s, in, 0, L);
  composeFrame(cv, s, in, L, o1.data(), 0, kH);
  composeFrame(cv, s, in, L, o2.data(), 0, kH);
  TEST_ASSERT_EQUAL_MEMORY(o1.data(), o2.data(), o1.size() * 2);
  uint32_t k1 = faceKey(s, in, 0);
  in.minute = 43;
  TEST_ASSERT_NOT_EQUAL(k1, faceKey(s, in, 0));
  char buf[16];
  formatSteps(12345, buf); TEST_ASSERT_EQUAL_STRING("12,345", buf);
  formatSteps(999, buf);   TEST_ASSERT_EQUAL_STRING("999", buf);
  formatSteps(1000, buf);  TEST_ASSERT_EQUAL_STRING("1,000", buf);
}

// The clock must stay legible on every piece: text pixels and the halo ring
// around them differ strongly in luminance, whatever the art does.
static int32_t luma565(uint16_t v) {
  int32_t r = ((v >> 11) & 31) << 3, g = ((v >> 5) & 63) << 2, b = (v & 31) << 3;
  return (54 * r + 183 * g + 19 * b) >> 8;
}

void test_clock_is_legible_on_every_piece() {
  static std::vector<uint8_t> mA(kW * kH), mB(kW * kH);
  static std::vector<uint16_t> out(kW * kH);
  const uint32_t levels[3] = { 500, 7000, 16000 };
  int32_t worst = 255;
  for (int i = 0; i < 365; i += 3) {
    for (uint32_t st : levels) {
      uint16_t day = (uint16_t)(D(2026, 1, 1) + i);
      render(day, bucketForSteps(st));
      Canvas cv; cv.px = gArt.data();
      ArtSpec s; describeDay(day, ALGO_VERSION, s);
      FaceInputs in; in.day = day; in.hour = 10; in.minute = 48; in.steps = st; in.goal = 8000;
      uint8_t tones = chooseTones(cv, s, kModeClock, s.pal.darkBg ? 0 : 3);
      FaceLayer L; L.maskText = mA.data(); L.maskHalo = mB.data();
      buildFaceLayer(s, in, tones, L);
      composeFrame(cv, s, in, L, out.data(), 0, kH);
      int64_t tl = 0, tn = 0, hl = 0, hn = 0;
      for (int y = 40; y < 112; y++) {
        for (int x = 0; x < kW; x++) {
          int k = y * kW + x;
          if (mA[k] > 220) { tl += luma565(out[k]); tn++; }
          else if (mA[k] == 0 && mB[k] > 120) { hl += luma565(out[k]); hn++; }
        }
      }
      TEST_ASSERT_TRUE(tn > 500 && hn > 500);
      int32_t c = (int32_t)(tl / tn) - (int32_t)(hl / hn);
      if (c < 0) c = -c;
      if (c < worst) worst = c;
    }
  }
  printf("  worst clock contrast over the year: %d / 255\n", (int)worst);
  TEST_ASSERT_TRUE(worst >= 140);
}

// Frozen outputs of ALGO_VERSION 1. If any of these change, recorded days
// in people's collections would re-render differently: don't "fix" the
// table — add ALGO_VERSION 2 alongside version 1 instead.
struct Golden { int y, m, d; uint32_t steps; uint32_t hash; };
static const Golden kGolden[] = {
#include "golden.inc"
};

void test_golden_hashes() {
  int n = (int)(sizeof(kGolden) / sizeof(kGolden[0]));
  TEST_ASSERT_TRUE(n >= 14);
  for (int i = 0; i < n; i++) {
    const Golden &g = kGolden[i];
    uint32_t h = render(D(g.y, g.m, g.d), bucketForSteps(g.steps));
    char msg[64];
    snprintf(msg, sizeof(msg), "%04d-%02d-%02d %u steps", g.y, g.m, g.d, (unsigned)g.steps);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(g.hash, h, msg);
  }
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_same_inputs_same_hash);
  RUN_TEST(test_chunking_does_not_change_the_image);
  RUN_TEST(test_growth_is_incremental);
  RUN_TEST(test_every_day_of_the_year_is_different);
  RUN_TEST(test_weekly_family_schedule);
  RUN_TEST(test_palettes_have_value_contrast);
  RUN_TEST(test_special_days);
  RUN_TEST(test_dates_round_trip);
  RUN_TEST(test_growth_curve);
  RUN_TEST(test_face_compositor);
  RUN_TEST(test_clock_is_legible_on_every_piece);
  RUN_TEST(test_golden_hashes);
  return UNITY_END();
}
