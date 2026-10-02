// Friend Radar pages (v2): menu, mates, mate detail, keyboard, calibration,
// help and toast, all in the instrument's material language: a dark room,
// glass tiles and keycaps, recessed icon wells, physical switches, engraved
// small caps. Each page is a pure draw function over a small state struct,
// with hit-test helpers that share its layout, so the watch view and the host
// previews render identical pixels.
#pragma once
#include "fr_ui.h"
#include "fr_zones.h"

namespace fr {

// What every page shares: the prebuilt room backdrop and the title bar.
struct PageChrome {
  const uint16_t *backdrop = nullptr;   // W*H from buildPageBackground(); nullptr = draw it now
  bool     backPressed = false;
  bool     onAir = false;               // "VISIBLE" lamp: this watch is on the air
  uint32_t tMs = 0;                     // drives small living details (lamp, cursor)
};
void drawBackdrop(Canvas &c, const PageChrome &pc);
// Title, back button and the on-air lamp (pages call this last, over content).
void drawTitleBar(Canvas &c, const PageChrome &pc, const char *title);

// ---- scrolling lists ------------------------------------------------------------
struct ListView {
  int scroll = 0;     // pixels scrolled
  int pressed = -1;   // row index under the finger (visual feedback)
};
constexpr int kListViewH = layout::H - layout::listTop;   // visible list height
int  listRowAt(int y, const ListView &v, int rowCount);   // -1 = none
int  listMaxScroll(int rowCount);
void listClampScroll(ListView &v, int rowCount);

// ---- menu -------------------------------------------------------------------------
struct MenuRow {
  enum Kind : uint8_t { Nav, Toggle, Danger } kind = Nav;
  enum Icon : uint8_t { IcNone, IcPeople, IcTag, IcTarget, IcBell, IcMoon, IcClock, IcQuestion,
                        IcRefresh } icon = IcNone;
  char  title[24] = {0};
  char  sub[44] = {0};       // optional second line
  char  value[20] = {0};     // right-hand value (Nav rows)
  bool  on = false;          // Toggle rows
  float knob = -1.f;         // animated switch position 0..1 (< 0: follow `on`)
};
void drawMenu(Canvas &c, const PageChrome &pc, const char *title, const MenuRow *rows, int n,
              const ListView &v);

// ---- mates list ---------------------------------------------------------------------
struct MateRowView {
  char     name[kMaxName + 1] = {0};
  bool     live = false;         // heard right now
  Zone     zone = Zone::Lost;
  char     status[32] = {0};     // "Near now · ~2 m", "Seen 5 min ago", "Not seen yet"
  uint16_t color = 0;            // the mate's light
  float    signal = 0.f;         // 0..1, halo strength when live
};
void drawMates(Canvas &c, const PageChrome &pc, const MateRowView *rows, int n, const ListView &v);

// ---- mate detail ----------------------------------------------------------------------
struct MateDetailView {
  char name[kMaxName + 1] = {0};
  bool live = false;
  Zone zone = Zone::Lost;
  float distM = 0.f;
  char status[40] = {0};            // "Last seen 2 h ago" when not live
  uint16_t color = 0;
  float signal = 0.f;
  int  nLog = 0;
  char when[6][24] = {{0}};         // "Today 14:05"
  char dur[6][16] = {{0}};          // "25 min"
  Zone closest[6] = {Zone::Far, Zone::Far, Zone::Far, Zone::Far, Zone::Far, Zone::Far};
  bool confirmRemove = false;
  uint8_t pressed = 0;              // 1 rename, 2 remove
};
void drawMateDetail(Canvas &c, const PageChrome &pc, const MateDetailView &m);
const Rect &mateDetailRenameBtn();
const Rect &mateDetailRemoveBtn();

// ---- keyboard --------------------------------------------------------------------------
// Six columns of 40 x 44 px keys (every key is a full-size touch target), the
// text field in the title bar between Back and Done.
enum KeyCode : int {
  kKeyNone = 0, kKeyShift = -1, kKeyMode = -2, kKeySpace = -3, kKeyBackspace = -4, kKeyDone = -5,
};
struct KeyboardView {
  char title[20] = {0};
  char text[kMaxName + 1] = {0};
  bool shift = true;
  bool symbols = false;
  int  pressedKey = kKeyNone;     // key code under the finger
  uint32_t tMs = 0;               // cursor blink
};
void drawKeyboard(Canvas &c, const PageChrome &pc, const KeyboardView &k);
int  keyboardHit(const KeyboardView &k, int x, int y);   // a KeyCode or a character
const Rect &keyboardDoneBtn();
Rect keyboardKeyRect(int row, int col);                   // visual keycap of a cell

// ---- calibration --------------------------------------------------------------------------
struct CalibrateView {
  enum Phase : uint8_t { Intro, Measuring, Result, Failed } phase = Intro;
  char    peer[kMaxName + 1] = {0};   // who we measure against
  bool    peerAvailable = false;
  int8_t  currentRef = -60;
  bool    currentCalibrated = false;
  int     liveRssi = 0;
  int     samples = 0, target = 60;
  int8_t  result = 0;
  uint8_t spread = 0;
  uint8_t failReason = 0;             // CalResult::Reason
  uint8_t pressed = 0;                // 1 primary, 2 secondary
  uint32_t tMs = 0;
};
void drawCalibrate(Canvas &c, const PageChrome &pc, const CalibrateView &v);
const Rect &calibratePrimaryBtn(const CalibrateView &v);
const Rect &calibrateSecondaryBtn();   // only in Result

// ---- help ----------------------------------------------------------------------------------
void drawHelp(Canvas &c, const PageChrome &pc, int scroll);
int  helpContentHeight();

// ---- toast -----------------------------------------------------------------------------------
// A glass capsule with a lamp; t is its spring position (0 hidden .. 1 shown).
void drawToast(Canvas &c, const char *msg, float t);

// "Today 14:05", "Yesterday 09:12", "Mon 18:30", "12 Sep 18:30" (RTC local seconds since 2000).
void formatWhen(uint32_t whenSec, uint32_t nowSec, char *out, size_t cap);
void formatMinutes(uint16_t minutes, char *out, size_t cap);   // "<1 min", "25 min", "1 h 10 min"

}  // namespace fr
