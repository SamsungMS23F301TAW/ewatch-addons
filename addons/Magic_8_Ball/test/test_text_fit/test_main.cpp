// Text fitting tests: every answer in every pack must fit on the die at a
// legible size, with every rasterised pixel of ink inside the triangle; every
// idle prompt must fit in the liquid. Uses the real BaseOS font.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "FreeSansBold24pt7b.h"
#include "answers.h"
#include "oracle_config.h"
#include "oracle_render.h"
#include "text_layout.h"

using namespace oracle;

namespace {

const GFXfont *font() { return &FreeSansBold24pt7b; }

FitParams dieParams() {
  FitParams p;
  p.capMax = kDieCapMax;
  p.capMin = kDieCapMin;
  p.condense = kDieCondense;
  p.lineGap = kDieLineGap;
  p.maxLines = kDieMaxLines;
  p.uppercase = true;
  return p;
}

template <typename F>
void forEachAnswer(F f) {
  for (int p = 0; p < packCount(); p++) {
    const Pack &pk = packAt(p);
    for (int i = 0; i < pk.count; i++) f(pk.answers[i].text);
    f(pk.golden);
  }
}

Renderer &renderer() {
  static Renderer r;
  static bool ok = r.begin(font());
  TEST_ASSERT_TRUE_MESSAGE(ok, "renderer allocates on the host");
  return r;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_every_answer_fits_at_a_legible_size() {
  TriangleRegion tri(kDieR, kDieTextInset, kDiePointsDown);
  float smallest = 1e9f;
  const char *worst = "";
  forEachAnswer([&](const char *text) {
    TextLayout L;
    bool ok = fitText(font(), text, tri, dieParams(), L);
    char msg[128];
    snprintf(msg, sizeof msg, "\"%s\" must fit (cap %.2f)", text, L.cap);
    TEST_ASSERT_TRUE_MESSAGE(ok, msg);
    TEST_ASSERT_TRUE_MESSAGE(L.cap >= kDieCapMin - 1e-3f, msg);
    TEST_ASSERT_TRUE_MESSAGE(L.lineCount >= 1 && L.lineCount <= kDieMaxLines, msg);
    if (L.cap < smallest) { smallest = L.cap; worst = text; }
  });
  printf("smallest die text: \"%s\" at %.2f px caps\n", worst, smallest);
}

void test_ink_box_stays_inside_the_inset_triangle() {
  // Every glyph's ink rectangle (all four corners) is inside the triangle
  // inset by the text margin, with a little float tolerance.
  TriangleRegion inner(kDieR, kDieTextInset, kDiePointsDown);
  forEachAnswer([&](const char *text) {
    TextLayout L;
    fitText(font(), text, inner, dieParams(), L);
    float x0, y0, x1, y1;
    layoutInkBounds(font(), L, x0, y0, x1, y1);
    char msg[128];
    snprintf(msg, sizeof msg, "ink of \"%s\" leaves the triangle", text);
    // Line by line: the ink box of each line must be inside.
    for (int k = 0; k < L.lineCount; k++) {
      TextLayout one = L;
      one.lineCount = 1;
      one.lines[0] = L.lines[k];
      float lx0, ly0, lx1, ly1;
      layoutInkBounds(font(), one, lx0, ly0, lx1, ly1);
      const float cx[4] = {lx0, lx1, lx0, lx1}, cy[4] = {ly0, ly0, ly1, ly1};
      for (int c = 0; c < 4; c++)
        TEST_ASSERT_TRUE_MESSAGE(inner.edgeDistance(cx[c], cy[c]) <= 0.05f, msg);
    }
  });
}

void test_rasterised_ink_is_on_the_die_face() {
  // Render each answer into the real die sprite and check pixel by pixel:
  // wherever there is any text coverage, the die face is fully opaque and
  // the pixel sits at least 2.5 px inside the face's edges.
  Renderer &r = renderer();
  TriangleRegion edges(kDieR, 0.f, kDiePointsDown);
  forEachAnswer([&](const char *text) {
    r.setDieText(text);
    const uint32_t *sp = r.sprite();
    int inked = 0;
    for (int j = 0; j < r.spriteH(); j++) {
      for (int i = 0; i < r.spriteW(); i++) {
        uint32_t t = sp[j * r.spriteW() + i];
        int cov = t & 0xFF, txt = (t >> 16) & 0xFF;
        if (!txt) continue;
        inked++;
        float lx = i + 0.5f - r.spriteOX(), ly = j + 0.5f - r.spriteOY();
        char msg[160];
        snprintf(msg, sizeof msg, "\"%s\": ink at (%.1f, %.1f) is off the face", text, lx, ly);
        TEST_ASSERT_EQUAL_INT_MESSAGE(255, cov, msg);
        TEST_ASSERT_TRUE_MESSAGE(edges.edgeDistance(lx, ly) <= -2.5f, msg);
      }
    }
    char msg[96];
    snprintf(msg, sizeof msg, "\"%s\" drew no ink", text);
    TEST_ASSERT_TRUE_MESSAGE(inked > 40, msg);
  });
}

void test_lines_are_centred() {
  TriangleRegion tri(kDieR, kDieTextInset, kDiePointsDown);
  forEachAnswer([&](const char *text) {
    TextLayout L;
    fitText(font(), text, tri, dieParams(), L);
    for (int k = 0; k < L.lineCount; k++) {
      TextLayout one = L;
      one.lineCount = 1;
      one.lines[0] = L.lines[k];
      float x0, y0, x1, y1;
      layoutInkBounds(font(), one, x0, y0, x1, y1);
      TEST_ASSERT_FLOAT_WITHIN(0.6f, 0.f, 0.5f * (x0 + x1));
    }
  });
}

void test_every_pack_prompt_fits_the_liquid() {
  DiscRegion disc(kWinR - 22.f);
  for (int p = 0; p < packCount(); p++) {
    FitParams fp;
    fp.capMax = kPromptCapMax;
    fp.capMin = kPromptCapMin;
    fp.condense = 0.92f;
    fp.lineGap = 0.55f;
    fp.maxLines = 3;
    fp.uppercase = false;
    fp.tracking = 0.3f;
    TextLayout L;
    TEST_ASSERT_TRUE_MESSAGE(fitText(font(), packAt(p).prompt, disc, fp, L), packAt(p).prompt);
    TEST_ASSERT_EQUAL_INT(2, L.lineCount);   // every prompt has one forced break
  }
}

void test_forced_line_breaks_are_honoured() {
  DiscRegion disc(80.f);
  FitParams fp;
  fp.capMax = 12.f;
  fp.capMin = 6.f;
  fp.maxLines = 4;
  fp.uppercase = false;
  TextLayout L;
  TEST_ASSERT_TRUE(fitText(font(), "one two\nthree", disc, fp, L));
  TEST_ASSERT_EQUAL_INT(2, L.lineCount);
  TEST_ASSERT_EQUAL_STRING_LEN("one two", L.text + L.lines[0].start, L.lines[0].len);
  TEST_ASSERT_EQUAL_INT(7, L.lines[0].len);
  TEST_ASSERT_EQUAL_STRING_LEN("three", L.text + L.lines[1].start, L.lines[1].len);
}

void test_normalisation_uppercases_and_collapses_space() {
  TriangleRegion tri(kDieR, kDieTextInset, kDiePointsDown);
  TextLayout L;
  TEST_ASSERT_TRUE(fitText(font(), "  heck   yes ", tri, dieParams(), L));
  TEST_ASSERT_EQUAL_STRING("HECK YES", L.text);
}

void test_unfittable_text_fails_gracefully() {
  TriangleRegion tri(kDieR, kDieTextInset, kDiePointsDown);
  TextLayout L;
  // One enormous word cannot wrap; the fitter says no instead of overflowing.
  TEST_ASSERT_FALSE(fitText(font(), "Supercalifragilisticexpialidocious", tri, dieParams(), L));
  TEST_ASSERT_FALSE(L.ok);
  // Empty and whitespace-only text never "fits".
  TEST_ASSERT_FALSE(fitText(font(), "", tri, dieParams(), L));
  TEST_ASSERT_FALSE(fitText(font(), "   ", tri, dieParams(), L));
  // Characters outside the font must not crash the rasteriser.
  TEST_ASSERT_TRUE(fitText(font(), "caf\xC3\xA9 \x01", tri, dieParams(), L));
}

void test_bigger_region_never_gives_smaller_text() {
  // Sanity of the shrink-to-fit search: growing the die never shrinks text.
  forEachAnswer([&](const char *text) {
    TextLayout a, b;
    fitText(font(), text, TriangleRegion(kDieR, kDieTextInset, kDiePointsDown), dieParams(), a);
    fitText(font(), text, TriangleRegion(kDieR + 10.f, kDieTextInset, kDiePointsDown), dieParams(), b);
    TEST_ASSERT_TRUE(b.cap + 0.05f >= a.cap);
  });
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_every_answer_fits_at_a_legible_size);
  RUN_TEST(test_ink_box_stays_inside_the_inset_triangle);
  RUN_TEST(test_rasterised_ink_is_on_the_die_face);
  RUN_TEST(test_lines_are_centred);
  RUN_TEST(test_every_pack_prompt_fits_the_liquid);
  RUN_TEST(test_forced_line_breaks_are_honoured);
  RUN_TEST(test_normalisation_uppercases_and_collapses_space);
  RUN_TEST(test_unfittable_text_fails_gracefully);
  RUN_TEST(test_bigger_region_never_gives_smaller_text);
  return UNITY_END();
}
