#include "console.h"
#include <Arduino.h>
#include <esp_task_wdt.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "model.h"
#include "artslots.h"
#include "daystore.h"
#include "dayrec.h"
#include "steps_hw.h"
#include "power_bg.h"
#include "gf_settings.h"
#include "gf_art.h"
#include "gf_face.h"
#include "gf_date.h"

// ---- log gate: other tasks' log lines vs. SNAP's binary stream
static portMUX_TYPE sLogMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool sStreaming = false;
static volatile int  sLogWriters = 0;

void gfLog(const char *fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
  portENTER_CRITICAL(&sLogMux);
  bool ok = !sStreaming;
  if (ok) sLogWriters++;
  portEXIT_CRITICAL(&sLogMux);
  if (!ok) return;
  Serial.write((const uint8_t *)buf, (size_t)n);
  portENTER_CRITICAL(&sLogMux);
  sLogWriters--;
  portEXIT_CRITICAL(&sLogMux);
}

namespace {

void streamBegin() {
  portENTER_CRITICAL(&sLogMux);
  sStreaming = true;
  portEXIT_CRITICAL(&sLogMux);
  for (int i = 0; i < 200 && sLogWriters > 0; i++) delay(1);   // a line already on its way
}

void streamEnd() { sStreaming = false; }

bool writeAll(const uint8_t *p, size_t n) {
  uint32_t t0 = millis();
  size_t done = 0;
  while (done < n) {
    size_t k = Serial.write(p + done, n - done > 256 ? 256 : n - done);
    done += k;
    if (k == 0) {
      if (!Serial || millis() - t0 > 4000) return false;     // host stopped reading
      delay(2);
    } else {
      t0 = millis();
    }
    esp_task_wdt_reset();
  }
  return true;
}

uint16_t todayIndex(bool &ok) {
  uint16_t y; uint8_t m, d; bool r;
  { ModelLock lk; y = model.year; m = model.month; d = model.day; r = model.rtcOk; }
  ok = r && gf::plausibleDate(y, m, d);
  return ok ? gf::dayIndex(y, m, d) : 0;
}

// Renders the whole job in slices so other tasks keep running.
void renderAll(gf::ArtJob &job) {
  while (!artRunFor(job, 15000)) {
    esp_task_wdt_reset();
    vTaskDelay(1);
  }
}

void cmdSnap(char *args) {
  ArtSlot *slot = artSlotShared();
  if (!slot) { Serial.println("SNAP ERR no memory"); return; }
  bool todayOk;
  uint16_t today = todayIndex(todayOk);
  bool clockFace = true, plain = false;
  uint16_t day = today;
  uint32_t steps = 0;
  uint8_t algo = gf::ALGO_VERSION;
  char *tok = strtok(args, " ");
  if (tok) {
    if (!gf::parseIsoDate(tok, day)) { Serial.println("SNAP ERR bad date (use YYYY-MM-DD)"); return; }
    clockFace = false;
    tok = strtok(nullptr, " ");
    if (tok && strcasecmp(tok, "PLAIN") == 0) plain = true;
  }
  if (day == today && stepsDay() == today) {
    steps = stepsToday();
  } else {
    gf::DayRecord r;
    if (!daystoreFind(day, r)) { Serial.println("SNAP ERR no record for that day"); return; }
    steps = r.steps;
    algo = (r.algo == 0 || r.algo > gf::ALGO_VERSION) ? gf::ALGO_VERSION : r.algo;
  }
  if (!artSharedLock(3000)) { Serial.println("SNAP ERR busy"); return; }
  slot->owner = kOwnerSnap;
  slot->job.begin(&slot->canvas, slot->scratch, day, gf::bucketForSteps(steps), algo);
  renderAll(slot->job);
  const gf::ArtSpec &spec = slot->job.spec();

  gf::FaceInputs in;
  in.day = day;
  in.steps = steps;
  if (clockFace) {
    GfSettings set = gfSettings();
    uint8_t batPct; bool batOk;
    { ModelLock lk; in.hour = model.hour; in.minute = model.minute; in.rtcOk = model.rtcOk;
      batPct = model.batPct; batOk = model.batOk; }
    in.mode = gf::kModeClock;
    in.goal = set.goal;
    in.ambient = set.ambient;
    in.timeUnset = !todayOk;
    in.batteryLow = batOk && batPct <= 10;
  } else {
    in.mode = plain ? gf::kModePlain : gf::kModeGallery;
    in.isToday = (day == today);
  }
  uint8_t tones = gf::chooseTones(slot->canvas, spec, in.mode, spec.pal.darkBg ? 0 : 3);
  gf::buildFaceLayer(spec, in, tones, slot->layer);

  char iso[11];
  gf::formatIsoDate(day, iso);
  streamBegin();
  Serial.printf("SNAP BEGIN w=%d h=%d fmt=RGB565LE bytes=%d day=%s steps=%lu algo=%u family=%s\n",
                gf::kW, gf::kH, gf::kW * gf::kH * 2, iso, (unsigned long)steps,
                (unsigned)algo, gf::familyName(spec.family));
  static uint16_t rows[gf::kW * 8];
  uint32_t crc = 0;
  bool ok = true;
  for (int32_t y = 0; y < gf::kH && ok; y += 8) {
    gf::composeFrame(slot->canvas, spec, in, slot->layer, rows, y, y + 8);
    static uint8_t bytes[gf::kW * 8 * 2];
    for (int32_t i = 0; i < gf::kW * 8; i++) {
      bytes[2 * i] = (uint8_t)(rows[i] & 0xFF);
      bytes[2 * i + 1] = (uint8_t)(rows[i] >> 8);
    }
    crc = gf::crc32(bytes, sizeof(bytes), crc);
    ok = writeAll(bytes, sizeof(bytes));
  }
  slot->layer.valid = false;
  artSharedUnlock();
  if (ok) Serial.printf("\nSNAP END crc32=%08lx\n", (unsigned long)crc);
  streamEnd();
}

void cmdDays() {
  int32_t n = daystoreCount();
  char iso[11];
  for (int32_t i = 0; i < n; i++) {
    gf::DayRecord r;
    if (!daystoreAt(i, r)) continue;
    gf::formatIsoDate(r.day, iso);
    gf::ArtSpec spec;
    gf::describeDay(r.day, r.algo ? r.algo : gf::ALGO_VERSION, spec);
    Serial.printf("DAY %s %lu v%u %s\n", iso, (unsigned long)r.steps, (unsigned)r.algo,
                  gf::familyName(spec.family));
    if ((i & 31) == 31) { esp_task_wdt_reset(); delay(1); }
  }
  bool ok;
  uint16_t today = todayIndex(ok);
  if (ok) {
    gf::formatIsoDate(today, iso);
    Serial.printf("TODAY %s %lu\n", iso, (unsigned long)stepsToday());
  }
  Serial.printf("DAYS END %ld\n", (long)n);
}

void cmdArtHash(char *args) {
  char *d = strtok(args, " ");
  char *s = strtok(nullptr, " ");
  uint16_t day;
  if (!d || !s || !gf::parseIsoDate(d, day)) { Serial.println("ARTHASH ERR usage: ARTHASH YYYY-MM-DD STEPS"); return; }
  uint32_t steps = (uint32_t)strtoul(s, nullptr, 10);
  ArtSlot *slot = artSlotShared();
  if (!slot || !artSharedLock(3000)) { Serial.println("ARTHASH ERR busy"); return; }
  slot->owner = kOwnerSnap;
  uint32_t t0 = millis();
  slot->job.begin(&slot->canvas, slot->scratch, day, gf::bucketForSteps(steps));
  renderAll(slot->job);
  uint32_t ms = millis() - t0;
  uint32_t h = gf::canvasHash(slot->canvas);
  uint32_t ops = slot->job.opsDone();
  artSharedUnlock();
  Serial.printf("ARTHASH %s %lu v%u %08lx (%lu ms, %lu ops)\n", d, (unsigned long)steps,
                (unsigned)gf::ALGO_VERSION, (unsigned long)h, (unsigned long)ms, (unsigned long)ops);
}

void cmdDayAdd(char *args) {
  char *d = strtok(args, " ");
  char *s = strtok(nullptr, " ");
  uint16_t day;
  if (!d || !s || !gf::parseIsoDate(d, day)) { Serial.println("DAYADD ERR usage: DAYADD YYYY-MM-DD STEPS"); return; }
  gf::DayRecord r;
  r.day = day;
  r.steps = (uint32_t)strtoul(s, nullptr, 10);
  r.algo = gf::ALGO_VERSION;
  Serial.println(daystoreAdd(r) ? "DAYADD OK" : "DAYADD ERR storage");
}

void handle(char *line) {
  size_t n = strlen(line);
  while (n && (line[n - 1] == '\r' || line[n - 1] == '\n' || line[n - 1] == ' ')) line[--n] = 0;
  while (*line == ' ') line++;
  if (!*line) return;
  char *args = strchr(line, ' ');
  if (args) *args++ = 0; else args = line + strlen(line);
  if (!strcasecmp(line, "HELP")) {
    Serial.println("Dayprint console: SNAP | SNAP YYYY-MM-DD [PLAIN] | DAYS | ARTHASH YYYY-MM-DD STEPS |");
    Serial.println("                  STEPS | PWR | DAYADD YYYY-MM-DD STEPS | HELP");
  } else if (!strcasecmp(line, "SNAP"))    cmdSnap(args);
  else if (!strcasecmp(line, "DAYS"))      cmdDays();
  else if (!strcasecmp(line, "ARTHASH"))   cmdArtHash(args);
  else if (!strcasecmp(line, "STEPS"))     stepsDebugPrint(Serial);
  else if (!strcasecmp(line, "PWR"))       powerBgStatsPrint(Serial);
  else if (!strcasecmp(line, "DAYADD"))    cmdDayAdd(args);
  else Serial.printf("ERR unknown command '%s' (try HELP)\n", line);
}
}  // namespace

void consolePoll() {
  static char buf[96];
  static size_t pos = 0;
  while (Serial.available()) {
    int c = Serial.read();
    if (c < 0) break;
    if (c == '\n' || c == '\r') {
      buf[pos] = 0;
      if (pos) handle(buf);
      pos = 0;
    } else if (pos < sizeof(buf) - 1) {
      buf[pos++] = (char)c;
    }
  }
}
