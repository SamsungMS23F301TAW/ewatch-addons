// Tilt Parallax — time of day: sun, moon, sky colours and terrain lighting.
//
// The sun's altitude comes from the standard solar-elevation formula at an
// assumed latitude (48 deg N), using the RTC's local time and date, so dawn
// and dusk shift with the seasons. The moon's phase is computed from the date
// (synodic month), and it rises roughly 50 minutes later each day. Every
// colour in the scene is derived from the resulting SkyState.
#pragma once
#include <stdint.h>
#include "tp_color.h"
#include "tp_layer.h"

namespace tp {

struct LocalTime {
  int year = 2026, month = 6, day = 21;   // month 1..12
  int hour = 12, minute = 0, second = 0;
};

struct SkyState {
  float tod = 12;              // local hours
  float sunAlt = 45;           // degrees above horizon
  float sunX = 120, sunY = 40; // screen position at zero parallax
  bool  sunUp = true;
  float moonAlt = -10, moonPhase = 0.5f, moonX = 120, moonY = 60;
  float moonLit = 1;           // illuminated fraction 0..1
  bool  moonUp = false;
  bool  morning = true;        // sun east of the meridian
  float night = 0;             // 0 = day, 1 = full night
  float twilight = 0;          // 0..1 strength of sunrise / sunset colour
  float starAmt = 0;           // 0..1 star visibility
  RGB   zenith, horizon;       // sky gradient
  RGB   glow;                  // sun / twilight glow colour
  RGB   sunDisc;
  RGB   ambient;               // sky light on terrain, 255 = x1
  RGB   sunLight;              // direct light on terrain, 255 = x1
  float lightX = 0, lightY = 1;// unit direction TO the key light (x right, y up)
  RGB   haze;                  // atmospheric perspective colour
  float windowsLit = 0;        // city: fraction of windows lit
  RGB   text;                  // clock colour (tinted per time of day)
};

// Per-scene sky options.
struct SkyStyle {
  int16_t  horizonY = 170;     // screen y of the horizon at zero parallax
  bool     sea = false;        // draw a reflective sea below the horizon
  float    lightPollution = 0; // warm night glow on the horizon (city)
  float    starDensity = 1;
  bool     milkyWay = false;
  RGB      seaDeep{ 18, 52, 84 };
  uint32_t seed = 1;
};

// Days since 2000-01-01 (fractional).
double daysSince2000(const LocalTime &t);
// Moon phase 0..1 (0 = new, 0.5 = full).
float moonPhaseFor(const LocalTime &t);

void computeSky(const LocalTime &t, const SkyStyle &style, SkyState &out);

// Bright stars that may twinkle: where they are in the sky layer and the
// sky pixels under their 5-pixel "plus", so they can be re-blended cheaply.
struct TwinkleStar {
  int16_t  x, y;          // layer coordinates of the centre pixel
  uint16_t bg[5];         // centre, left, right, up, down (before the star)
  RGB      col;
  float    b;             // base brightness 0..1
  uint8_t  lastLevel;
};
struct TwinkleSet {
  static const int kMax = 12;
  int n = 0;
  TwinkleStar s[kMax];
};

// Renders the sky (gradient, glow, stars, moon, sun, optional sea) into an
// RGB layer whose geometry (x0,y0,w,h) the caller has already set. When
// `tw` is given it receives the brightest unobstructed stars.
void renderSky(Layer &sky, const SkyState &s, const SkyStyle &style, TwinkleSet *tw = nullptr);

}  // namespace tp
