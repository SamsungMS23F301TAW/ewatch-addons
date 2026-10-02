// Pixel Pet scenes: the watch face diorama and the pet app pages.
// All art and layout here is original. See scenes.h.
#include "scenes.h"
#include <stdio.h>
#include <string.h>

using px::Canvas;
using px::Font;
using px::hex;

namespace scenes {

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
static const int kTopY = 0, kTopH = 86;          // clock + date over the sky
static const int kPetY = 86, kPetH = 140;        // the pet's world
static const int kStatY = 226, kStatH = 54;      // soil panel: steps + snack bar
static const int kGroundY = 214;                 // top of the grass
static const int kFaceScale = 4;
static const int kFacePetX = 40;                 // 40 + 160 = 200
static const int kFacePetY = kGroundY - 29 * kFaceScale;   // feet stand on the grass

void bandRows(Band b, int &y0, int &h) {
  switch (b) {
    case Band::Top:   y0 = kTopY;  h = kTopH;  break;
    case Band::Pet:   y0 = kPetY;  h = kPetH;  break;
    default:          y0 = kStatY; h = kStatH; break;
  }
}

bool faceHitPet(int x, int y) { return x >= 36 && x < 204 && y >= 100 && y < kGroundY + 8; }
bool faceHitStats(int x, int y) { (void)x; return y >= kStatY; }

// ---------------------------------------------------------------------------
// Palette (the face owns its palette; it is artwork)
// ---------------------------------------------------------------------------
static const uint16_t kCream    = hex(0xfff3da);
static const uint16_t kCreamDim = hex(0xc9bfa7);
static const uint16_t kShadow   = hex(0x10132b);
static const uint16_t kSoil     = hex(0x2c2030);
static const uint16_t kSoilDk   = hex(0x221828);
static const uint16_t kPebble   = hex(0x3b2c3f);
static const uint16_t kBerry    = hex(0xff5468);
static const uint16_t kBerryLt  = hex(0xff8f9c);
static const uint16_t kGold     = hex(0xffcf4a);
static const uint16_t kTrack    = hex(0x18111c);
static const uint16_t kMint     = hex(0x7fe3b6);
static const uint16_t kPanel    = hex(0x1b2140);
static const uint16_t kPanelLt  = hex(0x2a3360);
static const uint16_t kPanelEdge = hex(0x3c4a86);

static uint32_t hash32(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
  return x;
}

// ---------------------------------------------------------------------------
// Sky: dithered gradient keyed to the time of day
// ---------------------------------------------------------------------------
struct SkyKey { float h; uint32_t top, bot; };
static const SkyKey kSky[] = {
  {0.0f,  0x070a1d, 0x18204a},
  {5.0f,  0x0a0f2e, 0x232c5c},
  {6.5f,  0x3c3c7c, 0xf2a07e},   // dawn
  {8.0f,  0x3a7dd4, 0x9fd8ff},   // morning
  {16.5f, 0x3276cf, 0x9ad3ff},   // afternoon
  {18.5f, 0x4b3a90, 0xff9d6c},   // sunset
  {20.0f, 0x1c1d52, 0x5b3f7c},   // dusk
  {21.5f, 0x0a0f2e, 0x212c5a},
  {24.0f, 0x070a1d, 0x18204a},
};

static uint16_t lerpHex(uint32_t a, uint32_t b, float t) {
  return px::mix(hex(a), hex(b), (int)(t * 256.0f));
}

static void skyColours(float hour, uint16_t &top, uint16_t &bot) {
  for (unsigned i = 0; i + 1 < sizeof(kSky) / sizeof(kSky[0]); i++) {
    if (hour >= kSky[i].h && hour <= kSky[i + 1].h) {
      float t = (hour - kSky[i].h) / (kSky[i + 1].h - kSky[i].h);
      top = lerpHex(kSky[i].top, kSky[i + 1].top, t);
      bot = lerpHex(kSky[i].bot, kSky[i + 1].bot, t);
      return;
    }
  }
  top = hex(kSky[0].top);
  bot = hex(kSky[0].bot);
}

// 0 by day .. 1 at night (stars, moon, pale clock digits)
static float nightness(float hour) {
  if (hour >= 21.0f || hour < 5.0f) return 1.0f;
  if (hour >= 5.0f && hour < 6.5f) return 1.0f - (hour - 5.0f) / 1.5f;
  if (hour >= 19.0f) return (hour - 19.0f) / 2.0f;
  return 0.0f;
}

static void drawSky(Canvas &c, int y0, int y1, float hour, uint32_t nowMs) {
  uint16_t top, bot;
  skyColours(hour, top, bot);
  const int steps = 14;
  for (int y = y0; y < y1 && y < kGroundY; y++) {
    float f = (float)y / (float)kGroundY * (float)steps;
    int i = (int)f;
    float frac = f - (float)i;
    uint16_t a = px::mix(top, bot, i * 256 / steps);
    uint16_t b = px::mix(top, bot, (i + 1) * 256 / steps);
    px::dither(c, 0, y, W, 1, a, b, (int)(frac * 16.0f));
  }
  const float night = nightness(hour);
  if (night > 0.3f) {
    // stars: fixed positions, gentle twinkle
    for (uint32_t i = 0; i < 26; i++) {
      uint32_t h = hash32(i * 977 + 13);
      int x = (int)(h % W);
      int y = (int)((h >> 9) % 196) + 2;
      if (y < y0 || y >= y1) continue;
      bool bright = (h >> 20) % 4 == 0;
      uint32_t tw = (nowMs / 600 + (h >> 24)) % 7;
      uint16_t col = px::mix(top, hex(0xfff8e0), tw == 0 ? 120 : (int)(night * 230));
      if (bright && tw != 0) {
        px::rect(c, x - 1, y, 3, 1, col);
        px::rect(c, x, y - 1, 1, 3, col);
      } else {
        px::rect(c, x, y, 2, 2, col);
      }
    }
  } else {
    // daytime clouds drifting by, behind everything
    static const char *cloud[] = {
      "....####......",
      "..########.##.",
      ".############.",
      "##############",
    };
    for (int k = 0; k < 3; k++) {
      int cw = 14 * 3;
      int span = W + cw;
      int x = (int)((nowMs / (260 + k * 90) + (uint32_t)k * 97) % (uint32_t)span) - cw;
      int y = 104 + k * 26 - (k == 2 ? 70 : 0);
      if (y + 12 < y0 || y >= y1) continue;
      uint16_t col = px::mix(bot, hex(0xffffff), 170);
      for (int r = 0; r < 4; r++)
        for (int q = 0; q < 14; q++)
          if (cloud[r][q] == '#') px::rect(c, x + q * 3, y + r * 3, 3, 3, col);
    }
  }
}

// ---------------------------------------------------------------------------
// Ground: grass lip with tufts, then the soil panel
// ---------------------------------------------------------------------------
static void drawGround(Canvas &c, int y0, int y1, float hour) {
  const float night = nightness(hour);
  uint16_t grass  = px::mix(hex(0x63c25f), hex(0x24533f), (int)(night * 256));
  uint16_t grassL = px::mix(hex(0x8be07a), hex(0x2f6a4c), (int)(night * 256));
  uint16_t grassD = px::mix(hex(0x3f9a4c), hex(0x183a2e), (int)(night * 256));
  for (int y = kGroundY - 4; y < kStatY; y++) {
    if (y < y0 || y >= y1) continue;
    for (int x = 0; x < W; x += 2) {
      uint32_t h = hash32((uint32_t)(x / 2) * 31 + 7);
      int tuft = (int)(h % 4);               // 0..3 px tall tufts above the line
      int top = kGroundY - tuft;
      uint16_t col;
      if (y < top) continue;
      if (y < kGroundY + 2) col = grassL;
      else if (y < kGroundY + 8) col = grass;
      else col = grassD;
      px::rect(c, x, y, 2, 1, col);
    }
  }
  for (int y = kStatY; y < H; y++) {
    if (y < y0 || y >= y1) continue;
    px::rect(c, 0, y, W, 1, y < kStatY + 2 ? kSoilDk : kSoil);
  }
  // pebbles
  for (uint32_t i = 0; i < 14; i++) {
    uint32_t h = hash32(i * 131 + 5);
    int x = (int)(h % W), y = kStatY + 4 + (int)((h >> 8) % (kStatH - 8));
    if (y >= y0 && y < y1) px::rect(c, x, y, 3, 2, kPebble);
  }
}

// The food bowl next to the pet; shows what's in the pantry.
static void drawBowl(Canvas &c, int x, int baseY, int scale, uint8_t pantry) {
  static const char *bowl[] = {
    "o..........o",
    "obbbbbbbbbbo",
    ".obbbbbbbbo.",
    "..oooooooo..",
  };
  const uint16_t rim = hex(0x5a3b2a), wood = hex(0xc0834f), woodL = hex(0xe0a768);
  int y = baseY - 4 * scale;
  // snacks peeking out of the bowl
  for (int i = 0; i < pantry && i < 3; i++) {
    int bx = x + (2 + i * 3) * scale, by = y - 2 * scale + (i == 1 ? -scale : 0);
    px::rect(c, bx, by, 3 * scale, 2 * scale, kBerry);
    px::rect(c, bx + scale, by - scale, scale, scale, hex(0x5cc24a));
    px::rect(c, bx, by, scale, scale, kBerryLt);
  }
  for (int r = 0; r < 4; r++)
    for (int q = 0; q < 12; q++) {
      char ch = bowl[r][q];
      if (ch == '.') continue;
      uint16_t col = ch == 'o' ? rim : (r == 1 ? woodL : wood);
      px::rect(c, x + q * scale, y + r * scale, scale, scale, col);
    }
}

// ---------------------------------------------------------------------------
// Small UI helpers
// ---------------------------------------------------------------------------
static void bar(Canvas &c, int x, int y, int w, int h, uint8_t pct, uint16_t fill, uint16_t fillHi,
                uint16_t track, uint16_t edge) {
  px::rect(c, x + 1, y, w - 2, h, edge);
  px::rect(c, x, y + 1, w, h - 2, edge);
  px::rect(c, x + 1, y + 1, w - 2, h - 2, track);
  int fw = (int)((uint32_t)(w - 4) * (pct > 100 ? 100 : pct) / 100);
  if (fw > 0) {
    px::rect(c, x + 2, y + 2, fw, h - 4, fill);
    px::rect(c, x + 2, y + 2, fw, 2, fillHi);
  }
}

static void bigClock(Canvas &c, const FaceData &d, float night) {
  char buf[8];
  if (d.rtcOk) snprintf(buf, sizeof buf, "%02u%c%02u", d.hour, d.colonOn ? ':' : ' ', d.minute);
  else snprintf(buf, sizeof buf, "--:--");
  const int scale = 5;
  int w = px::textWidth(buf, scale, Font::Big);
  int x = (W - w) / 2, y = 12;
  uint16_t col = px::mix(kCream, hex(0xcfd8ff), (int)(night * 200));
  px::text(c, buf, x + 3, y + 3, scale, kShadow, Font::Big);
  px::text(c, buf, x, y, scale, col, Font::Big);
}

static void dateLine(Canvas &c, const FaceData &d, float night) {
  static const char *kWd[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  static const char *kMo[] = {"???", "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                              "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  char buf[32];
  if (d.clockUnset) snprintf(buf, sizeof buf, "SET DATE+TIME");
  else snprintf(buf, sizeof buf, "%s %u %s", d.weekday < 7 ? kWd[d.weekday] : "---", d.day,
                d.month >= 1 && d.month <= 12 ? kMo[d.month] : "???");
  uint16_t col = px::mix(kCream, hex(0xcfd8ff), (int)(night * 200));
  int w = px::textWidth(buf, 2);
  px::textShadow(c, buf, (W - w) / 2, 66, 2, col, kShadow);
}

static void battery(Canvas &c, const FaceData &d) {
  if (!d.batOk) return;
  const int x = 212, y = 6;
  uint16_t edge = kCream, fill = d.batPct <= 15 ? kBerry : kMint;
  px::rect(c, x + 1, y + 1, 18, 9, kShadow);
  px::frame(c, x, y, 18, 9, edge);
  px::rect(c, x + 18, y + 3, 2, 3, edge);
  int fw = d.batPct * 14 / 100;
  if (fw < 1) fw = 1;
  px::rect(c, x + 2, y + 2, fw, 5, fill);
}

static void toast(Canvas &c, const FaceData &d, int cy) {
  if (!d.toast) return;
  int rise = (int)(d.toastAgeMs / 90);
  if (rise > 10) rise = 10;
  int w = px::textWidth(d.toast, 2);
  int x = (W - w) / 2, y = cy - rise;
  px::rect(c, x - 4, y - 3, w + 8, 20, kShadow);
  px::rect(c, x - 3, y - 2, w + 6, 18, kPanel);
  px::text(c, d.toast, x, y + 1, 2, kGold);
}

// ---------------------------------------------------------------------------
// Face
// ---------------------------------------------------------------------------
static float hourOf(const FaceData &d) {
  return d.rtcOk ? (float)d.hour + (float)d.minute / 60.0f : 12.0f;
}

static void statsPanel(Canvas &c, const FaceData &d, uint32_t nowMs) {
  char buf[24];
  // row 1: steps today
  px::text(c, "~", 12, kStatY + 9, 3, kShadow);
  px::text(c, "~", 10, kStatY + 7, 3, kCream);
  px::fmtThousands(buf, sizeof buf, d.stepsToday);
  const int sw = px::textShadow(c, buf, 36, kStatY + 9, 3, kCream, kShadow);
  // today's goal met: a gold star beside the count
  if (d.goal && d.stepsToday >= d.goal) petart::drawIcon(c, petart::Icon::Star, 36 + sw + 6, kStatY + 9, 3);
  // walking indicator: little footprints marching
  if (d.walking) {
    int k = (int)((nowMs / 250) % 3);
    for (int i = 0; i < 3; i++)
      px::rect(c, 214 + i * 7, kStatY + 16 - ((i == k) ? 3 : 0), 4, 4, i <= k ? kGold : kPebble);
  }
  // row 2: progress to the next snack (or to hatching)
  const bool egg = d.pose.stage == pet::Stage::Egg;
  const int by = kStatY + 36;
  petart::drawIcon(c, egg ? petart::Icon::Star : petart::Icon::Snack, 8, by - 6, 2);
  bar(c, 26, by - 2, 128, 12, d.snackPct, egg ? kGold : kBerry, egg ? hex(0xffe9a8) : kBerryLt,
      kTrack, hex(0x4a3650));
  if (egg) snprintf(buf, sizeof buf, "HATCH");
  else snprintf(buf, sizeof buf, "%u", d.toSnack);
  px::text(c, buf, 160, by, 2, kCreamDim);
}

void drawFace(Canvas &c, const FaceData &d, uint32_t nowMs, Band band) {
  int y0, h;
  bandRows(band, y0, h);
  c.clip(0, y0, W, y0 + h);
  const float hour = hourOf(d);
  const float night = nightness(hour);
  drawSky(c, y0, y0 + h, hour, nowMs);
  drawGround(c, y0, y0 + h, hour);
  switch (band) {
    case Band::Top:
      bigClock(c, d, night);
      dateLine(c, d, night);
      battery(c, d);
      break;
    case Band::Pet: {
      // soft shadow under the pet
      uint16_t sh = px::mix(hex(0x63c25f), kShadow, 120);
      px::rect(c, 76, kGroundY + 2, 88, 4, sh);
      px::rect(c, 84, kGroundY + 6, 72, 2, sh);
      drawBowl(c, 190, kGroundY + 6, 3, d.pantry);
      petart::draw(c, d.pose, kFacePetX, kFacePetY, kFaceScale);
      toast(c, d, 96);
      break;
    }
    case Band::Stats:
      statsPanel(c, d, nowMs);
      break;
  }
  c.noClip();
}

void drawFaceAll(Canvas &c, const FaceData &d, uint32_t nowMs) {
  drawFace(c, d, nowMs, Band::Top);
  drawFace(c, d, nowMs, Band::Pet);
  drawFace(c, d, nowMs, Band::Stats);
}

// ---------------------------------------------------------------------------
// Pet app
// ---------------------------------------------------------------------------
static const int kAppPetScale = 5;
static const int kAppGroundY = 220;
static const int kAppPetX = 20;
static const int kAppPetY = kAppGroundY - 29 * kAppPetScale;

bool appHitPet(int x, int y) { return x >= 40 && x < 200 && y >= 80 && y < kAppGroundY + 8; }
bool appHitBowl(int x, int y) { return x >= 186 && x < 236 && y >= kAppGroundY - 30 && y < kAppGroundY + 12; }

static void backChevron(Canvas &c) {
  px::rect(c, 6, 8, 30, 26, kShadow);
  px::rect(c, 5, 7, 30, 26, kPanelLt);
  px::text(c, "{", 13, 14, 2, kCream);
}

static void pageDots(Canvas &c, Page p) {
  for (int i = 0; i < (int)Page::Count; i++) {
    int y = 100 + i * 14;
    bool on = i == (int)p;
    px::rect(c, 230, y, on ? 6 : 4, on ? 6 : 4, on ? kCream : kPanelEdge);
  }
}

static void appPet(Canvas &c, const AppData &d, uint32_t nowMs) {
  const float hour = hourOf(d.face);
  drawSky(c, 0, H, hour, nowMs);
  // ground for the app stage
  const float night = nightness(hour);
  uint16_t grass  = px::mix(hex(0x63c25f), hex(0x24533f), (int)(night * 256));
  uint16_t grassL = px::mix(hex(0x8be07a), hex(0x2f6a4c), (int)(night * 256));
  for (int x = 0; x < W; x += 2) {
    int tuft = (int)(hash32((uint32_t)x * 17 + 3) % 4);
    px::rect(c, x, kAppGroundY - tuft, 2, tuft + 2, grassL);
  }
  px::rect(c, 0, kAppGroundY + 2, W, 8, grass);
  px::rect(c, 0, kAppGroundY + 10, W, H - kAppGroundY - 10, kSoil);
  // title: name, stage, age
  char buf[40];
  snprintf(buf, sizeof buf, "%s", d.face.name);
  px::textShadow(c, buf, (W - px::textWidth(buf, 3)) / 2, 10, 3, kCream, kShadow);
  snprintf(buf, sizeof buf, "%s - DAY %u", d.stageName, (unsigned)d.ageDays + 1);
  px::textShadow(c, buf, (W - px::textWidth(buf, 1)) / 2, 36, 1, kCreamDim, kShadow);
  backChevron(c);
  // the pet, big, with its bowl
  uint16_t sh = px::mix(grass, kShadow, 120);
  px::rect(c, 60, kAppGroundY + 3, 120, 5, sh);
  drawBowl(c, 190, kAppGroundY + 8, 4, d.face.pantry);
  petart::draw(c, d.face.pose, kAppPetX, kAppPetY, kAppPetScale);
  // mood tag
  snprintf(buf, sizeof buf, "%s", d.moodName);
  int tw = px::textWidth(buf, 2);
  px::rect(c, (W - tw) / 2 - 6, 50, tw + 12, 20, kShadow);
  px::rect(c, (W - tw) / 2 - 5, 49, tw + 10, 20, kPanel);
  px::text(c, buf, (W - tw) / 2, 52, 2, kGold);
  // belly meter
  px::text(c, "BELLY", 10, kAppGroundY + 22, 2, kCreamDim);
  uint8_t full = (uint8_t)(d.fullness < 0 ? 0 : (d.fullness > 100 ? 100 : d.fullness));
  bar(c, 74, kAppGroundY + 19, 156, 16, full, kMint, hex(0xc9f7df), kTrack, hex(0x4a3650));
  for (int k = 1; k < 5; k++) px::rect(c, 74 + 2 + (152 * k) / 5, kAppGroundY + 21, 1, 12, kTrack);
  pageDots(c, Page::Pet);
  if (d.face.toast) toast(c, d.face, 76);
}

static void panelBg(Canvas &c) {
  px::fill(c, kPanel);
  for (int y = 0; y < H; y += 4) px::rect(c, 0, y, W, 1, hex(0x1e2547));
}

static void title(Canvas &c, const char *t) {
  backChevron(c);
  px::textShadow(c, t, (W - px::textWidth(t, 3)) / 2, 12, 3, kCream, kShadow);
}

static void statRow(Canvas &c, int y, petart::Icon icon, const char *label, const char *value,
                    uint16_t valueCol) {
  petart::drawIcon(c, icon, 10, y - 4, 2);
  px::text(c, label, 30, y, 2, kCreamDim);
  px::text(c, value, W - 14 - px::textWidth(value, 2), y, 2, valueCol);
}

static void appToday(Canvas &c, const AppData &d) {
  panelBg(c);
  title(c, "TODAY");
  char buf[40], a[16], b[16];
  px::fmtThousands(a, sizeof a, d.face.stepsToday);
  px::textShadow(c, a, (W - px::textWidth(a, 5)) / 2, 50, 5, kCream, kShadow);
  px::fmtThousands(b, sizeof b, d.face.goal);
  snprintf(buf, sizeof buf, "STEPS OF %s", b);
  px::text(c, buf, (W - px::textWidth(buf, 1)) / 2, 90, 1, kCreamDim);
  uint32_t pct = d.face.goal ? d.face.stepsToday * 100u / d.face.goal : 0;
  bar(c, 16, 102, 208, 14, (uint8_t)(pct > 100 ? 100 : pct), kGold, hex(0xffe9a8), kTrack, kPanelEdge);
  snprintf(buf, sizeof buf, "%u", d.snacksToday);
  statRow(c, 130, petart::Icon::Snack, "SNACKS", buf, kBerryLt);
  if (d.face.pose.stage == pet::Stage::Egg) snprintf(buf, sizeof buf, "%u TO HATCH", d.face.toSnack);
  else snprintf(buf, sizeof buf, "IN %u", d.face.toSnack);
  statRow(c, 152, petart::Icon::Bowl, "NEXT", buf, kCream);
  snprintf(buf, sizeof buf, "%u (BEST %u)", d.streak, d.bestStreak);
  statRow(c, 174, petart::Icon::Flame, "STREAK", buf, kGold);
  px::fmtThousands(a, sizeof a, d.lifetimeSteps);
  statRow(c, 196, petart::Icon::Foot, "ALL TIME", a, kCream);
  if (d.toNextStage) {
    px::fmtThousands(a, sizeof a, d.toNextStage);
    snprintf(buf, sizeof buf, "%s IN %s", d.nextStageName, a);
  } else {
    snprintf(buf, sizeof buf, "FULLY GROWN!");
  }
  statRow(c, 218, petart::Icon::Star, "GROWS", "", kCream);
  px::text(c, buf, W - 14 - px::textWidth(buf, 1), 222, 1, kCream);
  bar(c, 16, 240, 208, 10, d.stagePct, kMint, hex(0xc9f7df), kTrack, kPanelEdge);
  snprintf(buf, sizeof buf, "STREAK PRIZE: %s", d.accessoryName);
  px::text(c, buf, (W - px::textWidth(buf, 1)) / 2, 260, 1, kCreamDim);
  pageDots(c, Page::Today);
}

static void appWeek(Canvas &c, const AppData &d) {
  panelBg(c);
  title(c, "WEEK");
  static const char *kDay = "SMTWTFS";
  uint32_t mx = d.face.goal;
  uint64_t sum = 0;
  uint32_t best = 0;
  for (int i = 0; i < 7; i++) {
    if (d.week[i] > mx) mx = d.week[i];
    sum += d.week[i];
    if (d.week[i] > best) best = d.week[i];
  }
  const int x0 = 18, baseY = 220, top = 66, colW = 28;
  const int span = baseY - top;
  // goal line
  int gy = baseY - (int)((uint64_t)span * d.face.goal / mx);
  for (int x = x0 - 4; x < x0 + 7 * colW; x += 6) px::rect(c, x, gy, 3, 1, kGold);
  px::text(c, "GOAL", 196, gy - 9, 1, kGold);
  for (int i = 0; i < 7; i++) {
    int hgt = (int)((uint64_t)span * d.week[i] / mx);
    int x = x0 + i * colW;
    bool today = i == 6;
    uint16_t col = d.week[i] >= d.face.goal ? kMint : hex(0x5d6aa6);
    if (today) col = d.week[i] >= d.face.goal ? hex(0xb6f7d8) : hex(0x8b97d6);
    if (hgt > 0) {
      px::rect(c, x, baseY - hgt, colW - 8, hgt, col);
      px::rect(c, x, baseY - hgt, colW - 8, 2, px::mix(col, 0xFFFF, 110));
    }
    char lab[2] = {kDay[(d.weekDay0 + i) % 7], 0};
    px::text(c, lab, x + (colW - 8) / 2 - 5, baseY + 6, 2, today ? kCream : kCreamDim);
    if (d.week[i] > 0) {
      char v[16];
      if (d.week[i] >= 10000) snprintf(v, sizeof v, "%uK", (unsigned)(d.week[i] / 1000));
      else snprintf(v, sizeof v, "%u.%uK", (unsigned)(d.week[i] / 1000), (unsigned)((d.week[i] % 1000) / 100));
      px::text(c, v, x + (colW - 8) / 2 - px::textWidth(v, 1) / 2, baseY - hgt - 10, 1, kCream);
    }
  }
  char a[16], b[16], buf[48];
  px::fmtThousands(a, sizeof a, (uint32_t)(sum / 7));
  px::fmtThousands(b, sizeof b, best);
  snprintf(buf, sizeof buf, "AVG %s  BEST %s", a, b);
  px::text(c, buf, (W - px::textWidth(buf, 1)) / 2, 254, 1, kCreamDim);
  pageDots(c, Page::Week);
}

// Options layout
static const int kOptY0 = 52, kOptRowH = 28;

int appOptionAt(int x, int y) {
  int row = (y - kOptY0) / kOptRowH;
  if (y < kOptY0 || row < 0 || row > 6) return -1;
  switch (row) {
    case 0: return (int)OptRow::BgSteps;
    case 1: return (int)OptRow::PetFace;
    case 2: return (int)OptRow::SnackBuzz;
    case 3: return (int)OptRow::Nudges;
    case 4: return x < 120 ? (int)OptRow::GoalMinus : (int)OptRow::GoalPlus;
    case 5: return (int)OptRow::Rename;
    case 6: return (int)OptRow::Reset;
  }
  return -1;
}

static void toggleRow(Canvas &c, int row, const char *label, bool on, bool enabled = true) {
  int y = kOptY0 + row * kOptRowH;
  px::rect(c, 8, y, 224, kOptRowH - 4, kPanelLt);
  px::text(c, label, 16, y + 6, 2, enabled ? kCream : kCreamDim);
  int px0 = 186;
  px::rect(c, px0, y + 4, 38, 16, on ? hex(0x2f8a68) : hex(0x3a3f63));
  px::rect(c, on ? px0 + 22 : px0 + 2, y + 6, 14, 12, on ? kMint : kCreamDim);
}

static void appOptions(Canvas &c, const AppData &d) {
  panelBg(c);
  title(c, "OPTIONS");
  toggleRow(c, 0, d.bgAvailable ? "STEPS ASLEEP" : "STEPS ASLEEP N/A", d.optBgSteps && d.bgAvailable, d.bgAvailable);
  toggleRow(c, 1, "PET FACE", d.optPetFace);
  toggleRow(c, 2, "SNACK BUZZ", d.optSnackBuzz);
  toggleRow(c, 3, "NUDGES", d.optNudges);
  char a[16], buf[32];
  int y = kOptY0 + 4 * kOptRowH;
  px::rect(c, 8, y, 224, kOptRowH - 4, kPanelLt);
  px::text(c, "-", 18, y + 6, 2, kCream);
  px::text(c, "+", 210, y + 6, 2, kCream);
  px::fmtThousands(a, sizeof a, d.face.goal);
  snprintf(buf, sizeof buf, "GOAL %s", a);
  px::text(c, buf, (W - px::textWidth(buf, 2)) / 2, y + 6, 2, kGold);
  y += kOptRowH;
  px::rect(c, 8, y, 224, kOptRowH - 4, kPanelLt);
  snprintf(buf, sizeof buf, "NAME: %s", d.face.name);
  px::text(c, buf, 16, y + 6, 2, kCream);
  px::text(c, "}", 210, y + 6, 2, kCreamDim);
  y += kOptRowH;
  px::rect(c, 8, y, 224, kOptRowH - 4, hex(0x4a2232));
  if (d.resetArmPct) px::rect(c, 8, y, 224 * d.resetArmPct / 100, kOptRowH - 4, hex(0x9a2f45));
  px::text(c, "HOLD: NEW EGG", 16, y + 6, 2, kCream);
  pageDots(c, Page::Options);
}

void drawApp(Canvas &c, Page page, const AppData &d, uint32_t nowMs) {
  c.noClip();
  switch (page) {
    case Page::Pet:     appPet(c, d, nowMs); break;
    case Page::Today:   appToday(c, d); break;
    case Page::Week:    appWeek(c, d); break;
    case Page::Options: appOptions(c, d); break;
    default:            appPet(c, d, nowMs); break;
  }
}

}  // namespace scenes
