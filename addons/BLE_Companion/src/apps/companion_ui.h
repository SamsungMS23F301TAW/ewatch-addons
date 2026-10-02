// Companion UI — the drawing half of the Companion and Message screens.
//
// Pure C++11 on top of the anti-aliased engine (aa_gfx) and Halo's palette,
// so tools/preview renders exactly what the watch shows. companion.cpp owns
// state, input and pushing pixels; this file only paints a Model.
//
// Visual language (shared with the Halo face): the theme accent is the light
// source. Each state has one hero object: a glossy orb with sonar rings while
// the watch is visible, a draining dial that frames the 6-digit code while
// pairing, a lit orb with a closed ring once connected, a dim glass orb when
// off, a red draining ring with a lock during lockout. Buttons are pills with
// depth (graded fill, lit top edge, coloured glow) or glass.
#pragma once
#include <stdint.h>

#include "aa_gfx.h"
#include "face_halo.h"

namespace cui {

enum Screen : uint8_t { SC_VISIBLE, SC_PAIRING, SC_CONNECTED, SC_OFF, SC_LOCKED, SC_MESSAGE };

struct Rect { int16_t x, y, w, h; };
bool inRect(uint16_t x, uint16_t y, const Rect &r);

// Touch targets (generous; the visuals sit inside them).
extern const Rect kBtnHide, kBtnPairCancel, kBtnShowFace, kBtnEnd, kBtnVisible, kBtnResetFace,
                  kBtnResetLook;

struct Model {
  uint8_t  screen;
  char     device[24];
  uint32_t code;
  uint8_t  attemptsLeft, maxAttempts;
  uint32_t remainMs, totalMs;     // the countdown this screen shows (window, code, lockout, message)
  uint32_t sessionMs, writes;
  char     status[48];            // Off: why it is off; Locked: unused
  char     toast[28];             // empty = none
  uint8_t  hold;                  // 0 none, 1 reset face, 2 reset colours
  uint16_t holdPermille;
  uint8_t  icon;                  // Message: bleproto icon (0 = chat bubble)
  uint8_t  msg[72];               // Message: CP437 glyphs
  uint8_t  msgLen;
  uint32_t animMs;                // animation clock
};

void paint(aa::Surface &s, const halo::Palette &p, const Model &m);

// ---- incremental redraw helpers --------------------------------------------
// Rects whose pixels depend on animMs alone (pulses), for this screen.
int  animRects(const Model &m, Rect *out, int max);
// Box around the moving end of the screen's countdown ring (empty if none).
Rect ringEnd(const Model &m);
// Rect of the countdown / status text line, of the toast, and of the hold
// buttons, for repainting when their values change.
Rect textRect(const Model &m);
Rect toastRect();
Rect holdRect(uint8_t hold);
Rect fullRect();

// The "remaining" text shown for a countdown ("2:41", "41 s left", ...).
void countdownText(const Model &m, char *out, int cap);

}  // namespace cui
