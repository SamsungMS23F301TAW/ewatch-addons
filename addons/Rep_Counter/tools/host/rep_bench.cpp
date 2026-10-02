// Rep Counter host bench: runs the detector over randomised synthetic
// scenarios (test/support/rep_scenarios.h) and prints accuracy per category.
//
//   tools/host/build.sh && build/host/rep_bench [--n 40] [--cat press]
//        [--mode auto|curl|press|raise|row] [--set name=value ...] [--poll] [-v]
//
// --poll runs the REPS_IMU_FIFO=0 fallback instead: jittery ~45 Hz polls at
// +/-2 g, resampled to 50 Hz.
//
// Lifting categories report how often the count is exact / within one rep,
// the label accuracy and when the set ended. Negative categories (walking,
// running, fidgeting, gestures) report false sets and tentative reps.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "rep_csv.h"
#include "rep_harness.h"
#include "rep_scenarios.h"

int main(int argc, char **argv) {
  int n = 30;
  std::string only;
  bool verbose = false, polled = false, haveMode = false;
  reps::Mode mode = reps::Mode::Auto;
  reps::Tuning tune;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--n") && i + 1 < argc) n = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--cat") && i + 1 < argc) only = argv[++i];
    else if (!strcmp(argv[i], "-v")) verbose = true;
    else if (!strcmp(argv[i], "--poll")) polled = true;
    else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
      if (!csv::parseMode(argv[++i], mode)) { fprintf(stderr, "bad mode\n"); return 2; }
      haveMode = true;
    }
    else if (!strcmp(argv[i], "--set") && i + 1 < argc) {
      std::string kv = argv[++i];
      size_t eq = kv.find('=');
      float *f = eq == std::string::npos ? nullptr : reps::tuningField(tune, kv.substr(0, eq).c_str());
      if (!f) { fprintf(stderr, "unknown tuning '%s'\n", kv.c_str()); return 2; }
      *f = (float)atof(kv.c_str() + eq + 1);
    } else {
      fprintf(stderr, "usage: rep_bench [--n N] [--cat name] [--mode m] [--set name=value] [--poll] [-v]\n");
      return 2;
    }
  }
  printf("%-11s %5s %6s %6s %6s %6s %7s %7s %8s\n", "category", "sets", "exact", "+-1",
         "maxErr", "label", "endMean", "endMax", "falseSet");
  int failures = 0; (void)failures;
  for (int c = 0; c < scen::kNumCategories; c++) {
    std::string cat = scen::kCategories[c];
    if (!only.empty() && cat != only) continue;
    int sets = 0, exact = 0, within1 = 0, maxErr = 0, label = 0, falseSets = 0, tent = 0;
    double endSum = 0, endMax = -99;
    int endN = 0;
    for (int s = 1; s <= n; s++) {
      scen::Built b = scen::build(cat, (uint32_t)(s * 7919 + c * 104729));
      b.cfg.tune = tune;
      if (haveMode) b.cfg.mode = mode;
      if (polled) {                       // REPS_IMU_FIFO=0 fallback path
        b.xyz = harness::polledTo50Hz(b.xyz, (uint32_t)s);
        b.cfg.fs = 50.f;
      }
      harness::RunResult res = harness::run(b.xyz, b.cfg);
      harness::Score sc = harness::score(b.truth, res);
      sets += (int)b.truth.size();
      exact += sc.exact;
      falseSets += (int)sc.extra.size();
      tent += res.tentatives;
      if (sc.countErrMax > maxErr) maxErr = sc.countErrMax;
      label += sc.labelOk;
      for (auto &m : sc.matches) {
        int got = m.det ? m.det->reps : 0;
        if (std::abs(got - m.truth->reps) <= 1) within1++;
        if (m.det) {
          double e = m.det->tEnd - m.truth->tLastEnd;
          endSum += e; endN++;
          if (e > endMax) endMax = e;
        }
      }
      bool bad = sc.countErrMax > 0 || !sc.extra.empty();
      if (bad) failures++;
      if (verbose && bad) harness::print(b.desc.c_str(), b.truth, res, sc);
    }
    if (sets)
      printf("%-11s %5d %5.0f%% %5.0f%% %6d %5.0f%% %6.1fs %6.1fs %8d\n", cat.c_str(), sets,
             100.0 * exact / sets, 100.0 * within1 / sets, maxErr, 100.0 * label / sets,
             endN ? endSum / endN : 0.0, endMax, falseSets);
    else
      printf("%-11s %5s %6s %6s %6s %6s %7s %7s %8d   (tentative reps %d)\n", cat.c_str(), "-",
             "", "", "", "", "", "", falseSets, tent);
  }
  return 0;
}
