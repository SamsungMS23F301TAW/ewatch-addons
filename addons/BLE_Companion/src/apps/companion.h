// Companion screens:
//   CompanionView — makes the watch visible to the web page, shows the
//                   6-digit pairing code, session status, and the "reset"
//                   safety nets (hold to confirm).
//   MessageView   — shows a message pushed from the page, then returns to
//                   wherever the user was.
// Drawing lives in companion_ui.* (pure C++, anti-aliased); these views own
// state and input, render into frameCanvas() and push only what changed.
#pragma once
#include "companion_ui.h"
#include "model.h"
#include "view.h"

class CompanionView : public View {
public:
  void onEnter() override;
  void onExit() override;
  void render() override;
  void onEvent(const Event &e) override;

private:
  enum Hold : uint8_t { HOLD_NONE, HOLD_RESET_FACE, HOLD_RESET_THEME };

  void buildModel(cui::Model &m, uint32_t now);
  void doHoldAction();

  cui::Model last_;
  bool       lastValid_ = false;
  uint16_t   lastTheme_[4] = { 0, 0, 0, 0 };
  uint32_t   lastAnimMs_ = 0;
  uint8_t    shownScreen_ = 0xFF;
  uint32_t   stateSinceMs_ = 0;
  bool       touchedSinceState_ = false;
  Hold       hold_ = HOLD_NONE;
  uint32_t   holdStartMs_ = 0;
  bool       holdFired_ = false;
  uint32_t   toastUntilMs_ = 0;
  char       toast_[28] = "";
};

class MessageView : public View {
public:
  static void setReturnScreen(Screen s);
  void onEnter() override;
  void render() override;
  void onEvent(const Event &e) override;

private:
  void close();
  uint32_t   enteredMs_ = 0;
  bool       loaded_ = false;
  cui::Model m_;
  bool       drawn_ = false;
  int        lastRem_ = -1;
  uint16_t   lastTheme_[4] = { 0, 0, 0, 0 };
};
