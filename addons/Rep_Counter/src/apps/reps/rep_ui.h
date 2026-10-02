// Rep Counter: screen layout and drawing.
//
// render() paints one full 240x280 frame from a UiState snapshot; hitTest()
// maps a touch to the control under it using the same layout. No Arduino, no
// globals: the view fills UiState from the session/detector each frame, and
// the host preview tool (tools/host/rep_preview.cpp) fills it by hand.
#pragma once
#include <stdint.h>
#include "rep_gfx.h"
#include "rep_session.h"

namespace repui {

static const int W = 240;
static const int H = 280;

enum class Page : uint8_t { Main = 0, History = 1, Settings = 2 };
static const int kPages = 3;

struct Theme {
  uint16_t bg = 0x0000, fg = 0xFFFF, accent = 0x000F, line = 0x7BEF;
};

// Settings rows, top to bottom.
enum SettingRow : uint8_t {
  RowExercise = 0, RowWrist, RowSetEnd, RowTick, RowRest, RowClear, kSettingRows
};

struct UiState {
  Page page = Page::Main;
  Theme theme;

  // Main page.
  reps::Phase phase = reps::Phase::Idle;
  bool autoPaused = false;
  uint8_t reps = 0;
  bool tentative = false;
  reps::Exercise exercise = reps::Exercise::None;
  reps::Mode mode = reps::Mode::Auto;
  uint8_t confidence = 0;
  uint16_t setNo = 1;              // the set in progress / the next one
  float tempoSec = 0.f;
  uint32_t restSec = 0;
  uint16_t restGoalSec = 90;
  bool restGoalReached = false;
  float progress = 0.f;            // live lift meter 0..1
  bool moving = false;
  float pop = 0.f;                 // 1 right after a rep is counted -> 0
  uint16_t todaySets = 0, todayReps = 0;
  reps::Exercise lastExercise = reps::Exercise::None;
  bool clockValid = false;
  uint8_t hour = 0, minute = 0;
  bool recording = false;          // serial REC streaming

  // Set-done card.
  bool summary = false;
  float summaryAge = 0.f;          // 0..1
  reps::SetRecord lastSet;
  uint16_t lastSetNo = 0;

  const char *toast = nullptr;     // short message pill at the bottom

  // History page.
  const reps::DayLog *log = nullptr;
  const reps::History *hist = nullptr;
  int histScroll = 0;              // first visible row

  // Settings page.
  reps::Settings settings;
  bool handLearned = false;
  float clearHold = 0.f;           // 0..1 progress of the hold-to-clear
  int pressedRow = -1;             // row under the finger (highlight)
};

void render(rgfx::Surface &s, const UiState &u);

enum class Hit : uint8_t {
  None, Back, ModeChip, Center, PageDots,
  Row0, Row1, Row2, Row3, Row4, Row5,     // settings rows
};
Hit hitTest(const UiState &u, int x, int y);

// History list geometry (the view scrolls with swipes).
int historyVisibleRows();

}  // namespace repui
