// Meeting Countdown — feed sync engine. See mc_sync.h.
#include "mc_sync.h"

#if defined(EWATCH_ENABLE_WIFI) && EWATCH_ENABLE_WIFI

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <esp_sntp.h>
#include <esp_wifi.h>
#include <time.h>
#include <string.h>
#include <new>
#include "mc_app.h"
#include "ics_parser.h"
#include "http_parse.h"
#include "mc_text.h"
#include "model.h"
#include "storage.h"
#include "controller.h"
#include "wifi_svc.h"
#include "pins.h"

using namespace mc;
using mcapp::SyncResult;

// The ESP-IDF SDK that Arduino-ESP32 2.0.x ships already contains the
// Mozilla root bundle (136 roots, ~64 KB) as x509_crt_bundle.S in
// libmbedtls.a; referencing it links it in.
extern const uint8_t kCaBundleStart[] asm("_binary_x509_crt_bundle_start");

namespace mcsync {

// Budgets (ms). Every blocking step stays well under the 20 s task watchdog.
static const uint32_t kWifiBudgetMs    = 18000;
static const uint32_t kStallMs         = 12000;     // no bytes for this long = dead
static const uint32_t kTlsHandshakeS   = 10;
static const uint32_t kSocketTimeoutS  = 7;
static const uint32_t kInteractiveMs   = 75000;
static const int      kMaxRedirects    = 4;
static const int64_t  kHorizonSec      = 48 * 3600;

static SemaphoreHandle_t gProgMx = nullptr;
static Progress gProg;
static volatile bool gRunning = false;

static void setProgress(Phase ph, const char *step, uint8_t feed = 0, uint8_t feeds = 0, uint32_t bytes = 0) {
  if (!gProgMx) return;
  xSemaphoreTake(gProgMx, portMAX_DELAY);
  gProg.running = gRunning;
  gProg.phase = ph;
  gProg.feed = feed;
  gProg.feeds = feeds;
  gProg.bytes = bytes;
  if (step) copyStr(gProg.step, sizeof(gProg.step), step);
  xSemaphoreGive(gProgMx);
}

Progress progress() {
  Progress p;
  if (!gProgMx) return p;
  xSemaphoreTake(gProgMx, portMAX_DELAY);
  p = gProg;
  xSemaphoreGive(gProgMx);
  p.running = gRunning;
  return p;
}

bool running() { return gRunning; }

// ---------------------------------------------------------------------------
// context shared by one sync run
// ---------------------------------------------------------------------------
struct Ctx {
  bool     headless;
  uint32_t deadline;          // millis()
  bool     aborted;
};

// Headless only: a button press or a touch means "show me the watch". The
// CST816S INT line only pulses, so it is latched by an edge interrupt.
static volatile bool gTouchSeen = false;
static void IRAM_ATTR onTouchEdge() { gTouchSeen = true; }

static bool userWantsUi() {
  return gTouchSeen || digitalRead(PIN_BTN) == HIGH;    // button is active-high
}

static bool expired(Ctx &c) {
  esp_task_wdt_reset();
  if (!c.headless) mcapp::keepAwakeFor(15000);
  if (c.headless && userWantsUi()) { c.aborted = true; return true; }
  return (int32_t)(millis() - c.deadline) >= 0;
}

static void sleepMs(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

// ---------------------------------------------------------------------------
// radio
// ---------------------------------------------------------------------------
static bool headlessConnect(Ctx &c) {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  uint32_t until = millis() + kWifiBudgetMs;
  char ssid[33], pass[Storage::KNOWN_PASS];
  uint8_t bssid[6], channel;
  // Fast path: rejoin the access point that worked last time (no scan).
  if (mcapp::lastAp(ssid, bssid, channel) && Storage::knownLookup(ssid, pass)) {
    WiFi.begin(ssid, pass, channel, bssid);
    uint32_t t0 = millis();
    while (millis() - t0 < 4000) {
      if (WiFi.status() == WL_CONNECTED) return true;
      if (expired(c)) return false;
      sleepMs(50);
    }
    WiFi.disconnect(false, true);
    sleepMs(100);
  }
  // Scan and pick the strongest saved network.
  setProgress(Phase::Connecting, "Looking for WiFi...");
  int n = WiFi.scanNetworks(false, false, false, 110);
  esp_task_wdt_reset();
  int best = -1;
  int8_t bestRssi = -127;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (!s.length() || !Storage::knownLookup(s.c_str(), pass)) continue;
    if (WiFi.RSSI(i) > bestRssi) { bestRssi = (int8_t)WiFi.RSSI(i); best = i; }
  }
  if (best < 0) { WiFi.scanDelete(); return false; }
  String bs = WiFi.SSID(best);
  copyStr(ssid, sizeof(ssid), bs.c_str());
  Storage::knownLookup(ssid, pass);
  uint8_t ch = (uint8_t)WiFi.channel(best);
  uint8_t *bb = WiFi.BSSID(best);
  memcpy(bssid, bb, 6);
  WiFi.scanDelete();
  WiFi.begin(ssid, pass, ch, bssid);
  while ((int32_t)(millis() - until) < 0) {
    if (WiFi.status() == WL_CONNECTED) {
      mcapp::rememberAp(ssid, bssid, ch);
      return true;
    }
    if (expired(c)) return false;
    sleepMs(50);
  }
  return false;
}

static void headlessDisconnect() {
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
}

static bool interactiveConnect(Ctx &c) {
  wifiSvcLease(true);
  uint32_t until = millis() + kWifiBudgetMs + 6000;   // the service scans first
  while ((int32_t)(millis() - until) < 0) {
    bool conn;
    { ModelLock lk; conn = model.wifiConnected; }
    if (conn && WiFi.status() == WL_CONNECTED) {
      uint8_t ch = (uint8_t)WiFi.channel();
      mcapp::rememberAp(WiFi.SSID().c_str(), WiFi.BSSID(), ch);
      return true;
    }
    if (expired(c)) return false;
    sleepMs(100);
  }
  return false;
}

// ---------------------------------------------------------------------------
// clock
// ---------------------------------------------------------------------------
// The ESP32 keeps its system clock running through deep sleep, so time()
// can look plausible long before SNTP answers. Only trust a real sync.
static volatile bool gSntpSynced = false;
static void onSntpSync(struct timeval *) { gSntpSynced = true; }

static bool ntpUtc(int64_t &utc, Ctx &c) {
  gSntpSynced = false;
  sntp_set_time_sync_notification_cb(onSntpSync);
  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  uint32_t t0 = millis();
  bool ok = false;
  while (millis() - t0 < 4000) {
    if (gSntpSynced) {
      time_t t = time(nullptr);
      if (t > 1735689600) { utc = (int64_t)t; ok = true; }       // sanity: after 2025-01-01
      break;
    }
    if (expired(c)) break;
    sleepMs(50);
  }
  sntp_stop();
  sntp_set_time_sync_notification_cb(nullptr);
  return ok;
}

// Writes the RTC (local time = UTC + offset) the right way for the context.
static void setClock(int64_t utc, int tz, bool headless) {
  Civil cv = secondsToCivil(utc + (int64_t)tz * 60);
  if (headless) {
    writeRTC((uint8_t)cv.hour, (uint8_t)cv.minute, (uint8_t)cv.second, (uint8_t)cv.weekday,
             (uint8_t)cv.day, (uint8_t)cv.month, (uint16_t)cv.year);
    ModelLock lk;
    model.hour = (uint8_t)cv.hour; model.minute = (uint8_t)cv.minute; model.second = (uint8_t)cv.second;
    model.weekday = (uint8_t)cv.weekday; model.day = (uint8_t)cv.day; model.month = (uint8_t)cv.month;
    model.year = (uint16_t)cv.year; model.rtcOk = true;
  } else {
    requestSetRTC((uint8_t)cv.hour, (uint8_t)cv.minute, (uint8_t)cv.second, (uint8_t)cv.weekday,
                  (uint8_t)cv.day, (uint8_t)cv.month, (uint16_t)cv.year);
  }
}

// ---------------------------------------------------------------------------
// HTTP(S) fetch streamed into the parser
// ---------------------------------------------------------------------------
struct FetchOut {
  SyncResult res = SyncResult::Ok;
  int        http = 0;
  uint32_t   bytes = 0;
  int64_t    date = 0;
};

struct BodyCtx {
  HttpResponseParser *hp;
  IcsParser *ics;
  uint32_t bytes;
};

static void onBody(void *vctx, const char *data, size_t n) {
  BodyCtx *b = (BodyCtx *)vctx;
  if (b->hp->status() != 200 || b->hp->gzip()) return;      // error/redirect bodies are ignored
  b->ics->feed(data, n);
  b->bytes += (uint32_t)n;
}

static FetchOut fetchFeed(const char *urlStr, IcsParser *ics, Ctx &c, bool insecure, uint8_t feedNo,
                          uint8_t feeds) {
  FetchOut out;
  Url url;
  if (!parseUrl(urlStr, url)) { out.res = SyncResult::BadData; return out; }
  static uint8_t buf[1460];
  static HttpResponseParser hp;
  BodyCtx bctx;

  for (int hop = 0; hop <= kMaxRedirects; hop++) {
    if (url.tls) {
      // mbedTLS allocates from internal RAM (CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC):
      // a 16 KB record buffer plus handshake state, ~45 KB in all. Refuse
      // instead of failing half-way (an alloc failure here once hung the
      // WiFi task in EWatchOS).
      size_t freeInt = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      size_t block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      if (freeInt < 52000 || block < 20000) {
        Serial.printf("MC: TLS skipped, internal heap %u (block %u)\n", (unsigned)freeInt, (unsigned)block);
        out.res = SyncResult::LowMemory;
        return out;
      }
    }
    char step[48];
    snprintf(step, sizeof(step), feeds > 1 ? "Contacting %s (%u/%u)" : "Contacting %s", url.host,
             (unsigned)feedNo, (unsigned)feeds);
    setProgress(Phase::Fetching, step, feedNo, feeds, 0);

    IPAddress ip;
    esp_task_wdt_reset();
    if (!WiFi.hostByName(url.host, ip)) { out.res = SyncResult::DnsFailed; return out; }
    if (expired(c)) { out.res = c.aborted ? SyncResult::Aborted : SyncResult::Timeout; return out; }

    WiFiClient *cl;
    WiFiClientSecure *tls = nullptr;
    WiFiClient *plain = nullptr;
    bool ok;
    esp_task_wdt_reset();
    if (url.tls) {
      tls = new WiFiClientSecure();
      if (insecure) tls->setInsecure();
#if defined(MC_TLS_VERIFY) && MC_TLS_VERIFY
      else tls->setCACertBundle(kCaBundleStart);
#else
      else tls->setInsecure();
#endif
      tls->setHandshakeTimeout(kTlsHandshakeS);
      tls->setTimeout(kSocketTimeoutS);
      ok = tls->connect(ip, url.port, url.host, nullptr, nullptr, nullptr);
      cl = tls;
    } else {
      plain = new WiFiClient();
      ok = plain->connect(ip, url.port, (int32_t)kSocketTimeoutS * 1000);
      cl = plain;
    }
    esp_task_wdt_reset();
    if (!ok) {
      if (tls) {
        char err[96] = "";
        tls->lastError(err, sizeof(err));
        Serial.printf("MC: TLS connect to %s failed: %s\n", url.host, err);
      }
      out.res = tls ? SyncResult::TlsFailed : SyncResult::ConnectFailed;
      delete tls;
      delete plain;
      return out;
    }

    // Request. Identity encoding: the parser wants plain text.
    String req;
    req.reserve(strlen(url.path) + strlen(url.host) + 200);
    req += "GET ";
    req += url.path;
    req += " HTTP/1.1\r\nHost: ";
    req += url.host;
    if ((url.tls && url.port != 443) || (!url.tls && url.port != 80)) { req += ':'; req += url.port; }
    req += "\r\nUser-Agent: EWatch-MeetingCountdown/1.0\r\n"
           "Accept: text/calendar, text/plain;q=0.9, */*;q=0.5\r\n"
           "Accept-Encoding: identity\r\nConnection: close\r\n\r\n";
    cl->write((const uint8_t *)req.c_str(), req.length());

    bctx.hp = &hp;
    bctx.ics = ics;
    bctx.bytes = 0;
    hp.begin(onBody, &bctx);
    uint32_t lastData = millis(), lastProg = 0;
    bool stalled = false;
    for (;;) {
      if (expired(c)) break;
      int avail = cl->available();
      if (avail > 0) {
        int n = cl->read(buf, avail < (int)sizeof(buf) ? (size_t)avail : sizeof(buf));
        if (n > 0) {
          hp.feed((const char *)buf, (size_t)n);
          lastData = millis();
          if (hp.headersDone() && hp.status() != 200) break;   // redirect or error: headers suffice
          if (hp.done() || hp.error()) break;
          if (bctx.bytes - lastProg >= 16384) {
            lastProg = bctx.bytes;
            char s[48];
            snprintf(s, sizeof(s), "Downloading %u KB...", (unsigned)(bctx.bytes / 1024));
            setProgress(Phase::Fetching, s, feedNo, feeds, bctx.bytes);
          }
          continue;
        }
      }
      if (!cl->connected() && cl->available() <= 0) { hp.onEof(); break; }
      if (millis() - lastData > kStallMs) { stalled = true; break; }
      sleepMs(2);
    }
    cl->stop();
    delete tls;
    delete plain;
    out.http = hp.status();
    out.bytes += bctx.bytes;
    if (hp.date()) out.date = hp.date();
    if (c.aborted) { out.res = SyncResult::Aborted; return out; }

    int st = hp.status();
    if ((st == 301 || st == 302 || st == 303 || st == 307 || st == 308) && hp.location()[0]) {
      Url next;
      if (!resolveLocation(url, hp.location(), next)) { out.res = SyncResult::HttpError; return out; }
      url = next;
      continue;
    }
    if (st != 200) { out.res = st ? SyncResult::HttpError : SyncResult::Timeout; return out; }
    if (hp.gzip()) { out.res = SyncResult::BadData; return out; }
    if (!hp.done() || stalled) { out.res = SyncResult::Timeout; return out; }
    out.res = SyncResult::Ok;
    return out;
  }
  out.res = SyncResult::HttpError;      // redirect loop
  return out;
}

// ---------------------------------------------------------------------------
// the run
// ---------------------------------------------------------------------------
static bool runSync(Ctx &c) {
  mcapp::SyncInfo info = mcapp::syncInfo();
  mcapp::Settings set = mcapp::settings();
  int64_t now = 0;
  bool haveNow = mcapp::nowUtc(now);
  if (haveNow) info.lastAttempt = now;
  mcapp::markSyncActive(true);
  mcapp::clearSyncRequest();
  setProgress(Phase::Connecting, "Starting...");

  char urls[mcapp::kMaxFeeds][mcapp::kUrlMax];
  int feeds = 0;
  for (int i = 0; i < mcapp::kMaxFeeds; i++) if (mcapp::feedUrl(i, urls[i], sizeof(urls[i]))) feeds++;
  else urls[i][0] = '\0';

  SyncResult overall = SyncResult::Ok;
  bool feedOk[mcapp::kMaxFeeds] = {false, false, false};
  bool anyOk = false;
  bool aborted = false;
  Event *results = nullptr;
  int nResults = 0;
  IcsParser *ics = nullptr;

  if (feeds == 0) { overall = SyncResult::NoFeed; goto finish; }
  if (Storage::knownCount() == 0) { overall = SyncResult::NoWifi; goto finish; }

  setProgress(Phase::Connecting, "Connecting to WiFi...");
  if (!(c.headless ? headlessConnect(c) : interactiveConnect(c))) {
    overall = c.aborted ? SyncResult::Aborted : SyncResult::WifiFailed;
    goto disconnect;
  }

  {
    setProgress(Phase::Clock, "Setting the clock...");
    int64_t ntp;
    int tz = mcapp::tzOffsetMin();
    bool clockOk = ntpUtc(ntp, c);
    if (clockOk) {
      if (!haveNow || llabs(ntp - now) > 2) setClock(ntp, tz, c.headless);
      now = ntp;
      haveNow = true;
    }
    if (c.aborted) { overall = SyncResult::Aborted; goto disconnect; }

    void *mem = heap_caps_malloc(sizeof(IcsParser), MALLOC_CAP_SPIRAM);
    if (!mem) mem = malloc(sizeof(IcsParser));
    if (mem) ics = new (mem) IcsParser();          // ~17 KB, PSRAM preferred
    size_t resCap = (size_t)mcapp::kMaxFeedEv * mcapp::kMaxFeeds;
    results = (Event *)heap_caps_malloc(sizeof(Event) * resCap, MALLOC_CAP_SPIRAM);
    if (!results) results = (Event *)malloc(sizeof(Event) * resCap);
    if (!ics || !results) { overall = SyncResult::LowMemory; goto disconnect; }

    int feedNo = 0;
    bool calTzTaken = false;
    for (int i = 0; i < mcapp::kMaxFeeds; i++) {
      if (!urls[i][0]) continue;
      feedNo++;
      if (!haveNow) { info.feed[i].result = (uint8_t)SyncResult::Timeout; overall = SyncResult::Timeout; continue; }
      IcsOptions o;
      o.windowStart = now;
      o.windowEnd = now + kHorizonSec;
      o.fallbackOffsetMin = mcapp::tzOffsetMin();
      o.feedIndex = (uint8_t)i;
      o.source = (uint8_t)Source::Feed;
      o.maxEvents = mcapp::kMaxFeedEv;
      o.maxAllDay = 4;
      ics->begin(o);
      FetchOut fo = fetchFeed(urls[i], ics, c, set.insecureTls != 0, (uint8_t)feedNo, (uint8_t)feeds);
      if (fo.res == SyncResult::Ok) {
        ics->finish();
        if (!ics->stats().sawCalendar) fo.res = SyncResult::BadData;
      }
      if (!clockOk && fo.date && fo.res == SyncResult::Ok) {
        // No NTP: the server's Date header is good to a second or so.
        if (llabs(fo.date - now) > 2) setClock(fo.date, mcapp::tzOffsetMin(), c.headless);
        now = fo.date;
        clockOk = true;
      }
      mcapp::FeedStatus &fs = info.feed[i];
      fs.result = (uint8_t)fo.res;
      fs.http = (int16_t)fo.http;
      fs.bytes = fo.bytes;
      fs.events = 0;
      const IcsStats &st = ics->stats();
      Serial.printf("MC: feed %d: %s http=%d bytes=%u vevents=%u kept=%d\n", i,
                    mcapp::syncResultText(fo.res), fo.http, (unsigned)fo.bytes, st.events, ics->count());
      if (fo.res == SyncResult::Ok) {
        feedOk[i] = true;
        anyOk = true;
        fs.events = (uint16_t)ics->count();
        for (int k = 0; k < ics->count() && nResults < (int)resCap; k++) results[nResults++] = ics->event(k);
        if (!calTzTaken && (st.calName[0] || st.calTz[0])) {
          calTzTaken = true;
          copyStr(info.calName, sizeof(info.calName), st.calName);
          copyStr(info.calTz, sizeof(info.calTz), st.calTz);
          info.calTzResolved = st.calTzResolved;
          info.calTzOffset = (int16_t)st.calTzOffsetMin;
        }
      } else {
        if (overall == SyncResult::Ok) overall = fo.res;
      }
      if (fo.res == SyncResult::Aborted) { aborted = true; break; }
    }
  }

disconnect:
  setProgress(Phase::Saving, "Saving...");
  if (c.headless) headlessDisconnect();
  else wifiSvcLease(false);

finish:
  if (c.aborted) aborted = true;
  if (anyOk) {
    mcapp::applyFeedResults(results, nResults, feedOk);
    // Follow the calendar's time zone through daylight-saving changes. Only
    // DST-sized steps (<= 1 h) are applied automatically: a bigger gap means
    // the calendar lives in another zone than the wearer, and the web page
    // offers to fix the clock from the phone instead.
    if (set.autoTz && info.calTzResolved) {
      int cur = mcapp::tzOffsetMin();
      int delta = info.calTzOffset - cur;
      if (delta != 0 && delta >= -60 && delta <= 60) {
        int64_t utc;
        bool haveUtc = mcapp::nowUtc(utc);
        { ModelLock lk; model.tzOffsetMin = info.calTzOffset; model.revision++; }
        Storage::save();
        if (haveUtc) setClock(utc, info.calTzOffset, c.headless);
        info.tzChangedFrom = (int16_t)cur;
        Serial.printf("MC: time zone %s: %+d -> %+d min\n", info.calTz, cur, info.calTzOffset);
      }
    }
    info.lastSuccess = haveNow ? now : info.lastAttempt;
    info.failures = 0;
    info.last = overall == SyncResult::Ok ? SyncResult::Ok : SyncResult::Partial;
  } else {
    info.last = aborted ? SyncResult::Aborted : overall;
    if (!aborted && overall != SyncResult::NoFeed) info.failures = (uint8_t)(info.failures < 250 ? info.failures + 1 : 250);
  }
  if (aborted && !anyOk) {
    // Interrupted by the user: retry soon, without counting a failure.
    mcapp::requestSync();
  }
  mcapp::recordSync(info);
  mcapp::markSyncActive(false);
  if (ics) { ics->~IcsParser(); free(ics); }
  free(results);
  setProgress(Phase::Done, anyOk ? "Up to date" : mcapp::syncResultText(info.last));
  Serial.printf("MC: sync %s (%d events from %d feeds)\n", mcapp::syncResultText(info.last), nResults, feeds);
  return !aborted;
}

// ---------------------------------------------------------------------------
// entry points
// ---------------------------------------------------------------------------
static void syncTask(void *) {
  esp_task_wdt_add(nullptr);
  Ctx c;
  c.headless = false;
  c.deadline = millis() + kInteractiveMs;
  c.aborted = false;
  runSync(c);
  { ModelLock lk; model.revision++; }
  esp_task_wdt_delete(nullptr);
  gRunning = false;
  vTaskDelete(nullptr);
}

bool startAsync() {
  if (!gProgMx) gProgMx = xSemaphoreCreateMutex();
  if (gRunning) return false;
  gRunning = true;
  setProgress(Phase::Connecting, "Starting...");
  mcapp::keepAwakeFor(20000);
  // 16 KB: TLS handshake + our frames; internal RAM (tasks must not run
  // from PSRAM stacks while flash writes disable the cache).
  if (xTaskCreatePinnedToCore(syncTask, "mcsync", 16384, nullptr, 3, nullptr, 0) != pdPASS) {
    gRunning = false;
    return false;
  }
  return true;
}

static volatile bool gHeadlessDone = false;
static volatile bool gHeadlessCompleted = false;
static uint32_t gHeadlessBudget = 0;

static void headlessTask(void *) {
  esp_task_wdt_add(nullptr);
  Ctx c;
  c.headless = true;
  c.deadline = millis() + gHeadlessBudget;
  c.aborted = false;
  gHeadlessCompleted = runSync(c);
  esp_task_wdt_delete(nullptr);
  gHeadlessDone = true;
  vTaskDelete(nullptr);
}

bool runHeadless(uint32_t budgetMs) {
  if (!gProgMx) gProgMx = xSemaphoreCreateMutex();
  gRunning = true;
  gHeadlessDone = false;
  gHeadlessCompleted = false;
  gHeadlessBudget = budgetMs;
  gTouchSeen = false;
  pinMode(PIN_TOUCH_INT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_TOUCH_INT), onTouchEdge, FALLING);
  // Run on a 16 KB task: loopTask's 8 KB stack is too small for a TLS handshake.
  if (xTaskCreatePinnedToCore(headlessTask, "mcsync", 16384, nullptr, 3, nullptr, 0) != pdPASS) {
    gRunning = false;
    return true;
  }
  uint32_t hardStop = millis() + budgetMs + 15000;
  while (!gHeadlessDone && (int32_t)(millis() - hardStop) < 0) {
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  detachInterrupt(digitalPinToInterrupt(PIN_TOUCH_INT));
  gRunning = false;
  return gHeadlessDone ? gHeadlessCompleted : true;
}

}  // namespace mcsync

#else  // WiFi compiled out

namespace mcsync {
bool startAsync() { return false; }
bool running() { return false; }
Progress progress() { return Progress(); }
bool runHeadless(uint32_t) { return true; }
}  // namespace mcsync

#endif
