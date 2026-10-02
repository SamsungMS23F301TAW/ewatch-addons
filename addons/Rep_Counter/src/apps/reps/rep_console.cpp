// Rep Counter: serial console and REC streaming. See rep_console.h.
#include "rep_console.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <atomic>
#include <string.h>
#include "imu_stream.h"
#include "rep_store.h"
#include "rep_types.h"

#ifndef REPS_FW_VERSION
#define REPS_FW_VERSION "1.0.0"
#endif

#if !defined(REPS_ENABLE_REC) || REPS_ENABLE_REC

static std::atomic<bool> gRec{false};
static uint32_t gCursor = 0;
static uint32_t gLost = 0;
static uint8_t gFmt = 0xFF;            // flags of the format last announced
static uint32_t gRecStart = 0;
static uint32_t gNoHostSince = 0;
static QueueHandle_t gEvQ = nullptr;
static SemaphoreHandle_t gStatusMx = nullptr;
static char gStatus[96] = "app not open";
static char gLine[72];
static size_t gLinePos = 0;
static char gOut[640];                 // pending CSV text not yet accepted by USB
static size_t gOutLen = 0;

struct EvLine { char s[72]; };

static void ensureInit() {
  if (!gEvQ) gEvQ = xQueueCreate(8, sizeof(EvLine));
  if (!gStatusMx) gStatusMx = xSemaphoreCreateMutex();
}

bool repsRecActive() { return gRec.load(); }
bool repsConsoleBusy() { return gRec.load(); }

void repsConsoleEvent(const char *line) {
  if (!gRec.load() || !gEvQ) return;
  EvLine e;
  strncpy(e.s, line, sizeof(e.s) - 1);
  e.s[sizeof(e.s) - 1] = 0;
  xQueueSend(gEvQ, &e, 0);
}

void repsConsoleSetStatus(const char *line) {
  ensureInit();
  if (xSemaphoreTake(gStatusMx, 0) != pdTRUE) return;
  strncpy(gStatus, line, sizeof(gStatus) - 1);
  gStatus[sizeof(gStatus) - 1] = 0;
  xSemaphoreGive(gStatusMx);
}

// Push as much pending text as the USB buffer takes, never blocking.
static bool flushOut() {
  if (!gOutLen) return true;
  int room = Serial.availableForWrite();
  if (room <= 0) return false;
  size_t n = (size_t)room < gOutLen ? (size_t)room : gOutLen;
  size_t w = Serial.write((const uint8_t *)gOut, n);
  if (w < gOutLen) memmove(gOut, gOut + w, gOutLen - w);
  gOutLen -= w;
  return gOutLen == 0;
}

static void outAppend(const char *s) {
  size_t n = strlen(s);
  if (gOutLen + n > sizeof(gOut)) return;   // caller checks room first
  memcpy(gOut + gOutLen, s, n);
  gOutLen += n;
}

static void recStart() {
  gCursor = imuStreamHead();
  gLost = 0;
  gFmt = 0xFF;
  gOutLen = 0;
  gRecStart = millis();
  gNoHostSince = 0;
  imuStreamWant(IMU_CLIENT_REC, true);
  gRec.store(true);
  reps::Settings st;
  reps::DayLog unusedLog;
  reps::History unusedHist;
  repstore::load(st, unusedLog, unusedHist);
  Serial.println("# rep-counter rec v1");
  Serial.printf("# wrist=%s mode=%s set_end_s=%u hand=%d fw=%s\n",
                st.wrist == (uint8_t)reps::Wrist::Right ? "R" : "L",
                reps::modeName((reps::Mode)st.mode), st.setEndSec, st.handSign, REPS_FW_VERSION);
}

static void recStop(const char *why) {
  gRec.store(false);
  imuStreamWant(IMU_CLIENT_REC, false);
  flushOut();
  Serial.printf("# end %s samples_lost=%lu duration_s=%lu\n", why, (unsigned long)gLost,
                (unsigned long)((millis() - gRecStart) / 1000));
}

static void pumpRec() {
  if (!flushOut()) return;
  // Device events first, so they land next to the samples they describe.
  EvLine e;
  while (gOutLen + 80 < sizeof(gOut) && gEvQ && xQueueReceive(gEvQ, &e, 0) == pdTRUE) {
    outAppend("# ev ");
    outAppend(e.s);
    outAppend("\n");
  }
  ImuSample s;
  uint32_t lostBefore = gLost;
  while (gOutLen + 96 < sizeof(gOut) && imuStreamRead(gCursor, s, gLost)) {
#if !defined(REPS_IMU_FIFO) || REPS_IMU_FIFO
    if (!(s.flags & IMU_FLAG_FIFO)) continue;          // wait for the 100 Hz mode
#endif
    char ln[96];
    uint8_t fmt = (uint8_t)(s.flags & ~IMU_FLAG_GAP);
    if (fmt != gFmt) {
      // (Re)announce the format; the polled fallback isn't a fixed rate.
      bool fifo = fmt & IMU_FLAG_FIFO;
      snprintf(ln, sizeof ln, "# fs_hz=%d counts_per_g=%d range_g=%d\nseq,t_ms,ax,ay,az\n",
               fifo ? 100 : 45, (int)imuCountsPerG(fmt), (fmt & IMU_FLAG_4G) ? 4 : 2);
      outAppend(ln);
      gFmt = fmt;
    }
    if ((s.flags & IMU_FLAG_GAP) || gLost != lostBefore) {
      snprintf(ln, sizeof ln, "# gap lost=%lu\n", (unsigned long)(gLost - lostBefore));
      outAppend(ln);
      lostBefore = gLost;
    }
    snprintf(ln, sizeof ln, "%lu,%lu,%d,%d,%d\n", (unsigned long)s.seq, (unsigned long)s.tMs,
             s.x, s.y, s.z);
    outAppend(ln);
  }
  flushOut();
  // Host gone (cable pulled, terminal closed) for 10 s: stop, let it sleep.
  if (!Serial) {
    if (!gNoHostSince) gNoHostSince = millis();
    else if (millis() - gNoHostSince > 10000) recStop("no-host");
  } else {
    gNoHostSince = 0;
  }
}

static void cmdLog() {
  reps::DayLog log;
  if (!repstore::loadLog(log)) { Serial.println("# no log saved yet"); return; }
  Serial.printf("# date=%04u-%02u-%02u sets=%u reps=%u\n", log.year, log.month, log.day, log.count,
                log.totalReps());
  Serial.println("set,exercise,reps,start_hhmm,duration_s,rest_s,tempo_s,confidence");
  for (int i = 0; i < log.count && i < reps::kMaxSets; i++) {
    const reps::SetRecord &r = log.sets[i];
    char hhmm[8] = "-";
    if (r.startMin != 0xFFFF) snprintf(hhmm, sizeof hhmm, "%02u:%02u", r.startMin / 60, r.startMin % 60);
    char rest[8] = "-";
    if (r.restS != 0xFFFF) snprintf(rest, sizeof rest, "%u", r.restS);
    Serial.printf("%d,%s,%u,%s,%u,%s,%.2f,%u\n", i + 1, reps::exerciseName((reps::Exercise)r.exercise),
                  r.reps, hhmm, r.durationS, rest, r.tempoCs / 100.0, r.confidence);
  }
}

static void handleLine(char *l) {
  size_t n = strlen(l);
  while (n && (l[n - 1] == ' ' || l[n - 1] == '\r' || l[n - 1] == '\n')) l[--n] = 0;
  while (*l == ' ') l++;
  if (!*l) return;
  for (char *p = l; *p; p++) *p = (char)tolower((unsigned char)*p);
  if (!strcmp(l, "rec on")) {
    if (!gRec.load()) recStart();
  } else if (!strcmp(l, "rec off")) {
    if (gRec.load()) recStop("user");
  } else if (!strcmp(l, "status")) {
    char st[96];
    if (gStatusMx && xSemaphoreTake(gStatusMx, pdMS_TO_TICKS(20)) == pdTRUE) {
      strncpy(st, gStatus, sizeof st);
      st[sizeof st - 1] = 0;
      xSemaphoreGive(gStatusMx);
    } else {
      strcpy(st, "busy");
    }
    Serial.printf("# rep-counter: %s; imu stream %s; rec %s\n", st,
                  imuStreamWanted() ? "on" : "off", gRec.load() ? "on" : "off");
  } else if (!strcmp(l, "log")) {
    cmdLog();
  } else if (!strcmp(l, "help")) {
    Serial.println("# rep-counter commands: REC on | REC off | status | log | help");
  } else {
    Serial.printf("# unknown command '%s' (try help)\n", l);
  }
}

void repsConsolePoll() {
  ensureInit();
  while (Serial.available() > 0) {
    int c = Serial.read();
    if (c < 0) break;
    if (c == '\n' || c == '\r') {
      gLine[gLinePos] = 0;
      if (gLinePos) handleLine(gLine);
      gLinePos = 0;
    } else if (gLinePos < sizeof(gLine) - 1) {
      gLine[gLinePos++] = (char)c;
    }
  }
  if (gRec.load()) pumpRec();
}

#else  // REPS_ENABLE_REC == 0: no console at all

void repsConsolePoll() {}
bool repsConsoleBusy() { return false; }
bool repsRecActive() { return false; }
void repsConsoleEvent(const char *) {}
void repsConsoleSetStatus(const char *) {}

#endif
