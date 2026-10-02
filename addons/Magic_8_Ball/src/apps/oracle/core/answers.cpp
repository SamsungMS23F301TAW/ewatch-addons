#include "answers.h"

#include <string.h>

#include "oracle_config.h"

namespace oracle {

namespace {

// ---- ORACLE: the classic pack. Balanced 9 yes / 9 maybe / 9 no, plus three
// rare surprises. Written for Shake Oracle; deliberately not the toy's list.
const Answer kOracle[] = {
    {"Absolutely", Kind::Yes},
    {"Heck yes", Kind::Yes},
    {"The deep says yes", Kind::Yes},
    {"Go for it", Kind::Yes},
    {"Oh, totally", Kind::Yes},
    {"Green light", Kind::Yes},
    {"Yes, with sprinkles", Kind::Yes},
    {"The stars agree", Kind::Yes},
    {"Do it. Today.", Kind::Yes},

    {"Too murky to say", Kind::Maybe},
    {"Shake harder", Kind::Maybe},
    {"Ask after snacks", Kind::Maybe},
    {"Hmm. Maybe?", Kind::Maybe},
    {"50/50, honestly", Kind::Maybe},
    {"Sleep on it", Kind::Maybe},
    {"Could go either way", Kind::Maybe},
    {"Try a better question", Kind::Maybe},
    {"The oracle is napping", Kind::Maybe},

    {"Nope", Kind::No},
    {"Not a chance", Kind::No},
    {"Hard pass", Kind::No},
    {"In your dreams", Kind::No},
    {"The deep says no", Kind::No},
    {"Let's not", Kind::No},
    {"Red light", Kind::No},
    {"Absolutely not", Kind::No},
    {"Not in this timeline", Kind::No},

    {"Ask your cat", Kind::Rare},
    {"Why ask a watch?", Kind::Rare},
    {"Plot twist: yes", Kind::Rare},
};

// ---- YES / NO: decisive answers only, evenly split.
const Answer kYesNo[] = {
    {"Yep", Kind::Yes},
    {"Yes, go", Kind::Yes},
    {"Definitely", Kind::Yes},
    {"For sure", Kind::Yes},
    {"Do it", Kind::Yes},
    {"Uh-huh", Kind::Yes},
    {"Yes, yes, yes", Kind::Yes},
    {"Affirmative", Kind::Yes},

    {"Nope", Kind::No},
    {"No, stop", Kind::No},
    {"Definitely not", Kind::No},
    {"No way", Kind::No},
    {"Don't", Kind::No},
    {"Nuh-uh", Kind::No},
    {"No, no, no", Kind::No},
    {"Negative", Kind::No},
};

// ---- FOOD: settles "what should we eat?".
const Answer kFood[] = {
    {"Pizza", Kind::Pick},
    {"Tacos", Kind::Pick},
    {"Noodles", Kind::Pick},
    {"Sushi", Kind::Pick},
    {"Curry", Kind::Pick},
    {"Burgers", Kind::Pick},
    {"Dumplings", Kind::Pick},
    {"Soup", Kind::Pick},
    {"Pasta", Kind::Pick},
    {"Fried rice", Kind::Pick},
    {"A big salad", Kind::Pick},
    {"Breakfast for dinner", Kind::Pick},
    {"Leftovers", Kind::Pick},
    {"Toast. Just toast.", Kind::Pick},
    {"Something spicy", Kind::Pick},
    {"A big sandwich", Kind::Pick},
    {"Cook something new", Kind::Pick},
    {"Cereal. No shame.", Kind::Pick},
};

// ---- EXCUSES: for when you need one, fast.
const Answer kExcuses[] = {
    {"My cat sat on it", Kind::Pick},
    {"Mercury is in retrograde", Kind::Pick},
    {"The internet ate it", Kind::Pick},
    {"I had a thing", Kind::Pick},
    {"Long story", Kind::Pick},
    {"My plant needed me", Kind::Pick},
    {"Low battery (mine)", Kind::Pick},
    {"A goose blocked the path", Kind::Pick},
    {"It's a whole thing", Kind::Pick},
    {"Traffic. Somehow.", Kind::Pick},
    {"My horoscope said no", Kind::Pick},
    {"Overslept. Heroically.", Kind::Pick},
    {"The oracle said no", Kind::Pick},
    {"I'm in my cocoon era", Kind::Pick},
    {"Allergic to Mondays", Kind::Pick},
    {"My socks didn't match", Kind::Pick},
};

#define ORACLE_COUNT(a) (uint8_t)(sizeof(a) / sizeof((a)[0]))

const Pack kPacks[] = {
    {"ORACLE", "Ask a question,\nthen shake", kOracle, ORACLE_COUNT(kOracle), "Cosmic YES!"},
    {"YES / NO", "Yes or no?\nShake to decide", kYesNo, ORACLE_COUNT(kYesNo),
     "Biggest YES ever"},
    {"FOOD", "What's for dinner?\nShake to choose", kFood, ORACLE_COUNT(kFood),
     "Dessert first!"},
    {"EXCUSES", "Need an excuse?\nShake for one", kExcuses, ORACLE_COUNT(kExcuses),
     "Try the truth. Wild!"},
};

}  // namespace

int packCount() { return (int)(sizeof(kPacks) / sizeof(kPacks[0])); }

const Pack &packAt(int index) {
  if (index < 0) index = 0;
  if (index >= packCount()) index = packCount() - 1;
  return kPacks[index];
}

uint16_t answerWeight(Kind k) {
  return k == Kind::Rare ? kWeightRare : kWeightNormal;
}

// ---------------------------------------------------------------- PCG32

void Pcg32::seed64(uint64_t seed) {
  state_ = 0;
  next();
  state_ += seed;
  next();
}

void Pcg32::mix(uint32_t entropy) {
  state_ ^= (uint64_t)entropy * 0x9E3779B97F4A7C15ULL;
  next();
}

uint32_t Pcg32::next() {
  uint64_t old = state_;
  state_ = old * 6364136223846793005ULL + 1442695040888963407ULL;
  uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
  uint32_t rot = (uint32_t)(old >> 59u);
  return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
}

uint32_t Pcg32::below(uint32_t bound) {
  if (bound <= 1) return 0;
  // Lemire's multiply-shift with rejection: unbiased.
  uint32_t threshold = (uint32_t)(-bound) % bound;
  for (;;) {
    uint64_t m = (uint64_t)next() * bound;
    if ((uint32_t)m >= threshold) return (uint32_t)(m >> 32);
  }
}

float Pcg32::unit() { return (next() >> 8) * (1.0f / 16777216.0f); }

// ---------------------------------------------------------------- picker

Pick AnswerPicker::next(int packIndex) {
  if (packIndex < 0) packIndex = 0;
  if (packIndex >= packCount()) packIndex = packCount() - 1;
  const Pack &p = kPacks[packIndex];

  // The text shown last, from whichever pack, so a pack switch can't repeat
  // it either ("Nope" lives in two packs).
  const char *lastText = nullptr;
  if (lastPack_ >= 0 && lastPack_ < packCount()) {
    const Pack &lp = kPacks[lastPack_];
    if (lastIndex_ == -1) lastText = lp.golden;
    else if (lastIndex_ >= 0 && lastIndex_ < lp.count) lastText = lp.answers[lastIndex_].text;
  }
  auto same = [&](const char *t) -> bool { return lastText && t && strcmp(t, lastText) == 0; };

  Pick out;
  out.pack = packIndex;

  bool goldenOk = p.golden && !same(p.golden);
  if (goldenOk && rng_.below(kGoldenOdds) == 0) {
    out.index = -1;
    out.golden = true;
    out.text = p.golden;
  } else {
    uint32_t total = 0;
    for (int i = 0; i < p.count; i++)
      if (!same(p.answers[i].text)) total += answerWeight(p.answers[i].kind);
    int chosen = 0;
    if (total > 0) {
      uint32_t r = rng_.below(total);
      for (int i = 0; i < p.count; i++) {
        if (same(p.answers[i].text)) continue;
        uint16_t w = answerWeight(p.answers[i].kind);
        if (r < w) { chosen = i; break; }
        r -= w;
      }
    }
    out.index = chosen;
    out.text = p.answers[chosen].text;
  }
  lastPack_ = packIndex;
  lastIndex_ = out.index;
  return out;
}

}  // namespace oracle
