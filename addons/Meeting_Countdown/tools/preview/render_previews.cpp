// Host preview renderer: draws every Meeting Countdown screen with the exact
// watch code (lib/meetcore) into 240x280 RGB565 frames and writes them as
// binary PPM files. tools/preview/render.sh builds this, runs it and turns
// the frames into PNGs (plus framed marketplace shots) with Pillow.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include "mc_ui.h"
#include "mc_proto.h"
#include "mc_text.h"

using namespace mc;

static uint16_t fb[240 * 280];
static const int TZ = 60;                                    // UTC+1
static const int64_t NOW = 1790847420;                        // 2026-10-01 10:37:00 local

static void save(const char *dir, const char *name) {
  std::string path = std::string(dir) + "/" + name + ".ppm";
  FILE *f = fopen(path.c_str(), "wb");
  if (!f) { perror(path.c_str()); exit(1); }
  fprintf(f, "P6\n240 280\n255\n");
  for (int i = 0; i < 240 * 280; i++) {
    uint32_t c = rgb888(fb[i]);
    unsigned char px[3] = {(unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c};
    fwrite(px, 1, 3, f);
  }
  fclose(f);
  printf("  %s\n", path.c_str());
}

static Event ev(const char *title, int h, int m, int durMin, const char *loc = "", int leave = 0) {
  Event e;
  const char *err;
  char st[8];
  snprintf(st, sizeof(st), "%02d:%02d", h, m);
  makeEvent(title, "2026-10-01", st, "", durMin, leave, loc, TZ, Source::Feed, (uint32_t)(h * 60 + m), e, &err);
  return e;
}

static FaceModel face(Event *list, int n, int64_t now, const char *status, bool warn = false,
                      float phase = 0) {
  sortEvents(list, n);
  FaceModel m;
  m.now = now;
  m.tz = TZ;
  m.ev = list;
  m.n = n;
  m.leadMin = 60;
  m.fi = computeFace(list, n, now, TZ, 60);
  copyStr(m.status.text, sizeof(m.status.text), status);
  m.status.warn = warn;
  m.phase = phase;
  return m;
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : ".";
  Canvas c{fb, 240, 280};
  int64_t at1037 = NOW;

  {  // Far off: next event in nearly four hours, ring full and calm.
    Event l[] = {ev("Quarterly planning", 14, 30, 90, "Atrium")};
    FaceModel m = face(l, 1, at1037, "synced 4 min ago");
    drawFace(c, m);
    save(dir, "face_far");
  }
  {  // The brief's example: "in 23 min".
    Event l[] = {ev("Design review", 11, 0, 45, "Room 4.12"), ev("Lunch with Priya", 12, 30, 60)};
    FaceModel m = face(l, 2, at1037, "synced 12 min ago");
    drawFace(c, m);
    save(dir, "face_23min");
  }
  {  // Close: amber.
    Event l[] = {ev("Design review", 11, 0, 45, "Room 4.12"), ev("Lunch with Priya", 12, 30, 60)};
    FaceModel m = face(l, 2, at1037 + 14 * 60, "synced 26 min ago");
    drawFace(c, m);
    save(dir, "face_close");
  }
  {  // Imminent: under a minute, red.
    Event l[] = {ev("Board update: Q3 numbers and the hiring plan", 11, 0, 30, "Zoom")};
    FaceModel m = face(l, 1, at1037 + 22 * 60 + 15, "synced 34 min ago");
    drawFace(c, m);
    save(dir, "face_imminent");
  }
  {  // Final minute: the ring zooms to seconds.
    Event l[] = {ev("Design review", 11, 0, 45, "Room 4.12")};
    FaceModel m = face(l, 1, at1037 + 22 * 60 + 38, "synced 34 min ago", false, 0.2f);
    drawFace(c, m);
    save(dir, "face_final_minute");
  }
  {  // Clear, with an all-day event today.
    Event bday;
    const char *err;
    makeEvent("Mum's birthday", "2026-10-01", "", "", 0, 0, "", TZ, Source::Feed, 98, bday, &err);
    Event l[] = {bday};
    FaceModel m = face(l, 1, at1037, "synced 9 min ago");
    m.alertsOn = false;
    drawFace(c, m);
    save(dir, "face_clear_allday");
  }
  {  // In a meeting, with what's next.
    Event l[] = {ev("Sprint planning", 10, 0, 60, "Blue room"), ev("1:1 with Sam", 11, 30, 30)};
    FaceModel m = face(l, 2, at1037, "synced 8 min ago");
    drawFace(c, m);
    save(dir, "face_meeting");
  }
  {  // Clear.
    FaceModel m = face(nullptr, 0, at1037, "synced 3 min ago");
    drawFace(c, m);
    save(dir, "face_clear");
  }
  {  // First run: nothing configured yet.
    FaceModel m = face(nullptr, 0, at1037, "tap to add your calendar");
    drawFace(c, m);
    save(dir, "face_first_run");
  }
  {  // Leave-now buffer.
    Event l[] = {ev("Dentist", 11, 15, 30, "High St Clinic", 30)};
    FaceModel m = face(l, 1, at1037, "synced just now");
    drawFace(c, m);
    save(dir, "face_leave");
  }
  {  // Stale sync warning.
    Event l[] = {ev("Weekly sync", 15, 0, 30)};
    FaceModel m = face(l, 1, at1037, "sync failed - 3 h ago", true);
    drawFace(c, m);
    save(dir, "face_stale");
  }
  {  // Alert.
    AlertModel a;
    a.now = at1037 + 18 * 60;
    a.tz = TZ;
    a.ev = ev("Design review", 11, 0, 45, "Room 4.12");
    a.kind = AlertKind::Before;
    a.snoozeMin = 3;
    drawAlert(c, a);
    save(dir, "alert");
    a.kind = AlertKind::Leave;
    a.ev = ev("Dentist", 11, 15, 30, "High St Clinic", 30);
    a.now = at1037 + 8 * 60;
    a.phase = 0.5f;
    drawAlert(c, a);
    save(dir, "alert_leave");
    a.kind = AlertKind::Before;
    a.ev = ev("All-hands", 11, 0, 60, "Main hall");
    a.now = at1037 + 23 * 60;
    a.more = 1;
    a.pressed = 1;
    drawAlert(c, a);
    save(dir, "alert_now");
  }
  {  // Agenda.
    Event l[] = {ev("Sprint planning", 10, 0, 60, "Blue room"), ev("1:1 with Sam", 11, 30, 30),
                 ev("Lunch with Priya", 12, 30, 60, "Cafe Nero"), ev("Design review", 14, 0, 45, "Room 4.12"),
                 ev("Gym", 18, 0, 60)};
    Event bday;
    const char *err;
    makeEvent("Mum's birthday", "2026-10-01", "", "", 0, 0, "", TZ, Source::Feed, 99, bday, &err);
    Event all[6] = {bday, l[0], l[1], l[2], l[3], l[4]};
    sortEvents(all, 6);
    AgendaModel a;
    a.now = at1037;
    a.tz = TZ;
    a.ev = all;
    a.n = 6;
    copyStr(a.status.text, sizeof(a.status.text), "synced 12 min ago");
    a.bg = 0x000000;
    a.fg = 0xFFFFFF;
    a.accent = 0x000080;
    drawAgenda(c, a);
    save(dir, "agenda");
    a.syncing = true;
    a.syncStep = "Downloading 412 KB...";
    a.phase = 0.3f;
    a.first = 1;
    drawAgenda(c, a);
    save(dir, "agenda_syncing");
    a.syncing = false;
    a.n = 0;
    a.first = 0;
    copyStr(a.status.text, sizeof(a.status.text), "no calendar yet");
    drawAgenda(c, a);
    save(dir, "agenda_empty");
  }
  {  // Settings.
    SettingsModel s;
    s.n = 6;
    s.rows[0].label = "Alert";          copyStr(s.rows[0].value, 28, "5 min before");
    s.rows[1].label = "Ring window";    copyStr(s.rows[1].value, 28, "60 min");
    s.rows[2].label = "Auto sync";      copyStr(s.rows[2].value, 28, "every 30 min");
    s.rows[3].label = "Watch face";     copyStr(s.rows[3].value, 28, "Meeting");
    s.rows[4].label = "Sync now";       s.rows[4].action = true;
    s.rows[5].label = "Test alert";     s.rows[5].action = true;
    s.footer = "Calendars & events from any browser:";
    s.footer2 = "http://192.168.1.42/meet";
    s.accent = 0x000080;
    drawSettings(c, s);
    save(dir, "settings");
  }
  return 0;
}
