// Gallery: browse the collection. Page 0 is today (live steps); each older
// page is a recorded day, re-rendered from its record (date, final steps,
// algorithm version) and blooming in. Gestures:
//   swipe up      an earlier day          swipe down   a later day
//   tap           hide / show the label   long press   this week's remix
//   button or swipe right   back
#pragma once
#include <stdint.h>
#include "view.h"

class GalleryView : public View {
 public:
  void onEnter() override;
  void onExit() override;
  void render() override;
  void onEvent(const Event &e) override;
  static void openFromFace(bool fromFace);

 private:
  void select(int32_t idx);
  int32_t  idx_ = 0, count_ = 1;
  uint16_t day_ = 0;
  uint32_t steps_ = 0;
  uint8_t  algo_ = 1;
  bool     remix_ = false;          // showing the weekly remix page
  uint8_t  tones_ = 0;
  bool     caption_ = true;
  bool     restart_ = true;
  bool     needCompose_ = true;
  uint32_t lastComposeMs_ = 0;
  bool     pressActive_ = false, pressMoved_ = false;
  uint16_t pressX_ = 0, pressY_ = 0;
  uint32_t pressMs_ = 0, pendingTapMs_ = 0;
};
