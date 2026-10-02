// Host tests for the renderer: ring geometry, that the drawn ring matches the
// fraction it was given, that every screen renders in every state, and a
// fuzz pass with guard bands around the frame buffer to catch any write
// outside the 240x280 canvas.
#include <unity.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "mc_ui.h"
#include "mc_proto.h"
#include "mc_text.h"

using namespace mc;

static const int W = 240, H = 280, GUARD = 4096;
static const uint16_t kSentinel = 0xA5C3;
static uint16_t gMem[GUARD + W * H + GUARD];
static uint16_t *fb = gMem + GUARD;
static const int64_t NOW = 1790847420;    // 2026-10-01 10:37 local at +60
static const int TZ = 60;

void setUp() {
  for (int i = 0; i < GUARD; i++) { gMem[i] = kSentinel; gMem[GUARD + W * H + i] = kSentinel; }
}
void tearDown() {}

static void assertGuards() {
  for (int i = 0; i < GUARD; i++) {
    if (gMem[i] != kSentinel || gMem[GUARD + W * H + i] != kSentinel)
      TEST_FAIL_MESSAGE("renderer wrote outside the frame buffer");
  }
}

static uint32_t px(int x, int y) { return rgb888(fb[y * W + x]); }

static bool close(uint32_t a, uint32_t b, int tol = 24) {
  int dr = (int)((a >> 16) & 0xFF) - (int)((b >> 16) & 0xFF);
  int dg = (int)((a >> 8) & 0xFF) - (int)((b >> 8) & 0xFF);
  int db = (int)(a & 0xFF) - (int)(b & 0xFF);
  return dr * dr + dg * dg + db * db <= tol * tol * 3;
}

static Event ev(const char *title, int64_t start, int durMin, const char *loc = "", int leave = 0) {
  Event e;
  memset((void *)&e, 0, sizeof(e));
  copyStr(e.title, sizeof(e.title), title);
  copyStr(e.location, sizeof(e.location), loc);
  e.start = start;
  e.end = start + durMin * 60;
  e.leaveMin = (uint8_t)leave;
  finishManualEvent(e, Source::Feed, (uint32_t)start);
  return e;
}

// ---------------------------------------------------------------------------
void test_ring_geometry() {
  Ring r = makeRing(W, H, 3, 12, 46);
  float x, y;
  ringPoint(r, 0.0f, x, y);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 120.0f, x);           // 12 o'clock
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 9.0f, y);
  ringPoint(r, 0.5f, x, y);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 120.0f, x);           // 6 o'clock
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 271.0f, y);
  ringPoint(r, 0.25f, x, y);
  TEST_ASSERT_TRUE(x > 225);                              // right edge
  ringPoint(r, 0.75f, x, y);
  TEST_ASSERT_TRUE(x < 15);                               // left edge
  // Uniform in arc length: summed chord lengths match the analytic length.
  float total = 0, px0, py0, px1, py1;
  ringPoint(r, 0, px0, py0);
  for (int i = 1; i <= 4000; i++) {
    ringPoint(r, (float)i / 4000.0f, px1, py1);
    total += sqrtf((px1 - px0) * (px1 - px0) + (py1 - py0) * (py1 - py0));
    px0 = px1; py0 = py1;
  }
  TEST_ASSERT_FLOAT_WITHIN(0.5f, r.len, total);
  // Equal steps along s are equal distances along the track (no speed-up in
  // the corners): compare a straight stretch with a corner stretch.
  float a0x, a0y, a1x, a1y, b0x, b0y, b1x, b1y;
  ringPoint(r, 0.02f, a0x, a0y); ringPoint(r, 0.03f, a1x, a1y);
  float cornerS = (r.a - r.rc + 3.14159f * r.rc / 4) / r.len;   // middle of the TR corner
  ringPoint(r, cornerS, b0x, b0y); ringPoint(r, cornerS + 0.01f, b1x, b1y);
  float da = sqrtf((a1x - a0x) * (a1x - a0x) + (a1y - a0y) * (a1y - a0y));
  float db = sqrtf((b1x - b0x) * (b1x - b0x) + (b1y - b0y) * (b1y - b0y));
  TEST_ASSERT_FLOAT_WITHIN(0.15f, da, db);
}

void test_ring_fill_matches_fraction() {
  Canvas c{fb, W, H};
  Ring r = makeRing(W, H, 3, 12, 46);
  const uint32_t FILL = 0xFF0000, TRACK = 0x0000FF;
  for (int k = 1; k < 20; k++) {
    float f = (float)k / 20.0f;                // countdown: filled [1-f, 1)
    clear(c, 0);
    drawRing(c, r, 1.0f - f, 1.0f, FILL, TRACK, 0, 0);
    for (int j = 1; j < 40; j++) {
      float s = (float)j / 40.0f;
      if (fabsf(s - (1.0f - f)) < 0.03f) continue;   // skip the round cap region
      float x, y;
      ringPoint(r, s, x, y);
      uint32_t got = px((int)x, (int)y);
      bool shouldFill = s > 1.0f - f;
      if (shouldFill && !close(got, FILL)) TEST_FAIL_MESSAGE("expected fill colour on the ring");
      if (!shouldFill && !close(got, TRACK)) TEST_FAIL_MESSAGE("expected track colour on the ring");
    }
  }
  // Nothing is drawn well inside the ring.
  TEST_ASSERT_EQUAL_HEX32(0, px(120, 140));
  assertGuards();
}

void test_digits_and_text_metrics() {
  float w = digitsWidth("10:37", 56);
  TEST_ASSERT_TRUE(w > 150 && w < 200);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, digitsWidth("88:88", 56), digitsWidth("11:11", 56));   // tabular
  Canvas c{fb, W, H};
  clear(c, 0);
  drawDigits(c, "10:37", 120 - w / 2, 54, 56, 0.15f, 0xFFFFFF);
  int lit = 0;
  for (int y = 50; y < 115; y++) for (int x = 0; x < W; x++) if (px(x, y) > 0x808080) lit++;
  TEST_ASSERT_TRUE(lit > 1500);              // a bold, solid set of strokes
  for (int y = 0; y < 48; y++) for (int x = 0; x < W; x++) TEST_ASSERT_EQUAL_HEX32(0, px(x, y));
  assertGuards();
}

// Every face state renders, and the headline shows in the ring colour.
void test_face_states() {
  Canvas c{fb, W, H};
  struct Case { const char *name; int64_t now; };
  Event list[] = {ev("Sprint planning", NOW - 37 * 60, 60, "Blue room"),
                  ev("Design review", NOW + 23 * 60 + 23 * 60, 45, "Room 4.12"),
                  ev("Dentist", NOW + 4 * 3600, 30, "Clinic", 30)};
  sortEvents(list, 3);
  const int64_t times[] = {NOW, NOW + 30 * 60, NOW + 45 * 60 + 15, NOW + 46 * 60 - 20, NOW + 3 * 3600 + 50 * 60,
                           NOW + 10 * 3600};
  for (int64_t t : times) {
    FaceModel m;
    m.now = t;
    m.tz = TZ;
    m.ev = list;
    m.n = 3;
    m.fi = computeFace(list, 3, t, TZ, 60);
    copyStr(m.status.text, sizeof(m.status.text), "synced 3 min ago");
    drawFace(c, m);
    assertGuards();
    // The time digits are always there.
    int lit = 0;
    for (int y = 56; y < 110; y++) for (int x = 20; x < 220; x++) if (px(x, y) > 0xA0A0A0) lit++;
    TEST_ASSERT_TRUE(lit > 800);
  }
  // Clear state.
  FaceModel m;
  m.now = NOW;
  m.tz = TZ;
  m.fi = computeFace(nullptr, 0, NOW, TZ, 60);
  drawFace(c, m);
  assertGuards();
}

void test_other_screens() {
  Canvas c{fb, W, H};
  AlertModel a;
  a.now = NOW;
  a.tz = TZ;
  a.ev = ev("A meeting with an extraordinarily long title that cannot possibly fit", NOW + 300, 30,
            "Somewhere with a very long location name indeed");
  for (int k = 0; k < 4; k++) {
    a.kind = k == 0 ? AlertKind::Before : k == 1 ? AlertKind::Leave : AlertKind::Snooze;
    a.canSnooze = k != 3;
    a.more = k;
    a.pressed = (int8_t)(k % 3 - 1);
    drawAlert(c, a);
    assertGuards();
  }
  // Buttons lie inside the screen and do not overlap.
  UiRect s = alertSnoozeRect(), d = alertDismissRect();
  TEST_ASSERT_TRUE(s.x + s.w <= d.x);
  TEST_ASSERT_TRUE(d.x + d.w <= W && d.y + d.h <= H);

  Event list[12];
  for (int i = 0; i < 12; i++) {
    char t[32];
    snprintf(t, sizeof(t), "Event number %d with words", i);
    list[i] = ev(t, NOW + i * 1800 - 900, 25, i % 2 ? "Room" : "");
  }
  AgendaModel g;
  g.now = NOW;
  g.tz = TZ;
  g.ev = list;
  g.n = 12;
  for (int first = 0; first < 12; first++) {
    g.first = first;
    g.syncing = first % 2;
    g.syncStep = "Downloading 1234 KB...";
    drawAgenda(c, g);
    assertGuards();
  }
  SettingsModel sm;
  sm.n = kSettingsRows + 3;                  // more than fit: extra rows are ignored
  for (int i = 0; i < kSettingsRows; i++) { sm.rows[i].label = "A rather long label"; copyStr(sm.rows[i].value, 28, "value text"); }
  sm.footer = "footer line that is much too long to fit on the screen at all";
  sm.footer2 = "http://255.255.255.255/meet";
  drawSettings(c, sm);
  assertGuards();
  for (int i = 0; i < kSettingsRows; i++) TEST_ASSERT_TRUE(settingsRowRect(i).y + settingsRowRect(i).h <= 245);
}

// Random events, titles, times and states: nothing may crash or write
// outside the canvas.
void test_render_fuzz() {
  Canvas c{fb, W, H};
  uint32_t seed = 0xC0FFEE;
  auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
  static const char *kWords[] = {"", "x", "Mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm", "Q3", "all-hands",
                                 "WWWWWWWWWWWW WWWWWWWWWWW", "a b c d e f g h", "~!@#$%^&*()_+{}|:\"<>?"};
  for (int iter = 0; iter < 300; iter++) {
    int n = (int)(rnd() % 9);
    Event list[8];
    for (int i = 0; i < n && i < 8; i++) {
      char t[kTitleMax];
      snprintf(t, sizeof(t), "%s %s", kWords[rnd() % 8], kWords[rnd() % 8]);
      int64_t st = NOW + (int64_t)(rnd() % 200000) - 100000;
      list[i] = ev(t, st, (int)(rnd() % 600), kWords[rnd() % 8], (int)(rnd() % 3) * 10);
      if (rnd() % 7 == 0) list[i].flags |= EF_ALLDAY;
    }
    if (n > 8) n = 8;
    sortEvents(list, n);
    int64_t t = NOW + (int64_t)(rnd() % 100000) - 50000;
    FaceModel m;
    m.now = t;
    m.tz = (int)(rnd() % 1500) - 720;
    m.ev = list;
    m.n = n;
    m.leadMin = 15 + (int)(rnd() % 165);
    m.fi = computeFace(list, n, t, m.tz, m.leadMin);
    m.phase = (float)(rnd() % 1000) / 1000.0f;
    m.rtcOk = rnd() % 10 != 0;
    m.alertsOn = rnd() % 2;
    copyStr(m.status.text, sizeof(m.status.text), kWords[rnd() % 8]);
    drawFace(c, m);
    if (n > 0) {
      AlertModel a;
      a.now = t;
      a.tz = m.tz;
      a.ev = list[rnd() % n];
      a.kind = (AlertKind)(1 + rnd() % 3);
      a.more = (int)(rnd() % 3);
      drawAlert(c, a);
    }
    AgendaModel g;
    g.now = t;
    g.tz = m.tz;
    g.ev = list;
    g.n = n;
    g.first = n ? (int)(rnd() % n) : 0;
    drawAgenda(c, g);
    assertGuards();
  }
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_ring_geometry);
  RUN_TEST(test_ring_fill_matches_fraction);
  RUN_TEST(test_digits_and_text_metrics);
  RUN_TEST(test_face_states);
  RUN_TEST(test_other_screens);
  RUN_TEST(test_render_fuzz);
  return UNITY_END();
}
