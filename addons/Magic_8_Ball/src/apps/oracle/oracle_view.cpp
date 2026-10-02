// Shake Oracle: watch-side glue. Everything interesting (detector, answers,
// physics, pixels) lives in core/ and is host-tested; this file moves data
// between BaseOS and OracleApp:
//
//   IMU stream  -> OracleApp::imuSample()   (every taskIO sample, timestamped)
//   touch/swipe -> OracleApp::tap()/swipePack()
//   OracleApp::frame() -> canvas rows -> panel, buzzes -> hapticBuzz()
//
// It also keeps the screen awake (minAwakeSec), dims the backlight shortly
// before the idle timeout, saves the chosen pack in NVS namespace
// "shake-oracle", and keeps the last answer + RNG in RTC memory so the same
// answer never comes up twice in a row, even across deep sleep.
#include "oracle_view.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>

#include "FreeSansBold24pt7b.h"
#include "controller.h"
#include "display.h"
#include "haptic.h"
#include "imu_stream.h"
#include "model.h"
#include "oracle_app.h"
#include "oracle_config.h"

using namespace oracle;

namespace {

const char *kNvsNamespace = "shake-oracle";   // <= 15 chars
const char *kNvsPackKey = "pack";

// ---- state that survives deep sleep (lost on a full power-off)
constexpr uint32_t kRtcMagic = 0x0AC1E5A7u;
RTC_DATA_ATTR uint32_t rtcMagic = 0;
RTC_DATA_ATTR int8_t   rtcLastPack = -1;
RTC_DATA_ATTR int8_t   rtcLastIndex = -2;
RTC_DATA_ATTR uint64_t rtcRng = 0;

OracleApp       gApp;
bool            gBeginTried = false;
bool            gReady = false;          // full renderer available
Arduino_Canvas *gCanvas = nullptr;
bool            gFullFlush = false;
bool            gDimmed = false;
uint32_t        gLastSampleMs = 0;

// Touch: a tap is a press that neither moved nor lingered. It is acted on a
// moment later, so a quick flick whose swipe gesture arrives just after the
// finger lifts doesn't count as a tap as well.
bool     gPressActive = false;
bool     gPressMoved = false;
uint16_t gPressX = 0, gPressY = 0;
uint32_t gPressMs = 0;
bool     gTapPending = false;
uint32_t gTapDueMs = 0;
constexpr uint32_t kTapDelayMs = 110;
// Press and hold the glass to churn, release to reveal: for anyone who can't
// shake hard, and the only way in if the accelerometer is missing.
bool     gHoldChurn = false;
constexpr uint32_t kHoldMs = 600;

// Frame timing, reported by "oracle stats" and on exit.
struct Stats {
  uint32_t frames = 0;
  uint64_t renderUs = 0, flushUs = 0;
  uint32_t worstUs = 0;
  uint32_t sinceMs = 0;
} gStats;

// ---- serial console (written by loopTask, read by the render task)
enum Cmd : int { kNone = 0, kShake, kStop, kAsk, kGold, kSay, kPack, kStats };
std::atomic<int> gCmd{kNone};
char             gCmdText[kMaxTextLen];
int              gCmdArg = 0;
char             gSayText[kMaxTextLen];   // must outlive the answer it forces
uint32_t         gAskStopAt = 0;          // "oracle ask": when to stop shaking

// ---- fallback when PSRAM can't hold the renderer: plain text, same brain
struct Fallback {
  ShakeDetector det;
  AnswerPicker  picker{1};
  const char   *text = nullptr;   // answer on screen, nullptr = prompt
  bool          dirty = true;
} gFb;

void *oracleAlloc(size_t n, bool hot) {
  // Per-frame tables go in internal RAM when there's plenty to spare.
  if (hot && heap_caps_get_free_size(MALLOC_CAP_INTERNAL) > n + 96 * 1024) {
    void *p = heap_caps_calloc(1, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p) return p;
  }
  void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : calloc(1, n);
}

uint64_t hwSeed() { return ((uint64_t)esp_random() << 32) ^ esp_random(); }

int loadPack() {
  Preferences p;
  if (!p.begin(kNvsNamespace, /*readOnly=*/true)) return 0;   // first run
  int v = p.getUChar(kNvsPackKey, 0);
  p.end();
  return v < packCount() ? v : 0;
}

void savePack(int pack) {
  Preferences p;
  if (!p.begin(kNvsNamespace, /*readOnly=*/false)) return;
  p.putUChar(kNvsPackKey, (uint8_t)pack);
  p.end();
}

void saveRtc() {
  int lp, li;
  uint64_t rng;
  gApp.exportState(lp, li, rng);
  rtcLastPack = (int8_t)lp;
  rtcLastIndex = (int8_t)li;
  rtcRng = rng;
  rtcMagic = kRtcMagic;
}

uint8_t userBrightness() {
  ModelLock lk;
  return model.brightness;
}

// Dim gently in the last few seconds before the idle timeout, like a phone.
void updateDim() {
  uint16_t to = effectiveSleepTimeoutSec();
  bool dim = to > 0 && idleMs() + kDimLeadMs >= (uint32_t)to * 1000u;
  if (dim == gDimmed) return;
  uint8_t b = userBrightness();
  uint8_t lo = (uint8_t)(b * kDimPercent / 100);
  if (lo < 6 && b >= 6) lo = 6;
  backlightSet(dim ? lo : b);
  gDimmed = dim;
}

void printStats(const char *why) {
  uint32_t ms = millis() - gStats.sinceMs;
  if (!gStats.frames || !ms) return;
  Serial.printf("oracle %s: %lu frames in %.1f s (%.1f fps), render %.1f ms + flush %.1f ms avg, "
                "worst %.1f ms, imu drops %lu, reversals %lu\n",
                why, (unsigned long)gStats.frames, ms / 1000.f, gStats.frames * 1000.f / ms,
                gStats.renderUs / 1000.f / gStats.frames, gStats.flushUs / 1000.f / gStats.frames,
                gStats.worstUs / 1000.f, (unsigned long)imuStreamDropped(),
                (unsigned long)gApp.detector().reversals());
  Serial.printf("oracle heap: internal %u free, psram %u free\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void pollConsole(uint32_t now) {
  if (gAskStopAt && (int32_t)(now - gAskStopAt) >= 0) {
    gAskStopAt = 0;
    gApp.simulateShake(false);
  }
  int c = gCmd.load(std::memory_order_acquire);
  if (c == kNone) return;
  char text[kMaxTextLen];
  memcpy(text, gCmdText, sizeof text);
  int arg = gCmdArg;
  gCmd.store(kNone, std::memory_order_release);
  switch (c) {
    case kShake: gApp.simulateShake(true); break;
    case kStop:  gApp.simulateShake(false); break;
    case kAsk:   gApp.simulateShake(true); gAskStopAt = now + 900; break;
    case kGold:
      gApp.forceNextAnswer(packAt(gApp.pack()).golden, true);
      Serial.println("oracle: the next answer will be golden");
      break;
    case kSay:
      memcpy(gSayText, text, sizeof gSayText);
      gApp.forceNextAnswer(gSayText, false);
      Serial.printf("oracle: the next answer will be \"%s\"\n", gSayText);
      break;
    case kPack: {
      int n = packCount();
      int d = ((arg % n) + n - gApp.pack()) % n;
      if (d) gApp.swipePack(d);
      break;
    }
    case kStats: printStats("stats"); break;
  }
}

// ---------------------------------------------------------------- fallback

void fallbackDraw() {
  if (!gfx || !gFb.dirty) return;
  gFb.dirty = false;
  gfx->fillScreen(BLACK);
  gfx->fillCircle(120, 140, 100, 0x0005);
  gfx->setTextColor(WHITE);
  gfx->setTextSize(2);
  const char *msg = gFb.text ? gFb.text : "Ask a question, then shake";
  // Greedy word wrap for the 12 px wide size-2 bitmap font.
  char lines[4][20];
  int n = 0, len = 0;
  lines[0][0] = 0;
  char word[kMaxTextLen];
  const char *p = msg;
  while (*p && n < 4) {
    int wl = 0;
    while (*p == ' ') p++;
    while (*p && *p != ' ' && wl < (int)sizeof word - 1) word[wl++] = *p++;
    word[wl] = 0;
    if (!wl) break;
    if (len && len + 1 + wl > 14) { n++; len = 0; if (n >= 4) break; lines[n][0] = 0; }
    if (len) { lines[n][len++] = ' '; }
    int take = wl > 14 ? 14 : wl;
    memcpy(lines[n] + len, word, take);
    len += take;
    lines[n][len] = 0;
  }
  int count = n + (len ? 1 : 0);
  int y = 140 - count * 10;
  for (int i = 0; i < count; i++, y += 20) {
    gfx->setCursor(120 - (int)strlen(lines[i]) * 6, y);
    gfx->print(lines[i]);
  }
}

void fallbackRender(uint32_t now) {
  if (gTapPending && (int32_t)(now - gTapDueMs) >= 0) {
    gTapPending = false;
    if (gFb.text) { gFb.text = nullptr; gFb.dirty = true; hapticBuzz(70, 22); }
  }
  if (gPressActive && !gPressMoved && !gHoldChurn && now - gPressMs >= kHoldMs) {
    gHoldChurn = true;   // revealed on release (OracleView::onEvent)
    gFb.text = "...";
    gFb.dirty = true;
  }
  ImuSample s;
  while (imuStreamPop(s)) {
    auto ev = gFb.det.feed(s.ms, s.x / kCountsPerG, s.y / kCountsPerG, s.z / kCountsPerG);
    if (ev == ShakeDetector::Event::Started) {
      gFb.text = "...";
      gFb.dirty = true;
      noteActivity();
    } else if (ev == ShakeDetector::Event::Stopped) {
      gFb.text = gFb.picker.next(gApp.pack()).text;
      gFb.dirty = true;
      hapticBuzz(kThunkHit, kThunkHitMs);
      noteActivity();
    }
  }
  if (gFb.det.shaking()) noteActivity();
  fallbackDraw();
}

}  // namespace

// ---------------------------------------------------------------- view

void OracleView::onEnter() {
  gCanvas = frameCanvas();
  if (!gBeginTried) {
    gBeginTried = true;
    gReady = gCanvas && gApp.begin(&FreeSansBold24pt7b, oracleAlloc, hwSeed());
    if (rtcMagic == kRtcMagic) gApp.importState(rtcLastPack, rtcLastIndex, rtcRng);
    gApp.setPack(loadPack());
    if (!gReady) Serial.println("oracle: not enough memory for the renderer; text mode");
  }
  gApp.stirEntropy(esp_random());
  if (gReady) {
    gApp.enter(gCanvas->getFramebuffer(), millis());
    gFullFlush = true;
  } else {
    gFb.picker.seed(hwSeed());
    gFb.det.reset();
    gFb.text = nullptr;
    gFb.dirty = true;
  }
  gPressActive = false;
  gTapPending = false;
  gHoldChurn = false;
  gLastSampleMs = millis();
  gDimmed = false;
  gAskStopAt = 0;
  gStats = Stats();
  gStats.sinceMs = millis();
  imuStreamEnable(true);
  noteActivity();
}

void OracleView::onExit() {
  imuStreamEnable(false);
  if (gDimmed) backlightSet(userBrightness());
  gDimmed = false;
  if (gReady) {
    saveRtc();
    printStats("session");
  }
}

uint16_t OracleView::framePeriodMs() const {
  return gReady ? gApp.framePeriodMs() : 60;
}

uint16_t OracleView::minAwakeSec() const { return kAwakeSec; }

void OracleView::render() {
  uint32_t now = millis();
  if (!gReady) {
    fallbackRender(now);
    updateDim();
    return;
  }

  // Every IMU sample since the last frame, in order, with its timestamp.
  ImuSample s;
  bool any = false;
  while (imuStreamPop(s)) {
    gApp.imuSample(s.ms, s.x, s.y, s.z);
    gLastSampleMs = s.ms;
    any = true;
  }
  if (!any && now - gLastSampleMs > 250) gApp.imuIdle(now);   // IMU silent
  bool imuOk;
  { ModelLock lk; imuOk = model.imuOk; }
  gApp.setImuOk(imuOk);
  pollConsole(now);
  if (gTapPending && (int32_t)(now - gTapDueMs) >= 0) {
    gTapPending = false;
    gApp.tap(gPressX, gPressY);
  }
  if (gPressActive && !gPressMoved && !gHoldChurn && now - gPressMs >= kHoldMs) {
    gHoldChurn = true;
    gApp.simulateShake(true);
  }

  uint16_t *fb = gCanvas->getFramebuffer();
  uint32_t t0 = micros();
  FrameOut out;
  gApp.frame(fb, now, out);
  uint32_t t1 = micros();
  if (gFullFlush) {
    gCanvas->flush();
    gFullFlush = false;
  } else if (gfx) {
    // Only the window's rows change: full-width rows are contiguous in the
    // canvas, so they go out as one block (about 60% of a full flush).
    gfx->draw16bitRGBBitmap(0, kWinTop, fb + kWinTop * kScreenW, kScreenW, kWinRows);
    if (out.labelFlush)
      gfx->draw16bitRGBBitmap(0, out.labelY0, fb + out.labelY0 * kScreenW, kScreenW,
                              out.labelRows);
  }
  uint32_t t2 = micros();
  gStats.frames++;
  gStats.renderUs += t1 - t0;
  gStats.flushUs += t2 - t1;
  if (t2 - t0 > gStats.worstUs) gStats.worstUs = t2 - t0;

  for (int i = 0; i < out.buzzCount; i++) hapticBuzz(out.buzz[i].intensity, out.buzz[i].ms);
  if (out.activity) noteActivity();
  if (out.packChanged) savePack(gApp.pack());
  if (out.landed) saveRtc();
  updateDim();
}

void OracleView::onEvent(const Event &e) {
  switch (e.type) {
    case EventType::ButtonShort:     // the button, or a right swipe
      if (gHoldChurn && gReady) gApp.simulateShake(false);
      gHoldChurn = false;
      switchTo(Screen::AppList);
      return;
    case EventType::Gesture:
      if (e.gesture == Gesture::SwipeUp || e.gesture == Gesture::SwipeDown) {
        gPressActive = false;
        gTapPending = false;   // it was a flick, not a tap
        if (gReady) gApp.swipePack(e.gesture == Gesture::SwipeUp ? +1 : -1);
        else { gApp.setPack(gApp.pack() + 1); savePack(gApp.pack()); }
      }
      return;
    case EventType::Touch:
      gPressActive = true;
      gPressMoved = false;
      gPressX = e.x;
      gPressY = e.y;
      gPressMs = millis();
      return;
    case EventType::TouchHold:
    case EventType::TouchUp: {
      if (!gPressActive) return;
      int dx = (int)e.x - gPressX, dy = (int)e.y - gPressY;
      if (dx * dx + dy * dy > 16 * 16) gPressMoved = true;
      if (e.type == EventType::TouchUp) {
        gPressActive = false;
        if (gHoldChurn) {
          gHoldChurn = false;
          if (gReady) {
            gApp.simulateShake(false);   // release reveals the answer
          } else {
            gFb.text = gFb.picker.next(gApp.pack()).text;
            gFb.dirty = true;
            hapticBuzz(kThunkHit, kThunkHitMs);
          }
        } else if (!gPressMoved && millis() - gPressMs < kHoldMs) {
          gTapPending = true;
          gTapDueMs = millis() + kTapDelayMs;
        }
      }
      return;
    }
    default:
      return;
  }
}

// ---------------------------------------------------------------- console

bool oracleConsoleCommand(const char *line) {
  if (strncmp(line, "oracle", 6) != 0 || (line[6] != 0 && line[6] != ' ')) return false;
  const char *arg = line + 6;
  while (*arg == ' ') arg++;
  if (!*arg || strcmp(arg, "help") == 0) {
    Serial.println("oracle commands (open Shake Oracle on the watch first):");
    Serial.println("  oracle ask        shake for a moment, then reveal an answer");
    Serial.println("  oracle shake      start churning   | oracle stop  reveal");
    Serial.println("  oracle gold       make the next answer the golden one");
    Serial.println("  oracle say <txt>  make the next answer <txt>");
    Serial.println("  oracle pack <n>   switch to pack n (0 oracle, 1 yes/no, 2 food, 3 excuses)");
    Serial.println("  oracle stats      frame timing, IMU drops, heap");
    return true;
  }
  Screen scr;
  { ModelLock lk; scr = model.screen; }
  if (scr != Screen::Oracle) {
    Serial.println("oracle: open Shake Oracle on the watch first");
    return true;
  }
  if (gCmd.load(std::memory_order_acquire) != kNone) {
    Serial.println("oracle: busy, try again");
    return true;
  }
  int c = kNone;
  if (!strcmp(arg, "ask")) c = kAsk;
  else if (!strcmp(arg, "shake")) c = kShake;
  else if (!strcmp(arg, "stop")) c = kStop;
  else if (!strcmp(arg, "gold")) c = kGold;
  else if (!strcmp(arg, "stats")) c = kStats;
  else if (!strncmp(arg, "pack ", 5)) { gCmdArg = atoi(arg + 5); c = kPack; }
  else if (!strncmp(arg, "say ", 4)) {
    // Printable ASCII only: the die's font stops at '~'.
    const char *t = arg + 4;
    size_t n = 0;
    for (; *t && n < sizeof gCmdText - 1; t++)
      if (*t >= ' ' && *t <= '~') gCmdText[n++] = *t;
    gCmdText[n] = 0;
    c = n ? kSay : kNone;
  }
  if (c == kNone) {
    Serial.println("oracle: unknown command (try: oracle help)");
    return true;
  }
  gCmd.store(c, std::memory_order_release);
  return true;
}

// ---------------------------------------------------------------- tile icon

void oracleDrawTileIcon(Arduino_GFX *g, int16_t cx, int16_t cy, int16_t r, uint16_t bg) {
  // Rendered once (anti-aliased, by the same core as the app) and cached;
  // re-rendered only if the tile colour or size changes.
  static uint16_t *sprite = nullptr;
  static int       spriteSize = 0;
  static uint16_t  spriteBg = 0;
  static bool      spriteValid = false;
  int size = 2 * r + 1;
  if (!g || size <= 0) return;
  if (!sprite || spriteSize != size) {
    free(sprite);
    sprite = (uint16_t *)heap_caps_malloc((size_t)size * size * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!sprite) sprite = (uint16_t *)malloc((size_t)size * size * 2);
    spriteSize = size;
    spriteValid = false;
  }
  if (!sprite) return;
  if (!spriteValid || spriteBg != bg) {
    renderTileIcon(sprite, size, bg);
    spriteBg = bg;
    spriteValid = true;
  }
  g->draw16bitRGBBitmap(cx - r, cy - r, sprite, size, size);
}
