// Answer packs and the random picker. Portable C++, unit-tested on the host.
//
// Every answer here is original writing for Shake Oracle. Text must be plain
// printable ASCII (the GFX fonts stop at '~') and short enough to fit the die
// at a readable size; test/test_text_fit checks every entry.
#pragma once
#include <stdint.h>

namespace oracle {

enum class Kind : uint8_t { Yes, Maybe, No, Rare, Pick };

struct Answer {
  const char *text;
  Kind        kind;
};

struct Pack {
  const char   *name;     // shown on the ball, e.g. "FOOD"
  const char   *prompt;   // idle prompt in the liquid ('\n' = line break)
  const Answer *answers;
  uint8_t       count;
  const char   *golden;   // the rare jackpot answer for this pack
};

int         packCount();
const Pack &packAt(int index);   // index is clamped into range

// Small, fast, well-distributed PRNG (PCG32, O'Neill 2014).
class Pcg32 {
 public:
  explicit Pcg32(uint64_t seed = 0x853c49e6748fea9bULL) { seed64(seed); }
  void     seed64(uint64_t seed);
  void     mix(uint32_t entropy);          // stir extra entropy in
  uint32_t next();
  uint32_t below(uint32_t bound);          // uniform in [0, bound)
  float    unit();                         // uniform in [0, 1)
  uint64_t state() const { return state_; }
  void     setState(uint64_t s) { state_ = s; }
 private:
  uint64_t state_ = 0;
};

struct Pick {
  int         pack = 0;
  int         index = -1;     // -1 means the golden answer
  bool        golden = false;
  const char *text = "";
};

// Picks answers with per-kind weights (rare answers are rarer), rolls the
// golden answer about once in kGoldenOdds, and never returns the answer that
// was shown last, so the same answer can't come up twice in a row.
class AnswerPicker {
 public:
  explicit AnswerPicker(uint64_t seed = 1) : rng_(seed) {}
  void  seed(uint64_t s) { rng_.seed64(s); }
  void  mix(uint32_t entropy) { rng_.mix(entropy); }
  Pick  next(int pack);
  // The previous pick, so it can survive deep sleep in RTC memory.
  void  setLast(int pack, int index) { lastPack_ = pack; lastIndex_ = index; }
  int   lastPack() const { return lastPack_; }
  int   lastIndex() const { return lastIndex_; }
  Pcg32 &rng() { return rng_; }
  uint64_t rngState() const { return rng_.state(); }
 private:
  Pcg32 rng_;
  int   lastPack_ = -1;
  int   lastIndex_ = -2;   // -1 is the golden answer; -2 means none yet
};

uint16_t answerWeight(Kind k);

}  // namespace oracle
