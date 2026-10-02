// Rep Counter: run the detector over a sample stream and score it against
// ground truth. Shared by the Unity tests and the host replay tool.
// Host only (uses the STL).
#pragma once
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "rep_detector.h"
#include "rep_resample.h"
#include "rep_synth.h"

namespace harness {

struct DetectedSet {
  reps::Exercise ex = reps::Exercise::None;
  int reps = 0;
  double tStart = 0;      // first rep began (s)
  double tLastRep = 0;    // last rep counted (s)
  double tEnd = 0;        // SetEnd event (s)
  int confidence = 0;
  double tempo = 0;
};

struct RunResult {
  std::vector<DetectedSet> sets;
  std::vector<reps::Event> events;
  int tentatives = 0;     // RepTentative events
  int discarded = 0;      // tentative reps that expired
};

// Feed interleaved xyz counts (100 Hz) through a fresh detector.
inline RunResult run(const std::vector<int16_t> &xyz, const reps::Config &cfg,
                     reps::Detector *keep = nullptr,
                     const std::vector<size_t> *gapsBefore = nullptr) {
  reps::Detector local;
  reps::Detector &d = keep ? *keep : local;
  d.reset(cfg);
  RunResult r;
  DetectedSet cur;
  bool open = false;
  size_t n = xyz.size() / 3;
  auto drain = [&]() {
    reps::Event e;
    while (d.pollEvent(e)) {
      r.events.push_back(e);
      double t = e.tMs / 1000.0;
      switch (e.type) {
        case reps::EventType::RepTentative: r.tentatives++; break;
        case reps::EventType::SetStart:
          cur = DetectedSet();
          cur.tStart = e.setStartMs / 1000.0;
          cur.reps = e.reps;
          cur.ex = e.exercise;
          cur.tLastRep = t;
          open = true;
          break;
        case reps::EventType::Rep:
          cur.reps = e.reps;
          cur.ex = e.exercise;
          cur.tLastRep = t;
          cur.confidence = e.confidence;
          cur.tempo = e.tempoSec;
          break;
        case reps::EventType::SetEnd:
          if (open) {
            cur.reps = e.reps;
            cur.ex = e.exercise;
            cur.tEnd = t;
            cur.confidence = e.confidence;
            cur.tempo = e.tempoSec;
            r.sets.push_back(cur);
          }
          open = false;
          break;
        case reps::EventType::SetDiscarded: r.discarded++; break;
        default: break;
      }
    }
  };
  size_t gi = 0;
  for (size_t i = 0; i < n; i++) {
    if (gapsBefore && gi < gapsBefore->size() && (*gapsBefore)[gi] == i) { d.markGap(); gi++; }
    d.push(xyz[3 * i], xyz[3 * i + 1], xyz[3 * i + 2]);
    drain();
  }
  // Flush: give a trailing set time to end.
  int flush = (int)(15.f * cfg.fs);
  for (int i = 0; i < flush && n > 0; i++) {
    d.push(xyz[3 * (n - 1)], xyz[3 * (n - 1) + 1], xyz[3 * (n - 1) + 2]);
    drain();
  }
  if (open) { cur.tEnd = d.nowMs() / 1000.0; r.sets.push_back(cur); }
  return r;
}

// Simulate the REPS_IMU_FIFO=0 fallback: taskIO polls the (BaseOS, +/-2 g)
// accelerometer about every 22 ms with jitter and the odd hiccup; the view
// resamples to 50 Hz. Input is the 100 Hz synthetic stream (2048 counts/g).
inline std::vector<int16_t> polledTo50Hz(const std::vector<int16_t> &xyz, uint32_t seed) {
  std::vector<int16_t> out;
  size_t n = xyz.size() / 3;
  if (n < 2) return out;
  uint32_t lcg = seed * 2654435761u + 1;
  auto rnd = [&]() { lcg = lcg * 1664525u + 1013904223u; return (lcg >> 8) / 16777216.0; };
  reps::Resampler rs(50.f, 250);
  double t = 0;
  while (true) {
    double idx = t / 10.0;                       // 100 Hz source
    size_t i0 = (size_t)idx;
    if (i0 + 1 >= n) break;
    double a = idx - i0;
    float g[3];
    for (int k = 0; k < 3; k++) {
      double v = (xyz[3 * i0 + k] * (1 - a) + xyz[3 * (i0 + 1) + k] * a) / 2048.0;
      if (v > 2.0) v = 2.0;                      // BaseOS range
      if (v < -2.0) v = -2.0;
      g[k] = (float)v;
    }
    rs.push((uint32_t)t, g, false, [&](float x, float y, float z, bool) {
      out.push_back((int16_t)lround(x * 2048.0));
      out.push_back((int16_t)lround(y * 2048.0));
      out.push_back((int16_t)lround(z * 2048.0));
    });
    double step = 20.0 + 2.0 + (rnd() - 0.5) * 6.0;   // work + vTaskDelay(20)
    if (rnd() < 0.01) step += 20.0 + rnd() * 30.0;    // battery read / flash write
    t += step;
  }
  return out;
}

struct Match {
  const synth::SetTruth *truth = nullptr;
  const DetectedSet *det = nullptr;   // null = missed
};

struct Score {
  std::vector<Match> matches;
  std::vector<const DetectedSet *> extra;   // detected sets matching no truth
  int countErrMax = 0;                       // worst |detected - true| reps
  int exact = 0;                             // sets counted exactly
  int labelOk = 0;
};

// Pair each true set with the detected set that overlaps it most.
inline Score score(const std::vector<synth::SetTruth> &truth, const RunResult &res) {
  Score s;
  std::vector<bool> used(res.sets.size(), false);
  for (auto &t : truth) {
    Match m;
    m.truth = &t;
    double best = 0;
    int bi = -1;
    for (size_t i = 0; i < res.sets.size(); i++) {
      if (used[i]) continue;
      const DetectedSet &d = res.sets[i];
      double a = std::max(t.tFirstStart - 2.0, d.tStart);
      double b = std::min(t.tLastEnd + 2.0, d.tLastRep);
      double ov = b - a;
      if (ov > best) { best = ov; bi = (int)i; }
    }
    if (bi >= 0) { used[bi] = true; m.det = &res.sets[bi]; }
    int got = m.det ? m.det->reps : 0;
    int err = std::abs(got - t.reps);
    if (err > s.countErrMax) s.countErrMax = err;
    if (err == 0) s.exact++;
    if (m.det && m.det->ex == t.ex) s.labelOk++;
    s.matches.push_back(m);
  }
  for (size_t i = 0; i < res.sets.size(); i++)
    if (!used[i]) s.extra.push_back(&res.sets[i]);
  return s;
}

inline void print(const char *title, const std::vector<synth::SetTruth> &truth,
                  const RunResult &res, const Score &s, FILE *f = stdout) {
  std::fprintf(f, "== %s: %zu true sets, %zu detected, tentative %d, discarded %d\n",
               title, truth.size(), res.sets.size(), res.tentatives, res.discarded);
  for (auto &m : s.matches) {
    std::fprintf(f, "   %-14s true %2d %-5s | ", m.truth->label.c_str(), m.truth->reps,
                 reps::exerciseName(m.truth->ex));
    if (m.det)
      std::fprintf(f, "got %2d %-5s conf %d tempo %.1fs  end +%.1fs after last rep\n",
                   m.det->reps, reps::exerciseName(m.det->ex), m.det->confidence,
                   m.det->tempo, m.det->tEnd - m.truth->tLastEnd);
    else
      std::fprintf(f, "MISSED\n");
  }
  for (auto *d : s.extra)
    std::fprintf(f, "   EXTRA set: %d %s at %.1f-%.1fs\n", d->reps,
                 reps::exerciseName(d->ex), d->tStart, d->tLastRep);
}

}  // namespace harness
