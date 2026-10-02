#include "parallax_settings.h"
#include <Arduino_GFX_Library.h>
#include <string.h>
#include "display.h"
#include "haptic.h"
#include "parallax_store.h"
#include "tp_config.h"
#include "tp_scene.h"
#include "tp_color.h"

static Screen sBack = Screen::Settings;

void parallaxSettingsOpen(Screen back) {
  sBack = back;
  switchTo(Screen::ParallaxSettings);
}

#ifndef TP_RAISE_TO_WAKE
#define TP_RAISE_TO_WAKE 0
#endif

#if TP_RAISE_TO_WAKE
static const int kRows = 6;
static const int16_t kRowX = 10, kRowW = 220, kRowH = 32, kRowY0 = 52, kRowStep = 37;
#else
static const int kRows = 5;
static const int16_t kRowX = 10, kRowW = 220, kRowH = 38, kRowY0 = 54, kRowStep = 44;
#endif

static const char *rowLabel(int i) {
  static const char *const k[6] = { "Scene", "Depth", "Clock", "Ambient", "Face", "Raise" };
  return i < 6 ? k[i] : "";
}

static const char *rowValue(int i) {
  const ParallaxSettings &s = parallaxSettings();
  switch (i) {
    case 0: return tp::sceneName(s.scene);
    case 1: return tp::kStrengthName[s.depth < tp::kStrengthCount ? s.depth : tp::kStrengthDefault];
    case 2: return s.h12 ? "12-hour" : "24-hour";
    case 3: return s.ambient ? "On" : "Off";
    case 4: return s.classicFace ? "Classic" : "Parallax";
    case 5: return s.raiseToWake ? "On" : "Off";
  }
  return "";
}

static void cycleRow(int i) {
  ParallaxSettings &s = parallaxSettings();
  switch (i) {
    case 0: s.scene = (uint8_t)((s.scene + 1) % tp::sceneCount()); break;
    case 1: s.depth = (uint8_t)((s.depth + 1) % tp::kStrengthCount); break;
    case 2: s.h12 = !s.h12; break;
    case 3: s.ambient = !s.ambient; break;
    case 4: s.classicFace = !s.classicFace; break;
    case 5: s.raiseToWake = !s.raiseToWake; break;
  }
}

void ParallaxSettingsView::onEnter() {
  if (gfx) { ThemeColors t = theme(); gfx->fillScreen(t.bg); }
  firstDraw_ = true;
}

void ParallaxSettingsView::onExit() {
  parallaxSettingsSave();
  sBack = Screen::Settings;
}

void ParallaxSettingsView::drawRow(int i) {
  ThemeColors t = theme();
  int16_t y = kRowY0 + i * kRowStep;
  int16_t ty = y + (kRowH - 16) / 2;
  uint16_t rowBg = tp::blend565(t.line, t.bg, 70);
  gfx->fillRoundRect(kRowX, y, kRowW, kRowH, 8, rowBg);
  gfx->setTextSize(2);
  gfx->setTextColor(tp::blend565(t.fg, rowBg, 170), rowBg);
  gfx->setCursor(kRowX + 12, ty);
  gfx->print(rowLabel(i));
  const char *v = rowValue(i);
  int16_t vw = (int16_t)strlen(v) * 12;
  gfx->setTextColor(t.fg, rowBg);
  gfx->setCursor(kRowX + kRowW - 12 - vw, ty);
  gfx->print(v);
}

void ParallaxSettingsView::render() {
  if (!gfx || !firstDraw_) return;
  drawTitleBar("Parallax");
  for (int i = 0; i < kRows; i++) drawRow(i);
  firstDraw_ = false;
}

void ParallaxSettingsView::onEvent(const Event &e) {
  if (e.type == EventType::ButtonShort) { switchTo(sBack); return; }
  if (e.type != EventType::Touch) return;
  if (tappedBack(e.x, e.y)) { switchTo(sBack); return; }
  for (int i = 0; i < kRows; i++) {
    int16_t y = kRowY0 + i * kRowStep;
    if (e.x >= kRowX && e.x < kRowX + kRowW && e.y >= y && e.y < y + kRowH) {
      cycleRow(i);
      hapticBuzz(50, 40);
      if (gfx) drawRow(i);
      return;
    }
  }
}
