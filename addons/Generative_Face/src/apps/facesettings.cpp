#include <Arduino_GFX_Library.h>
#include "facesettings.h"
#include "display.h"
#include "haptic.h"
#include "gf_settings.h"

#ifndef GF_BG_STEPS
#define GF_BG_STEPS 1
#endif

namespace {
const int16_t kRowY[5] = { 58, 102, 146, 190, 234 };
const int16_t kPillX = 132, kPillW = 96, kPillH = 28;
const int16_t kMinusX = 132, kPlusX = 198, kStepW = 30;

bool hit(uint16_t x, uint16_t y, int16_t rx, int16_t ry, int16_t rw, int16_t rh) {
  return (int16_t)x >= rx && (int16_t)x < rx + rw && (int16_t)y >= ry - 6 && (int16_t)y < ry + rh + 6;
}

void label(int16_t y, const char *text, const char *sub) {
  ThemeColors t = theme();
  gfx->setTextSize(1);
  gfx->setTextColor(t.fg, t.bg);
  gfx->setCursor(14, y + 6);
  gfx->print(text);
  gfx->setTextColor(t.line, t.bg);
  gfx->setCursor(14, y + 18);
  gfx->print(sub);
}

void pill(int16_t y, const char *text, bool on) {
  ThemeColors t = theme();
  uint16_t bg = on ? t.accent : t.bg;
  gfx->fillRoundRect(kPillX, y, kPillW, kPillH, 8, bg);
  gfx->drawRoundRect(kPillX, y, kPillW, kPillH, 8, on ? t.accent : t.line);
  gfx->setTextSize(2);
  gfx->setTextColor(on ? contrastFor(t.accent) : t.fg, bg);
  int16_t w = (int16_t)strlen(text) * 12;
  gfx->setCursor(kPillX + (kPillW - w) / 2, y + 7);
  gfx->print(text);
}
}  // namespace

void FaceSettingsView::onEnter() {
  first_ = true;
  dirty_ = true;
}

void FaceSettingsView::render() {
  if (!gfx) return;
  if (first_) {
    drawTitleBar("Face");
    first_ = false;
    dirty_ = true;
  }
  if (dirty_) { drawRows(); dirty_ = false; }
}

void FaceSettingsView::drawRows() {
  ThemeColors t = theme();
  GfSettings s = gfSettings();
  gfx->fillRect(0, 52, 240, 228, t.bg);
  label(kRowY[0], "Watch face", "art or stock");
  pill(kRowY[0], s.classicFace ? "Classic" : "Dayprint", !s.classicFace);
#if GF_BG_STEPS
  label(kRowY[1], "Steps when dark", "uses more battery");
  pill(kRowY[1], s.bgSteps ? "On" : "Off", s.bgSteps);
#else
  label(kRowY[1], "Steps when dark", "not in this build");
  pill(kRowY[1], "Off", false);
#endif
  label(kRowY[2], "Daily goal", "buzz when reached");
  gfx->drawRoundRect(kMinusX, kRowY[2], kStepW, kPillH, 6, t.line);
  gfx->drawRoundRect(kPlusX, kRowY[2], kStepW, kPillH, 6, t.line);
  gfx->setTextSize(2);
  gfx->setTextColor(t.fg, t.bg);
  gfx->setCursor(kMinusX + 9, kRowY[2] + 7); gfx->print('-');
  gfx->setCursor(kPlusX + 9, kRowY[2] + 7);  gfx->print('+');
  char buf[8];
  if (s.goal == 0) snprintf(buf, sizeof(buf), "off");
  else             snprintf(buf, sizeof(buf), "%uk", (unsigned)(s.goal / 1000));
  if (s.goal % 1000) snprintf(buf, sizeof(buf), "%u.%uk", (unsigned)(s.goal / 1000), (unsigned)(s.goal % 1000) / 100);
  gfx->setTextSize(1);
  int16_t w = (int16_t)strlen(buf) * 6;
  gfx->setCursor(kMinusX + kStepW + (kPlusX - kMinusX - kStepW - w) / 2, kRowY[2] + 10);
  gfx->print(buf);
  label(kRowY[3], "Light of day", "warm dusk, cool night");
  pill(kRowY[3], s.ambient ? "On" : "Off", s.ambient);
  label(kRowY[4], "Drifting light", "slow glow when awake");
  pill(kRowY[4], s.animate ? "On" : "Off", s.animate);
}

void FaceSettingsView::onEvent(const Event &e) {
  if (e.type == EventType::ButtonShort) { switchTo(Screen::Settings); return; }
  if (e.type != EventType::Touch) return;
  if (tappedBack(e.x, e.y)) { switchTo(Screen::Settings); return; }
  GfSettings s = gfSettings();
  bool changed = true;
  if (hit(e.x, e.y, kPillX, kRowY[0], kPillW, kPillH))      s.classicFace = !s.classicFace;
#if GF_BG_STEPS
  else if (hit(e.x, e.y, kPillX, kRowY[1], kPillW, kPillH)) s.bgSteps = !s.bgSteps;
#endif
  else if (hit(e.x, e.y, kMinusX, kRowY[2], kStepW, kPillH)) {
    s.goal = (s.goal <= 2000) ? 0 : (uint16_t)(s.goal - 500);
  } else if (hit(e.x, e.y, kPlusX, kRowY[2], kStepW, kPillH)) {
    s.goal = (s.goal == 0) ? 2000 : (uint16_t)(s.goal >= 30000 ? 30000 : s.goal + 500);
  }
  else if (hit(e.x, e.y, kPillX, kRowY[3], kPillW, kPillH)) s.ambient = !s.ambient;
  else if (hit(e.x, e.y, kPillX, kRowY[4], kPillW, kPillH)) s.animate = !s.animate;
  else changed = false;
  if (!changed) return;
  gfSettingsSave(s);
  hapticBuzz(50, 45);
  dirty_ = true;
}
