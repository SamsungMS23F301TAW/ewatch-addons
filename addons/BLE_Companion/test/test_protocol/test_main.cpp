// Host unit tests for the EWatch Companion wire protocol and face slots.
//
//   ~/.platformio/penv/bin/pio test -d addons/BLE_Companion -e native
//
// Every case in test/vectors/protocol_vectors.json is checked here AND by the
// JavaScript suite (test/web/codec.test.mjs), so the firmware and the web page
// are held to the same bytes. Exhaustive property checks follow the vectors.
#include <unity.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "ble_proto.h"
#include "face_slots.h"
#include "json_mini.h"

using namespace bleproto;

static jm::Value V;   // the parsed vector file

static std::string vectorsPath() {
  std::string f = __FILE__;                       // .../test/test_protocol/test_main.cpp
  size_t cut = f.rfind("test_protocol");
  std::string base = (cut == std::string::npos) ? std::string("test/") : f.substr(0, cut);
  return base + "vectors/protocol_vectors.json";
}

static bool loadVectors() {
  std::ifstream in(vectorsPath().c_str(), std::ios::binary);
  if (!in) return false;
  std::stringstream ss;
  ss << in.rdbuf();
  std::string text = ss.str();
  V = jm::Parser(text).parse();
  return V.type == jm::Value::Object;
}

static char msgBuf[256];
static const char *ctx(const char *section, size_t i) {
  snprintf(msgBuf, sizeof(msgBuf), "%s[%zu]", section, i);
  return msgBuf;
}

#define EXPECT_HEX(expected, buf, n, m) \
  TEST_ASSERT_EQUAL_STRING_MESSAGE((expected).c_str(), jm::tohex((buf), (n)).c_str(), (m))

// ---------------------------------------------------------------------------
// Vectors
// ---------------------------------------------------------------------------
static void test_rgb(void) {
  const jm::Value &a = V["rgb"];
  TEST_ASSERT_TRUE(a.size() > 10);
  for (size_t i = 0; i < a.size(); i++) {
    const std::string &rgb = a[i]["rgb"].s;
    uint8_t r = (uint8_t)strtoul(rgb.substr(1, 2).c_str(), nullptr, 16);
    uint8_t g = (uint8_t)strtoul(rgb.substr(3, 2).c_str(), nullptr, 16);
    uint8_t b = (uint8_t)strtoul(rgb.substr(5, 2).c_str(), nullptr, 16);
    uint16_t c = rgb565FromRgb888(r, g, b);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(a[i]["rgb565"].u(), c, ctx("rgb", i));
    uint8_t rr, gg, bb;
    rgb888FromRgb565(c, rr, gg, bb);
    char back[8];
    snprintf(back, sizeof(back), "#%02x%02x%02x", rr, gg, bb);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(a[i]["back"].s.c_str(), back, ctx("rgb", i));
  }
}

static void test_theme(void) {
  const jm::Value &a = V["theme"];
  for (size_t i = 0; i < a.size(); i++) {
    std::vector<uint8_t> in = jm::unhex(a[i]["hex"].s);
    Theme t{};
    Result r = decodeTheme(in.data(), in.size(), t);
    if (a[i].has("error")) {
      TEST_ASSERT_EQUAL_MESSAGE(a[i]["error"].i(), r, ctx("theme", i));
      continue;
    }
    TEST_ASSERT_EQUAL_MESSAGE(RES_OK, r, ctx("theme", i));
    TEST_ASSERT_EQUAL_UINT16(a[i]["bg"].u(), t.bg);
    TEST_ASSERT_EQUAL_UINT16(a[i]["fg"].u(), t.fg);
    TEST_ASSERT_EQUAL_UINT16(a[i]["accent"].u(), t.accent);
    TEST_ASSERT_EQUAL_UINT16(a[i]["line"].u(), t.line);
    uint8_t out[kThemeLen];
    size_t n = encodeTheme(t, out);
    EXPECT_HEX(a[i]["hex"].s, out, n, ctx("theme", i));
  }
}

static void test_brightness(void) {
  const jm::Value &a = V["brightness"];
  for (size_t i = 0; i < a.size(); i++) {
    std::vector<uint8_t> in = jm::unhex(a[i]["hex"].s);
    uint8_t v = 0;
    Result r = decodeBrightness(in.data(), in.size(), v);
    if (a[i].has("error")) { TEST_ASSERT_EQUAL_MESSAGE(a[i]["error"].i(), r, ctx("brightness", i)); continue; }
    TEST_ASSERT_EQUAL_MESSAGE(RES_OK, r, ctx("brightness", i));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(a[i]["value"].u(), v, ctx("brightness", i));
  }
}

static void test_face(void) {
  const jm::Value &a = V["face"];
  for (size_t i = 0; i < a.size(); i++) {
    std::vector<uint8_t> in = jm::unhex(a[i]["hex"].s);
    FaceCfg f{};
    Result r = decodeFace(in.data(), in.size(), (uint8_t)a[i]["styleCount"].u(), f);
    if (a[i].has("error")) { TEST_ASSERT_EQUAL_MESSAGE(a[i]["error"].i(), r, ctx("face", i)); continue; }
    TEST_ASSERT_EQUAL_MESSAGE(RES_OK, r, ctx("face", i));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(a[i]["style"].u(), f.style, ctx("face", i));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(a[i]["options"].u(), f.options, ctx("face", i));
  }
}

static void test_slots(void) {
  const jm::Value &a = V["slots"];
  for (size_t i = 0; i < a.size(); i++) {
    std::vector<uint8_t> in = jm::unhex(a[i]["hex"].s);
    SlotCfg s[kSlotCount];
    memset(s, 0xAA, sizeof(s));
    Result r = decodeSlots(in.data(), in.size(), s);
    if (a[i].has("error")) {
      TEST_ASSERT_EQUAL_MESSAGE(a[i]["error"].i(), r, ctx("slots", i));
      TEST_ASSERT_EQUAL_UINT8_MESSAGE(0xAA, s[0].source, "decoder must not touch output on error");
      continue;
    }
    TEST_ASSERT_EQUAL_MESSAGE(RES_OK, r, ctx("slots", i));
    for (uint8_t k = 0; k < kSlotCount; k++) {
      const jm::Value &e = a[i]["slots"][k];
      TEST_ASSERT_EQUAL_UINT8_MESSAGE(e[0].u(), s[k].source, ctx("slots", i));
      TEST_ASSERT_EQUAL_UINT8_MESSAGE(e[1].u(), s[k].arg, ctx("slots", i));
      TEST_ASSERT_EQUAL_UINT8_MESSAGE(e[2].u(), s[k].color, ctx("slots", i));
      TEST_ASSERT_EQUAL_UINT8_MESSAGE(e[3].u(), s[k].flags, ctx("slots", i));
    }
  }
}

static void test_text_write(void) {
  const jm::Value &a = V["textWrite"];
  for (size_t i = 0; i < a.size(); i++) {
    std::vector<uint8_t> in = jm::unhex(a[i]["hex"].s);
    uint8_t idx = 0xFF;
    TextEntry t{};
    Result r = decodeTextWrite(in.data(), in.size(), idx, t);
    if (a[i].has("error")) { TEST_ASSERT_EQUAL_MESSAGE(a[i]["error"].i(), r, ctx("textWrite", i)); continue; }
    TEST_ASSERT_EQUAL_MESSAGE(RES_OK, r, ctx("textWrite", i));
    TEST_ASSERT_EQUAL_UINT8(a[i]["index"].u(), idx);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(a[i]["text"].s.c_str(), t.text, ctx("textWrite", i));
    TEST_ASSERT_EQUAL_UINT8(a[i]["text"].s.size(), t.len);
    uint8_t out[kTextWriteMax];
    size_t n = encodeTextWrite(idx, t.text, t.len, out);
    EXPECT_HEX(a[i]["hex"].s, out, n, ctx("textWrite", i));
  }
}

static void test_texts_read(void) {
  const jm::Value &a = V["textsRead"];
  for (size_t i = 0; i < a.size(); i++) {
    TextEntry t[kTextCount];
    memset(t, 0, sizeof(t));
    for (uint8_t k = 0; k < kTextCount; k++) {
      const std::string &s = a[i]["texts"][k].s;
      t[k].len = (uint8_t)s.size();
      memcpy(t[k].text, s.data(), s.size());
    }
    uint8_t out[kTextsReadMax];
    size_t n = encodeTextsRead(t, out);
    EXPECT_HEX(a[i]["hex"].s, out, n, ctx("textsRead", i));
  }
}

static void fillFeed(const jm::Value &e, FeedEntry &f) {
  memset(&f, 0, sizeof(f));
  f.icon = (uint8_t)e["icon"].u();
  f.kind = (uint8_t)e["kind"].u();
  f.expiresAt = (uint32_t)e["expiresAt"].n;
  f.targetAt = (uint32_t)e["targetAt"].n;
  f.len = (uint8_t)e["text"].s.size();
  memcpy(f.text, e["text"].s.data(), f.len);
}

static void test_feed_write(void) {
  const jm::Value &a = V["feedWrite"];
  for (size_t i = 0; i < a.size(); i++) {
    std::vector<uint8_t> in = jm::unhex(a[i]["hex"].s);
    uint8_t idx = 0xFF;
    FeedEntry f{};
    Result r = decodeFeedWrite(in.data(), in.size(), idx, f);
    if (a[i].has("error")) { TEST_ASSERT_EQUAL_MESSAGE(a[i]["error"].i(), r, ctx("feedWrite", i)); continue; }
    TEST_ASSERT_EQUAL_MESSAGE(RES_OK, r, ctx("feedWrite", i));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(a[i]["index"].u(), idx, ctx("feedWrite", i));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(a[i]["icon"].u(), f.icon, ctx("feedWrite", i));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(a[i]["kind"].u(), f.kind, ctx("feedWrite", i));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)a[i]["expiresAt"].n, f.expiresAt, ctx("feedWrite", i));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)a[i]["targetAt"].n, f.targetAt, ctx("feedWrite", i));
    TEST_ASSERT_EQUAL_STRING_MESSAGE(a[i]["text"].s.c_str(), f.text, ctx("feedWrite", i));
    // Re-encode from the expected fields (not the decoded ones) to check the
    // encoder independently; skip the one vector whose targetAt is normalised.
    if (a[i]["targetAt"].n == 0 && in.size() >= kFeedWriteMin && getU32(in.data() + 7) != 0) continue;
    FeedEntry e;
    fillFeed(a[i], e);
    uint8_t out[kFeedWriteMax];
    size_t n = encodeFeedWrite(idx, e, out);
    EXPECT_HEX(a[i]["hex"].s, out, n, ctx("feedWrite", i));
  }
}

static void test_feeds_read(void) {
  const jm::Value &a = V["feedsRead"];
  for (size_t i = 0; i < a.size(); i++) {
    FeedEntry f[kFeedCount];
    for (uint8_t k = 0; k < kFeedCount; k++) fillFeed(a[i]["feeds"][k], f[k]);
    uint8_t out[kFeedsReadMax];
    size_t n = encodeFeedsRead(f, out);
    EXPECT_HEX(a[i]["hex"].s, out, n, ctx("feedsRead", i));
  }
}

static void test_time(void) {
  const jm::Value &a = V["timeWrite"];
  for (size_t i = 0; i < a.size(); i++) {
    std::vector<uint8_t> in = jm::unhex(a[i]["hex"].s);
    TimeSync t{};
    Result r = decodeTimeWrite(in.data(), in.size(), t);
    if (a[i].has("error")) { TEST_ASSERT_EQUAL_MESSAGE(a[i]["error"].i(), r, ctx("timeWrite", i)); continue; }
    TEST_ASSERT_EQUAL_MESSAGE(RES_OK, r, ctx("timeWrite", i));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)a[i]["unix"].n, t.unix);
    TEST_ASSERT_EQUAL_INT16((int16_t)a[i]["tz"].i(), t.tzOffsetMin);
    TEST_ASSERT_EQUAL_UINT16(a[i]["ms"].u(), t.millis);
    uint8_t out[kTimeWriteLen];
    size_t n = encodeTimeWrite(t, out);
    EXPECT_HEX(a[i]["hex"].s, out, n, ctx("timeWrite", i));
  }
  const jm::Value &b = V["timeRead"];
  for (size_t i = 0; i < b.size(); i++) {
    uint8_t out[kTimeReadLen];
    size_t n = encodeTimeRead((uint32_t)b[i]["unix"].n, (int16_t)b[i]["tz"].i(), b[i]["rtcOk"].b, out);
    EXPECT_HEX(b[i]["hex"].s, out, n, ctx("timeRead", i));
  }
}

static void test_auth(void) {
  const jm::Value &w = V["auth"]["write"];
  for (size_t i = 0; i < w.size(); i++) {
    std::vector<uint8_t> in = jm::unhex(w[i]["hex"].s);
    uint32_t code = 0xFFFFFFFF;
    Result r = decodeAuthWrite(in.data(), in.size(), code);
    if (w[i].has("error")) { TEST_ASSERT_EQUAL_MESSAGE(w[i]["error"].i(), r, ctx("authWrite", i)); continue; }
    TEST_ASSERT_EQUAL_MESSAGE(RES_OK, r, ctx("authWrite", i));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)w[i]["code"].n, code);
    uint8_t out[kAuthWriteLen];
    EXPECT_HEX(w[i]["hex"].s, out, encodeAuthWrite(code, out), ctx("authWrite", i));
  }
  const jm::Value &rd = V["auth"]["read"];
  for (size_t i = 0; i < rd.size(); i++) {
    uint8_t out[kAuthReadLen];
    size_t n = encodeAuthRead((uint8_t)rd[i]["state"].u(), (uint8_t)rd[i]["attempts"].u(),
                              (uint8_t)rd[i]["seconds"].u(), out);
    EXPECT_HEX(rd[i]["hex"].s, out, n, ctx("authRead", i));
  }
}

static void test_revision(void) {
  const jm::Value &a = V["revision"];
  for (size_t i = 0; i < a.size(); i++) {
    Revision r{};
    r.revision = (uint32_t)a[i]["revision"].n;
    r.changedMask = (uint16_t)a[i]["mask"].u();
    r.source = (uint8_t)a[i]["source"].u();
    r.result = (uint8_t)a[i]["result"].u();
    uint8_t out[kRevisionLen];
    size_t n = encodeRevision(r, out);
    EXPECT_HEX(a[i]["hex"].s, out, n, ctx("revision", i));
    Revision d{};
    TEST_ASSERT_EQUAL(RES_OK, decodeRevision(out, n, d));
    TEST_ASSERT_EQUAL_UINT32(r.revision, d.revision);
    TEST_ASSERT_EQUAL_UINT16(r.changedMask, d.changedMask);
  }
}

static void test_message(void) {
  const jm::Value &a = V["message"];
  for (size_t i = 0; i < a.size(); i++) {
    std::vector<uint8_t> in = jm::unhex(a[i]["hex"].s);
    Message m{};
    Result r = decodeMessage(in.data(), in.size(), m);
    if (a[i].has("error")) { TEST_ASSERT_EQUAL_MESSAGE(a[i]["error"].i(), r, ctx("message", i)); continue; }
    TEST_ASSERT_EQUAL_MESSAGE(RES_OK, r, ctx("message", i));
    TEST_ASSERT_EQUAL_UINT8(a[i]["icon"].u(), m.icon);
    TEST_ASSERT_EQUAL_UINT8(a[i]["flags"].u(), m.flags);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(a[i]["text"].s.c_str(), m.text, ctx("message", i));
  }
}

static void test_control(void) {
  const jm::Value &a = V["control"];
  for (size_t i = 0; i < a.size(); i++) {
    std::vector<uint8_t> in = jm::unhex(a[i]["hex"].s);
    uint8_t op = 0;
    Result r = decodeControl(in.data(), in.size(), op);
    if (a[i].has("error")) { TEST_ASSERT_EQUAL_MESSAGE(a[i]["error"].i(), r, ctx("control", i)); continue; }
    TEST_ASSERT_EQUAL_MESSAGE(RES_OK, r, ctx("control", i));
    TEST_ASSERT_EQUAL_UINT8(a[i]["op"].u(), op);
  }
}

static void test_info(void) {
  const jm::Value &a = V["info"];
  for (size_t i = 0; i < a.size(); i++) {
    std::vector<std::string> names;
    std::vector<const char *> ptrs;
    for (size_t k = 0; k < a[i]["styles"].size(); k++) names.push_back(a[i]["styles"][k].s);
    for (size_t k = 0; k < names.size(); k++) ptrs.push_back(names[k].c_str());
    InfoParams p;
    p.capabilities = (uint16_t)a[i]["caps"].u();
    p.styleCount = (uint8_t)names.size();
    p.styleNames = ptrs.data();
    p.firmware = a[i]["firmware"].s.c_str();
    p.deviceName = a[i]["device"].s.c_str();
    uint8_t out[kInfoMax];
    size_t n = encodeInfo(p, out, sizeof(out));
    EXPECT_HEX(a[i]["hex"].s, out, n, ctx("info", i));
  }
}

static void test_civil(void) {
  const jm::Value &a = V["civil"];
  for (size_t i = 0; i < a.size(); i++) {
    uint32_t unix = (uint32_t)a[i]["unix"].n;
    int16_t tz = (int16_t)a[i]["tz"].i();
    CivilTime t{};
    unixToCivil(unix, tz, t);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(a[i]["year"].u(), t.year, ctx("civil", i));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(a[i]["month"].u(), t.month, ctx("civil", i));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(a[i]["day"].u(), t.day, ctx("civil", i));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(a[i]["hour"].u(), t.hour, ctx("civil", i));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(a[i]["minute"].u(), t.minute, ctx("civil", i));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(a[i]["second"].u(), t.second, ctx("civil", i));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(a[i]["weekday"].u(), t.weekday, ctx("civil", i));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(unix, civilToUnix(t, tz), ctx("civil", i));
  }
}

static void test_glyphs(void) {
  const jm::Value &a = V["glyphs"];
  for (size_t i = 0; i < a.size(); i++) {
    std::vector<uint8_t> in = jm::unhex(a[i]["utf8"].s);
    uint8_t out[64];
    size_t cap = a[i]["cap"].u();
    size_t n = faceslots::utf8ToGlyphs((const char *)in.data(), in.size(), out, cap);
    EXPECT_HEX(a[i]["glyphs"].s, out, n, ctx("glyphs", i));
    TEST_ASSERT_EQUAL_UINT8(0, out[n]);
  }
}

static void test_countdown(void) {
  const jm::Value &a = V["countdown"];
  for (size_t i = 0; i < a.size(); i++) {
    char out[24];
    faceslots::formatCountdown((int64_t)a[i]["delta"].n, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING_MESSAGE(a[i]["text"].s.c_str(), out, ctx("countdown", i));
  }
}

static void test_date(void) {
  const jm::Value &a = V["date"];
  for (size_t i = 0; i < a.size(); i++) {
    CivilTime t{};
    t.year = (uint16_t)a[i]["year"].u();
    t.month = (uint8_t)a[i]["month"].u();
    t.day = (uint8_t)a[i]["day"].u();
    t.weekday = (uint8_t)a[i]["weekday"].u();
    char out[32];
    faceslots::formatDate(t, false, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING_MESSAGE(a[i]["long"].s.c_str(), out, ctx("date", i));
    faceslots::formatDate(t, true, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING_MESSAGE(a[i]["short"].s.c_str(), out, ctx("date", i));
  }
}

static void test_layout(void) {
  const jm::Value &a = V["layout"];
  TEST_ASSERT_TRUE(a.size() >= 15);
  for (size_t i = 0; i < a.size(); i++) {
    const jm::Value &d = a[i]["data"];
    TextEntry texts[kTextCount];
    FeedEntry feeds[kFeedCount];
    memset(texts, 0, sizeof(texts));
    for (uint8_t k = 0; k < kTextCount; k++) {
      const std::string &s = d["texts"][k].s;
      texts[k].len = (uint8_t)s.size();
      memcpy(texts[k].text, s.data(), s.size());
    }
    for (uint8_t k = 0; k < kFeedCount; k++) fillFeed(d["feeds"][k], feeds[k]);
    faceslots::FaceData fd{};
    fd.rtcOk = d["rtcOk"].b;
    fd.now.year = (uint16_t)d["now"]["year"].u();
    fd.now.month = (uint8_t)d["now"]["month"].u();
    fd.now.day = (uint8_t)d["now"]["day"].u();
    fd.now.hour = (uint8_t)d["now"]["hour"].u();
    fd.now.minute = (uint8_t)d["now"]["minute"].u();
    fd.now.second = (uint8_t)d["now"]["second"].u();
    fd.now.weekday = (uint8_t)d["now"]["weekday"].u();
    fd.unixNow = (uint32_t)d["unixNow"].n;
    fd.batOk = d["batOk"].b;
    fd.batPct = (uint8_t)d["batPct"].u();
    fd.texts = texts;
    fd.feeds = feeds;
    SlotCfg cfg;
    cfg.source = (uint8_t)a[i]["slot"][0].u();
    cfg.arg = (uint8_t)a[i]["slot"][1].u();
    cfg.color = (uint8_t)a[i]["slot"][2].u();
    cfg.flags = (uint8_t)a[i]["slot"][3].u();
    faceslots::SlotContent c;
    faceslots::layoutSlot(cfg, fd, (uint8_t)a[i]["rowSize"].u(), (int16_t)a[i]["width"].i(), c);
    EXPECT_HEX(a[i]["glyphs"].s, c.glyphs, c.len, ctx("layout", i));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(a[i]["size"].u(), c.size, ctx("layout.size", i));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(a[i]["icon"].u(), c.icon, ctx("layout.icon", i));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(a[i]["iconArg"].u(), c.iconArg, ctx("layout.iconArg", i));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(a[i]["color"].u(), c.color, ctx("layout.color", i));
    TEST_ASSERT_EQUAL_MESSAGE(a[i]["truncated"].b, c.truncated, ctx("layout.truncated", i));
    TEST_ASSERT_TRUE_MESSAGE(faceslots::contentWidth(c) <= (int16_t)a[i]["width"].i(), ctx("layout.width", i));
  }
}

// ---------------------------------------------------------------------------
// Exhaustive / property checks
// ---------------------------------------------------------------------------
static void test_rgb565_roundtrip_all(void) {
  for (uint32_t c = 0; c <= 0xFFFF; c++) {
    uint8_t r, g, b;
    rgb888FromRgb565((uint16_t)c, r, g, b);
    TEST_ASSERT_EQUAL_UINT16(c, rgb565FromRgb888(r, g, b));
  }
}

static void test_civil_every_day_2000_2099(void) {
  int32_t start = daysFromCivil(2000, 1, 1);
  int32_t end = daysFromCivil(2099, 12, 31);
  TEST_ASSERT_EQUAL_INT32(10957, start);
  uint8_t wd = 6;                                   // 2000-01-01 was a Saturday
  for (int32_t z = start; z <= end; z++) {
    int32_t y; uint32_t m, d;
    civilFromDays(z, y, m, d);
    TEST_ASSERT_EQUAL_INT32(z, daysFromCivil(y, m, d));
    TEST_ASSERT_EQUAL_UINT8(wd, weekdayFromDays(z));
    wd = (uint8_t)((wd + 1) % 7);
  }
}

static void test_decoders_reject_every_wrong_length(void) {
  uint8_t buf[200];
  memset(buf, 0, sizeof(buf));
  for (size_t len = 0; len < sizeof(buf); len++) {
    Theme t; uint8_t b; FaceCfg f; SlotCfg s[kSlotCount]; TimeSync ts; uint32_t code;
    if (len != kThemeLen)      TEST_ASSERT_EQUAL(RES_BAD_LENGTH, decodeTheme(buf, len, t));
    if (len != kBrightnessLen) TEST_ASSERT_EQUAL(RES_BAD_LENGTH, decodeBrightness(buf, len, b));
    if (len != kFaceLen)       TEST_ASSERT_EQUAL(RES_BAD_LENGTH, decodeFace(buf, len, 8, f));
    if (len != kSlotsLen)      TEST_ASSERT_EQUAL(RES_BAD_LENGTH, decodeSlots(buf, len, s));
    if (len != kTimeWriteLen)  TEST_ASSERT_EQUAL(RES_BAD_LENGTH, decodeTimeWrite(buf, len, ts));
    if (len != kAuthWriteLen)  TEST_ASSERT_EQUAL(RES_BAD_LENGTH, decodeAuthWrite(buf, len, code));
  }
}

static void test_glyphs_never_emit_controls(void) {
  // Every 1- and 2-byte input must decode without emitting NUL, LF or CR.
  uint8_t in[2], out[8];
  for (uint32_t v = 0; v <= 0xFFFF; v++) {
    in[0] = (uint8_t)(v >> 8); in[1] = (uint8_t)v;
    for (size_t len = 1; len <= 2; len++) {
      size_t n = faceslots::utf8ToGlyphs((const char *)(len == 1 ? in + 1 : in), len, out, 6);
      for (size_t k = 0; k < n; k++) {
        TEST_ASSERT_NOT_EQUAL(0, out[k]);
        TEST_ASSERT_NOT_EQUAL('\n', out[k]);
        TEST_ASSERT_NOT_EQUAL('\r', out[k]);
      }
    }
  }
}

static void test_layout_always_fits(void) {
  // Fuzz slot layouts across sources, sizes, widths and texts: the result must
  // never exceed the row width or the glyph buffer.
  TextEntry texts[kTextCount];
  FeedEntry feeds[kFeedCount];
  memset(texts, 0, sizeof(texts));
  memset(feeds, 0, sizeof(feeds));
  const char *samples[] = { "", "a", "Hello world", "\xC2\xB0\xC2\xB0\xC2\xB0", "xxxxxxxxxxxxxxxxxxxx",
                            "   spaces   ", "\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6" };
  uint32_t seed = 12345;
  for (int iter = 0; iter < 20000; iter++) {
    seed = seed * 1103515245u + 12345u;
    const char *s = samples[(seed >> 8) % 7];
    size_t len = strlen(s);
    if (len > kTextMax) len = kTextMax;
    uint8_t k = (uint8_t)((seed >> 4) % kTextCount);
    texts[k].len = (uint8_t)len; memcpy(texts[k].text, s, len); texts[k].text[len] = 0;
    feeds[k].len = (uint8_t)len; memcpy(feeds[k].text, s, len); feeds[k].text[len] = 0;
    feeds[k].kind = (uint8_t)((seed >> 12) & 1);
    feeds[k].icon = (uint8_t)((seed >> 14) % 16);
    feeds[k].targetAt = 1790859909u + ((seed >> 3) % 400000u);
    feeds[k].expiresAt = 0;
    faceslots::FaceData fd{};
    fd.rtcOk = true; fd.now.year = 2026; fd.now.month = 10; fd.now.day = 1; fd.now.weekday = 4;
    fd.unixNow = 1790859909u; fd.batOk = (seed & 1) != 0; fd.batPct = (uint8_t)(seed % 101);
    fd.texts = texts; fd.feeds = feeds;
    SlotCfg cfg;
    cfg.source = (uint8_t)((seed >> 16) % 7);
    cfg.arg = (uint8_t)((seed >> 20) % 4);
    cfg.color = 0;
    cfg.flags = (uint8_t)((seed >> 22) & 1);
    uint8_t rowSize = (uint8_t)(2 + ((seed >> 24) & 1));
    int16_t width = (int16_t)(40 + (seed >> 25) % 201);
    faceslots::SlotContent c;
    faceslots::layoutSlot(cfg, fd, rowSize, width, c);
    TEST_ASSERT_TRUE(c.len <= faceslots::kMaxGlyphs);
    TEST_ASSERT_TRUE(faceslots::contentWidth(c) <= width);
    TEST_ASSERT_EQUAL_UINT8(0, c.glyphs[c.len]);
  }
}

static void test_feed_visibility(void) {
  FeedEntry f{};
  f.kind = FEED_VALUE; f.len = 0;
  TEST_ASSERT_FALSE(faceslots::feedVisible(f, 100));
  f.len = 3; f.expiresAt = 0;
  TEST_ASSERT_TRUE(faceslots::feedVisible(f, 100));
  TEST_ASSERT_TRUE(faceslots::feedVisible(f, 0));     // no clock: value feeds still show
  f.expiresAt = 100;
  TEST_ASSERT_TRUE(faceslots::feedVisible(f, 99));
  TEST_ASSERT_FALSE(faceslots::feedVisible(f, 100));
  f.kind = FEED_COUNTDOWN; f.expiresAt = 0; f.targetAt = 1000; f.len = 0;
  TEST_ASSERT_TRUE(faceslots::feedVisible(f, 999));
  TEST_ASSERT_TRUE(faceslots::feedVisible(f, 1000 + 899));   // "now" for 15 minutes
  TEST_ASSERT_FALSE(faceslots::feedVisible(f, 1000 + 900));
  TEST_ASSERT_FALSE(faceslots::feedVisible(f, 0));            // no clock: hidden
}

int main(int, char **) {
  UNITY_BEGIN();
  if (!loadVectors()) {
    printf("could not load %s\n", vectorsPath().c_str());
    TEST_FAIL_MESSAGE("vector file missing");
  }
  RUN_TEST(test_rgb);
  RUN_TEST(test_theme);
  RUN_TEST(test_brightness);
  RUN_TEST(test_face);
  RUN_TEST(test_slots);
  RUN_TEST(test_text_write);
  RUN_TEST(test_texts_read);
  RUN_TEST(test_feed_write);
  RUN_TEST(test_feeds_read);
  RUN_TEST(test_time);
  RUN_TEST(test_auth);
  RUN_TEST(test_revision);
  RUN_TEST(test_message);
  RUN_TEST(test_control);
  RUN_TEST(test_info);
  RUN_TEST(test_civil);
  RUN_TEST(test_glyphs);
  RUN_TEST(test_countdown);
  RUN_TEST(test_date);
  RUN_TEST(test_layout);
  RUN_TEST(test_rgb565_roundtrip_all);
  RUN_TEST(test_civil_every_day_2000_2099);
  RUN_TEST(test_decoders_reject_every_wrong_length);
  RUN_TEST(test_glyphs_never_emit_controls);
  RUN_TEST(test_layout_always_fits);
  RUN_TEST(test_feed_visibility);
  return UNITY_END();
}
