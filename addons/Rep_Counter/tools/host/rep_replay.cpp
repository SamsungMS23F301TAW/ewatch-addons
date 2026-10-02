// Rep Counter: replay recordings through the host-built detector.
//
//   build/host/rep_replay [options] rec1.csv [rec2.csv ...]
//
//   --mode auto|curl|press|raise|row   override the recorded exercise mode
//   --wrist L|R                        override the recorded wrist
//   --set name=value                   change a tuning value (see --list)
//   --list                             print every tuning value and exit
//   --events                           print every detector event
//   --cands                            print every candidate rep + verdict
//   --trace out.csv                    dump per-sample internal signals
//
// Recordings come from `REC on` (tools/record.py). If a file has an
// "# expect reps=..." line the replay is scored against it, and the exit
// status is non-zero when any file is off by more than its tolerance. That is
// what makes a real recording a regression test: drop it into test/data/.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "rep_csv.h"
#include "rep_harness.h"

static bool gCands = false;

static void candHook(const reps::Candidate &c, bool acc, const char *why, void *) {
  if (!gCands) return;
  std::printf("  %-3s %s t=%7.2f-%7.2fs amp=%7.2f%s dur=%.2fs elev=%+.2f->%+.2f up=%+.2f "
              "path=%.2f hold=%.2f rot=%.0f hand=%+.2f  %s\n",
              acc ? "ok" : "--", c.ch == reps::Channel::Rotation ? "ROT" : "LIN",
              c.nStart / 100.0, c.nEnd / 100.0, c.amp,
              c.ch == reps::Channel::Rotation ? "deg" : "m  ", (c.nEnd - c.nStart) / 100.0,
              c.elevHome, c.elevPeak, c.upAtPeak, c.path, c.holdFrac, c.rotDeg, c.handG, why);
}

static const char *evName(reps::EventType t) {
  switch (t) {
    case reps::EventType::RepTentative: return "rep?";
    case reps::EventType::SetStart: return "set+";
    case reps::EventType::Rep: return "rep";
    case reps::EventType::SetEnd: return "end";
    case reps::EventType::SetDiscarded: return "drop";
    default: return "?";
  }
}

int main(int argc, char **argv) {
  std::vector<std::string> files;
  reps::Tuning tune;
  bool events = false, haveMode = false, haveWrist = false;
  reps::Mode mode = reps::Mode::Auto;
  reps::Wrist wrist = reps::Wrist::Left;
  std::string tracePath;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--events") events = true;
    else if (a == "--cands") gCands = true;
    else if (a == "--list") {
      for (int k = 0; reps::tuningName(k); k++)
        std::printf("%-15s %g\n", reps::tuningName(k), *reps::tuningField(tune, reps::tuningName(k)));
      return 0;
    } else if (a == "--mode" && i + 1 < argc) {
      if (!csv::parseMode(argv[++i], mode)) { std::fprintf(stderr, "bad mode\n"); return 2; }
      haveMode = true;
    } else if (a == "--wrist" && i + 1 < argc) {
      wrist = (argv[++i][0] == 'R' || argv[i][0] == 'r') ? reps::Wrist::Right : reps::Wrist::Left;
      haveWrist = true;
    } else if (a == "--trace" && i + 1 < argc) {
      tracePath = argv[++i];
    } else if (a == "--set" && i + 1 < argc) {
      std::string kv = argv[++i];
      size_t eq = kv.find('=');
      float *f = eq == std::string::npos ? nullptr : reps::tuningField(tune, kv.substr(0, eq).c_str());
      if (!f) { std::fprintf(stderr, "unknown tuning '%s' (try --list)\n", kv.c_str()); return 2; }
      *f = (float)atof(kv.c_str() + eq + 1);
    } else if (!a.empty() && a[0] == '-') {
      std::fprintf(stderr, "unknown option %s\n", a.c_str());
      return 2;
    } else {
      files.push_back(a);
    }
  }
  if (files.empty()) {
    std::fprintf(stderr, "usage: rep_replay [--mode m] [--wrist L|R] [--set k=v] [--events] "
                         "[--cands] [--trace out.csv] [--list] file.csv...\n");
    return 2;
  }
  int bad = 0;
  for (auto &path : files) {
    csv::Recording rec;
    if (!csv::load(path, rec)) { std::printf("%s: %s\n", path.c_str(), rec.error.c_str()); bad++; continue; }
    reps::Config cfg = csv::configFor(rec);
    cfg.tune = tune;
    if (haveMode) cfg.mode = mode;
    if (haveWrist) cfg.wrist = wrist;
    size_t n = rec.xyz.size() / 3;
    std::printf("%s: %.1f s at %g Hz, %zu gap(s), mode %s, wrist %s\n", path.c_str(), n / rec.fs,
                rec.fs, rec.gapsBefore.size(), reps::modeName(cfg.mode),
                cfg.wrist == reps::Wrist::Left ? "L" : "R");
    reps::Detector d;
    d.setCandidateHook(candHook, nullptr);
    harness::RunResult res;
    if (!tracePath.empty()) {
      // Single pass with a per-sample trace.
      FILE *tf = std::fopen(tracePath.c_str(), "w");
      if (!tf) { std::fprintf(stderr, "cannot write %s\n", tracePath.c_str()); return 2; }
      std::fprintf(tf, "t,ax,ay,az,theta,rotPh,linExc,linPh,vel,accV,rate,hf,gated,frozen,progress,reps,setState\n");
      d.reset(cfg);
      size_t gi = 0;
      for (size_t i = 0; i < n; i++) {
        if (gi < rec.gapsBefore.size() && rec.gapsBefore[gi] == i) { d.markGap(); gi++; }
        d.push(rec.xyz[3 * i], rec.xyz[3 * i + 1], rec.xyz[3 * i + 2]);
        reps::Event e;
        while (d.pollEvent(e)) res.events.push_back(e);
        const reps::Debug &g = d.debug();
        const reps::Live &l = d.live();
        std::fprintf(tf, "%.3f,%.4f,%.4f,%.4f,%.1f,%d,%.3f,%d,%.3f,%.4f,%.1f,%.3f,%d,%d,%.2f,%d,%d\n",
                     i / rec.fs, rec.xyz[3 * i] / rec.countsPerG, rec.xyz[3 * i + 1] / rec.countsPerG,
                     rec.xyz[3 * i + 2] / rec.countsPerG, g.theta, g.rotPh, g.linExc, g.linPh, g.vel,
                     g.accV, g.rate, g.hfRms, g.gated, g.frozen, l.progress, l.reps, l.setState);
      }
      std::fclose(tf);
      std::printf("  trace written to %s\n", tracePath.c_str());
    }
    res = harness::run(rec.xyz, cfg, &d, &rec.gapsBefore);
    if (events)
      for (auto &e : res.events)
        std::printf("  %8.2fs %-4s reps=%-3d %-5s conf=%d tempo=%.1fs\n", e.tMs / 1000.0,
                    evName(e.type), e.reps, reps::exerciseName(e.exercise), e.confidence, e.tempoSec);
    std::printf("  sets:");
    if (res.sets.empty()) std::printf(" none");
    for (auto &st : res.sets)
      std::printf(" %d %s (%.0f-%.0fs)", st.reps, reps::exerciseName(st.ex), st.tStart, st.tLastRep);
    std::printf("\n  tentative reps %d, discarded %d\n", res.tentatives, res.discarded);
    if (rec.hasExpect) {
      bool ok = res.sets.size() == rec.expectReps.size();
      for (size_t i = 0; ok && i < res.sets.size(); i++) {
        if (std::abs(res.sets[i].reps - rec.expectReps[i]) > rec.expectTol) ok = false;
        if (i < rec.expectEx.size() && rec.expectEx[i] != reps::Exercise::None &&
            rec.expectEx[i] != res.sets[i].ex)
          std::printf("  note: set %zu labelled %s, expected %s\n", i + 1,
                      reps::exerciseName(res.sets[i].ex), reps::exerciseName(rec.expectEx[i]));
      }
      std::printf("  expected:");
      if (rec.expectReps.empty()) std::printf(" no sets");
      for (int r : rec.expectReps) std::printf(" %d", r);
      std::printf(" (tol %d) -> %s\n", rec.expectTol, ok ? "PASS" : "FAIL");
      if (!ok) bad++;
    }
  }
  return bad ? 1 : 0;
}
