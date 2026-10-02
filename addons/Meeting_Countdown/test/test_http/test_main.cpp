// Host tests for URL handling and the HTTP/1.1 response parser that frames
// feed downloads (Content-Length, chunked, close-delimited; redirects;
// interim 1xx; Date header) — fed at every possible split point.
#include <unity.h>
#include <string.h>
#include <string>
#include "http_parse.h"

using namespace mc;

void setUp() {}
void tearDown() {}

static std::string gBody;
static void sink(void *, const char *d, size_t n) { gBody.append(d, n); }

// Feed `resp` split at position `cut` (and byte-by-byte if cut == 0).
static bool run(HttpResponseParser &p, const std::string &resp, size_t cut, bool eof = true) {
  gBody.clear();
  p.begin(sink, nullptr);
  bool ok = true;
  if (cut == 0) {
    for (char c : resp) ok = p.feed(&c, 1) && ok;
  } else {
    ok = p.feed(resp.data(), cut) && ok;
    ok = p.feed(resp.data() + cut, resp.size() - cut) && ok;
  }
  if (eof) p.onEof();
  return ok;
}

void test_parse_url() {
  Url u;
  TEST_ASSERT_TRUE(parseUrl("https://calendar.google.com/calendar/ical/me%40x.com/private-abc/basic.ics", u));
  TEST_ASSERT_TRUE(u.tls);
  TEST_ASSERT_EQUAL_UINT16(443, u.port);
  TEST_ASSERT_EQUAL_STRING("calendar.google.com", u.host);
  TEST_ASSERT_EQUAL_STRING("/calendar/ical/me%40x.com/private-abc/basic.ics", u.path);
  TEST_ASSERT_TRUE(parseUrl("webcal://P01-Caldav.iCloud.com/published/2/abc", u));
  TEST_ASSERT_TRUE(u.tls);
  TEST_ASSERT_EQUAL_STRING("p01-caldav.icloud.com", u.host);
  TEST_ASSERT_TRUE(parseUrl("http://192.168.1.20:8080/cal.ics?user=k&x=1#frag", u));
  TEST_ASSERT_FALSE(u.tls);
  TEST_ASSERT_EQUAL_UINT16(8080, u.port);
  TEST_ASSERT_EQUAL_STRING("/cal.ics?user=k&x=1", u.path);
  TEST_ASSERT_TRUE(parseUrl("https://example.com", u));
  TEST_ASSERT_EQUAL_STRING("/", u.path);
  TEST_ASSERT_TRUE(parseUrl("https://example.com?x=1", u));
  TEST_ASSERT_EQUAL_STRING("/?x=1", u.path);
  TEST_ASSERT_TRUE(parseUrl("  https://example.com/a b.ics", u));
  TEST_ASSERT_EQUAL_STRING("/a%20b.ics", u.path);
  TEST_ASSERT_FALSE(parseUrl("ftp://example.com/x.ics", u));
  TEST_ASSERT_FALSE(parseUrl("https://user:pw@example.com/x", u));
  TEST_ASSERT_FALSE(parseUrl("https:///x", u));
  TEST_ASSERT_FALSE(parseUrl("https://example.com:99999/", u));
  TEST_ASSERT_FALSE(parseUrl("calendar.google.com/x.ics", u));
}

void test_resolve_location() {
  Url base, out;
  TEST_ASSERT_TRUE(parseUrl("https://a.example.com/dir/feed.ics?k=1", base));
  TEST_ASSERT_TRUE(resolveLocation(base, "https://b.example.net/other.ics", out));
  TEST_ASSERT_EQUAL_STRING("b.example.net", out.host);
  TEST_ASSERT_TRUE(resolveLocation(base, "//c.example.org/x", out));
  TEST_ASSERT_EQUAL_STRING("c.example.org", out.host);
  TEST_ASSERT_TRUE(out.tls);
  TEST_ASSERT_TRUE(resolveLocation(base, "/abs/path.ics", out));
  TEST_ASSERT_EQUAL_STRING("a.example.com", out.host);
  TEST_ASSERT_EQUAL_STRING("/abs/path.ics", out.path);
  TEST_ASSERT_TRUE(resolveLocation(base, "rel.ics", out));
  TEST_ASSERT_EQUAL_STRING("/dir/rel.ics", out.path);
  TEST_ASSERT_FALSE(resolveLocation(base, "", out));
}

void test_redact_url() {
  char b[96];
  redactUrl("https://calendar.google.com/calendar/ical/me%40x.com/private-0123456789abcdef/basic.ics", b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("calendar.google.com/.../basic.ics", b);
  TEST_ASSERT_NULL(strstr(b, "private"));
  redactUrl("https://outlook.office365.com/owa/calendar/abc@x.com/SECRET/calendar.ics", b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("outlook.office365.com/.../calendar.ics", b);
  redactUrl("https://example.com/feed.ics?token=SECRET", b, sizeof(b));
  TEST_ASSERT_NULL(strstr(b, "SECRET"));
  redactUrl("https://example.com/cal.ics", b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("example.com/cal.ics", b);
  // iCloud puts the secret in the last segment: never show it.
  redactUrl("webcal://p42-caldav.icloud.com/published/2/MTIzNDU2Nzg5MDEyMzQ1Njc4", b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("p42-caldav.icloud.com/...", b);
  TEST_ASSERT_NULL(strstr(b, "MTIz"));
  redactUrl("https://user.fm/calendar/v1-SECRETSECRET/Calendar.ics", b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("user.fm/.../Calendar.ics", b);
  redactUrl("https://example.com/", b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("example.com", b);
}

void test_http_date() {
  int64_t t;
  TEST_ASSERT_TRUE(parseHttpDate("Thu, 01 Oct 2026 08:00:00 GMT", t));
  TEST_ASSERT_EQUAL_INT64(1790841600, t);
  TEST_ASSERT_FALSE(parseHttpDate("yesterday", t));
}

void test_content_length_every_split() {
  std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/calendar; charset=utf-8\r\n"
                     "Date: Thu, 01 Oct 2026 08:00:00 GMT\r\nContent-Length: 23\r\n\r\n"
                     "BEGIN:VCALENDAR\r\nEND:X\r\nGARBAGE-AFTER-BODY";
  HttpResponseParser p;
  for (size_t cut = 0; cut < resp.size(); cut++) {
    TEST_ASSERT_TRUE(run(p, resp, cut));
    TEST_ASSERT_EQUAL_INT(200, p.status());
    TEST_ASSERT_TRUE(p.done());
    TEST_ASSERT_EQUAL_STRING("BEGIN:VCALENDAR\r\nEND:X\r", gBody.c_str());
    TEST_ASSERT_EQUAL_INT64(1790841600, p.date());
    TEST_ASSERT_EQUAL_INT64(23, p.contentLength());
  }
}

void test_chunked_every_split() {
  std::string resp = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                     "5\r\nBEGIN\r\n"
                     "18;ext=1\r\n:VCALENDAR\r\nVERSION:2.0\r\r\n"
                     "0\r\nX-Trailer: yes\r\n\r\n";
  HttpResponseParser p;
  for (size_t cut = 0; cut < resp.size(); cut++) {
    TEST_ASSERT_TRUE(run(p, resp, cut, /*eof=*/false));
    TEST_ASSERT_TRUE(p.chunked());
    TEST_ASSERT_TRUE(p.done());
    TEST_ASSERT_EQUAL_STRING("BEGIN:VCALENDAR\r\nVERSION:2.0\r", gBody.c_str());
  }
}

void test_close_delimited_and_redirect() {
  std::string resp = "HTTP/1.0 200 OK\r\n\r\nline one\nline two";
  HttpResponseParser p;
  TEST_ASSERT_TRUE(run(p, resp, 7, /*eof=*/false));
  TEST_ASSERT_FALSE(p.done());                  // still waiting for close
  p.onEof();
  TEST_ASSERT_TRUE(p.done());
  TEST_ASSERT_EQUAL_STRING("line one\nline two", gBody.c_str());

  std::string redir = "HTTP/1.1 301 Moved Permanently\r\nLocation: https://cal.example.com/new.ics\r\n"
                      "Content-Length: 0\r\n\r\n";
  TEST_ASSERT_TRUE(run(p, redir, 10));
  TEST_ASSERT_EQUAL_INT(301, p.status());
  TEST_ASSERT_EQUAL_STRING("https://cal.example.com/new.ics", p.location());
  TEST_ASSERT_TRUE(p.done());
}

void test_interim_and_errors() {
  std::string resp = "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 404 Not Found\r\nContent-Length: 2\r\n\r\nno";
  HttpResponseParser p;
  TEST_ASSERT_TRUE(run(p, resp, 0));
  TEST_ASSERT_EQUAL_INT(404, p.status());
  TEST_ASSERT_EQUAL_STRING("no", gBody.c_str());

  TEST_ASSERT_FALSE(run(p, "SSH-2.0-OpenSSH\r\n", 3));
  TEST_ASSERT_TRUE(p.error());
  TEST_ASSERT_FALSE(run(p, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nZZ\r\n", 0));
  TEST_ASSERT_TRUE(p.error());

  std::string gz = "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: 1\r\n\r\nx";
  TEST_ASSERT_TRUE(run(p, gz, 5));
  TEST_ASSERT_TRUE(p.gzip());

  // Endless headers are cut off instead of eating memory.
  std::string big = "HTTP/1.1 200 OK\r\n";
  while (big.size() < 40000) big += "X-Padding: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n";
  TEST_ASSERT_FALSE(run(p, big, 1000));
  TEST_ASSERT_TRUE(p.error());
  // A single absurdly long header line is ignored, not fatal.
  std::string longHdr = "HTTP/1.1 200 OK\r\nX-Long: " + std::string(3000, 'a') +
                        "\r\nContent-Length: 2\r\n\r\nok";
  TEST_ASSERT_TRUE(run(p, longHdr, 0));
  TEST_ASSERT_EQUAL_STRING("ok", gBody.c_str());
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_parse_url);
  RUN_TEST(test_resolve_location);
  RUN_TEST(test_redact_url);
  RUN_TEST(test_http_date);
  RUN_TEST(test_content_length_every_split);
  RUN_TEST(test_chunked_every_split);
  RUN_TEST(test_close_delimited_and_redirect);
  RUN_TEST(test_interim_and_errors);
  return UNITY_END();
}
