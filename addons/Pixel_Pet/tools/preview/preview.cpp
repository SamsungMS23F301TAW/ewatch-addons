// Host preview renderer for Pixel Pet. Compiles the watch's own pure drawing
// code (px, petart, scenes) with clang++ and writes PNG sheets, so the art
// can be reviewed (and iterated) without a watch.
//
//   tools/preview/build.sh [out_dir] [filter]   (default out_dir: docs)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "pngout.h"
#include "px.h"
#include "petart.h"
#include "petmodel.h"
#include "scenes.h"

using petart::Act;
using petart::Pose;
using pet::Accessory;
using pet::Mood;
using pet::Stage;

static std::string gOut = "docs";

struct Sheet {
  int w, h;
  std::vector<uint16_t> buf;
  px::Canvas c;
  Sheet(int W, int H, uint16_t bg) : w(W), h(H), buf((size_t)W * H, bg), c(buf.data(), (int16_t)W, (int16_t)H) {}
  void save(const char *name, int up = 1) {
    std::string path = gOut + "/" + name;
    if (!pngout::write565(path.c_str(), buf.data(), w, h, up)) {
      fprintf(stderr, "failed to write %s\n", path.c_str());
      exit(1);
    }
    printf("  wrote %s (%dx%d)\n", path.c_str(), w * up, h * up);
  }
};

static const uint16_t kSheetBg  = px::hex(0x10142a);
static const uint16_t kCellBg   = px::hex(0x1e2747);
static const uint16_t kCellBg2  = px::hex(0xdfe8f0);
static const uint16_t kLabel    = px::hex(0xf5e9c8);
static const uint16_t kLabelDim = px::hex(0x7f8bb0);

// A row of frames of one pose sampled at the given times.
static void row(Sheet &s, int y, const char *label, Pose base, const std::vector<uint32_t> &times,
                bool useAnim, int scale, uint16_t cellBg = kCellBg) {
  px::text(s.c, label, 8, y + 4, 2, kLabel);
  int x = 8;
  int cy = y + 22;
  const int cw = petart::kW * scale, ch = petart::kH * scale;
  for (uint32_t t : times) {
    px::rect(s.c, x, cy, cw, ch, cellBg);
    Pose p = base;
    if (useAnim) { p.animMs = t; p.clockMs = 10000 + t; } else { p.clockMs = t; }
    petart::draw(s.c, p, x, cy, scale);
    char tl[16];
    snprintf(tl, sizeof tl, "%u", (unsigned)t);
    px::text(s.c, tl, x + 2, cy + ch - 9, 1, kLabelDim);
    x += cw + 6;
  }
}

static std::vector<uint32_t> span(uint32_t start, uint32_t step, int n) {
  std::vector<uint32_t> v;
  for (int i = 0; i < n; i++) v.push_back(start + step * (uint32_t)i);
  return v;
}

static void sheetMoods() {
  const int scale = 4, n = 9;
  const int rowH = 22 + petart::kH * scale + 10;
  Sheet s(8 + n * (petart::kW * scale + 6), 8 + 7 * rowH, kSheetBg);
  Pose p;
  p.stage = Stage::Kid;
  const char *names[] = {"ECSTATIC - JUMPS FOR JOY", "HAPPY - BOUNCY, WAVES", "CONTENT - BREATHES, BLINKS",
                         "PECKISH - TUMMY RUMBLES", "GRUMPY - ARMS CROSSED, FOOT TAP",
                         "FURIOUS - STORM CLOUD STOMP"};
  for (int m = 0; m < 6; m++) {
    p.mood = (Mood)m;
    p.sulking = false;
    // loop moods sample fast; slow moods sample across their events
    std::vector<uint32_t> times = (m == 0 || m == 5) ? span(0, 125, n)
                                : (m == 4) ? span(0, 300, n) : span(0, 650, n);
    row(s, 8 + m * rowH, names[m], p, times, false, scale);
  }
  p.mood = Mood::Furious;
  p.sulking = true;
  row(s, 8 + 6 * rowH, "SULKING - FURIOUS FOR 2H, BACK TURNED", p, span(0, 700, n), false, scale);
  s.save("sheet_moods.png");
}

static void sheetActs() {
  const int scale = 4, n = 9;
  const int rowH = 22 + petart::kH * scale + 10;
  const int rows = 10;
  Sheet s(8 + n * (petart::kW * scale + 6), 8 + rows * rowH, kSheetBg);
  Pose p;
  p.stage = Stage::Kid;
  int r = 0;
  p.mood = Mood::Happy; p.act = Act::Walk;
  row(s, 8 + r++ * rowH, "WALKING ALONG (HAPPY)", p, span(0, 95, n), false, scale);
  p.mood = Mood::Grumpy;
  row(s, 8 + r++ * rowH, "WALKING ALONG (GRUMPY)", p, span(0, 95, n), false, scale);
  p.mood = Mood::Content; p.act = Act::Eat;
  row(s, 8 + r++ * rowH, "EATING A SNACK", p, span(0, 200, n), true, scale);
  p.mood = Mood::Happy; p.act = Act::Petted;
  row(s, 8 + r++ * rowH, "PETTED (HAPPY)", p, span(0, 170, n), true, scale);
  p.mood = Mood::Grumpy;
  row(s, 8 + r++ * rowH, "PETTED (GRUMPY) - HMPH", p, span(0, 170, n), true, scale);
  p.mood = Mood::Furious;
  row(s, 8 + r++ * rowH, "PETTED (FURIOUS)", p, span(0, 170, n), true, scale);
  p.mood = Mood::Grumpy; p.act = Act::Nudge;
  row(s, 8 + r++ * rowH, "NUDGE (WITH THE BUZZ)", p, span(0, 180, n), true, scale);
  p.mood = Mood::Happy; p.act = Act::Celebrate;
  row(s, 8 + r++ * rowH, "GOAL / EVOLVE CELEBRATION", p, span(0, 140, n), true, scale);
  p.act = Act::Idle; p.asleep = true; p.mood = Mood::Content;
  row(s, 8 + r++ * rowH, "ASLEEP AT NIGHT", p, span(0, 600, n), false, scale);
  p.mood = Mood::Grumpy;
  row(s, 8 + r++ * rowH, "ASLEEP, HUNGRY", p, span(0, 600, n), false, scale);
  s.save("sheet_acts.png");
}

static void sheetLife() {
  const int scale = 4, n = 9;
  const int rowH = 22 + petart::kH * scale + 10;
  const int rows = 7;
  Sheet s(8 + n * (petart::kW * scale + 6), 8 + rows * rowH, kSheetBg);
  Pose p;
  int r = 0;
  p.stage = Stage::Egg;
  {
    px::text(s.c, "EGG: WALK TO HATCH (0% 40% 70% 95%), THEN HATCHING", 8, 8 + r * rowH + 4, 2, kLabel);
    int x = 8, cy = 8 + r * rowH + 22;
    const uint8_t pcts[] = {0, 0, 40, 40, 70, 70, 95, 95};
    const uint32_t ts[] = {0, 1700, 0, 1900, 0, 1000, 0, 700};
    for (int i = 0; i < 8; i++) {
      px::rect(s.c, x, cy, petart::kW * scale, petart::kH * scale, kCellBg);
      Pose e = p; e.eggPct = pcts[i]; e.clockMs = ts[i];
      petart::draw(s.c, e, x, cy, scale);
      x += petart::kW * scale + 6;
    }
    r++;
  }
  p.act = Act::Hatch;
  row(s, 8 + r++ * rowH, "HATCHING", p, span(0, 300, n), true, scale);
  p.act = Act::Idle;
  const char *stageNames[] = {"", "BABY", "KID", "ADULT", "ELDER"};
  for (int st = 1; st <= 4; st++) {
    char label[64];
    snprintf(label, sizeof label, "%s: HAPPY, CONTENT, PECKISH, GRUMPY, FURIOUS, ASLEEP", stageNames[st]);
    px::text(s.c, label, 8, 8 + r * rowH + 4, 2, kLabel);
    int x = 8, cy = 8 + r * rowH + 22;
    for (int k = 0; k < 9; k++) {
      px::rect(s.c, x, cy, petart::kW * scale, petart::kH * scale, kCellBg);
      Pose q;
      q.stage = (Stage)st;
      q.clockMs = 400 + 137 * k;
      if (k < 5) q.mood = (Mood)(k + 1);
      else if (k == 5) { q.mood = Mood::Content; q.asleep = true; }
      else if (k == 6) { q.mood = Mood::Ecstatic; q.clockMs = 400; }
      else if (k == 7) { q.mood = Mood::Happy; q.act = Act::Walk; q.clockMs = 100; }
      else { q.mood = Mood::Furious; q.sulking = true; }
      petart::draw(s.c, q, x, cy, scale);
      x += petart::kW * scale + 6;
    }
    r++;
  }
  // accessories
  {
    px::text(s.c, "STREAK REWARDS: NONE, BOW (3D), PARTY HAT (7D), FLOWERS (14D), CROWN (30D)", 8, 8 + r * rowH + 4, 2, kLabel);
    int x = 8, cy = 8 + r * rowH + 22;
    for (int k = 0; k < 5; k++) {
      px::rect(s.c, x, cy, petart::kW * scale, petart::kH * scale, kCellBg);
      Pose q;
      q.stage = Stage::Adult;
      q.mood = Mood::Happy;
      q.acc = (Accessory)k;
      q.clockMs = 500;
      petart::draw(s.c, q, x, cy, scale);
      x += petart::kW * scale + 6;
    }
    // on a light background too (outline check)
    for (int k = 0; k < 4; k++) {
      px::rect(s.c, x, cy, petart::kW * scale, petart::kH * scale, kCellBg2);
      Pose q;
      q.stage = Stage::Kid;
      q.mood = (Mood)(k * 1 + (k > 1 ? 2 : 1));
      q.clockMs = 900;
      petart::draw(s.c, q, x, cy, scale);
      x += petart::kW * scale + 6;
    }
  }
  s.save("sheet_life.png");
}

static void closeups() {
  // One big frame per mood: the "at a glance" check, at the watch's x4.
  const int scale = 4;
  const int cw = petart::kW * scale, ch = petart::kH * scale;
  Sheet s(3 * (cw + 8) + 8, 2 * (ch + 30) + 8, kSheetBg);
  for (int m = 0; m < 6; m++) {
    int x = 8 + (m % 3) * (cw + 8), y = 8 + (m / 3) * (ch + 30);
    px::rect(s.c, x, y + 22, cw, ch, kCellBg);
    Pose p;
    p.stage = Stage::Kid;
    p.mood = (Mood)m;
    p.clockMs = (m == 0) ? 400 : (m == 5 ? 100 : 1000);
    petart::draw(s.c, p, x, y + 22, scale);
    px::text(s.c, pet::Model::moodName((Mood)m), x, y + 4, 2, kLabel);
  }
  s.save("closeup_moods.png", 2);
}

// ---------------------------------------------------------------------------
// Full-screen mockups (240x280, what the watch shows)
// ---------------------------------------------------------------------------
struct FaceCase {
  const char *file, *label;
  uint8_t h, m;
  Mood mood;
  Stage stage;
  Act act;
  bool asleep, sulking, walking;
  uint32_t steps;
  uint8_t snackPct, pantry, eggPct;
  uint16_t toSnack;
  const char *toast;
  uint32_t clock;
};

static scenes::FaceData faceFor(const FaceCase &fc) {
  scenes::FaceData d;
  d.hour = fc.h; d.minute = fc.m; d.second = 7;
  d.weekday = 3; d.day = 1; d.month = 10;
  d.batOk = true; d.batPct = 72;
  d.stepsToday = fc.steps; d.goal = 6000;
  d.snackPct = fc.snackPct; d.toSnack = fc.toSnack; d.pantry = fc.pantry;
  d.walking = fc.walking;
  d.pose.stage = fc.stage; d.pose.mood = fc.mood; d.pose.act = fc.act;
  d.pose.asleep = fc.asleep; d.pose.sulking = fc.sulking;
  d.pose.clockMs = fc.clock; d.pose.animMs = fc.clock % 1700; d.pose.eggPct = fc.eggPct;
  d.name = "BIX";
  d.toast = fc.toast; d.toastAgeMs = 200;
  return d;
}

static const FaceCase kFaces[] = {
  {"face_happy.png",     "HAPPY 14:25",       14, 25, Mood::Happy,    Stage::Kid,   Act::Idle, false, false, false, 4210, 62, 1, 0, 152, nullptr, 300},
  {"face_ecstatic.png",  "ECSTATIC 12:40",    12, 40, Mood::Ecstatic, Stage::Kid,   Act::Idle, false, false, false, 7120, 20, 3, 0, 320, nullptr, 400},
  {"face_walking.png",   "WALKING 08:12",      8, 12, Mood::Content,  Stage::Kid,   Act::Walk, false, false, true,  1835, 85, 0, 0, 60,  nullptr, 120},
  {"face_eating.png",    "SNACK! 08:20",       8, 20, Mood::Content,  Stage::Kid,   Act::Eat,  false, false, true,  2240, 2,  0, 0, 392, "+1 SNACK!", 200},
  {"face_content.png",   "CONTENT 10:05",     10,  5, Mood::Content,  Stage::Adult, Act::Idle, false, false, false, 2900, 40, 0, 0, 240, nullptr, 1000},
  {"face_peckish.png",   "PECKISH 15:30",     15, 30, Mood::Peckish,  Stage::Kid,   Act::Idle, false, false, false, 1650, 15, 0, 0, 340, nullptr, 2600},
  {"face_grumpy.png",    "GRUMPY 18:40",      18, 40, Mood::Grumpy,   Stage::Kid,   Act::Idle, false, false, false, 1210, 30, 0, 0, 280, nullptr, 0},
  {"face_furious.png",   "FURIOUS 19:55",     19, 55, Mood::Furious,  Stage::Kid,   Act::Idle, false, false, false, 640,  60, 0, 0, 160, nullptr, 125},
  {"face_sulking.png",   "SULKING 20:45",     20, 45, Mood::Furious,  Stage::Kid,   Act::Idle, false, true,  false, 640,  60, 0, 0, 160, nullptr, 2100},
  {"face_night.png",     "ASLEEP 23:48",      23, 48, Mood::Content,  Stage::Kid,   Act::Idle, true,  false, false, 6420, 55, 0, 0, 180, nullptr, 900},
  {"face_egg.png",       "EGG 07:42",          7, 42, Mood::Content,  Stage::Egg,   Act::Idle, false, false, false, 120,  40, 0, 40, 180, nullptr, 900},
  {"face_baby.png",      "BABY 06:55",         6, 55, Mood::Happy,    Stage::Baby,  Act::Idle, false, false, false, 350,  12, 0, 0, 350, nullptr, 700},
};

static void faces() {
  const int n = (int)(sizeof(kFaces) / sizeof(kFaces[0]));
  const int cols = 4, gap = 12, lab = 22;
  const int rows = (n + cols - 1) / cols;
  Sheet sheet(cols * (scenes::W + gap) + gap, rows * (scenes::H + gap + lab) + gap, kSheetBg);
  for (int i = 0; i < n; i++) {
    std::vector<uint16_t> buf(scenes::W * scenes::H, 0);
    px::Canvas c(buf.data(), scenes::W, scenes::H);
    scenes::FaceData d = faceFor(kFaces[i]);
    scenes::drawFaceAll(c, d, 5000 + kFaces[i].clock);
    std::string path = gOut + "/" + kFaces[i].file;
    pngout::write565(path.c_str(), buf.data(), scenes::W, scenes::H, 2);
    int x = gap + (i % cols) * (scenes::W + gap), y = gap + (i / cols) * (scenes::H + gap + lab);
    px::text(sheet.c, kFaces[i].label, x, y, 2, kLabel);
    for (int yy = 0; yy < scenes::H; yy++)
      memcpy(&sheet.buf[(size_t)(y + lab + yy) * sheet.w + x], &buf[(size_t)yy * scenes::W], scenes::W * 2);
  }
  sheet.save("sheet_faces.png");
}

static void appPages() {
  scenes::AppData d;
  d.face = faceFor(kFaces[0]);
  d.face.pantry = 2;
  d.stageName = "KID"; d.moodName = "HAPPY"; d.fullness = 78;
  d.snacksToday = 10; d.streak = 4; d.bestStreak = 9;
  d.lifetimeSteps = 61240; d.stagePct = 38; d.toNextStage = 58760; d.nextStageName = "ADULT";
  d.ageDays = 11;
  const uint32_t wk[7] = {6230, 4120, 8840, 2010, 7010, 6555, 4210};
  for (int i = 0; i < 7; i++) d.week[i] = wk[i];
  d.weekDay0 = 4;
  d.accessoryName = "PARTY HAT";
  d.face.pose.acc = Accessory::PartyHat;
  const int gap = 12;
  Sheet sheet(4 * (scenes::W + gap) + gap, scenes::H + 2 * gap, kSheetBg);
  const char *files[] = {"app_pet.png", "app_today.png", "app_week.png", "app_options.png"};
  for (int i = 0; i < 4; i++) {
    std::vector<uint16_t> buf(scenes::W * scenes::H, 0);
    px::Canvas c(buf.data(), scenes::W, scenes::H);
    scenes::drawApp(c, (scenes::Page)i, d, 6000);
    std::string path = gOut + "/" + files[i];
    pngout::write565(path.c_str(), buf.data(), scenes::W, scenes::H, 2);
    int x = gap + i * (scenes::W + gap);
    for (int yy = 0; yy < scenes::H; yy++)
      memcpy(&sheet.buf[(size_t)(gap + yy) * sheet.w + x], &buf[(size_t)yy * scenes::W], scenes::W * 2);
  }
  sheet.save("sheet_app.png");
}

// ---------------------------------------------------------------------------
// story: a grumpy pet walked back to happiness, frame by frame (10 fps).
// Frames go to <out>/story_NNN.png; tools/preview/story.py makes the GIF.
// ---------------------------------------------------------------------------
static void story() {
  struct Beat { uint32_t t0; Mood mood; Act act; bool walking; const char *toast; };
  // (time ms, mood, act, walking, caption)
  static const Beat beats[] = {
    {0,     Mood::Grumpy,  Act::Idle, false, nullptr},
    {2200,  Mood::Grumpy,  Act::Walk, true,  nullptr},
    {4400,  Mood::Peckish, Act::Eat,  true,  "+1 SNACK!"},
    {6100,  Mood::Peckish, Act::Walk, true,  nullptr},
    {8000,  Mood::Content, Act::Eat,  true,  "+1 SNACK!"},
    {9700,  Mood::Happy,   Act::Walk, true,  nullptr},
    {11400, Mood::Happy,   Act::Petted, false, "HEE HEE!"},
    {12900, Mood::Happy,   Act::Idle, false, nullptr},
    {14000, Mood::Happy,   Act::Idle, false, nullptr},
  };
  const int nb = (int)(sizeof(beats) / sizeof(beats[0]));
  std::vector<uint16_t> buf(scenes::W * scenes::H, 0);
  px::Canvas c(buf.data(), scenes::W, scenes::H);
  int frame = 0;
  for (uint32_t t = 0; t < beats[nb - 1].t0; t += 100, frame++) {
    int b = 0;
    while (b + 1 < nb && t >= beats[b + 1].t0) b++;
    const Beat &bt = beats[b];
    scenes::FaceData d;
    d.hour = 18; d.minute = 5 + (uint8_t)(t / 60000); d.second = (uint8_t)((t / 1000) % 60);
    d.colonOn = ((t / 1000) % 2) == 0;
    d.weekday = 3; d.day = 1; d.month = 10;
    d.batOk = true; d.batPct = 64;
    // Time-lapse step count, scripted so the snack bar fills exactly as each
    // snack lands (at 4.4 s and 8.0 s).
    static const uint32_t kt[] = {0, 2200, 4400, 8000, 9700, 11400, 100000};
    static const uint32_t kw[] = {0, 0,    100,  500,  600,  700,   700};
    uint32_t walked = 0;
    for (int k = 0; k < 6; k++) {
      if (t >= kt[k] && t < kt[k + 1]) {
        walked = kw[k] + (kw[k + 1] - kw[k]) * (t - kt[k]) / (kt[k + 1] - kt[k]);
        break;
      }
    }
    d.stepsToday = 1210 + walked;
    uint32_t bank = (300 + walked) % 400;
    d.toSnack = (uint16_t)(400 - bank);
    d.snackPct = (uint8_t)(bank * 100 / 400);
    d.pantry = 0;
    d.walking = bt.walking;
    d.pose.stage = Stage::Kid;
    d.pose.mood = bt.mood;
    d.pose.act = bt.act;
    d.pose.clockMs = 30000 + t;
    d.pose.animMs = t - bt.t0;
    d.toast = bt.toast;
    d.toastAgeMs = t - bt.t0;
    if (bt.toast && t - bt.t0 > 1600) d.toast = nullptr;
    scenes::drawFaceAll(c, d, 30000 + t);
    char name[64];
    snprintf(name, sizeof name, "%s/story_%03d.png", gOut.c_str(), frame);
    pngout::write565(name, buf.data(), scenes::W, scenes::H, 1);
  }
  printf("  wrote %d story frames\n", frame);
}

// ---------------------------------------------------------------------------
// icon.svg — generated from the real sprite so the marketplace icon matches
// the watch exactly: a happy kid Stepmunk on a flat sky tile.
// ---------------------------------------------------------------------------
static void iconSvg(const char *path) {
  uint8_t idx[petart::kW * petart::kH];
  Pose p;
  p.stage = Stage::Kid;
  p.mood = Mood::Happy;
  p.clockMs = 200;                       // eyes open, standing
  const uint16_t *pal = petart::compose(p, idx);
  int x0 = petart::kW, y0 = petart::kH, x1 = -1, y1 = -1;
  for (int y = 0; y < petart::kH; y++)
    for (int x = 0; x < petart::kW; x++)
      if (idx[y * petart::kW + x]) {
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
      }
  const int pw = x1 - x0 + 1, ph = y1 - y0 + 1;
  const int unit = 4, size = 128;
  const int ox = (size - pw * unit) / 2, oy = size - 20 - ph * unit + 4;
  FILE *f = fopen(path, "w");
  if (!f) { perror(path); exit(1); }
  fprintf(f, "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 %d %d\" width=\"%d\" height=\"%d\" shape-rendering=\"crispEdges\">\n",
          size, size, size, size);
  fprintf(f, "  <title>Stepmunk</title>\n");
  fprintf(f, "  <defs><clipPath id=\"tile\"><rect width=\"%d\" height=\"%d\" rx=\"26\"/></clipPath></defs>\n", size, size);
  fprintf(f, "  <g clip-path=\"url(#tile)\">\n");
  fprintf(f, "  <rect width=\"%d\" height=\"%d\" fill=\"#5aa9f0\"/>\n", size, size);
  fprintf(f, "  <rect y=\"%d\" width=\"%d\" height=\"24\" fill=\"#5fc25c\"/>\n", size - 24, size);
  fprintf(f, "  <rect y=\"%d\" width=\"%d\" height=\"4\" fill=\"#8be07a\"/>\n", size - 24, size);
  for (int y = y0; y <= y1; y++) {
    int x = x0;
    while (x <= x1) {
      uint8_t v = idx[y * petart::kW + x];
      if (!v) { x++; continue; }
      int run = 1;
      while (x + run <= x1 && idx[y * petart::kW + x + run] == v) run++;
      uint16_t c = pal[v];
      unsigned r = ((c >> 11) & 31) * 255 / 31, g = ((c >> 5) & 63) * 255 / 63, b = (c & 31) * 255 / 31;
      fprintf(f, "    <rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" fill=\"#%02x%02x%02x\"/>\n",
              ox + (x - x0) * unit, oy + (y - y0) * unit, run * unit, unit, r, g, b);
      x += run;
    }
  }
  fprintf(f, "  </g>\n</svg>\n");   // closes the clip group
  fclose(f);
  printf("  wrote %s\n", path);
}

int main(int argc, char **argv) {
  if (argc > 1) gOut = argv[1];
  const char *only = argc > 2 ? argv[2] : "";
  auto want = [&](const char *k) { return !*only || strstr(only, k); };
  if (want("moods")) sheetMoods();
  if (want("acts")) sheetActs();
  if (want("life")) sheetLife();
  if (want("close")) closeups();
  if (want("faces")) faces();
  if (want("app")) appPages();
  if (strstr(only, "icon")) iconSvg((gOut + "/icon.svg").c_str());
  if (strstr(only, "story")) story();
  return 0;
}
