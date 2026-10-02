// Rep Counter: rep / set detector. See rep_detector.h for the overview.
#include "rep_detector.h"
#include <string.h>

namespace reps {

static const float kG = 9.80665f;

static inline bool before(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static inline float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// ---------------------------------------------------------------------------
// Tuning table (host tools)
// ---------------------------------------------------------------------------
#define REPS_TUNING_FIELDS(X)                                                \
  X(lpfHz) X(rotLpfHz) X(rotArmDeg) X(rotDepartDeg) X(rotMinRepDeg) X(homeTauSec)          \
  X(rotGateDeg) X(linArmM) X(linDepartM) X(linMinRepM) X(baseTauSec) X(velTauSec)           \
  X(velHpSec) X(steadyG) X(zuptSec) X(rotGateDps) X(linMaxRotDeg)             \
  X(returnFrac) X(minRepSec) X(maxRepSec) X(holdRebaseSec) X(confirmGapSec)    \
  X(endPeriodK) X(settleSec)                                                    \
  X(ampRatioMin) X(ampRatioMax) X(durRatioMin) X(durRatioMax) X(setAmpMin)     \
  X(setAmpMax) X(impactHz) X(impactG) X(hfGateG) X(hfHoldSec) X(curlMinElev)  \
  X(curlHomeElev) X(raiseMinElev) X(raiseMinDeg) X(raiseHomeElev) X(raiseMinSec) X(maxPath)    \
  X(maxHoldFrac) X(parkDps) X(linMinRepSec) X(linMaxPath)      \
  X(manualMinElev) X(raiseMaxDeg) X(raiseMinUp) X(handUpG)

float *tuningField(Tuning &t, const char *name) {
#define X(f) if (strcmp(name, #f) == 0) return &t.f;
  REPS_TUNING_FIELDS(X)
#undef X
  return nullptr;
}

const char *tuningName(int i) {
  static const char *const names[] = {
#define X(f) #f,
    REPS_TUNING_FIELDS(X)
#undef X
  };
  return (i >= 0 && i < (int)(sizeof(names) / sizeof(names[0]))) ? names[i] : nullptr;
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void Detector::reset(const Config &cfg) {
  cfg_ = cfg;
  if (cfg_.fs < 10.f) cfg_.fs = 10.f;
  if (cfg_.countsPerG <= 0.f) cfg_.countsPerG = 2048.f;
  if (cfg_.handAxisSignLeft >= 0) cfg_.handAxisSignLeft = 1; else cfg_.handAxisSignLeft = -1;
  const Tuning &T = cfg_.tune;
  dt_ = 1.f / cfg_.fs;
  n_ = 0;
  primed_ = false;
  for (int i = 0; i < 3; i++) lp_[i] = Biquad::lowpass(T.lpfHz, cfg_.fs);
  for (int i = 0; i < 3; i++) rlp_[i] = Biquad::lowpass(T.rotLpfHz, cfg_.fs);
  // Butterworth 4th order as two sections (Q = 0.541, 1.307).
  hfHp_[0] = Biquad::highpass(T.impactHz, cfg_.fs, 0.5411961f);
  hfHp_[1] = Biquad::highpass(T.impactHz, cfg_.fs, 1.3065630f);
  hfE_.setTau(0.25f, cfg_.fs);
  base_.setTau(T.baseTauSec, cfg_.fs);
  for (int i = 0; i < 3; i++) gSlow_[i].setTau(0.8f, cfg_.fs);
  rate_.setTau(0.10f, cfg_.fs);
  homeK_ = 1.f - expf(-1.f / (T.homeTauSec * cfg_.fs));
  velLeak_ = expf(-1.f / (T.velTauSec * cfg_.fs));
  fastBaseK_ = 1.f - expf(-1.f / (0.15f * cfg_.fs));
  velHpK_ = 1.f - expf(-1.f / (T.velHpSec * cfg_.fs));
  settleK_ = 1.f - expf(-1.f / (0.2f * cfg_.fs));
  rot_ = Rot();
  lin_ = Lin();
  setState_ = Idle;
  tent_ = Rec();
  near_ = Rec();
  nearValid_ = false;
  setCh_ = Channel::None;
  reps_ = 0;
  setFirstStart_ = setLastEnd_ = setLastStart_ = 0;
  nAmps_ = nPeriods_ = 0;
  memset(votes_, 0, sizeof(votes_));
  marginSum_ = 0.f;
  handVotes_ = cfg_.learnedHandSign != 0 ? 2 : 0;
  gateUntil_ = 0;
  suppressUntil_ = 0;
  for (int i = 0; i < 4; i++) impacts_[i] = 0;
  nImpacts_ = 0;
  steadyN_ = 0;
  winHead_ = 0;
  winSum_ = winSum2_ = 0.f;
  qHead_ = qCount_ = 0;
  live_ = Live();
  lastReject_ = "";
}

void Detector::setMode(Mode m) {
  if (m == cfg_.mode) return;
  cfg_.mode = m;
  if (setState_ == Active) endSet(false);
  else if (setState_ == Tentative) endSet(true);
}

void Detector::markGap() {
  primed_ = false;         // filters restart from the next sample
  if (setState_ == Tentative) endSet(true);
  // A confirmed set survives a short hole; its end timer keeps running.
}

void Detector::suppressGate(uint32_t ms) {
  uint32_t until = n_ + (uint32_t)(ms * cfg_.fs / 1000.f) + secToN(0.05f);
  if (before(suppressUntil_, until)) suppressUntil_ = until;
}

void Detector::endSetNow() {
  if (setState_ == Active) endSet(false);
  else if (setState_ == Tentative) endSet(true);
}

void Detector::prime(const V3 &g) {
  float m = norm(g);
  lp_[0].prime(g.x);
  lp_[1].prime(g.y);
  lp_[2].prime(g.z);
  rlp_[0].prime(g.x);
  rlp_[1].prime(g.y);
  rlp_[2].prime(g.z);
  hfHp_[0].prime(m);
  hfHp_[1].prime(0.f);
  hfE_.prime(0.f);
  base_.prime(m);
  gSlow_[0].prime(g.x);
  gSlow_[1].prime(g.y);
  gSlow_[2].prime(g.z);
  rate_.prime(0.f);
  u_ = normalized(g);
  uFast_ = u_;
  for (int i = 0; i < kUHist; i++) uHist_[i] = u_;
  uHead_ = 0;
  for (int i = 0; i < kTurnHist; i++) turnHist_[i] = u_;
  turnHead_ = 0;
  turnDiv_ = 0;
  turn_ = 0.f;
  for (int i = 0; i < kWin; i++) win_[i] = m;
  winHead_ = 0;
  winSum_ = m * kWin;
  winSum2_ = m * m * kWin;
  steadyN_ = 0;
  rot_ = Rot();
  rot_.h = u_;
  rot_.uPrev = u_;
  rot_.nQuiet = n_;
  lin_ = Lin();
  lin_.nQuiet = n_;
  lin_.uStart = u_;
}

// ---------------------------------------------------------------------------
// Per-sample processing
// ---------------------------------------------------------------------------
void Detector::pushG(float gx, float gy, float gz) {
  const Tuning &T = cfg_.tune;
  V3 g(gx, gy, gz);
  if (!primed_) { prime(g); primed_ = true; }
  n_++;

  V3 a(lp_[0].step(gx), lp_[1].step(gy), lp_[2].step(gz));
  float m = norm(a);
  float mRaw = norm(g);

  // --- Locomotion gate: walking/running make a train of sharp impacts in
  // |a| at a steady cadence; a lift is smooth, with almost nothing above
  // ~8 Hz. Look for >= 3 regularly spaced impacts in that band.
  float hp = hfHp_[1].step(hfHp_[0].step(mRaw));
  bool suppressed = before(n_, suppressUntil_);
  if (!suppressed) hfE_.step(hp * hp);
  float hfRms = sqrtf(hfE_.y);
  if (!suppressed && hp > T.impactG &&
      (nImpacts_ == 0 || n_ - impacts_[(nImpacts_ - 1) & 3] > secToN(0.2f))) {
    impacts_[nImpacts_ & 3] = n_;
    nImpacts_++;
    if (nImpacts_ >= 3) {
      uint32_t i1 = impacts_[(nImpacts_ - 1) & 3];
      uint32_t i2 = impacts_[(nImpacts_ - 2) & 3];
      uint32_t i3 = impacts_[(nImpacts_ - 3) & 3];
      float d1 = (i1 - i2) * dt_, d2 = (i2 - i3) * dt_;
      bool cadence = d1 > 0.22f && d1 < 0.9f && d2 > 0.22f && d2 < 0.9f &&
                     fabsf(d1 - d2) < 0.35f * (d1 > d2 ? d1 : d2);
      if (cadence) gateUntil_ = n_ + secToN(T.hfHoldSec);
    }
  }
  // Sustained heavy shaking (running, jumping) also gates.
  if (!suppressed && hfRms > T.hfGateG) gateUntil_ = n_ + secToN(T.hfHoldSec);

  // --- Gravity direction and its rate of change.
  // The rotation channel uses a smoother copy; the rotation gate (which
  // freezes the linear channel) needs the quicker one.
  V3 ar(rlp_[0].step(a.x), rlp_[1].step(a.y), rlp_[2].step(a.z));
  float mr = norm(ar);
  if (mr > 0.3f && mr < 3.5f) u_ = ar * (1.f / mr);
  if (m > 0.3f && m < 3.5f) uFast_ = a * (1.f / m);
  for (int i = 0; i < 3; i++) gSlow_[i].step(i == 0 ? a.x : (i == 1 ? a.y : a.z));
  V3 uOld = uHist_[uHead_];
  uHist_[uHead_] = uFast_;
  uHead_ = (uHead_ + 1) % kUHist;
  float rateDps = angleDeg(uFast_, uOld) / (kUHist * dt_);
  float rateS = rate_.step(rateDps);
  rateNow_ = rateS;
  // Total turn over the last ~0.5 s (decimated history), so a quick wobble
  // of a few degrees during a press can't freeze the linear channel.
  if (++turnDiv_ >= 5) {
    turnDiv_ = 0;
    turnHist_[turnHead_] = uFast_;
    turnHead_ = (turnHead_ + 1) % kTurnHist;
  }
  turn_ = angleDeg(uFast_, turnHist_[turnHead_]);

  // --- Steadiness of |a| over the last 0.25 s (drives the linear channel's
  // baseline and its zero-velocity update).
  float old = win_[winHead_];
  win_[winHead_] = m;
  winHead_ = (winHead_ + 1) % kWin;
  winSum_ += m - old;
  winSum2_ += m * m - old * old;
  float mean = winSum_ / kWin;
  float var = winSum2_ / kWin - mean * mean;
  bool steady = var < T.steadyG * T.steadyG;
  if (steady) { if (steadyN_ < 0xFFFF) steadyN_++; } else steadyN_ = 0;

  Candidate c;
  if (stepRot(c)) handleCandidate(c);
  if (stepLin(m, mean, steady, rateS, c)) handleCandidate(c);

  dbg_.theta = rot_.theta;
  dbg_.rotPh = (uint8_t)rot_.ph;
  dbg_.linExc = lin_.exc;
  dbg_.linPh = (uint8_t)lin_.ph;
  dbg_.vel = lin_.v;
  dbg_.accV = m - base_.y;
  dbg_.rate = rateS;
  dbg_.hfRms = hfRms;
  dbg_.gated = !before(gateUntil_, n_) && gateUntil_ != 0;
  dbg_.frozen = before(n_, lin_.rotHoldUntil);

  // --- Set timers.
  if (setState_ == Tentative) {
    float idle = (n_ - tent_.nEnd) * dt_;
    if (idle > cfg_.setEndSec && !channelBusy(tent_.ch)) endSet(true);
  } else if (setState_ == Active) {
    // Slow sets get more time: the gap between counted reps is the rep period.
    float limit = cfg_.setEndSec;
    if (nPeriods_ > 0) {
      float p = smallMedian(periods_, nPeriods_) * T.endPeriodK;
      if (p > limit) limit = p;
    }
    float idle = (n_ - setLastEnd_) * dt_;
    if (idle > limit && !channelBusy(setCh_)) endSet(false);
  }
  updateLive();
}

// ---------------------------------------------------------------------------
// Rotation channel
// ---------------------------------------------------------------------------
bool Detector::stepRot(Candidate &out) {
  const Tuning &T = cfg_.tune;
  Rot &r = rot_;
  float step = angleDeg(u_, r.uPrev);
  r.uPrev = u_;
  if (r.ph != Rot::Home) r.path += step;
  switch (r.ph) {
    case Rot::Home: {
      // Follow slow posture drift so "home" is wherever the arm rests.
      r.h = normalized(r.h + (u_ - r.h) * homeK_, u_);
      r.theta = angleDeg(u_, r.h);
      if (r.theta < 3.f) r.nQuiet = n_;
      if (r.theta > T.rotArmDeg) {
        r.ph = Rot::Rising;
        r.nStart = r.nQuiet;
        r.thMax = r.theta;
        r.nPeak = n_;
        r.uPeak = u_;
        r.departed = false;
        r.path = r.theta;
        r.nearPeak = 0;
        r.holdRef = r.theta;
      }
      return false;
    }
    case Rot::Rising: {
      r.theta = angleDeg(u_, r.h);
      if (r.theta > r.thMax) { r.thMax = r.theta; r.nPeak = n_; r.uPeak = u_; }
      // Time parked away from home (barely moving, well out): drinking from
      // a bottle or reading the watch looks like a curl with a long pause.
      if (rateNow_ < T.parkDps && r.theta > 0.6f * r.thMax) r.nearPeak++;
      if (!r.departed) {
        if (r.thMax >= T.rotDepartDeg) {
          r.departed = true;
        } else if (r.theta < 0.5f * T.rotArmDeg) {
          r.ph = Rot::Home;            // a wobble, not a movement
          r.nQuiet = n_;
          return false;
        } else if (n_ - r.nStart > secToN(3.0f) && rateNow_ < 12.f) {
          r.ph = Rot::Home;            // drifted to a new resting posture
          r.h = u_;
          r.nQuiet = n_;
          return false;
        }
      }
      if (r.departed && r.theta <= r.thMax * (1.f - T.returnFrac)) {
        out = Candidate();
        out.ch = Channel::Rotation;
        out.nStart = r.nStart;
        out.nPeak = r.nPeak;
        out.nEnd = n_;
        out.amp = r.thMax;
        out.upAtPeak = r.uPeak.z;      // +Z = out of the watch face
        out.axis = normalized(cross(r.h, r.uPeak));
        out.elevHome = r.h.x;          // X = along the forearm
        out.elevPeak = r.uPeak.x;
        out.path = r.path / (1.6f * (r.thMax > 1.f ? r.thMax : 1.f));
        out.holdFrac = (n_ > r.nStart) ? (float)r.nearPeak / (float)(n_ - r.nStart) : 0.f;
        // Track the way back down; the turnaround becomes the next home.
        r.ph = Rot::Falling;
        r.thMin = angleDeg(u_, r.uPeak);   // distance from the peak
        r.uMin = u_;
        r.nMin = n_;
        return true;
      }
      if (n_ - r.nStart > secToN(T.holdRebaseSec) && rateNow_ < 30.f) {
        // Parked away from home for ages: a new posture, not a rep. (Only
        // re-home while the arm is still, never part-way through a motion.)
        r.ph = Rot::Home;
        r.h = u_;
        r.nQuiet = n_;
      }
      return false;
    }
    case Rot::Falling: {
      // thMin here holds the largest distance from the peak seen so far, i.e.
      // how far back down we've come; the farthest point is the turnaround.
      float fromPeak = angleDeg(u_, r.uPeak);
      if (fromPeak > r.thMin) { r.thMin = fromPeak; r.uMin = u_; r.nMin = n_; }
      r.theta = angleDeg(u_, r.h);
      if (r.theta > r.thMax + T.rotArmDeg) {
        // Went further out than the peak: that "return" was only a wobble.
        // Keep the original home and carry on rising.
        r.ph = Rot::Rising;
        r.thMax = r.theta;
        r.nPeak = n_;
        r.uPeak = u_;
        return false;
      }
      float away = angleDeg(u_, r.uMin);
      if (away > T.rotArmDeg && fromPeak < r.thMin - 0.5f * T.rotArmDeg) {
        // Heading back up: the next rep starts at the turnaround.
        r.h = r.uMin;
        r.ph = Rot::Rising;
        r.nStart = r.nMin;
        r.theta = away;
        r.thMax = away;
        r.nPeak = n_;
        r.uPeak = u_;
        r.departed = false;
        r.path = away;
        r.nearPeak = 0;
        r.holdRef = away;
        return false;
      }
      if (n_ - r.nMin > secToN(0.4f)) {
        r.ph = Rot::Home;
        r.h = r.uMin;
        r.nQuiet = r.nMin;
      }
      return false;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Linear channel
// ---------------------------------------------------------------------------
bool Detector::stepLin(float m, float meanM, bool steady, float rateDps,
                       Candidate &out) {
  const Tuning &T = cfg_.tune;
  Lin &L = lin_;

  bool rotating = rateDps > T.rotGateDps && turn_ > T.rotGateDeg;
  if (rotating) L.rotHoldUntil = n_ + secToN(0.3f);
  bool frozen = rotating || before(n_, L.rotHoldUntil);

  // Baseline of |a| = 1 g plus this orientation's sensor offset (a few
  // percent, and it changes with orientation). While frozen by a rotation it
  // re-learns quickly; otherwise it only creeps, because the vertical
  // acceleration of a slow rep can stay nearly constant for a second.
  if (frozen) {
    base_.y += fastBaseK_ * (m - base_.y);
    L.settleUntil = n_ + secToN(T.settleSec);
    L.settleArmed = false;
  } else if (before(n_, L.settleUntil)) {
    // Just after a posture change. Once the arm has been held still for a
    // moment the baseline converges quickly (a held arm has no acceleration
    // to eat); the first movement after that ends the window, so the start
    // of a slow rep can't be mistaken for an offset.
    if (steady && steadyN_ >= secToN(0.3f)) {
      L.settleArmed = true;
      base_.y += settleK_ * (meanM - base_.y);
    } else if (!steady && L.settleArmed) {
      L.settleUntil = n_;
    }
  } else {
    base_.step(m);
  }
  float av = m - base_.y;                 // vertical linear acceleration, g

  if (frozen) {
    // Big orientation change: |a| is polluted by centripetal terms and the
    // baseline is stale. Freeze, forget velocity, re-anchor home.
    L.v = 0.f;
    L.vRaw = 0.f;
    L.vMean = 0.f;
    L.d = 0.f;
    L.dHome = 0.f;
    L.ph = Lin::Home;
    L.exc = 0.f;
    L.nQuiet = n_;
    L.uStart = u_;
    return false;
  }

  // Velocity: leaky integral of the vertical acceleration, then high-passed
  // so it is zero-mean. Any residual baseline error (a few mg is plenty to
  // wreck a double integral) becomes a decaying transient instead of a
  // drift, and a rep's up and down lobes balance by construction.
  L.vRaw = L.vRaw * velLeak_ + av * kG * dt_;
  L.vMean += velHpK_ * (L.vRaw - L.vMean);
  L.v = L.vRaw - L.vMean;
  if (T.zuptSec > 0.f && steadyN_ > secToN(T.zuptSec)) {
    L.vRaw *= 0.85f;                       // held still => not moving
    L.vMean *= 0.85f;
  }
  L.d += L.v * dt_;
  if (L.ph != Lin::Home) L.path += fabsf(L.v) * dt_;

  switch (L.ph) {
    case Lin::Home: {
      // Home follows the position while nothing is happening.
      L.dHome += (L.d - L.dHome) * homeK_;
      float exc = L.d - L.dHome;
      L.exc = fabsf(exc);
      if (L.exc < 0.01f) { L.nQuiet = n_; L.uStart = u_; }
      if (L.exc > T.linArmM) {
        L.ph = Lin::Rising;
        L.sign = exc > 0 ? 1 : -1;
        L.nStart = L.nQuiet;
        L.excMax = L.exc;
        L.dPeak = L.d;
        L.nPeak = n_;
        L.departed = false;
        L.rotMax = angleDeg(u_, L.uStart);
        L.path = L.exc;
        L.nearPeak = 0;
        L.holdRef = L.exc;
      }
      // Keep numbers small: re-centre while idle.
      if (fabsf(L.d) > 100.f) { L.dHome -= L.d; L.d = 0.f; }
      return false;
    }
    case Lin::Rising: {
      float exc = (L.d - L.dHome) * L.sign;
      L.exc = exc;
      float rd = angleDeg(u_, L.uStart);
      if (rd > L.rotMax) L.rotMax = rd;
      if (exc > L.excMax) { L.excMax = exc; L.dPeak = L.d; L.nPeak = n_; }
      if (fabsf(L.v) < 0.04f && exc > 0.6f * L.excMax) L.nearPeak++;
      if (!L.departed) {
        if (L.excMax >= T.linDepartM) {
          L.departed = true;
        } else if (exc < 0.5f * T.linArmM) {
          L.ph = Lin::Home;
          L.nQuiet = n_;
          return false;
        }
      }
      if (L.departed && exc <= L.excMax * (1.f - T.returnFrac)) {
        out = Candidate();
        out.ch = Channel::Linear;
        out.nStart = L.nStart;
        out.nPeak = L.nPeak;
        out.nEnd = n_;
        out.amp = L.excMax;
        out.rotDeg = L.rotMax;
        out.sign = L.sign;
        out.path = L.path / (1.6f * (L.excMax > 1e-3f ? L.excMax : 1e-3f));
        out.holdFrac = (n_ > L.nStart) ? (float)L.nearPeak / (float)(n_ - L.nStart) : 0.f;
        V3 gs = normalized(V3(gSlow_[0].y, gSlow_[1].y, gSlow_[2].y));
        out.handG = gs.x * handSign();
        L.ph = Lin::Falling;
        L.dMin = L.d;                 // farthest point on the way back
        L.nMin = n_;
        return true;
      }
      if (n_ - L.nStart > secToN(T.holdRebaseSec) && steady) {
        L.ph = Lin::Home;
        L.dHome = L.d;
        L.nQuiet = n_;
        L.uStart = u_;
      }
      return false;
    }
    case Lin::Falling: {
      // Track the turnaround (farthest point back from the peak).
      float back = (L.dPeak - L.d) * L.sign;
      float best = (L.dPeak - L.dMin) * L.sign;
      if (back > best) { L.dMin = L.d; L.nMin = n_; best = back; }
      L.exc = (L.d - L.dHome) * L.sign;
      if (L.exc > L.excMax + T.linArmM) {
        L.ph = Lin::Rising;               // the dip was a wobble; keep rising
        L.excMax = L.exc;
        L.dPeak = L.d;
        L.nPeak = n_;
        return false;
      }
      float again = (L.d - L.dMin) * L.sign;       // moving away again
      if (again > T.linArmM) {
        L.dHome = L.dMin;
        L.ph = Lin::Rising;
        L.nStart = L.nMin;
        L.excMax = again;
        L.exc = again;
        L.dPeak = L.d;
        L.nPeak = n_;
        L.departed = false;
        L.uStart = u_;
        L.rotMax = 0.f;
        L.path = again;
        L.nearPeak = 0;
        L.holdRef = again;
        return false;
      }
      if (n_ - L.nMin > secToN(0.5f)) {
        L.ph = Lin::Home;
        L.dHome = L.dMin;
        L.nQuiet = L.nMin;
        L.uStart = u_;
      }
      return false;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Arbiter: classification and set logic
// ---------------------------------------------------------------------------
bool Detector::gatedAt(uint32_t nFrom, uint32_t nTo) const {
  // gateUntil_ is pushed forward while locomotion is detected; if it reaches
  // into this rep's window, the rep happened while walking/running.
  (void)nTo;
  return !before(gateUntil_, nFrom) && gateUntil_ != 0;
}

Exercise Detector::classify(const Candidate &c, float &margin, const char *&why) const {
  const Tuning &T = cfg_.tune;
  bool manual = cfg_.mode != Mode::Auto;
  Exercise forced = modeExercise(cfg_.mode);
  margin = 0.f;
  why = "too small";
  float dur = (c.nEnd - c.nStart) * dt_;
  // Before a set is confirmed, a rep that sat parked at the top (a sip of
  // water, a look at the watch) doesn't count. Once a set is under way a
  // paused rep is just a paused rep.
  bool screenHold = setState_ != Active && c.holdFrac > T.maxHoldFrac;
  if (c.ch == Channel::Rotation) {
    if (c.amp < T.rotMinRepDeg) return Exercise::None;
    float dElev = fabsf(c.elevHome - c.elevPeak);
    bool raiseLike = c.amp < T.raiseMaxDeg && c.upAtPeak > T.raiseMinUp;
    if (c.path > T.maxPath) { why = "wiggly"; return Exercise::None; }
    if (screenHold) { why = "held"; return Exercise::None; }
    // A curl or raise changes how steeply the forearm points (gravity along
    // the watch's X axis). Swinging arms while walking, waving and most
    // fidgeting rotate the wrist without that. A manual Raise skips the
    // "back of the wrist faces up" test (thumbs-up raises don't do that);
    // a manual Curl accepts a slightly smaller tilt change (preacher curls).
    bool asRaise = manual ? forced == Exercise::Raise : raiseLike;
    // Raise-shaped humps get the raise checks even when Curl is forced.
    if (asRaise || (raiseLike && forced == Exercise::Curl)) {
      if (c.amp < T.raiseMinDeg || dElev < T.raiseMinElev) { why = "no lift"; return Exercise::None; }
      // Raises start from a hanging arm and are never flicks. (Turning a
      // forearm that's already level - phone in hand - is neither.)
      if (fabsf(c.elevHome) < T.raiseHomeElev) { why = "not hanging"; return Exercise::None; }
      if (dur < T.raiseMinSec) { why = "too fast"; return Exercise::None; }
      margin = clampf((c.upAtPeak - T.raiseMinUp) / 0.3f, 0.f, 1.f) *
               clampf((T.raiseMaxDeg - c.amp) / 25.f, 0.f, 1.f);
      if (!asRaise) return Exercise::Curl;
      return Exercise::Raise;
    }
    if (dElev < (manual ? T.manualMinElev : T.curlMinElev)) { why = "no lift"; return Exercise::None; }
    if (fabsf(c.elevHome) < T.curlHomeElev) {
      // Curls start with the forearm hanging (or, measured the other way
      // round, upright). A "home" with the forearm level means this hump was
      // measured from the top of something else - e.g. a raise.
      why = "not hanging";
      return Exercise::None;
    }
    // Curl: bigger swing and tilt change => more certain.
    margin = clampf((c.amp - T.rotMinRepDeg) / 50.f, 0.f, 1.f) *
             clampf((dElev - 0.6f) / 0.6f, 0.2f, 1.f);
    if (c.amp < T.raiseMaxDeg)
      margin *= clampf((T.raiseMinUp - c.upAtPeak) / 0.4f, 0.25f, 1.f);
    return Exercise::Curl;
  }
  if (c.ch == Channel::Linear) {
    // Same floors in manual modes: the bottom of every curl moves the hand a
    // little vertically, and a forced "Press" mustn't count those.
    if (c.amp < T.linMinRepM) return Exercise::None;
    if (c.rotDeg > T.linMaxRotDeg) { why = "rotated"; return Exercise::None; }
    if (dur < T.linMinRepSec) { why = "too fast"; return Exercise::None; }
    if (c.path > T.linMaxPath) { why = "wiggly"; return Exercise::None; }
    if (screenHold) { why = "held"; return Exercise::None; }
    float sz = clampf((c.amp - T.linMinRepM) / 0.15f, 0.f, 1.f);
    float still = clampf((T.linMaxRotDeg - c.rotDeg) / 25.f, 0.f, 1.f);
    bool handDown = c.handG < -T.handUpG;
    bool asRow = manual ? forced == Exercise::Row : handDown;
    if (asRow) {
      // Hand below the elbow. A row pulls up first; bending down to pick
      // something up goes down first.
      if (c.sign < 0) { why = "bend"; return Exercise::None; }
      margin = sz * still * clampf((-c.handG - T.handUpG) / 0.3f, 0.f, 1.f);
      return Exercise::Row;
    }
    margin = sz * still * (c.handG > T.handUpG ? 1.f : 0.5f);
    return Exercise::Press;
  }
  return Exercise::None;
}

bool Detector::channelBusy(Channel ch) const {
  // A movement that looks like another rep of this set is in flight: at
  // least half the set's usual swing, and not dragging on far longer than
  // the set's own reps take.
  float med = nAmps_ ? smallMedian(amps_, nAmps_) : 0.f;
  uint32_t maxN = secToN(cfg_.tune.maxRepSec);
  if (nPeriods_ > 0)
    maxN = secToN(clampf(1.5f * smallMedian(periods_, nPeriods_), 2.f, cfg_.tune.maxRepSec));
  if (ch == Channel::Rotation)
    return rot_.ph == Rot::Rising && rot_.departed && rot_.thMax >= 0.5f * med &&
           (n_ - rot_.nStart) < maxN;
  if (ch == Channel::Linear)
    return lin_.ph == Lin::Rising && lin_.departed && lin_.excMax >= 0.5f * med &&
           (n_ - lin_.nStart) < maxN;
  return false;
}

void Detector::reject(const Candidate &c, const char *why) {
  lastReject_ = why;
  if (hook_) hook_(c, false, why, hookCtx_);
}

void Detector::handleCandidate(const Candidate &c) {
  const Tuning &T = cfg_.tune;
  float dur = (c.nEnd - c.nStart) * dt_;
  if (gatedAt(c.nStart, c.nEnd)) { reject(c, "locomotion"); return; }
  if (dur < T.minRepSec) { reject(c, "too fast"); return; }
  if (dur > T.maxRepSec) { reject(c, "too slow"); return; }

  float margin = 0.f;
  const char *why = "";
  Exercise ex = classify(c, margin, why);
  if (ex == Exercise::None) {
    // A rep that only just missed the size floor is remembered: the first
    // rep after a posture change is often under-measured while the filters
    // settle. If a set confirms right after it, it's counted (see below).
    bool small = strcmp(why, "too small") == 0 &&
                 c.amp >= 0.6f * (c.ch == Channel::Rotation ? T.rotMinRepDeg : T.linMinRepM);
    bool held = strcmp(why, "held") == 0;
    if ((small || held) && setState_ != Active) rememberNear(c, Exercise::None, dur);
    // Measured the wrong way round (from the top of the movement): turn the
    // channel around so the next hump starts from the bottom.
    if (strcmp(why, "bend") == 0 || strcmp(why, "not hanging") == 0) flipPhase(c);
    reject(c, why);
    return;
  }

  if (cfg_.mode != Mode::Auto) {
    Exercise forced = modeExercise(cfg_.mode);
    if (exerciseChannel(forced) != c.ch) { reject(c, "other channel"); return; }
    ex = forced;
    margin = 1.f;
  }

  switch (setState_) {
    case Idle:
      startTentative(c, ex, dur, margin);
      break;

    case Tentative: {
      if (c.ch != tent_.ch) {
        // The other channel often sees a sliver of the same movement. Only
        // let it take over once that movement is clearly over.
        if (before(c.nStart, tent_.nEnd + secToN(0.3f))) { reject(c, "overlap"); return; }
        startTentative(c, ex, dur, margin);
        break;
      }
      float gap = before(c.nStart, tent_.nEnd) ? 0.f : (c.nStart - tent_.nEnd) * dt_;
      float ar = c.amp / (tent_.amp > 1e-6f ? tent_.amp : 1e-6f);
      float dr = dur / (tent_.dur > 1e-3f ? tent_.dur : 1e-3f);
      bool axisOk = true;
      if (c.ch == Channel::Rotation) axisOk = dot(c.axis, tent_.axis) > 0.2f;
      else axisOk = c.sign == tent_.sign;
      bool consistent = gap <= T.confirmGapSec &&
                        ar >= T.ampRatioMin && ar <= T.ampRatioMax &&
                        dr >= T.durRatioMin && dr <= T.durRatioMax && axisOk;
      if (!consistent) {
        if (hook_) hook_(c, false, "restart", hookCtx_);
        if (gap <= T.confirmGapSec) {
          Candidate old;
          old.ch = tent_.ch; old.nStart = tent_.nStart; old.nEnd = tent_.nEnd;
          old.amp = tent_.amp; old.axis = tent_.axis; old.sign = tent_.sign;
          rememberNear(old, tent_.ex, tent_.dur);
        }
        startTentative(c, ex, dur, margin);
        break;
      }
      // Confirmed: the set is real and already has two reps.
      setState_ = Active;
      setCh_ = c.ch;
      reps_ = 2;
      setFirstStart_ = tent_.nStart;
      setLastStart_ = c.nStart;
      setLastEnd_ = c.nEnd;
      setAxis_ = c.axis;
      setSign_ = c.sign;
      setElevHome_ = tent_.elevHome;
      nAmps_ = 0;
      amps_[nAmps_++] = tent_.amp;
      amps_[nAmps_++] = c.amp;
      nPeriods_ = 0;
      periods_[nPeriods_++] = (c.nStart - tent_.nStart) * dt_;
      memset(votes_, 0, sizeof(votes_));
      votes_[(int)tent_.ex]++;
      votes_[(int)ex]++;
      marginSum_ = tent_.margin + margin;
      // Count a near-miss that led straight into this set (same channel and
      // direction, ending within the confirm gap before the first rep).
      if (nearValid_ && near_.ch == c.ch && !before(tent_.nStart, near_.nEnd) &&
          (tent_.nStart - near_.nEnd) * dt_ <= T.confirmGapSec &&
          (c.ch == Channel::Rotation ? dot(near_.axis, tent_.axis) > 0.2f
                                     : near_.sign == tent_.sign)) {
        reps_ = 3;
        setFirstStart_ = near_.nStart;
        periods_[nPeriods_++] = (tent_.nStart - near_.nStart) * dt_;
        if (hook_) hook_(c, true, "confirm+near", hookCtx_);
      } else if (hook_) {
        hook_(c, true, "confirm", hookCtx_);
      }
      nearValid_ = false;
      emit(EventType::SetStart, dur);
      break;
    }

    case Active: {
      if (c.ch != setCh_) { reject(c, "other channel"); return; }
      if (c.ch == Channel::Rotation && dot(c.axis, setAxis_) < 0.2f) {
        reject(c, "wrong way"); return;
      }
      if (c.ch == Channel::Linear && c.sign != setSign_) {
        reject(c, "wrong way"); return;
      }
      float med = smallMedian(amps_, nAmps_);
      if (c.amp < T.setAmpMin * med) { reject(c, "partial"); return; }
      if (c.amp > T.setAmpMax * med) { reject(c, "outlier"); return; }
      if (reps_ < 255) reps_++;
      if (nAmps_ < kHist) amps_[nAmps_++] = c.amp;
      else { memmove(amps_, amps_ + 1, sizeof(float) * (kHist - 1)); amps_[kHist - 1] = c.amp; }
      float period = (c.nStart - setLastStart_) * dt_;
      if (nPeriods_ < kHist) periods_[nPeriods_++] = period;
      else { memmove(periods_, periods_ + 1, sizeof(float) * (kHist - 1)); periods_[kHist - 1] = period; }
      setLastStart_ = c.nStart;
      setLastEnd_ = c.nEnd;
      votes_[(int)ex]++;
      marginSum_ += margin;
      if (hook_) hook_(c, true, "rep", hookCtx_);
      emit(EventType::Rep, dur);
      break;
    }
  }
}

void Detector::startTentative(const Candidate &c, Exercise ex, float dur, float margin) {
  tent_.ch = c.ch;
  tent_.ex = ex;
  tent_.nStart = c.nStart;
  tent_.nEnd = c.nEnd;
  tent_.amp = c.amp;
  tent_.dur = dur;
  tent_.axis = c.axis;
  tent_.sign = c.sign;
  tent_.elevHome = c.elevHome;
  tent_.margin = margin;
  setState_ = Tentative;
  reps_ = 1;
  setFirstStart_ = c.nStart;
  setLastStart_ = c.nStart;
  setLastEnd_ = c.nEnd;
  memset(votes_, 0, sizeof(votes_));
  votes_[(int)ex]++;
  marginSum_ = margin;
  nAmps_ = 0;
  nPeriods_ = 0;
  if (hook_) hook_(c, true, "tentative", hookCtx_);
  emit(EventType::RepTentative, dur);
}

float Detector::handSign() const {
  if (cfg_.learnedHandSign != 0) return (float)cfg_.learnedHandSign;
  return (cfg_.wrist == Wrist::Left ? 1.f : -1.f) * cfg_.handAxisSignLeft;
}

void Detector::flipPhase(const Candidate &c) {
  const Tuning &T = cfg_.tune;
  if (c.ch == Channel::Rotation && rot_.ph == Rot::Falling) {
    Rot &r = rot_;
    r.h = r.uPeak;                   // the old peak was really the bottom
    r.ph = Rot::Rising;
    r.nStart = c.nPeak;
    r.theta = angleDeg(u_, r.h);
    r.thMax = r.theta;
    r.uPeak = u_;
    r.nPeak = n_;
    r.departed = r.thMax >= T.rotDepartDeg;
    r.path = r.thMax;
    r.nearPeak = 0;
  } else if (c.ch == Channel::Linear && lin_.ph == Lin::Falling) {
    Lin &L = lin_;
    L.sign = (int8_t)-L.sign;
    L.dHome = L.dPeak;
    L.ph = Lin::Rising;
    L.nStart = c.nPeak;
    L.excMax = (L.d - L.dHome) * L.sign;
    if (L.excMax < 0.f) L.excMax = 0.f;
    L.exc = L.excMax;
    L.dPeak = L.d;
    L.nPeak = n_;
    L.departed = L.excMax >= T.linDepartM;
    L.path = L.excMax;
    L.nearPeak = 0;
    L.rotMax = 0.f;
    L.uStart = u_;
  }
}

void Detector::rememberNear(const Candidate &c, Exercise ex, float dur) {
  near_.ch = c.ch;
  near_.ex = ex;
  near_.nStart = c.nStart;
  near_.nEnd = c.nEnd;
  near_.amp = c.amp;
  near_.dur = dur;
  near_.axis = c.axis;
  near_.sign = c.sign;
  nearValid_ = true;
}

void Detector::endSet(bool discarded) {
  if (discarded) {
    emit(EventType::SetDiscarded, 0.f);
  } else {
    emit(EventType::SetEnd, 0.f);
    // Curls and raises start with the hand hanging below the elbow, so the
    // first rep's home tells us which way the watch's X axis points. Two
    // agreeing sets in a row (net) are needed to set or change it.
    if (setCh_ == Channel::Rotation && reps_ >= 3 && fabsf(setElevHome_) > 0.6f) {
      // Hanging arm: "up" (what the accelerometer reads) points from the
      // hand towards the elbow, i.e. against the hand direction.
      int8_t s = setElevHome_ > 0.f ? -1 : 1;
      if (cfg_.learnedHandSign == 0) {
        cfg_.learnedHandSign = s;            // first evidence: adopt it
        handVotes_ = 1;
      } else if (s == cfg_.learnedHandSign) {
        if (handVotes_ < 3) handVotes_++;    // agreeing set: more confident
      } else if (--handVotes_ < 0) {
        cfg_.learnedHandSign = s;            // outvoted: switch
        handVotes_ = 1;
      }
    }
  }
  setState_ = Idle;
  setCh_ = Channel::None;
  reps_ = 0;
  nAmps_ = nPeriods_ = 0;
  if (!discarded) nearValid_ = false;
}

Exercise Detector::votedExercise() const {
  if (cfg_.mode != Mode::Auto) return modeExercise(cfg_.mode);
  int best = 0;
  for (int i = 1; i < kExerciseCount; i++)
    if (votes_[i] > votes_[best]) best = i;
  return (Exercise)best;
}

uint8_t Detector::confidence() const {
  if (setState_ == Idle) return 0;
  if (setState_ == Tentative) return 1;
  // Agreement between reps and how far the features sit from the boundaries.
  int total = 0, top = 0;
  for (int i = 1; i < kExerciseCount; i++) {
    total += votes_[i];
    if (votes_[i] > top) top = votes_[i];
  }
  float agree = total ? (float)top / total : 0.f;
  float margin = reps_ ? marginSum_ / reps_ : 0.f;
  if (cfg_.mode != Mode::Auto) margin = 1.f;
  if (reps_ >= 3 && agree > 0.8f && margin > 0.45f) return 3;
  if (agree > 0.6f && margin > 0.15f) return 2;
  return 1;
}

float Detector::tempo() const {
  if (nPeriods_ > 0) return smallMedian(periods_, nPeriods_);
  if (setState_ == Tentative) return tent_.dur * 1.3f;
  return 0.f;
}

void Detector::emit(EventType t, float repSec) {
  Event e;
  e.type = t;
  e.tMs = toMs(n_);
  e.reps = reps_;
  e.exercise = (setState_ == Tentative) ? tent_.ex : votedExercise();
  if (cfg_.mode != Mode::Auto && e.exercise == Exercise::None)
    e.exercise = modeExercise(cfg_.mode);
  e.confidence = confidence();
  e.repSec = repSec;
  e.tempoSec = tempo();
  e.setStartMs = toMs(setFirstStart_);
  if (t == EventType::SetEnd) {
    e.setSec = (setLastEnd_ - setFirstStart_) * dt_;
    // A set's last rep is counted at 60 % of the way down; the lowering
    // finishes a little later. Report the count-to-count span plus that tail.
  }
  if (qCount_ < kQ) {
    q_[(qHead_ + qCount_) % kQ] = e;
    qCount_++;
  } else {
    // Queue full (caller not polling): drop the oldest.
    q_[qHead_] = e;
    qHead_ = (qHead_ + 1) % kQ;
  }
}

bool Detector::pollEvent(Event &out) {
  if (qCount_ == 0) return false;
  out = q_[qHead_];
  qHead_ = (qHead_ + 1) % kQ;
  qCount_--;
  return true;
}

void Detector::updateLive() {
  live_.setState = (uint8_t)setState_;
  live_.reps = reps_;
  live_.exercise = (setState_ == Tentative) ? tent_.ex :
                   (setState_ == Active ? votedExercise() :
                   (cfg_.mode != Mode::Auto ? modeExercise(cfg_.mode) : Exercise::None));
  live_.confidence = confidence();
  live_.tempoSec = tempo();
  live_.gated = !before(gateUntil_, n_) && gateUntil_ != 0;
  live_.rotDeg = (rot_.ph == Rot::Home) ? 0.f : rot_.theta;
  live_.linM = (lin_.ph == Lin::Home) ? 0.f : (lin_.exc > 0.f ? lin_.exc : 0.f);

  // Progress through the current swing, normalised by the set's typical
  // swing (or a sensible default before there is one).
  float med = nAmps_ ? smallMedian(amps_, nAmps_) : 0.f;
  float pr = 0.f, pl = 0.f;
  float refR = (setCh_ == Channel::Rotation && med > 0.f) ? med :
               (setState_ == Tentative && tent_.ch == Channel::Rotation ? tent_.amp : 110.f);
  float refL = (setCh_ == Channel::Linear && med > 0.f) ? med :
               (setState_ == Tentative && tent_.ch == Channel::Linear ? tent_.amp : 0.30f);
  if (rot_.ph == Rot::Rising)  pr = rot_.theta / refR;
  if (rot_.ph == Rot::Falling) pr = rot_.theta / refR;
  if (lin_.ph != Lin::Home)    pl = live_.linM / refL;
  Channel ch = setCh_;
  if (ch == Channel::None && setState_ == Tentative) ch = tent_.ch;
  if (cfg_.mode != Mode::Auto) ch = exerciseChannel(modeExercise(cfg_.mode));
  float p = (ch == Channel::Rotation) ? pr : (ch == Channel::Linear ? pl : (pr > pl ? pr : pl));
  live_.progress = clampf(p, 0.f, 1.2f);
  live_.moving = (ch == Channel::Linear) ? (lin_.ph == Lin::Rising) :
                 (ch == Channel::Rotation) ? (rot_.ph == Rot::Rising) :
                 (rot_.ph == Rot::Rising || lin_.ph == Lin::Rising);
}

}  // namespace reps
