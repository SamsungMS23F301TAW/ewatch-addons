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
  // Animated views: ask the render task for a frame at least every N ms
  // (0 = render only on input / model changes / the 1 s heartbeat).
  virtual uint16_t frameMs() const { return 0; }
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
