// Rep Counter: a library of randomised synthetic scenarios for tests and the
// host bench. Each category draws its parameters (tempo, range of motion,
// grip, pauses, sticking points, wrist, sensor errors...) from realistic
// ranges using the seed, so a sweep over seeds covers a lot of variety.
// Host only.
#pragma once
#include <cstring>
#include <string>
#include <vector>
#include "rep_synth.h"

namespace scen {

using namespace synth;

struct Built {
  std::vector<int16_t> xyz;
  std::vector<SetTruth> truth;
  reps::Config cfg;
  std::string desc;
};

struct Rng {
  std::mt19937 g;
  explicit Rng(uint32_t s) : g(s * 2654435761u + 12345u) {}
  double uni(double a, double b) { return std::uniform_real_distribution<double>(a, b)(g); }
  int irange(int a, int b) { return std::uniform_int_distribution<int>(a, b)(g); }
  bool coin(double p = 0.5) { return uni(0, 1) < p; }
};

inline const char *const kCategories[] = {
  "curl", "curl_fast", "curl_slow", "hammer", "press", "press_slow", "bench",
  "raise", "row", "walk", "run", "fidget", "gestures", "session",
};
inline const int kNumCategories = (int)(sizeof(kCategories) / sizeof(kCategories[0]));

inline bool isNegative(const std::string &c) {
  return c == "walk" || c == "run" || c == "fidget" || c == "gestures";
}

inline RepPlan plan(Rng &r, int reps, double conc0, double conc1, double ecc0, double ecc1,
                    double top0, double top1, double bot0, double bot1) {
  RepPlan p;
  p.reps = reps;
  p.conc = r.uni(conc0, conc1);
  p.ecc = r.uni(ecc0, ecc1);
  p.top = r.uni(top0, top1);
  p.bottom = r.uni(bot0, bot1);
  p.tempoJitter = r.uni(0.05, 0.2);
  p.ampJitter = r.uni(0.03, 0.12);
  p.fatigue = r.uni(0.0, 0.35);
  p.lead = r.uni(0.5, 2.0);
  p.tail = r.uni(1.0, 2.5);
  p.seed = (uint32_t)r.irange(1, 1 << 30);
  return p;
}

inline SensorParams sensor(Rng &r) {
  SensorParams sp;
  sp.offsetG = r.uni(0.0, 0.04);
  sp.scaleErr = r.uni(0.0, 0.025);
  sp.noiseG = r.uni(0.001, 0.004);
  sp.tremorDeg = r.uni(0.1, 0.8);
  sp.wobbleDeg = r.uni(0.3, 2.0);
  return sp;
}

inline void addCurl(Scenario &sc, Rng &r, const Mount &mt, int kind /*0 normal 1 fast 2 slow 3 hammer*/) {
  CurlParams cp;
  int reps = r.irange(5, 15);
  if (kind == 1) cp.plan = plan(r, reps, 0.5, 0.75, 0.6, 0.9, 0.0, 0.15, 0.0, 0.15);
  else if (kind == 2) cp.plan = plan(r, r.irange(4, 8), 1.8, 3.0, 2.2, 3.5, 0.3, 1.2, 0.3, 1.2);
  else cp.plan = plan(r, reps, 0.8, 1.5, 1.0, 2.4, 0.0, 0.6, 0.0, 0.7);
  cp.bottomDeg = r.uni(3, 25);
  cp.topDeg = r.uni(120, 150);
  cp.gripDeg = kind == 3 ? r.uni(75, 100) : (r.coin(0.8) ? r.uni(-10, 15) : r.uni(160, 185));
  cp.cheat = r.coin(0.3) ? r.uni(0.0, 0.05) : 0.0;
  cp.elbowDrift = r.uni(0.0, 0.06);
  sc.add(curlSet(cp, mt));
}

inline void addPress(Scenario &sc, Rng &r, const Mount &mt, bool slow) {
  PressParams pp;
  if (slow) pp.plan = plan(r, r.irange(4, 8), 1.4, 2.4, 1.8, 3.0, 0.2, 1.5, 0.2, 1.2);
  else pp.plan = plan(r, r.irange(5, 12), 0.7, 1.4, 0.9, 2.0, 0.0, 0.8, 0.0, 0.8);
  pp.plan.stick = r.coin(slow ? 0.7 : 0.3) ? r.uni(0.2, 0.75) : 0.0;
  pp.plan.lead = r.uni(0.2, 1.5);
  pp.travel = r.uni(0.3, 0.5);
  pp.tiltDeg = r.uni(5, 20);
  pp.fwdLeanDeg = r.uni(0, 10);
  sc.add(clean(r.uni(0.8, 1.3), mt, true));
  sc.add(pressSet(pp, mt));
  sc.add(clean(r.uni(0.8, 1.3), mt, false));
}

inline Built build(const std::string &cat, uint32_t seed) {
  Rng r(seed);
  Mount mt;
  mt.wrist = r.coin(0.3) ? reps::Wrist::Right : reps::Wrist::Left;
  mt.handAxisSignLeft = +1;
  Built b;
  b.cfg.wrist = mt.wrist;
  b.cfg.handAxisSignLeft = mt.handAxisSignLeft;
  b.desc = cat + " #" + std::to_string(seed) + (mt.wrist == reps::Wrist::Right ? " R" : " L");
  Scenario sc(mt, sensor(r), seed);
  sc.add(rest(r.uni(2, 5), mt, seed));
  if (cat == "curl") addCurl(sc, r, mt, 0);
  else if (cat == "curl_fast") addCurl(sc, r, mt, 1);
  else if (cat == "curl_slow") addCurl(sc, r, mt, 2);
  else if (cat == "hammer") addCurl(sc, r, mt, 3);
  else if (cat == "press") addPress(sc, r, mt, false);
  else if (cat == "press_slow") addPress(sc, r, mt, true);
  else if (cat == "bench") {
    PressParams pp;
    pp.plan = plan(r, r.irange(5, 12), 0.7, 1.5, 0.9, 2.0, 0.0, 0.8, 0.0, 0.6);
    pp.plan.stick = r.coin(0.3) ? r.uni(0.2, 0.6) : 0.0;
    pp.travel = r.uni(0.28, 0.45);
    pp.tiltDeg = r.uni(3, 15);
    sc.add(benchSet(pp, mt), r.uni(2.0, 3.0));
    sc.add(rest(r.uni(4, 8), mt, seed + 1), r.uni(1.5, 2.5));
  } else if (cat == "raise") {
    RaiseParams rp;
    rp.plan = plan(r, r.irange(6, 15), 0.8, 1.4, 1.0, 2.2, 0.0, 0.5, 0.0, 0.5);
    rp.front = r.coin(0.3);
    rp.bottomDeg = r.uni(3, 15);
    rp.topDeg = r.uni(75, 100);
    sc.add(raiseSet(rp, mt));
  } else if (cat == "row") {
    RowParams rp;
    rp.plan = plan(r, r.irange(6, 14), 0.7, 1.3, 1.0, 2.0, 0.1, 0.8, 0.0, 0.6);
    rp.travel = r.uni(0.2, 0.35);
    rp.tiltDeg = r.uni(5, 25);
    sc.add(rowSet(rp, mt), 1.5);
    sc.add(rest(r.uni(4, 8), mt, seed + 1), 1.5);
  } else if (cat == "walk") {
    WalkParams wp;
    wp.T = r.uni(60, 120);
    wp.cadence = r.uni(1.5, 2.1);
    wp.swingDeg = r.uni(8, 32);
    wp.impactG = r.uni(0.25, 1.0);
    wp.bounce = r.uni(0.015, 0.035);
    wp.elbowDeg = r.uni(5, 40);
    wp.seed = seed;
    sc.add(walk(wp, mt));
  } else if (cat == "run") {
    WalkParams wp = runParams(r.uni(45, 90), seed);
    wp.cadence = r.uni(2.4, 3.1);
    wp.swingDeg = r.uni(20, 45);
    wp.impactG = r.uni(1.2, 3.0);
    wp.elbowDeg = r.uni(70, 100);
    sc.add(walk(wp, mt, "run"));
  } else if (cat == "fidget") {
    FidgetParams fp;
    fp.T = r.uni(90, 150);
    fp.rotDeg = r.uni(15, 50);
    fp.posM = r.uni(0.03, 0.12);
    fp.flicksPerMin = r.uni(2, 12);
    fp.tapsPerMin = r.uni(0, 10);
    fp.armUp = r.coin();
    fp.seed = seed;
    sc.add(fidget(fp, mt));
  } else if (cat == "gestures") {
    for (int i = 0; i < 6; i++) {
      int k = r.irange(0, 3);
      if (k == 0) sc.add(glance(r.uni(0.8, 4.0), mt));
      else if (k == 1) sc.add(drink(r.uni(1.5, 4.0), mt));
      else if (k == 2) sc.add(pickUp(mt));
      else { WalkParams wp; wp.T = r.uni(5, 15); wp.seed = seed + i; sc.add(walk(wp, mt)); }
      sc.add(rest(r.uni(1.0, 8.0), mt, seed + 10 + i));
    }
  } else if (cat == "session") {
    // A realistic stretch of a workout: sets separated by rests and walking.
    for (int i = 0; i < 3; i++) {
      int k = r.irange(0, 3);
      if (k == 0) addCurl(sc, r, mt, 0);
      else if (k == 1) addPress(sc, r, mt, false);
      else if (k == 2) {
        RaiseParams rp;
        rp.plan = plan(r, r.irange(6, 12), 0.8, 1.4, 1.0, 2.0, 0.0, 0.4, 0.0, 0.4);
        rp.topDeg = r.uni(78, 98);
        sc.add(raiseSet(rp, mt));
      } else addCurl(sc, r, mt, 3);
      sc.add(rest(r.uni(5, 15), mt, seed + 20 + i));
      WalkParams wp; wp.T = r.uni(10, 25); wp.seed = seed + 30 + i;
      wp.impactG = r.uni(0.3, 0.9);
      sc.add(walk(wp, mt));
      sc.add(rest(r.uni(5, 20), mt, seed + 40 + i));
    }
  }
  sc.add(rest(r.uni(6, 10), mt, seed + 99));
  b.xyz = sc.render();
  b.truth = sc.truths();
  return b;
}

}  // namespace scen
