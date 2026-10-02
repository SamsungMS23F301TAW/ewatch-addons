#include "mc_event.h"
#include "mc_text.h"
#include <string.h>

namespace mc {

uint32_t instanceKey(uint32_t uid, int64_t originalStart) {
  uint32_t h = fnv1aByte(0x5A, uid ^ 0x9E3779B9u);
  for (int i = 0; i < 8; i++) h = fnv1aByte((uint8_t)(originalStart >> (i * 8)), h);
  return h;
}

bool eventLess(const Event &a, const Event &b) {
  if (a.start != b.start) return a.start < b.start;
  if (a.end != b.end) return a.end < b.end;
  int c = strncmp(a.title, b.title, kTitleMax);
  if (c != 0) return c < 0;
  return a.key < b.key;
}

void sortEvents(Event *ev, int n) {
  // Insertion sort: n is tiny (<= a few dozen) and the input is usually
  // nearly sorted already.
  for (int i = 1; i < n; i++) {
    Event t = ev[i];
    int j = i - 1;
    while (j >= 0 && eventLess(t, ev[j])) { ev[j + 1] = ev[j]; j--; }
    ev[j + 1] = t;
  }
}

}  // namespace mc
