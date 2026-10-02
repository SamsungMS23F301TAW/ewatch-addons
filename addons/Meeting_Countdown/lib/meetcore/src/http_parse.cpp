#include "http_parse.h"
#include "mc_text.h"
#include "mc_time.h"
#include <string.h>
#include <stdio.h>

namespace mc {

static bool startsNoCase(const char *s, const char *prefix) {
  size_t n = strlen(prefix);
  return strlen(s) >= n && equalsNoCase(s, prefix, n);
}

bool parseUrl(const char *in, Url &out) {
  out = Url();
  if (!in) return false;
  while (*in == ' ' || *in == '\t') in++;
  const char *p;
  if (startsNoCase(in, "https://"))        { out.tls = true;  out.port = 443; p = in + 8; }
  else if (startsNoCase(in, "webcals://")) { out.tls = true;  out.port = 443; p = in + 10; }
  else if (startsNoCase(in, "webcal://"))  { out.tls = true;  out.port = 443; p = in + 9; }
  else if (startsNoCase(in, "http://"))    { out.tls = false; out.port = 80;  p = in + 7; }
  else return false;

  // authority = host[:port], ends at '/', '?' or '#' or end
  const char *a = p;
  while (*p && *p != '/' && *p != '?' && *p != '#') p++;
  size_t alen = (size_t)(p - a);
  if (alen == 0) return false;
  if (memchr(a, '@', alen)) return false;                 // no user-info
  const char *colon = (const char *)memchr(a, ':', alen);
  size_t hlen = colon ? (size_t)(colon - a) : alen;
  if (hlen == 0 || hlen >= sizeof(out.host)) return false;
  for (size_t i = 0; i < hlen; i++) {
    char c = a[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '.' || c == '_';
    if (!ok) return false;
    out.host[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
  }
  out.host[hlen] = '\0';
  if (colon) {
    unsigned long port = 0;
    const char *q = colon + 1;
    if (q == a + alen) return false;
    for (; q < a + alen; q++) {
      if (*q < '0' || *q > '9') return false;
      port = port * 10 + (unsigned long)(*q - '0');
      if (port > 65535) return false;
    }
    if (port == 0) return false;
    out.port = (uint16_t)port;
  }
  // path + query (fragment dropped); spaces are not legal in a request line.
  size_t w = 0;
  if (*p != '/') out.path[w++] = '/';
  for (; *p && *p != '#'; p++) {
    char c = *p;
    if (c == ' ') {
      if (w + 3 >= sizeof(out.path)) return false;
      out.path[w++] = '%'; out.path[w++] = '2'; out.path[w++] = '0';
      continue;
    }
    if ((unsigned char)c < 0x21 || c == 0x7F) return false;
    if (w + 1 >= sizeof(out.path)) return false;
    out.path[w++] = c;
  }
  out.path[w] = '\0';
  return true;
}

bool resolveLocation(const Url &base, const char *loc, Url &out) {
  if (!loc || !*loc) return false;
  if (strstr(loc, "://")) return parseUrl(loc, out);
  out = base;
  if (loc[0] == '/' && loc[1] == '/') {                    // scheme-relative
    char buf[600];
    snprintf(buf, sizeof(buf), "%s:%s", base.tls ? "https" : "http", loc);
    return parseUrl(buf, out);
  }
  if (loc[0] == '/') {
    if (strlen(loc) >= sizeof(out.path)) return false;
    copyStr(out.path, sizeof(out.path), loc);
    return true;
  }
  // Relative path: replace everything after the last '/' of the base path.
  char path[sizeof(out.path)];
  copyStr(path, sizeof(path), base.path);
  char *q = strchr(path, '?');
  if (q) *q = '\0';
  char *slash = strrchr(path, '/');
  if (slash) slash[1] = '\0';
  if (strlen(path) + strlen(loc) >= sizeof(out.path)) return false;
  snprintf(out.path, sizeof(out.path), "%s%s", path, loc);
  return true;
}

void redactUrl(const char *url, char *out, size_t cap) {
  if (!cap) return;
  out[0] = '\0';
  if (!url || !*url) return;
  Url u;
  if (!parseUrl(url, u)) { copyStr(out, cap, "(invalid URL)"); return; }
  // Calendar providers hide the secret token somewhere in the path: in the
  // middle (Google), or as the last segment (iCloud). Show the host, plus the
  // last segment only when it is a plain .ics file name.
  const char *path = u.path;
  const char *q = strchr(path, '?');
  size_t plen = q ? (size_t)(q - path) : strlen(path);
  size_t s = plen;
  while (s > 0 && path[s - 1] != '/') s--;
  size_t tl = plen - s;
  char tail[32] = "";
  bool showTail = tl >= 5 && tl <= 24 && equalsNoCase(path + plen - 4, ".ics", 4);
  if (showTail) {
    memcpy(tail, path + s, tl);
    tail[tl] = '\0';
  }
  bool hidden = (plen > 1 && !showTail) || s > 1 || q;
  if (showTail) snprintf(out, cap, hidden ? "%s/.../%s" : "%s/%s", u.host, tail);
  else snprintf(out, cap, hidden ? "%s/..." : "%s", u.host);
}

// ---------------------------------------------------------------------------
bool parseHttpDate(const char *s, int64_t &unixOut) {
  // IMF-fixdate: "Sun, 06 Nov 1994 08:49:37 GMT"
  static const char *kMon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  if (!s) return false;
  const char *comma = strchr(s, ',');
  if (!comma) return false;
  int d, y, h, mi, sec;
  char mon[4] = {0};
  if (sscanf(comma + 1, " %d %3s %d %d:%d:%d", &d, mon, &y, &h, &mi, &sec) != 6) return false;
  int m = 0;
  for (int i = 0; i < 12; i++) if (equalsNoCase(mon, kMon[i], 3)) m = i + 1;
  if (!m || d < 1 || d > 31 || y < 2000 || y > 2200 || h > 23 || mi > 59 || sec > 60) return false;
  unixOut = civilToSeconds(y, m, d, h, mi, sec > 59 ? 59 : sec);
  return true;
}

// ---------------------------------------------------------------------------
void HttpResponseParser::begin(BodyFn fn, void *ctx) {
  fn_ = fn;
  ctx_ = ctx;
  state_ = S_STATUS;
  lineLen_ = 0;
  lineOverflow_ = false;
  status_ = 0;
  chunked_ = gzip_ = false;
  contentLength_ = -1;
  remaining_ = 0;
  bodyBytes_ = 0;
  date_ = 0;
  headerBytes_ = 0;
  location_[0] = '\0';
  contentType_[0] = '\0';
}

static const char *afterColon(const char *line) {
  const char *c = strchr(line, ':');
  if (!c) return "";
  c++;
  while (*c == ' ' || *c == '\t') c++;
  return c;
}

void HttpResponseParser::headerLine() {
  line_[lineLen_] = '\0';
  if (state_ == S_STATUS) {
    // "HTTP/1.1 200 OK"
    if (strncmp(line_, "HTTP/", 5) != 0) { state_ = S_ERROR; return; }
    const char *sp = strchr(line_, ' ');
    if (!sp) { state_ = S_ERROR; return; }
    int code = 0;
    for (const char *p = sp + 1; *p >= '0' && *p <= '9'; p++) code = code * 10 + (*p - '0');
    if (code < 100 || code > 599) { state_ = S_ERROR; return; }
    status_ = code;
    state_ = S_HEADERS;
    return;
  }
  if (state_ == S_TRAILER) {
    if (lineLen_ == 0) state_ = S_DONE;
    return;
  }
  // S_HEADERS
  if (lineLen_ == 0) {
    if (status_ >= 100 && status_ < 200) {      // interim response: start over
      state_ = S_STATUS;
      return;
    }
    startBody();
    return;
  }
  if (lineOverflow_) return;                     // ignore absurdly long headers
  if (startsNoCase(line_, "content-length:")) {
    const char *v = afterColon(line_);
    int64_t n = 0;
    bool any = false;
    for (; *v >= '0' && *v <= '9'; v++) { n = n * 10 + (*v - '0'); any = true; }
    if (any) contentLength_ = n;
  } else if (startsNoCase(line_, "transfer-encoding:")) {
    const char *v = afterColon(line_);
    // "chunked" must be the final coding.
    size_t l = strlen(v);
    while (l && (v[l - 1] == ' ' || v[l - 1] == '\t')) l--;
    if (l >= 7 && equalsNoCase(v + l - 7, "chunked", 7)) chunked_ = true;
    if (strstr(v, "gzip") || strstr(v, "deflate")) gzip_ = true;
  } else if (startsNoCase(line_, "content-encoding:")) {
    const char *v = afterColon(line_);
    if (!startsNoCase(v, "identity") && *v) gzip_ = true;
  } else if (startsNoCase(line_, "location:")) {
    copyStr(location_, sizeof(location_), afterColon(line_));
  } else if (startsNoCase(line_, "content-type:")) {
    copyStr(contentType_, sizeof(contentType_), afterColon(line_));
  } else if (startsNoCase(line_, "date:")) {
    int64_t t;
    if (parseHttpDate(afterColon(line_), t)) date_ = t;
  }
}

void HttpResponseParser::startBody() {
  if (status_ == 204 || status_ == 304) { state_ = S_DONE; return; }
  if (chunked_) { state_ = S_CHUNK_SIZE; lineLen_ = 0; remaining_ = 0; return; }
  if (contentLength_ >= 0) {
    remaining_ = contentLength_;
    state_ = remaining_ == 0 ? S_DONE : S_BODY_LEN;
    return;
  }
  state_ = S_BODY_CLOSE;                         // read until the peer closes
}

bool HttpResponseParser::feed(const char *data, size_t n) {
  size_t i = 0;
  while (i < n) {
    switch (state_) {
      case S_DONE:
        return true;                             // ignore anything after the body
      case S_ERROR:
        return false;
      case S_STATUS:
      case S_HEADERS:
      case S_TRAILER: {
        char c = data[i++];
        if (++headerBytes_ > 32768) { state_ = S_ERROR; return false; }
        if (c == '\r') break;
        if (c == '\n') {
          headerLine();
          lineLen_ = 0;
          lineOverflow_ = false;
          break;
        }
        if (lineLen_ < sizeof(line_) - 1) line_[lineLen_++] = c;
        else lineOverflow_ = true;
        break;
      }
      case S_BODY_LEN: {
        size_t take = n - i;
        if ((int64_t)take > remaining_) take = (size_t)remaining_;
        if (fn_ && take) fn_(ctx_, data + i, take);
        bodyBytes_ += (int64_t)take;
        remaining_ -= (int64_t)take;
        i += take;
        if (remaining_ == 0) state_ = S_DONE;
        break;
      }
      case S_BODY_CLOSE: {
        size_t take = n - i;
        if (fn_ && take) fn_(ctx_, data + i, take);
        bodyBytes_ += (int64_t)take;
        i += take;
        break;
      }
      case S_CHUNK_SIZE: {
        char c = data[i++];
        int v = -1;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        if (v >= 0) {
          if (++lineLen_ > 12) { state_ = S_ERROR; return false; }   // > 2^48: nonsense
          remaining_ = remaining_ * 16 + v;
        } else if (c == ';' || c == ' ' || c == '\t') {
          state_ = S_CHUNK_EXT;
        } else if (c == '\r') {
          // wait for '\n'
        } else if (c == '\n') {
          if (lineLen_ == 0) { state_ = S_ERROR; return false; }
          lineLen_ = 0;
          if (remaining_ == 0) { state_ = S_TRAILER; }
          else state_ = S_CHUNK_DATA;
        } else {
          state_ = S_ERROR;
          return false;
        }
        break;
      }
      case S_CHUNK_EXT: {
        char c = data[i++];
        if (c == '\n') {
          if (lineLen_ == 0) { state_ = S_ERROR; return false; }
          lineLen_ = 0;
          state_ = remaining_ == 0 ? S_TRAILER : S_CHUNK_DATA;
        }
        break;
      }
      case S_CHUNK_DATA: {
        size_t take = n - i;
        if ((int64_t)take > remaining_) take = (size_t)remaining_;
        if (fn_ && take) fn_(ctx_, data + i, take);
        bodyBytes_ += (int64_t)take;
        remaining_ -= (int64_t)take;
        i += take;
        if (remaining_ == 0) state_ = S_CHUNK_CRLF;
        break;
      }
      case S_CHUNK_CRLF: {
        char c = data[i++];
        if (c == '\n') { state_ = S_CHUNK_SIZE; lineLen_ = 0; remaining_ = 0; }
        else if (c != '\r') { state_ = S_ERROR; return false; }
        break;
      }
    }
  }
  return state_ != S_ERROR;
}

void HttpResponseParser::onEof() {
  if (state_ == S_BODY_CLOSE) state_ = S_DONE;
  else if (state_ == S_TRAILER) state_ = S_DONE;   // trailer CRLF may be missing
}

}  // namespace mc
