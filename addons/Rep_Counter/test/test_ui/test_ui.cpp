// Rep Counter: renderer tests - every screen draws inside the framebuffer,
// deterministically, and the hit areas line up with what's drawn.
#include <unity.h>
#include <cstring>
#include "rep_font.h"
#include "rep_gfx.h"
#include "rep_ui.h"

static const int kGuard = 4096;
static uint16_t mem[kGuard + 240 * 280 + kGuard];
static uint16_t *const fb = mem + kGuard;

static uint32_t frameHash() {
  uint32_t h = 2166136261u;
  for (int i = 0; i < 240 * 280; i++) h = (h ^ fb[i]) * 16777619u;
  return h;
}

static uint32_t draw(const repui::UiState &u) {
  for (int i = 0; i < kGuard; i++) { mem[i] = 0xA5A5; mem[kGuard + 240 * 280 + i] = 0x5A5A; }
  rgfx::Surface s(fb, 240, 280);
  repui::render(s, u);
  for (int i = 0; i < kGuard; i++) {
    TEST_ASSERT_EQUAL_HEX16(0xA5A5, mem[i]);
    TEST_ASSERT_EQUAL_HEX16(0x5A5A, mem[kGuard + 240 * 280 + i]);
  }
  return frameHash();
}

static reps::DayLog gLog;
static reps::History gHist;

static repui::UiState base() {
  repui::UiState u;
  u.clockValid = true; u.hour = 7; u.minute = 5;
  u.log = &gLog; u.hist = &gHist;
  return u;
}

static void test_every_state_renders_in_bounds_and_differs() {
  const reps::Phase phases[] = {reps::Phase::Idle, reps::Phase::Ready, reps::Phase::Lifting,
                                reps::Phase::Resting, reps::Phase::Paused};
  uint32_t seen[16];
  int n = 0;
  for (auto ph : phases) {
    repui::UiState u = base();
    u.phase = ph;
    u.reps = 12; u.exercise = reps::Exercise::Curl; u.confidence = 2;
    u.restSec = 75; u.progress = 0.6f; u.tempoSec = 2.4f;
    seen[n++] = draw(u);
  }
  for (int i = 0; i < n; i++)
    for (int j = i + 1; j < n; j++) TEST_ASSERT_NOT_EQUAL(seen[i], seen[j]);
}

static void test_extremes_stay_in_bounds() {
  // Huge numbers, long rests, full logs, odd themes, toasts and cards at once.
  gLog = reps::DayLog();
  gLog.year = 2026; gLog.month = 2; gLog.day = 29;
  for (int i = 0; i < reps::kMaxSets; i++) {
    gLog.sets[i].reps = 255; gLog.sets[i].tempoCs = 65000; gLog.sets[i].exercise = (uint8_t)(i % 5);
  }
  gLog.count = reps::kMaxSets;
  gHist.n = reps::kHistDays;
  for (int i = 0; i < reps::kHistDays; i++) { gHist.days[i].year = 2026; gHist.days[i].month = 2; gHist.days[i].day = (uint8_t)(20 + i); gHist.days[i].reps = 60000; }
  for (int pg = 0; pg < repui::kPages; pg++) {
    repui::UiState u = base();
    u.page = (repui::Page)pg;
    u.phase = reps::Phase::Resting;
    u.reps = 255; u.restSec = 99 * 3600; u.restGoalSec = 240; u.progress = 9.f; u.pop = 1.f;
    u.summary = true; u.summaryAge = 0.f; u.lastSet.reps = 255; u.lastSet.durationS = 65000;
    u.lastSet.tempoCs = 65000; u.lastSetNo = 999; u.setNo = 65535;
    u.toast = "A very long message that cannot possibly fit";
    u.histScroll = 1000; u.clearHold = 3.f; u.pressedRow = 5;
    u.theme.bg = 0xFFE0; u.theme.fg = 0x001F; u.recording = true;
    draw(u);
    u.phase = reps::Phase::Lifting;
    draw(u);
  }
  gLog = reps::DayLog();
  gHist = reps::History();
}

static void test_rendering_is_deterministic() {
  repui::UiState u = base();
  u.phase = reps::Phase::Lifting; u.reps = 7; u.progress = 0.4f;
  TEST_ASSERT_EQUAL_UINT32(draw(u), draw(u));
}

static void test_hit_targets() {
  repui::UiState u = base();
  TEST_ASSERT_EQUAL_INT((int)repui::Hit::Back, (int)repui::hitTest(u, 20, 20));
  TEST_ASSERT_EQUAL_INT((int)repui::Hit::ModeChip, (int)repui::hitTest(u, 110, 20));
  TEST_ASSERT_EQUAL_INT((int)repui::Hit::Center, (int)repui::hitTest(u, 120, 140));
  TEST_ASSERT_EQUAL_INT((int)repui::Hit::PageDots, (int)repui::hitTest(u, 120, 272));
  u.page = repui::Page::Settings;
  TEST_ASSERT_EQUAL_INT((int)repui::Hit::Row0, (int)repui::hitTest(u, 200, 60));
  TEST_ASSERT_EQUAL_INT((int)repui::Hit::Row5, (int)repui::hitTest(u, 200, 52 + 5 * 33 + 10));
}

static void test_blend_and_fonts() {
  TEST_ASSERT_EQUAL_HEX16(0x1234, rgfx::blend(0xFFFF, 0x1234, 0));
  TEST_ASSERT_EQUAL_HEX16(0xFFFF, rgfx::blend(0xFFFF, 0x1234, 255));
  uint16_t mid = rgfx::blend(0xFFFF, 0x0000, 128);
  TEST_ASSERT_TRUE(rgfx::luma(mid) > 110 && rgfx::luma(mid) < 145);
  // Tabular digits: the counter never shifts sideways as it ticks.
  TEST_ASSERT_EQUAL_INT(rgfx::aaWidth(rgfx::kFontHero, "11"), rgfx::aaWidth(rgfx::kFontHero, "88"));
  const rgfx::AaFont *fonts[] = {&rgfx::kFontHero, &rgfx::kFontTimer, &rgfx::kFontLarge, &rgfx::kFontMedium};
  for (auto f : fonts) {
    for (const char *c = "0123456789"; *c; c++) {
      bool found = false;
      for (int i = 0; i < f->count; i++) if (f->chars[i] == *c) found = true;
      TEST_ASSERT_TRUE(found);
    }
    for (int i = 0; i < f->count; i++) {
      const rgfx::AaGlyph &g = f->glyphs[i];
      TEST_ASSERT_TRUE(g.w > 0 && g.h > 0 && g.h <= f->height + 4);
    }
  }
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_every_state_renders_in_bounds_and_differs);
  RUN_TEST(test_extremes_stay_in_bounds);
  RUN_TEST(test_rendering_is_deterministic);
  RUN_TEST(test_hit_targets);
  RUN_TEST(test_blend_and_fonts);
  return UNITY_END();
}
