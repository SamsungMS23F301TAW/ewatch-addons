#include "mc_ui.h"
#include "mc_text.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#ifndef PROGMEM
#define PROGMEM   // ESP32 keeps const data in flash anyway; the host has no PROGMEM
#endif
#include "FreeSansBold24pt7b.h"
#include "FreeSans24pt7b.h"

namespace mc {

const Palette kPal = {
  0x000000,   // bg
  0xF2F4F7,   // text
  0xA7B1BC,   // text2
  0x6C7783,   // text3
  0x1A2028,   // track
  0x3B4550,   // tick
  0x2CD9A8,   // calm (mint)
  0xFFB224,   // amber
  0xFF4B4B,   // red
  0x9583FF,   // meeting (violet)
};

static const GFXfont *const BOLD = &FreeSansBold24pt7b;
static const GFXfont *const REG = &FreeSans24pt7b;
static const float kW = 240;
static const float kPi = 3.14159265f;

// Type scale (cap heights from the 24 pt sources: 34 px at 1.0).
static const float S_DATE = 0.30f;     // ~10 px caps, letter-spaced
static const float S_BIG = 0.62f;      // ~21 px
static const float S_TITLE = 0.47f;    // ~16 px
static const float S_BODY = 0.37f;     // ~13 px
static const float S_SMALL = 0.32f;    // ~11 px
static const float S_TINY = 0.29f;     // ~10 px

// ---------------------------------------------------------------------------
// colour
// ---------------------------------------------------------------------------
static void toHsv(uint32_t c, float &h, float &s, float &v) {
  float r = (float)((c >> 16) & 0xFF) / 255.f, g = (float)((c >> 8) & 0xFF) / 255.f,
        b = (float)(c & 0xFF) / 255.f;
  float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
  v = mx;
  s = mx > 0 ? d / mx : 0;
  if (d <= 0) { h = 0; return; }
  if (mx == r) h = fmodf((g - b) / d, 6.f);
  else if (mx == g) h = (b - r) / d + 2.f;
  else h = (r - g) / d + 4.f;
  h *= 60.f;
  if (h < 0) h += 360.f;
}

static uint32_t fromHsv(float h, float s, float v) {
  float c = v * s, x = c * (1 - fabsf(fmodf(h / 60.f, 2.f) - 1)), m = v - c;
  float r, g, b;
  if (h < 60) { r = c; g = x; b = 0; }
  else if (h < 120) { r = x; g = c; b = 0; }
  else if (h < 180) { r = 0; g = c; b = x; }
  else if (h < 240) { r = 0; g = x; b = c; }
  else if (h < 300) { r = x; g = 0; b = c; }
  else { r = c; g = 0; b = x; }
  auto q = [](float f) {
    int i = (int)lrintf(f * 255.f);
    return (uint32_t)(i < 0 ? 0 : i > 255 ? 255 : i);
  };
  return (q(r + m) << 16) | (q(g + m) << 8) | q(b + m);
}

static uint32_t hsvMix(uint32_t a, uint32_t b, float t) {
  float h1, s1, v1, h2, s2, v2;
  toHsv(a, h1, s1, v1);
  toHsv(b, h2, s2, v2);
  float dh = h2 - h1;
  if (dh > 180) dh -= 360;
  if (dh < -180) dh += 360;
  float h = h1 + dh * t;
  if (h < 0) h += 360;
  if (h >= 360) h -= 360;
  return fromHsv(h, s1 + (s2 - s1) * t, v1 + (v2 - v1) * t);
}

uint32_t urgencyColor(int64_t remainingSec) {
  float u = urgency(remainingSec);
  if (u <= 1.f) return hsvMix(kPal.calm, kPal.amber, u);
  return hsvMix(kPal.amber, kPal.red, u - 1.f);
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
static void centered(Canvas &c, const GFXfont *f, const char *s, float y, float scale, uint32_t rgb,
                     float tracking = 0) {
  drawTextAligned(c, f, s, kW / 2, y, scale, rgb, Align::Center, tracking);
}

static void centeredFit(Canvas &c, const GFXfont *f, const char *s, float y, float scale,
                        uint32_t rgb, float maxW) {
  char buf[96];
  ellipsize(f, s, scale, maxW, buf, sizeof(buf));
  centered(c, f, buf, y, scale, rgb);
}

// Word-wraps `s` into at most `maxLines` centred lines; returns lines used.
static int wrapCentered(Canvas &c, const GFXfont *f, const char *s, float y, float lineH,
                        float scale, uint32_t rgb, float maxW, int maxLines, bool draw = true) {
  int lines = 0;
  const char *p = s;
  char buf[96];
  while (*p && lines < maxLines) {
    while (*p == ' ') p++;
    if (!*p) break;
    if (lines == maxLines - 1) {
      ellipsize(f, p, scale, maxW, buf, sizeof(buf));
      if (draw) centered(c, f, buf, y + lineH * (float)lines, scale, rgb);
      lines++;
      break;
    }
    int n = fitText(f, p, scale, maxW, true);
    if (n <= 0) n = 1;
    size_t k = (size_t)n < sizeof(buf) - 1 ? (size_t)n : sizeof(buf) - 1;
    memcpy(buf, p, k);
    buf[k] = '\0';
    if (draw) centered(c, f, buf, y + lineH * (float)lines, scale, rgb);
    lines++;
    p += n;
  }
  return lines;
}

static void timeRange(const Event &e, int tz, char *buf, size_t n) {
  char a[8], b[8];
  formatHHMM(e.start + (int64_t)tz * 60, a, sizeof(a));
  formatHHMM(e.end + (int64_t)tz * 60, b, sizeof(b));
  if (e.end > e.start) snprintf(buf, n, "%s-%s", a, b);
  else snprintf(buf, n, "%s", a);
}

// "10:30-11:00  •  Room 4" centred; the location is shortened to fit.
static void detailLine(Canvas &c, const char *left, const char *loc, float y, uint32_t rgb,
                       float maxW) {
  float s = S_BODY;
  float lw = textWidth(REG, left, s);
  if (!loc || !loc[0]) { centered(c, REG, left, y, s, rgb); return; }
  float gap = 13;
  float room = maxW - lw - gap;
  char lb[40];
  if (room < 34) { centered(c, REG, left, y, s, rgb); return; }
  ellipsize(REG, loc, s, room, lb, sizeof(lb));
  float total = lw + gap + textWidth(REG, lb, s);
  float x = kW / 2 - total / 2;
  drawText(c, REG, left, x, y, s, rgb);
  fillCircle(c, x + lw + gap / 2, y - 4.5f, 1.5f, rgb);
  drawText(c, REG, lb, x + lw + gap, y, s, rgb);
}

void drawDateLine(Canvas &c, int64_t now, int tz, float y, uint32_t rgb) {
  static const char *kWd[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  static const char *kMo[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  Civil cv = secondsToCivil(now + (int64_t)tz * 60);
  char b[24];
  snprintf(b, sizeof(b), "%s %d %s", kWd[cv.weekday], cv.day, kMo[(cv.month + 11) % 12]);
  centered(c, BOLD, b, y, S_DATE, rgb, 1.6f);
}

void drawSpinner(Canvas &c, float cx, float cy, float r, float phase, uint32_t rgb) {
  for (int i = 0; i < 8; i++) {
    float a = (float)i / 8.f * 2 * kPi;
    float k = fmodf((float)i / 8.f - phase + 1.f, 1.f);
    fillCircle(c, cx + r * cosf(a), cy + r * sinf(a), 1.6f, rgb, 0.25f + 0.75f * (1 - k));
  }
}

// The face's ring geometry (shared by face and alert).
static Ring faceRing(float thick) { return makeRing((int)kW, 280, 3, thick, 46); }

static void ringHead(Canvas &c, const Ring &r, float s, uint32_t col) {
  float x, y;
  ringPoint(r, s, x, y);
  fillCircle(c, x, y, r.thick / 2 + 1.5f, col);
  fillCircle(c, x, y, r.thick / 2 - 2.5f, mixRgb(col, 0xFFFFFF, 0.75f));
}

static void drawClock(Canvas &c, const FaceModel &m, float top, float height) {
  if (!m.rtcOk) {
    centered(c, BOLD, "--:--", top + height - 4, 1.3f, kPal.red);
    return;
  }
  Civil cv = secondsToCivil(m.now + (int64_t)m.tz * 60);
  char t[8];
  snprintf(t, sizeof(t), "%02d:%02d", cv.hour, cv.minute);
  float w = digitsWidth(t, height);
  drawDigits(c, t, kW / 2 - w / 2, top, height, 0.15f, kPal.text);
}

// ---------------------------------------------------------------------------
// face
// ---------------------------------------------------------------------------
void drawFace(Canvas &c, const FaceModel &m) {
  clear(c, kPal.bg);
  const FaceInfo &fi = m.fi;
  Ring ring = faceRing(12);
  const float maxW = 196;

  // ---- ring ----
  uint32_t col = kPal.calm;
  if (fi.mode == FaceMode::Upcoming) {
    col = urgencyColor(fi.remaining);
    if (fi.finalMinute) {
      // Final minute: the ring refills and sweeps away with the seconds,
      // pulsing; each track dot is now 5 seconds.
      col = scaleRgb(col, 0.70f + 0.30f * (0.5f + 0.5f * cosf(m.phase * 2 * kPi)));
      float s0 = 1.0f - fi.minuteRing;
      drawRing(c, ring, s0, 1.0f, col, mixRgb(kPal.track, kPal.red, 0.18f), 12, kPal.tick);
      ringHead(c, ring, s0, col);
    } else {
      float s0 = 1.0f - fi.ring;
      drawRing(c, ring, s0, 1.0f, col, kPal.track, 12, kPal.tick);
      if (fi.ring > 0.002f && fi.ring < 0.998f) ringHead(c, ring, s0, col);
    }
  } else if (fi.mode == FaceMode::InMeeting) {
    col = kPal.meet;
    drawRing(c, ring, 0.0f, fi.ring, col, kPal.track, 12, kPal.tick);
    if (fi.ring > 0.002f && fi.ring < 0.998f) ringHead(c, ring, fi.ring, col);
  } else {
    drawRing(c, ring, 0.0f, 1.0f, mixRgb(kPal.track, kPal.calm, 0.38f), kPal.track, 0, 0);
  }

  // ---- date + time ----
  drawDateLine(c, m.now, m.tz, 42, kPal.text3);
  drawClock(c, m, 54, 56);

  // ---- body ----
  char line[112], buf[48];
  if (fi.mode == FaceMode::Clear) {
    centered(c, BOLD, "All clear", 154, S_BIG, kPal.calm);
    if (fi.nAllDay > 0) {
      snprintf(line, sizeof(line), "Today: %s", m.ev[fi.allDay[0]].title);
      wrapCentered(c, REG, line, 184, 19, S_BODY, kPal.text2, maxW, 2);
    } else {
      centered(c, REG, "No meetings coming up", 184, S_BODY, kPal.text2);
    }
  } else {
    const Event &e = fi.mode == FaceMode::InMeeting ? m.ev[fi.cur] : m.ev[fi.next];
    // Headline: the countdown, coloured like the ring.
    if (fi.mode == FaceMode::InMeeting) {
      fmtDuration(fi.remaining, buf, sizeof(buf));
      snprintf(line, sizeof(line), "%s left", buf);
    } else if (fi.leave) {
      fmtDuration(fi.remaining, buf, sizeof(buf));
      snprintf(line, sizeof(line), "leave in %s", buf);
    } else if (fi.leaveNow) {
      snprintf(line, sizeof(line), "leave now");
    } else if (fi.remaining > 12 * 3600) {
      fmtWhen(e.start, m.now, m.tz, line, sizeof(line));
    } else {
      fmtCountdown(fi.remaining, line, sizeof(line));
    }
    // Title (1-2 lines) + detail, vertically balanced in the band 118..232.
    int titleLines = wrapCentered(c, BOLD, e.title, 0, 21, S_TITLE, kPal.text, maxW, 2, false);
    float blockH = 27 + 21.0f * (float)titleLines + 20;
    float y = 118 + (114 - blockH) / 2 + 22;
    centered(c, BOLD, line, y, S_BIG, col);
    y += 27;
    wrapCentered(c, BOLD, e.title, y, 21, S_TITLE, kPal.text, maxW, 2);
    y += 21.0f * (float)titleLines;
    char range[24];
    if (fi.mode == FaceMode::InMeeting) {
      char hm[8];
      formatHHMM(e.end + (int64_t)m.tz * 60, hm, sizeof(hm));
      snprintf(range, sizeof(range), "until %s", hm);
    } else if (fi.leave || fi.leaveNow) {
      char hm[8];
      formatHHMM(e.start + (int64_t)m.tz * 60, hm, sizeof(hm));
      snprintf(range, sizeof(range), "starts %s", hm);
    } else {
      timeRange(e, m.tz, range, sizeof(range));
    }
    detailLine(c, range, e.location, y, kPal.text2, maxW);
  }

  // ---- "then" line: what comes after ----
  const float yThen = 245;
  int thenIdx = -1;
  if (fi.mode == FaceMode::InMeeting && fi.next >= 0) thenIdx = fi.next;
  else if (fi.mode == FaceMode::Upcoming && fi.later >= 0) thenIdx = fi.later;
  if (fi.mode == FaceMode::Upcoming && fi.sameStart > 0) {
    snprintf(line, sizeof(line), "+%d more at the same time", fi.sameStart);
    centeredFit(c, REG, line, yThen, S_SMALL, kPal.text3, 184);
  } else if (thenIdx >= 0) {
    const Event &t = m.ev[thenIdx];
    char when[16];
    fmtWhen(t.start, m.now, m.tz, when, sizeof(when));
    snprintf(line, sizeof(line), "%s %s  %s", fi.mode == FaceMode::InMeeting ? "next" : "then",
             when, t.title);
    centeredFit(c, REG, line, yThen, S_SMALL, kPal.text3, 184);
  } else if (fi.mode != FaceMode::Clear && fi.nAllDay > 0) {
    snprintf(line, sizeof(line), "today: %s", m.ev[fi.allDay[0]].title);
    centeredFit(c, REG, line, yThen, S_SMALL, kPal.text3, 184);
  }

  // ---- status footer ----
  if (m.status.text[0]) {
    uint32_t sc = m.status.warn ? kPal.amber : mixRgb(kPal.text3, kPal.bg, 0.15f);
    centeredFit(c, REG, m.status.text, 260, S_TINY, sc, 150);
  }
  if (!m.alertsOn) {
    // Muted alerts: a small bell with a slash above the date.
    float bx = kW / 2, by = 24;
    fillRoundRect(c, bx - 4, by - 5, 8, 8, 3.5f, kPal.text3);
    fillRect(c, (int)(bx - 6), (int)(by + 1), 12, 2, kPal.text3);
    fillCircle(c, bx, by + 4.5f, 1.4f, kPal.text3);
    fillCapsule(c, bx - 7, by - 6, bx + 7, by + 6, 1.0f, kPal.bg);
    fillCapsule(c, bx - 6, by - 6, bx + 6, by + 5, 0.7f, kPal.text3);
  }
}

// ---------------------------------------------------------------------------
// alert
// ---------------------------------------------------------------------------
static const UiRect kSnooze = {30, 206, 86, 44};
static const UiRect kDismiss = {124, 206, 86, 44};
UiRect alertSnoozeRect() { return kSnooze; }
UiRect alertDismissRect() { return kDismiss; }

static void button(Canvas &c, const UiRect &r, const char *label, uint32_t fill, uint32_t text,
                   bool pressed) {
  uint32_t f = pressed ? mixRgb(fill, 0xFFFFFF, 0.25f) : fill;
  fillRoundRect(c, (float)r.x, (float)r.y, (float)r.w, (float)r.h, (float)r.h / 2, f);
  drawTextAligned(c, BOLD, label, (float)r.x + (float)r.w / 2, (float)r.y + (float)r.h / 2 + 6.5f,
                  0.40f, text, Align::Center);
}

void drawAlert(Canvas &c, const AlertModel &m) {
  int64_t remaining = m.ev.start - m.now;
  uint32_t col = m.kind == AlertKind::Leave ? kPal.amber : urgencyColor(remaining > 0 ? remaining : 0);
  float k = 0.5f + 0.5f * cosf(m.phase * 2 * kPi);
  clear(c, mixRgb(kPal.bg, col, 0.10f + 0.06f * k));
  Ring ring = faceRing(14);
  drawRing(c, ring, 0, 1, scaleRgb(col, 0.70f + 0.30f * k), kPal.track, 0, 0);

  const float maxW = 192;
  char head[64];
  if (m.kind == AlertKind::Leave) snprintf(head, sizeof(head), "TIME TO LEAVE");
  else if (remaining <= 30) snprintf(head, sizeof(head), remaining < -30 ? "STARTED" : "STARTING");
  else snprintf(head, sizeof(head), "STARTS IN");
  if (m.more > 0) {
    size_t l = strlen(head);
    snprintf(head + l, sizeof(head) - l, "  +%d MORE", m.more);
  }
  centered(c, BOLD, head, 50, S_DATE, kPal.text2, 1.8f);

  // Hero: minutes as clock digits ("5 min"), or a word.
  if (m.kind == AlertKind::Leave) {
    centered(c, BOLD, "Leave now", 100, 0.80f, kPal.text);
  } else if (remaining <= 30) {
    centered(c, BOLD, remaining < -30 ? "Now on" : "Now", 100, 0.95f, kPal.text);
  } else {
    char big[16], unit[8];
    int64_t mins = (remaining + 59) / 60;
    if (mins >= 60) {
      snprintf(big, sizeof(big), "%d:%02d", (int)(mins / 60), (int)(mins % 60));
      snprintf(unit, sizeof(unit), "h");
    } else {
      snprintf(big, sizeof(big), "%d", (int)mins);
      snprintf(unit, sizeof(unit), "min");
    }
    float dh = 46;
    float dw = digitsWidth(big, dh);
    float uw = textWidth(BOLD, unit, 0.50f);
    float x = kW / 2 - (dw + 8 + uw) / 2;
    drawDigits(c, big, x, 58, dh, 0.16f, kPal.text);
    drawText(c, BOLD, unit, x + dw + 8, 104, 0.50f, kPal.text2);
  }

  int lines = wrapCentered(c, BOLD, m.ev.title, 0, 22, 0.52f, kPal.text, maxW, 2, false);
  bool hasLoc = m.ev.location[0] != '\0';
  float y = 140;
  if (lines == 1) y += 10;
  if (!hasLoc) y += 8;
  wrapCentered(c, BOLD, m.ev.title, y, 22, 0.52f, kPal.text, maxW, 2);
  y += 22.0f * (float)(lines - 1) + 21;
  char range[24];
  timeRange(m.ev, m.tz, range, sizeof(range));
  centered(c, REG, range, y, S_BODY, kPal.text2);
  if (hasLoc) centeredFit(c, REG, m.ev.location, y + 17, S_BODY, kPal.text2, maxW);
  if (m.canSnooze) {
    char sl[16];
    snprintf(sl, sizeof(sl), "+%d min", m.snoozeMin);
    button(c, kSnooze, sl, 0x2A3139, kPal.text, m.pressed == 0);
    button(c, kDismiss, "OK", col, 0x101214, m.pressed == 1);
  } else {
    UiRect wide = {40, kDismiss.y, 160, kDismiss.h};
    button(c, wide, "OK", col, 0x101214, m.pressed == 1);
  }
}

// ---------------------------------------------------------------------------
// agenda
// ---------------------------------------------------------------------------
static const UiRect kBack = {0, 0, 64, 46};
static const UiRect kSync = {176, 0, 64, 46};
static const UiRect kGear = {188, 232, 52, 48};
static const float kRowTop = 50, kRowH = 44;
static const int kRows = 4;

UiRect agendaBackRect() { return kBack; }
UiRect agendaSyncRect() { return kSync; }
UiRect agendaSettingsRect() { return kGear; }
int agendaRowsVisible() { return kRows; }
int agendaRowCount(const AgendaModel &m) { return m.n; }

static void backChevron(Canvas &c, uint32_t accent, uint32_t fg, bool pressed) {
  uint32_t f = pressed ? mixRgb(accent, fg, 0.3f) : accent;
  fillRoundRect(c, 6, 8, 44, 32, 10, f);
  fillCapsule(c, 31, 16, 23, 24, 1.8f, fg);
  fillCapsule(c, 23, 24, 31, 32, 1.8f, fg);
}

static void syncIcon(Canvas &c, float cx, float cy, uint32_t col) {
  // Two arcs chasing each other, with arrowheads: "refresh".
  for (int half = 0; half < 2; half++) {
    float a0 = -0.4f + (float)half * kPi;
    for (int i = 0; i <= 14; i++) {
      float a = a0 + 0.25f + (float)i / 14.f * (kPi - 0.85f);
      fillCircle(c, cx + 8.5f * cosf(a), cy + 8.5f * sinf(a), 1.35f, col);
    }
    float ae = a0 + 0.25f + (kPi - 0.85f);
    float ex = cx + 8.5f * cosf(ae), ey = cy + 8.5f * sinf(ae);
    float tx = -sinf(ae), ty = cosf(ae);
    fillCapsule(c, ex, ey, ex - 4.5f * tx + 3.5f * cosf(ae), ey - 4.5f * ty + 3.5f * sinf(ae), 1.25f, col);
    fillCapsule(c, ex, ey, ex - 4.5f * tx - 3.5f * cosf(ae), ey - 4.5f * ty - 3.5f * sinf(ae), 1.25f, col);
  }
}

static void gearIcon(Canvas &c, float cx, float cy, uint32_t col, uint32_t bg) {
  for (int i = 0; i < 8; i++) {
    float a = (float)i / 8.f * 2 * kPi;
    fillCapsule(c, cx + 5.5f * cosf(a), cy + 5.5f * sinf(a), cx + 8.5f * cosf(a),
                cy + 8.5f * sinf(a), 1.9f, col);
  }
  fillCircle(c, cx, cy, 6.6f, col);
  fillCircle(c, cx, cy, 2.6f, bg);
}

void drawAgenda(Canvas &c, const AgendaModel &m) {
  clear(c, m.bg);
  uint32_t fg = m.fg, fg2 = mixRgb(m.fg, m.bg, 0.35f), fg3 = mixRgb(m.fg, m.bg, 0.6f);
  uint32_t line = mixRgb(m.fg, m.bg, 0.86f);
  backChevron(c, m.accent, contrastOn(m.accent), m.pressed == 0);
  centered(c, BOLD, "Up next", 31, 0.44f, fg);
  if (m.syncing) drawSpinner(c, 208, 24, 8, m.phase, fg);
  else syncIcon(c, 208, 24, m.pressed == 1 ? m.accent : fg2);

  if (m.n == 0) {
    centered(c, BOLD, "Nothing coming up", 122, 0.44f, fg);
    centered(c, REG, "Add a calendar feed or", 150, S_BODY, fg2);
    centered(c, REG, "events in Settings below", 168, S_BODY, fg2);
  }
  for (int r = 0; r < kRows; r++) {
    int i = m.first + r;
    if (i >= m.n) break;
    const Event &e = m.ev[i];
    float top = kRowTop + kRowH * (float)r;
    bool allDay = isAllDay(e);
    bool now = !allDay && e.start <= m.now && m.now < e.end;
    uint32_t bar;
    char t1[16], t2[24];
    t2[0] = '\0';
    if (allDay) {
      bar = fg3;
      snprintf(t1, sizeof(t1), "All");
      snprintf(t2, sizeof(t2), "day");
    } else if (now) {
      bar = kPal.meet;
      snprintf(t1, sizeof(t1), "Now");
      char d[12];
      fmtDurShort(e.end - m.now, d, sizeof(d));
      snprintf(t2, sizeof(t2), "%s left", d);
    } else {
      bar = urgencyColor(e.start - m.now);
      char when[16];
      fmtWhen(e.start, m.now, m.tz, when, sizeof(when));
      if (strlen(when) > 5) {                        // "Fri 09:30": day on line 2
        copyStr(t1, sizeof(t1), when + 4);
        copyStr(t2, sizeof(t2), when);
        t2[3] = '\0';
      } else {
        copyStr(t1, sizeof(t1), when);
        fmtDurShort(e.start - m.now, t2, sizeof(t2));
      }
    }
    fillRoundRect(c, 10, top + 6, 4, kRowH - 12, 2, bar);
    drawText(c, BOLD, t1, 21, top + 20, 0.40f, allDay ? fg2 : fg);
    drawText(c, REG, t2, 21, top + 36, S_SMALL, allDay ? fg3 : bar);
    char tb[64];
    ellipsize(BOLD, e.title, 0.40f, 150, tb, sizeof(tb));
    drawText(c, BOLD, tb, 80, top + 20, 0.40f, fg);
    char sub[72];
    if (allDay) {
      int days = (int)((e.end - e.start) / kDay);
      if (e.location[0]) snprintf(sub, sizeof(sub), "%s", e.location);
      else if (days > 1) snprintf(sub, sizeof(sub), "%d days", days);
      else snprintf(sub, sizeof(sub), "all day");
    } else {
      char range[24];
      timeRange(e, m.tz, range, sizeof(range));
      if (e.location[0]) snprintf(sub, sizeof(sub), "%s  %s", range, e.location);
      else snprintf(sub, sizeof(sub), "%s", range);
    }
    ellipsize(REG, sub, S_SMALL, 150, tb, sizeof(tb));
    drawText(c, REG, tb, 80, top + 36, S_SMALL, fg2);
    if (r < kRows - 1 && i + 1 < m.n) fillRect(c, 21, (int)(top + kRowH), 209, 1, line);
  }
  // Scroll indicator when the list is longer than the screen.
  if (m.n > kRows) {
    float trackH = kRowH * kRows - 8;
    float thumbH = trackH * (float)kRows / (float)m.n;
    float thumbY = kRowTop + 4 + (trackH - thumbH) * (float)m.first / (float)(m.n - kRows);
    fillRoundRect(c, 234, kRowTop + 4, 3, trackH, 1.5f, line);
    fillRoundRect(c, 234, thumbY, 3, thumbH, 1.5f, fg3);
  }
  // Footer: status on the left, settings gear on the right.
  fillRect(c, 12, 232, 216, 1, line);
  if (m.syncing) {
    drawSpinner(c, 20, 255, 5.5f, m.phase, fg2);
    char st[48];
    ellipsize(REG, m.syncStep, S_TINY, 150, st, sizeof(st));
    drawText(c, REG, st, 32, 259, S_TINY, fg2);
  } else if (m.status.text[0]) {
    char st[48];
    ellipsize(REG, m.status.text, S_TINY, 162, st, sizeof(st));
    drawText(c, REG, st, 14, 259, S_TINY, m.status.warn ? kPal.amber : fg3);
  }
  gearIcon(c, 214, 255, m.pressed == 2 ? m.accent : fg2, m.bg);
}

// ---------------------------------------------------------------------------
// settings
// ---------------------------------------------------------------------------
static const float kSetTop = 50, kSetRowH = 30;
UiRect settingsRowRect(int i) {
  UiRect r = {8, (int16_t)(kSetTop + kSetRowH * (float)i), 224, (int16_t)(kSetRowH - 3)};
  return r;
}
UiRect settingsBackRect() { return kBack; }

void drawSettings(Canvas &c, const SettingsModel &m) {
  clear(c, m.bg);
  uint32_t fg = m.fg, fg2 = mixRgb(m.fg, m.bg, 0.35f);
  backChevron(c, m.accent, contrastOn(m.accent), m.pressed == 100);
  centered(c, BOLD, m.title, 31, 0.44f, fg);
  int rows = m.n < kSettingsRows ? m.n : kSettingsRows;
  for (int i = 0; i < rows; i++) {
    UiRect r = settingsRowRect(i);
    bool pressed = m.pressed == i;
    uint32_t rowBg = pressed ? mixRgb(m.bg, m.fg, 0.20f) : mixRgb(m.bg, m.fg, 0.08f);
    fillRoundRect(c, (float)r.x, (float)r.y, (float)r.w, (float)r.h, 8, rowBg);
    float base = (float)r.y + (float)r.h / 2 + 5;
    if (m.rows[i].action) {
      drawTextAligned(c, BOLD, m.rows[i].label, kW / 2, base, 0.37f, fg, Align::Center);
    } else {
      drawText(c, REG, m.rows[i].label, (float)r.x + 10, base, 0.37f, fg);
      drawTextAligned(c, BOLD, m.rows[i].value, (float)(r.x + r.w - 10), base, 0.37f,
                      kPal.calm, Align::Right);
    }
  }
  if (m.footer && m.footer[0]) centeredFit(c, REG, m.footer, 254, S_TINY, fg2, 224);
  if (m.footer2 && m.footer2[0]) centeredFit(c, BOLD, m.footer2, 270, 0.31f, kPal.calm, 224);
}

}  // namespace mc
