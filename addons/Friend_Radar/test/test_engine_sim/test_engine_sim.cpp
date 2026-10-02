// Host tests: two complete RadarEngines talking through the real beacon codec
// over a simulated radio (path-loss RSSI + noise, latency, packet loss).
// Verifies the behaviour the user sees: one alert per arrival, cooldown,
// identical shared animations started at (nearly) the same moment on both
// watches, bump-to-celebrate, and rendezvous clock convergence.
#include <unity.h>
#include <math.h>
#include <string.h>
#include <vector>
#include <algorithm>
#include "fr_engine.h"

using namespace fr;

void setUp() {}
void tearDown() {}

static uint32_t rng = 1;
static float urand() { rng = rng * 1664525u + 1013904223u; return (float)(rng >> 8) / 16777216.f; }
static float gauss() { float s = 0; for (int i = 0; i < 12; ++i) s += urand(); return s - 6.f; }

struct Watch {
  RadarEngine eng;
  uint32_t id = 0;
  float x = 0, y = 0;
  int32_t clkOffsetSec = 0;          // radar clock = sim seconds + offset
  uint32_t nextSendMs = 0;
  std::vector<PlayCmd> plays;
  std::vector<uint32_t> playAtMs;    // sim time the play command was issued
  int buzzHello = 0, buzzCeleb = 0, clockAdjusts = 0;
  float distAtFirstHello = -1;
};

struct Packet { uint32_t at; int to; uint8_t data[31]; size_t len; int rssi; };

struct Sim {
  Watch w[2];
  std::vector<Packet> air;
  uint32_t now = 0;
  float noiseDb = 4.f, loss = 0.1f;
  const uint32_t epoch0 = 800000000u;

  float dist() const { return hypotf(w[0].x - w[1].x, w[0].y - w[1].y); }
  uint8_t clk(const Watch &v) const { return (uint8_t)(((int64_t)now / 1000 + v.clkOffsetSec + 1200000) % 120); }

  void apply(Watch &v, const Effects &fx) {
    if (fx.play.kind != AnimKind::None) {
      v.plays.push_back(fx.play);
      v.playAtMs.push_back(now);
      if (fx.play.kind == AnimKind::Hello && v.distAtFirstHello < 0) v.distAtFirstHello = dist();
    }
    if (fx.buzzHello) ++v.buzzHello;
    if (fx.buzzCelebrate) ++v.buzzCeleb;
    if (fx.clockAdjust) { v.clkOffsetSec += fx.clockDeltaSec; ++v.clockAdjusts; }
  }

  void step() {
    // Deliveries due now.
    for (size_t i = 0; i < air.size();) {
      if (air[i].at <= now) {
        Packet p = air[i];
        air.erase(air.begin() + (long)i);
        Beacon b;
        if (decodeAdv(p.data, p.len, b)) {
          Effects fx;
          Watch &rx = w[p.to];
          rx.eng.onBeacon(b, p.rssi, now, epoch0 + now / 1000, clk(rx), fx);
          apply(rx, fx);
        }
      } else {
        ++i;
      }
    }
    // Transmissions: one advertising event every ~100 ms per watch.
    for (int s = 0; s < 2; ++s) {
      Watch &tx = w[s];
      if (now < tx.nextSendMs) continue;
      tx.nextSendMs = now + 100 + (uint32_t)(urand() * 10.f);
      if (urand() < loss) continue;
      Beacon b;
      tx.eng.buildBeacon(b, now, clk(tx));
      Packet p;
      p.len = encodeAdv(b, p.data, sizeof(p.data));
      TEST_ASSERT_TRUE(p.len > 0);
      p.to = 1 - s;
      p.at = now + 2 + (uint32_t)(urand() * 105.f);       // reception latency
      float d = std::max(dist(), 0.15f);
      float rssi = b.ref1m - 10.f * 2.5f * log10f(d) + noiseDb * gauss();
      p.rssi = (int)lroundf(std::min(rssi, -20.f));
      air.push_back(p);
    }
    // Periodic housekeeping.
    if (now % 50 == 0) {
      for (auto &v : w) { Effects fx; v.eng.tick(now, epoch0 + now / 1000, fx); apply(v, fx); }
    }
    ++now;
  }

  void run(uint32_t ms) { uint32_t end = now + ms; while (now < end) step(); }
  void runWalk(int who, float toX, float speed) {
    while (fabsf(w[who].x - toX) > 0.01f) {
      float dir = toX > w[who].x ? 1.f : -1.f;
      w[who].x += dir * std::min(speed * 0.001f, fabsf(toX - w[who].x));
      step();
    }
  }
};

static const uint32_t IDA = 0x1111AAAA, IDB = 0x7777BBBB;

static void setupPair(Sim &s, bool aHasB, bool bHasA) {
  s.w[0].id = IDA; s.w[1].id = IDB;
  s.w[0].eng.begin(IDA); s.w[1].eng.begin(IDB);
  s.w[0].eng.setName("Ana"); s.w[1].eng.setName("Ben");
  if (aHasB) s.w[0].eng.addMate(IDB, "Ben", s.epoch0);
  if (bHasA) s.w[1].eng.addMate(IDA, "Ana", s.epoch0);
  s.w[0].x = 0; s.w[1].x = 25;
}

static void test_walk_up_one_shared_hello() {
  Sim s; rng = 11;
  setupPair(s, true, true);
  s.run(3000);                                   // far apart: nothing happens
  TEST_ASSERT_EQUAL(0, (int)s.w[0].plays.size());
  s.runWalk(1, 1.2f, 1.2f);                      // Ben walks up to 1.2 m
  s.run(10000);

  for (int i = 0; i < 2; ++i) {
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)s.w[i].plays.size(), "exactly one animation per watch");
    TEST_ASSERT_EQUAL(1, s.w[i].buzzHello);
    TEST_ASSERT_EQUAL((int)AnimKind::Hello, (int)s.w[i].plays[0].kind);
  }
  const PlayCmd &a = s.w[0].plays[0], &b = s.w[1].plays[0];
  TEST_ASSERT_EQUAL_UINT32(a.seed, b.seed);                        // same frames
  TEST_ASSERT_EQUAL_UINT32(IDB, a.peerId);
  TEST_ASSERT_EQUAL_UINT32(IDA, b.peerId);
  TEST_ASSERT_TRUE(a.initiator != b.initiator);                   // one waved, one joined
  int skew = (int)a.startMs - (int)b.startMs;
  TEST_ASSERT_TRUE_MESSAGE(skew >= -150 && skew <= 150, "animations start together");
  float d = s.w[0].distAtFirstHello;
  TEST_ASSERT_TRUE_MESSAGE(d > 0 && d < 5.f, "alert only once the mate is near");

  PeerView pv[4];
  int n = s.w[0].eng.snapshot(pv, 4, s.now);
  TEST_ASSERT_EQUAL(1, n);
  TEST_ASSERT_TRUE(pv[0].mate);
  TEST_ASSERT_EQUAL_STRING("Ben", pv[0].label);
  TEST_ASSERT_TRUE(pv[0].zone == Zone::Near || pv[0].zone == Zone::RightHere);
}

static void test_cooldown_across_comings_and_goings() {
  Sim s; rng = 22;
  setupPair(s, true, true);
  s.runWalk(1, 1.0f, 1.5f);
  s.run(5000);
  TEST_ASSERT_EQUAL(1, (int)s.w[0].plays.size());
  // Leaves for 3 minutes and comes back: still cooling down, no alert.
  s.runWalk(1, 30.f, 1.5f);
  s.run(150000);
  s.runWalk(1, 1.0f, 1.5f);
  s.run(10000);
  TEST_ASSERT_EQUAL(1, (int)s.w[0].plays.size());
  TEST_ASSERT_EQUAL(1, (int)s.w[1].plays.size());
  // Leaves again; back well after the 10 minute cooldown: one more alert each.
  s.runWalk(1, 30.f, 1.5f);
  s.run(420000);
  s.runWalk(1, 1.0f, 1.5f);
  s.run(10000);
  TEST_ASSERT_EQUAL(2, (int)s.w[0].plays.size());
  TEST_ASSERT_EQUAL(2, (int)s.w[1].plays.size());
  TEST_ASSERT_EQUAL_UINT32(s.w[0].plays[1].seed, s.w[1].plays[1].seed);
}

static void test_one_sided_mate_only_buzzes_that_side() {
  Sim s; rng = 33;
  setupPair(s, true, false);                     // Ana added Ben; Ben did not add Ana
  s.runWalk(1, 1.2f, 1.2f);
  s.run(8000);
  TEST_ASSERT_EQUAL(1, (int)s.w[0].plays.size());
  TEST_ASSERT_EQUAL(0, (int)s.w[1].plays.size()); // strangers cannot make Ben's watch buzz
  TEST_ASSERT_EQUAL(0, s.w[1].buzzHello);
}

static void test_alerts_disabled_means_silence() {
  Sim s; rng = 44;
  setupPair(s, true, true);
  s.w[0].eng.config().alertsEnabled = false;
  s.w[1].eng.config().alertsEnabled = false;
  s.runWalk(1, 1.0f, 1.2f);
  s.run(8000);
  TEST_ASSERT_EQUAL(0, (int)s.w[0].plays.size());
  TEST_ASSERT_EQUAL(0, (int)s.w[1].plays.size());
}

static void test_bump_to_celebrate() {
  Sim s; rng = 55;
  setupPair(s, true, true);
  s.runWalk(1, 0.35f, 1.0f);
  s.run(12000);                                  // hello plays, zones settle
  size_t base0 = s.w[0].plays.size(), base1 = s.w[1].plays.size();
  { Effects fx; s.w[0].eng.onLocalShake(s.now, fx); s.apply(s.w[0], fx); }
  s.run(700);
  { Effects fx; s.w[1].eng.onLocalShake(s.now, fx); s.apply(s.w[1], fx); }
  s.run(1500);
  TEST_ASSERT_EQUAL(base0 + 1, s.w[0].plays.size());
  TEST_ASSERT_EQUAL(base1 + 1, s.w[1].plays.size());
  const PlayCmd &a = s.w[0].plays.back(), &b = s.w[1].plays.back();
  TEST_ASSERT_EQUAL((int)AnimKind::Celebrate, (int)a.kind);
  TEST_ASSERT_EQUAL((int)AnimKind::Celebrate, (int)b.kind);
  TEST_ASSERT_EQUAL_UINT32(a.seed, b.seed);
  int skew = (int)a.startMs - (int)b.startMs;
  TEST_ASSERT_TRUE(skew >= -150 && skew <= 150);
  TEST_ASSERT_EQUAL(1, s.w[0].buzzCeleb);
  TEST_ASSERT_EQUAL(1, s.w[1].buzzCeleb);
}

static void test_shakes_at_a_distance_do_not_celebrate() {
  Sim s; rng = 66;
  setupPair(s, true, true);
  s.runWalk(1, 2.5f, 1.0f);                       // Near, not right here
  s.run(10000);
  size_t base0 = s.w[0].plays.size(), base1 = s.w[1].plays.size();
  { Effects fx; s.w[0].eng.onLocalShake(s.now, fx); s.apply(s.w[0], fx); }
  s.run(500);
  { Effects fx; s.w[1].eng.onLocalShake(s.now, fx); s.apply(s.w[1], fx); }
  s.run(2000);
  TEST_ASSERT_EQUAL(base0, s.w[0].plays.size());
  TEST_ASSERT_EQUAL(base1, s.w[1].plays.size());
}

static void test_rendezvous_clock_converges_to_lower_id() {
  Sim s; rng = 77;
  setupPair(s, true, true);
  s.w[1].clkOffsetSec = 9;                       // Ben's RTC is 9 s off
  s.w[1].x = 6.f;
  s.run(20000);
  TEST_ASSERT_EQUAL(0, s.w[0].clockAdjusts);     // Ana (lower id) never moves
  TEST_ASSERT_TRUE(s.w[1].clockAdjusts >= 1);
  int d = clkDelta(s.clk(s.w[0]), s.clk(s.w[1]));
  TEST_ASSERT_TRUE(d >= -1 && d <= 1);
}

static void test_fresh_session_hears_wave_already_on_air() {
  // Models a background window: Ben's engine starts fresh while Ana's wave
  // (sent because she spotted Ben's background beacon) is already on air.
  Sim s; rng = 88;
  setupPair(s, true, true);
  s.w[1].x = 1.0f;
  s.run(4000);
  TEST_ASSERT_EQUAL(1, (int)s.w[1].plays.size());
  // Ben's "next wake": a new engine, mates restored, wave state restored.
  MateAlertEntry saved[kMaxMates];
  int n = s.w[1].eng.exportAlerts(saved, kMaxMates);
  TEST_ASSERT_EQUAL(1, n);
  s.w[1].eng.begin(IDB);
  s.w[1].eng.importAlerts(saved, n);
  s.run(4000);
  // Already greeted (alert state survived the restart): no repeat.
  TEST_ASSERT_EQUAL(1, (int)s.w[1].plays.size());
}

static void test_snapshot_and_strongest_peer() {
  Sim s; rng = 99;
  setupPair(s, false, false);
  s.w[1].x = 4.f;
  s.run(3000);
  uint32_t id = 0;
  TEST_ASSERT_TRUE(s.w[0].eng.strongestPeer(s.now, id));
  TEST_ASSERT_EQUAL_HEX32(IDB, id);
  PeerView pv[2];
  TEST_ASSERT_EQUAL(1, s.w[0].eng.snapshot(pv, 2, s.now));
  TEST_ASSERT_FALSE(pv[0].mate);
  TEST_ASSERT_EQUAL_STRING("Ben", pv[0].label);
  TEST_ASSERT_TRUE(pv[0].samples > 10);
  TEST_ASSERT_TRUE(pv[0].distM > 1.5f && pv[0].distM < 12.f);
  // Ben goes quiet: Lost after 10 s, gone after 60 s.
  s.w[1].x = 500.f;                              // out of range: drop all packets
  s.loss = 1.0f;
  s.run(11000);
  TEST_ASSERT_EQUAL(1, s.w[0].eng.snapshot(pv, 2, s.now));
  TEST_ASSERT_EQUAL((int)Zone::Lost, (int)pv[0].zone);
  TEST_ASSERT_EQUAL(0, s.w[0].eng.liveCount(s.now));
  s.run(60000);
  TEST_ASSERT_EQUAL(0, s.w[0].eng.snapshot(pv, 2, s.now));
}

static void test_calibration_capture() {
  Sim s; rng = 123;
  setupPair(s, false, false);
  s.w[1].x = 1.0f;
  s.noiseDb = 2.f;
  s.w[0].eng.calStart(IDB);
  s.run(6000);
  TEST_ASSERT_TRUE(s.w[0].eng.calCount() > 30);
  CalResult r = calibrateFromSamples(s.w[0].eng.calSamples(), s.w[0].eng.calCount());
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_TRUE(r.ref1m >= -62 && r.ref1m <= -58);   // true value -60 at 1 m
  s.w[0].eng.calStop();
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_walk_up_one_shared_hello);
  RUN_TEST(test_cooldown_across_comings_and_goings);
  RUN_TEST(test_one_sided_mate_only_buzzes_that_side);
  RUN_TEST(test_alerts_disabled_means_silence);
  RUN_TEST(test_bump_to_celebrate);
  RUN_TEST(test_shakes_at_a_distance_do_not_celebrate);
  RUN_TEST(test_rendezvous_clock_converges_to_lower_id);
  RUN_TEST(test_fresh_session_hears_wave_already_on_air);
  RUN_TEST(test_snapshot_and_strongest_peer);
  RUN_TEST(test_calibration_capture);
  return UNITY_END();
}
