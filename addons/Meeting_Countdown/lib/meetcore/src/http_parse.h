// Meeting Countdown — HTTP/1.1 response parsing and URL handling, kept free of
// any socket code so it can be unit-tested on the host. The watch feeds it the
// raw bytes read from WiFiClient(Secure); body bytes come out of a callback,
// already de-chunked, straight into the streaming ICS parser.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace mc {

struct Url {
  bool     tls = true;
  char     host[96] = "";
  uint16_t port = 443;
  char     path[400] = "/";       // path + query, never empty
};

// Accepts http://, https://, webcal:// (-> https) and webcals://. Rejects
// user-info, empty hosts and anything that does not fit the buffers.
bool parseUrl(const char *in, Url &out);

// Resolve a redirect Location (absolute, scheme-relative or path) against
// the URL that produced it.
bool resolveLocation(const Url &base, const char *location, Url &out);

// Writes "host/…/basic.ics" style text with the secret middle elided, safe to
// show on screen, in logs or on the web page.
void redactUrl(const char *url, char *out, size_t cap);

// Parses an IMF-fixdate ("Thu, 01 Oct 2026 09:30:00 GMT") into Unix seconds.
bool parseHttpDate(const char *s, int64_t &unixOut);

class HttpResponseParser {
public:
  typedef void (*BodyFn)(void *ctx, const char *data, size_t n);

  void begin(BodyFn fn, void *ctx);
  // Feed raw bytes from the socket. Returns false once a protocol error was
  // detected (malformed status line, bad chunk size, oversize headers).
  bool feed(const char *data, size_t n);
  // Call when the peer closed the connection.
  void onEof();

  bool headersDone() const { return state_ > S_HEADERS; }
  bool done() const { return state_ == S_DONE; }
  bool error() const { return state_ == S_ERROR; }
  int  status() const { return status_; }
  bool chunked() const { return chunked_; }
  int64_t contentLength() const { return contentLength_; }   // -1 if absent
  int64_t bodyBytes() const { return bodyBytes_; }
  const char *location() const { return location_; }
  const char *contentType() const { return contentType_; }
  bool gzip() const { return gzip_; }
  int64_t date() const { return date_; }                      // 0 if absent
  // True when the body ended the way the headers promised (length reached,
  // final chunk, or connection close without a length).
  bool complete() const { return done(); }

private:
  enum State : uint8_t {
    S_STATUS, S_HEADERS, S_BODY_LEN, S_BODY_CLOSE, S_CHUNK_SIZE, S_CHUNK_EXT,
    S_CHUNK_DATA, S_CHUNK_CRLF, S_TRAILER, S_DONE, S_ERROR
  };
  void headerLine();
  void startBody();

  BodyFn  fn_ = nullptr;
  void   *ctx_ = nullptr;
  State   state_ = S_STATUS;
  char    line_[512];
  size_t  lineLen_ = 0;
  bool    lineOverflow_ = false;
  int     status_ = 0;
  bool    chunked_ = false, gzip_ = false;
  int64_t contentLength_ = -1;
  int64_t remaining_ = 0;
  int64_t bodyBytes_ = 0;
  int64_t date_ = 0;
  uint32_t headerBytes_ = 0;
  char    location_[480];
  char    contentType_[48];
};

}  // namespace mc
