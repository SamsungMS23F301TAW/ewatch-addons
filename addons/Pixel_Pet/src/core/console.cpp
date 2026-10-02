#include <Arduino.h>
#include <esp_task_wdt.h>
#include <stdlib.h>
#include <string.h>
#include "console.h"
#include "steps.h"
#include "petsvc.h"
#include "bgpower.h"
#include "controller.h"
#include "haptic.h"
#include "model.h"

static char s_line[64];
static uint8_t s_len = 0;

static void printStatus() {
  PetTime t = petTimeFromModel();
  StepsSnapshot ss = stepsSvcSnapshot();
  float vb; uint8_t pct; bool bok;
  { ModelLock lk; vb = model.vbat; pct = model.batPct; bok = model.batOk; }
  Serial.printf("time %04u-%02u-%02u %02u:%02u:%02u (%s) day=%lu\n", t.year, t.month, t.mday,
                t.hour, t.minute, t.second, t.ok ? "ok" : "NOT SET", (unsigned long)t.day);
  Serial.printf("steps today %lu lifetime %lu walking=%d cadence=%.0f spm\n",
                (unsigned long)ss.today, (unsigned long)ss.lifetime, ss.walking ? 1 : 0, ss.cadenceSpm);
  petSvcPrint();
  Serial.printf("battery %.2f V %u%% (%s) background=%s\n", vb, pct, bok ? "ok" : "n/a",
                bgActive() ? "on" : (bgSafeMode() ? "off (safe mode: crashed twice while dark)" : "off"));
  BgStats b = bgStats();
  Serial.printf("dark periods %lu, light sleeps %lu (sim %lu), wakes fifo %lu timer %lu btn %lu touch %lu jolt %lu\n",
                (unsigned long)b.screenOffs, (unsigned long)b.lightSleeps, (unsigned long)b.simulated,
                (unsigned long)b.wakeFifo, (unsigned long)b.wakeTimer, (unsigned long)b.wakeButton,
                (unsigned long)b.wakeTouch, (unsigned long)b.wakeJolt);
  Serial.printf("spurious touch %lu, int2 faults %lu, deep entries %lu, background boots %lu, samples %lu\n",
                (unsigned long)b.spuriousTouch, (unsigned long)b.int2Faults, (unsigned long)b.deepEntries,
                (unsigned long)b.bgBoots, (unsigned long)b.samples);
  Serial.printf("deep sleeps without motion wake %lu, deep entries put off (INT1 busy) %lu\n",
                (unsigned long)b.deepNoMotion, (unsigned long)b.deepBusy);
}

static void printHistory() {
  PetTime t = petTimeFromModel();
  if (!t.ok) { Serial.println("clock not set"); return; }
  for (int i = 13; i >= 0; i--) {
    uint32_t d = t.day - (uint32_t)i;
    Serial.printf("  day %lu%s: %lu\n", (unsigned long)d, i == 0 ? " (today)" : "",
                  (unsigned long)stepsSvcOnDay(d));
  }
}

static void dumpRecording() {
  uint32_t n, cap, counted;
  bool on;
  stepsSvcRecStatus(n, cap, counted, on);
  Serial.printf("# pixel-pet accel recording: %lu samples @12.5Hz, detector counted %lu steps\n",
                (unsigned long)n, (unsigned long)counted);
  Serial.println("x,y,z");
  int16_t buf[48 * 3];
  for (uint32_t i = 0; i < n;) {
    uint32_t got = stepsSvcRecCopy(i, buf, 48);
    if (!got) break;
    for (uint32_t k = 0; k < got; k++) {
      Serial.printf("%d,%d,%d\n", buf[k * 3], buf[k * 3 + 1], buf[k * 3 + 2]);
    }
    i += got;
    esp_task_wdt_reset();
    delay(2);
  }
  Serial.println("# end");
}

static void runCommand(char *cmd) {
  char *arg = strchr(cmd, ' ');
  if (arg) { *arg++ = 0; while (*arg == ' ') arg++; }
  if (!strcmp(cmd, "help")) {
    Serial.println("help status pet hist | steps <n> | full <0-100> | rec <s>|rec stop | dump | buzz | off");
  } else if (!strcmp(cmd, "status")) {
    printStatus();
  } else if (!strcmp(cmd, "pet")) {
    petSvcPrint();
  } else if (!strcmp(cmd, "hist")) {
    printHistory();
  } else if (!strcmp(cmd, "steps") && arg) {
    long n = atol(arg);
    if (n > 0 && n < 100000) {
      PetTime t = petTimeFromModel();
      stepsSvcInject((uint32_t)n, t.day);
      Serial.printf("added %ld steps\n", n);
    }
  } else if (!strcmp(cmd, "full") && arg) {
    petSvcDebugFullness((float)atof(arg));
    Serial.println("ok");
  } else if (!strcmp(cmd, "rec")) {
    if (arg && !strcmp(arg, "stop")) { stepsSvcRecStop(); Serial.println("recording stopped"); return; }
    uint32_t secs = arg ? (uint32_t)atol(arg) : 0;
    if (secs) {
      Serial.println(stepsSvcRecStart(secs) ? "recording (walk now; `dump` afterwards)" : "no PSRAM");
    } else {
      uint32_t n, cap, counted; bool on;
      stepsSvcRecStatus(n, cap, counted, on);
      Serial.printf("rec %s %lu/%lu samples, %lu steps\n", on ? "ON" : "off", (unsigned long)n,
                    (unsigned long)cap, (unsigned long)counted);
    }
  } else if (!strcmp(cmd, "dump")) {
    dumpRecording();
  } else if (!strcmp(cmd, "buzz")) {
    static const HapticStep kNom[] = {{110, 35, 70}, {110, 35, 70}, {175, 90, 0}};
    hapticPattern(kNom, 3);
  } else if (!strcmp(cmd, "off")) {
    requestScreenOff();
  } else if (*cmd) {
    Serial.println("? (try help)");
  }
}

void consolePoll() {
  while (Serial.available() > 0) {
    int c = Serial.read();
    if (c < 0) break;
    if (c == '\r' || c == '\n') {
      s_line[s_len] = 0;
      if (s_len) runCommand(s_line);
      s_len = 0;
    } else if (s_len < sizeof(s_line) - 1) {
      s_line[s_len++] = (char)c;
    }
  }
}
