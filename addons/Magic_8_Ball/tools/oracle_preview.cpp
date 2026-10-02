// Host preview renderer for Shake Oracle.
//
// Drives the real OracleApp (the same code the watch runs) with synthetic
// accelerometer samples, an idle spell, a 4 Hz shake and then stillness,
// and writes PNG frames exactly as the RGB565 panel would show them.
//
// Build and run (from the addon folder):
//   tools/make_previews.sh            # writes docs/*.png
//
// Usage: oracle_preview <out_dir> [--frames]
//   --frames  also dump every frame of each scenario as <scenario>_NNNN.png
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "FreeSansBold24pt7b.h"
#include "oracle_app.h"
#include "png_write.h"

using namespace oracle;

namespace {

uint16_t gFb[kScreenW * kScreenH];

void savePng(const std::string &path, int scale = 1) {
  std::vector<uint8_t> rgb(kScreenW * kScreenH * 3);
  pngw::rgb565ToRgb(gFb, kScreenW * kScreenH, rgb.data());
  if (!pngw::writeRGB(path, rgb.data(), kScreenW, kScreenH, scale))
    fprintf(stderr, "could not write %s\n", path.c_str());
}

// Deterministic jitter for sample timing.
uint32_t gLcg = 12345;
float jitter() {
  gLcg = gLcg * 1664525u + 1013904223u;
  return ((gLcg >> 8) & 0xFFFF) / 65535.f - 0.5f;
}

struct Scenario {
  const char *name;
  int         pack;
  const char *answer;   // forced answer text (nullptr: random pick)
  bool        golden;
  // Named captures: time in ms after the shake stops (negative: before the
  // shake starts, measured from t = 0 as -t), plus file names.
  struct Shot { int whenMs; const char *file; bool afterStop; };
  std::vector<Shot> shots;
};

// Synthetic wrist: held in front of the face with the screen tilted toward
// you (screen top up), a little sway, and a vigorous side-to-side shake.
void accelAt(float tS, float shakeFrom, float shakeTo, float &ax, float &ay, float &az) {
  ax = 0.03f * sinf(tS * 2.9f);
  ay = 0.55f + 0.03f * sinf(tS * 1.7f);
  az = 0.83f;
  if (tS >= shakeFrom && tS < shakeTo) {
    float u = tS - shakeFrom;
    float env = fminf(1.f, u / 0.15f) * fminf(1.f, (shakeTo - tS) / 0.1f);
    float w = 2.f * 3.14159265f * 4.2f;
    ax += env * 1.7f * sinf(w * u);
    ay += env * 0.5f * sinf(w * u + 0.6f);
    az += env * 0.3f * sinf(w * u * 2.f);
  }
  // The MMA8451 at +-2 g clips.
  ax = fmaxf(-2.f, fminf(1.999f, ax));
  ay = fmaxf(-2.f, fminf(1.999f, ay));
  az = fmaxf(-2.f, fminf(1.999f, az));
}

void *hostAlloc(size_t n, bool) { return calloc(1, n); }

bool runScenario(OracleApp &app, const Scenario &sc, const std::string &outDir, bool frames) {
  app.setPack(sc.pack);
  app.setImuOk(true);
  app.enter(gFb, 0);
  if (sc.answer) app.forceNextAnswer(sc.answer, sc.golden);

  const float shakeFrom = 1.8f, shakeTo = 3.2f;
  uint32_t t = 0, nextSample = 0;
  int stopMs = -1;
  size_t shot = 0;
  int frameNo = 0;
  bool printed = false;
  while (t < 9000 && shot < sc.shots.size()) {
    // Feed every IMU sample up to now (~45 Hz with jitter, like taskIO).
    while (nextSample <= t) {
      float ax, ay, az;
      accelAt(nextSample * 0.001f, shakeFrom, shakeTo, ax, ay, az);
      app.imuSample(nextSample, (int16_t)lroundf(ax * kCountsPerG),
                    (int16_t)lroundf(ay * kCountsPerG), (int16_t)lroundf(az * kCountsPerG));
      nextSample += 22 + (int)(jitter() * 6.f);
    }
    FrameOut out;
    app.frame(gFb, t, out);
    if (stopMs < 0 && app.scene().phase() == Phase::Rising) {
      stopMs = (int)t;
      if (!printed) {
        printf("  %-8s shake stopped at %.2f s -> \"%s\"%s\n", sc.name, t / 1000.f, app.answer(),
               app.answerGolden() ? " (golden)" : "");
        printed = true;
      }
    }
    if (frames) {
      char p[512];
      snprintf(p, sizeof p, "%s/%s_%04d.png", outDir.c_str(), sc.name, frameNo);
      savePng(p);
      snprintf(p, sizeof p, "%s/%s_frames.csv", outDir.c_str(), sc.name);
      FILE *f = fopen(p, frameNo == 0 ? "w" : "a");
      if (f) {
        fprintf(f, "%d,%u,%d,%d\n", frameNo, (unsigned)t, (int)app.scene().phase(), out.buzzCount);
        fclose(f);
      }
    }
    frameNo++;
    // Captures due?
    while (shot < sc.shots.size()) {
      const auto &s = sc.shots[shot];
      int due = s.afterStop ? (stopMs < 0 ? -1 : stopMs + s.whenMs) : s.whenMs;
      if (due < 0 || (int)t < due) break;
      savePng(outDir + "/" + s.file);
      printf("    wrote %s at %.2f s (phase %d)\n", s.file, t / 1000.f, (int)app.scene().phase());
      shot++;
    }
    t += app.framePeriodMs();
  }
  return shot == sc.shots.size();
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <out_dir> [--frames]\n", argv[0]);
    return 2;
  }
  std::string out = argv[1];
  bool frames = argc > 2 && strcmp(argv[2], "--frames") == 0;

  OracleApp app;
  if (!app.begin(&FreeSansBold24pt7b, hostAlloc, 0x5EED0F0A11ULL)) {
    fprintf(stderr, "OracleApp::begin failed\n");
    return 1;
  }
  std::vector<Scenario> scenarios = {
      {"classic", 0, "The deep says yes", false,
       {{1500, "idle.png", false},
        {2700, "churning.png", false},
        {180, "rising.png", true},
        {2600, "answer.png", true}}},
      {"long", 0, "Try a better question", false, {{2600, "answer_long.png", true}}},
      {"golden", 0, nullptr, true, {{1300, "golden.png", true}}},
      {"yesno", 1, "Nope", false, {{1500, "idle_yesno.png", false}, {2600, "answer_nope.png", true}}},
      {"food", 2, "Dumplings", false, {{1500, "idle_food.png", false}, {2600, "answer_food.png", true}}},
      {"excuses", 3, "A goose blocked the path", false,
       {{2600, "answer_excuse.png", true}}},
  };
  // The golden scenario uses the pack's own jackpot text.
  scenarios[2].answer = packAt(0).golden;

  bool ok = true;
  for (const auto &sc : scenarios) ok = runScenario(app, sc, out, frames) && ok;
  if (!ok) {
    fprintf(stderr, "some captures were not reached\n");
    return 1;
  }
  return 0;
}
