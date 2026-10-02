#include "tp_shade.h"
#include "tp_noise.h"
#include "tp_platform.h"
#include <math.h>

namespace tp {

static inline RGB lightMul(RGB albedo, RGB light) {
  return RGB{ albedo.r * light.r / 255.0f, albedo.g * light.g / 255.0f,
              albedo.b * light.b / 255.0f };
}

// Lit colour of one material level, before haze. *hazeK receives how much
// of the layer's haze applies (emissive light cuts through it).
static RGB shadeLit(const Material &m, float s, const LayerLook &look, const SkyState &sky,
                    float *hazeKOut) {
  RGB c;
  float hazeK = 1.0f;     // emissive things punch through haze a little
  switch (m.kind) {
    case MAT_SLOPE_L:
    case MAT_SLOPE_R: {
      float th = s * 1.22f;                         // 0..70 degrees
      float nx = (m.kind == MAT_SLOPE_L ? -1.0f : 1.0f) * sinf(th);
      float ny = cosf(th);
      float ndl = nx * sky.lightX + ny * sky.lightY;
      if (ndl < 0) ndl = 0;
      RGB amb = scale(sky.ambient, 0.80f + 0.20f * ny);
      // direct sun on faces turned toward it, plus a little bounce light so
      // shaded faces stay warm instead of going dead grey
      RGB light = add(amb, scale(sky.sunLight, 0.62f * ndl + 0.10f));
      c = lightMul(m.albedo, light);
      if (m.sheen > 0) c = mix(c, sky.zenith, m.sheen * (0.4f + 0.6f * sky.night));
      break;
    }
    case MAT_RAMP:
    case MAT_ACCENT: {
      RGB base = mix(m.albedoDark, m.albedo, s);
      RGB light = add(sky.ambient, scale(sky.sunLight, 0.42f));
      c = lightMul(base, light);
      if (m.kind == MAT_ACCENT) c = saturate(c, 1.0f + 0.4f * sky.night);
      break;
    }
    case MAT_WINDOWS: {
      RGB light = add(sky.ambient, scale(sky.sunLight, 0.30f));
      RGB glass = mix(lightMul(m.albedo, light), sky.horizon, 0.18f);
      uint32_t h = hash32((uint32_t)(s * 62.0f + 0.5f) * 2654435761u + 17u);
      bool lit = s < sky.windowsLit;
      RGB warm = mix(rgb(255, 196, 112), rgb(255, 236, 196), hashUnit(h));
      warm = scale(warm, 0.78f + 0.22f * hashUnit(h >> 7));
      RGB nightC = lit ? warm : lightMul(m.albedoDark, add(sky.ambient, rgb(10, 10, 14)));
      c = mix(glass, nightC, smoothstepf(0.15f, 0.85f, sky.night));
      if (lit) hazeK = 0.45f;
      break;
    }
    case MAT_LAMP: {
      RGB light = add(sky.ambient, scale(sky.sunLight, 0.42f));
      RGB day = lightMul(m.albedo, light);
      RGB glowC = mix(rgb(255, 214, 140), rgb(255, 248, 220), s);
      c = mix(day, glowC, smoothstepf(0.05f, 0.6f, sky.night));
      hazeK = 0.4f;
      break;
    }
    case MAT_WATER: {
      RGB light = add(sky.ambient, scale(sky.sunLight, 0.30f));
      RGB deep = lightMul(m.albedo, light);
      c = mix(deep, mix(sky.horizon, sky.zenith, 0.3f), s);
      break;
    }
    default:
      c = rgb(255, 0, 255);
  }
  c = scale(c, look.exposure);
  // Night floor: lift very dark silhouettes toward the sky's zenith colour so
  // the landscape still reads against the night sky.
  if (look.nightFloor > 0 && sky.night > 0) {
    c = mix(c, add(c, scale(sky.zenith, 0.5f)), look.nightFloor * sky.night);
  }
  *hazeKOut = hazeK;
  return c;
}

static inline RGB applyHaze(RGB c, float haze, const SkyState &sky) {
  float hz = sat(haze);
  c = mix(c, sky.haze, hz);
  return saturate(c, 1.0f - 0.35f * hz);   // distant things lose saturation
}

RGB shadeMaterial(const Material &m, float s, float haze, const LayerLook &look,
                  const SkyState &sky) {
  float hk = 1.0f;
  RGB c = shadeLit(m, s, look, sky, &hk);
  return applyHaze(c, haze * hk, sky);
}

void buildLuts(Layer &L, const LayerLook &look, const SkyState &sky) {
  if (L.kind != LAYER_INDEXED || !L.luts) return;
  // Lighting once per material level, then only haze per row band. Scratch
  // lives on the heap: this runs on the render task's modest stack.
  struct Scratch { RGB lit[252]; float hk[252]; };
  Scratch *sc = (Scratch *)allocBig(sizeof(Scratch));
  if (!sc) return;
  RGB *lit = sc->lit;
  float *hk = sc->hk;
  for (int m = 0; m < 4; m++) {
    const Material &mat = look.mats[m];
    for (int lv = 0; lv < 63; lv++) {
      int i = m * 63 + lv;
      if (mat.kind == MAT_UNUSED) { lit[i] = rgb(255, 0, 255); hk[i] = 0; continue; }
      lit[i] = shadeLit(mat, lv / 62.0f, look, sky, &hk[i]);
    }
  }
  for (int b = 0; b < L.nBands; b++) {
    float bf = (L.nBands <= 1) ? 0.0f : (b + 0.5f) / L.nBands;
    float haze = look.haze + look.mist * bf * bf;
    uint16_t *lut = L.luts + (b << 8);
    lut[0] = 0;
    for (int i = 0; i < 252; i++) lut[1 + i] = to565(applyHaze(lit[i], haze * hk[i], sky));
    for (int i = 253; i < 256; i++) lut[i] = 0xF81F;
  }
  freeMem(sc);
}

}  // namespace tp
