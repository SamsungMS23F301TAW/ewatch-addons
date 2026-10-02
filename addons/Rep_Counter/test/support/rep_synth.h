// Rep Counter: synthetic accelerometer generator for host tests.
//
// A small kinematic model of a forearm wearing the watch. Each activity
// (curl set, press set, walking, fidgeting, ...) is a Segment that returns the
// watch's world position and orientation over time. The scenario renderer
// differentiates position twice (1 kHz), adds gravity, rotates into the watch
// frame, then applies a sensor model: per-axis offset and scale error, white
// noise, a 10-sample average (the MMA8451's oversampling) down to 100 Hz,
// 14-bit quantisation at +/-4 g (2048 counts/g) and clipping.
//
// World frame: x forward, y to the person's left, z up. The watch frame is
// built from the forearm direction (elbow -> hand) and the back-of-wrist
// normal: Z = out of the watch face, X = along the forearm (sign from the
// wrist and the mounting constant), Y = Z x X.
//
// Header-only, deterministic for a given seed. Host only (uses the STL).
#pragma once
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>
#include "rep_types.h"

namespace synth {

static const double kPi = 3.14159265358979323846;
static const double kGrav = 9.80665;
inline double rad(double d) { return d * kPi / 180.0; }

// ---------------------------------------------------------------------------
// Vector / rotation math
// ---------------------------------------------------------------------------
struct V {
  double x = 0, y = 0, z = 0;
  V() = default;
  V(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
  V operator+(const V &o) const { return V(x + o.x, y + o.y, z + o.z); }
  V operator-(const V &o) const { return V(x - o.x, y - o.y, z - o.z); }
  V operator*(double k) const { return V(x * k, y * k, z * k); }
  V operator-() const { return V(-x, -y, -z); }
};
inline double dot(const V &a, const V &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V cross(const V &a, const V &b) {
  return V(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
inline double len(const V &a) { return std::sqrt(dot(a, a)); }
inline V unit(const V &a) { double l = len(a); return l > 1e-12 ? a * (1.0 / l) : V(0, 0, 1); }

// Rotate v about unit axis k by angle a (Rodrigues).
inline V rotAbout(const V &v, const V &k, double a) {
  double c = std::cos(a), s = std::sin(a);
  return v * c + cross(k, v) * s + k * (dot(k, v) * (1 - c));
}

// Columns are the watch axes expressed in world coordinates.
struct M {
  V c[3];
  V toWorld(const V &d) const { return c[0] * d.x + c[1] * d.y + c[2] * d.z; }
  V toBody(const V &w) const { return V(dot(c[0], w), dot(c[1], w), dot(c[2], w)); }
};

struct Q {
  double w = 1, x = 0, y = 0, z = 0;
};

inline Q quatFromM(const M &m) {
  // m(r, c) = component r of column c
  double m00 = m.c[0].x, m01 = m.c[1].x, m02 = m.c[2].x;
  double m10 = m.c[0].y, m11 = m.c[1].y, m12 = m.c[2].y;
  double m20 = m.c[0].z, m21 = m.c[1].z, m22 = m.c[2].z;
  Q q;
  double tr = m00 + m11 + m22;
  if (tr > 0) {
    double s = std::sqrt(tr + 1.0) * 2;
    q.w = 0.25 * s; q.x = (m21 - m12) / s; q.y = (m02 - m20) / s; q.z = (m10 - m01) / s;
  } else if (m00 > m11 && m00 > m22) {
    double s = std::sqrt(1.0 + m00 - m11 - m22) * 2;
    q.w = (m21 - m12) / s; q.x = 0.25 * s; q.y = (m01 + m10) / s; q.z = (m02 + m20) / s;
  } else if (m11 > m22) {
    double s = std::sqrt(1.0 + m11 - m00 - m22) * 2;
    q.w = (m02 - m20) / s; q.x = (m01 + m10) / s; q.y = 0.25 * s; q.z = (m12 + m21) / s;
  } else {
    double s = std::sqrt(1.0 + m22 - m00 - m11) * 2;
    q.w = (m10 - m01) / s; q.x = (m02 + m20) / s; q.y = (m12 + m21) / s; q.z = 0.25 * s;
  }
  return q;
}

inline M mFromQuat(const Q &q) {
  M m;
  double w = q.w, x = q.x, y = q.y, z = q.z;
  m.c[0] = V(1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y));
  m.c[1] = V(2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x));
  m.c[2] = V(2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y));
  return m;
}

inline Q slerp(Q a, Q b, double t) {
  double d = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
  if (d < 0) { b.w = -b.w; b.x = -b.x; b.y = -b.y; b.z = -b.z; d = -d; }
  if (d > 0.9995) {
    Q r{a.w + t * (b.w - a.w), a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.z + t * (b.z - a.z)};
    double n = std::sqrt(r.w * r.w + r.x * r.x + r.y * r.y + r.z * r.z);
    r.w /= n; r.x /= n; r.y /= n; r.z /= n;
    return r;
  }
  double th = std::acos(d), s = std::sin(th);
  double ka = std::sin((1 - t) * th) / s, kb = std::sin(t * th) / s;
  return Q{ka * a.w + kb * b.w, ka * a.x + kb * b.x, ka * a.y + kb * b.y, ka * a.z + kb * b.z};
}

inline double angleBetween(const M &a, const M &b) {
  Q qa = quatFromM(a), qb = quatFromM(b);
  double d = std::fabs(qa.w * qb.w + qa.x * qb.x + qa.y * qb.y + qa.z * qb.z);
  if (d > 1) d = 1;
  return 2 * std::acos(d);
}

// Minimum-jerk 0 -> 1 profile.
inline double mj(double s) {
  if (s <= 0) return 0;
  if (s >= 1) return 1;
  return s * s * s * (10 - 15 * s + 6 * s * s);
}

// ---------------------------------------------------------------------------
// Watch mounting
// ---------------------------------------------------------------------------
struct Mount {
  reps::Wrist wrist = reps::Wrist::Left;
  int handAxisSignLeft = +1;      // must match the detector Config
  double side() const { return wrist == reps::Wrist::Left ? 1.0 : -1.0; }
};

// Watch frame from forearm direction (elbow -> hand) and back-of-wrist normal.
inline M watchFrame(const V &fwdIn, const V &dorsalIn, const Mount &mt) {
  V fwd = unit(fwdIn);
  V dor = unit(dorsalIn - fwd * dot(dorsalIn, fwd));
  double s = (mt.wrist == reps::Wrist::Left ? 1.0 : -1.0) * mt.handAxisSignLeft;
  M m;
  m.c[0] = fwd * s;
  m.c[2] = dor;
  m.c[1] = cross(m.c[2], m.c[0]);
  return m;
}

struct Pose {
  V p;
  M R;
};

// ---------------------------------------------------------------------------
// Segments
// ---------------------------------------------------------------------------
struct SetTruth {
  reps::Exercise ex = reps::Exercise::None;
  int reps = 0;
  double tFirstStart = 0;   // absolute seconds (filled by Scenario)
  double tLastEnd = 0;      // last rep back at the start position
  std::vector<double> repDone;   // per rep: when it returned to the start
  std::string label;
};

struct Segment {
  std::string name;
  double T = 1.0;
  std::function<Pose(double)> pose;
  std::function<V(double)> extra;   // extra world acceleration (impacts), m/s^2
  bool isSet = false;
  SetTruth truth;                   // times local to the segment
};

// ---------------------------------------------------------------------------
// Rep timing
// ---------------------------------------------------------------------------
struct RepPlan {
  int reps = 10;
  double conc = 1.0, top = 0.25, ecc = 1.4, bottom = 0.35;   // seconds
  double tempoJitter = 0.15;   // relative, per phase
  double ampJitter = 0.08;     // relative, per rep
  double fatigue = 0.0;        // last rep this much slower (relative)
  double lead = 1.0, tail = 1.5;
  double stick = 0.0;          // sticking-point strength 0..0.9 (concentric)
  bool startAtTop = false;     // value starts at 1 (e.g. bench press)
  uint32_t seed = 1;
};

struct RepTrack {
  struct Rep { double t0, t1, t2, t3, t4, a; };   // start, top, top end, down, rest end
  std::vector<Rep> r;
  double T = 0, stick = 0;
  bool startAtTop = false;

  void build(const RepPlan &p) {
    std::mt19937 rng(p.seed);
    std::uniform_real_distribution<double> u(-1, 1);
    stick = p.stick;
    startAtTop = p.startAtTop;
    double t = p.lead;
    r.clear();
    for (int i = 0; i < p.reps; i++) {
      double slow = 1.0 + p.fatigue * (p.reps > 1 ? (double)i / (p.reps - 1) : 0);
      auto jit = [&](double v) { return std::max(0.0, v * slow * (1 + p.tempoJitter * u(rng))); };
      Rep x;
      x.a = 1 + p.ampJitter * u(rng);
      double first = startAtTop ? p.ecc : p.conc, second = startAtTop ? p.conc : p.ecc;
      double hold1 = startAtTop ? p.bottom : p.top, hold2 = startAtTop ? p.top : p.bottom;
      x.t0 = t;
      x.t1 = x.t0 + std::max(0.25, jit(first));
      x.t2 = x.t1 + jit(hold1);
      x.t3 = x.t2 + std::max(0.3, jit(second));
      x.t4 = x.t3 + (i + 1 < p.reps ? jit(hold2) : 0.0);
      t = x.t4;
      r.push_back(x);
    }
    T = t + p.tail;
  }

  // Normalised position: 0 = start of rep (bottom), a = far end.
  double value(double t) const {
    double base = startAtTop ? 1.0 : 0.0;
    if (r.empty() || t <= r.front().t0) return base;
    for (size_t i = 0; i < r.size(); i++) {
      const Rep &x = r[i];
      if (t > x.t4 && i + 1 < r.size()) continue;
      double prevA = startAtTop ? (i ? r[i - 1].a : 1.0) : 0.0;
      if (!startAtTop) {
        if (t < x.t1) return x.a * shape((t - x.t0) / (x.t1 - x.t0), true);
        if (t < x.t2) return x.a;
        if (t < x.t3) return x.a * (1 - mj((t - x.t2) / (x.t3 - x.t2)));
        return 0.0;
      } else {
        // starts at the top: go down first, then press back up
        if (t < x.t1) return prevA * (1 - mj((t - x.t0) / (x.t1 - x.t0)));
        if (t < x.t2) return 0.0;
        if (t < x.t3) return x.a * shape((t - x.t2) / (x.t3 - x.t2), true);
        return x.a;
      }
    }
    return startAtTop ? r.back().a : 0.0;
  }

  // Concentric shape with an optional sticking point (velocity dip mid-way).
  double shape(double s, bool concentric) const {
    double m = mj(s);
    if (!concentric || stick <= 0) return m;
    return m + stick / (2 * kPi) * std::sin(2 * kPi * m);
  }

  // When each rep is back at its start position (for set-end checks).
  std::vector<double> repDone() const {
    std::vector<double> v;
    for (auto &x : r) v.push_back(x.t3);
    return v;
  }
};

// ---------------------------------------------------------------------------
// Activity builders
// ---------------------------------------------------------------------------
struct CurlParams {
  RepPlan plan;
  double bottomDeg = 10, topDeg = 140;
  double gripDeg = 0;        // 0 supinated, 90 hammer, 180 reverse
  double forearm = 0.26;     // elbow -> watch, metres
  double elbowDrift = 0.04;  // elbow moves forward as the curl rises
  double cheat = 0.0;        // body swing, metres
};

inline Segment curlSet(const CurlParams &cp, const Mount &mt, reps::Exercise ex = reps::Exercise::Curl) {
  auto track = std::make_shared<RepTrack>();
  track->build(cp.plan);
  Segment s;
  s.name = "curl";
  s.T = track->T;
  s.isSet = true;
  s.truth.ex = ex;
  s.truth.reps = cp.plan.reps;
  s.truth.repDone = track->repDone();
  double side = mt.side();
  s.pose = [=](double t) {
    double v = track->value(t);
    double phi = rad(cp.bottomDeg + (cp.topDeg - cp.bottomDeg) * v);
    V fwd(std::sin(phi), 0, -std::cos(phi));
    V d0(-std::cos(phi), 0, -std::sin(phi));
    double psi = rad(cp.gripDeg) * side;
    V dor = d0 * std::cos(psi) + cross(fwd, d0) * std::sin(psi);
    double lift = (1 - std::cos(phi)) * 0.5;
    V E(cp.elbowDrift * lift + cp.cheat * std::sin(kPi * v), side * 0.2, 1.15 + 0.01 * lift);
    return Pose{E + fwd * cp.forearm, watchFrame(fwd, dor, mt)};
  };
  return s;
}

struct PressParams {
  RepPlan plan;
  double travel = 0.42;       // metres of vertical hand travel
  double tiltDeg = 14;        // forearm lean at the bottom
  double fwdLeanDeg = 6;
};

inline Segment pressSet(const PressParams &pp, const Mount &mt) {
  auto track = std::make_shared<RepTrack>();
  track->build(pp.plan);
  Segment s;
  s.name = "press";
  s.T = track->T;
  s.isSet = true;
  s.truth.ex = reps::Exercise::Press;
  s.truth.reps = pp.plan.reps;
  s.truth.repDone = track->repDone();
  double side = mt.side();
  s.pose = [=](double t) {
    double v = track->value(t);
    double b = rad(pp.tiltDeg * (1 - 0.6 * v));
    double f = rad(pp.fwdLeanDeg);
    V fwd = unit(V(std::sin(f), -side * std::sin(b), std::cos(b) * std::cos(f)));
    V dor(-1, 0, 0);
    V P(0.05, side * (0.25 - 0.07 * v), 1.5 + pp.travel * v);
    return Pose{P, watchFrame(fwd, dor, mt)};
  };
  return s;
}

// Bench press: lying down, forearm vertical, starts at lockout.
inline Segment benchSet(PressParams pp, const Mount &mt) {
  pp.plan.startAtTop = true;
  auto track = std::make_shared<RepTrack>();
  track->build(pp.plan);
  Segment s;
  s.name = "bench";
  s.T = track->T;
  s.isSet = true;
  s.truth.ex = reps::Exercise::Press;
  s.truth.reps = pp.plan.reps;
  s.truth.repDone = track->repDone();
  double side = mt.side();
  s.pose = [=](double t) {
    double v = track->value(t);
    double b = rad(pp.tiltDeg * (1 - 0.7 * v));
    V fwd = unit(V(0, -side * std::sin(b), std::cos(b)));
    V dor(-1, 0, 0);
    V P(0, side * (0.28 - 0.05 * v), 0.75 + pp.travel * v);
    return Pose{P, watchFrame(fwd, dor, mt)};
  };
  return s;
}

struct RaiseParams {
  RepPlan plan;
  double bottomDeg = 8, topDeg = 88;
  bool front = false;
  double arm = 0.52;
};

inline Segment raiseSet(const RaiseParams &rp, const Mount &mt) {
  auto track = std::make_shared<RepTrack>();
  track->build(rp.plan);
  Segment s;
  s.name = rp.front ? "front raise" : "lateral raise";
  s.T = track->T;
  s.isSet = true;
  s.truth.ex = reps::Exercise::Raise;
  s.truth.reps = rp.plan.reps;
  s.truth.repDone = track->repDone();
  double side = mt.side();
  s.pose = [=](double t) {
    double v = track->value(t);
    double a = rad(rp.bottomDeg + (rp.topDeg - rp.bottomDeg) * v);
    V dir, dor;
    if (rp.front) {
      dir = V(std::sin(a), 0, -std::cos(a));
      dor = V(std::cos(a), 0, std::sin(a));
    } else {
      dir = V(0, side * std::sin(a), -std::cos(a));
      dor = V(0, side * std::cos(a), std::sin(a));
    }
    V fwd = unit(dir + V(0.12, 0, 0));
    V Sh(0, side * 0.2, 1.45);
    return Pose{Sh + dir * rp.arm, watchFrame(fwd, dor, mt)};
  };
  return s;
}

struct RowParams {
  RepPlan plan;
  double travel = 0.28;
  double tiltDeg = 15;
};

inline Segment rowSet(const RowParams &rp, const Mount &mt) {
  auto track = std::make_shared<RepTrack>();
  track->build(rp.plan);
  Segment s;
  s.name = "row";
  s.T = track->T;
  s.isSet = true;
  s.truth.ex = reps::Exercise::Row;
  s.truth.reps = rp.plan.reps;
  s.truth.repDone = track->repDone();
  double side = mt.side();
  s.pose = [=](double t) {
    double v = track->value(t);
    double b = rad(rp.tiltDeg * v);
    V fwd = unit(V(-std::sin(b), 0, -std::cos(b)));
    V dor(0, side, 0);
    V P(-0.1 * v, side * 0.2, 0.6 + rp.travel * v);
    return Pose{P, watchFrame(fwd, dor, mt)};
  };
  return s;
}

// Standing with the arm hanging; slow sway.
inline Segment rest(double T, const Mount &mt, uint32_t seed, double swayDeg = 3) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> ph(0, 2 * kPi), fr(0.05, 0.4);
  double p1 = ph(rng), p2 = ph(rng), p3 = ph(rng), f1 = fr(rng), f2 = fr(rng), f3 = fr(rng);
  Segment s;
  s.name = "rest";
  s.T = T;
  double side = mt.side();
  s.pose = [=](double t) {
    double env = mj(t / 1.0) * mj((T - t) / 1.0);
    double a = rad(swayDeg) * env * std::sin(2 * kPi * f1 * t + p1);
    double b = rad(swayDeg) * env * std::sin(2 * kPi * f2 * t + p2);
    V fwd = unit(V(std::sin(a), std::sin(b), -1));
    V dor(0, side, 0);
    V P(0.01 * env * std::sin(2 * kPi * f3 * t + p3), side * 0.2, 0.9);
    return Pose{P, watchFrame(fwd, dor, mt)};
  };
  return s;
}

// Smooth posture change (e.g. hanging -> holding dumbbells at the shoulders,
// lying down on a bench). Orientation slerps, position follows min-jerk.
inline Segment transition(double T, std::function<Pose()> from, std::function<Pose()> to,
                          const std::string &name = "transition") {
  Segment s;
  s.name = name;
  s.T = T;
  s.pose = [=](double t) {
    Pose a = from(), b = to();
    double k = mj(t / T);
    Q q = slerp(quatFromM(a.R), quatFromM(b.R), k);
    return Pose{a.p + (b.p - a.p) * k, mFromQuat(q)};
  };
  return s;
}

// Curl-like clean from the hang to the shoulders (or the reverse).
inline Segment clean(double T, const Mount &mt, bool up = true) {
  Segment s;
  s.name = up ? "clean" : "lower";
  s.T = T;
  double side = mt.side();
  s.pose = [=](double t) {
    double k = mj(t / T);
    if (!up) k = 1 - k;
    double phi = rad(5 + 165 * k);
    V fwd(std::sin(phi), 0, -std::cos(phi));
    V d0(-std::cos(phi), 0, -std::sin(phi));
    double psi = rad(90 + 90 * k) * side;
    V dor = d0 * std::cos(psi) + cross(fwd, d0) * std::sin(psi);
    V E(0.05 * k, side * (0.2 + 0.05 * k), 1.15 + 0.12 * k);
    return Pose{E + fwd * 0.26, watchFrame(fwd, dor, mt)};
  };
  return s;
}

struct WalkParams {
  double T = 60;
  double cadence = 1.8;      // steps per second
  double swingDeg = 20;      // arm swing amplitude
  double impactG = 0.6;      // heel-strike spike at the wrist
  double bounce = 0.025;     // body vertical oscillation amplitude, m
  double elbowDeg = 15;      // elbow bend (running: ~85)
  uint32_t seed = 7;
};

inline Segment walk(const WalkParams &wp, const Mount &mt, const std::string &name = "walk") {
  std::mt19937 rng(wp.seed);
  std::uniform_real_distribution<double> u(-1, 1);
  auto steps = std::make_shared<std::vector<std::pair<double, double>>>();
  for (double t = 0.5; t < wp.T - 0.5; t += (1.0 / wp.cadence) * (1 + 0.04 * u(rng)))
    steps->push_back({t, wp.impactG * (1 + 0.25 * u(rng))});
  Segment s;
  s.name = name;
  s.T = wp.T;
  double side = mt.side();
  double ph0 = kPi * (u(rng) + 1);
  s.pose = [=](double t) {
    double env = mj(t / 1.0) * mj((wp.T - t) / 1.0);
    double fs = wp.cadence / 2;
    double a = rad(wp.swingDeg) * env * std::sin(2 * kPi * fs * t + ph0);
    V upper(std::sin(a), 0, -std::cos(a));
    double e = a + rad(wp.elbowDeg);
    V fwd(std::sin(e), 0, -std::cos(e));
    V dor(0, side, 0);
    V Sh(0, side * (0.2 + 0.015 * env * std::sin(2 * kPi * fs * t)),
         1.45 + wp.bounce * env * std::cos(2 * kPi * wp.cadence * t));
    return Pose{Sh + upper * 0.3 + fwd * 0.26, watchFrame(fwd, dor, mt)};
  };
  s.extra = [=](double t) {
    double az = 0;
    for (auto &st : *steps) {
      double dt = t - st.first;
      if (dt < -0.05 || dt > 0.15) continue;
      double g = std::exp(-(dt / 0.018) * (dt / 0.018));
      double ring = dt > 0 ? std::exp(-dt / 0.03) * std::sin(2 * kPi * 22 * dt) * 0.5 : 0;
      az += st.second * kGrav * (g + ring);
    }
    return V(0.15 * az, 0, az);
  };
  return s;
}

inline WalkParams runParams(double T, uint32_t seed) {
  WalkParams w;
  w.T = T;
  w.cadence = 2.8;
  w.swingDeg = 35;
  w.impactG = 2.2;
  w.bounce = 0.045;
  w.elbowDeg = 85;
  w.seed = seed;
  return w;
}

// Random everyday wrist motion: smooth wandering orientation and position,
// occasional quick flicks and taps on hard surfaces.
struct FidgetParams {
  double T = 60;
  double rotDeg = 35;
  double posM = 0.08;
  double flicksPerMin = 8;
  double tapsPerMin = 6;
  bool armUp = false;        // forearm held in front (phone/typing) vs hanging
  uint32_t seed = 11;
};

inline Segment fidget(const FidgetParams &fp, const Mount &mt) {
  std::mt19937 rng(fp.seed);
  std::uniform_real_distribution<double> u(-1, 1), ph(0, 2 * kPi), fr(0.08, 1.2);
  struct Sin { double a, f, p; };
  auto comps = std::make_shared<std::vector<Sin>>();
  for (int i = 0; i < 18; i++) comps->push_back({u(rng), fr(rng), ph(rng)});
  struct Flick { double t, dur, ang; V axis; };
  auto flicks = std::make_shared<std::vector<Flick>>();
  int nf = (int)(fp.flicksPerMin * fp.T / 60.0);
  for (int i = 0; i < nf; i++) {
    double t = 1.5 + (fp.T - 3) * (0.5 + 0.5 * u(rng));
    flicks->push_back({t, 0.25 + 0.2 * (0.5 + 0.5 * u(rng)), rad(30 + 50 * (0.5 + 0.5 * u(rng))),
                       unit(V(u(rng), u(rng), u(rng)))});
  }
  auto taps = std::make_shared<std::vector<std::pair<double, double>>>();
  int nt = (int)(fp.tapsPerMin * fp.T / 60.0);
  for (int i = 0; i < nt; i++)
    taps->push_back({1.0 + (fp.T - 2) * (0.5 + 0.5 * u(rng)), 0.4 + 0.8 * (0.5 + 0.5 * u(rng))});
  Segment s;
  s.name = "fidget";
  s.T = fp.T;
  double side = mt.side();
  s.pose = [=](double t) {
    double env = mj(t / 1.5) * mj((fp.T - t) / 1.5);
    const auto &c = *comps;
    V rv(0, 0, 0), pv(0, 0, 0);
    for (int i = 0; i < 6; i++) {
      rv.x += c[i].a * std::sin(2 * kPi * c[i].f * t + c[i].p);
      rv.y += c[i + 6].a * std::sin(2 * kPi * c[i + 6].f * t + c[i + 6].p);
      rv.z += c[i + 12].a * std::sin(2 * kPi * c[i + 12].f * t + c[i + 12].p);
      pv.x += c[i].a * std::cos(2 * kPi * c[i].f * 0.7 * t + c[i + 6].p);
      pv.y += c[i + 6].a * std::cos(2 * kPi * c[i + 6].f * 0.7 * t + c[i + 12].p);
      pv.z += c[i + 12].a * std::cos(2 * kPi * c[i + 12].f * 0.7 * t + c[i].p);
    }
    rv = rv * (rad(fp.rotDeg) / 2.5 * env);
    pv = pv * (fp.posM / 2.5 * env);
    V fwd = fp.armUp ? V(1, 0, -0.15) : V(0.1, 0, -1);
    V dor = fp.armUp ? V(0, 0, 1) : V(0, side, 0);
    double ang = len(rv);
    if (ang > 1e-9) { V k = rv * (1 / ang); fwd = rotAbout(fwd, k, ang); dor = rotAbout(dor, k, ang); }
    for (auto &f : *flicks) {
      double dt = t - f.t;
      if (dt < 0 || dt > f.dur) continue;
      double k = std::sin(kPi * dt / f.dur);
      fwd = rotAbout(fwd, f.axis, f.ang * k * k);
      dor = rotAbout(dor, f.axis, f.ang * k * k);
    }
    V P = V(0.2, side * 0.25, fp.armUp ? 1.1 : 0.9) + pv;
    return Pose{P, watchFrame(fwd, dor, mt)};
  };
  s.extra = [=](double t) {
    double az = 0;
    for (auto &tp : *taps) {
      double dt = t - tp.first;
      if (dt < -0.02 || dt > 0.08) continue;
      az += tp.second * kGrav * std::exp(-(dt / 0.008) * (dt / 0.008));
    }
    return V(0, 0, az);
  };
  return s;
}

// Single gestures that look a bit like a rep.
inline Segment glance(double hold, const Mount &mt) {
  Segment s;
  s.name = "glance";
  s.T = 0.7 + hold + 0.6 + 0.5;
  double side = mt.side();
  s.pose = [=](double t) {
    double k = t < 0.7 ? mj(t / 0.7) : (t < 0.7 + hold ? 1 : 1 - mj((t - 0.7 - hold) / 0.6));
    double phi = rad(5 + 95 * k);
    V fwd(std::sin(phi), 0, -std::cos(phi));
    V d0(-std::cos(phi), 0, -std::sin(phi));
    double psi = rad(90 + 90 * k) * side;   // turn the face up
    V dor = d0 * std::cos(psi) + cross(fwd, d0) * std::sin(psi);
    V E(0.05 * k, side * 0.2, 1.15);
    return Pose{E + fwd * 0.26, watchFrame(fwd, dor, mt)};
  };
  return s;
}

inline Segment drink(double hold, const Mount &mt) {
  Segment s;
  s.name = "drink";
  s.T = 0.9 + hold + 0.8 + 0.5;
  double side = mt.side();
  s.pose = [=](double t) {
    double k = t < 0.9 ? mj(t / 0.9) : (t < 0.9 + hold ? 1 : 1 - mj((t - 0.9 - hold) / 0.8));
    double phi = rad(5 + 140 * k);
    V fwd(std::sin(phi), 0, -std::cos(phi));
    V d0(-std::cos(phi), 0, -std::sin(phi));
    double psi = rad(90) * side;
    V dor = d0 * std::cos(psi) + cross(fwd, d0) * std::sin(psi);
    V E(0.08 * k, side * 0.15, 1.15 + 0.05 * k);
    return Pose{E + fwd * 0.26, watchFrame(fwd, dor, mt)};
  };
  return s;
}

// Bend down and pick something up (arm hangs, body drops ~0.5 m).
inline Segment pickUp(const Mount &mt) {
  Segment s;
  s.name = "pick up";
  s.T = 3.0;
  double side = mt.side();
  s.pose = [=](double t) {
    double k = t < 1.3 ? mj(t / 1.3) : 1 - mj((t - 1.5) / 1.3);
    if (t >= 1.3 && t < 1.5) k = 1;
    V fwd = unit(V(0.15 * k, 0, -1));
    V dor(0, side, 0);
    return Pose{V(0.1 * k, side * 0.2, 0.9 - 0.5 * k), watchFrame(fwd, dor, mt)};
  };
  return s;
}

// ---------------------------------------------------------------------------
// Sensor and scenario
// ---------------------------------------------------------------------------
struct SensorParams {
  double offsetG = 0.03;      // per-axis offset, uniform +/-
  double scaleErr = 0.02;     // per-axis scale error, uniform +/-
  double noiseG = 0.002;      // white noise per 100 Hz sample
  double tremorDeg = 0.4;     // physiological tremor (8-12 Hz)
  double wobbleDeg = 1.0;     // watch rocking on the strap (2-5 Hz)
  double countsPerG = 2048;
  double rangeG = 4.0;
};

class Scenario {
public:
  Scenario(const Mount &mt, const SensorParams &sp, uint32_t seed)
      : mt_(mt), sp_(sp), rng_(seed) {}

  const Mount &mount() const { return mt_; }

  // Append a segment. Its positions are shifted so it starts where the
  // previous one ended; orientation is blended over `blend` seconds.
  void add(Segment s, double blend = -1) {
    Item it;
    it.seg = std::move(s);
    Pose start = it.seg.pose(0);
    if (!items_.empty()) {
      Item &prev = items_.back();
      Pose end = prev.seg.pose(prev.seg.T);
      end.p = end.p + prev.offset;
      double ang = angleBetween(end.R, start.R);
      it.blend = blend >= 0 ? blend : (ang < rad(5) ? 0.0 : 0.3 + 0.6 * ang / kPi);
      it.offset = end.p - start.p;
      it.blendFrom = end.R;
      it.t0 = prev.t0 + prev.blend + prev.seg.T;
    } else {
      it.blend = 0;
      it.offset = V(0, 0, 0) - start.p;
      it.blendFrom = start.R;
      it.t0 = 0;
    }
    items_.push_back(std::move(it));
  }

  double duration() const {
    if (items_.empty()) return 0;
    const Item &l = items_.back();
    return l.t0 + l.blend + l.seg.T;
  }

  std::vector<SetTruth> truths() const {
    std::vector<SetTruth> v;
    for (auto &it : items_) {
      if (!it.seg.isSet) continue;
      SetTruth st = it.seg.truth;
      double base = it.t0 + it.blend;
      for (auto &d : st.repDone) d += base;
      st.tFirstStart = base;
      st.tLastEnd = st.repDone.empty() ? base : st.repDone.back();
      st.label = it.seg.name;
      v.push_back(st);
    }
    return v;
  }

  // Render raw counts at 100 Hz: xyz interleaved.
  std::vector<int16_t> render() {
    std::uniform_real_distribution<double> u(-1, 1), ph(0, 2 * kPi);
    std::normal_distribution<double> nrm(0, 1);
    double off[3], scl[3];
    for (int i = 0; i < 3; i++) { off[i] = sp_.offsetG * u(rng_); scl[i] = 1 + sp_.scaleErr * u(rng_); }
    double tf[3], tp[3], wf[3], wp[3];
    for (int i = 0; i < 3; i++) {
      tf[i] = 8 + 4 * (0.5 + 0.5 * u(rng_)); tp[i] = ph(rng_);
      wf[i] = 2 + 3 * (0.5 + 0.5 * u(rng_)); wp[i] = ph(rng_);
    }
    const double h = 0.001;
    double T = duration();
    long nTicks = (long)(T / h);
    std::vector<int16_t> out;
    out.reserve((size_t)(nTicks / 10 + 1) * 3);
    double acc[3] = {0, 0, 0};
    int k = 0;
    for (long i = 1; i < nTicks - 1; i++) {
      double t = i * h;
      Pose pm = poseAt(t - h), p0 = poseAt(t), pp = poseAt(t + h);
      V a = (pp.p - p0.p * 2 + pm.p) * (1.0 / (h * h));
      V f = a + V(0, 0, kGrav) + extraAt(t);
      // tremor + strap wobble: small extra rotation of the watch
      V rv(rad(sp_.tremorDeg) * std::sin(2 * kPi * tf[0] * t + tp[0]) +
               rad(sp_.wobbleDeg) * std::sin(2 * kPi * wf[0] * t + wp[0]),
           rad(sp_.tremorDeg) * std::sin(2 * kPi * tf[1] * t + tp[1]) +
               rad(sp_.wobbleDeg) * std::sin(2 * kPi * wf[1] * t + wp[1]),
           rad(sp_.tremorDeg) * std::sin(2 * kPi * tf[2] * t + tp[2]) +
               rad(sp_.wobbleDeg) * std::sin(2 * kPi * wf[2] * t + wp[2]));
      V fb = p0.R.toBody(f) * (1.0 / kGrav);
      double ang = len(rv);
      if (ang > 1e-9) fb = rotAbout(fb, rv * (1 / ang), ang);
      acc[0] += fb.x; acc[1] += fb.y; acc[2] += fb.z;
      if (++k == 10) {
        for (int j = 0; j < 3; j++) {
          double g = acc[j] / 10.0 * scl[j] + off[j] + sp_.noiseG * nrm(rng_);
          if (g > sp_.rangeG) g = sp_.rangeG;
          if (g < -sp_.rangeG) g = -sp_.rangeG;
          long c = std::lround(g * sp_.countsPerG);
          if (c > 8191) c = 8191;
          if (c < -8192) c = -8192;
          out.push_back((int16_t)c);
          acc[j] = 0;
        }
        k = 0;
      }
    }
    return out;
  }

private:
  struct Item {
    Segment seg;
    double t0 = 0, blend = 0;
    V offset;
    M blendFrom;
  };
  Mount mt_;
  SensorParams sp_;
  std::mt19937 rng_;
  std::vector<Item> items_;

  const Item *find(double t, double &local) const {
    for (size_t i = 0; i < items_.size(); i++) {
      const Item &it = items_[i];
      double end = it.t0 + it.blend + it.seg.T;
      if (t < end || i + 1 == items_.size()) { local = t - it.t0; return &it; }
    }
    local = 0;
    return nullptr;
  }

  Pose poseAt(double t) const {
    double local;
    const Item *it = find(t, local);
    if (!it) return Pose{};
    if (local < it->blend) {
      Pose b = it->seg.pose(0);
      double k = mj(local / it->blend);
      Q q = slerp(quatFromM(it->blendFrom), quatFromM(b.R), k);
      return Pose{b.p + it->offset, mFromQuat(q)};
    }
    double st = local - it->blend;
    if (st > it->seg.T) st = it->seg.T;
    Pose p = it->seg.pose(st);
    p.p = p.p + it->offset;
    return p;
  }

  V extraAt(double t) const {
    double local;
    const Item *it = find(t, local);
    if (!it || !it->seg.extra || local < it->blend) return V(0, 0, 0);
    return it->seg.extra(local - it->blend);
  }
};

}  // namespace synth
