// Host preview + golden-frame generator for the EWatch Companion face.
//
// Compiles the firmware's own face renderer (src/apps/face/*) against the real
// Arduino_GFX core and Arduino_Canvas (from .pio/libdeps) behind a tiny
// Arduino shim, renders every scenario in test/vectors/face_scenarios.json,
// and writes raw RGB565 frames for tools/preview/render.py to turn into PNGs.
//
// It also self-checks the incremental renderer: for each scenario pair (A, B)
// it draws A, then B on top, pushes only the reported dirty rows to a
// simulated panel, and requires both the canvas and the panel to equal a
// fresh render of B. Exit code 1 on any mismatch.
//
// Build + run via:  python3 tools/preview/render.py
#include <Arduino_GFX.h>
#include <canvas/Arduino_Canvas.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "ble_proto.h"
#include "face_icons.h"
#include "face_render.h"
#include "face_slots.h"
#include "json_mini.h"

using namespace bleproto;

static const int W = 240, H = 280;

struct Scenario {
  std::string name;
  FaceRender::Frame f;
  TextEntry texts[kTextCount];
  FeedEntry feeds[kFeedCount];
  // The frame points into this struct's own arrays; re-aim after any copy.
  void fix() { f.texts = texts; f.feeds = feeds; }
};

static uint16_t hexColor(const jm::Value &v) {
  return (uint16_t)strtoul(v.s.c_str(), nullptr, 16);
}

static Scenario parseScenario(const jm::Value &def, const jm::Value &over) {
  auto pick = [&](const char *k) -> const jm::Value & { return over.has(k) ? over[k] : def[k]; };
  Scenario s;
  memset(&s.f, 0, sizeof(s.f));
  memset(s.texts, 0, sizeof(s.texts));
  memset(s.feeds, 0, sizeof(s.feeds));
  s.name = over["name"].s;
  const jm::Value &th = pick("theme");
  s.f.bg = hexColor(th[0]); s.f.fg = hexColor(th[1]);
  s.f.accent = hexColor(th[2]); s.f.line = hexColor(th[3]);
  s.f.style = (uint8_t)pick("style").u();
  s.f.options = (uint8_t)pick("options").u();
  s.f.rtcOk = pick("rtcOk").b;
  const jm::Value &n = pick("now");
  s.f.now.year = (uint16_t)n[0].u(); s.f.now.month = (uint8_t)n[1].u(); s.f.now.day = (uint8_t)n[2].u();
  s.f.now.hour = (uint8_t)n[3].u(); s.f.now.minute = (uint8_t)n[4].u(); s.f.now.second = (uint8_t)n[5].u();
  s.f.now.weekday = (uint8_t)n[6].u();
  int16_t tz = (int16_t)pick("tz").i();
  uint32_t unix = civilToUnix(s.f.now, tz);
  s.f.unixNow = s.f.rtcOk ? unix : 0;
  s.f.batOk = pick("batOk").b;
  s.f.batPct = (uint8_t)pick("batPct").u();
  const jm::Value &wf = pick("wifi");
  s.f.wifiShown = wf[0].b; s.f.wifiEnabled = wf[1].b; s.f.wifiAp = wf[2].b;
  s.f.wifiConnected = wf[3].b; s.f.wifiRssi = (int8_t)wf[4].i();
  s.f.ble = (uint8_t)pick("ble").u();
  const jm::Value &tm = pick("timers");
  s.f.swRun = tm[0].b; s.f.swMs = (uint32_t)tm[1].n; s.f.tmrOn = tm[2].b; s.f.tmrMs = (uint32_t)tm[3].n;
  const jm::Value &sl = pick("slots");
  for (uint8_t i = 0; i < kSlotCount; i++) {
    s.f.slots[i].source = (uint8_t)sl[i][0].u(); s.f.slots[i].arg = (uint8_t)sl[i][1].u();
    s.f.slots[i].color = (uint8_t)sl[i][2].u();  s.f.slots[i].flags = (uint8_t)sl[i][3].u();
  }
  const jm::Value &tx = pick("texts");
  for (uint8_t i = 0; i < kTextCount; i++) {
    const std::string &t = tx[i].s;
    s.texts[i].len = (uint8_t)(t.size() > kTextMax ? kTextMax : t.size());
    memcpy(s.texts[i].text, t.data(), s.texts[i].len);
  }
  const jm::Value &fd = pick("feeds");
  for (uint8_t i = 0; i < kFeedCount; i++) {
    const jm::Value &e = fd[i];
    FeedEntry &f = s.feeds[i];
    f.icon = (uint8_t)e["icon"].u();
    f.kind = (uint8_t)e["kind"].u();
    long long ex = e["expiresIn"].i(), tg = e["targetIn"].i();
    f.expiresAt = ex ? (uint32_t)((long long)unix + ex) : 0;
    f.targetAt = (f.kind == FEED_COUNTDOWN) ? (uint32_t)((long long)unix + tg) : 0;
    const std::string &t = e["text"].s;
    f.len = (uint8_t)(t.size() > kFeedTextMax ? kFeedTextMax : t.size());
    memcpy(f.text, t.data(), f.len);
  }
  s.fix();
  return s;
}

static bool writeFrame(const std::string &path, const uint16_t *fb) {
  FILE *fp = fopen(path.c_str(), "wb");
  if (!fp) return false;
  for (int i = 0; i < W * H; i++) {
    uint8_t b[2] = { (uint8_t)(fb[i] & 0xFF), (uint8_t)(fb[i] >> 8) };   // little-endian
    fwrite(b, 1, 2, fp);
  }
  fclose(fp);
  return true;
}

static Arduino_Canvas *newCanvas() {
  Arduino_Canvas *c = new Arduino_Canvas(W, H, nullptr);
  c->begin(GFX_SKIP_OUTPUT_BEGIN);
  return c;
}

static void renderFresh(Scenario s, std::vector<uint16_t> &out) {
  s.fix();
  Arduino_Canvas *c = newCanvas();
  FaceRender::Renderer r;
  r.draw(c, s.f, c->getFramebuffer());
  out.assign(c->getFramebuffer(), c->getFramebuffer() + W * H);
  delete c;
}

static int countDiff(const uint16_t *a, const uint16_t *b) {
  int n = 0;
  for (int i = 0; i < W * H; i++) n += a[i] != b[i];
  return n;
}

// Every icon at text sizes 2 and 3, plus battery levels, on two backgrounds.
static void renderIconSheet(const std::string &path) {
  Arduino_Canvas *c = newCanvas();
  c->fillScreen(0x0000);
  c->fillRect(120, 0, 120, H, 0xFFDF);
  struct Item { uint8_t id, arg; };
  std::vector<Item> items;
  for (uint8_t id = 1; id <= kIconMax; id++) items.push_back(Item{ id, 255 });
  items.push_back(Item{ faceslots::kIconBattery, 100 });
  items.push_back(Item{ faceslots::kIconBattery, 55 });
  items.push_back(Item{ faceslots::kIconBattery, 10 });
  items.push_back(Item{ faceslots::kIconBattery, 255 });
  for (size_t i = 0; i < items.size(); i++) {
    int col = (int)(i / 10), row = (int)(i % 10);
    int16_t y = (int16_t)(6 + row * 27);
    for (int side = 0; side < 2; side++) {
      uint16_t bg = side ? 0xFFDF : 0x0000, fg = side ? 0x18E3 : 0xFFFF;
      int16_t x = (int16_t)(side * 120 + 4 + col * 58);
      FaceIcons::draw(c, items[i].id, items[i].arg, x, (int16_t)(y + 4), 2, fg, bg);
      FaceIcons::draw(c, items[i].id, items[i].arg, (int16_t)(x + 24), y, 3, fg, bg);
    }
  }
  writeFrame(path, c->getFramebuffer());
  delete c;
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s scenarios.json outdir\n", argv[0]);
    return 2;
  }
  std::ifstream in(argv[1], std::ios::binary);
  if (!in) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
  std::stringstream ss;
  ss << in.rdbuf();
  std::string text = ss.str();
  jm::Value root = jm::Parser(text).parse();
  std::string outdir = argv[2];

  std::map<std::string, Scenario> byName;
  const jm::Value &list = root["scenarios"];
  for (size_t i = 0; i < list.size(); i++) {
    Scenario s = parseScenario(root["defaults"], list[i]);
    s.fix();
    std::vector<uint16_t> fb;
    renderFresh(s, fb);
    if (!writeFrame(outdir + "/" + s.name + ".rgb565", fb.data())) {
      fprintf(stderr, "cannot write frame %s\n", s.name.c_str());
      return 2;
    }
    byName[s.name] = s;
    printf("rendered %-14s\n", s.name.c_str());
  }
  renderIconSheet(outdir + "/icons.rgb565");
  printf("rendered %-14s\n", "icons");

  // Incremental consistency (canvas) and dirty-span coverage (panel).
  int failures = 0;
  const jm::Value &pairs = root["incremental"];
  for (size_t i = 0; i < pairs.size(); i++) {
    Scenario &a = byName[pairs[i][0].s];
    Scenario &b = byName[pairs[i][1].s];
    a.fix();
    b.fix();
    Arduino_Canvas *c = newCanvas();
    FaceRender::Renderer r;
    r.draw(c, a.f, c->getFramebuffer());
    std::vector<uint16_t> panel(c->getFramebuffer(), c->getFramebuffer() + W * H);
    r.draw(c, b.f, c->getFramebuffer());
    const uint16_t *fb = c->getFramebuffer();
    if (r.dirtyFull()) {
      panel.assign(fb, fb + W * H);
    } else {
      for (int k = 0; k < r.dirtyCount(); k++) {
        const FaceRender::Renderer::Span &sp = r.dirtySpan(k);
        memcpy(&panel[(size_t)sp.y0 * W], fb + (size_t)sp.y0 * W, (size_t)(sp.y1 - sp.y0) * W * 2);
      }
    }
    std::vector<uint16_t> fresh;
    renderFresh(b, fresh);
    int dCanvas = countDiff(fb, fresh.data());
    int dPanel = countDiff(panel.data(), fresh.data());
    printf("incremental %-14s -> %-14s canvas diff %d, panel diff %d, %s %d span(s)\n",
           a.name.c_str(), b.name.c_str(), dCanvas, dPanel,
           r.dirtyFull() ? "full" : "dirty", r.dirtyFull() ? 0 : r.dirtyCount());
    if (dCanvas || dPanel) failures++;
    delete c;
  }
  if (failures) { printf("FAIL: %d incremental mismatch(es)\n", failures); return 1; }
  printf("OK\n");
  return 0;
}
