// Rep Counter: render the app's screens on the host for review and for the
// marketplace screenshots. Writes one PPM per state into the output folder;
// tools/previews.py turns them into PNGs (and a contact sheet).
//
//   build/host/rep_preview build/previews
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "rep_gfx.h"
#include "rep_session.h"
#include "rep_ui.h"

static uint16_t fb[240 * 280];

static void save(const std::string &dir, const std::string &name, const repui::UiState &u) {
  rgfx::Surface s(fb, 240, 280);
  repui::render(s, u);
  std::string path = dir + "/" + name + ".ppm";
  FILE *f = std::fopen(path.c_str(), "wb");
  if (!f) { std::perror(path.c_str()); return; }
  std::fprintf(f, "P6\n240 280\n255\n");
  for (int i = 0; i < 240 * 280; i++) {
    uint16_t c = fb[i];
    uint8_t px[3] = {rgfx::red8(c), rgfx::green8(c), rgfx::blue8(c)};
    std::fwrite(px, 1, 3, f);
  }
  std::fclose(f);
  std::printf("%s\n", path.c_str());
}

int main(int argc, char **argv) {
  std::string dir = argc > 1 ? argv[1] : "build/previews";
  using reps::Phase;

  static reps::DayLog log;
  log.year = 2026; log.month = 10; log.day = 1;
  struct { reps::Exercise e; uint8_t r; uint16_t t; } sets[] = {
    {reps::Exercise::Curl, 12, 240}, {reps::Exercise::Curl, 11, 255}, {reps::Exercise::Curl, 10, 262},
    {reps::Exercise::Press, 8, 305}, {reps::Exercise::Press, 8, 311}, {reps::Exercise::Raise, 12, 228},
    {reps::Exercise::Row, 10, 270},
  };
  for (auto &x : sets) {
    reps::SetRecord r;
    r.exercise = (uint8_t)x.e; r.reps = x.r; r.tempoCs = x.t; r.durationS = (uint16_t)(x.r * x.t / 100);
    log.sets[log.count++] = r;
  }
  static reps::History hist;
  uint16_t past[] = {64, 0, 88, 52, 0, 71};
  for (int i = 0; i < 6; i++) {
    hist.days[i].year = 2026; hist.days[i].month = 9; hist.days[i].day = (uint8_t)(30 - i);
    hist.days[i].reps = past[i]; hist.days[i].sets = past[i] / 10;
  }
  hist.n = 6;

  repui::UiState base;
  base.clockValid = true; base.hour = 18; base.minute = 42;
  base.log = &log; base.hist = &hist;
  base.todaySets = log.count; base.todayReps = log.totalReps();

  repui::UiState u = base;
  u.phase = Phase::Idle; u.todaySets = 0; u.todayReps = 0;
  save(dir, "01_idle", u);

  u = base; u.phase = Phase::Ready; u.setNo = 1; u.progress = 0.08f;
  save(dir, "02_ready", u);

  u = base; u.phase = Phase::Lifting; u.reps = 1; u.tentative = true; u.exercise = reps::Exercise::Curl;
  u.confidence = 1; u.setNo = 8; u.progress = 0.55f; u.moving = true;
  save(dir, "03_first_rep", u);

  u = base; u.phase = Phase::Lifting; u.reps = 7; u.exercise = reps::Exercise::Curl; u.confidence = 3;
  u.setNo = 8; u.tempoSec = 2.4f; u.progress = 0.72f; u.moving = true;
  save(dir, "04_lifting", u);

  u.pop = 0.9f; u.reps = 8; u.progress = 0.38f;
  save(dir, "05_rep_pop", u);

  u = base; u.phase = Phase::Lifting; u.reps = 12; u.exercise = reps::Exercise::Press; u.confidence = 2;
  u.setNo = 8; u.tempoSec = 3.1f; u.progress = 0.9f; u.moving = true;
  save(dir, "06_press", u);

  u = base; u.phase = Phase::Resting; u.exercise = reps::Exercise::None; u.lastExercise = reps::Exercise::Curl;
  u.setNo = 8; u.restSec = 47; u.restGoalSec = 90; u.lastSet = log.sets[2]; u.lastSetNo = 3;
  u.summary = true; u.summaryAge = 0.3f;
  save(dir, "07_set_done", u);

  u.summary = false;
  save(dir, "08_rest", u);

  u.restSec = 96; u.restGoalReached = true;
  save(dir, "09_rest_goal", u);

  u = base; u.phase = Phase::Paused; u.mode = reps::Mode::Curl;
  save(dir, "10_paused", u);

  u = base; u.page = repui::Page::History;
  save(dir, "11_history", u);

  u = base; u.page = repui::Page::Settings; u.handLearned = true;
  save(dir, "12_settings", u);

  u.clearHold = 0.55f; u.pressedRow = repui::RowClear;
  save(dir, "13_settings_clear", u);

  u = base; u.phase = Phase::Lifting; u.reps = 9; u.exercise = reps::Exercise::Raise; u.confidence = 3;
  u.setNo = 3; u.tempoSec = 2.2f; u.progress = 0.3f; u.mode = reps::Mode::Raise;
  u.theme.bg = 0xFFFF; u.theme.fg = 0x0000; u.theme.accent = 0x001F; u.theme.line = 0xC618;
  save(dir, "14_light_theme", u);

  u = base; u.phase = Phase::Lifting; u.reps = 6; u.exercise = reps::Exercise::Curl; u.confidence = 3;
  u.setNo = 2; u.tempoSec = 2.6f; u.progress = 0.5f; u.recording = true;
  u.toast = "Back again to finish";
  save(dir, "15_toast_rec", u);

  u = base; u.page = repui::Page::History; static reps::DayLog empty; empty.year = 2026; empty.month = 10; empty.day = 1;
  u.log = &empty;
  save(dir, "16_history_empty", u);
  return 0;
}
