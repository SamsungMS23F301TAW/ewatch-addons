// Internal interface between ArtJob and the composition families.
//
// A family is five functions:
//   Init       decide parameters (draws only from ctx.famRng)
//   PrepCount  number of fixed prep ops (background, fields, precomputation)
//   Prep(i)    prep op i, called once each, in order
//   ElemCount  number of growth elements at a growth level (monotonic!)
//   Elem(i)    element i, called once each, in order (draws from elemRng)
// Elements are appended as the step bucket rises, so Elem(i) must never
// depend on how many elements will eventually be drawn.
#pragma once
#include "gf_art.h"
#include "gf_noise.h"

namespace gf {

static const uint32_t kStateBytes = 4096;     // per-family state block

struct JobCtx {
  Canvas        *cv;
  uint8_t       *mem;       // family scratch (after the state block)
  uint32_t       memBytes;
  const ArtSpec *spec;
  Rng           *famRng;
  Rng           *elemRng;
};

#define GF_FAMILY_DECL(name)                                   \
  void    name##Init(void *st, JobCtx &ctx);                   \
  int32_t name##PrepCount(const void *st);                     \
  void    name##Prep(void *st, JobCtx &ctx, int32_t i);        \
  int32_t name##ElemCount(const void *st, int32_t growth);     \
  void    name##Elem(void *st, JobCtx &ctx, int32_t i);

GF_FAMILY_DECL(currents)
GF_FAMILY_DECL(orbits)
GF_FAMILY_DECL(weave)
GF_FAMILY_DECL(garden)
GF_FAMILY_DECL(contours)
GF_FAMILY_DECL(mosaic)
GF_FAMILY_DECL(dunes)

// ---- helpers shared by the families -----------------------------------

// Standard background: palette gradient + light film grain, rows [y0, y1).
void backgroundRows(JobCtx &ctx, int32_t y0, int32_t y1, int32_t grain);
static const int32_t kBgRowsPerOp = 28;       // 10 ops for 280 rows

// Linear interpolation of element counts: n0 at growth 0, n1 at 1000.
static inline int32_t growCount(int32_t n0, int32_t n1, int32_t growth) {
  return n0 + (int32_t)(((int64_t)(n1 - n0) * growth) / 1000);
}

// Sorts `keys` ascending in place (insertion sort). Callers make keys
// unique (index in the low bits), so the order is fully deterministic.
void sortKeys(uint32_t *keys, int32_t n);

// Uniformly picks one of the five inks, with a small chance of the accent.
uint32_t pickInk(const Palette &p, Rng &r, uint32_t accentPermille);

}  // namespace gf
