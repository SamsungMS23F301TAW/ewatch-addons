// Shake Oracle: every tunable in one place.
//
// This header is portable C++ (no Arduino). It is shared by the firmware, the
// host unit tests (pio test -e native) and the host preview renderer
// (tools/oracle_preview.cpp), so a change here is visible in all three.
#pragma once
#include <stdint.h>

namespace oracle {

// ---------------------------------------------------------------- geometry
// Panel is 240 x 280 portrait. The ball fills the width; the window is the
// round glass port in its middle where the liquid and the die live.
constexpr int   kScreenW   = 240;
constexpr int   kScreenH   = 280;
constexpr float kBallCX    = 119.5f;   // pixel centres are at +0.5
constexpr float kBallCY    = 139.5f;
constexpr float kBallR     = 119.0f;
constexpr float kWinR      = 90.0f;    // radius of the visible liquid
constexpr float kBevelW    = 6.0f;     // recessed lip between ball and glass

// Rows the per-frame window pass touches. Only these rows are pushed to the
// panel each frame (full-width rows are contiguous in the canvas).
constexpr int   kWinTop    = (int)(kBallCY - kWinR) - 1;
constexpr int   kWinBottom = (int)(kBallCY + kWinR) + 2;   // exclusive
constexpr int   kWinRows   = kWinBottom - kWinTop;

// The die: an equilateral triangle with rounded corners, pointing up. Local
// units are panel pixels when the die is pressed against the glass (scale 1).
// Pointing up fits the answer set at a slightly larger average size than
// pointing down (measured with the fitter over every answer).
constexpr float kDieR        = 82.0f;  // circumradius
constexpr float kDieCorner   = 9.0f;   // corner rounding radius
constexpr float kDieTextInset = 7.0f;  // text keeps this far from the edges
constexpr bool  kDiePointsDown = false;
// An inscribed upward triangle looks top-heavy, so it rests a little low.
constexpr float kDieRestDy   = 10.0f;

// Text fitting on the die (cap height in pixels at scale 1).
constexpr float kDieCapMax   = 17.0f;
constexpr float kDieCapMin   = 9.0f;   // legibility floor; tests enforce it
constexpr float kDieCondense = 0.85f;  // synthetic condensed caps
constexpr float kDieLineGap  = 0.30f;  // line pitch = cap * (1 + gap)
constexpr int   kDieMaxLines = 4;

// Idle prompt text inside the liquid.
constexpr float kPromptCapMax = 9.5f;
constexpr float kPromptCapMin = 7.0f;

// --------------------------------------------------------------- behaviour
constexpr uint32_t kAwakeSec        = 45;   // min idle timeout while open
constexpr uint32_t kDimLeadMs       = 8000; // dim this long before sleeping
constexpr uint8_t  kDimPercent      = 35;   // backlight level while dimmed
constexpr uint16_t kFramePeriodBusy = 28;   // ~35 fps while things move
constexpr uint16_t kFramePeriodCalm = 50;   // ~20 fps when settled or idle

// Answer picker. Weights are relative: a normal answer weighs kWeightNormal.
constexpr uint16_t kWeightNormal = 8;
constexpr uint16_t kWeightRare   = 1;      // "rare surprise" answers
constexpr uint16_t kGoldenOdds   = 60;     // 1 in N shakes rolls the golden answer

// IMU: BaseOS runs the MMA8451Q at +-2 g, 14-bit, 4096 counts per g.
constexpr float kCountsPerG = 4096.0f;

// Tilt drift. The die is buoyant, so it slides toward the HIGH side of the
// glass, like the bubble in a spirit level. kTiltSignX/Y map accelerometer
// axes to screen axes (+x right, +y down). They match System > IMU Gestures,
// whose dot also moves to the high side; flip a sign here if hardware testing
// shows the die sliding downhill.
constexpr float kTiltSignX     = -1.0f;
constexpr float kTiltSignY     = -1.0f;
constexpr float kTiltDriftPx   = 9.0f;   // max die offset at 1 g in-plane
constexpr float kGlareShiftPx  = 3.0f;   // glass reflection parallax

// ----------------------------------------------------------------- haptics
// hapticBuzz() intensities (0..255, scaled by the user's strength setting).
constexpr uint8_t  kRumbleMin    = 70;
constexpr uint8_t  kRumbleMax    = 140;
constexpr uint16_t kRumblePulseMs = 46;
constexpr uint16_t kRumbleGapMs   = 26;
constexpr uint8_t  kThunkHit     = 255;
constexpr uint16_t kThunkHitMs   = 38;
constexpr uint8_t  kThunkTail    = 90;
constexpr uint16_t kThunkTailMs  = 24;

}  // namespace oracle
