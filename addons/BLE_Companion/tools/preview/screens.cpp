// Host previews of the Companion and Message screens (src/apps/companion.cpp
// + companion_ui.cpp) in each state, with BleConfig, the theme helpers and the
// view registry stubbed out. Writes raw RGB565 frames for render.py.
//
// The views draw into frameCanvas() and push rectangles to `gfx`; here
// frameCanvas() is an Arduino_Canvas whose output is a second canvas, the
// "panel", and gfx is that panel too. Every shot is taken from the panel
// (what the user would see) after a first frame and an animation frame, and
// is checked against a fresh render made directly at the shot's time: any
// difference means an incremental push missed something. Exit code 1 then.
#include <Arduino_GFX_Library.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ble_config.h"
#include "companion.h"
#include "display.h"
#include "haptic.h"
#include "model.h"
#include "view.h"

extern uint32_t g_hostMillis;

// ---------------------------------------------------------------------------
// Stubs for what the screens use from the rest of the firmware
// ---------------------------------------------------------------------------
Arduino_GFX *gfx = nullptr;
static Arduino_Canvas *gFrame = nullptr;
Arduino_Canvas *frameCanvas() { return gFrame; }
View *currentView = nullptr;
static Screen gLastSwitch = Screen::Watch;
void switchTo(Screen s) { gLastSwitch = s; }
void hapticBuzz(uint8_t, uint16_t) {}
void hapticSetStrengthPct(uint8_t) {}

ThemeColors theme() {
  ThemeColors t;
  t.bg = model.bgColor; t.fg = model.fgColor; t.accent = model.accentColor; t.line = model.lineColor;
  return t;
}
uint16_t contrastFor(uint16_t bg) {          // copy of BaseOS view.cpp
  uint8_t r = (bg >> 11) & 0x1F, g = (bg >> 5) & 0x3F, b = bg & 0x1F;
  uint16_t lum = (uint16_t)((r << 3) * 3 + (g << 2) * 6 + (b << 3) * 1) / 10;
  return (lum < 128) ? 0xFFFF : 0x0000;
}
bool tappedBack(uint16_t x, uint16_t y) { return x < 72 && y < 52; }

namespace BleConfig {
static Status gStatus;
static bleproto::Message gMsg;
static bool gHasMsg = false;
void begin() {}
void startAdvertising(uint32_t) {}
void stopAdvertising() {}
void disconnect() {}
Status status() { return gStatus; }
bool clientConnected() { return gStatus.state == State::Pairing || gStatus.state == State::Connected; }
uint32_t lastActivityMs() { return 0; }
void noteUserActivity() {}
bool takeMessage(bleproto::Message &m) { if (!gHasMsg) return false; m = gMsg; return true; }
void resetFaceFromWatch() {}
void resetThemeFromWatch() {}
void prepareForSleep() {}
}  // namespace BleConfig

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------
static int gFailures = 0;

static void setTheme(uint16_t bg, uint16_t fg, uint16_t accent, uint16_t line) {
  model.bgColor = bg; model.fgColor = fg; model.accentColor = accent; model.lineColor = line;
}

static BleConfig::Status baseStatus(BleConfig::State st, BleConfig::Reason why) {
  BleConfig::Status s;
  memset(&s, 0, sizeof(s));
  s.state = st;
  s.reason = why;
  s.radioUp = true;
  s.attemptsLeft = BleConfig::kMaxAttempts;
  strcpy(s.deviceName, "EWatch-1A2B");
  return s;
}

static void dump(const std::string &dir, const char *name) {
  std::string path = dir + "/" + name + ".rgb565";
  FILE *fp = fopen(path.c_str(), "wb");
  const uint16_t *fb = static_cast<Arduino_Canvas *>(gfx)->getFramebuffer();
  for (int i = 0; i < 240 * 280; i++) {
    uint8_t b[2] = { (uint8_t)(fb[i] & 0xFF), (uint8_t)(fb[i] >> 8) };
    fwrite(b, 1, 2, fp);
  }
  fclose(fp);
}

static void clearAll() {
  memset(gFrame->getFramebuffer(), 0, 240 * 280 * 2);
  memset(static_cast<Arduino_Canvas *>(gfx)->getFramebuffer(), 0, 240 * 280 * 2);
}

static int panelDiff(const std::vector<uint16_t> &ref) {
  const uint16_t *fb = static_cast<Arduino_Canvas *>(gfx)->getFramebuffer();
  int n = 0;
  for (int i = 0; i < 240 * 280; i++) n += fb[i] != ref[i];
  return n;
}

static std::vector<uint16_t> panelCopy() {
  const uint16_t *fb = static_cast<Arduino_Canvas *>(gfx)->getFramebuffer();
  return std::vector<uint16_t>(fb, fb + 240 * 280);
}

// First frame at 1000 ms, then the shot's frame at `atMs` (incremental), with
// `s2` as the status by then. Checked against a fresh render at `atMs`.
template <class Setup>
static void shot(const std::string &dir, const char *name, const BleConfig::Status &s1,
                 const BleConfig::Status &s2, uint32_t atMs, Setup setup) {
  clearAll();
  BleConfig::gStatus = s1;
  {
    CompanionView v;
    g_hostMillis = 1000;
    v.onEnter();
    v.render();
    g_hostMillis = atMs;
    BleConfig::gStatus = s2;
    setup(v);
    v.render();
  }
  std::vector<uint16_t> incremental = panelCopy();
  clearAll();
  {
    CompanionView v;
    g_hostMillis = atMs;
    v.onEnter();
    setup(v);
    v.render();
  }
  int d = panelDiff(incremental);
  printf("rendered %-28s incremental vs fresh: %d px differ\n", name, d);
  if (d) gFailures++;
  dump(dir, name);
}

static void shot(const std::string &dir, const char *name, const BleConfig::Status &s, uint32_t atMs) {
  shot(dir, name, s, s, atMs, [](CompanionView &) {});
}

static void messageShot(const std::string &dir, const char *name, const char *text, uint8_t icon,
                        uint32_t atMs) {
  BleConfig::gMsg.icon = icon;
  BleConfig::gMsg.flags = 0;
  BleConfig::gMsg.len = (uint8_t)strlen(text);
  strcpy(BleConfig::gMsg.text, text);
  BleConfig::gHasMsg = true;
  clearAll();
  MessageView mv;
  g_hostMillis = 1000;
  mv.onEnter();
  mv.render();
  g_hostMillis = atMs;
  mv.render();
  printf("rendered %s\n", name);
  dump(dir, name);
}

static Event touchAt(uint16_t x, uint16_t y) {
  Event e;
  memset(&e, 0, sizeof(e));
  e.type = EventType::Touch;
  e.x = x;
  e.y = y;
  return e;
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s outdir\n", argv[0]); return 2; }
  std::string dir = argv[1];
  Arduino_Canvas *panel = new Arduino_Canvas(240, 280, nullptr);
  panel->begin(GFX_SKIP_OUTPUT_BEGIN);
  gFrame = new Arduino_Canvas(240, 280, panel);
  gFrame->begin(GFX_SKIP_OUTPUT_BEGIN);
  gfx = panel;
  modelInit();

  using BleConfig::State;
  using BleConfig::Reason;

  // Stock BaseOS colours (Midnight).
  setTheme(0x0000, 0xFFFF, 0x000F, 0x7BEF);
  BleConfig::Status s = baseStatus(State::Advertising, Reason::Started);
  s.advRemainingMs = 161000;
  BleConfig::Status s2 = s;
  s2.advRemainingMs = 160300;
  shot(dir, "companion_visible", s, s2, 1700, [](CompanionView &) {});

  s = baseStatus(State::Pairing, Reason::WrongCode);
  s.code = 482913; s.attemptsLeft = 2; s.pairRemainingMs = 41000;
  s2 = s;
  s2.pairRemainingMs = 40100;
  shot(dir, "companion_pairing", s, s2, 1900, [](CompanionView &) {});

  s = baseStatus(State::Connected, Reason::Paired);
  s.sessionMs = 185000; s.writes = 12;
  s2 = s;
  s2.sessionMs = 186000; s2.writes = 13;
  shot(dir, "companion_connected", s, s2, 1500, [](CompanionView &v) {
    v.onEvent(touchAt(120, 150));     // a touch keeps it from jumping to the face
  });

  s = baseStatus(State::Off, Reason::WindowEnded);
  shot(dir, "companion_off", s, s, 1500, [](CompanionView &v) {
    v.onEvent(touchAt(60, 250));      // holding "Reset face"
    g_hostMillis += 420;
  });

  s = baseStatus(State::Off, Reason::LockedOut);
  s.lockoutRemainingMs = 27000;
  s2 = s;
  s2.lockoutRemainingMs = 26500;
  shot(dir, "companion_lockout", s, s2, 1500, [](CompanionView &) {});

  // Light themes and other presets.
  setTheme(0xFFDF, 0x18E3, 0x02BF, 0x8C51);                     // Paper
  s = baseStatus(State::Pairing, Reason::ClientConnected);
  s.code = 70415; s.pairRemainingMs = 58000;
  shot(dir, "companion_pairing_light", s, 1500);

  setTheme(0x1820, 0xFE60, 0xFB45, 0x7A20);                     // Ember
  s = baseStatus(State::Advertising, Reason::Started);
  s.advRemainingMs = 95000;
  shot(dir, "companion_visible_ember", s, 2100);

  setTheme(0x00E5, 0xE7BF, 0x1E1D, 0x3BB2);                     // Ocean
  s = baseStatus(State::Connected, Reason::Paired);
  s.sessionMs = 9000; s.writes = 1;
  shot(dir, "companion_connected_ocean", s, s, 1500, [](CompanionView &v) {
    v.onEvent(touchAt(120, 150));
    Event e;
    memset(&e, 0, sizeof(e));
    e.type = EventType::BleRequest;
    e.x = BleConfig::REQ_IDENTIFY;
    v.onEvent(e);                     // "Hello from the page!" toast
  });

  // Unreadable theme from the page (fg == bg): text must fall back.
  setTheme(0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF);
  s = baseStatus(State::Connected, Reason::Paired);
  s.sessionMs = 9000; s.writes = 3;
  shot(dir, "companion_connected_badtheme", s, s, 1500, [](CompanionView &v) {
    v.onEvent(touchAt(120, 150));
  });

  // Message screen.
  setTheme(0x0864, 0xFFFF, 0x2D7F, 0x52AA);
  messageShot(dir, "message", "Dinner is ready! Come down when you can \xE2\x99\xA5", 0, 6000);
  setTheme(0xFF9E, 0x38E5, 0xE22F, 0xBC53);                     // Sakura
  messageShot(dir, "message_sakura", "Running 10 min late, save me a seat at the caf\xC3\xA9!",
              12, 15000);

  if (gFailures) { printf("FAIL: %d screen(s) differ after incremental redraw\n", gFailures); return 1; }
  return 0;
}
