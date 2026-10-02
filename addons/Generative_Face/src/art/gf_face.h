// Dayprint face compositor: art + clock/caption overlay -> RGB565.
//
// Two stages so the watch does the expensive part rarely:
//   buildFaceLayer()  renders the text into coverage masks (time, date,
//                     steps, caption) plus a soft halo. Only needed when the
//                     displayed text changes (about once a minute).
//   composeFrame()    blends art + scrim + halo + text, applies the optional
//                     time-of-day light, and ordered-dithers to RGB565. Cheap
//                     enough to run every bloom frame.
// Both are pure C++ and run identically in the host preview tool.
#pragma once
#include <stdint.h>
#include "gf_art.h"
#include "gf_font.h"

namespace gf {

enum FaceMode : uint8_t {
  kModeClock = 0,      // watch face: big time, date, steps
  kModeArt = 1,        // tap on the face: art with a museum-label caption
  kModeGallery = 2,    // gallery page for a recorded day
  kModePlain = 3,      // art only, no overlay
};

struct FaceInputs {
  uint16_t day = 0;          // day being shown (date line / caption)
  uint8_t  hour = 0, minute = 0;
  bool     rtcOk = true;
  bool     timeUnset = false;  // RTC holds an implausible date: show a hint
  uint32_t steps = 0;
  uint32_t goal = 0;           // 0 hides the progress line
  uint8_t  mode = kModeClock;
  bool     batteryLow = false;
  bool     ambient = false;    // tint the art with the time of day
  bool     isToday = false;    // gallery: label the live day "TODAY"
  uint16_t lightPhase = 0;     // slow drifting highlight (0 = off)
  const char *note = nullptr;  // gallery: optional hint along the top
  const char *caption1 = nullptr;  // gallery/art: override the label lines
  const char *caption2 = nullptr;
};

struct FaceLayer {
  uint8_t *maskText = nullptr;   // kW*kH coverage, owned by the caller
  uint8_t *maskHalo = nullptr;   // kW*kH coverage, owned by the caller
  // Two tones: [0] light text on a dark halo/scrim, [1] dark text on light.
  uint32_t textColor[2] = { 0, 0 }, haloColor[2] = { 0, 0 };
  uint8_t  scrim[kH];            // per-row scrim alpha (0..255)
  uint8_t  rowFlags[kH];         // bit0: masks non-empty, bit1: dark-text tone
  uint8_t  tones = 0;            // bit0: top region dark text, bit1: bottom
  uint32_t key = 0;              // fingerprint of what's drawn
  bool     valid = false;
};

// Picks the text tone for the top (time/date) and bottom (steps/caption)
// regions from the art's luminance there, with hysteresis around `prev`
// so a tone only flips when the art clearly calls for it.
uint8_t chooseTones(const Canvas &art, const ArtSpec &spec, uint8_t mode, uint8_t prev);

// Fingerprint of everything that changes the overlay text; rebuild the
// layer when it differs from layer.key.
uint32_t faceKey(const ArtSpec &spec, const FaceInputs &in, uint8_t tones);

void buildFaceLayer(const ArtSpec &spec, const FaceInputs &in, uint8_t tones,
                    FaceLayer &layer);

// Writes rows [y0, y1) of the composed frame as native RGB565; `out` points
// at the slot for row y0 (pass the full framebuffer with y0 = 0).
void composeFrame(const Canvas &art, const ArtSpec &spec, const FaceInputs &in,
                  const FaceLayer &layer, uint16_t *out, int32_t y0, int32_t y1);

// Thousands separator: 8412 -> "8,412". buf needs 16 bytes.
void formatSteps(uint32_t steps, char *buf);

}  // namespace gf
