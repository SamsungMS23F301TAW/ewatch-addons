// Tilt Parallax host preview tool.
//
// Renders the real watch-face pipeline (scene generators, sky, LUTs, clock,
// compositor) on the host and writes PNGs, so the art can be inspected before
// it ever reaches a watch. See tools/render_previews.sh.
//
//   preview frame <out.png> [--scene N] [--time HH:MM] [--date YYYY-MM-DD]
//                 [--tilt X,Y] [--strength S] [--scale K] [--battery P] [--h12]
//   preview sheet <out.png> --scene N [--times HH:MM,...] [--tilts X,Y;X,Y...]
//                 [--date ...] [--scale K]
//   preview bench [--scene N]          timing of generation and composition
//   preview sweep <dir> [--scene N] [--time ..] [--frames K]
//                 frames of a figure-eight wrist tilt (for GIFs)
//   preview transition <dir> --scene A --to B [--time ..]
//                 frames (30 fps) of the scene-change animation
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>
#include <chrono>
#include <zlib.h>
#include "tp_engine.h"
#include "tp_scene.h"
#include "tp_compose.h"
#include "tp_transition.h"
#include "tp_tilt.h"

using namespace tp;

// ---------------------------------------------------------------------------
// Minimal PNG writer (RGB8).
// ---------------------------------------------------------------------------
static void be32(std::vector<uint8_t> &v, uint32_t x) {
  v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x);
}
static void chunk(FILE *f, const char *type, const std::vector<uint8_t> &data) {
  std::vector<uint8_t> buf;
  be32(buf, (uint32_t)data.size());
  buf.insert(buf.end(), type, type + 4);
  buf.insert(buf.end(), data.begin(), data.end());
  uint32_t crc = crc32(0, buf.data() + 4, (uInt)(buf.size() - 4));
  be32(buf, crc);
  fwrite(buf.data(), 1, buf.size(), f);
}
static bool writePng(const char *path, const std::vector<uint8_t> &rgb, int w, int h) {
  FILE *f = fopen(path, "wb");
  if (!f) { perror(path); return false; }
  static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
  fwrite(sig, 1, 8, f);
  std::vector<uint8_t> ihdr;
  be32(ihdr, w); be32(ihdr, h);
  ihdr.push_back(8); ihdr.push_back(2); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
  chunk(f, "IHDR", ihdr);
  std::vector<uint8_t> raw;
  raw.reserve((size_t)(w * 3 + 1) * h);
  for (int y = 0; y < h; y++) {
    raw.push_back(0);
    raw.insert(raw.end(), rgb.begin() + (size_t)y * w * 3, rgb.begin() + (size_t)(y + 1) * w * 3);
  }
  uLongf clen = compressBound((uLong)raw.size());
  std::vector<uint8_t> comp(clen);
  compress2(comp.data(), &clen, raw.data(), (uLong)raw.size(), 9);
  comp.resize(clen);
  chunk(f, "IDAT", comp);
  chunk(f, "IEND", std::vector<uint8_t>());
  fclose(f);
  return true;
}

static void to888(uint16_t c, uint8_t *o) {
  int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
  o[0] = (uint8_t)((r << 3) | (r >> 2));
  o[1] = (uint8_t)((g << 2) | (g >> 4));
  o[2] = (uint8_t)((b << 3) | (b >> 2));
}

// ---------------------------------------------------------------------------
struct Opts {
  int scene = 0;
  LocalTime t;
  float tx = 0, ty = 0, strength = 1.0f;
  int scale = 2;
  int battery = 78;
  bool h12 = false;
  bool round = true;       // draw the panel's rounded corners
  int toScene = 1;
  int frames = 48;
  std::vector<std::string> times;
  std::vector<std::pair<float, float>> tilts;
};

static const char *kWd[] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
static const char *kMo[] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                             "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
static int weekday(int y, int m, int d) {
  static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
  if (m < 3) y -= 1;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

static void parseTime(const char *s, LocalTime &t) {
  int h = 12, m = 0;
  sscanf(s, "%d:%d", &h, &m);
  t.hour = h; t.minute = m; t.second = 0;
}
static void parseDate(const char *s, LocalTime &t) {
  int y = 2026, mo = 6, d = 21;
  sscanf(s, "%d-%d-%d", &y, &mo, &d);
  t.year = y; t.month = mo; t.day = d;
}

// Renders one frame into rgb (240x280x3).
static void renderFrame(FaceRenderer &fr, Scene &scene, const Opts &o, const LocalTime &t,
                        float tx, float ty, std::vector<uint8_t> &out) {
  (void)scene;
  fr.setTimeOfDay(t, true);
  fr.setClock(t.hour, t.minute, true, o.h12);
  StatusInfo st;
  snprintf(st.date, sizeof(st.date), "%s %d %s", kWd[weekday(t.year, t.month, t.day)], t.day,
           kMo[(t.month + 11) % 12]);
  st.battery = o.battery;
  fr.setStatus(st);
  fr.setParallax(tx, ty, o.strength);
  fr.markAllDirty();
  std::vector<uint16_t> fb(kScreenW * kScreenH);
  int y0, y1;
  while (fr.popDirtyRun(y0, y1)) fr.composeRows(y0, y1, fb.data() + y0 * kScreenW, kScreenW);
  out.assign(kScreenW * kScreenH * 3, 0);
  for (int i = 0; i < kScreenW * kScreenH; i++) to888(fb[i], &out[i * 3]);
  if (o.round) {
    // The panel has rounded corners (~radius 36 px); mask them so previews
    // show what's actually visible.
    const float R = 36;
    for (int y = 0; y < kScreenH; y++)
      for (int x = 0; x < kScreenW; x++) {
        float cx = x < R ? R : (x > kScreenW - 1 - R ? kScreenW - 1 - R : x);
        float cy = y < R ? R : (y > kScreenH - 1 - R ? kScreenH - 1 - R : y);
        float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy));
        float a = d > R ? 0 : (d > R - 1 ? R - d : 1);
        for (int c = 0; c < 3; c++) out[(y * kScreenW + x) * 3 + c] = (uint8_t)(out[(y * kScreenW + x) * 3 + c] * a);
      }
  }
}

static void upscale(const std::vector<uint8_t> &in, int w, int h, int k, std::vector<uint8_t> &out) {
  out.assign((size_t)w * k * h * k * 3, 0);
  for (int y = 0; y < h * k; y++)
    for (int x = 0; x < w * k; x++)
      memcpy(&out[((size_t)y * w * k + x) * 3], &in[((size_t)(y / k) * w + x / k) * 3], 3);
}

static double msSince(std::chrono::high_resolution_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: preview frame|sheet|bench ...\n");
    return 1;
  }
  std::string mode = argv[1];
  const char *outPath = nullptr;
  Opts o;
  int ai = 2;
  if (mode != "bench") {
    if (argc < 3) { fprintf(stderr, "missing output path\n"); return 1; }
    outPath = argv[2];
    ai = 3;
  }
  for (; ai < argc; ai++) {
    std::string a = argv[ai];
    auto next = [&]() -> const char * { return ai + 1 < argc ? argv[++ai] : ""; };
    if (a == "--scene") o.scene = atoi(next());
    else if (a == "--time") parseTime(next(), o.t);
    else if (a == "--date") parseDate(next(), o.t);
    else if (a == "--tilt") sscanf(next(), "%f,%f", &o.tx, &o.ty);
    else if (a == "--strength") o.strength = (float)atof(next());
    else if (a == "--scale") o.scale = atoi(next());
    else if (a == "--to") o.toScene = atoi(next());
    else if (a == "--frames") o.frames = atoi(next());
    else if (a == "--battery") o.battery = atoi(next());
    else if (a == "--h12") o.h12 = true;
    else if (a == "--square") o.round = false;
    else if (a == "--times") {
      std::string s = next();
      size_t p = 0;
      while (p <= s.size()) {
        size_t q = s.find(',', p);
        if (q == std::string::npos) q = s.size();
        if (q > p) o.times.push_back(s.substr(p, q - p));
        p = q + 1;
      }
    } else if (a == "--tilts") {
      std::string s = next();
      size_t p = 0;
      while (p <= s.size()) {
        size_t q = s.find(';', p);
        if (q == std::string::npos) q = s.size();
        float x = 0, y = 0;
        if (q > p && sscanf(s.substr(p, q - p).c_str(), "%f,%f", &x, &y) == 2)
          o.tilts.push_back(std::make_pair(x, y));
        p = q + 1;
      }
    } else {
      fprintf(stderr, "unknown option %s\n", a.c_str());
      return 1;
    }
  }

  Scene *scene = new Scene();
  auto t0 = std::chrono::high_resolution_clock::now();
  if (!buildScene(o.scene, *scene)) { fprintf(stderr, "buildScene failed\n"); return 1; }
  double buildMs = msSince(t0);
  FaceRenderer *fr = new FaceRenderer();
  if (!fr->begin()) { fprintf(stderr, "renderer begin failed\n"); return 1; }
  fr->setScene(scene);

  if (mode == "bench") {
    printf("scene %d (%s): build %.1f ms host, %zu KB layers, %zu KB renderer\n", o.scene,
           sceneName(o.scene), buildMs, scene->bytes() / 1024, fr->bytes() / 1024);
    for (int i = 0; i < scene->count; i++) {
      const Layer &L = scene->layers[i];
      printf("  layer %d depth %.2f  %dx%d  spans %u (%.1f/row)  %zu KB\n", i, L.depth, L.w, L.h,
             L.spanCount, (double)L.spanCount / L.h, L.bytes() / 1024);
    }
    LocalTime t = o.t;
    auto t1 = std::chrono::high_resolution_clock::now();
    fr->setTimeOfDay(t, true);
    printf("  sky+luts: %.2f ms host\n", msSince(t1));
    t1 = std::chrono::high_resolution_clock::now();
    fr->setClock(t.hour, t.minute, true, false);
    printf("  clock: %.2f ms host\n", msSince(t1));
    std::vector<uint16_t> fb(kScreenW * kScreenH);
    const int frames = 2000;
    t1 = std::chrono::high_resolution_clock::now();
    for (int f = 0; f < frames; f++) {
      float a = f * 0.05f;
      fr->setParallax(sinf(a), cosf(a * 0.7f) * 0.6f, 1.0f);
      fr->markAllDirty();
      int y0, y1;
      while (fr->popDirtyRun(y0, y1)) fr->composeRows(y0, y1, fb.data() + y0 * kScreenW, kScreenW);
    }
    printf("  full-frame compose: %.3f ms host avg\n", msSince(t1) / frames);
    // Realistic glance: wrist rocking +-10 deg roll and +-6 deg pitch at
    // ~0.6 Hz, accel at ~45 Hz through the real tilt filter, frames every
    // 27 ms (the watch's estimated full-frame time). Counts rows to send.
    {
      TiltFilter tf;
      uint32_t t = 1000, nextSample = t;
      double rows = 0;
      int framesPushed = 0, framesTotal = 0, full = 0;
      fr->markAllDirty();
      int y0, y1;
      while (fr->popDirtyRun(y0, y1)) {}
      for (int f = 0; f < 400; f++) {
        t += 27;
        while (nextSample <= t) {
          float s = nextSample * 0.001f;
          float roll = 10.0f * sinf(2 * 3.14159f * 0.6f * s);
          float pitch = 35.0f + 6.0f * sinf(2 * 3.14159f * 0.45f * s + 1.0f);
          float b = pitch * 3.14159f / 180.0f, r = roll * 3.14159f / 180.0f;
          float ux = cosf(b) * sinf(r), uy = sinf(b), uz = cosf(b) * cosf(r);
          tf.addSample((int16_t)(-ux * 4096), (int16_t)(uy * 4096), (int16_t)(uz * 4096), nextSample);
          nextSample += 20 + (nextSample % 5);
        }
        float ox, oy;
        tf.step(t, ox, oy);
        fr->setParallax(ox, oy, 1.0f);
        int n = fr->dirtyCount();
        while (fr->popDirtyRun(y0, y1)) {}
        framesTotal++;
        if (n) { framesPushed++; rows += n; if (n >= kScreenH - 2) full++; }
      }
      printf("  motion sim: %d/%d frames needed a push, %.0f rows avg, %d%% full-screen\n",
             framesPushed, framesTotal, framesPushed ? rows / framesPushed : 0.0,
             framesPushed ? full * 100 / framesPushed : 0);
    }
    return 0;
  }

  std::vector<uint8_t> frame;
  // Composes whatever the renderer currently shows (no state changes).
  auto grab = [&](std::vector<uint8_t> &out) {
    std::vector<uint16_t> fb(kScreenW * kScreenH);
    fr->markAllDirty();
    int y0, y1;
    while (fr->popDirtyRun(y0, y1)) fr->composeRows(y0, y1, fb.data() + y0 * kScreenW, kScreenW);
    out.assign(kScreenW * kScreenH * 3, 0);
    for (int i = 0; i < kScreenW * kScreenH; i++) to888(fb[i], &out[i * 3]);
  };
  auto prime = [&](const LocalTime &t) {
    fr->setTimeOfDay(t, true);
    fr->setClock(t.hour, t.minute, true, o.h12);
    StatusInfo st;
    snprintf(st.date, sizeof(st.date), "%s %d %s", kWd[weekday(t.year, t.month, t.day)], t.day,
             kMo[(t.month + 11) % 12]);
    st.battery = o.battery;
    fr->setStatus(st);
  };
  if (mode == "sweep") {
    prime(o.t);
    for (int k = 0; k < o.frames; k++) {
      float a = 6.2831853f * k / o.frames;
      fr->setParallax(sinf(a), 0.45f * sinf(2 * a), o.strength);
      grab(frame);
      char path[512];
      snprintf(path, sizeof(path), "%s/f%03d.png", outPath, k);
      writePng(path, frame, kScreenW, kScreenH);
    }
    printf("wrote %d frames to %s\n", o.frames, outPath);
    return 0;
  }
  if (mode == "transition") {
    prime(o.t);
    fr->setParallax(o.tx, o.ty, o.strength);
    int k = 0;
    char path[512];
    auto out = [&]() {
      grab(frame);
      snprintf(path, sizeof(path), "%s/f%03d.png", outPath, k++);
      writePng(path, frame, kScreenW, kScreenH);
    };
    for (int i = 0; i < 6; i++) out();                       // hold
    bool done = false;
    float depths[Scene::kMaxLayers];
    for (int i = 0; i < scene->count; i++) depths[i] = scene->layers[i].depth;
    int ranks = transitionRank(depths, scene->count - 1) + 1;
    for (uint32_t t = 0; !done; t += 33) {                   // drop away
      done = true;
      for (int i = 0; i < scene->count; i++)
        if (transitionAnimates(depths[i]))
          fr->setLayerAnim(i, 0, transitionOutDy(transitionRank(depths, i), ranks, t, 0,
                                                 kScreenH + 8 - scene->layers[i].y0, &done));
      out();
    }
    Scene *next = new Scene();
    if (!buildScene(o.toScene, *next)) { fprintf(stderr, "build failed\n"); return 1; }
    fr->setScene(next);
    prime(o.t);
    fr->setParallax(o.tx, o.ty, o.strength);
    done = false;
    for (int i = 0; i < next->count; i++) depths[i] = next->layers[i].depth;
    for (uint32_t t = 0; !done; t += 33) {                   // rise in
      done = true;
      for (int i = 0; i < next->count; i++)
        if (transitionAnimates(depths[i]))
          fr->setLayerAnim(i, 0, transitionInDy(transitionRank(depths, i), t,
                                                kScreenH + 8 - next->layers[i].y0, &done));
      out();
    }
    for (int i = 0; i < 10; i++) out();
    printf("wrote %d frames to %s\n", k, outPath);
    return 0;
  }
  if (mode == "frame") {
    renderFrame(*fr, *scene, o, o.t, o.tx, o.ty, frame);
    std::vector<uint8_t> big;
    upscale(frame, kScreenW, kScreenH, o.scale, big);
    writePng(outPath, big, kScreenW * o.scale, kScreenH * o.scale);
    printf("wrote %s (build %.1f ms host)\n", outPath, buildMs);
    return 0;
  }
  if (mode == "sheet") {
    if (o.times.empty()) {
      char b[8];
      snprintf(b, sizeof(b), "%02d:%02d", o.t.hour, o.t.minute);
      o.times.push_back(b);
    }
    if (o.tilts.empty()) o.tilts.push_back(std::make_pair(0.0f, 0.0f));
    const int pad = 8;
    int cols = (int)o.times.size() * (int)o.tilts.size();
    int W = cols * (kScreenW * o.scale + pad) + pad, H = kScreenH * o.scale + 2 * pad;
    std::vector<uint8_t> sheet((size_t)W * H * 3, 24);
    int c = 0;
    for (size_t ti = 0; ti < o.times.size(); ti++) {
      LocalTime t = o.t;
      parseTime(o.times[ti].c_str(), t);
      for (size_t k = 0; k < o.tilts.size(); k++, c++) {
        renderFrame(*fr, *scene, o, t, o.tilts[k].first, o.tilts[k].second, frame);
        std::vector<uint8_t> big;
        upscale(frame, kScreenW, kScreenH, o.scale, big);
        int ox = pad + c * (kScreenW * o.scale + pad), oy = pad;
        for (int y = 0; y < kScreenH * o.scale; y++)
          memcpy(&sheet[((size_t)(oy + y) * W + ox) * 3], &big[(size_t)y * kScreenW * o.scale * 3],
                 (size_t)kScreenW * o.scale * 3);
      }
    }
    writePng(outPath, sheet, W, H);
    printf("wrote %s (%d frames, build %.1f ms host)\n", outPath, c, buildMs);
    return 0;
  }
  fprintf(stderr, "unknown mode %s\n", mode.c_str());
  return 1;
}
