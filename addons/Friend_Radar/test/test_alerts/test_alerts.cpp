// Host tests: arrival alert cooldown and the shared-animation handshake.
#include <unity.h>
#include <string.h>
#include "fr_alerts.h"
#include "fr_handshake.h"

using namespace fr;

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------
// Alert policy
// ---------------------------------------------------------------------------
static const uint32_t T0 = 800000000u;   // some RTC epoch in 2025

static void test_first_near_sighting_alerts_once() {
  AlertConfig c;
  MateAlertState s;
  TEST_ASSERT_FALSE(alertOnZone(s, Zone::Around, T0, c));
  TEST_ASSERT_FALSE(alertOnZone(s, Zone::Far, T0 + 1, c));
  TEST_ASSERT_TRUE(alertOnZone(s, Zone::Near, T0 + 2, c));
  // Sitting next to us for half an hour: no further alerts.
  for (uint32_t t = T0 + 3; t < T0 + 1800; t += 1)
    TEST_ASSERT_FALSE(alertOnZone(s, (t % 7) ? Zone::Near : Zone::RightHere, t, c));
}

static void test_lost_never_alerts() {
  AlertConfig c;
  MateAlertState s;
  TEST_ASSERT_FALSE(alertOnZone(s, Zone::Lost, T0, c));
  TEST_ASSERT_FALSE(s.nearSeen);
}

static void test_cooldown_blocks_quick_return() {
  AlertConfig c;            // cooldown 600 s, grace 90 s
  MateAlertState s;
  TEST_ASSERT_TRUE(alertOnZone(s, Zone::Near, T0, c));
  // Leaves (not near) and comes back after 3 minutes: arrival, but cooling down.
  TEST_ASSERT_FALSE(alertOnZone(s, Zone::Around, T0 + 60, c));
  TEST_ASSERT_FALSE(alertOnZone(s, Zone::Near, T0 + 180, c));
  TEST_ASSERT_EQUAL_UINT32(420, cooldownLeft(s, T0 + 180, c));
  // Leaves again and returns after the cooldown and the away grace: alert.
  TEST_ASSERT_TRUE(alertOnZone(s, Zone::RightHere, T0 + 180 + 600, c));
  TEST_ASSERT_EQUAL_UINT32(0, cooldownLeft(s, T0 + 180 + 600 + 600, c));
}

static void test_edge_hovering_is_not_an_arrival() {
  AlertConfig c;
  MateAlertState s;
  TEST_ASSERT_TRUE(alertOnZone(s, Zone::Near, T0, c));
  // Flickers out of Near and back every 30 s for an hour, never away > 90 s.
  for (uint32_t t = T0 + 30; t < T0 + 3600; t += 30) {
    TEST_ASSERT_FALSE(alertOnZone(s, Zone::Around, t, c));
    TEST_ASSERT_FALSE(alertOnZone(s, Zone::Near, t + 15, c));
  }
}

static void test_accept_wave_rate_limit_and_suppression() {
  AlertConfig c;
  MateAlertState s;
  TEST_ASSERT_TRUE(acceptWave(s, T0, c));
  TEST_ASSERT_FALSE(acceptWave(s, T0 + 60, c));       // within 2 min
  // Having played along, our own arrival alert does not fire on top.
  TEST_ASSERT_FALSE(alertOnZone(s, Zone::Near, T0 + 5, c));
  TEST_ASSERT_TRUE(acceptWave(s, T0 + 121, c));
}

// ---------------------------------------------------------------------------
// Handshake
// ---------------------------------------------------------------------------
struct Facts : PeerFacts {
  uint32_t mates[4] = {0, 0, 0, 0};
  uint32_t close[4] = {0, 0, 0, 0};
  bool isMate(uint32_t id) const override { for (auto m : mates) if (m == id) return true; return false; }
  bool isClose(uint32_t id) const override { for (auto m : close) if (m == id) return true; return false; }
};

static bool acceptAll(void *, uint32_t) { return true; }
static bool rejectAll(void *, uint32_t) { return false; }

static const uint32_t A = 0x0A0A0A0A, B = 0x0B0B0B0B, C = 0x0C0C0C0C;

static Beacon beaconFrom(Handshake &h, uint32_t id, uint32_t nowMs) {
  Beacon b;
  b.id = id;
  h.fillBeacon(b, nowMs);
  return b;
}

static void test_hello_wave_reaches_mate_in_sync() {
  Handshake ha, hb;
  ha.reset(A); hb.reset(B);
  Facts fa, fb;
  fa.mates[0] = B; fb.mates[0] = A;

  PlayCmd pa = ha.startHello(B, 10000);
  TEST_ASSERT_EQUAL((int)AnimKind::Hello, (int)pa.kind);
  TEST_ASSERT_TRUE(pa.initiator);
  TEST_ASSERT_EQUAL_UINT32(10000 + ha.config().leadMs, pa.startMs);

  // B hears A's beacon 70 ms later.
  Beacon w = beaconFrom(ha, A, 10070);
  TEST_ASSERT_EQUAL((int)EventKind::Wave, (int)w.evKind);
  TEST_ASSERT_EQUAL_HEX16(targetTag(B), w.evTarget);
  PlayCmd pb;
  TEST_ASSERT_TRUE(hb.onBeacon(w, 10070, fb, acceptAll, nullptr, pb));
  TEST_ASSERT_EQUAL((int)AnimKind::Hello, (int)pb.kind);
  TEST_ASSERT_FALSE(pb.initiator);
  TEST_ASSERT_EQUAL_UINT32(pa.seed, pb.seed);                    // same animation
  int skew = (int)pb.startMs - (int)pa.startMs;
  TEST_ASSERT_TRUE(skew >= -100 && skew <= 100);

  // The same wave heard again is not a new event.
  Beacon w2 = beaconFrom(ha, A, 10180);
  TEST_ASSERT_FALSE(hb.onBeacon(w2, 10180, fb, acceptAll, nullptr, pb));
}

static void test_wave_requires_mate_target_and_acceptance() {
  Handshake ha, hb;
  ha.reset(A); hb.reset(B);
  Facts notMate;                                  // B has not added A
  ha.startHello(B, 1000);
  PlayCmd p;
  TEST_ASSERT_FALSE(hb.onBeacon(beaconFrom(ha, A, 1050), 1050, notMate, acceptAll, nullptr, p));

  Handshake hc; hc.reset(C);                      // addressed to B, C is listening
  Facts fc; fc.mates[0] = A;
  TEST_ASSERT_FALSE(hc.onBeacon(beaconFrom(ha, A, 1060), 1060, fc, acceptAll, nullptr, p));

  Handshake hb2; hb2.reset(B);                    // policy says no (rate limit / alerts off)
  Facts fb; fb.mates[0] = A;
  TEST_ASSERT_FALSE(hb2.onBeacon(beaconFrom(ha, A, 1070), 1070, fb, rejectAll, nullptr, p));
}

static void test_late_join_matches_frame() {
  Handshake ha, hb;
  ha.reset(A); hb.reset(B);
  Facts fb; fb.mates[0] = A;
  PlayCmd pa = ha.startHello(B, 50000);
  // B only hears the wave 1.4 s after it went on air (e.g. a background window).
  Beacon w = beaconFrom(ha, A, 51400);
  TEST_ASSERT_EQUAL_UINT8(14, w.evAge);
  PlayCmd pb;
  TEST_ASSERT_TRUE(hb.onBeacon(w, 51400, fb, acceptAll, nullptr, pb));
  // B's t=0 lines up with A's t=0 (to the 100 ms age resolution).
  int skew = (int)pb.startMs - (int)pa.startMs;
  TEST_ASSERT_TRUE(skew >= -100 && skew <= 100);
  TEST_ASSERT_TRUE(pb.startMs < 51400);           // joined mid-animation
}

static void test_shake_pairing_both_orders() {
  for (int order = 0; order < 2; ++order) {
    Handshake ha, hb;
    ha.reset(A); hb.reset(B);
    Facts fa, fb;
    fa.mates[0] = B; fa.close[0] = B;
    fb.mates[0] = A; fb.close[0] = A;
    PlayCmd pa, pb;
    bool aPlays = false, bPlays = false;
    uint32_t t1 = 20000, t2 = 20900;               // 0.9 s apart
    Handshake &first = order ? hb : ha;
    Handshake &second = order ? ha : hb;
    uint32_t firstId = order ? B : A, secondId = order ? A : B;
    Facts &ffirst = order ? fb : fa, &fsecond = order ? fa : fb;
    PlayCmd &pfirst = order ? pb : pa, &psecond = order ? pa : pb;
    bool &playsFirst = order ? bPlays : aPlays, &playsSecond = order ? aPlays : bPlays;

    // First wrist shakes; nobody to pair with yet.
    TEST_ASSERT_FALSE(first.localShake(t1, ffirst, pfirst));
    // Second hears it 60 ms later (records the shake, no pairing yet).
    TEST_ASSERT_FALSE(second.onBeacon(beaconFrom(first, firstId, t1 + 60), t1 + 60, fsecond, acceptAll, nullptr, psecond));
    // Second shakes: pairs immediately with the first.
    playsSecond = second.localShake(t2, fsecond, psecond);
    // First hears the second's beacon 80 ms later and pairs too.
    playsFirst = first.onBeacon(beaconFrom(second, secondId, t2 + 80), t2 + 80, ffirst, acceptAll, nullptr, pfirst);

    TEST_ASSERT_TRUE(aPlays);
    TEST_ASSERT_TRUE(bPlays);
    TEST_ASSERT_EQUAL((int)AnimKind::Celebrate, (int)pa.kind);
    TEST_ASSERT_EQUAL((int)AnimKind::Celebrate, (int)pb.kind);
    TEST_ASSERT_EQUAL_UINT32(pa.seed, pb.seed);
    int skew = (int)pa.startMs - (int)pb.startMs;
    TEST_ASSERT_TRUE(skew >= -100 && skew <= 100);
  }
}

static void test_shakes_too_far_apart_do_not_pair() {
  Handshake ha, hb;
  ha.reset(A); hb.reset(B);
  Facts fa, fb;
  fa.mates[0] = B; fa.close[0] = B;
  fb.mates[0] = A; fb.close[0] = A;
  PlayCmd p;
  ha.localShake(30000, fa, p);
  hb.onBeacon(beaconFrom(ha, A, 30050), 30050, fb, acceptAll, nullptr, p);
  TEST_ASSERT_FALSE(hb.localShake(32000, fb, p));          // 2 s later
  TEST_ASSERT_FALSE(ha.onBeacon(beaconFrom(hb, B, 32050), 32050, fa, acceptAll, nullptr, p));
}

static void test_shake_needs_closeness_on_detecting_side_but_partner_joins() {
  // A thinks B is right here, B's own estimate says only Near. A's celebrate
  // event, addressed to B, still brings B in because B shook recently.
  Handshake ha, hb;
  ha.reset(A); hb.reset(B);
  Facts fa, fb;
  fa.mates[0] = B; fa.close[0] = B;
  fb.mates[0] = A;                                          // not "close" for B
  PlayCmd pa, pb;
  TEST_ASSERT_FALSE(hb.localShake(40000, fb, pb));
  TEST_ASSERT_FALSE(ha.onBeacon(beaconFrom(hb, B, 40070), 40070, fa, acceptAll, nullptr, pa));
  TEST_ASSERT_TRUE(ha.localShake(40500, fa, pa));           // A pairs with B's shake
  // B hears A's shake first (no pairing: not close for B)...
  Beacon fromA = beaconFrom(ha, A, 40560);
  TEST_ASSERT_EQUAL((int)EventKind::Celebrate, (int)fromA.evKind);   // celebrate outranks shake
  TEST_ASSERT_TRUE(hb.onBeacon(fromA, 40560, fb, acceptAll, nullptr, pb));
  TEST_ASSERT_EQUAL((int)AnimKind::Celebrate, (int)pb.kind);
  TEST_ASSERT_EQUAL_UINT32(pa.seed, pb.seed);
}

static void test_celebrate_ignored_without_recent_shake() {
  Handshake ha, hb;
  ha.reset(A); hb.reset(B);
  Facts fa, fb;
  fa.mates[0] = B; fa.close[0] = B;
  fb.mates[0] = A;
  PlayCmd p;
  hb.localShake(1000, fb, p);
  ha.onBeacon(beaconFrom(hb, B, 1050), 1050, fa, acceptAll, nullptr, p);
  TEST_ASSERT_TRUE(ha.localShake(1400, fa, p));
  // A B whose wearer has not shaken recently (a fresh session) must not join.
  Handshake hb2; hb2.reset(B);
  TEST_ASSERT_FALSE(hb2.onBeacon(beaconFrom(ha, A, 1450), 1450, fb, acceptAll, nullptr, p));
}

static void test_no_double_animation_on_simultaneous_hello() {
  Handshake ha, hb;
  ha.reset(A); hb.reset(B);
  Facts fa, fb;
  fa.mates[0] = B; fb.mates[0] = A;
  ha.startHello(B, 5000);
  hb.startHello(A, 5020);                       // both noticed each other
  PlayCmd p;
  TEST_ASSERT_FALSE(hb.onBeacon(beaconFrom(ha, A, 5080), 5080, fb, acceptAll, nullptr, p));
  TEST_ASSERT_FALSE(ha.onBeacon(beaconFrom(hb, B, 5090), 5090, fa, acceptAll, nullptr, p));
}

static void test_independent_hellos_converge_on_earliest_start() {
  // A starts at 5000, B notices A independently 900 ms later. When B hears
  // A's wave it re-aligns to A's start; A ignores B's later wave.
  Handshake ha, hb;
  ha.reset(A); hb.reset(B);
  Facts fa, fb;
  fa.mates[0] = B; fb.mates[0] = A;
  PlayCmd pa = ha.startHello(B, 5000);
  PlayCmd pb = hb.startHello(A, 5900);
  PlayCmd p;
  TEST_ASSERT_TRUE(hb.onBeacon(beaconFrom(ha, A, 5950), 5950, fb, acceptAll, nullptr, p));
  TEST_ASSERT_TRUE(p.resync);
  TEST_ASSERT_EQUAL_UINT32(pb.seed, p.seed);
  int skew = (int)p.startMs - (int)pa.startMs;
  TEST_ASSERT_TRUE(skew >= -100 && skew <= 100);
  TEST_ASSERT_FALSE(ha.onBeacon(beaconFrom(hb, B, 5990), 5990, fa, acceptAll, nullptr, p));
}

static void test_beacon_event_priority_and_expiry() {
  Handshake h;
  h.reset(A);
  Facts f;
  PlayCmd p;
  h.startHello(B, 0);
  Beacon b = beaconFrom(h, A, 100);
  TEST_ASSERT_EQUAL((int)EventKind::Wave, (int)b.evKind);
  uint8_t waveSeq = b.evSeq;
  h.localShake(1000, f, p);
  b = beaconFrom(h, A, 1100);
  TEST_ASSERT_EQUAL((int)EventKind::Shake, (int)b.evKind);   // transient wins
  TEST_ASSERT_EQUAL_UINT16(0, b.evTarget);
  b = beaconFrom(h, A, 4500);                                // shake expired
  TEST_ASSERT_EQUAL((int)EventKind::Wave, (int)b.evKind);
  TEST_ASSERT_EQUAL_UINT8(waveSeq, b.evSeq);                 // same event resumes
  TEST_ASSERT_EQUAL_UINT8(45, b.evAge);
  b = beaconFrom(h, A, 29000);
  TEST_ASSERT_EQUAL_UINT8(255, b.evAge);                     // saturates
  TEST_ASSERT_TRUE(h.eventActive(29000));
  b = beaconFrom(h, A, 31000);
  TEST_ASSERT_EQUAL((int)EventKind::None, (int)b.evKind);
  TEST_ASSERT_FALSE(h.eventActive(31000));
}

static void test_resumed_wave_is_not_a_new_event() {
  Handshake ha, hb;
  ha.reset(A); hb.reset(B);
  Facts fa, fb;
  fb.mates[0] = A;
  PlayCmd p;
  ha.startHello(B, 0);
  TEST_ASSERT_TRUE(hb.onBeacon(beaconFrom(ha, A, 50), 50, fb, acceptAll, nullptr, p));
  ha.localShake(500, fa, p);
  hb.onBeacon(beaconFrom(ha, A, 550), 550, fb, acceptAll, nullptr, p);
  // Shake expires, wave resumes with its old sequence number: ignored.
  TEST_ASSERT_FALSE(hb.onBeacon(beaconFrom(ha, A, 9000), 9000, fb, acceptAll, nullptr, p));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_first_near_sighting_alerts_once);
  RUN_TEST(test_lost_never_alerts);
  RUN_TEST(test_cooldown_blocks_quick_return);
  RUN_TEST(test_edge_hovering_is_not_an_arrival);
  RUN_TEST(test_accept_wave_rate_limit_and_suppression);
  RUN_TEST(test_hello_wave_reaches_mate_in_sync);
  RUN_TEST(test_wave_requires_mate_target_and_acceptance);
  RUN_TEST(test_late_join_matches_frame);
  RUN_TEST(test_shake_pairing_both_orders);
  RUN_TEST(test_shakes_too_far_apart_do_not_pair);
  RUN_TEST(test_shake_needs_closeness_on_detecting_side_but_partner_joins);
  RUN_TEST(test_celebrate_ignored_without_recent_shake);
  RUN_TEST(test_no_double_animation_on_simultaneous_hello);
  RUN_TEST(test_independent_hellos_converge_on_earliest_start);
  RUN_TEST(test_beacon_event_priority_and_expiry);
  RUN_TEST(test_resumed_wave_is_not_a_new_event);
  return UNITY_END();
}
