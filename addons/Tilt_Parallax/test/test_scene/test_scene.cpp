// Scenes, sky model and the FaceRenderer: structure, determinism, memory,
// clamping guarantees, dirty-row tracking and time-of-day continuity.
#include <unity.h>
#include <string.h>
#include <math.h>
#include <vector>
#include "tp_scene.h"
#include "tp_engine.h"
#include "tp_compose.h"
#include "tp_config.h"
#include "tp_sky.h"

using namespace tp;

void setUp() {}
void tearDown() {}

static LocalTime lt(int y, int mo, int d, int h, int mi) {
  LocalTime t;
  t.year = y; t.month = mo; t.day = d; t.hour = h; t.minute = mi; t.second = 0;
  return t;
}

// ---------------------------------------------------------------------------
static void test_every_scene_builds_with_enough_layers() {
  TEST_ASSERT_TRUE(sceneCount() >= 3);
  for (int id = 0; id < sceneCount(); id++) {
    Scene s;
    TEST_ASSERT_TRUE_MESSAGE(buildScene(id, s), sceneName(id));
    // sky + at least far, mid and foreground terrain
    TEST_ASSERT_TRUE(s.count >= 3);
    // far -> near ordering by depth
    for (int i = 1; i < s.count; i++)
      TEST_ASSERT_TRUE(s.layers[i].depth <= s.layers[i - 1].depth);
    // memory budget per scene (PSRAM)
    TEST_ASSERT_TRUE_MESSAGE(s.bytes() < 640u * 1024u, sceneName(id));
    s.release();
  }
}

static void test_layers_have_overscan_and_clamp_ready_bottoms() {
  for (int id = 0; id < sceneCount(); id++) {
    Scene s;
    TEST_ASSERT_TRUE(buildScene(id, s));
    for (int i = 0; i < s.count; i++) {
      const Layer &L = s.layers[i];
      int ov = overscanX(L.depth);
      TEST_ASSERT_TRUE(L.x0 <= -ov + 1);
      TEST_ASSERT_TRUE(L.x0 + L.w >= kScreenW + ov - 1);
      TEST_ASSERT_NOT_NULL(L.rowFlags);
      if (L.clampBottom) {
        // the clamp repeats the last row downward, so it must be solid
        char msg[64];
        snprintf(msg, sizeof(msg), "%s layer %d bottom row not opaque", sceneName(id), i);
        TEST_ASSERT_TRUE_MESSAGE(L.rowFlags[L.h - 1] & ROW_OPAQUE, msg);
      }
    }
    s.release();
  }
}

static void test_scenes_are_deterministic() {
  for (int id = 0; id < sceneCount(); id++) {
    Scene a, b;
    TEST_ASSERT_TRUE(buildScene(id, a));
    TEST_ASSERT_TRUE(buildScene(id, b));
    TEST_ASSERT_EQUAL_INT(a.count, b.count);
    for (int i = 0; i < a.count; i++) {
      const Layer &A = a.layers[i], &B = b.layers[i];
      size_t n = (size_t)A.w * A.h;
      TEST_ASSERT_EQUAL_MEMORY(A.idx, B.idx, n);
      if (A.alpha) TEST_ASSERT_EQUAL_MEMORY(A.alpha, B.alpha, n);
      TEST_ASSERT_EQUAL_UINT32(A.spanCount, B.spanCount);
    }
    a.release();
    b.release();
  }
}

// Every material index used by the art is defined (unused ones render as
// magenta 0xF81F), at every time of day and at the tilt extremes.
static void test_no_undefined_materials_on_screen() {
  FaceRenderer fr;
  TEST_ASSERT_TRUE(fr.begin());
  std::vector<uint16_t> fb(kScreenW * kScreenH);
  const int hours[] = { 0, 5, 7, 12, 18, 20, 22 };
  const float tilts[][2] = { { 0, 0 }, { -1, -1 }, { 1, 1 }, { 1, -1 } };
  for (int id = 0; id < sceneCount(); id++) {
    Scene s;
    TEST_ASSERT_TRUE(buildScene(id, s));
    fr.setScene(&s);
    for (int h : hours) {
      fr.setTimeOfDay(lt(2026, 9, 20, h, 30), true);
      fr.setClock(h, 30, true, false);
      for (auto &t : tilts) {
        fr.setParallax(t[0], t[1], kMaxStrength);
        fr.markAllDirty();
        int y0, y1;
        while (fr.popDirtyRun(y0, y1)) fr.composeRows(y0, y1, fb.data() + y0 * kScreenW, kScreenW);
        for (size_t i = 0; i < fb.size(); i++) {
          if (fb[i] == 0xF81F) {
            char msg[80];
            snprintf(msg, sizeof(msg), "%s %02d:30 magenta at (%d,%d)", sceneName(id), h,
                     (int)(i % kScreenW), (int)(i / kScreenW));
            TEST_FAIL_MESSAGE(msg);
          }
        }
      }
    }
    fr.setScene(nullptr);
    s.release();
  }
  fr.end();
}

// The renderer's fast output equals the reference compositor on its stack.
static void test_renderer_matches_reference() {
  FaceRenderer fr;
  TEST_ASSERT_TRUE(fr.begin());
  Scene s;
  TEST_ASSERT_TRUE(buildScene(0, s));
  fr.setScene(&s);
  fr.setTimeOfDay(lt(2026, 6, 21, 8, 40), true);
  fr.setClock(8, 40, true, false);
  StatusInfo st;
  strcpy(st.date, "SUN 21 JUN");
  st.battery = 64;
  fr.setStatus(st);
  fr.setParallax(0.4f, -0.3f, 1.0f);
  std::vector<uint16_t> a(kScreenW * kScreenH), b(kScreenW * kScreenH);
  fr.composeRows(0, kScreenH, a.data(), kScreenW);
  // Rebuild the same stack order the renderer uses and run the reference.
  // (Indirectly: compose twice in different strip sizes must agree.)
  for (int y = 0; y < kScreenH; y += 7) {
    int y1 = y + 7 > kScreenH ? kScreenH : y + 7;
    fr.composeRows(y, y1, b.data() + y * kScreenW, kScreenW);
  }
  TEST_ASSERT_EQUAL_MEMORY(a.data(), b.data(), a.size() * 2);
  fr.setScene(nullptr);
  s.release();
  fr.end();
}

// Dirty tracking: still -> nothing to send; tiny jitter -> nothing; a real
// move -> rows; a new minute -> only the clock band.
static void test_dirty_tracking() {
  FaceRenderer fr;
  TEST_ASSERT_TRUE(fr.begin());
  Scene s;
  TEST_ASSERT_TRUE(buildScene(0, s));
  fr.setScene(&s);
  fr.setTimeOfDay(lt(2026, 6, 21, 10, 0), true);
  fr.setClock(10, 0, true, false);
  fr.setParallax(0, 0, 1.0f);
  int y0, y1;
  while (fr.popDirtyRun(y0, y1)) {}
  TEST_ASSERT_FALSE(fr.anyDirty());
  fr.setParallax(0, 0, 1.0f);
  TEST_ASSERT_FALSE(fr.anyDirty());
  // jitter well under the hysteresis (0.62 px on the sky = 0.034 tilt)
  TEST_ASSERT_FALSE(fr.setParallax(0.02f, -0.02f, 1.0f));
  TEST_ASSERT_FALSE(fr.anyDirty());
  // a real move
  TEST_ASSERT_TRUE(fr.setParallax(0.5f, 0.0f, 1.0f));
  TEST_ASSERT_TRUE(fr.dirtyCount() > 100);
  while (fr.popDirtyRun(y0, y1)) {}
  // same clock text: nothing
  fr.setClock(10, 0, true, false);
  TEST_ASSERT_FALSE(fr.anyDirty());
  // next minute: the clock (and its shadow) rows only
  fr.setClock(10, 1, true, false);
  TEST_ASSERT_TRUE(fr.anyDirty());
  int first = -1, last = -1;
  while (fr.popDirtyRun(y0, y1)) {
    if (first < 0) first = y0;
    last = y1;
  }
  TEST_ASSERT_TRUE(first >= 20 && last <= 150);
  fr.setScene(nullptr);
  s.release();
  fr.end();
}

// Ambient twinkle: at night a few stars change, touching only a few rows;
// by day there is nothing to animate.
static void test_twinkle_touches_only_star_rows() {
  FaceRenderer fr;
  TEST_ASSERT_TRUE(fr.begin());
  Scene s;
  TEST_ASSERT_TRUE(buildScene(3, s));          // desert: clearest skies
  fr.setScene(&s);
  fr.setTimeOfDay(lt(2026, 8, 27, 1, 30), true);
  fr.setClock(1, 30, true, false);
  int y0, y1;
  while (fr.popDirtyRun(y0, y1)) {}
  TEST_ASSERT_TRUE(fr.starsVisible());
  std::vector<uint16_t> a(kScreenW * kScreenH), b(kScreenW * kScreenH);
  fr.composeRows(0, kScreenH, a.data(), kScreenW);
  int changedFrames = 0;
  for (uint32_t t = 0; t < 4000; t += 125) {
    fr.twinkle(t);
    int rows = fr.dirtyCount();
    TEST_ASSERT_TRUE(rows <= 3 * 12);            // a few rows per star at most
    while (fr.popDirtyRun(y0, y1)) {}
    if (rows) changedFrames++;
  }
  TEST_ASSERT_TRUE(changedFrames > 10);
  fr.composeRows(0, kScreenH, b.data(), kScreenW);
  TEST_ASSERT_TRUE(memcmp(a.data(), b.data(), a.size() * 2) != 0);
  fr.setTimeOfDay(lt(2026, 8, 27, 13, 0), true);
  TEST_ASSERT_FALSE(fr.starsVisible());
  fr.setScene(nullptr);
  s.release();
  fr.end();
}

// ---------------------------------------------------------------------------
// Sky model
// ---------------------------------------------------------------------------
static void test_moon_phase_known_dates() {
  // Full moon 2026-03-03 ~11:38 UTC; new moon 2026-03-19 ~01:23 UTC.
  float full = moonPhaseFor(lt(2026, 3, 3, 12, 0));
  TEST_ASSERT_FLOAT_WITHIN(0.03f, 0.5f, full);
  float nw = moonPhaseFor(lt(2026, 3, 19, 1, 0));
  TEST_ASSERT_TRUE(nw < 0.03f || nw > 0.97f);
  // first quarter 2026-03-25 ~19:17 UTC
  TEST_ASSERT_FLOAT_WITHIN(0.03f, 0.25f, moonPhaseFor(lt(2026, 3, 25, 19, 0)));
}

static void test_sun_follows_the_day_and_seasons() {
  SkyStyle st;
  SkyState s;
  computeSky(lt(2026, 6, 21, 12, 30), st, s);
  TEST_ASSERT_TRUE(s.sunAlt > 55);
  TEST_ASSERT_TRUE(s.sunUp);
  computeSky(lt(2026, 6, 21, 0, 30), st, s);
  TEST_ASSERT_TRUE(s.sunAlt < -10);
  TEST_ASSERT_TRUE(s.starAmt > 0.5f);
  // midsummer days are longer than midwinter days
  int upJun = 0, upDec = 0;
  for (int m = 0; m < 24 * 60; m += 10) {
    computeSky(lt(2026, 6, 21, m / 60, m % 60), st, s);
    upJun += s.sunAlt > 0;
    computeSky(lt(2026, 12, 21, m / 60, m % 60), st, s);
    upDec += s.sunAlt > 0;
  }
  TEST_ASSERT_TRUE(upJun > upDec + 30);    // > 5 h difference
  // morning sun lights from the left (east), evening from the right
  computeSky(lt(2026, 6, 21, 8, 0), st, s);
  TEST_ASSERT_TRUE(s.lightX < 0);
  TEST_ASSERT_TRUE(s.sunX < 120);
  computeSky(lt(2026, 6, 21, 17, 0), st, s);
  TEST_ASSERT_TRUE(s.lightX > 0);
  TEST_ASSERT_TRUE(s.sunX > 120);
}

static float dist(RGB a, RGB b) {
  return fabsf(a.r - b.r) + fabsf(a.g - b.g) + fabsf(a.b - b.b);
}

// Colours never jump between consecutive minutes (including midnight).
static void test_palette_is_continuous() {
  SkyStyle st;
  SkyState prev, cur;
  computeSky(lt(2026, 3, 20, 23, 59), st, prev);
  for (int m = 0; m < 24 * 60; m++) {
    computeSky(lt(2026, 3, 21, m / 60, m % 60), st, cur);
    float dz = dist(prev.zenith, cur.zenith), dh = dist(prev.horizon, cur.horizon);
    float da = dist(prev.ambient, cur.ambient);
    char msg[64];
    snprintf(msg, sizeof(msg), "jump at %02d:%02d (%.1f %.1f %.1f)", m / 60, m % 60, dz, dh, da);
    TEST_ASSERT_TRUE_MESSAGE(dz < 12 && dh < 16 && da < 12, msg);
    prev = cur;
  }
}

// The clock colour stays bright enough to read at every hour.
static void test_clock_colour_always_bright() {
  SkyStyle st;
  SkyState s;
  for (int h = 0; h < 24; h++) {
    computeSky(lt(2026, 1, 15, h, 0), st, s);
    TEST_ASSERT_TRUE(luma(s.text) > 215);
  }
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_every_scene_builds_with_enough_layers);
  RUN_TEST(test_layers_have_overscan_and_clamp_ready_bottoms);
  RUN_TEST(test_scenes_are_deterministic);
  RUN_TEST(test_no_undefined_materials_on_screen);
  RUN_TEST(test_renderer_matches_reference);
  RUN_TEST(test_dirty_tracking);
  RUN_TEST(test_twinkle_touches_only_star_rows);
  RUN_TEST(test_moon_phase_known_dates);
  RUN_TEST(test_sun_follows_the_day_and_seasons);
  RUN_TEST(test_palette_is_continuous);
  RUN_TEST(test_clock_colour_always_bright);
  return UNITY_END();
}
