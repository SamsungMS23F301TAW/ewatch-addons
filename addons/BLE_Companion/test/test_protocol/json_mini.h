// Minimal JSON reader for the host tests (objects, arrays, strings with
// \uXXXX escapes and surrogate pairs, numbers, booleans, null). Test-only
// code: it trusts its input and reports failures by returning a Null value.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace jm {

struct Value {
  enum Type { Null, Bool, Number, String, Array, Object } type = Null;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<Value> a;
  std::map<std::string, Value> o;

  bool has(const std::string &k) const { return type == Object && o.count(k) != 0; }
  const Value &operator[](const std::string &k) const {
    static const Value kNull;
    auto it = o.find(k);
    return it == o.end() ? kNull : it->second;
  }
  const Value &operator[](size_t i) const { return a[i]; }
  size_t size() const { return type == Array ? a.size() : o.size(); }
  long long i() const { return (long long)n; }
  unsigned u() const { return (unsigned)n; }
};

class Parser {
public:
  explicit Parser(const std::string &text) : t_(text) {}
  Value parse() { Value v = value(); ws(); return v; }

private:
  const std::string &t_;
  size_t p_ = 0;

  void ws() { while (p_ < t_.size() && (t_[p_] == ' ' || t_[p_] == '\n' || t_[p_] == '\r' || t_[p_] == '\t')) p_++; }

  Value value() {
    ws();
    Value v;
    if (p_ >= t_.size()) return v;
    char c = t_[p_];
    if (c == '{') {
      v.type = Value::Object; p_++;
      ws();
      if (t_[p_] == '}') { p_++; return v; }
      for (;;) {
        ws();
        std::string k = str();
        ws(); p_++;                       // ':'
        v.o[k] = value();
        ws();
        if (t_[p_] == ',') { p_++; continue; }
        p_++;                             // '}'
        break;
      }
    } else if (c == '[') {
      v.type = Value::Array; p_++;
      ws();
      if (t_[p_] == ']') { p_++; return v; }
      for (;;) {
        v.a.push_back(value());
        ws();
        if (t_[p_] == ',') { p_++; continue; }
        p_++;                             // ']'
        break;
      }
    } else if (c == '"') {
      v.type = Value::String; v.s = str();
    } else if (c == 't') { v.type = Value::Bool; v.b = true; p_ += 4; }
    else if (c == 'f') { v.type = Value::Bool; v.b = false; p_ += 5; }
    else if (c == 'n') { p_ += 4; }
    else {
      v.type = Value::Number;
      char *end = nullptr;
      v.n = strtod(t_.c_str() + p_, &end);
      p_ = (size_t)(end - t_.c_str());
    }
    return v;
  }

  static void utf8(std::string &out, uint32_t cp) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
      out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F));
      out += (char)(0x80 | (cp & 0x3F));
    } else {
      out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F));
      out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
    }
  }

  uint32_t hex4() {
    uint32_t v = (uint32_t)strtoul(t_.substr(p_, 4).c_str(), nullptr, 16);
    p_ += 4;
    return v;
  }

  std::string str() {
    std::string out;
    p_++;                                 // opening quote
    while (p_ < t_.size() && t_[p_] != '"') {
      char c = t_[p_++];
      if (c != '\\') { out += c; continue; }
      char e = t_[p_++];
      switch (e) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u': {
          uint32_t cp = hex4();
          if (cp >= 0xD800 && cp <= 0xDBFF && t_[p_] == '\\' && t_[p_ + 1] == 'u') {
            p_ += 2;
            uint32_t lo = hex4();
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          utf8(out, cp);
          break;
        }
        default: out += e; break;         // \" \\ \/
      }
    }
    p_++;                                 // closing quote
    return out;
  }
};

inline std::vector<uint8_t> unhex(const std::string &h) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < h.size(); i += 2) {
    out.push_back((uint8_t)strtoul(h.substr(i, 2).c_str(), nullptr, 16));
  }
  return out;
}

inline std::string tohex(const uint8_t *p, size_t n) {
  static const char *d = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; i++) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
  return s;
}

}  // namespace jm
