// Rep Counter: replay every recording in test/data/ through the detector and
// check it against the file's "# expect" line. Real recordings made with
// tools/record.py become regression tests by dropping them in that folder.
#include <unity.h>
#include <dirent.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "rep_csv.h"
#include "rep_harness.h"
#include "rep_scenarios.h"

static std::string dataDir() {
  // test/test_replay/test_replay.cpp -> test/data
  std::string here = __FILE__;
  size_t p = here.rfind("test_replay");
  if (p != std::string::npos) {
    std::string d = here.substr(0, p) + "data";
    if (DIR *dir = opendir(d.c_str())) { closedir(dir); return d; }
  }
  return "test/data";
}

static std::vector<std::string> recordings() {
  std::vector<std::string> out;
  std::string d = dataDir();
  if (DIR *dir = opendir(d.c_str())) {
    while (dirent *e = readdir(dir)) {
      std::string n = e->d_name;
      if (n.size() > 4 && n.substr(n.size() - 4) == ".csv") out.push_back(d + "/" + n);
    }
    closedir(dir);
  }
  return out;
}

static void test_recordings_match_expectations() {
  std::vector<std::string> files = recordings();
  TEST_ASSERT_TRUE_MESSAGE(!files.empty(), "no recordings found in test/data");
  int checked = 0;
  for (auto &path : files) {
    csv::Recording rec;
    TEST_ASSERT_TRUE_MESSAGE(csv::load(path, rec), rec.error.c_str());
    if (!rec.hasExpect) continue;              // unlabelled recording: just parse it
    reps::Config cfg = csv::configFor(rec);
    reps::Detector d;
    harness::RunResult r = harness::run(rec.xyz, cfg, &d, &rec.gapsBefore);
    std::printf("  %s: %zu set(s):", path.c_str(), r.sets.size());
    for (auto &s : r.sets) std::printf(" %d", s.reps);
    std::printf(" | expected:");
    for (int x : rec.expectReps) std::printf(" %d", x);
    std::printf("\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)rec.expectReps.size(), (int)r.sets.size(), path.c_str());
    for (size_t i = 0; i < r.sets.size(); i++) {
      TEST_ASSERT_INT_WITHIN_MESSAGE(rec.expectTol, rec.expectReps[i], r.sets[i].reps, path.c_str());
      if (i < rec.expectEx.size() && rec.expectEx[i] != reps::Exercise::None)
        TEST_ASSERT_EQUAL_INT_MESSAGE((int)rec.expectEx[i], (int)r.sets[i].ex, path.c_str());
    }
    checked++;
  }
  TEST_ASSERT_TRUE(checked > 0);
}

static void test_csv_parser_handles_gaps_and_comments() {
  const char *tmp = "/tmp/rep_counter_csv_test.csv";
  FILE *f = std::fopen(tmp, "w");
  TEST_ASSERT_NOT_NULL(f);
  std::fprintf(f, "# rep-counter rec v1\r\n# fs_hz=50 counts_per_g=4096 wrist=R mode=Press\n");
  std::fprintf(f, "# expect reps=3,4 exercise=Press,Row tol=0\nseq,t_ms,ax,ay,az\n");
  std::fprintf(f, "0,10,1,2,3\n1,30,4,5,6\n# ev t_ms=40 type=rep reps=1\n");
  std::fprintf(f, "5,110,7,8,9\n\n6,130,-1,-2,-3\n# gap lost=2\n9,190,0,0,4096\n");
  std::fclose(f);
  csv::Recording rec;
  TEST_ASSERT_TRUE(csv::load(tmp, rec));
  TEST_ASSERT_EQUAL_INT(5 * 3, (int)rec.xyz.size());
  TEST_ASSERT_EQUAL_INT(2, (int)rec.gapsBefore.size());
  TEST_ASSERT_EQUAL_INT(2, (int)rec.gapsBefore[0]);
  TEST_ASSERT_EQUAL_INT(4, (int)rec.gapsBefore[1]);
  TEST_ASSERT_EQUAL_FLOAT(50.f, rec.fs);
  TEST_ASSERT_EQUAL_FLOAT(4096.f, rec.countsPerG);
  TEST_ASSERT_EQUAL_INT(2, (int)rec.expectReps.size());
  TEST_ASSERT_EQUAL_INT(0, rec.expectTol);
  TEST_ASSERT_EQUAL_INT((int)reps::Exercise::Row, (int)rec.expectEx[1]);
  reps::Config c = csv::configFor(rec);
  TEST_ASSERT_EQUAL_INT((int)reps::Wrist::Right, (int)c.wrist);
  TEST_ASSERT_EQUAL_INT((int)reps::Mode::Press, (int)c.mode);
  std::remove(tmp);
}

static void test_csv_roundtrip_reproduces_counts() {
  scen::Built b = scen::build("raise", 42);
  std::vector<int> expect;
  for (auto &t : b.truth) expect.push_back(t.reps);
  const char *tmp = "/tmp/rep_counter_roundtrip.csv";
  FILE *f = std::fopen(tmp, "w");
  TEST_ASSERT_NOT_NULL(f);
  std::map<std::string, std::string> meta;
  meta["wrist"] = b.cfg.wrist == reps::Wrist::Left ? "L" : "R";
  csv::write(f, b.xyz, 100.f, meta, expect, {});
  std::fclose(f);
  csv::Recording rec;
  TEST_ASSERT_TRUE(csv::load(tmp, rec));
  TEST_ASSERT_EQUAL_INT((int)b.xyz.size(), (int)rec.xyz.size());
  harness::RunResult direct = harness::run(b.xyz, b.cfg);
  harness::RunResult replay = harness::run(rec.xyz, csv::configFor(rec));
  TEST_ASSERT_EQUAL_INT((int)direct.sets.size(), (int)replay.sets.size());
  for (size_t i = 0; i < direct.sets.size(); i++) TEST_ASSERT_EQUAL_INT(direct.sets[i].reps, replay.sets[i].reps);
  std::remove(tmp);
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_csv_parser_handles_gaps_and_comments);
  RUN_TEST(test_csv_roundtrip_reproduces_counts);
  RUN_TEST(test_recordings_match_expectations);
  return UNITY_END();
}
