// View interface + concrete views. Views render from the model and consume
// events the controller posts. Switching is done by calling switchTo(Screen).
#pragma once
#include "model.h"
#include "event.h"

class View {
public:
  virtual ~View() = default;
  virtual void onEnter() {}
  virtual void onExit()  {}
  virtual void render()  = 0;
  virtual void onEvent(const Event &) {}

  // Frame pacing hint for the render task (Tilt Parallax addition, same idea
  // as EWatchOS2.1):
  //   0  -> event-driven (default): render() runs on events / model changes
  //         plus a 1 s heartbeat, exactly as in stock BaseOS.
  //   >0 -> animated: render() is also called every N ms with no events.
  // Views may change their answer at any time (e.g. only while moving).
  virtual uint16_t desiredFrameMs() const { return 0; }
};

extern View *currentView;
void  viewsInit();
void  switchTo(Screen s);
View *viewFor(Screen s);

// Backlight gate (Tilt Parallax addition): the watch face calls
// uiMarkFirstFrame() once its first complete frame is on the panel, and
// setup() waits for it (bounded) before turning the backlight on, so every
// wake shows a finished face instead of a half-drawn one.
void uiMarkFirstFrame();
bool uiFirstFrameShown();

// Watch face style picker — defined alongside WatchFaceView in view.cpp.
// The web settings page and the on-watch Display settings both use these to
// render and validate the dropdown / row picker.
int          watchFaceStyleCount();
const char  *watchFaceStyleName(int idx);

// Theme helpers — defined in view.cpp, used by every view so a Settings →
// Display change repaints the whole UI on the next render.
struct ThemeColors { uint16_t bg, fg, accent, line; };
ThemeColors  theme();
uint16_t     contrastFor(uint16_t bg);     // WHITE/BLACK best against bg
void         drawBackButton();              // top-left chevron (accent bg)
void         drawTitleBar(const char *title,
                          int16_t titleY = 14, int16_t lineY = 46,
                          int16_t lineX = 20, int16_t lineW = 200);
bool         tappedBack(uint16_t x, uint16_t y);
