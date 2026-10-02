// Pixel Pet views:
//   PetFaceView — the default Screen::Watch: clock, the pet in its little
//                 world, today's steps and progress to the next snack.
//   PetAppView  — Screen::PetApp in the launcher: the pet up close, Today
//                 stats, the week chart and the options.
// Both render with px/scenes into the shared frame canvas. The face flushes
// only the bands that changed (clock / pet / stats).
#pragma once
#include "view.h"
#include "petart.h"
#include "scenes.h"

// Shared one-shot animation + caption state for a pet on screen.
struct PetStageAnim {
  petart::Act act = petart::Act::Idle;
  uint32_t actStart = 0;
  char toast[24] = "";
  uint32_t toastStart = 0;
  uint32_t toastMs = 0;

  void play(petart::Act a, uint32_t now) { act = a; actStart = now; }
  void say(const char *s, uint32_t now, uint32_t ms = 1800);
  void consumeServiceEvents(uint32_t now, const char *petName, bool asleep);
  void expire(uint32_t now);
};

class PetFaceView : public View {
public:
  void onEnter() override;
  void onExit() override;
  void render() override;
  void onEvent(const Event &e) override;
  uint16_t frameMs() const override { return 110; }
private:
  PetStageAnim anim;
  bool     full = true;
  uint32_t lastPetMs = 0;
  uint32_t eggHintMs = 0;          // onboarding caption while it's an egg
  uint32_t topKey = 0xFFFFFFFF, statsKey = 0xFFFFFFFF;
  bool     pressActive = false, pressMoved = false;
  uint16_t pressX = 0, pressY = 0;
  uint32_t pressMs = 0;
};

class PetAppView : public View {
public:
  void onEnter() override;
  void onExit() override;
  void render() override;
  void onEvent(const Event &e) override;
  uint16_t frameMs() const override { return page == scenes::Page::Pet ? 110 : 250; }
private:
  void tap(int x, int y);
  scenes::Page page = scenes::Page::Pet;
  PetStageAnim anim;
  bool     pressActive = false, pressMoved = false;
  uint16_t pressX = 0, pressY = 0;
  uint32_t pressMs = 0;
  bool     resetHeld = false;
  uint32_t resetStart = 0;
  bool     resetDone = false;
};

// Draw the pet (current mood, gently animated) into a launcher tile. `fb` is
// the 240x280 frame canvas buffer; (cx, top) is the centre-top of the icon.
void petDrawLauncherIcon(uint16_t *fb, int cx, int top, uint32_t nowMs);
