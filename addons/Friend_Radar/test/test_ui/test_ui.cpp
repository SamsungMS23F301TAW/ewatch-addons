// Host tests: pure UI helpers the watch relies on for every tap and frame
// (hit tests, keyboard mapping, list rows, text fitting, date formatting,
// blip animation and renderer determinism).
#include <unity.h>
#include <initializer_list>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "fr_anim.h"
#include "fr_motion.h"
#include "fr_radar.h"
#include "fr_screens.h"

using namespace fr;

void setUp() {}
void tearDown() {}

static uint16_t gFrameA[dial::kBgPixels], gFrameB[dial::kBgPixels];
static uint16_t gBg[dial::kBgPixels], gPolar[dial::kPolarEntries], gOverlay[dial::kPolarEntries];
static uint16_t gClean[dial::kPolarEntries];

static void test_text_metrics_and_fit() {
  TEST_ASSERT_EQUAL(0, textWidth(kFontM, ""));
  int w1 = textWidth(kFontM, "Kyle");
  TEST_ASSERT_TRUE(w1 > 20 && w1 < 60);
  TEST_ASSERT_TRUE(textWidth(kFontL, "Kyle") > w1);
  char out[32];
  fitText(kFontM, "Kyle", 200, out, sizeof out);
  TEST_ASSERT_EQUAL_STRING("Kyle", out);
  fitText(kFontM, "A very long display name", 60, out, sizeof out);
  TEST_ASSERT_TRUE(textWidth(kFontM, out) <= 60);
  TEST_ASSERT_EQUAL_HEX8(0x80, (uint8_t)out[strlen(out) - 1]);     // ends with an ellipsis
  // Unknown bytes render as '?' instead of reading out of bounds.
  TEST_ASSERT_EQUAL(textWidth(kFontS, "?"), textWidth(kFontS, "\xF0"));
}

static void test_word_wrap() {
  const char *s = "Bluetooth can't sense direction, so each friend keeps their own spot.";
  int lines = wrappedLineCount(kFontS, 120, s);
  TEST_ASSERT_TRUE(lines >= 3 && lines <= 6);
  TEST_ASSERT_EQUAL(1, wrappedLineCount(kFontS, 1000, s));
  TEST_ASSERT_EQUAL(0, wrappedLineCount(kFontS, 100, ""));
  TEST_ASSERT_EQUAL(2, wrappedLineCount(kFontS, 400, "one\ntwo"));
}

static void test_format_when_and_minutes() {
  // 2025-06-02 is a Monday. Seconds since 2000-01-01 (local).
  const uint32_t day0 = 9284;                  // days from 2000-01-01 to 2025-06-02
  const uint32_t now = day0 * 86400u + 15 * 3600u;      // Mon 15:00
  char b[24];
  formatWhen(now - 3600, now, b, sizeof b);    TEST_ASSERT_EQUAL_STRING("Today 14:00", b);
  formatWhen(now - 86400, now, b, sizeof b);   TEST_ASSERT_EQUAL_STRING("Yesterday 15:00", b);
  formatWhen(now - 3 * 86400, now, b, sizeof b); TEST_ASSERT_EQUAL_STRING("Fri 15:00", b);
  formatWhen(now - 30 * 86400, now, b, sizeof b); TEST_ASSERT_EQUAL_STRING("3 May 15:00", b);
  formatWhen(0, 400 * 86400u, b, sizeof b);    TEST_ASSERT_EQUAL_STRING("1 Jan 00:00", b);
  formatMinutes(0, b, sizeof b);   TEST_ASSERT_EQUAL_STRING("<1 min", b);
  formatMinutes(25, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("25 min", b);
  formatMinutes(60, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("1 h", b);
  formatMinutes(70, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("1 h 10 min", b);
  formatAgo(3, b, sizeof b);       TEST_ASSERT_EQUAL_STRING("just now", b);
  formatAgo(42, b, sizeof b);      TEST_ASSERT_EQUAL_STRING("42 s ago", b);
  formatAgo(7200, b, sizeof b);    TEST_ASSERT_EQUAL_STRING("2 h ago", b);
  formatAgo(90000, b, sizeof b);   TEST_ASSERT_EQUAL_STRING("yesterday", b);
}

static void test_list_rows_and_scroll() {
  ListView v;
  TEST_ASSERT_EQUAL(-1, listRowAt(layout::listTop - 1, v, 5));
  TEST_ASSERT_EQUAL(0, listRowAt(layout::listTop + 1, v, 5));
  TEST_ASSERT_EQUAL(2, listRowAt(layout::listTop + 2 * layout::rowH + 5, v, 5));
  TEST_ASSERT_EQUAL(-1, listRowAt(layout::listTop + 5 * layout::rowH + 5, v, 5));
  v.scroll = layout::rowH;                       // scrolled by one row
  TEST_ASSERT_EQUAL(1, listRowAt(layout::listTop + 1, v, 5));
  TEST_ASSERT_EQUAL(0, listMaxScroll(3));        // fits: no scrolling
  TEST_ASSERT_TRUE(listMaxScroll(8) > 0);
  v.scroll = 9999;
  listClampScroll(v, 8);
  TEST_ASSERT_EQUAL(listMaxScroll(8), v.scroll);
  v.scroll = -50;
  listClampScroll(v, 8);
  TEST_ASSERT_EQUAL(0, v.scroll);
}

static void test_keyboard_mapping() {
  KeyboardView k;
  k.shift = true;
  // Six 40 px columns; rows of 44 px from y = 58.
  TEST_ASSERT_EQUAL('A', keyboardHit(k, 20, 80));
  TEST_ASSERT_EQUAL('F', keyboardHit(k, 220, 80));
  k.shift = false;
  TEST_ASSERT_EQUAL('g', keyboardHit(k, 20, 124));
  TEST_ASSERT_EQUAL('x', keyboardHit(k, 220, 212));
  TEST_ASSERT_EQUAL(kKeyShift, keyboardHit(k, 20, 256));
  TEST_ASSERT_EQUAL('y', keyboardHit(k, 60, 256));
  TEST_ASSERT_EQUAL('z', keyboardHit(k, 100, 256));
  TEST_ASSERT_EQUAL(kKeySpace, keyboardHit(k, 140, 256));
  TEST_ASSERT_EQUAL(kKeyMode, keyboardHit(k, 180, 256));
  TEST_ASSERT_EQUAL(kKeyBackspace, keyboardHit(k, 220, 256));
  k.symbols = true;
  TEST_ASSERT_EQUAL('1', keyboardHit(k, 20, 80));
  TEST_ASSERT_EQUAL('\'', keyboardHit(k, 20, 168));
  TEST_ASSERT_EQUAL('-', keyboardHit(k, 60, 168));
  TEST_ASSERT_EQUAL('/', keyboardHit(k, 20, 256));
  TEST_ASSERT_EQUAL(kKeySpace, keyboardHit(k, 140, 256));
  TEST_ASSERT_EQUAL(kKeyDone, keyboardHit(k, 215, 20));
  TEST_ASSERT_EQUAL(kKeyNone, keyboardHit(k, 120, 30));      // the text field
  // Every key owns a whole 40 x 44 cell: all four corners map to the same code.
  KeyboardView l;
  l.shift = false;
  for (int row = 0; row < 5; ++row)
    for (int col = 0; col < 6; ++col) {
      int x0 = col * 40 + 1, x1 = col * 40 + 38, y0 = 58 + row * 44 + 1, y1 = 58 + row * 44 + 42;
      int code = keyboardHit(l, (x0 + x1) / 2, (y0 + y1) / 2);
      TEST_ASSERT_TRUE(code != kKeyNone);
      TEST_ASSERT_EQUAL(code, keyboardHit(l, x0, y0));
      TEST_ASSERT_EQUAL(code, keyboardHit(l, x1, y0));
      TEST_ASSERT_EQUAL(code, keyboardHit(l, x0, y1));
      TEST_ASSERT_EQUAL(code, keyboardHit(l, x1, y1));
      Rect r = keyboardKeyRect(row, col);             // the drawn cap sits inside its cell
      TEST_ASSERT_TRUE(r.x >= col * 40 && r.x + r.w <= col * 40 + 40);
      TEST_ASSERT_TRUE(r.y >= 58 + row * 44 && r.y + r.h <= 58 + row * 44 + 44);
    }
}

static void test_touch_targets_are_at_least_40px() {
  const Rect rects[] = {layout::backHit, layout::menuHit, layout::cardBtnL, layout::cardBtnR,
                        layout::cardBtnWide, mateDetailRenameBtn(), mateDetailRemoveBtn(),
                        calibrateSecondaryBtn(), keyboardDoneBtn()};
  for (const Rect &r : rects) {
    TEST_ASSERT_TRUE(r.w >= 40);
    TEST_ASSERT_TRUE(r.h >= 40);
    TEST_ASSERT_TRUE(r.x >= 0 && r.y >= 0 && r.x + r.w <= layout::W && r.y + r.h <= layout::H);
  }
  CalibrateView v;
  for (int ph = 0; ph < 4; ++ph) {
    v.phase = (CalibrateView::Phase)ph;
    const Rect &p = calibratePrimaryBtn(v);
    TEST_ASSERT_TRUE(p.w >= 40 && p.h >= 40);
  }
  TEST_ASSERT_TRUE(layout::rowH >= 40);
  // Card buttons and detail buttons never overlap each other.
  TEST_ASSERT_TRUE(layout::cardBtnL.x + layout::cardBtnL.w <= layout::cardBtnR.x);
  TEST_ASSERT_TRUE(mateDetailRenameBtn().x + mateDetailRenameBtn().w <= mateDetailRemoveBtn().x);
}

static void test_zone_labels_never_collide_with_blips() {
  // The zone names are engraved along the bottom arc of the glass; blips keep
  // out of that sector for every id and every radius in their band. Each
  // label occupies an annular sector (its cap height inward of the baseline);
  // no point of a mate's orb (plus a margin) may fall inside one.
  const char *names[3] = {"NEAR", "AROUND", "FAR"};
  const float labelR[3] = {layout::ringR[1] - 3.5f, layout::ringR[2] - 3.5f, (float)layout::Rg - 6.f};
  const float discR = 8.5f + 2.f;
  float half[3];
  for (int i = 0; i < 3; ++i) half[i] = textArcHalfSpanDeg(kFontXS, labelR[i], names[i], 2.2f) + 1.f;
  for (uint32_t id = 1; id < 20000; id += 37) {
    float a = (float)blipAngleDeg(id) * 0.0174533f;
    for (int z = 0; z < 4; ++z) {
      for (float rho : {layout::bandLo[z], layout::bandHi[z]}) {
        float bx = rho * sinf(a), by = -rho * cosf(a);          // centre-relative, y down
        for (int k = 0; k < 48; ++k) {
          float t = (float)k * 6.2831853f / 48.f;
          float px = bx + discR * cosf(t), py = by + discR * sinf(t);
          float r = sqrtf(px * px + py * py);
          float deg = atan2f(px, -py) * 57.2958f;               // 0 = up, clockwise
          if (deg < 0.f) deg += 360.f;
          float off = fabsf(deg - 180.f);
          for (int i = 0; i < 3; ++i) {
            bool inside = r > labelR[i] - 11.f && r < labelR[i] + 2.f && off < half[i];
            TEST_ASSERT_FALSE(inside);
          }
        }
      }
    }
  }
}

static void test_blip_animator_and_hit() {
  PeerView p[2];
  p[0].id = 0xAAAA0001; strcpy(p[0].label, "Ewan"); p[0].mate = true;
  p[0].zone = Zone::Near; p[0].bandPos = 0.5f;
  p[1].id = 0xAAAA0002; strcpy(p[1].label, "Jo");
  p[1].zone = Zone::Far; p[1].bandPos = 0.2f;
  BlipAnimator a;
  a.update(p, 2, 0);
  for (uint32_t t = 0; t <= 1500; t += 33) a.step(t);
  Blip b[kMaxPeers];
  int n = a.blips(b, kMaxPeers, 0xAAAA0001);
  TEST_ASSERT_EQUAL(2, n);
  const Blip e = b[0].id == 0xAAAA0001 ? b[0] : b[1];     // copy: b is reused below
  TEST_ASSERT_TRUE(e.selected);
  TEST_ASSERT_EQUAL_UINT8(255, e.alpha);
  TEST_ASSERT_FLOAT_WITHIN(0.6f, BlipAnimator::targetRadius(Zone::Near, 0.5f), e.radius);
  TEST_ASSERT_TRUE(e.radius > layout::ringR[0] && e.radius < layout::ringR[1]);
  TEST_ASSERT_EQUAL_FLOAT((float)blipAngleDeg(0xAAAA0001), e.angleDeg);
  // Tapping exactly on the blip selects it; far away does not.
  const RadarCamera ov = RadarCamera::overview();
  float x, y;
  RadarScene::blipScreenXY(e, ov, x, y);
  int hit = RadarScene::hitBlip(b, n, ov, (int)x, (int)y);
  TEST_ASSERT_TRUE(hit >= 0);
  TEST_ASSERT_EQUAL_HEX32(0xAAAA0001, b[hit].id);
  TEST_ASSERT_EQUAL(-1, RadarScene::hitBlip(b, n, ov, 2, 278));
  // Locked on, the mate sits at the focus point and is still tappable there.
  float bx, by;
  RadarScene::blipOverviewXY(e, bx, by);
  RadarCamera lock = RadarCamera::lockOn(bx, by, 1.f);
  RadarScene::blipScreenXY(e, lock, x, y);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, dial::kFocusX, x);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, dial::kFocusY, y);
  hit = RadarScene::hitBlip(b, n, lock, (int)x, (int)y);
  TEST_ASSERT_TRUE(hit >= 0);
  TEST_ASSERT_EQUAL_HEX32(0xAAAA0001, b[hit].id);
  RadarCamera half = RadarCamera::lockOn(bx, by, 0.f);     // u = 0 is the overview
  TEST_ASSERT_EQUAL_FLOAT(1.f, half.s);
  TEST_ASSERT_EQUAL_FLOAT((float)layout::cx, half.tx);
  // Ewan walks to Right here: the blip glides inward rather than jumping.
  p[0].zone = Zone::RightHere;
  a.update(p, 2, 1500);
  a.step(1533);
  n = a.blips(b, kMaxPeers, 0);
  const Blip e2 = b[0].id == 0xAAAA0001 ? b[0] : b[1];
  TEST_ASSERT_TRUE(e2.radius < e.radius && e2.radius > BlipAnimator::targetRadius(Zone::RightHere, 0.5f));
  // Everyone goes silent: blips fade out and are freed.
  a.update(p, 0, 2000);
  for (uint32_t t = 2000; t <= 3000; t += 33) a.step(t);
  TEST_ASSERT_EQUAL(0, a.blips(b, kMaxPeers, 0));
}

static void test_radar_frame_is_deterministic() {
  RadarScene s;
  s.attach(gBg, gPolar, gOverlay, gClean);
  s.build();
  TEST_ASSERT_TRUE(s.ready());
  Blip b[1];
  b[0].id = 7; strcpy(b[0].label, "Sam"); b[0].mate = true; b[0].zone = Zone::Near;
  b[0].angleDeg = 120.f; b[0].radius = 40.f; b[0].color = mateColor(7); b[0].signal = 0.5f;
  RadarFrame f;
  f.tMs = 1000;
  f.beamDeg = 90.f;
  f.blips = b;
  f.nBlips = 1;
  f.hud.radio = RadioState::Live;
  strcpy(f.hud.myName, "Kyle");
  Canvas ca(gFrameA, layout::W, layout::H), cb(gFrameB, layout::W, layout::H);
  s.drawFrame(ca, f);
  s.drawFrame(cb, f);
  TEST_ASSERT_EQUAL_MEMORY(gFrameA, gFrameB, sizeof gFrameA);
  RadarFrame g = f;
  g.beamDeg = 180.f;                                     // the beam moved
  s.drawFrame(cb, g);
  TEST_ASSERT_TRUE(memcmp(gFrameA, gFrameB, sizeof gFrameA) != 0);
  // Content never leaks outside the glass: a locked-on, zoomed frame leaves
  // the bezel and the room exactly as the static layer has them (bar the
  // chrome drawn on top: lettering, lamp, buttons and the card).
  RadarFrame z = f;
  z.cam = RadarCamera::lockOn(150.f, 120.f, 1.f);
  z.focusId = 7;
  s.drawFrame(cb, z);
  RadarFrame plain = f;
  plain.blips = nullptr;
  plain.nBlips = 0;
  s.drawFrame(ca, plain);
  for (int y = 150; y < 240; ++y) {                       // left flank of the bezel
    int x = layout::cx - (int)layout::Ro + 6;
    TEST_ASSERT_EQUAL_HEX16(gFrameA[y * layout::W + x], gFrameB[y * layout::W + x]);
  }
}

static void test_pair_animation_identical_on_both_watches() {
  // Watch A (id 1) and watch B (id 2) render the same frame for the same t,
  // whichever order the ids are given in.
  PairAnimSpec a, b;
  a.kind = b.kind = AnimKind::Hello;
  a.seed = b.seed = pairSeed(1, 2, kSaltHello);
  a.idA = 1; a.idB = 2;
  b.idA = 2; b.idB = 1;
  Canvas ca(gFrameA, layout::W, layout::H), cb(gFrameB, layout::W, layout::H);
  for (int32_t t : {200, 1100, 2000}) {
    clear(ca, 0); clear(cb, 0);
    drawPairAnim(ca, a, t);
    drawPairAnim(cb, b, t);
    TEST_ASSERT_EQUAL_MEMORY(gFrameA, gFrameB, sizeof gFrameA);   // captions are equal here too
  }
  // A different pair looks different.
  PairAnimSpec c = a;
  c.idB = 3;
  c.seed = pairSeed(1, 3, kSaltHello);
  clear(ca, 0); clear(cb, 0);
  drawPairAnim(ca, a, 1500);
  drawPairAnim(cb, c, 1500);
  TEST_ASSERT_TRUE(memcmp(gFrameA, gFrameB, sizeof gFrameA) != 0);
  // Outside its duration nothing is drawn.
  clear(ca, 0x1234);
  drawPairAnim(ca, a, (int32_t)kHelloDurMs + 10);
  drawPairAnim(ca, a, -50);
  TEST_ASSERT_EQUAL_HEX16(0x1234, gFrameA[layout::cy * layout::W + layout::cx]);
}

static void test_blend_and_colour_helpers() {
  TEST_ASSERT_EQUAL_HEX16(0x0000, blend(0x0000, 0xFFFF, 0));
  TEST_ASSERT_EQUAL_HEX16(0xFFFF, blend(0x0000, 0xFFFF, 255));
  uint16_t mid = blend(0x0000, 0xFFFF, 128);
  TEST_ASSERT_TRUE(luma(mid) > 100 && luma(mid) < 155);
  TEST_ASSERT_EQUAL_HEX16(0xFFFF, addColor(0xFFFF, 0xFFFF, 255));   // saturates
  TEST_ASSERT_EQUAL_HEX16(rgb(255, 0, 0), hsv(0, 255, 255));
  TEST_ASSERT_EQUAL_HEX16(rgb(0, 255, 0), hsv(120, 255, 255));
  TEST_ASSERT_TRUE(inkOn(0xFFFF) != inkOn(0x0000));
  PairPalette p = pairPalette(10, 20), q = pairPalette(20, 10);
  TEST_ASSERT_EQUAL_HEX16(p.a, q.a);
  TEST_ASSERT_EQUAL_HEX16(p.b, q.b);
  // Every pair gets two distinct lights, and no mate light is phosphor green.
  for (uint32_t i = 1; i < 300; ++i) {
    PairPalette r = pairPalette(i, i * 7919u + 13u);
    TEST_ASSERT_TRUE(r.a != r.b);
    uint16_t m = mateColor(i);
    float R, G, B;
    unpack(m, R, G, B);
    TEST_ASSERT_FALSE(G > R + 40.f && G > B + 20.f);   // never green-dominant
  }
}

static void test_springs_settle_independent_of_frame_rate() {
  Spring a(14.f, 0.75f), b(14.f, 0.75f);
  a.snap(0.f); b.snap(0.f);
  a.target = b.target = 1.f;
  for (int i = 0; i < 30; ++i) a.step(1.f / 60.f);      // 0.5 s at 60 fps
  for (int i = 0; i < 10; ++i) b.step(1.f / 20.f);      // 0.5 s at 20 fps
  TEST_ASSERT_FLOAT_WITHIN(0.01f, a.x, b.x);
  for (int i = 0; i < 200; ++i) a.step(1.f / 60.f);
  TEST_ASSERT_TRUE(a.settled());
  TEST_ASSERT_EQUAL_FLOAT(1.f, a.x);
  // Underdamped: it overshoots a little on the way, like a real card would.
  Spring c(14.f, 0.6f);
  c.snap(0.f);
  c.target = 1.f;
  float peak = 0.f;
  for (int i = 0; i < 120; ++i) { c.step(1.f / 60.f); if (c.x > peak) peak = c.x; }
  TEST_ASSERT_TRUE(peak > 1.01f && peak < 1.2f);
}

static void test_page_slide() {
  // Old page all 0x1111, new page all 0x2222.
  static uint16_t under[dial::kBgPixels];
  for (auto &p : under) p = 0x1111;
  Canvas c(gFrameA, layout::W, layout::H);
  clear(c, 0x2222);
  composeSlide(c, under, 1.f, true);                    // finished: only the new page
  TEST_ASSERT_EQUAL_HEX16(0x2222, gFrameA[10]);
  TEST_ASSERT_EQUAL_HEX16(0x2222, gFrameA[layout::W * 140 + 239]);
  clear(c, 0x2222);
  composeSlide(c, under, 0.f, true);                    // not started: only the old page
  TEST_ASSERT_EQUAL_HEX16(0x1111, gFrameA[layout::W * 140 + 100]);
  clear(c, 0x2222);
  composeSlide(c, under, 0.5f, true);                   // half way: old (dimmed) left, new right
  TEST_ASSERT_EQUAL_HEX16(0x2222, gFrameA[layout::W * 140 + 200]);
  uint16_t left = gFrameA[layout::W * 140 + 20];
  TEST_ASSERT_TRUE(left != 0x1111 && left != 0x2222);
  TEST_ASSERT_TRUE(luma(left) < luma(0x1111));
  clear(c, 0x2222);
  composeSlide(c, under, 0.5f, false);                  // going back: the old page slides off right
  TEST_ASSERT_EQUAL_HEX16(0x1111, gFrameA[layout::W * 140 + 200]);
}

static void test_clipping_never_writes_outside() {
  // Shapes far off-canvas or straddling the edges must stay in bounds.
  static uint16_t guard[10 + dial::kBgPixels + 10];
  for (auto &g : guard) g = 0xBEEF;
  Canvas c(guard + 10, layout::W, layout::H);
  clear(c, 0);
  fillCircle(c, -30.f, -30.f, 50.f, 0xFFFF);
  fillCircle(c, 260.f, 300.f, 50.f, 0xFFFF);
  ring(c, 120.f, 140.f, 300.f, 3.f, 0xFFFF);
  line(c, -100.f, -100.f, 400.f, 500.f, 3.f, 0xFFFF);
  fillRoundRect(c, -20, 270, 300, 40, 10.f, 0xFFFF);
  glow(c, 239.f, 279.f, 40.f, 0xFFFF, 255);
  petal(c, 0.f, 0.f, 2.3f, 200.f, 20.f, 0xFFFF);
  arc(c, 120.f, 140.f, 160.f, 4.f, 10.f, 300.f, 0xFFFF);
  text(c, kFontXL, 200, 290, "Overflow", 0xFFFF);
  textCentered(c, kFontL, -40, 10, "Edge", 0xFFFF);
  for (int i = 0; i < 10; ++i) {
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, guard[i]);
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, guard[10 + dial::kBgPixels + i]);
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_text_metrics_and_fit);
  RUN_TEST(test_word_wrap);
  RUN_TEST(test_format_when_and_minutes);
  RUN_TEST(test_list_rows_and_scroll);
  RUN_TEST(test_keyboard_mapping);
  RUN_TEST(test_touch_targets_are_at_least_40px);
  RUN_TEST(test_zone_labels_never_collide_with_blips);
  RUN_TEST(test_blip_animator_and_hit);
  RUN_TEST(test_radar_frame_is_deterministic);
  RUN_TEST(test_pair_animation_identical_on_both_watches);
  RUN_TEST(test_blend_and_colour_helpers);
  RUN_TEST(test_clipping_never_writes_outside);
  RUN_TEST(test_springs_settle_independent_of_frame_rate);
  RUN_TEST(test_page_slide);
  return UNITY_END();
}
