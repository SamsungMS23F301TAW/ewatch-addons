// Rep Counter: the recording format (REC on / tools/record.py) and a reader
// that feeds a recording through the detector. Host only.
//
//   # rep-counter rec v1
//   # fs_hz=100 counts_per_g=2048 range_g=4
//   # wrist=L mode=Auto fw=1.0.0
//   # expect reps=12,10 exercise=Curl,Curl      <- optional, for regression
//   seq,t_ms,ax,ay,az
//   0,523011,-25,-2031,96
//   1,523021,-27,-2029,99
//   # ev t_ms=531200 type=rep reps=3 ex=Curl conf=2   <- the watch's own events
//   # gap lost=7                                       <- samples lost on the way
//
// Lines starting with '#' carry metadata as key=value pairs; data rows are
// integers in raw counts. A jump in `seq` (or a `# gap` line) is a hole in
// the stream and is passed to Detector::markGap().
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "rep_detector.h"

namespace csv {

struct Recording {
  std::map<std::string, std::string> meta;   // every key=value seen in # lines
  std::vector<int16_t> xyz;                   // interleaved raw counts
  std::vector<size_t> gapsBefore;             // sample indices preceded by a hole
  std::vector<int> expectReps;                // from "# expect reps=..."
  std::vector<reps::Exercise> expectEx;       // from "# expect exercise=..."
  int expectTol = 1;
  bool hasExpect = false;                     // an "# expect" line was present
  float fs = 100.f;
  float countsPerG = 2048.f;
  std::string error;                          // non-empty if the file was unusable
};

inline std::vector<std::string> split(const std::string &s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == sep) { out.push_back(cur); cur.clear(); }
    else cur += c;
  }
  out.push_back(cur);
  return out;
}

inline reps::Exercise parseExercise(const std::string &s) {
  std::string l;
  for (char c : s) l += (char)tolower((unsigned char)c);
  if (l == "curl") return reps::Exercise::Curl;
  if (l == "press") return reps::Exercise::Press;
  if (l == "raise") return reps::Exercise::Raise;
  if (l == "row") return reps::Exercise::Row;
  return reps::Exercise::None;
}

inline bool parseMode(const std::string &s, reps::Mode &m) {
  std::string l;
  for (char c : s) l += (char)tolower((unsigned char)c);
  if (l == "auto") m = reps::Mode::Auto;
  else if (l == "curl") m = reps::Mode::Curl;
  else if (l == "press") m = reps::Mode::Press;
  else if (l == "raise") m = reps::Mode::Raise;
  else if (l == "row") m = reps::Mode::Row;
  else return false;
  return true;
}

inline bool load(const std::string &path, Recording &rec) {
  FILE *f = std::fopen(path.c_str(), "r");
  if (!f) { rec.error = "cannot open " + path; return false; }
  char line[512];
  long lastSeq = -1;
  bool pendingGap = false;
  while (std::fgets(line, sizeof line, f)) {
    size_t n = std::strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (!n) continue;
    if (line[0] == '#') {
      std::string body(line + 1);
      std::istringstream is(body);
      std::string tok, first;
      bool isExpect = false, isGap = false;
      is >> first;
      if (first == "expect") { isExpect = true; rec.hasExpect = true; }
      if (first == "gap") isGap = true;
      std::istringstream is2(body);
      while (is2 >> tok) {
        size_t eq = tok.find('=');
        if (eq == std::string::npos) continue;
        std::string k = tok.substr(0, eq), v = tok.substr(eq + 1);
        if (isExpect) {
          if (k == "reps") for (auto &x : split(v, ',')) if (!x.empty()) rec.expectReps.push_back(atoi(x.c_str()));
          if (k == "exercise") for (auto &x : split(v, ',')) rec.expectEx.push_back(parseExercise(x));
          if (k == "tol") rec.expectTol = atoi(v.c_str());
        } else if (first != "ev") {
          rec.meta[k] = v;
        }
      }
      if (isGap) pendingGap = true;
      continue;
    }
    if (line[0] < '0' || line[0] > '9') {
      if (line[0] != '-') continue;            // header row "seq,t_ms,..."
    }
    long v[5];
    int got = std::sscanf(line, "%ld,%ld,%ld,%ld,%ld", &v[0], &v[1], &v[2], &v[3], &v[4]);
    if (got != 5) continue;
    if (lastSeq >= 0 && v[0] != lastSeq + 1) pendingGap = true;
    lastSeq = v[0];
    if (pendingGap) { rec.gapsBefore.push_back(rec.xyz.size() / 3); pendingGap = false; }
    for (int i = 2; i < 5; i++) {
      long c = v[i];
      if (c > 32767) c = 32767;
      if (c < -32768) c = -32768;
      rec.xyz.push_back((int16_t)c);
    }
  }
  std::fclose(f);
  if (rec.meta.count("fs_hz")) rec.fs = (float)atof(rec.meta["fs_hz"].c_str());
  if (rec.meta.count("counts_per_g")) rec.countsPerG = (float)atof(rec.meta["counts_per_g"].c_str());
  if (rec.xyz.empty()) { rec.error = "no samples in " + path; return false; }
  if (rec.fs < 10.f || rec.fs > 1000.f) { rec.error = "bad fs_hz"; return false; }
  return true;
}

// Detector config for a recording: rate/scale from the header, wrist and mode
// as recorded (overridable by the caller afterwards).
inline reps::Config configFor(const Recording &rec) {
  reps::Config c;
  c.fs = rec.fs;
  c.countsPerG = rec.countsPerG;
  auto it = rec.meta.find("wrist");
  if (it != rec.meta.end() && (it->second == "R" || it->second == "r" || it->second == "right"))
    c.wrist = reps::Wrist::Right;
  it = rec.meta.find("mode");
  if (it != rec.meta.end()) parseMode(it->second, c.mode);
  it = rec.meta.find("set_end_s");
  if (it != rec.meta.end()) c.setEndSec = (float)atof(it->second.c_str());
  it = rec.meta.find("hand");
  if (it != rec.meta.end()) c.learnedHandSign = (int8_t)atoi(it->second.c_str());
  return c;
}

inline void write(FILE *f, const std::vector<int16_t> &xyz, float fs,
                  const std::map<std::string, std::string> &meta,
                  const std::vector<int> &expectReps,
                  const std::vector<reps::Exercise> &expectEx) {
  std::fprintf(f, "# rep-counter rec v1\n");
  std::fprintf(f, "# fs_hz=%g counts_per_g=2048 range_g=4\n", fs);
  if (!meta.empty()) {
    std::fprintf(f, "#");
    for (auto &kv : meta) std::fprintf(f, " %s=%s", kv.first.c_str(), kv.second.c_str());
    std::fprintf(f, "\n");
  }
  if (!expectReps.empty()) {
    std::fprintf(f, "# expect reps=");
    for (size_t i = 0; i < expectReps.size(); i++) std::fprintf(f, "%s%d", i ? "," : "", expectReps[i]);
    if (!expectEx.empty()) {
      std::fprintf(f, " exercise=");
      for (size_t i = 0; i < expectEx.size(); i++)
        std::fprintf(f, "%s%s", i ? "," : "", reps::exerciseName(expectEx[i]));
    }
    std::fprintf(f, "\n");
  } else {
    std::fprintf(f, "# expect reps=\n");
  }
  std::fprintf(f, "seq,t_ms,ax,ay,az\n");
  size_t n = xyz.size() / 3;
  for (size_t i = 0; i < n; i++)
    std::fprintf(f, "%zu,%lu,%d,%d,%d\n", i, (unsigned long)(i * 1000.0 / fs), xyz[3 * i],
                 xyz[3 * i + 1], xyz[3 * i + 2]);
}

}  // namespace csv
