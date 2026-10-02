// Friend Radar UI kit (v2): one instrument, one material language.
//
// The whole app is drawn as a lit sonar instrument: gunmetal bezel, deep
// phosphor glass, engraved caps, glass controls. It owns its palette (like a
// game or a face does) rather than following the BaseOS theme, so every
// screen reads as part of the same object. Layout rectangles live here so
// drawing and hit-testing can never disagree.
#pragma once
#include "fr_gfx.h"
#include "fr_types.h"

namespace fr {

namespace pal {
// Ambient room the instrument sits in.
constexpr uint16_t room0     = hex(0x04070A);
constexpr uint16_t room1     = hex(0x090E12);
// Phosphor (the scope's single light colour) and its ramp.
constexpr uint16_t phosHot   = hex(0xC6FFEC);
constexpr uint16_t phos      = hex(0x4CF2B8);
constexpr uint16_t phosMid   = hex(0x2DB389);
constexpr uint16_t phosDim   = hex(0x1C6A55);
constexpr uint16_t phosFaint = hex(0x113A30);
// Glass panels / controls.
constexpr uint16_t panelTop  = hex(0x162129);
constexpr uint16_t panelBot  = hex(0x0C1318);
constexpr uint16_t panelEdge = hex(0x2A3A44);
constexpr uint16_t well      = hex(0x070C0F);
// Type.
constexpr uint16_t text      = hex(0xE8F3EF);
constexpr uint16_t textDim   = hex(0x8DA79E);
constexpr uint16_t textFaint = hex(0x50665F);
constexpr uint16_t engrave   = hex(0x07090B);
// Signals.
constexpr uint16_t live      = hex(0x41F59C);
constexpr uint16_t warn      = hex(0xFFB44C);
constexpr uint16_t danger    = hex(0xFF5D6F);
constexpr uint16_t stranger  = hex(0x6FD9B8);
// Zones: one phosphor ramp, brighter = closer.
constexpr uint16_t zoneHere   = hex(0x9DFFDB);
constexpr uint16_t zoneNear   = hex(0x4CF2B8);
constexpr uint16_t zoneAround = hex(0x31BC92);
constexpr uint16_t zoneFar    = hex(0x2A8C72);
constexpr uint16_t zoneLost   = hex(0x46544F);
}  // namespace pal

uint16_t zoneColor(Zone z);
// A watch's personal colour (its orb, and its comet in shared animations),
// one of a small curated set that never reads as the phosphor green. Derived
// from the id alone, so both watches agree on it.
constexpr int kMateColorCount = 10;
int      mateColorIndex(uint32_t id);
uint16_t mateColorAt(int index);
uint16_t mateColor(uint32_t id);
uint16_t inkOn(uint16_t fill);

// ---- shared layout ----------------------------------------------------------------
namespace layout {
constexpr int W = 240, H = 280;
// Round glass buttons in the top corners of every page.
constexpr float btnR = 19.f;
constexpr float backCx = 27.f, backCy = 27.f;
constexpr float menuCx = 213.f, menuCy = 27.f;
const Rect backHit(0, 0, 64, 56);      // stops short of lists and keys below
const Rect menuHit(176, 0, 64, 56);
constexpr int headerH = 54;
// The instrument.
constexpr int cx = 120, cy = 140;
constexpr float Ro = 113.f;                  // bezel outer radius
constexpr int   Rg = 92;                     // glass radius
constexpr float ringR[3] = {23.f, 50.f, 73.f};   // here|near, near|around, around|far
constexpr float bandLo[4] = {9.f, 29.f, 56.f, 77.f};
constexpr float bandHi[4] = {17.f, 44.f, 67.f, 85.f};
// Zone labels sit along the bottom arc, in a sector blips never use.
constexpr float labelSectorDeg = (float)kBlipKeepOutHalfDeg;
// Detail card (glass sheet) when fully open.
constexpr int cardTop = 136;
const Rect card(0, cardTop, 240, 280 - cardTop);
const Rect cardBtnL(14, cardTop + 96, 102, 42);
const Rect cardBtnR(124, cardTop + 96, 102, 42);
const Rect cardBtnWide(14, cardTop + 96, 212, 42);
// Lists of glass tiles.
constexpr int listTop = 58, rowH = 52, tileH = 46;
// Bottom action button on single-purpose pages.
const Rect actionBtn(18, 226, 204, 44);
}  // namespace layout

// ---- line icons (engraved style; s ~ icon size in px) --------------------------
void iconChevronLeft(Canvas &c, float cx, float cy, float s, uint16_t col, uint8_t a = 255);
void iconChevronRight(Canvas &c, float cx, float cy, float s, uint16_t col, uint8_t a = 255);
void iconDots(Canvas &c, float cx, float cy, uint16_t col, uint8_t a = 255);
void iconCheck(Canvas &c, float cx, float cy, float s, uint16_t col, uint8_t a = 255);
void iconPlus(Canvas &c, float cx, float cy, float s, uint16_t col, uint8_t a = 255);
void iconCross(Canvas &c, float cx, float cy, float s, uint16_t col, uint8_t a = 255);
void iconHeart(Canvas &c, float cx, float cy, float s, uint16_t col, uint8_t a = 255);
void iconPeople(Canvas &c, float cx, float cy, float s, uint16_t col, uint8_t a = 255);
void iconTag(Canvas &c, float cx, float cy, float s, uint16_t col, uint8_t a = 255);
void iconTarget(Canvas &c, float cx, float cy, float s, uint16_t col, uint8_t a = 255);
void iconBell(Canvas &c, float cx, float cy, float s, uint16_t col, uint8_t a = 255);
void iconMoon(Canvas &c, float cx, float cy, float s, uint16_t col, uint16_t bg, uint8_t a = 255);
void iconClock(Canvas &c, float cx, float cy, float s, uint16_t col, uint8_t a = 255);
void iconQuestion(Canvas &c, float cx, float cy, float s, uint16_t col, uint8_t a = 255);
void iconRefresh(Canvas &c, float cx, float cy, float s, uint16_t col, uint8_t a = 255);
void iconPencil(Canvas &c, float cx, float cy, float s, uint16_t col, uint8_t a = 255);
void iconBackspace(Canvas &c, float cx, float cy, float s, uint16_t col, uint8_t a = 255);
void iconShift(Canvas &c, float cx, float cy, float s, uint16_t col, bool filled);
void iconWaves(Canvas &c, float cx, float cy, float s, uint16_t col, uint8_t a = 255);
void iconWatch(Canvas &c, float cx, float cy, float s, uint16_t glow);

// ---- materials and widgets -------------------------------------------------------
// The dark room behind every page: gradient, a soft phosphor glow from above,
// fine grain. Built once into a W*H buffer and copied per frame.
void buildPageBackground(Canvas &c);
// Round glass button (back / menu / done) with an icon drawn by the caller.
void drawGlassDisc(Canvas &c, float cx, float cy, float r, bool pressed, bool primary = false);
void drawBackButton(Canvas &c, bool pressed = false);
void drawMenuButton(Canvas &c, bool pressed = false);
// Page title, engraved divider below it.
void drawPageTitle(Canvas &c, const char *title, uint8_t a = 255);
// A glass tile (list row, panel).
void drawTile(Canvas &c, int x, int y, int w, int h, bool pressed, float r = 14.f, uint8_t a = 255);
// A recessed round well for a line icon.
void drawIconWell(Canvas &c, float cx, float cy, float r);
enum class PillStyle : uint8_t { Glass, Primary, Danger, DangerSolid, Disabled };
void drawPill(Canvas &c, const Rect &r, const char *label, PillStyle style, bool pressed,
              const Font &f = kFontMB);
// A physical switch: pos 0 = off .. 1 = on (springs between), 50 x 28.
void drawSwitch(Canvas &c, int x, int y, float pos, bool pressed);
// Engraved, letter-spaced small caps (labels, section headers).
void drawCaps(Canvas &c, int x, int baseline, const char *s, uint16_t col, uint8_t a = 255, int track = 2);
void drawCapsCentered(Canvas &c, int cx, int baseline, const char *s, uint16_t col, uint8_t a = 255, int track = 2);
// A glowing orb (a mate) with a breathing halo and their initial.
void drawOrb(Canvas &c, float cx, float cy, float r, uint16_t col, float halo, char initial, uint8_t a = 255);
// A small indicator lamp.
void drawLed(Canvas &c, float cx, float cy, uint16_t col, float glow);
// Five rising bars, `bars` of them lit.
void drawSignalMeter(Canvas &c, int x, int baseline, int bars, uint16_t on);

// Page push / pop: `c` holds the incoming page, `under` (W*H) the outgoing
// one. k is the transition position (0 = only the outgoing page .. 1 = only
// the incoming page; a spring may overshoot slightly). Forward slides the new
// page in from the right over the old one, which drifts left and dims; back
// slides the old page off to the right, uncovering the new one.
void composeSlide(Canvas &c, const uint16_t *under, float k, bool forward);

// "12 s ago", "5 min ago", "2 h ago", "yesterday", "3 days ago"
void formatAgo(uint32_t seconds, char *out, size_t cap);

}  // namespace fr
