// Pixel Pet art. All sprite data in this file is original.
//
// Layering per frame (back to front): feet -> body (procedural, with belly
// and cel shading) -> face -> arms -> head accessories -> effects (hearts,
// storm cloud, steam, Zs, crumbs...). Stamps are ASCII grids; see charIx()
// for the palette letters. Every frame is a pure function of the Pose.
#include "petart.h"
#include <math.h>
#include <string.h>

using pet::Accessory;
using pet::Mood;
using pet::Stage;

namespace petart {

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------
enum Ix : uint8_t {
  T_ = 0, OUT = 1, BODY, SHADE, LIGHT, WHITE, INK, CHEEK, MOUTH, TONGUE,
  GOLD, HEART, CLOUD, CLOUD_D, BOLT, WATER, STEAM, BERRY, LEAF, HAT, HAT_B,
  EGG, EGG_SPOT, EGG_SHADE, NIGHTCAP, PALE, DUST, BOW, PETAL, FOOT, ANGER,
  EDGE, IX_COUNT
};

static uint8_t charIx(char ch) {
  switch (ch) {
    case 'o': return OUT;      case 'b': return BODY;      case 's': return SHADE;
    case 'l': return LIGHT;    case 'w': return WHITE;     case 'k': return INK;
    case 'c': return CHEEK;    case 'm': return MOUTH;     case 't': return TONGUE;
    case 'y': return GOLD;     case 'r': return HEART;     case 'g': return CLOUD;
    case 'G': return CLOUD_D;  case 'z': return BOLT;      case 'u': return WATER;
    case 'e': return STEAM;    case 'R': return BERRY;     case 'L': return LEAF;
    case 'h': return HAT;      case 'H': return HAT_B;     case 'E': return EGG;
    case 'p': return EGG_SPOT; case 'S': return EGG_SHADE; case 'n': return NIGHTCAP;
    case 'Z': return PALE;     case 'd': return DUST;      case 'B': return BOW;
    case 'f': return PETAL;    case 'F': return FOOT;      case 'a': return ANGER;
    case 'O': return EDGE;
    default:  return T_;
  }
}

struct BodyCols { uint32_t body, shade, light, foot; };

// Per stage: the baby is pale mint and deepens as it grows; elders go sage-teal.
static const BodyCols kStageCols[] = {
  {0x9fefca, 0x67cfa3, 0xe0fdee, 0x4fae88},   // egg (body unused)
  {0x9fefca, 0x67cfa3, 0xe0fdee, 0x4fae88},   // baby
  {0x7fe3b6, 0x4fbb8f, 0xcdf8e2, 0x3c9d77},   // kid
  {0x66d6a6, 0x3fa47f, 0xbff2d8, 0x2f8a68},   // adult
  {0x62c9b6, 0x3a978a, 0xc8f2ea, 0x2c7d72},   // elder
};

static uint16_t gPal[IX_COUNT];

static void basePalette() {
  gPal[T_]        = 0;
  gPal[OUT]       = px::hex(0x1d2b3a);
  gPal[WHITE]     = px::hex(0xffffff);
  gPal[INK]       = px::hex(0x14162a);
  gPal[CHEEK]     = px::hex(0xff8fa6);
  gPal[MOUTH]     = px::hex(0x6e2236);
  gPal[TONGUE]    = px::hex(0xff6f8a);
  gPal[GOLD]      = px::hex(0xffcf4a);
  gPal[HEART]     = px::hex(0xff4d6d);
  gPal[CLOUD]     = px::hex(0xaab4c8);
  gPal[CLOUD_D]   = px::hex(0x5f6880);
  gPal[BOLT]      = px::hex(0xfff275);
  gPal[WATER]     = px::hex(0x6fd0ff);
  gPal[STEAM]     = px::hex(0xeef2f7);
  gPal[BERRY]     = px::hex(0xff5468);
  gPal[LEAF]      = px::hex(0x5cc24a);
  gPal[HAT]       = px::hex(0xa45cff);
  gPal[HAT_B]     = px::hex(0xffd84a);
  gPal[EGG]       = px::hex(0xfff3dc);
  gPal[EGG_SPOT]  = px::hex(0x8fd7b8);
  gPal[EGG_SHADE] = px::hex(0xe2c9a4);
  gPal[NIGHTCAP]  = px::hex(0x7a6ff0);
  gPal[PALE]      = px::hex(0xc2d8ff);
  gPal[DUST]      = px::hex(0xd8c8aa);
  gPal[BOW]       = px::hex(0xff7ab8);
  gPal[PETAL]     = px::hex(0xfff6fb);
  gPal[ANGER]     = px::hex(0xff3b4e);
  gPal[EDGE]      = px::hex(0x1d2440);
}

static void bodyPalette(const Pose &p) {
  int si = (int)p.stage;
  if (si < 0 || si > 4) si = 1;
  const BodyCols &bc = kStageCols[si];
  uint16_t body = px::hex(bc.body), shade = px::hex(bc.shade);
  uint16_t light = px::hex(bc.light), foot = px::hex(bc.foot);
  // Mood tints: sour olive when grumpy, flushed red when furious, a little
  // washed-out when peckish. Pose and face carry the mood too, so it still
  // reads for colour-blind wearers.
  int tint = 0;
  uint16_t to = 0;
  if (!p.asleep) {
    switch (p.mood) {
      case Mood::Peckish: tint = 50;  to = px::hex(0xc8cdb8); break;
      case Mood::Grumpy:  tint = 110; to = px::hex(0xc9b85c); break;
      case Mood::Furious: tint = 185; to = px::hex(0xf2565f); break;
      default: break;
    }
    // A sulk is a different feeling from a tantrum: dusky and drained.
    if (p.sulking && p.mood == Mood::Furious) { tint = 200; to = px::hex(0x9b5fae); }
  }
  if (tint) {
    body  = px::mix(body, to, tint);
    shade = px::mix(shade, px::mix(to, 0, 80), tint);
    light = px::mix(light, px::mix(to, 0xFFFF, 120), tint);
    foot  = px::mix(foot, px::mix(to, 0, 120), tint);
  }
  gPal[BODY] = body;
  gPal[SHADE] = shade;
  gPal[LIGHT] = light;
  gPal[FOOT] = foot;
  // A coloured outline (a deep shade of the body) keeps the silhouette crisp
  // on both the dark night scene and the light day sky.
  gPal[OUT] = px::mix(shade, gPal[INK], 150);
}

// ---------------------------------------------------------------------------
// Buffer + stamps
// ---------------------------------------------------------------------------
struct Buf {
  uint8_t *px;
  void set(int x, int y, uint8_t v) {
    if (x < 0 || y < 0 || x >= kW || y >= kH || v == T_) return;
    px[y * kW + x] = v;
  }
  void force(int x, int y, uint8_t v) {
    if (x < 0 || y < 0 || x >= kW || y >= kH) return;
    px[y * kW + x] = v;
  }
  uint8_t get(int x, int y) const {
    if (x < 0 || y < 0 || x >= kW || y >= kH) return T_;
    return px[y * kW + x];
  }
};

struct Stamp { uint8_t w, h; const char *rows; };
// Compile-time checked: every stamp string must be exactly w*h characters.
#define DEF_STAMP(name, w, h, str)                                   \
  static const Stamp name = {(uint8_t)(w), (uint8_t)(h), str};      \
  static_assert(sizeof(str) - 1 == (w) * (h), #name " size mismatch")

static void put(Buf &b, const Stamp &s, int x, int y, bool flip = false) {
  for (int r = 0; r < s.h; r++)
    for (int c = 0; c < s.w; c++) {
      char ch = s.rows[r * s.w + (flip ? (s.w - 1 - c) : c)];
      b.set(x + c, y + r, charIx(ch));
    }
}

// Stamp with an automatic 1-px outline around its opaque pixels (effects
// floating in the scene need it to read against any background).
static void putOutlined(Buf &b, const Stamp &s, int x, int y, bool flip = false) {
  auto opaque = [&](int rr, int cc) {
    if (rr < 0 || cc < 0 || rr >= s.h || cc >= s.w) return false;
    return charIx(s.rows[rr * s.w + (flip ? (s.w - 1 - cc) : cc)]) != T_;
  };
  for (int r = -1; r <= s.h; r++)
    for (int c = -1; c <= s.w; c++) {
      if (opaque(r, c)) continue;
      if (opaque(r - 1, c) || opaque(r + 1, c) || opaque(r, c - 1) || opaque(r, c + 1))
        b.set(x + c, y + r, EDGE);
    }
  put(b, s, x, y, flip);
}

// ---- face (left-eye / left-brow orientation; mirrored for the right) -------
DEF_STAMP(kEyeOpen, 4, 4, ".kk." "kwkk" "kkkk" ".kk.");
DEF_STAMP(kEyeSpark, 4, 4, ".kk." "kwkk" "kkwk" ".kk.");
DEF_STAMP(kEyeShut, 4, 4, "...." "...." "kkkk" "....");
DEF_STAMP(kEyeHappy, 4, 4, "...." ".kk." "k..k" "....");
DEF_STAMP(kEyeSleep, 4, 4, "...." "...." "k..k" ".kk.");
DEF_STAMP(kEyeGlare, 4, 4, "...." "kkkk" "kwkk" ".kk.");
DEF_STAMP(kEyeSquint, 4, 4, "k..." ".kk." ".kk." "k...");

DEF_STAMP(kBrowAngry, 4, 2, "kk.." "..kk");
DEF_STAMP(kBrowWorried, 4, 2, "..kk" "kk..");
DEF_STAMP(kBrowFurious, 5, 2, "kkk.." "..kkk");

DEF_STAMP(kMouthSmile, 4, 2, "k..k" ".kk.");
DEF_STAMP(kMouthGrin, 6, 4, "kkkkkk" "kmmmmk" ".kttk." "..kk..");
DEF_STAMP(kMouthFlat, 4, 1, "kkkk");
DEF_STAMP(kMouthFrown, 4, 2, ".kk." "k..k");
DEF_STAMP(kMouthScowl, 6, 2, ".kkkk." "k....k");
DEF_STAMP(kMouthPout, 4, 2, ".kk." "kkkk");
DEF_STAMP(kMouthO, 4, 3, ".kk." "kmmk" ".kk.");
DEF_STAMP(kMouthWavy, 5, 2, ".k.k." "k.k.k");
DEF_STAMP(kMouthTeeth, 6, 3, "kkkkkk" "kwwkwk" "kkkkkk");
DEF_STAMP(kMouthChomp, 6, 4, "kkkkkk" "kmmmmk" "kmttmk" ".kkkk.");
DEF_STAMP(kMouthChew, 4, 2, "kkkk" ".mm.");
DEF_STAMP(kMouthLick, 5, 3, "k..k." ".kkt." "...tt");
DEF_STAMP(kMouthTiny, 2, 1, "kk");

DEF_STAMP(kCheek, 2, 1, "cc");
DEF_STAMP(kVein, 5, 5, ".a.a." "aa.aa" "....." "aa.aa" ".a.a.");
DEF_STAMP(kSweat, 2, 3, ".u" "uu" "uu");

// ---- limbs (left side; mirrored for the right) -----------------------------
DEF_STAMP(kArmRest, 3, 5, ".o." "obo" "obo" "obo" ".o.");
DEF_STAMP(kArmUp, 5, 5, "oo..." "obo.." "obbo." ".obbo" "..obo");
DEF_STAMP(kArmOut, 5, 3, ".ooo." "obbbo" ".ooo.");
DEF_STAMP(kArmFist, 4, 5, ".oo." "obbo" "obbo" "obbo" ".oob");
DEF_STAMP(kArmWave, 5, 6, ".oo.." "obbo." "obbo." ".obbo" "..obo" "...oo");
DEF_STAMP(kFoot, 6, 3, ".oooo." "oFFFFo" ".oooo.");
DEF_STAMP(kPaw, 3, 3, ".o." "obo" ".o.");
DEF_STAMP(kMitt, 4, 3, ".oo." "obbo" ".oo.");

// ---- head accessories ------------------------------------------------------
DEF_STAMP(kSprout, 5, 4, "L...L" "LL.LL" ".LLL." "..L..");
DEF_STAMP(kEar, 5, 4, ".ooo." "oblbo" "oblbo" "obbbo");
DEF_STAMP(kEarBack, 5, 4, ".ooo." "obbbo" "obbbo" "obbbo");
DEF_STAMP(kTail, 5, 4, "..oo." ".obbo" "obbbo" ".ooo.");
DEF_STAMP(kBow, 7, 3, "BB...BB" "BBBoBBB" "BB...BB");
DEF_STAMP(kPartyHat, 5, 6, "..H.." "..h.." ".hHh." ".hhh." "hHhHh" "hhhhh");
DEF_STAMP(kFlower, 3, 3, ".f." "fyf" ".f.");
DEF_STAMP(kCrown, 7, 5, "y..y..y" "y.yyy.y" "yyyyyyy" "yryyyry" "yyyyyyy");
DEF_STAMP(kStarMark, 3, 3, ".y." "yyy" ".y.");
DEF_STAMP(kNightcap, 13, 7, ".....nnnn...." "...nnnnnnnn.." "..nnnnnnnnnnn" ".nnnnnnnnn.nn" "wwwwwwwwwww.n" "...........ww" "...........ww");

// ---- effects -----------------------------------------------------------------
DEF_STAMP(kHeart, 5, 4, "rr.rr" "rrrrr" ".rrr." "..r..");
DEF_STAMP(kHeartS, 3, 3, "r.r" "rrr" ".r.");
DEF_STAMP(kSparkle, 5, 5, "..y.." "..y.." "yywyy" "..y.." "..y..");
DEF_STAMP(kSparkleS, 3, 3, ".y." "ywy" ".y.");
DEF_STAMP(kCloud, 14, 6, "....ggg..gg..."
  "..gggggggggg.."
  ".gggggggggggg."
  "gggggggggggggg"
  "GGGGGGGGGGGGGG"
  ".GGGGGGGGGGGG.");
DEF_STAMP(kBolt, 3, 5, ".zz" ".z." "zzz" ".z." "z..");
DEF_STAMP(kSteam, 5, 4, ".ee.." "eeeee" "eeeee" ".eee.");
DEF_STAMP(kSteamS, 3, 3, ".e." "eee" ".e.");
DEF_STAMP(kZBig, 5, 5, "ZZZZZ" "...Z." "..Z.." ".Z..." "ZZZZZ");
DEF_STAMP(kZSmall, 4, 4, "ZZZZ" "..Z." ".Z.." "ZZZZ");
DEF_STAMP(kBerry, 5, 6, "..L.." "..LL." ".RRR." "RRwRR" "RRRRR" ".RRR.");
DEF_STAMP(kBerryBit, 5, 6, "..L.." "..LL." ".RR.." "RRw.." "RRRR." ".RRR.");
DEF_STAMP(kDust, 3, 2, ".d." "ddd");
DEF_STAMP(kNote, 3, 4, ".kk" ".k." ".k." "kk.");
DEF_STAMP(kRumble, 2, 3, ".k" "k." ".k");
DEF_STAMP(kBang, 1, 5, "a" "a" "a" "." "a");

// ---------------------------------------------------------------------------
// Timing helpers (pure functions of time; no state)
// ---------------------------------------------------------------------------
static uint32_t hash32(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
  return x;
}
// True while inside a short event that happens once per `period` at a
// pseudo-random offset (blinks, glances, puffs).
static bool event(uint32_t t, uint32_t period, uint32_t len, uint32_t salt) {
  uint32_t slot = t / period;
  uint32_t off = hash32(slot * 2654435761U + salt) % (period - len);
  uint32_t ph = t % period;
  return ph >= off && ph < off + len;
}
static uint32_t eventPhase(uint32_t t, uint32_t period, uint32_t len, uint32_t salt) {
  uint32_t slot = t / period;
  uint32_t off = hash32(slot * 2654435761U + salt) % (period - len);
  return (t % period) - off;
}
static bool blinking(uint32_t t) { return event(t, 3400, 130, 7); }

uint32_t actDuration(Act a) {
  switch (a) {
    case Act::Eat:       return 1700;
    case Act::Petted:    return 1500;
    case Act::Nudge:     return 1600;
    case Act::Celebrate: return 2200;
    case Act::Hatch:     return 2600;
    default:             return 0;
  }
}

// ---------------------------------------------------------------------------
// Body
// ---------------------------------------------------------------------------
struct Body {
  int W, H;      // size
  int bottom;    // last body row
  float cx;      // centre (pixel-edge coordinates)
  int top() const { return bottom - H + 1; }
};

static Body bodyFor(Stage st) {
  switch (st) {
    case Stage::Baby:  return {18, 17, 28, 20.0f};
    case Stage::Kid:   return {21, 20, 28, 20.0f};
    case Stage::Adult: return {23, 22, 28, 20.0f};
    case Stage::Elder: return {23, 22, 28, 20.0f};
    default:           return {21, 20, 28, 20.0f};
  }
}

static float halfWidth(const Body &bd, int r) {
  const int top = bd.top();
  if (r < top || r > bd.bottom) return -1.0f;
  const float v = ((float)(r - top) + 0.5f) / (float)bd.H;   // 0 top .. 1 bottom
  const float yc = 0.58f;                                  // widest row
  float f;
  if (v < yc) {
    float t = (yc - v) / yc;
    f = powf(1.0f - powf(t, 2.2f), 1.0f / 2.2f);            // round dome
  } else {
    float t = (v - yc) / (1.0f - yc);
    f = powf(1.0f - powf(t, 3.2f), 1.0f / 3.2f);            // flatter bottom
  }
  return 0.5f * (float)bd.W * f;
}

static bool insideBody(const Body &bd, int c, int r) {
  float hw = halfWidth(bd, r);
  if (hw < 0) return false;
  return fabsf((float)c + 0.5f - bd.cx) <= hw + 0.2f;
}

// front: belly + shine. back: plain (we see its back).
static void drawBody(Buf &b, const Body &bd, bool front) {
  const int top = bd.top();
  const float cyc = (float)top + 0.58f * (float)bd.H;
  for (int r = top; r <= bd.bottom; r++) {
    for (int c = 0; c < kW; c++) {
      if (!insideBody(bd, c, r)) continue;
      bool edge = !insideBody(bd, c - 1, r) || !insideBody(bd, c + 1, r) ||
                  !insideBody(bd, c, r - 1) || !insideBody(bd, c, r + 1);
      if (edge) { b.force(c, r, OUT); continue; }
      // distance class from the outline: ring 1 / ring 2
      bool ring1 = !insideBody(bd, c - 1, r - 1) || !insideBody(bd, c + 1, r - 1) ||
                   !insideBody(bd, c - 1, r + 1) || !insideBody(bd, c + 1, r + 1) ||
                   !insideBody(bd, c - 2, r) || !insideBody(bd, c + 2, r) ||
                   !insideBody(bd, c, r - 2) || !insideBody(bd, c, r + 2);
      const float nx = ((float)c + 0.5f - bd.cx) / (0.5f * (float)bd.W);
      const float ny = ((float)r + 0.5f - cyc) / (0.5f * (float)bd.H);
      uint8_t col = BODY;
      if (front) {
        // belly: soft oval on the lower front
        const float bx = ((float)c + 0.5f - bd.cx) / (0.31f * (float)bd.W);
        const float by = ((float)r + 0.5f - ((float)top + 0.74f * (float)bd.H)) / (0.24f * (float)bd.H);
        if (bx * bx + by * by <= 1.0f) col = LIGHT;
      }
      const float away = nx * 0.45f + ny * 0.9f;     // facing down-right
      if (ring1 && away > 0.30f) col = SHADE;
      else if (!ring1 && away > 0.92f) col = SHADE;
      b.force(c, r, col);
    }
  }
  // top-left rim light + glossy shine
  for (int r = top; r <= bd.bottom; r++) {
    for (int c = 0; c < kW; c++) {
      if (b.get(c, r) != BODY) continue;
      const float nx = ((float)c + 0.5f - bd.cx) / (0.5f * (float)bd.W);
      const float ny = ((float)r + 0.5f - cyc) / (0.5f * (float)bd.H);
      bool nearEdge = !insideBody(bd, c - 2, r - 1) || !insideBody(bd, c - 1, r - 2);
      if (nearEdge && (-nx * 0.6f - ny * 0.8f) > 0.55f) b.force(c, r, LIGHT);
    }
  }
  if (front) {
    int sx = (int)(bd.cx - 0.30f * bd.W);
    int sy = top + (int)(0.17f * bd.H);
    b.force(sx, sy + 1, WHITE);
    b.force(sx + 1, sy, WHITE);
    b.force(sx + 2, sy, WHITE);
  }
}

// ---------------------------------------------------------------------------
// Rig: everything that varies per frame
// ---------------------------------------------------------------------------
enum class Arm : uint8_t { Rest, Up, Out, Fist, Wave, Hidden };
enum class Front : uint8_t { None, Crossed, Tummy, Hold };

struct Rig {
  Body bd;
  int dx = 0, dy = 0;              // whole-body offset
  bool back = false;               // facing away (sulk)
  const Stamp *eyeL = &kEyeOpen, *eyeR = &kEyeOpen;
  bool mirrorEyes = false;         // mirror the right eye (directional shapes)
  int eyeDx = 0, eyeDy = 0;
  const Stamp *brow = nullptr;     // left brow, mirrored for the right
  const Stamp *mouth = &kMouthSmile;
  int mouthDx = 0, mouthDy = 0;
  bool cheeks = false;
  Arm armL = Arm::Rest, armR = Arm::Rest;
  Front front = Front::None;
  int footLUp = 0, footRUp = 0;
  bool vein = false, sweat = false;
  bool peekEye = false;            // back view: side-eye over the shoulder
  int berry = 0;                   // 1 = whole berry held, 2 = bitten
  bool nightcap = false;
};

static void drawFrontArms(Buf &b, const Rig &g, int top) {
  const int cx = (int)g.bd.cx + g.dx;
  const int half = (int)(0.5f * g.bd.W + 0.5f);
  const int bottom = g.bd.bottom + g.dy;
  (void)top;
  // An outlined forearm (body colour, shaded underside) lying across the
  // lighter belly. Rounded ends; `paw` puts a little paw bump on the tip.
  auto forearm = [&](int x0, int x1, int y) {
    for (int c = x0; c <= x1; c++) {
      for (int r = y; r <= y + 3; r++) {
        const bool er = (r == y || r == y + 3), ec = (c == x0 || c == x1);
        if (er && ec) continue;                       // rounded ends
        b.force(c, r, (er || ec) ? OUT : (r == y + 2 ? SHADE : BODY));
      }
    }
  };
  if (g.front == Front::Crossed) {
    const int y = bottom - 6;
    forearm(cx - half + 1, cx + 2, y + 1);            // left forearm, lower
    forearm(cx - 3, cx + half - 2, y);                // right forearm on top
    return;
  }
  if (g.front == Front::Tummy) {
    const int y = bottom - 6;
    forearm(cx - 4, cx + half - 2, y + 1);            // a paw rubbing the tummy
    return;
  }
  // (Front::Hold mitts are drawn after the snack in renderRig.)
}

static void drawSideArm(Buf &b, Arm a, const Rig &g, int top, bool right) {
  if (a == Arm::Hidden) return;
  const int cx = (int)g.bd.cx + g.dx;
  const int half = (int)(0.5f * g.bd.W + 0.5f);
  const int shoulder = top + (int)(0.52f * g.bd.H) + g.dy;
  const Stamp *s = &kArmRest;
  int x = 0, y = shoulder;
  switch (a) {
    case Arm::Rest: s = &kArmRest; x = cx - half - 1; y = shoulder + 1; break;
    case Arm::Up:   s = &kArmUp;   x = cx - half - 3; y = shoulder - 4; break;
    case Arm::Out:  s = &kArmOut;  x = cx - half - 3; y = shoulder + 1; break;
    case Arm::Fist: s = &kArmFist; x = cx - half - 3; y = shoulder - 3; break;
    case Arm::Wave: s = &kArmWave; x = cx - half - 3; y = shoulder - 5; break;
    default: break;
  }
  if (right) x = 2 * cx - x - s->w;   // mirror about the centre line
  put(b, *s, x, y, right);
}

static void drawHeadAccessories(Buf &b, const Pose &p, const Rig &g, int top) {
  const int cx = (int)g.bd.cx + g.dx;
  const int y0 = top + g.dy;
  const int half = (int)(0.5f * g.bd.W);
  // Growth features (ears are drawn behind the head in renderRig).
  if (p.stage == Stage::Baby) putOutlined(b, kSprout, cx - 3, y0 - 3);
  if (p.stage == Stage::Elder && !g.back) put(b, kStarMark, cx - 2, y0 + 3);
  // Accessories are outlined so they read against any sky.
  if (g.nightcap) { putOutlined(b, kNightcap, cx - 6, y0 - 4); return; }
  // Streak rewards (best accessory unlocked).
  switch (p.acc) {
    case Accessory::Bow:      putOutlined(b, kBow, cx + half - 7, y0 - 1); break;
    case Accessory::PartyHat: putOutlined(b, kPartyHat, cx - 3, y0 - 5); break;
    case Accessory::FlowerCrown:
      putOutlined(b, kFlower, cx - 7, y0 - 1);
      putOutlined(b, kFlower, cx - 2, y0 - 2);
      putOutlined(b, kFlower, cx + 3, y0 - 1);
      break;
    case Accessory::Crown:    putOutlined(b, kCrown, cx - 4, y0 - 4); break;
    default: break;
  }
}

static void renderRig(Buf &b, const Pose &p, const Rig &g) {
  Body bd = g.bd;
  bd.cx += (float)g.dx;
  bd.bottom += g.dy;
  const int top = bd.top();
  const int cx = (int)bd.cx;
  const int half = (int)(0.5f * bd.W + 0.5f);

  // feet (behind the body; they stay on the ground while it hops)
  const int fy = g.bd.bottom;
  const int fxL = cx - (int)(0.28f * bd.W) - 3;
  const int fxR = cx + (int)(0.28f * bd.W) - 3;
  if (!g.back) {
    if (g.footLUp == 0) put(b, kFoot, fxL, fy);
    if (g.footRUp == 0) put(b, kFoot, fxR, fy);
  }
  // ears behind the head
  if (p.stage != Stage::Baby) {
    const int ex = (int)(0.30f * bd.W);
    const Stamp &e = g.back ? kEarBack : kEar;
    put(b, e, cx - ex - 3, top - 2);
    put(b, e, cx + ex - 2, top - 2, true);
  }

  drawBody(b, bd, !g.back);

  // lifted feet in front of the body, kicked outward
  if (g.footLUp) put(b, kFoot, fxL - 2, fy - g.footLUp);
  if (g.footRUp) put(b, kFoot, fxR + 2, fy - g.footRUp);

  if (!g.back) {
    // face anchor
    const int eyeY = top + (int)(0.34f * bd.H) + g.eyeDy;
    const int sep = bd.W >= 21 ? 2 : 1;
    const int exL = cx - sep - 4 + g.eyeDx;
    const int exR = cx + sep + g.eyeDx;
    put(b, *g.eyeL, exL, eyeY);
    put(b, *g.eyeR, exR, eyeY, g.mirrorEyes);
    if (g.brow) {
      put(b, *g.brow, exL - (g.brow->w - 4), eyeY - 3);
      put(b, *g.brow, exR, eyeY - 3, true);
    }
    if (g.cheeks) {
      put(b, kCheek, exL - 1, eyeY + 4);
      put(b, kCheek, exR + 3, eyeY + 4);
    }
    const Stamp *m = g.mouth;
    if (m) put(b, *m, cx - m->w / 2 + g.mouthDx + (g.eyeDx > 0 ? 1 : (g.eyeDx < 0 ? -1 : 0)),
               eyeY + 5 + g.mouthDy);
    if (g.vein) put(b, kVein, cx + half - 6, top + 1);
    if (g.sweat) put(b, kSweat, cx + half - 3, eyeY - 2);
  } else {
    // seen from behind: chipmunk stripes down the back, tail, heels, and
    // now and then a side-eye over the shoulder
    for (int r = top + 2; r <= bd.bottom - 3; r++) {
      for (int k = -1; k <= 1; k += 2) {
        const int c = cx + k * 3 - (k > 0 ? 1 : 0);
        if (b.get(c, r) == BODY || b.get(c, r) == LIGHT) b.force(c, r, SHADE);
      }
      if (b.get(cx - 1, r) == BODY && r < bd.bottom - 5) b.force(cx - 1, r, OUT);
    }
    put(b, kTail, cx + half - 4, bd.bottom - 4);
    put(b, kFoot, fxL, fy + 1);
    put(b, kFoot, fxR, fy + 1);
    if (g.peekEye) {
      const int eyeY = top + (int)(0.36f * bd.H);
      put(b, kEyeGlare, cx + half - 6, eyeY, true);
      put(b, kBrowAngry, cx + half - 6, eyeY - 3, true);
    }
  }

  drawFrontArms(b, g, g.bd.top());
  drawSideArm(b, g.armL, g, g.bd.top(), false);
  drawSideArm(b, g.armR, g, g.bd.top(), true);
  if (g.berry) {
    const int eyeY = top + (int)(0.34f * bd.H);
    putOutlined(b, g.berry == 1 ? kBerry : kBerryBit, cx - 2, eyeY + 3);
    put(b, kMitt, cx - 5, eyeY + 6);          // two mitts hold it from below
    put(b, kMitt, cx + 2, eyeY + 6);
  }
  drawHeadAccessories(b, p, g, g.bd.top());
}

// ---------------------------------------------------------------------------
// Effects
// ---------------------------------------------------------------------------
static void floatUp(Buf &b, const Stamp &s, uint32_t t, uint32_t period, uint32_t phase,
                    int x, int y0, int rise, bool outlined = true) {
  uint32_t ph = (t + phase) % period;
  int y = y0 - (int)((uint64_t)rise * ph / period);
  int sway = ((ph / 220) % 2) ? 1 : 0;
  if (outlined) putOutlined(b, s, x + sway, y);
  else put(b, s, x + sway, y);
}

static void stormCloud(Buf &b, uint32_t t, int cx, int y, bool rain, bool bolts) {
  putOutlined(b, kCloud, cx - 7, y);
  if (bolts && event(t, 1500, 140, 31)) putOutlined(b, kBolt, cx - 1, y + 7);
  if (rain) {
    for (int i = 0; i < 4; i++) {
      int x = cx - 5 + i * 3 + (i & 1);
      int fall = (int)(((t / 70) + i * 3) % 7);
      b.set(x, y + 7 + fall, WATER);
      b.set(x, y + 8 + fall, WATER);
    }
  }
}

// ---------------------------------------------------------------------------
// Egg
// ---------------------------------------------------------------------------
static void drawEgg(Buf &b, int dx, int shear, uint8_t pct, bool flash, bool topOff, int topLift) {
  const float cx = 20.0f + (float)dx;
  const int bottom = 30, H = 21, W = 17;
  const int top = bottom - H + 1;
  auto inside = [&](int c, int r) {
    if (r < top || r > bottom) return false;
    const float v = ((float)(r - top) + 0.5f) / (float)H;
    const float yc = 0.62f;
    float f;
    if (v < yc) { float t = (yc - v) / yc; f = powf(1.0f - t * t, 0.5f) * (0.78f + 0.22f * (1.0f - t)); }
    else { float t = (v - yc) / (1.0f - yc); f = powf(1.0f - powf(t, 2.4f), 1.0f / 2.4f); }
    // shear for the wobble: top rows lean
    const float lean = (float)shear * (1.0f - v);
    return fabsf((float)c + 0.5f - cx - lean) <= 0.5f * (float)W * f + 0.2f;
  };
  const int crackRow = top + 8;
  for (int r = top; r <= bottom; r++)
    for (int c = 0; c < kW; c++) {
      if (!inside(c, r)) continue;
      if (topOff && r < crackRow) continue;
      bool edge = !inside(c - 1, r) || !inside(c + 1, r) || !inside(c, r - 1) || !inside(c, r + 1);
      uint8_t col = EGG;
      if (!edge) {
        bool shade = !inside(c + 2, r + 1) || !inside(c + 1, r + 2);
        if (shade) col = EGG_SHADE;
      }
      if (flash) col = edge ? (uint8_t)OUT : (uint8_t)WHITE;
      b.force(c, r, edge ? (uint8_t)OUT : col);
    }
  if (topOff) {
    // the lifted cap of shell
    for (int r = top; r < crackRow; r++)
      for (int c = 0; c < kW; c++) {
        if (!inside(c, r)) continue;
        bool edge = !inside(c - 1, r) || !inside(c + 1, r) || !inside(c, r - 1) || r == crackRow - 1;
        b.force(c + (topLift / 3), r - topLift, edge ? OUT : EGG);
      }
  }
  if (flash) return;
  // spots
  const int sx = (int)cx;
  static const int8_t spots[][3] = {{-4, 13, 2}, {3, 10, 2}, {-1, 17, 1}, {4, 16, 2}, {-5, 7, 1}};
  for (auto &s : spots) {
    if (topOff && top + s[1] < crackRow) continue;
    for (int yy = 0; yy < s[2]; yy++)
      for (int xx = 0; xx < s[2] + 1; xx++) {
        int x = sx + s[0] + xx, y = top + s[1] + yy;
        if (b.get(x, y) == EGG || b.get(x, y) == EGG_SHADE) b.force(x, y, EGG_SPOT);
      }
  }
  // shine
  if (!topOff) { b.force(sx - 5, top + 4, WHITE); b.force(sx - 4, top + 3, WHITE); b.force(sx - 5, top + 5, WHITE); }
  // cracks grow with hatch progress
  if (pct >= 34 || topOff) {
    int len = pct >= 90 ? 14 : (pct >= 67 ? 10 : 5);
    if (topOff) len = 0;
    int x = sx - len / 2, y = crackRow;
    for (int i = 0; i < len; i++) {
      int yy = y + ((i % 4 == 1) ? -1 : ((i % 4 == 3) ? 1 : 0));
      if (b.get(x + i, yy) != T_) b.force(x + i, yy, OUT);
    }
  }
}

// ---------------------------------------------------------------------------
// Compose
// ---------------------------------------------------------------------------
static void composeEgg(Buf &b, const Pose &p) {
  uint32_t t = p.clockMs;
  int shear = 0;
  // wobble: more often as it gets closer to hatching
  uint32_t period = p.eggPct >= 67 ? 1400 : (p.eggPct >= 34 ? 2200 : 3200);
  if (event(t, period, 420, 3)) {
    uint32_t ph = eventPhase(t, period, 420, 3);
    shear = (ph / 105) % 2 ? 1 : -1;
  }
  drawEgg(b, 0, shear, p.eggPct, false, false, 0);
  if (p.act == Act::Walk) {
    // excited little hops while you walk
    if ((t / 300) % 2) { /* nothing: hop is a scene offset */ }
    putOutlined(b, kBang, 30, 8 + (int)((t / 200) % 2));
  }
}

static void composeHatch(Buf &b, const Pose &p) {
  const uint32_t a = p.animMs;
  if (a < 1100) {                                   // frantic wobble, cracks spread
    int shear = (a / 90) % 2 ? 1 : -1;
    drawEgg(b, 0, shear, (uint8_t)(67 + a * 33 / 1100), false, false, 0);
    if (a > 600) putOutlined(b, kSparkleS, 31, 9 + (int)((a / 120) % 2));
    return;
  }
  if (a < 1400) {                                   // flash
    drawEgg(b, 0, 0, 100, true, false, 0);
    putOutlined(b, kSparkle, 6, 10);
    putOutlined(b, kSparkle, 29, 6);
    return;
  }
  // baby pops out, shell cap flies up, sparkles
  Pose baby = p;
  baby.stage = Stage::Baby;
  Rig g;
  g.bd = bodyFor(Stage::Baby);
  uint32_t k = a - 1400;
  g.dy = k < 250 ? -3 : (k < 450 ? -1 : 0);
  g.eyeL = &kEyeSpark; g.eyeR = &kEyeSpark;
  g.mouth = &kMouthGrin; g.cheeks = true;
  g.armL = Arm::Up; g.armR = Arm::Up;
  // lower shell half stays around its feet
  drawEgg(b, 0, 0, 100, false, true, (int)(k / 60) + 4);
  renderRig(b, baby, g);
  floatUp(b, kSparkleS, a, 900, 0, 7, 20, 14);
  floatUp(b, kSparkleS, a, 900, 450, 30, 22, 14);
  floatUp(b, kHeartS, a, 1100, 200, 26, 14, 10);
}

static void moodFace(Rig &g, Mood m, uint32_t t, bool marching) {
  switch (m) {
    case Mood::Ecstatic:
      g.eyeL = g.eyeR = &kEyeHappy; g.mouth = &kMouthGrin; g.cheeks = true; break;
    case Mood::Happy:
      g.eyeL = g.eyeR = blinking(t) ? &kEyeShut : &kEyeOpen;
      g.mouth = marching ? &kMouthGrin : &kMouthSmile; g.cheeks = true; break;
    case Mood::Content:
      g.eyeL = g.eyeR = blinking(t) ? &kEyeShut : &kEyeOpen;
      g.mouth = &kMouthSmile; break;
    case Mood::Peckish:
      g.eyeL = g.eyeR = blinking(t) ? &kEyeShut : &kEyeOpen;
      g.brow = &kBrowWorried; g.mouth = marching ? &kMouthFlat : &kMouthWavy; break;
    case Mood::Grumpy:
      g.eyeL = g.eyeR = &kEyeGlare; g.mirrorEyes = true;
      g.brow = &kBrowAngry; g.mouth = &kMouthScowl; break;
    case Mood::Furious:
    default:
      g.eyeL = g.eyeR = &kEyeGlare; g.mirrorEyes = true;
      g.brow = &kBrowFurious; g.mouth = &kMouthTeeth; g.vein = true; break;
  }
}

static void composeIdle(Buf &b, const Pose &p, Rig &g) {
  const uint32_t t = p.clockMs;
  switch (p.mood) {
    case Mood::Ecstatic: {
      // jump loop: crouch, launch, hang, land
      uint32_t ph = t % 1150;
      if (ph < 130)       { g.bd.W += 2; g.bd.H -= 2; }
      else if (ph < 260)  { g.bd.W -= 2; g.bd.H += 2; g.dy = -2; }
      else if (ph < 560)  { g.dy = -5; }
      else if (ph < 690)  { g.bd.W -= 1; g.bd.H += 1; g.dy = -2; }
      else if (ph < 820)  { g.bd.W += 2; g.bd.H -= 2; }
      moodFace(g, p.mood, t, false);
      if (ph >= 130 && ph < 690) { g.armL = Arm::Up; g.armR = Arm::Up; }
      if ((t / 2300) % 3 == 1) { g.eyeL = g.eyeR = &kEyeSpark; }
      renderRig(b, p, g);
      floatUp(b, kHeart, t, 1700, 0, 4, 18, 16);
      floatUp(b, kHeartS, t, 1700, 850, 32, 20, 16);
      if ((t / 400) % 2) putOutlined(b, kSparkleS, 31, 6);
      else putOutlined(b, kSparkleS, 6, 8);
      return;
    }
    case Mood::Happy: {
      // bouncy sway + occasional wave and hum
      uint32_t ph = t % 900;
      if (ph < 160) { g.bd.W += 1; g.bd.H -= 1; }
      else if (ph < 420) g.dy = -1;
      moodFace(g, p.mood, t, false);
      if (event(t, 7000, 1400, 11)) {
        g.armR = ((t / 220) % 2) ? Arm::Wave : Arm::Up;
        g.eyeL = g.eyeR = &kEyeHappy;
      }
      renderRig(b, p, g);
      if (event(t, 5200, 1600, 13)) {
        uint32_t k = eventPhase(t, 5200, 1600, 13);
        putOutlined(b, kNote, 31, 10 - (int)(k / 220));
      }
      return;
    }
    case Mood::Content: {
      if ((t % 2400) < 1200) g.bd.H -= 0; else { g.bd.H -= 1; g.bd.W += 1; }   // breathing
      moodFace(g, p.mood, t, false);
      if (event(t, 5600, 1300, 17)) g.eyeDx = ((t / 5600) % 2) ? 1 : -1;     // look around
      renderRig(b, p, g);
      return;
    }
    case Mood::Peckish: {
      moodFace(g, p.mood, t, false);
      g.front = Front::Tummy;
      g.armR = Arm::Hidden;
      g.eyeDx = 1; g.eyeDy = 1;                       // eyeing the bowl
      bool rumble = event(t, 3000, 650, 19);
      if (rumble) g.dx = ((t / 80) % 2) ? 1 : 0;
      if (event(t, 6500, 900, 23)) { g.mouth = &kMouthLick; g.eyeL = g.eyeR = &kEyeHappy; }
      renderRig(b, p, g);
      if (rumble) {
        const int y = g.bd.top() + (int)(0.66f * g.bd.H);
        put(b, kRumble, 5, y);
        put(b, kRumble, 33, y, true);
      }
      return;
    }
    case Mood::Grumpy: {
      moodFace(g, p.mood, t, false);
      g.front = Front::Crossed;
      g.armL = Arm::Hidden; g.armR = Arm::Hidden;
      // foot tapping in bursts: tap-tap-tap-tap ... pause
      uint32_t ph = t % 2600;
      if (ph < 1600 && ((ph / 200) % 2) == 0) g.footRUp = 2;
      bool hmph = event(t, 4800, 900, 29);
      if (hmph) { g.eyeDx = -1; g.mouth = &kMouthPout; }
      renderRig(b, p, g);
      if (hmph) {
        uint32_t k = eventPhase(t, 4800, 900, 29);
        putOutlined(b, k < 450 ? kSteamS : kSteam, 31 + (int)(k / 300), 12 - (int)(k / 300));
      }
      return;
    }
    case Mood::Furious:
    default: {
      if (p.sulking) {
        // back turned, hunched, own private rain cloud; side-eye now and then
        g.back = true;
        g.bd.W += 1; g.bd.H -= 1;
        g.armL = Arm::Hidden; g.armR = Arm::Hidden;
        g.peekEye = event(t, 6000, 1300, 37);
        g.dy = (t % 2600) < 1300 ? 0 : 1;            // heavy sighs
        renderRig(b, p, g);
        // elbows of the crossed arms poke out at the sides
        const int y = g.bd.top() + (int)(0.62f * g.bd.H) + g.dy;
        const int half = (int)(0.5f * g.bd.W + 0.5f);
        put(b, kPaw, 20 - half - 2, y);
        put(b, kPaw, 20 + half - 1, y);
        stormCloud(b, t, 20, 0, true, false);
        if (event(t, 5200, 800, 43)) {
          uint32_t k = eventPhase(t, 5200, 800, 43);
          putOutlined(b, kSteamS, 4 - (int)(k / 300), 13 - (int)(k / 250));
        }
        return;
      }
      // stomping tantrum under a storm cloud
      uint32_t ph = t % 600;
      if (ph < 150)      { g.footLUp = 2; g.dx = -1; }
      else if (ph < 300) { g.dy = 0; }
      else if (ph < 450) { g.footRUp = 2; g.dx = 1; }
      moodFace(g, p.mood, t, false);
      if (((t / 900) % 3) == 2) { g.eyeL = g.eyeR = &kEyeSquint; g.mirrorEyes = true; }
      g.armL = Arm::Fist; g.armR = Arm::Fist;
      renderRig(b, p, g);
      stormCloud(b, t, 20, 0, false, true);
      uint32_t k = t % 1000;
      putOutlined(b, (k % 500) < 250 ? kSteamS : kSteam, (k < 500 ? 4 : 31), 13 - (int)((k % 500) / 125));
      return;
    }
  }
}

static void composeSleep(Buf &b, const Pose &p, Rig &g) {
  const uint32_t t = p.clockMs;
  g.bd.W += 2; g.bd.H -= 2;                           // flopped down
  if ((t % 3000) > 1500) { g.bd.H -= 1; g.bd.W += 1; }  // slow breathing
  g.eyeL = g.eyeR = &kEyeSleep;
  const bool hungry = p.mood >= Mood::Grumpy;
  g.mouth = hungry ? &kMouthFrown : &kMouthTiny;
  g.armL = Arm::Hidden; g.armR = Arm::Hidden;
  g.nightcap = true;
  renderRig(b, p, g);
  floatUp(b, kZBig, t, 2400, 0, 30, 12, 10, false);
  floatUp(b, kZSmall, t, 2400, 1200, 33, 9, 8, false);
  if (hungry && event(t, 4000, 700, 41)) {
    const int y = g.bd.top() + (int)(0.6f * g.bd.H);
    put(b, kRumble, 5, y);
  }
}

static void composeWalk(Buf &b, const Pose &p, Rig &g) {
  const uint32_t t = p.clockMs;
  const uint32_t step = 380;
  const bool left = ((t / step) % 2) == 0;
  const uint32_t ph = t % step;
  moodFace(g, p.mood, t, true);
  if (p.mood == Mood::Ecstatic) g.eyeL = g.eyeR = &kEyeHappy;
  // Waddle: weight shifts onto the planted foot (sway), the stepping foot
  // kicks up and out, and the whole body hops a pixel mid-step.
  const bool lifted = ph < (step * 2) / 3;
  if (left) { g.footLUp = lifted ? 2 : 0; g.dx = 1; } else { g.footRUp = lifted ? 2 : 0; g.dx = -1; }
  g.dy = (ph > step / 6 && ph < step / 2) ? -1 : 0;
  if (p.mood >= Mood::Grumpy) {
    g.front = Front::Crossed; g.armL = Arm::Hidden; g.armR = Arm::Hidden;   // grumpy march
  } else {
    g.armL = left ? Arm::Out : Arm::Rest;
    g.armR = left ? Arm::Rest : Arm::Out;
  }
  if (p.mood == Mood::Furious) g.vein = true;
  renderRig(b, p, g);
  if (ph < 200) put(b, kDust, left ? 30 : 7, 29);
  if (p.mood == Mood::Furious) stormCloud(b, t, 20, 0, false, false);
}

static void composeEat(Buf &b, const Pose &p, Rig &g) {
  const uint32_t a = p.animMs, t = p.clockMs;
  g.armL = Arm::Hidden; g.armR = Arm::Hidden;
  if (a < 380) {
    g.front = Front::Hold; g.berry = 1;
    g.eyeL = g.eyeR = &kEyeSpark; g.mouth = &kMouthO; g.cheeks = true;
    g.dy = (a / 120) % 2 ? -1 : 0;
  } else if (a < 760) {
    g.front = Front::Hold; g.berry = 2;
    g.eyeL = g.eyeR = &kEyeShut; g.mouth = &kMouthChomp;
    g.bd.W += 1; g.bd.H -= 1;
  } else if (a < 1300) {
    g.eyeL = g.eyeR = &kEyeHappy; g.mouth = &kMouthChew; g.cheeks = true;
    if ((a / 130) % 2) { g.bd.W += 1; g.bd.H -= 1; }
    g.armL = Arm::Rest; g.armR = Arm::Rest;
  } else {
    g.eyeL = g.eyeR = &kEyeHappy; g.mouth = &kMouthGrin; g.cheeks = true;
    g.armL = Arm::Up; g.armR = Arm::Up; g.dy = -1;
  }
  (void)t;
  renderRig(b, p, g);
  if (a >= 380 && a < 1300) {
    // crumbs falling
    for (int i = 0; i < 3; i++) {
      int y = 20 + (int)(((a - 380) / 90 + i * 3) % 9);
      b.set(16 + i * 4, y, BERRY);
    }
  }
  if (a >= 1300) floatUp(b, kHeartS, a, 600, 0, 30, 14, 8);
}

static void composePetted(Buf &b, const Pose &p, Rig &g) {
  const uint32_t a = p.animMs, t = p.clockMs;
  switch (p.mood) {
    case Mood::Ecstatic:
    case Mood::Happy:
    case Mood::Content:
      g.eyeL = g.eyeR = &kEyeHappy; g.mouth = p.mood == Mood::Content ? &kMouthSmile : &kMouthGrin;
      g.cheeks = true;
      g.dx = ((a / 110) % 2) ? 1 : -1;                // happy wiggle
      if (a < 500) { g.bd.W += 1; g.bd.H -= 1; }
      renderRig(b, p, g);
      floatUp(b, kHeart, a, 1500, 0, 30, 16, 14);
      if (p.mood != Mood::Content) floatUp(b, kHeartS, a, 1500, 600, 6, 18, 12);
      return;
    case Mood::Peckish:
      moodFace(g, p.mood, t, false);
      g.eyeL = g.eyeR = &kEyeOpen; g.mouth = &kMouthO;
      g.front = Front::Tummy; g.armR = Arm::Hidden;
      g.dx = (a / 160) % 2;
      renderRig(b, p, g);
      put(b, kRumble, 5, 21);
      floatUp(b, kHeartS, a, 1500, 0, 31, 14, 8);
      return;
    case Mood::Grumpy:
      moodFace(g, p.mood, t, false);
      g.front = Front::Crossed; g.armL = Arm::Hidden; g.armR = Arm::Hidden;
      g.eyeDx = 2; g.mouth = &kMouthPout;              // "hmph", looks away
      renderRig(b, p, g);
      putOutlined(b, a < 700 ? kSteamS : kSteam, 4 - (int)(a / 400), 12 - (int)(a / 400));
      return;
    case Mood::Furious:
    default:
      moodFace(g, p.mood, t, false);
      g.eyeL = g.eyeR = &kEyeSquint; g.mirrorEyes = true;
      g.armL = Arm::Fist; g.armR = ((a / 140) % 2) ? Arm::Up : Arm::Fist;   // shakes a fist
      g.dx = ((a / 70) % 2) ? 1 : -1;
      renderRig(b, p, g);
      stormCloud(b, t, 20, 0, false, true);
      putOutlined(b, kSteam, 4, 11 - (int)(a / 300) % 3);
      putOutlined(b, kSteam, 32, 11 - (int)(a / 300) % 3);
      return;
  }
}

static void composeNudge(Buf &b, const Pose &p, Rig &g) {
  const uint32_t a = p.animMs, t = p.clockMs;
  moodFace(g, p.mood < Mood::Grumpy ? Mood::Grumpy : p.mood, t, false);
  uint32_t ph = a % 400;
  if (ph < 200) { g.footLUp = 2; g.dx = -1; } else { g.footRUp = 2; g.dx = 1; }
  g.armL = Arm::Fist; g.armR = Arm::Fist;
  renderRig(b, p, g);
  putOutlined(b, kBang, 32, 8 + (int)((a / 150) % 2));
  if (p.mood == Mood::Furious) stormCloud(b, t, 20, 0, false, true);
}

static void composeCelebrate(Buf &b, const Pose &p, Rig &g) {
  const uint32_t a = p.animMs;
  uint32_t ph = a % 1100;
  if (ph < 150)      { g.bd.W += 2; g.bd.H -= 2; }
  else if (ph < 300) { g.bd.W -= 2; g.bd.H += 2; g.dy = -3; }
  else if (ph < 650) { g.dy = -6; }
  else if (ph < 800) { g.dy = -2; }
  else if (ph < 950) { g.bd.W += 2; g.bd.H -= 2; }
  g.eyeL = g.eyeR = &kEyeSpark; g.mouth = &kMouthGrin; g.cheeks = true;
  g.armL = Arm::Up; g.armR = Arm::Up;
  renderRig(b, p, g);
  // sparkle burst
  static const int8_t pos[][2] = {{4, 6}, {33, 4}, {2, 18}, {35, 17}, {10, 1}, {28, 0}};
  for (int i = 0; i < 6; i++) {
    if (((a / 160) + i) % 3 == 0) putOutlined(b, kSparkle, pos[i][0] - 2, pos[i][1]);
    else if (((a / 160) + i) % 3 == 1) putOutlined(b, kSparkleS, pos[i][0] - 1, pos[i][1] + 1);
  }
}

const uint16_t *compose(const Pose &p, uint8_t out[kW * kH]) {
  memset(out, 0, kW * kH);
  basePalette();
  bodyPalette(p);
  Buf b{out};

  if (p.act == Act::Hatch) { composeHatch(b, p); return gPal; }
  if (p.stage == Stage::Egg) { composeEgg(b, p); return gPal; }

  Rig g;
  g.bd = bodyFor(p.stage);
  if (p.asleep && p.act != Act::Eat && p.act != Act::Walk) { composeSleep(b, p, g); return gPal; }
  switch (p.act) {
    case Act::Walk:      composeWalk(b, p, g); break;
    case Act::Eat:       composeEat(b, p, g); break;
    case Act::Petted:    composePetted(b, p, g); break;
    case Act::Nudge:     composeNudge(b, p, g); break;
    case Act::Celebrate: composeCelebrate(b, p, g); break;
    default:             composeIdle(b, p, g); break;
  }
  return gPal;
}

void draw(px::Canvas &c, const Pose &p, int x, int y, int scale) {
  static uint8_t buf[kW * kH];
  const uint16_t *pal = compose(p, buf);
  px::blit(c, buf, kW, kH, x, y, scale, pal);
}

// ---------------------------------------------------------------------------
// UI icons
// ---------------------------------------------------------------------------
DEF_STAMP(kIconFoot, 5, 7, ".oo.." "obbo." "obbo." ".oo.." "...oo" "..obb" "..oo.");
DEF_STAMP(kIconFlame, 5, 6, "..y.." ".yy.." ".yay." "yaaay" "yaaay" ".yyy.");
DEF_STAMP(kIconStar, 5, 5, "..y.." ".yyy." "yyyyy" ".yyy." ".y.y.");
DEF_STAMP(kIconMoon, 5, 5, ".ZZZ." "ZZ..." "Z...." "ZZ..." ".ZZZ.");
DEF_STAMP(kIconBowl, 7, 3, "ooooooo" ".obbbo." "..ooo..");

static const Stamp &iconStamp(Icon i) {
  switch (i) {
    case Icon::Snack: return kBerry;
    case Icon::Heart: return kHeart;
    case Icon::Foot:  return kIconFoot;
    case Icon::Flame: return kIconFlame;
    case Icon::Star:  return kIconStar;
    case Icon::Moon:  return kIconMoon;
    case Icon::Bowl:  return kIconBowl;
    default:          return kHeart;
  }
}

int iconW(Icon i) { return iconStamp(i).w + 2; }
int iconH(Icon i) { return iconStamp(i).h + 2; }

void drawIcon(px::Canvas &c, Icon i, int x, int y, int scale) {
  static uint8_t buf[kW * kH];
  memset(buf, 0, sizeof(buf));
  basePalette();
  Pose p;
  p.stage = Stage::Kid;
  bodyPalette(p);
  Buf b{buf};
  const Stamp &s = iconStamp(i);
  putOutlined(b, s, 1, 1);
  px::blit(c, buf, kW, kH, x, y, scale, gPal);
}

}  // namespace petart
