// Answer packs and picker tests: content rules, originality, balance,
// "never twice in a row", rarity of rare and golden answers, determinism.
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include <map>
#include <string>

#include "answers.h"
#include "oracle_config.h"

using namespace oracle;

namespace {

// FNV-1a of the lowercase letters of a phrase.
uint32_t letterHash(const char *s) {
  uint32_t h = 0x811C9DC5u;
  for (; *s; s++) {
    char c = (char)tolower((unsigned char)*s);
    if (c < 'a' || c > 'z') continue;
    h ^= (uint8_t)c;
    h *= 0x01000193u;
  }
  return h;
}

// Hashes (letters only, lowercase) of the twenty answers printed in the
// well-known toy. Only the hashes are kept here, never the text, so this
// repository doesn't carry that list; the test proves none of ours matches.
const uint32_t kToyAnswerHashes[] = {
    0x62176F32u, 0x63A1C07Du, 0x1A5CDA74u, 0xCC7498EBu, 0xA9269F27u,
    0xB6C39685u, 0x445307C2u, 0x4401CA89u, 0x4E9F3590u, 0x8F7180CBu,
    0xCA9FA622u, 0x880B88A4u, 0x49E91CFAu, 0x64990C8Du, 0x9534591Bu,
    0x31F89455u, 0x33FA977Au, 0x5F99E531u, 0xF90142B6u, 0x2D3FC33Cu,
};

template <typename F>
void forEachText(F f) {
  for (int p = 0; p < packCount(); p++) {
    const Pack &pk = packAt(p);
    for (int i = 0; i < pk.count; i++) f(p, pk.answers[i].text);
    f(p, pk.golden);
  }
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_oracle_pack_is_balanced() {
  const Pack &p = packAt(0);
  int yes = 0, maybe = 0, no = 0, rare = 0;
  for (int i = 0; i < p.count; i++) {
    switch (p.answers[i].kind) {
      case Kind::Yes: yes++; break;
      case Kind::Maybe: maybe++; break;
      case Kind::No: no++; break;
      case Kind::Rare: rare++; break;
      default: TEST_FAIL_MESSAGE("oracle pack holds yes/maybe/no/rare only");
    }
  }
  TEST_ASSERT_TRUE(p.count >= 25 && p.count <= 32);
  TEST_ASSERT_EQUAL_INT(yes, no);
  TEST_ASSERT_INT_WITHIN(1, yes, maybe);
  TEST_ASSERT_TRUE(rare >= 2 && rare <= 5);
  TEST_ASSERT_NOT_NULL(p.golden);
}

void test_text_is_short_printable_ascii() {
  forEachText([](int, const char *t) {
    size_t n = strlen(t);
    TEST_ASSERT_TRUE_MESSAGE(n >= 2 && n <= 26, t);
    TEST_ASSERT_TRUE_MESSAGE(t[0] != ' ' && t[n - 1] != ' ', t);
    for (const char *c = t; *c; c++)
      TEST_ASSERT_TRUE_MESSAGE(*c >= ' ' && *c <= '~', t);   // GFX fonts end at '~'
    TEST_ASSERT_NULL_MESSAGE(strstr(t, "  "), t);
  });
  for (int p = 0; p < packCount(); p++) {
    TEST_ASSERT_TRUE(strlen(packAt(p).name) > 0 && strlen(packAt(p).name) <= 10);
    TEST_ASSERT_NOT_NULL(strchr(packAt(p).prompt, '\n'));
  }
}

void test_no_duplicates_within_a_pack() {
  for (int p = 0; p < packCount(); p++) {
    const Pack &pk = packAt(p);
    for (int i = 0; i < pk.count; i++) {
      TEST_ASSERT_TRUE_MESSAGE(strcmp(pk.answers[i].text, pk.golden) != 0, pk.golden);
      for (int j = i + 1; j < pk.count; j++)
        TEST_ASSERT_TRUE_MESSAGE(letterHash(pk.answers[i].text) != letterHash(pk.answers[j].text),
                                 pk.answers[i].text);
    }
  }
}

void test_answers_are_original() {
  forEachText([](int, const char *t) {
    uint32_t h = letterHash(t);
    for (uint32_t toy : kToyAnswerHashes) TEST_ASSERT_TRUE_MESSAGE(h != toy, t);
  });
}

void test_never_the_same_answer_twice_in_a_row() {
  AnswerPicker picker(0xC0FFEEULL);
  Pcg32 chooser(7);
  const char *last = nullptr;
  int pack = 0;
  for (int i = 0; i < 200000; i++) {
    if (chooser.below(5) == 0) pack = (int)chooser.below((uint32_t)packCount());   // switch packs
    Pick p = picker.next(pack);
    TEST_ASSERT_NOT_NULL(p.text);
    if (last) TEST_ASSERT_TRUE_MESSAGE(strcmp(last, p.text) != 0, p.text);
    last = p.text;
  }
}

void test_distribution_rare_and_golden() {
  AnswerPicker picker(42);
  const Pack &pk = packAt(0);
  std::map<std::string, int> counts;
  const int N = 300000;
  int golden = 0;
  for (int i = 0; i < N; i++) {
    Pick p = picker.next(0);
    if (p.golden) golden++;
    else counts[p.text]++;
  }
  // Golden: about 1 in kGoldenOdds.
  float gRate = (float)golden / N;
  TEST_ASSERT_FLOAT_WITHIN(0.25f / kGoldenOdds, 1.f / kGoldenOdds, gRate);
  // Every answer shows up; rare ones at about kWeightRare/kWeightNormal.
  double normalSum = 0, rareSum = 0;
  int normalN = 0, rareN = 0;
  for (int i = 0; i < pk.count; i++) {
    int c = counts[pk.answers[i].text];
    TEST_ASSERT_TRUE_MESSAGE(c > 0, pk.answers[i].text);
    if (pk.answers[i].kind == Kind::Rare) { rareSum += c; rareN++; }
    else { normalSum += c; normalN++; }
  }
  double ratio = (rareSum / rareN) / (normalSum / normalN);
  TEST_ASSERT_FLOAT_WITHIN(0.03f, (float)kWeightRare / kWeightNormal, (float)ratio);
  // Normal answers are evenly spread (within 6% of their mean).
  double mean = normalSum / normalN;
  for (int i = 0; i < pk.count; i++) {
    if (pk.answers[i].kind == Kind::Rare) continue;
    double c = counts[pk.answers[i].text];
    TEST_ASSERT_TRUE_MESSAGE(c > mean * 0.94 && c < mean * 1.06, pk.answers[i].text);
  }
}

void test_same_seed_same_sequence() {
  AnswerPicker a(123), b(123), c(124);
  bool differs = false;
  for (int i = 0; i < 200; i++) {
    Pick pa = a.next(i % packCount()), pb = b.next(i % packCount()), pc = c.next(i % packCount());
    TEST_ASSERT_EQUAL_STRING(pa.text, pb.text);
    if (strcmp(pa.text, pc.text) != 0) differs = true;
  }
  TEST_ASSERT_TRUE(differs);
}

void test_state_survives_sleep() {
  // The watch keeps (last pack, last index, RNG) in RTC memory across deep
  // sleep; a fresh picker restored from it must not repeat the last answer.
  for (uint64_t seed = 1; seed < 2000; seed++) {
    AnswerPicker before(seed);
    Pick shown = before.next(0);
    AnswerPicker after(999);   // a new boot
    after.setLast(before.lastPack(), before.lastIndex());
    after.rng().setState(before.rngState());
    after.mix((uint32_t)seed * 2654435761u);   // fresh hardware entropy
    Pick next = after.next(0);
    TEST_ASSERT_TRUE(strcmp(shown.text, next.text) != 0);
  }
}

void test_rng_bounds() {
  Pcg32 r(9);
  for (int i = 0; i < 100000; i++) {
    TEST_ASSERT_TRUE(r.below(7) < 7);
    float u = r.unit();
    TEST_ASSERT_TRUE(u >= 0.f && u < 1.f);
  }
  TEST_ASSERT_EQUAL_UINT32(0, r.below(0));
  TEST_ASSERT_EQUAL_UINT32(0, r.below(1));
}

void test_pack_index_is_clamped() {
  TEST_ASSERT_EQUAL_STRING(packAt(0).name, packAt(-5).name);
  TEST_ASSERT_EQUAL_STRING(packAt(packCount() - 1).name, packAt(99).name);
  AnswerPicker p(1);
  TEST_ASSERT_NOT_NULL(p.next(-3).text);
  TEST_ASSERT_NOT_NULL(p.next(42).text);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_oracle_pack_is_balanced);
  RUN_TEST(test_text_is_short_printable_ascii);
  RUN_TEST(test_no_duplicates_within_a_pack);
  RUN_TEST(test_answers_are_original);
  RUN_TEST(test_never_the_same_answer_twice_in_a_row);
  RUN_TEST(test_distribution_rare_and_golden);
  RUN_TEST(test_same_seed_same_sequence);
  RUN_TEST(test_state_survives_sleep);
  RUN_TEST(test_rng_bounds);
  RUN_TEST(test_pack_index_is_clamped);
  return UNITY_END();
}
