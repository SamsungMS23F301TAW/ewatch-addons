// Rep Counter: write a synthetic scenario as a recording (same CSV format as
// REC on), with its expectations. Used to make the replay fixtures in
// test/data/ and to try the replay tool without a watch.
//
//   build/host/rep_synth_csv <category> <seed> out.csv
//   categories: see test/support/rep_scenarios.h (curl, press, raise, row,
//   walk, run, fidget, gestures, session, ...)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "rep_csv.h"
#include "rep_scenarios.h"

int main(int argc, char **argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: rep_synth_csv <category> <seed> out.csv\n");
    return 2;
  }
  scen::Built b = scen::build(argv[1], (uint32_t)atoi(argv[2]));
  std::vector<int> reps;
  std::vector<reps::Exercise> ex;
  for (auto &t : b.truth) { reps.push_back(t.reps); ex.push_back(t.ex); }
  std::map<std::string, std::string> meta;
  meta["source"] = std::string("synthetic:") + argv[1] + ":" + argv[2];
  meta["wrist"] = b.cfg.wrist == reps::Wrist::Left ? "L" : "R";
  meta["mode"] = "Auto";
  FILE *f = std::fopen(argv[3], "w");
  if (!f) { std::fprintf(stderr, "cannot write %s\n", argv[3]); return 1; }
  csv::write(f, b.xyz, 100.f, meta, reps, ex);
  std::fclose(f);
  std::printf("%s: %s, %.1f s, %zu set(s)\n", argv[3], b.desc.c_str(), b.xyz.size() / 300.0, reps.size());
  return 0;
}
