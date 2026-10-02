// Rep Counter: screen layout and drawing. See rep_ui.h.
#include "rep_ui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "FreeSans12pt7b.h"
#include "FreeSansBold12pt7b.h"
#include "rep_font.h"

namespace repui {

using namespace rgfx;
using reps::Exercise;
using reps::Phase;

static const GFXfont *const kBold = &FreeSansBold12pt7b;
static const GFXfont *const kReg = &FreeSans12pt7b;

// ---- palette -------------------------------------------------------------------
// Chrome follows the user's theme; the state colours (lift / rest / done) are
// the app's own and flip to darker shades on a light background.
struct Pal {
  uint16_t bg, fg, dim, faint, track, chip, line, accent, onAccent;
  uint16_t lift, rest, done, ready, warn;
};

static Pal palette(const Theme &t) {
  Pal p;
  p.bg = t.bg;
  p.fg = t.fg;
  p.line = t.line;
  p.dim = blend(t.fg, t.bg, 150);
  p.faint = blend(t.fg, t.bg, 84);
  p.track = blend(t.fg, t.bg, 38);
  p.chip = blend(t.fg, t.bg, 30);
  p.accent = t.accent;
  p.onAccent = luma(t.accent) < 128 ? 0xFFFF : 0x0000;   // like BaseOS contrastFor()
  bool light = luma(t.bg) > 140;
  if (!light) {
    p.lift = rgb(158, 230, 96);
    p.rest = rgb(255, 184, 76);
    p.done = rgb(80, 224, 132);
    p.ready = rgb(80, 196, 255);
    p.warn = rgb(255, 112, 112);
  } else {
    p.lift = rgb(64, 120, 16);
    p.rest = rgb(176, 84, 8);
    p.done = rgb(16, 128, 64);
    p.ready = rgb(8, 104, 160);
    p.warn = rgb(184, 32, 32);
  }
  return p;
}

// ---- layout ----------------------------------------------------------------------
static const int kBarH = 46;          // top bar
static const int kInfoBase = 68;      // info row baseline
static const int kHeroTop = 80;       // hero digits em-box top
static const int kMeterY = 211;       // lift meter centre line
static const int kHintBase = 248;     // footer hint baseline
static const int kDotsY = 270;
static const int kChipX = 48, kChipW = 114, kChipY = 8, kChipH = 30;
static const int kRowY0 = 52, kRowH = 33;
static const int kHistY0 = 80, kHistRowH = 27, kHistRows = 4;

int historyVisibleRows() { return kHistRows; }

// ---- small helpers -----------------------------------------------------------------
static void fmtClock(char *b, size_t n, uint32_t sec) {
  if (sec >= 3600) snprintf(b, n, "%lu:%02lu:%02lu", (unsigned long)(sec / 3600),
                            (unsigned long)(sec / 60 % 60), (unsigned long)(sec % 60));
  else snprintf(b, n, "%lu:%02lu", (unsigned long)(sec / 60), (unsigned long)(sec % 60));
}

static void fmtTempo(char *b, size_t n, float s) {
  snprintf(b, n, "%.1f s/rep", (double)s);
}

// "left  .  right" centred, with a small round dot as the separator (the
// FreeFonts are 7-bit ASCII, so no middle dot glyph).
static void dotPair(Surface &s, const GFXfont *f, int cx, int base, const char *a,
                    const char *b, uint16_t c) {
  int wa = gfxWidth(f, a), wb = gfxWidth(f, b);
  int x = cx - (wa + 18 + wb) / 2;
  gfxText(s, f, x, base, a, c);
  fillCircle(s, x + wa + 9.f, base - 6.f, 2.2f, c);
  gfxText(s, f, x + wa + 18, base, b, c);
}

static void drawBack(Surface &s, const Pal &p) {
  // BaseOS's back button is an accent-coloured pill; this is a compact one.
  fillRoundRect(s, 6, 8, 36, 30, 15, p.accent);
  capsule(s, 27.f, 15.5f, 20.f, 23.f, 2.1f, p.onAccent);
  capsule(s, 20.f, 23.f, 27.f, 30.5f, 2.1f, p.onAccent);
}

static void drawPips(Surface &s, int x, int cy, uint8_t n, uint16_t on, uint16_t off) {
  for (int i = 0; i < 3; i++) fillCircle(s, (float)(x + i * 9), (float)cy, 3.1f, i < n ? on : off);
}

static void drawLock(Surface &s, int x, int y, uint16_t c) {
  // Tiny padlock (manual mode): shackle arc over a rounded body.
  arc(s, x + 5.5f, y + 6.5f, 3.6f, 1.8f, 270.f, 450.f, c);
  fillRect(s, x + 1, y + 6, 2, 3, c);
  fillRect(s, x + 8, y + 6, 2, 3, c);
  fillRoundRect(s, x - 0.5f, y + 8.f, 12.f, 9.f, 2.f, c);
}

static void drawClock(Surface &s, const UiState &u, const Pal &p) {
  int right = 233;
  if (u.recording) {
    fillCircle(s, (float)right - 4.f, 23.f, 4.f, p.warn);
    right -= 14;
  }
  if (!u.clockValid) return;
  char b[8];
  snprintf(b, sizeof b, "%u:%02u", u.hour, u.minute);
  gfxTextRight(s, kReg, right, 31, b, p.dim);
}

static uint16_t phaseColor(const UiState &u, const Pal &p) {
  switch (u.phase) {
    case Phase::Lifting: return p.lift;
    case Phase::Resting: return u.restGoalReached ? p.done : p.rest;
    case Phase::Ready: return p.ready;
    case Phase::Paused: return p.dim;
    default: return p.ready;
  }
}

static void drawChip(Surface &s, const UiState &u, const Pal &p) {
  Exercise ex = u.exercise;
  if (ex == Exercise::None && u.phase == Phase::Resting) ex = u.lastExercise;
  bool manual = u.mode != reps::Mode::Auto;
  const char *label = manual ? reps::modeCaps(u.mode)
                             : (ex != Exercise::None ? reps::exerciseCaps(ex) : "AUTO");
  bool pips = !manual && ex != Exercise::None && (u.phase == Phase::Lifting || u.phase == Phase::Resting);
  int tw = gfxWidth(kBold, label);
  int extra = pips ? 30 : (manual ? 17 : 0);
  int total = tw + extra;
  fillRoundRect(s, kChipX, kChipY, kChipW, kChipH, 15, p.chip);
  int x = kChipX + (kChipW - total) / 2;
  gfxText(s, kBold, x, kChipY + 22, label, p.fg);
  if (pips) {
    uint8_t conf = u.phase == Phase::Lifting ? u.confidence : 3;
    drawPips(s, x + tw + 9, kChipY + 15, conf, phaseColor(u, p), p.track);
  } else if (manual) {
    drawLock(s, x + tw + 6, kChipY + 7, p.dim);
  }
}

static void drawMeter(Surface &s, float frac, uint16_t c, const Pal &p, bool glow) {
  const float x0 = 22.f, x1 = 218.f, r = 4.5f;
  capsule(s, x0, (float)kMeterY, x1, (float)kMeterY, r, p.track);
  if (frac <= 0.f) return;
  if (frac > 1.f) frac = 1.f;
  float xe = x0 + (x1 - x0) * frac;
  if (glow) capsule(s, x0, (float)kMeterY, xe, (float)kMeterY, r + 2.5f, blend(c, p.bg, 70));
  capsule(s, x0, (float)kMeterY, xe, (float)kMeterY, r, c);
}

static void drawPageDots(Surface &s, const UiState &u, const Pal &p) {
  for (int i = 0; i < kPages; i++) {
    bool on = (int)u.page == i;
    fillCircle(s, 108.f + i * 12.f, (float)kDotsY, on ? 3.3f : 2.6f, on ? p.fg : p.faint);
  }
}

static void drawHint(Surface &s, const char *t, const Pal &p) {
  gfxTextCentered(s, kReg, W / 2, kHintBase, t, p.faint);
}

static void drawToast(Surface &s, const char *t, const Pal &p) {
  int tw = gfxWidth(kReg, t);
  int w = tw + 28;
  if (w > 232) w = 232;
  int x = (W - w) / 2;
  fillRoundRect(s, x, 228, w, 30, 15, p.fg);
  gfxTextCentered(s, kReg, W / 2, 249, t, p.bg);
}

// Big count, centred, with the per-rep colour flash.
static void drawCount(Surface &s, uint8_t n, uint16_t c) {
  char b[6];
  snprintf(b, sizeof b, "%u", n);
  const AaFont &f = kFontHero;
  int w = aaWidth(f, b, -2);
  if (w > W - 8) {
    aaTextCentered(s, kFontTimer, W / 2, kHeroTop + 16, b, c, -1);
    return;
  }
  aaText(s, f, (W - w) / 2, kHeroTop, b, c, -2);
}

// ---- main page states ------------------------------------------------------------
static void drawIdle(Surface &s, const UiState &u, const Pal &p) {
  gfxText(s, kBold, 14, kInfoBase, "TODAY", p.dim);
  char b[32];
  if (u.todaySets) {
    snprintf(b, sizeof b, "%u set%s, %u reps", u.todaySets, u.todaySets == 1 ? "" : "s", u.todayReps);
    gfxTextRight(s, kReg, 226, kInfoBase, b, p.dim);
  } else {
    gfxTextRight(s, kReg, 226, kInfoBase, "No sets yet", p.dim);
  }
  // Start button.
  const float cx = 120.f, cy = 142.f;
  fillCircle(s, cx, cy, 56.f, blend(p.ready, p.bg, 46));
  strokeCircle(s, cx, cy, 56.f, 3.f, p.ready);
  fillTriangle(s, cx - 15.f, cy - 24.f, cx - 15.f, cy + 24.f, cx + 26.f, cy, p.ready);
  drawHint(s, "Tap to start", p);
}

static void drawReady(Surface &s, const UiState &u, const Pal &p) {
  char b[24];
  snprintf(b, sizeof b, "SET %u", u.setNo);
  gfxText(s, kBold, 14, kInfoBase, b, p.dim);
  gfxTextRight(s, kReg, 226, kInfoBase, "Listening", p.ready);
  drawCount(s, 0, p.track);
  drawMeter(s, u.progress, p.ready, p, u.moving);
  drawHint(s, "Start lifting", p);
}

static void drawLifting(Surface &s, const UiState &u, const Pal &p) {
  char b[24];
  snprintf(b, sizeof b, "SET %u", u.setNo);
  gfxText(s, kBold, 14, kInfoBase, b, p.dim);
  if (u.tempoSec > 0.2f && !u.tentative) {
    fmtTempo(b, sizeof b, u.tempoSec);
    gfxTextRight(s, kReg, 226, kInfoBase, b, p.dim);
  }
  uint16_t c = u.tentative ? p.dim : p.fg;
  if (u.pop > 0.f && !u.tentative) c = blend(p.lift, p.fg, (uint8_t)(u.pop * 255.f));
  drawCount(s, u.reps, c);
  drawMeter(s, u.progress, p.lift, p, u.moving);
  drawHint(s, u.tentative ? "Keep going..." : "Tap to pause", p);
}

static void drawResting(Surface &s, const UiState &u, const Pal &p) {
  uint16_t rc = u.restGoalReached ? p.done : p.rest;
  char b[32];
  if (!u.summary) {
    gfxText(s, kBold, 14, kInfoBase, "REST", rc);
    snprintf(b, sizeof b, "Next: set %u", u.setNo);
    gfxTextRight(s, kReg, 226, kInfoBase, b, p.dim);
  }
  fmtClock(b, sizeof b, u.restSec);
  const AaFont &f = (u.restSec >= 600) ? kFontLarge : kFontTimer;
  int top = (u.restSec >= 600) ? kHeroTop + 26 : kHeroTop + 14;
  aaTextCentered(s, f, W / 2, top, b, u.restGoalReached ? p.done : p.fg, -1);
  if (u.restGoalSec > 0) {
    drawMeter(s, (float)u.restSec / (float)u.restGoalSec, rc, p, false);
  }
  if (u.summary) {
    // the set-done card already says it
  } else if (u.restGoalReached) {
    gfxTextCentered(s, kReg, W / 2, kHintBase, "Rest goal reached", p.done);
  } else if (u.lastSet.reps) {
    char t[40];
    snprintf(t, sizeof t, "Last: %u %s", u.lastSet.reps,
             reps::exercisePlural((Exercise)u.lastSet.exercise, u.lastSet.reps));
    drawHint(s, t, p);
  } else {
    drawHint(s, "Tap to pause", p);
  }
}

static void drawPaused(Surface &s, const UiState &u, const Pal &p) {
  gfxText(s, kBold, 14, kInfoBase, u.autoPaused ? "PAUSED (IDLE)" : "PAUSED", p.dim);
  const float cx = 120.f, cy = 142.f;
  strokeCircle(s, cx, cy, 56.f, 3.f, p.dim);
  fillRoundRect(s, cx - 20.f, cy - 25.f, 13.f, 50.f, 4.f, p.dim);
  fillRoundRect(s, cx + 7.f, cy - 25.f, 13.f, 50.f, 4.f, p.dim);
  char b[40];
  if (u.todaySets) {
    char c[16];
    snprintf(b, sizeof b, "%u set%s", u.todaySets, u.todaySets == 1 ? "" : "s");
    snprintf(c, sizeof c, "%u reps", u.todayReps);
    dotPair(s, kReg, W / 2, kHintBase - 26, b, c, p.dim);
  }
  drawHint(s, "Tap to resume", p);
}

static void drawSummary(Surface &s, const UiState &u, const Pal &p) {
  // Slide up into place over the first 300 ms.
  float a = u.summaryAge * 5000.f / 300.f;
  if (a > 1.f) a = 1.f;
  float e = 1.f - (1.f - a) * (1.f - a) * (1.f - a);
  int dy = (int)((1.f - e) * 40.f);
  int x = 12, y = 50 + dy, w = 216, h = 172;
  s.clip(0, kBarH, W, H - kBarH);
  fillRoundRect(s, x, y, w, h, 22, blend(p.done, p.bg, 44));
  strokeRoundRect(s, x, y, w, h, 22, 2.5f, p.done);
  char b[32];
  snprintf(b, sizeof b, "SET %u DONE", u.lastSetNo);
  int tw = gfxWidth(kBold, b);
  int tx = (W - tw - 22) / 2;
  // check mark
  capsule(s, tx + 2.f, y + 24.f, tx + 7.f, y + 29.f, 2.2f, p.done);
  capsule(s, tx + 7.f, y + 29.f, tx + 15.f, y + 19.f, 2.2f, p.done);
  gfxText(s, kBold, tx + 22, y + 32, b, p.done);
  snprintf(b, sizeof b, "%u", u.lastSet.reps);
  aaTextCentered(s, kFontLarge, W / 2, y + 44, b, p.fg, -1);
  const char *pl = reps::exercisePlural((Exercise)u.lastSet.exercise, u.lastSet.reps);
  char up[16];
  size_t i = 0;
  for (; pl[i] && i + 1 < sizeof up; i++) up[i] = (char)(pl[i] >= 'a' && pl[i] <= 'z' ? pl[i] - 32 : pl[i]);
  up[i] = 0;
  gfxTextCentered(s, kBold, W / 2, y + 128, up, p.fg);
  char t[24], d[12];
  fmtClock(d, sizeof d, u.lastSet.durationS);
  if (u.lastSet.tempoCs) {
    snprintf(t, sizeof t, "%.1f s/rep", u.lastSet.tempoCs / 100.0);
    dotPair(s, kReg, W / 2, y + 156, t, d, p.dim);
  } else {
    gfxTextCentered(s, kReg, W / 2, y + 156, d, p.dim);
  }
  s.noClip();
}

static void drawMain(Surface &s, const UiState &u, const Pal &p) {
  drawBack(s, p);
  drawChip(s, u, p);
  drawClock(s, u, p);
  switch (u.phase) {
    case Phase::Idle: drawIdle(s, u, p); break;
    case Phase::Ready: drawReady(s, u, p); break;
    case Phase::Lifting: drawLifting(s, u, p); break;
    case Phase::Resting: drawResting(s, u, p); break;
    case Phase::Paused: drawPaused(s, u, p); break;
  }
  if (u.summary) drawSummary(s, u, p);
}

// ---- history page ------------------------------------------------------------------
static uint8_t weekday(uint16_t y, uint8_t m, uint8_t d) {   // 0 = Sunday
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  int yy = y;
  if (m < 3) yy -= 1;
  return (uint8_t)((yy + yy / 4 - yy / 100 + yy / 400 + t[(m - 1) % 12] + d) % 7);
}

static void drawTitle(Surface &s, const char *t, const Pal &p) {
  drawBack(s, p);
  gfxTextCentered(s, kBold, W / 2 + 10, 31, t, p.fg);
}

static void drawHistory(Surface &s, const UiState &u, const Pal &p) {
  drawTitle(s, "TODAY", p);
  const reps::DayLog *log = u.log;
  int n = log ? log->count : 0;
  char b[48];
  if (n) {
    char c[16];
    snprintf(b, sizeof b, "%d set%s", n, n == 1 ? "" : "s");
    snprintf(c, sizeof c, "%u reps", log->totalReps());
    dotPair(s, kReg, W / 2, kInfoBase, b, c, p.dim);
  }
  if (!n) {
    gfxTextCentered(s, kBold, W / 2, 124, "No sets yet", p.dim);
    gfxTextCentered(s, kReg, W / 2, 152, "Sets appear here", p.faint);
  } else {
    int first = u.histScroll;
    if (first > n - kHistRows) first = n - kHistRows;
    if (first < 0) first = 0;
    for (int r = 0; r < kHistRows && first + r < n; r++) {
      int i = first + r;
      const reps::SetRecord &rec = log->sets[i];
      int y = kHistY0 + r * kHistRowH;
      if (r % 2 == 0) fillRoundRect(s, 8, y, 224, kHistRowH - 2, 9, blend(p.fg, p.bg, 16));
      int base = y + 19;
      snprintf(b, sizeof b, "%d", i + 1);
      gfxTextRight(s, kReg, 34, base, b, p.dim);
      gfxText(s, kBold, 44, base, reps::exerciseName((Exercise)rec.exercise), p.fg);
      snprintf(b, sizeof b, "%u", rec.reps);
      gfxTextRight(s, kBold, 160, base, b, p.fg);
      if (rec.tempoCs) {
        snprintf(b, sizeof b, "%.1fs", rec.tempoCs / 100.0);
        gfxTextRight(s, kReg, 224, base, b, p.dim);
      }
    }
    if (n > kHistRows) {
      // Scroll bar.
      float track = (float)(kHistRows * kHistRowH);
      float th = track * kHistRows / n;
      float ty = kHistY0 + (track - th) * first / (n - kHistRows);
      fillRoundRect(s, 234, kHistY0, 3, (int)track, 1.5f, p.track);
      fillRoundRect(s, 234, ty, 3, th, 1.5f, p.dim);
    }
  }
  // Week strip: reps per day, today on the right.
  const reps::History *h = u.hist;
  uint16_t vals[7] = {0};
  uint8_t days[7] = {7, 7, 7, 7, 7, 7, 7};
  int nd = 0;
  if (log && log->year) { vals[nd] = log->totalReps(); days[nd] = weekday(log->year, log->month, log->day); nd++; }
  for (int i = 0; h && i < h->n && nd < 7; i++) {
    vals[nd] = h->days[i].reps;
    days[nd] = weekday(h->days[i].year, h->days[i].month, h->days[i].day);
    nd++;
  }
  uint16_t mx = 1;
  for (int i = 0; i < nd; i++) if (vals[i] > mx) mx = vals[i];
  static const char *const kDay[] = {"S", "M", "T", "W", "T", "F", "S", ""};
  const int baseY = 230, maxH = 26;
  for (int k = 0; k < 7; k++) {
    int i = 6 - k;                       // today at the right
    float x = 33.f + k * 29.f;
    if (i < nd) {
      float hh = 3.f + (maxH - 3.f) * vals[i] / mx;
      fillRoundRect(s, x - 7.f, baseY - hh, 14.f, hh, 3.f, i == 0 ? p.lift : p.dim);
      gfxTextCentered(s, kReg, (int)x, baseY + 22, kDay[days[i]], i == 0 ? p.fg : p.faint);
    } else {
      fillRoundRect(s, x - 7.f, baseY - 3.f, 14.f, 3.f, 1.5f, p.track);
    }
  }
  drawPageDots(s, u, p);
}

// ---- settings page -----------------------------------------------------------------
static void settingValue(const UiState &u, int row, char *b, size_t n) {
  const reps::Settings &st = u.settings;
  switch (row) {
    case RowExercise: snprintf(b, n, "%s", reps::modeName((reps::Mode)st.mode)); break;
    case RowWrist: snprintf(b, n, "%s", st.wrist == (uint8_t)reps::Wrist::Right ? "Right" : "Left"); break;
    case RowSetEnd: snprintf(b, n, "%u s", st.setEndSec); break;
    case RowTick: snprintf(b, n, "%s", st.repTick ? "On" : "Off"); break;
    case RowRest:
      if (!st.restGoalSec) snprintf(b, n, "Off");
      else fmtClock(b, n, st.restGoalSec);
      break;
    case RowClear: snprintf(b, n, "Hold"); break;
    default: b[0] = 0;
  }
}

static const char *const kRowLabel[kSettingRows] = {
  "Exercise", "Wrist", "Set ends after", "Rep tick", "Rest goal", "Clear today",
};

static void drawSettings(Surface &s, const UiState &u, const Pal &p) {
  drawTitle(s, "SETTINGS", p);
  char b[16];
  for (int r = 0; r < kSettingRows; r++) {
    int y = kRowY0 + r * kRowH;
    if (u.pressedRow == r) fillRoundRect(s, 6, y + 1, 228, kRowH - 2, 10, blend(p.fg, p.bg, 22));
    int base = y + 22;
    gfxText(s, kReg, 14, base, kRowLabel[r], r == RowClear ? p.warn : p.fg);
    settingValue(u, r, b, sizeof b);
    int tw = gfxWidth(kBold, b);
    int pw = tw + 22;
    int px = 228 - pw;
    if (r == RowClear) {
      fillRoundRect(s, px, y + 4, pw, kRowH - 8, 12, blend(p.warn, p.bg, 50));
      if (u.clearHold > 0.f) {
        s.clip(px, y + 4, (int)(pw * u.clearHold), kRowH - 8);
        fillRoundRect(s, px, y + 4, pw, kRowH - 8, 12, p.warn);
        s.noClip();
      }
      gfxText(s, kBold, px + 11, base, b, p.fg);
    } else {
      fillRoundRect(s, px, y + 4, pw, kRowH - 8, 12, p.chip);
      gfxText(s, kBold, px + 11, base, b, p.fg);
      if (r == RowWrist && u.handLearned) fillCircle(s, px - 8.f, y + kRowH / 2.f, 3.f, p.done);
    }
    if (r < kSettingRows - 1) fillRect(s, 14, y + kRowH - 1, 212, 1, blend(p.line, p.bg, 90));
  }
  drawPageDots(s, u, p);
}

// ---- entry points ------------------------------------------------------------------
void render(Surface &s, const UiState &u) {
  Pal p = palette(u.theme);
  s.noClip();
  fill(s, p.bg);
  switch (u.page) {
    case Page::Main: drawMain(s, u, p); drawPageDots(s, u, p); break;
    case Page::History: drawHistory(s, u, p); break;
    case Page::Settings: drawSettings(s, u, p); break;
  }
  if (u.toast) drawToast(s, u.toast, p);
}

Hit hitTest(const UiState &u, int x, int y) {
  if (x < 66 && y < 54) return Hit::Back;
  if (u.page == Page::Main) {
    if (y < kBarH && x >= kChipX - 4 && x < kChipX + kChipW + 4) return Hit::ModeChip;
    if (y >= kBarH && y < 236) return Hit::Center;
    if (y >= 258) return Hit::PageDots;
    return Hit::None;
  }
  if (u.page == Page::Settings) {
    if (y >= kRowY0 && y < kRowY0 + kSettingRows * kRowH)
      return (Hit)((int)Hit::Row0 + (y - kRowY0) / kRowH);
    if (y >= 258) return Hit::PageDots;
  }
  if (u.page == Page::History && y >= 258) return Hit::PageDots;
  return Hit::None;
}

}  // namespace repui
