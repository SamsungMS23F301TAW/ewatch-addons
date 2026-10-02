// Friend Radar app view (Screen::FriendRadar).
//
// One View with internal pages: the radar dial (with a detail card), the
// menu, mates list, mate detail, name keyboard, calibration and help. All
// pixels come from the pure renderer in gfx/ drawn into frameCanvas(); this
// file only handles state, input, timing and the radar service.
#pragma once
#include "view.h"

class FriendRadarView : public View {
public:
  void onEnter() override;
  void onExit() override;
  void render() override;
  void onEvent(const Event &e) override;
  int32_t nextFrameInMs() override;
};

// Open the next radar session as an "alert session" (the watch woke from a
// background window for a mate): the animation plays, and with no touch the
// watch goes back to sleep shortly afterwards.
void radarViewBeginAlertSession();
