// Host tests: advertisement encoding / decoding (docs/PROTOCOL.md).
#include <unity.h>
#include <string.h>
#include "fr_beacon.h"

using namespace fr;

void setUp() {}
void tearDown() {}

static Beacon sample() {
  Beacon b;
  b.id = 0x1A2B3C4D;
  b.ref1m = -59;
  b.flags = kFlagCalibrated | kFlagAlerts;
  b.clk = 77;
  b.evKind = EventKind::Wave;
  b.evSeq = 42;
  b.evTarget = 0xBEEF;
  b.evAge = 13;
  strcpy(b.name, "Kyle");
  return b;
}

static void test_roundtrip_all_fields() {
  uint8_t buf[31];
  Beacon b = sample();
  size_t n = encodeAdv(b, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_UINT32(4 + 15 + 4, n);
  Beacon d;
  TEST_ASSERT_TRUE(decodeAdv(buf, n, d));
  TEST_ASSERT_EQUAL_HEX32(b.id, d.id);
  TEST_ASSERT_EQUAL_INT8(-59, d.ref1m);
  TEST_ASSERT_EQUAL_HEX8(b.flags, d.flags);
  TEST_ASSERT_EQUAL_UINT8(77, d.clk);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)EventKind::Wave, (uint8_t)d.evKind);
  TEST_ASSERT_EQUAL_UINT8(42, d.evSeq);
  TEST_ASSERT_EQUAL_HEX16(0xBEEF, d.evTarget);
  TEST_ASSERT_EQUAL_UINT8(13, d.evAge);
  TEST_ASSERT_EQUAL_STRING("Kyle", d.name);
}

// The exact bytes documented in docs/PROTOCOL.md. If this test changes, the
// protocol document must change with it.
static void test_known_answer_vector() {
  const uint8_t expect[] = {
    0x16, 0xFF, 0xFF, 0xFF,                // AD len 22, type MSD, company 0xFFFF
    'E', 'W', 'R', 0x10,                   // magic, version 1.0
    0x4D, 0x3C, 0x2B, 0x1A,                // id 0x1A2B3C4D (little endian)
    0xC5,                                  // ref1m -59 dBm
    0x06,                                  // flags: calibrated | alerts
    0x4D,                                  // clk 77
    0xA9,                                  // seq 42 << 2 | kind 1 (wave)
    0xEF, 0xBE,                            // target tag 0xBEEF
    0x0D,                                  // age 1.3 s
    'K', 'y', 'l', 'e',
  };
  uint8_t buf[31];
  size_t n = encodeAdv(sample(), buf, sizeof(buf));
  TEST_ASSERT_EQUAL_UINT32(sizeof(expect), n);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expect, buf, sizeof(expect));
}

static void test_max_name_fills_31_bytes() {
  Beacon b = sample();
  strcpy(b.name, "ABCDEFGHIJKL");               // 12 chars
  uint8_t buf[40];
  size_t n = encodeAdv(b, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_UINT32(31, n);
  Beacon d;
  TEST_ASSERT_TRUE(decodeAdv(buf, n, d));
  TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJKL", d.name);
}

static void test_encode_rejects_small_buffer_and_bad_id() {
  uint8_t buf[31];
  Beacon b = sample();
  TEST_ASSERT_EQUAL_UINT32(0, encodeAdv(b, buf, 10));
  b.id = 0;
  TEST_ASSERT_EQUAL_UINT32(0, encodeAdv(b, buf, sizeof(buf)));
  b.id = 0xFFFFFFFF;
  TEST_ASSERT_EQUAL_UINT32(0, encodeAdv(b, buf, sizeof(buf)));
}

static void test_empty_name_ok() {
  Beacon b = sample();
  b.name[0] = '\0';
  uint8_t buf[31];
  size_t n = encodeAdv(b, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_UINT32(19, n);
  Beacon d;
  TEST_ASSERT_TRUE(decodeAdv(buf, n, d));
  TEST_ASSERT_EQUAL_STRING("", d.name);
}

static void test_decode_finds_msd_after_other_structures() {
  uint8_t enc[31];
  size_t n = encodeAdv(sample(), enc, sizeof(enc));
  uint8_t adv[64];
  const uint8_t pre[] = { 0x02, 0x01, 0x06,          // flags
                          0x03, 0x09, 'h', 'i',       // complete name
                          0x05, 0xFF, 0x4C, 0x00, 1, 2 };  // someone else's MSD
  memcpy(adv, pre, sizeof(pre));
  memcpy(adv + sizeof(pre), enc, n);
  Beacon d;
  TEST_ASSERT_TRUE(decodeAdv(adv, sizeof(pre) + n, d));
  TEST_ASSERT_EQUAL_HEX32(0x1A2B3C4D, d.id);
}

static void test_decode_rejects_malformed() {
  uint8_t buf[31];
  size_t n = encodeAdv(sample(), buf, sizeof(buf));
  Beacon d;
  uint8_t t[31];

  memcpy(t, buf, n); t[2] = 0x4C; t[3] = 0x00;                 // other company
  TEST_ASSERT_FALSE(decodeAdv(t, n, d));
  memcpy(t, buf, n); t[5] = 'X';                               // magic
  TEST_ASSERT_FALSE(decodeAdv(t, n, d));
  memcpy(t, buf, n); t[7] = 0x20;                              // major version 2
  TEST_ASSERT_FALSE(decodeAdv(t, n, d));
  memcpy(t, buf, n); t[8] = t[9] = t[10] = t[11] = 0;          // id 0
  TEST_ASSERT_FALSE(decodeAdv(t, n, d));
  memcpy(t, buf, n); t[8] = t[9] = t[10] = t[11] = 0xFF;       // id all ones
  TEST_ASSERT_FALSE(decodeAdv(t, n, d));
  memcpy(t, buf, n); t[12] = (uint8_t)(int8_t)-10;             // ref out of range
  TEST_ASSERT_FALSE(decodeAdv(t, n, d));
  memcpy(t, buf, n); t[14] = 120;                              // clk out of range
  TEST_ASSERT_FALSE(decodeAdv(t, n, d));
  TEST_ASSERT_FALSE(decodeAdv(buf, n - 6, d));                 // AD length overruns
  memcpy(t, buf, n); t[0] = 2 + 3 + 10;                        // too short for fixed part
  TEST_ASSERT_FALSE(decodeAdv(t, 1 + t[0], d));
  TEST_ASSERT_FALSE(decodeAdv(nullptr, 10, d));
  TEST_ASSERT_FALSE(decodeAdv(buf, 0, d));
}

static void test_decode_accepts_minor_version_and_nul_padding() {
  uint8_t buf[31];
  size_t n = encodeAdv(sample(), buf, sizeof(buf));
  buf[7] = 0x13;                                               // version 1.3
  buf[n] = 0;                                                  // NUL after name
  buf[0] += 1;
  Beacon d;
  TEST_ASSERT_TRUE(decodeAdv(buf, n + 1, d));
  TEST_ASSERT_EQUAL_STRING("Kyle", d.name);
}

static void test_decode_drops_non_printable_name_bytes() {
  uint8_t buf[31];
  size_t n = encodeAdv(sample(), buf, sizeof(buf));
  buf[n - 2] = 0x07;                                           // "Ky\x07e"
  Beacon d;
  TEST_ASSERT_TRUE(decodeAdv(buf, n, d));
  TEST_ASSERT_EQUAL_STRING("Kye", d.name);
}

static void test_sanitize_name() {
  char out[kMaxName + 1];
  TEST_ASSERT_EQUAL_UINT32(4, sanitizeName("  Kyle  ", out));
  TEST_ASSERT_EQUAL_STRING("Kyle", out);
  sanitizeName("Ana   Lu", out);
  TEST_ASSERT_EQUAL_STRING("Ana Lu", out);
  sanitizeName("This name is far too long", out);
  TEST_ASSERT_EQUAL_STRING("This name is", out);
  sanitizeName("caf\xc3\xa9!", out);
  TEST_ASSERT_EQUAL_STRING("caf!", out);
  TEST_ASSERT_EQUAL_UINT32(0, sanitizeName("   ", out));
  TEST_ASSERT_EQUAL_UINT32(0, sanitizeName(nullptr, out));
  // A space is never left dangling at the 12-char limit.
  sanitizeName("ABCDEFGHIJK LMNOP", out);
  TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJK", out);
}

static void test_default_name_and_hash_helpers() {
  char out[kMaxName + 1];
  defaultName(0x12343F2A, out);
  TEST_ASSERT_EQUAL_STRING("EWatch 3F2A", out);
  TEST_ASSERT_EQUAL_UINT32(pairSeed(5, 9, 1), pairSeed(9, 5, 1));
  TEST_ASSERT_NOT_EQUAL(pairSeed(5, 9, 1), pairSeed(5, 9, 2));
  TEST_ASSERT_NOT_EQUAL(pairSeed(5, 9, 1), pairSeed(5, 10, 1));
  // Tags are never 0 (0 = untargeted) and structured ids do not collide.
  for (uint32_t id = 1; id < 200000; id += 13) TEST_ASSERT_NOT_EQUAL(0, targetTag(id));
  TEST_ASSERT_NOT_EQUAL(targetTag(0x0B0B0B0B), targetTag(0x0C0C0C0C));
  TEST_ASSERT_NOT_EQUAL(targetTag(0x00010001), targetTag(0x00020002));
  TEST_ASSERT_EQUAL_HEX16(targetTag(0x1234), targetTag(0x1234));
  for (uint32_t id = 1; id < 2000; id += 7) {
    uint16_t a = blipAngleDeg(id);
    TEST_ASSERT_TRUE(a < 360);
    // Never in the engraved zone-label sector at the bottom of the dial.
    TEST_ASSERT_TRUE(a < 180 - kBlipKeepOutHalfDeg || a > 180 + kBlipKeepOutHalfDeg);
  }
  TEST_ASSERT_EQUAL_UINT16(blipAngleDeg(0xCAFE), blipAngleDeg(0xCAFE));
}

static void test_zone_names() {
  TEST_ASSERT_EQUAL_STRING("Right here", zoneName(Zone::RightHere));
  TEST_ASSERT_EQUAL_STRING("Lost", zoneName(Zone::Lost));
  TEST_ASSERT_EQUAL_STRING("near", zoneShort(Zone::Near));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_roundtrip_all_fields);
  RUN_TEST(test_known_answer_vector);
  RUN_TEST(test_max_name_fills_31_bytes);
  RUN_TEST(test_encode_rejects_small_buffer_and_bad_id);
  RUN_TEST(test_empty_name_ok);
  RUN_TEST(test_decode_finds_msd_after_other_structures);
  RUN_TEST(test_decode_rejects_malformed);
  RUN_TEST(test_decode_accepts_minor_version_and_nul_padding);
  RUN_TEST(test_decode_drops_non_printable_name_bytes);
  RUN_TEST(test_sanitize_name);
  RUN_TEST(test_default_name_and_hash_helpers);
  RUN_TEST(test_zone_names);
  return UNITY_END();
}
