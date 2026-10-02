// Meeting Countdown — screen layouts, drawn from plain view-model structs.
//
// Everything here is pure C++ so the host preview tool renders exactly what
// the watch shows. The device views (src/apps/meeting/mc_views.cpp) fill a
// model, call draw*(), flush the canvas, and use the *Rect() helpers for
// touch hit-testing so drawing and input can never disagree.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "mc_gfx.h"
#include "mc_agenda.h"

namespace mc {

struct UiRect {
  int16_t x, y, w, h;
  bool hit(int px, int py, int pad = 0) const {
    return px >= x - pad && px < x + w + pad && py >= y - pad && py < y + h + pad;
  }
};

struct Palette {
  uint32_t bg, text, text2, text3, track, tick;
  uint32_t calm, amber, red, meet;
};
extern const Palette kPal;

// Ring colour for an upcoming deadline, blending calm -> amber -> red.
uint32_t urgencyColor(int64_t remainingSec);

struct StatusLine {
  char text[48] = "";
  bool warn = false;
};

// ---- watch face ----
struct FaceModel {
  int64_t  now = 0;
  int      tz = 0;              // minutes
  bool     rtcOk = true;
  int      leadMin = 60;
  const Event *ev = nullptr;
  int      n = 0;
  FaceInfo fi;
  StatusLine status;            // footer (sync staleness etc.)
  float    phase = 0;           // 0..1 sub-second phase, drives the final-minute pulse
  bool     alertsOn = true;
};
void drawFace(Canvas &c, const FaceModel &m);

// ---- alert (T-5 etc.) ----
struct AlertModel {
  int64_t   now = 0;
  int       tz = 0;
  Event     ev;
  AlertKind kind = AlertKind::Before;
  int       more = 0;           // other events alerting at the same time
  float     phase = 0;          // 0..1 pulse
  int       snoozeMin = 3;
  int8_t    pressed = -1;       // -1 none, 0 snooze, 1 dismiss (pressed look)
  bool      canSnooze = true;
};
void drawAlert(Canvas &c, const AlertModel &m);
UiRect alertSnoozeRect();
UiRect alertDismissRect();

// ---- agenda (next events) ----
struct AgendaModel {
  int64_t  now = 0;
  int      tz = 0;
  const Event *ev = nullptr;
  int      n = 0;
  StatusLine status;
  bool     syncing = false;
  const char *syncStep = "";
  float    phase = 0;
  int      first = 0;             // scroll offset (rows)
  int8_t   pressed = -1;          // 0 back, 1 sync, 2 settings
  uint32_t bg = 0x000000, fg = 0xFFFFFF, accent = 0x000080;
};
void drawAgenda(Canvas &c, const AgendaModel &m);
int    agendaRowsVisible();
int    agendaRowCount(const AgendaModel &m);
UiRect agendaBackRect();
UiRect agendaSyncRect();
UiRect agendaSettingsRect();

// ---- settings list ----
struct SettingsRow {
  const char *label = "";
  char value[28] = "";
  bool action = false;            // a button rather than a value
};
constexpr int kSettingsRows = 6;   // what fits above the footer
struct SettingsModel {
  const char *title = "Meetings";
  SettingsRow rows[kSettingsRows];
  int      n = 0;
  int8_t   pressed = -1;
  const char *footer = "";
  const char *footer2 = "";
  uint32_t bg = 0x000000, fg = 0xFFFFFF, accent = 0x000080;
};
void drawSettings(Canvas &c, const SettingsModel &m);
UiRect settingsRowRect(int i);
UiRect settingsBackRect();

// Shared bits.
void drawDateLine(Canvas &c, int64_t now, int tz, float y, uint32_t rgb);
void drawSpinner(Canvas &c, float cx, float cy, float r, float phase, uint32_t rgb);

}  // namespace mc
