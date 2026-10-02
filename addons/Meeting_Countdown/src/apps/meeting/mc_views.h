// Meeting Countdown — the on-watch screens. Each view fills a view model from
// mcapp, draws it with lib/meetcore's renderer into frameCanvas() and flushes
// once, redrawing only when what is visible actually changed.
//
//   MeetingFaceView      Screen::Watch (default face): ring + time + next event
//   MeetingAlertView     Screen::MeetingAlert: the T-5 alert with snooze / OK
//   MeetingAgendaView    Screen::MeetingAgenda: next events, Sync, settings
//   MeetingSettingsView  Screen::MeetingSettings: on-watch options
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "view.h"
#include "mc_event.h"
#include "mc_app.h"

class MeetingFaceView : public View {
public:
  void onEnter() override;
  void onExit() override {}
  void render() override;
  void onEvent(const Event &e) override;
private:
  uint32_t lastSig = 0, lastDraw = 0;
  bool     pressActive = false, pressMoved = false;
  uint16_t pressX = 0, pressY = 0;
  uint32_t pressMs = 0;
};

class MeetingAlertView : public View {
public:
  void onEnter() override;
  void onExit() override;
  void render() override;
  void onEvent(const Event &e) override;
  bool holding() const;            // keeps the watch awake while ringing
private:
  void finish(bool snooze);
  uint32_t startMs = 0, lastDraw = 0;
  int8_t   pressed = -1;
  bool     done = false;
};

class MeetingAgendaView : public View {
public:
  void onEnter() override;
  void render() override;
  void onEvent(const Event &e) override;
private:
  uint32_t lastSig = 0, lastDraw = 0;
  int      first = 0;
  int8_t   pressed = -1;
  uint32_t pressedAt = 0;
  char     note[48] = "";
  uint32_t noteUntil = 0;
};

class MeetingSettingsView : public View {
public:
  void onEnter() override;
  void render() override;
  void onEvent(const Event &e) override;
private:
  uint32_t lastSig = 0;
  int8_t   pressed = -1;
  uint32_t pressedAt = 0;
};

// Classic BaseOS face: the line under the date ("Standup in 23m").
bool mcClassicFaceLine(char *buf, size_t n);
// taskRender's blockSleep OR-chain.
bool mcViewsHoldAwake();
// setup() waits for the first frame before lighting the backlight.
bool mcViewsFirstFrameDone();
