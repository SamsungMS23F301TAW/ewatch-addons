// Friend Radar host preview renderer.
//
// Compiles the firmware's pure renderer (src/apps/radar/gfx + logic) for the
// build machine and writes every screen and animation as PNGs, so the UI can
// be inspected without a watch. Run via tools/render_previews.sh.
//
//   preview <outdir> [--frames] [--bench]
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <chrono>
#include <string>
#include <vector>
#include "png.h"
#include "fr_anim.h"
#include "fr_motion.h"
#include "fr_radar.h"
#include "fr_screens.h"

using namespace fr;

static uint16_t gFrame[dial::kBgPixels];
static uint16_t gBg[dial::kBgPixels];
static uint16_t gPolar[dial::kPolarEntries];
static uint16_t gOverlay[dial::kPolarEntries];
static uint16_t gClean[dial::kPolarEntries];
static uint16_t gBackdrop[dial::kBgPixels];
static uint16_t gUnder[dial::kBgPixels];
static std::string gOut = ".";

// Contact sheet: frames laid out in a grid, 1:1 pixels, 6 px gutters.
struct Sheet {
  int cols, rows, W, H;
  std::vector<uint16_t> px;
  Sheet(int c, int r) : cols(c), rows(r), W(c * (layout::W + 6) + 6), H(r * (layout::H + 6) + 6),
                        px((size_t)W * H, 0x1082) {}
  void put(int idx, const uint16_t *frame) {
    int ox = 6 + (idx % cols) * (layout::W + 6), oy = 6 + (idx / cols) * (layout::H + 6);
    for (int y = 0; y < layout::H; ++y)
      memcpy(&px[(size_t)(oy + y) * W + ox], frame + (size_t)y * layout::W, layout::W * 2);
  }
  void save(const char *name) {
    std::string p = gOut + "/" + name;
    png::writeRgb565(p.c_str(), px.data(), W, H, 1);
    printf("  %s\n", p.c_str());
  }
};

static void save(const char *name, int zoom = 2) {
  std::string p = gOut + "/" + name;
  if (!png::writeRgb565(p.c_str(), gFrame, layout::W, layout::H, zoom))
    fprintf(stderr, "failed to write %s\n", p.c_str());
  else
    printf("  %s\n", p.c_str());
}

static PeerView peer(uint32_t id, const char *name, bool mate, Zone z, float bandPos,
                     float distM, uint32_t ageMs, uint8_t flags = 0) {
  PeerView v;
  v.id = id;
  snprintf(v.label, sizeof v.label, "%s", name);
  snprintf(v.name, sizeof v.name, "%s", name);
  v.mate = mate;
  v.zone = z;
  v.bandPos = bandPos;
  v.distM = distM;
  v.plDb = 22.f * log10f(distM > 0.1f ? distM : 0.1f);
  v.ageMs = ageMs;
  v.flags = flags;
  v.rssi = -65.f;
  v.samples = 40;
  return v;
}

// ---- the scene shared by the radar previews ----------------------------------------
static RadarScene gScene;
static BlipAnimator gAnim;
static Blip gBlips[kMaxPeers];
static int gNb = 0;

static RadarFrame liveFrame(uint32_t t, float beam) {
  RadarFrame f;
  f.tMs = t;
  f.beamDeg = beam;
  f.blips = gBlips;
  f.nBlips = gNb;
  f.hud.radio = RadioState::Live;
  snprintf(f.hud.myName, sizeof f.hud.myName, "Kyle");
  snprintf(f.hud.footer, sizeof f.hud.footer, "5 nearby " FR_MIDDOT " Sam is right here");
  return f;
}

static void cardFor(RadarFrame &f, uint32_t id, float u) {
  Blip b;
  if (!gAnim.find(id, b)) return;
  float bx, by;
  RadarScene::blipOverviewXY(b, bx, by);
  f.cam = RadarCamera::lockOn(bx, by, u);
  f.focusId = id;
  f.cardY = (float)layout::H - ((float)layout::H - (float)layout::cardTop) * u;
  CardInfo &k = f.card;
  k.show = true;
  k.id = id;
  snprintf(k.name, sizeof k.name, "%s", b.label);
  k.mate = b.mate;
  k.zone = b.zone;
  k.color = b.color;
  k.signal = b.signal;
}

static void benchmark(Canvas &c, const PageChrome &pc, const MenuRow *rows, int nr) {
  using clk = std::chrono::steady_clock;
  auto time = [&](const char *name, int iters, auto fn) {
    auto t0 = clk::now();
    for (int i = 0; i < iters; ++i) fn(i);
    double us = std::chrono::duration<double, std::micro>(clk::now() - t0).count() / iters;
    printf("  %-22s %8.1f us/frame\n", name, us);
  };
  time("build (static layers)", 20, [&](int) { gScene.build(); });
  time("radar", 400, [&](int i) { gScene.drawFrame(c, liveFrame((uint32_t)i * 50, (float)i * 4.5f)); });
  time("radar + card", 400, [&](int i) {
    RadarFrame f = liveFrame((uint32_t)i * 50, (float)i * 4.5f);
    cardFor(f, 0x1E3A0042, 1.f);
    f.card.zone = Zone::Near; f.card.distM = 2.1f; f.card.bars = 3;
    gScene.drawFrame(c, f);
  });
  PairAnimSpec hello;
  hello.kind = AnimKind::Hello;
  hello.idA = 0x1E3A0042; hello.idB = 0x7777BBBB;
  hello.seed = pairSeed(hello.idA, hello.idB, 0x48454c4fu);
  snprintf(hello.name, sizeof hello.name, "Ewan");
  time("radar + hello", 400, [&](int i) {
    RadarFrame f = liveFrame((uint32_t)i * 9, (float)i);
    f.anim = &hello; f.animT = (i * 9) % (int)kHelloDurMs;
    gScene.drawFrame(c, f);
  });
  PairAnimSpec cel = hello;
  cel.kind = AnimKind::Celebrate;
  time("radar + celebrate", 400, [&](int i) {
    RadarFrame f = liveFrame((uint32_t)i * 7, (float)i);
    f.anim = &cel; f.animT = (i * 7) % (int)kCelebrateDurMs;
    gScene.drawFrame(c, f);
  });
  time("menu", 400, [&](int) { drawMenu(c, pc, "Friend Radar", rows, nr, ListView()); });
  KeyboardView kb;
  snprintf(kb.title, sizeof kb.title, "Your name");
  snprintf(kb.text, sizeof kb.text, "Kyle");
  time("keyboard", 400, [&](int) { drawKeyboard(c, pc, kb); });
  time("page slide", 400, [&](int i) { composeSlide(c, gUnder, (float)(i % 10) / 10.f, i & 1); });
}

int main(int argc, char **argv) {
  if (argc > 1) gOut = argv[1];
  bool frames = false, bench = false;
  for (int i = 2; i < argc; ++i) {
    if (!strcmp(argv[i], "--frames")) frames = true;
    if (!strcmp(argv[i], "--bench")) bench = true;
  }
  Canvas c(gFrame, layout::W, layout::H);
  gScene.attach(gBg, gPolar, gOverlay, gClean);
  gScene.build();
  { Canvas b(gBackdrop, layout::W, layout::H); buildPageBackground(b); }
  PageChrome pc;
  pc.backdrop = gBackdrop;
  pc.onAir = true;
  pc.tMs = 1200;

  // A lively scene: three mates and two other EWatches.
  PeerView peers[] = {
    peer(0x5A17C0DE, "Sam",   true,  Zone::RightHere, 0.5f, 0.5f, 120),
    peer(0x1E3A0042, "Ewan",  true,  Zone::Near,      0.45f, 2.1f, 80),
    peer(0x77E1A9B3, "Priya", true,  Zone::Around,    0.6f, 6.4f, 300),
    peer(0x0BADF00D, "EWatch 3F2A", false, Zone::Far, 0.35f, 14.f, 900),
    peer(0x600DCAFE, "Jo",    false, Zone::Around,    0.25f, 4.8f, 2500, kFlagBackground),
  };
  const int np = (int)(sizeof peers / sizeof peers[0]);
  gAnim.update(peers, np, 0);
  for (uint32_t t = 0; t <= 2000; t += 40) gAnim.step(t);
  gNb = gAnim.blips(gBlips, kMaxPeers, 0);

  // ---- the radar -----------------------------------------------------------------
  gScene.drawFrame(c, liveFrame(1234, 64.f));
  save("radar.png");

  RadarFrame mf = liveFrame(2000, 200.f);
  cardFor(mf, 0x1E3A0042, 1.f);
  mf.card.zone = Zone::Near; mf.card.distM = 2.1f; mf.card.agoSec = 0; mf.card.bars = 3;
  gScene.drawFrame(c, mf);
  save("radar_card_mate.png");

  RadarFrame of = liveFrame(2600, 300.f);
  cardFor(of, 0x0BADF00D, 1.f);
  of.card.zone = Zone::Far; of.card.distM = 14.f; of.card.agoSec = 1; of.card.bars = 1;
  gScene.drawFrame(c, of);
  save("radar_card_other.png");

  {
    RadarFrame e = liveFrame(600, 300.f);
    e.blips = nullptr; e.nBlips = 0;
    snprintf(e.hud.footer, sizeof e.hud.footer, "Looking for EWatches nearby" FR_ELLIPSIS);
    gScene.drawFrame(c, e);
    save("radar_empty.png");
  }

  // Motion: power-on, then tapping Ewan (the camera locks on, the card springs up).
  {
    Sheet sh(3, 2);
    const float boots[3] = {0.25f, 0.55f, 1.f};
    for (int i = 0; i < 3; ++i) {
      RadarFrame f = liveFrame(300 + i * 200, 30.f + i * 40.f);
      f.boot = boots[i];
      gScene.drawFrame(c, f);
      sh.put(i, gFrame);
    }
    Spring s(13.f, 0.72f);
    s.snap(0.f);
    s.target = 1.f;
    int idx = 3;
    for (int step = 1; step <= 24 && idx < 6; ++step) {
      s.step(0.02f);
      if (step == 5 || step == 10 || step == 24) {
        RadarFrame f = liveFrame(2000 + step * 20, 120.f + step * 2.f);
        cardFor(f, 0x1E3A0042, s.x);
        f.card.zone = Zone::Near; f.card.distM = 2.1f; f.card.bars = 3;
        gScene.drawFrame(c, f);
        sh.put(idx++, gFrame);
      }
    }
    sh.save("sheet_radar_motion.png");
  }

  // ---- shared animations: the same frames play on both watches ---------------------------
  {
    PairAnimSpec hello;
    hello.kind = AnimKind::Hello;
    hello.idA = 0x1E3A0042; hello.idB = 0x7777BBBB;
    hello.seed = pairSeed(hello.idA, hello.idB, 0x48454c4fu);
    snprintf(hello.name, sizeof hello.name, "Ewan");
    const int times[] = {250, 700, 1050, 1350, 1900, 2900};
    Sheet sh(3, 2);
    for (int i = 0; i < 6; ++i) {
      RadarFrame f = liveFrame((uint32_t)times[i], 40.f + times[i] * 0.09f);
      f.anim = &hello; f.animT = times[i];
      gScene.drawFrame(c, f);
      sh.put(i, gFrame);
      if (times[i] == 1900) save("hello.png");
    }
    sh.save("sheet_hello.png");

    PairAnimSpec cel = hello;
    cel.kind = AnimKind::Celebrate;
    cel.seed = pairSeed(cel.idA, cel.idB, 0x43454c42u);
    const int ct[] = {90, 350, 700, 1200, 1900, 2700};
    Sheet sc(3, 2);
    for (int i = 0; i < 6; ++i) {
      RadarFrame f = liveFrame((uint32_t)ct[i], 40.f + ct[i] * 0.09f);
      f.anim = &cel; f.animT = ct[i];
      gScene.drawFrame(c, f);
      sc.put(i, gFrame);
      if (ct[i] == 700) save("celebrate.png");
    }
    sc.save("sheet_celebrate.png");

    // Another pair gets a different signature.
    PairAnimSpec other = hello;
    other.idA = 0x5A17C0DE; other.idB = 0x13579BDF;
    other.seed = pairSeed(other.idA, other.idB, 0x48454c4fu);
    snprintf(other.name, sizeof other.name, "Sam");
    RadarFrame f = liveFrame(1900, 200.f);
    f.anim = &other; f.animT = 1900;
    gScene.drawFrame(c, f);
    save("hello_other_pair.png");
  }

  // ---- pages ------------------------------------------------------------------------------
  MenuRow rows[9];
  int nr = 0;
  {
    auto add = [&](MenuRow::Kind k, MenuRow::Icon ic, const char *title, const char *sub, const char *value,
                   bool on = false) {
      MenuRow &r = rows[nr++];
      r.kind = k; r.icon = ic; r.on = on;
      snprintf(r.title, sizeof r.title, "%s", title);
      snprintf(r.sub, sizeof r.sub, "%s", sub);
      snprintf(r.value, sizeof r.value, "%s", value);
    };
    add(MenuRow::Nav, MenuRow::IcPeople, "Mates", "", "3");
    add(MenuRow::Nav, MenuRow::IcTag, "My name", "", "Kyle");
    add(MenuRow::Nav, MenuRow::IcTarget, "Calibrate distance", "-62 dBm at 1 m", "");
    add(MenuRow::Toggle, MenuRow::IcBell, "Mate alerts", "Buzz + animation", "", true);
    add(MenuRow::Toggle, MenuRow::IcMoon, "Background alerts", "On " FR_MIDDOT " uses extra battery", "", true);
    add(MenuRow::Nav, MenuRow::IcClock, "Check every", "", "60 s");
    add(MenuRow::Nav, MenuRow::IcQuestion, "How it works", "", "");
    add(MenuRow::Danger, MenuRow::IcRefresh, "New radar ID", "Mates must re-add you", "");
  }
  MateRowView mates[4];
  {
    auto mate = [&](int i, uint32_t id, const char *n, bool live, Zone z, const char *st, float sig) {
      snprintf(mates[i].name, sizeof mates[i].name, "%s", n);
      mates[i].live = live; mates[i].zone = z; mates[i].color = mateColor(id); mates[i].signal = sig;
      snprintf(mates[i].status, sizeof mates[i].status, "%s", st);
    };
    mate(0, 0x5A17C0DE, "Sam", true, Zone::RightHere, "Right here now", 1.f);
    mate(1, 0x1E3A0042, "Ewan", true, Zone::Near, "Near now " FR_MIDDOT " ~2 m", 0.6f);
    mate(2, 0x77E1A9B3, "Priya", false, Zone::Lost, "Seen 2 h ago", 0.f);
    mate(3, 0x13579BDF, "Alex", false, Zone::Lost, "Not seen yet", 0.f);
  }
  MateDetailView md;
  {
    snprintf(md.name, sizeof md.name, "Ewan");
    md.live = true; md.zone = Zone::Near; md.distM = 2.2f;
    md.color = mateColor(0x1E3A0042); md.signal = 0.6f;
    const uint32_t now = 812345678u;
    const uint32_t when[5] = {now - 600, now - 86400 - 3000, now - 3 * 86400, now - 9 * 86400, now - 40 * 86400};
    const uint16_t mins[5] = {12, 95, 3, 40, 0};
    const Zone cz[5] = {Zone::Near, Zone::RightHere, Zone::Around, Zone::Near, Zone::Far};
    md.nLog = 5;
    for (int i = 0; i < 5; ++i) {
      formatWhen(when[i], now, md.when[i], sizeof md.when[i]);
      formatMinutes(mins[i], md.dur[i], sizeof md.dur[i]);
      md.closest[i] = cz[i];
    }
  }
  KeyboardView kb;
  snprintf(kb.title, sizeof kb.title, "Your name");
  snprintf(kb.text, sizeof kb.text, "Kyle");
  kb.shift = false; kb.tMs = 100; kb.pressedKey = 'e';
  CalibrateView cv;
  cv.peerAvailable = true;
  snprintf(cv.peer, sizeof cv.peer, "Ewan");

  {
    Sheet sh(3, 3);
    int idx = 0;
    drawMenu(c, pc, "Friend Radar", rows, nr, ListView());
    sh.put(idx++, gFrame); save("menu.png");
    ListView lv;
    lv.scroll = listMaxScroll(nr); lv.pressed = 4;
    MenuRow anim[9];
    memcpy(anim, rows, sizeof rows);
    anim[4].knob = 0.55f;                     // the switch mid-flight
    drawMenu(c, pc, "Friend Radar", anim, nr, lv);
    sh.put(idx++, gFrame); save("menu_scrolled.png");

    ListView mv;
    mv.pressed = 1;
    drawMates(c, pc, mates, 4, mv);
    sh.put(idx++, gFrame); save("mates.png");
    drawMates(c, pc, mates, 0, ListView());
    sh.put(idx++, gFrame); save("mates_empty.png");

    drawMateDetail(c, pc, md);
    sh.put(idx++, gFrame); save("mate_detail.png");

    drawKeyboard(c, pc, kb);
    sh.put(idx++, gFrame); save("keyboard.png");
    KeyboardView ks = kb;
    ks.symbols = true; ks.pressedKey = kKeyNone; ks.tMs = 700;
    snprintf(ks.title, sizeof ks.title, "Nickname");
    snprintf(ks.text, sizeof ks.text, "Ewan-2");
    drawKeyboard(c, pc, ks);
    sh.put(idx++, gFrame); save("keyboard_symbols.png");

    drawCalibrate(c, pc, cv);
    sh.put(idx++, gFrame); save("calibrate.png");
    CalibrateView cm = cv;
    cm.phase = CalibrateView::Measuring; cm.liveRssi = -61; cm.samples = 37; cm.tMs = 400;
    drawCalibrate(c, pc, cm);
    sh.put(idx++, gFrame); save("calibrate_measuring.png");
    sh.save("sheet_screens.png");
  }
  {
    Sheet s2(3, 2);
    CalibrateView cr = cv;
    cr.phase = CalibrateView::Result; cr.result = -62; cr.spread = 4; cr.samples = 60;
    drawCalibrate(c, pc, cr);
    s2.put(0, gFrame); save("calibrate_result.png");
    CalibrateView cf = cv;
    cf.phase = CalibrateView::Failed; cf.failReason = CalResult::TooNoisy;
    drawCalibrate(c, pc, cf);
    s2.put(1, gFrame); save("calibrate_failed.png");
    drawHelp(c, pc, 0);
    s2.put(2, gFrame); save("help.png");
    drawHelp(c, pc, 260);
    s2.put(3, gFrame);
    // Toast over the menu.
    drawMenu(c, pc, "Friend Radar", rows, nr, ListView());
    drawToast(c, "Ewan is now a mate", 1.f);
    s2.put(4, gFrame); save("toast.png");
    // A page push mid-flight: radar -> menu.
    gScene.drawFrame(c, liveFrame(1234, 64.f));
    memcpy(gUnder, gFrame, sizeof gFrame);
    drawMenu(c, pc, "Friend Radar", rows, nr, ListView());
    composeSlide(c, gUnder, 0.62f, true);
    s2.put(5, gFrame); save("push_transition.png");
    s2.save("sheet_screens2.png");
  }

  if (frames) {
    // Beam sweep sequence for checking motion.
    for (int f = 0; f < 8; ++f) {
      char name[32];
      snprintf(name, sizeof name, "frame_%02d.png", f);
      gScene.drawFrame(c, liveFrame((uint32_t)(f * 50), (float)(f * 45)));
      save(name, 1);
    }
  }
  if (bench) benchmark(c, pc, rows, nr);
  return 0;
}
