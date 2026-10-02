// Fixed-point gradient noise (Perlin-style) for the generators.
// Inputs are Q16 lattice coordinates; outputs are Q16 signed, roughly
// [-65536, 65536]. Pure integer maths, so identical on every platform.
#pragma once
#include <stdint.h>
#include "gf_core.h"

namespace gf {

int32_t noise2(int32_t x, int32_t y, uint32_t seed);
// Fractal sum of `octaves` noise layers (lacunarity 2, gain 1/2), normalised.
int32_t fbm2(int32_t x, int32_t y, uint32_t seed, int32_t octaves);
int32_t noise1(int32_t x, uint32_t seed);
int32_t fbm1(int32_t x, uint32_t seed, int32_t octaves);

}  // namespace gf
