// Companion + Message screens. See companion.h; the painter is companion_ui.*.
#include "companion.h"

#include <Arduino_GFX_Library.h>
#include <stdio.h>
#include <string.h>

#include "aa_gfx.h"
#include "ble_config.h"
#include "display.h"
#include "face_halo.h"
#include "face_slots.h"
#include "haptic.h"

using BleConfig::State;
using BleConfig::Reason;

namespace {

const int16_t W = 240;
const uint32_t HOLD_MS = 1000;
const uint32_t kAnimFrameMs = 66;        // ~15 fps for the pulses

const char *reasonText(Reason r) {
  switch (r) {
    case Reason::WindowEnded: return "Visibility timed out.";
    case Reason::Stopped:     return "Hidden.";
    case Reason::ClientLeft:  return "The page disconnected.";
    case Reason::Kicked:      return "Disconnected.";
    case Reason::PairTimeout: return "No code entered in time.";
    case Reason::IdleTimeout: return "Idle for 5 min, so it let go.";
    case Reason::LockedOut:   return "Too many wrong codes.";
    default:                  return "Make it visible to connect.";
  }
}

// What to draw: a window that is open but not yet advertising (radio still
// starting) looks like "visible" rather than flashing "off".
uint8_t screenFor(const BleConfig::Status &s) {
  if (s.state == State::Off && s.advRemainingMs > 0 && s.lockoutRemainingMs == 0) return cui::SC_VISIBLE;
  switch (s.state) {
    case State::Advertising: return cui::SC_VISIBLE;
    case State::Pairing:     return cui::SC_PAIRING;
    case State::Connected:   return cui::SC_CONNECTED;
    default:                 return s.lockoutRemainingMs > 0 ? cui::SC_LOCKED : cui::SC_OFF;
  }
}

void themeKey(uint16_t *k) {
  ThemeColors t = theme();
  k[0] = t.bg; k[1] = t.fg; k[2] = t.accent; k[3] = t.line;
}

halo::Palette paletteNow() {
  ThemeColors t = theme();
  return halo::palette(t.bg, t.fg, t.accent, t.line);
}

// Push a rectangle of the canvas to the panel: whole rows when it is wide
// (one contiguous transfer), row by row when narrow.
void pushRect(Arduino_Canvas *cv, cui::Rect r) {
  if (!gfx || !cv) return;
  uint16_t *fb = cv->getFramebuffer();
  if (!fb) { cv->flush(); return; }
  int x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
  int x1 = r.x + r.w > W ? W : r.x + r.w, y1 = r.y + r.h > 280 ? 280 : r.y + r.h;
  if (x1 <= x0 || y1 <= y0) return;
  if (x1 - x0 > 150) {
    gfx->draw16bitRGBBitmap(0, y0, fb + (int32_t)y0 * W, W, y1 - y0);
  } else {
    for (int y = y0; y < y1; y++) gfx->draw16bitRGBBitmap(x0, y, fb + (int32_t)y * W + x0, x1 - x0, 1);
  }
}

void repaint(Arduino_Canvas *cv, const halo::Palette &p, const cui::Model &m, cui::Rect r) {
  if (r.w <= 0 || r.h <= 0) return;
  aa::Surface s;
  aa::init(s, cv->getFramebuffer(), W, 280);
  aa::setClip(s, r.x, r.y, r.x + r.w, r.y + r.h);
  cui::paint(s, p, m);
  pushRect(cv, r);
}

cui::Rect unite(cui::Rect a, cui::Rect b) {
  if (a.w <= 0 || a.h <= 0) return b;
  if (b.w <= 0 || b.h <= 0) return a;
  int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
  int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
  int y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
  cui::Rect r = { (int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0) };
  return r;
}

// Fallback without the PSRAM canvas: plain BaseOS text so pairing still works.
void drawFallback(const cui::Model &m) {
  ThemeColors t = theme();
  gfx->fillScreen(t.bg);
  gfx->setFont(nullptr);
  gfx->setTextColor(contrastFor(t.bg), t.bg);
  gfx->setTextSize(2);
  gfx->setCursor(20, 40);
  static const char *const kNames[] = { "Visible", "Code", "Connected", "Off", "Locked", "Message" };
  gfx->print(kNames[m.screen < 6 ? m.screen : 3]);
  if (m.screen == cui::SC_PAIRING) {
    char code[8];
    snprintf(code, sizeof(code), "%06lu", (unsigned long)(m.code % 1000000));
    gfx->setTextSize(4);
    gfx->setCursor(48, 120);
    gfx->print(code);
  }
}

}  // namespace

// ===========================================================================
// CompanionView
// ===========================================================================
void CompanionView::onEnter() {
  lastValid_ = false;
  shownScreen_ = 0xFF;
  hold_ = HOLD_NONE;
  toastUntilMs_ = 0;
  toast_[0] = 0;
  BleConfig::Status s = BleConfig::status();
  if (s.state == State::Off && s.lockoutRemainingMs == 0) BleConfig::startAdvertising();
}

void CompanionView::onExit() {
  hold_ = HOLD_NONE;
  // Advertising deliberately continues for the rest of its window so the
  // page can still connect while the user looks at the face.
}

void CompanionView::buildModel(cui::Model &m, uint32_t now) {
  memset(&m, 0, sizeof(m));
  BleConfig::Status s = BleConfig::status();
  m.screen = screenFor(s);
  strncpy(m.device, s.deviceName, sizeof(m.device) - 1);
  m.maxAttempts = BleConfig::kMaxAttempts;
  m.attemptsLeft = s.attemptsLeft;
  m.animMs = now;
  switch (m.screen) {
    case cui::SC_VISIBLE:
      m.remainMs = s.advRemainingMs;
      m.totalMs = s.advRemainingMs > BleConfig::kDefaultWindowMs ? s.advRemainingMs : BleConfig::kDefaultWindowMs;
      break;
    case cui::SC_PAIRING:
      m.code = s.code;
      m.remainMs = s.pairRemainingMs;
      m.totalMs = BleConfig::kPairTimeoutMs;
      break;
    case cui::SC_CONNECTED:
      m.sessionMs = s.sessionMs;
      m.writes = s.writes;
      break;
    case cui::SC_LOCKED:
      m.remainMs = s.lockoutRemainingMs;
      m.totalMs = BleConfig::kLockoutMs;
      break;
    default:
      strncpy(m.status, reasonText(s.reason), sizeof(m.status) - 1);
      break;
  }
  if (toastUntilMs_ && (int32_t)(toastUntilMs_ - now) > 0) strncpy(m.toast, toast_, sizeof(m.toast) - 1);
  if (hold_ != HOLD_NONE && !holdFired_) {
    uint32_t held = now - holdStartMs_;
    if (held > HOLD_MS) held = HOLD_MS;
    m.hold = hold_ == HOLD_RESET_THEME ? 2 : 1;
    m.holdPermille = (uint16_t)(held * 1000 / HOLD_MS);
  }
}

void CompanionView::doHoldAction() {
  holdFired_ = true;
  if (hold_ == HOLD_RESET_FACE) {
    BleConfig::resetFaceFromWatch();
    strncpy(toast_, "Face slots reset.", sizeof(toast_) - 1);
  } else {
    BleConfig::resetThemeFromWatch();
    strncpy(toast_, "Colours reset.", sizeof(toast_) - 1);
  }
  toast_[sizeof(toast_) - 1] = 0;
  toastUntilMs_ = millis() + 2500;
  hold_ = HOLD_NONE;
  hapticBuzz(200, 120);
}

void CompanionView::render() {
  if (!gfx) return;
  uint32_t now = millis();
  if (hold_ != HOLD_NONE && !holdFired_ && now - holdStartMs_ >= HOLD_MS) doHoldAction();

  cui::Model m;
  buildModel(m, now);
  if (m.screen != shownScreen_) {
    shownScreen_ = m.screen;
    stateSinceMs_ = now;
    touchedSinceState_ = false;
  }

  Arduino_Canvas *cv = frameCanvas();
  uint16_t key[4];
  themeKey(key);
  bool full = !lastValid_ || m.screen != last_.screen || memcmp(key, lastTheme_, sizeof(key)) != 0 ||
              strcmp(m.device, last_.device) != 0 || m.code != last_.code;
  if (!cv || !cv->getFramebuffer()) {
    if (full) drawFallback(m);
  } else {
    halo::Palette p = paletteNow();
    if (full) {
      aa::Surface s;
      aa::init(s, cv->getFramebuffer(), W, 280);
      cui::paint(s, p, m);
      cv->flush();
      lastAnimMs_ = now;
    } else {
      // Countdown ring end (or the message bar) moved.
      cui::Rect a = cui::ringEnd(last_), b = cui::ringEnd(m);
      if (a.x != b.x || a.y != b.y || m.remainMs / 100 != last_.remainMs / 100) {
        int dx = a.x - b.x, dy = a.y - b.y;
        if (dx * dx + dy * dy > 20 * 20) { a = cui::fullRect(); b = a; }
        repaint(cv, p, m, unite(a, b));
      }
      // Countdown / status line.
      char t0[48], t1[48];
      cui::countdownText(last_, t0, sizeof(t0));
      cui::countdownText(m, t1, sizeof(t1));
      if (strcmp(t0, t1) != 0) repaint(cv, p, m, cui::textRect(m));
      if (strcmp(m.toast, last_.toast) != 0) repaint(cv, p, m, cui::toastRect());
      if (m.hold != last_.hold || m.holdPermille != last_.holdPermille) {
        if (last_.hold) repaint(cv, p, m, cui::holdRect(last_.hold));
        if (m.hold) repaint(cv, p, m, cui::holdRect(m.hold));
      }
      // Pulses.
      if (now - lastAnimMs_ >= kAnimFrameMs) {
        cui::Rect r[2];
        int n = cui::animRects(m, r, 2);
        for (int i = 0; i < n; i++) repaint(cv, p, m, r[i]);
        lastAnimMs_ = now;
      }
    }
  }
  memcpy(lastTheme_, key, sizeof(key));
  last_ = m;
  lastValid_ = true;

  // Just paired from the page: show the face so edits are visible live.
  BleConfig::Status s = BleConfig::status();
  if (m.screen == cui::SC_CONNECTED && s.reason == Reason::Paired && !touchedSinceState_ &&
      s.sessionMs < 3000 && now - stateSinceMs_ > 1600) {
    switchTo(Screen::Watch);
  }
}

void CompanionView::onEvent(const Event &e) {
  if (e.type == EventType::ButtonShort) { switchTo(Screen::AppList); return; }
  if (e.type == EventType::BleRequest && e.x == BleConfig::REQ_IDENTIFY) {
    strncpy(toast_, "Hello from the page!", sizeof(toast_) - 1);
    toast_[sizeof(toast_) - 1] = 0;
    toastUntilMs_ = millis() + 2500;
    return;
  }
  if (e.type == EventType::TouchUp) {
    hold_ = HOLD_NONE;
    return;
  }
  if (e.type != EventType::Touch) return;
  touchedSinceState_ = true;
  if (tappedBack(e.x, e.y)) { switchTo(Screen::AppList); return; }

  switch (shownScreen_) {
    case cui::SC_VISIBLE:
      if (cui::inRect(e.x, e.y, cui::kBtnHide)) { BleConfig::stopAdvertising(); hapticBuzz(60, 50); }
      break;
    case cui::SC_PAIRING:
      if (cui::inRect(e.x, e.y, cui::kBtnPairCancel)) { BleConfig::disconnect(); hapticBuzz(80, 60); }
      break;
    case cui::SC_CONNECTED:
      if (cui::inRect(e.x, e.y, cui::kBtnShowFace)) { hapticBuzz(60, 50); switchTo(Screen::Watch); return; }
      if (cui::inRect(e.x, e.y, cui::kBtnEnd)) { BleConfig::disconnect(); hapticBuzz(80, 60); }
      break;
    default:
      if (cui::inRect(e.x, e.y, cui::kBtnVisible)) {
        BleConfig::startAdvertising();
        hapticBuzz(80, 60);
      } else if (cui::inRect(e.x, e.y, cui::kBtnResetFace)) {
        hold_ = HOLD_RESET_FACE; holdStartMs_ = millis(); holdFired_ = false; hapticBuzz(40, 30);
      } else if (cui::inRect(e.x, e.y, cui::kBtnResetLook)) {
        hold_ = HOLD_RESET_THEME; holdStartMs_ = millis(); holdFired_ = false; hapticBuzz(40, 30);
      }
      break;
  }
}

// ===========================================================================
// MessageView
// ===========================================================================
static Screen sMessageReturn = Screen::Watch;
static const uint32_t kMessageShowMs = 20000;

void MessageView::setReturnScreen(Screen s) {
  if (s != Screen::Message) sMessageReturn = s;
}

void MessageView::onEnter() {
  enteredMs_ = millis();
  loaded_ = false;
  drawn_ = false;
  lastRem_ = -1;
}

void MessageView::close() {
  switchTo(sMessageReturn);
}

void MessageView::render() {
  if (!gfx) return;
  uint32_t now = millis();
  if (!loaded_) {
    loaded_ = true;
    bleproto::Message msg;
    if (!BleConfig::takeMessage(msg)) { close(); return; }
    memset(&m_, 0, sizeof(m_));
    m_.screen = cui::SC_MESSAGE;
    m_.icon = msg.icon;
    m_.msgLen = (uint8_t)faceslots::utf8ToGlyphs(msg.text, msg.len, m_.msg, sizeof(m_.msg) - 1);
    m_.totalMs = kMessageShowMs;
  }
  uint32_t elapsed = now - enteredMs_;
  m_.remainMs = elapsed >= kMessageShowMs ? 0 : kMessageShowMs - elapsed;
  m_.animMs = now;

  Arduino_Canvas *cv = frameCanvas();
  uint16_t key[4];
  themeKey(key);
  if (!cv || !cv->getFramebuffer()) {
    if (!drawn_) {
      drawFallback(m_);
      gfx->setTextSize(2);
      gfx->setCursor(12, 120);
      gfx->print((const char *)m_.msg);
    }
    drawn_ = true;
  } else {
    halo::Palette p = paletteNow();
    int rem = (int)(m_.remainMs / 250);
    if (!drawn_ || memcmp(key, lastTheme_, sizeof(key)) != 0) {
      aa::Surface s;
      aa::init(s, cv->getFramebuffer(), W, 280);
      cui::paint(s, p, m_);
      cv->flush();
      drawn_ = true;
    } else if (rem != lastRem_) {
      repaint(cv, p, m_, cui::ringEnd(m_));
    }
    lastRem_ = rem;
  }
  memcpy(lastTheme_, key, sizeof(key));
  if (elapsed > kMessageShowMs) close();
}

void MessageView::onEvent(const Event &e) {
  if (e.type == EventType::ButtonShort || e.type == EventType::Touch) {
    hapticBuzz(50, 40);
    close();
  }
}
