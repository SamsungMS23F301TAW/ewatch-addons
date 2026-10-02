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
  // Continuous animation: the render() cadence this view wants, in ms, even
  // when nothing else changed. taskRender waits for events no longer than
  // that. 0 (default) keeps the classic dirty-driven loop (render on model
  // change, at least once a second, waiting up to 50 ms for events).
  virtual uint16_t framePeriodMs() const { return 0; }
  // Minimum idle-to-sleep timeout while this view is active, in seconds. The
  // user's own Settings > Sleep timeout wins if it is longer, and 0 there
  // (never sleep) still means never. 0 here (default) = no override.
  virtual uint16_t minAwakeSec() const { return 0; }
};

extern View *currentView;
void  viewsInit();
void  switchTo(Screen s);
View *viewFor(Screen s);

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
