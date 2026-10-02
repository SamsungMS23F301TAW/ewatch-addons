// Tilt Parallax — settings page (Settings -> Parallax, or long-press / swipe
// down on the face). Tap a row to cycle its value; changes apply at once and
// are saved to NVS when you leave the page.
#pragma once
#include "view.h"

class ParallaxSettingsView : public View {
public:
  void onEnter() override;
  void onExit() override;
  void render() override;
  void onEvent(const Event &e) override;

private:
  void drawRow(int i);
  bool firstDraw_ = true;
};

// Opens the page and makes "back" return to `back` (the face uses this).
void parallaxSettingsOpen(Screen back);
