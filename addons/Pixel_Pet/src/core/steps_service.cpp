// On-watch step service (declared in steps.h). Owns the single Detector and
// StepBook; fed raw FIFO samples by whichever task owns the I2C bus.
//
// Persistence layers:
//   * RTC slow memory mirror (RTC_NOINIT_ATTR, magic + checksum) — survives
//     deep sleep and panic/watchdog resets (RTC_DATA_ATTR would not: the
//     bootloader re-initialises it on every reset but a deep-sleep wake),
//     updated on every change (a memcpy). Garbage after a power cycle, which
//     the checks reject.
//   * NVS checkpoint in the addon namespace — survives power-off; written
//     every 250 steps, every 10 min with changes, at midnight, and on demand
//     (deep sleep / power-off). Render task only.
#include <Arduino.h>
#include <esp_attr.h>
#include <type_traits>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "steps.h"
#include "accel.h"
#include "addonstore.h"

namespace {

steps::DetectorConfig detectorConfig() {
  steps::DetectorConfig c;
  c.sampleHz = accel::kOdrHz;
  return c;
}

steps::Detector  s_det(detectorConfig());
steps::StepBook  s_book;
SemaphoreHandle_t s_lock = nullptr;
uint32_t s_savedLifetime = 0, s_savedDay = 0, s_lastSaveMs = 0, s_lastStepMs = 0;
bool     s_dirty = false;

struct Lock {
  Lock()  { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
  ~Lock() { if (s_lock) xSemaphoreGive(s_lock); }
};

// ---- accelerometer recorder (serial console: `rec`, `dump`) -----------------
int16_t *s_rec = nullptr;
uint32_t s_recCap = 0, s_recLen = 0;
uint32_t s_recSteps = 0;
bool     s_recOn = false;

}  // namespace

// No constructor may touch these: it would wipe the mirror at every boot.
static_assert(std::is_trivially_default_constructible<steps::StepBook>::value,
              "the RTC mirror must stay a plain struct");
RTC_NOINIT_ATTR static steps::StepBook s_rtcBook;
RTC_NOINIT_ATTR static uint32_t        s_rtcCrc;

static void mirror() {
  s_rtcBook = s_book;
  s_rtcCrc = addonstore::fnv1a(&s_rtcBook, sizeof(s_rtcBook));
}

void stepsSvcInit(bool trustRtcMirror) {
  if (!s_lock) s_lock = xSemaphoreCreateMutex();
  // The RTC mirror is valid after deep sleep and software/panic/watchdog
  // resets; garbage after power-on (magic + checksum reject it). Prefer
  // whichever copy has counted more steps (the lifetime count only grows).
  // main() withholds trust after repeated crash-boots.
  steps::StepBook nv;
  const bool rtcOk = trustRtcMirror && s_rtcBook.valid() &&
                     addonstore::fnv1a(&s_rtcBook, sizeof(s_rtcBook)) == s_rtcCrc;
  const bool nvOk = addonstore::getBlob("steps", &nv, sizeof(nv)) && nv.valid();
  {
    Lock lk;
    if (rtcOk && (!nvOk || s_rtcBook.lifetime >= nv.lifetime)) s_book = s_rtcBook;
    else if (nvOk) s_book = nv;
    else s_book.init();
    s_savedLifetime = nvOk ? nv.lifetime : 0;
    s_savedDay = nvOk ? nv.day : 0;
    s_lastSaveMs = millis();
    s_det.reset();
    mirror();
  }
  Serial.printf("steps: today %lu lifetime %lu (%s)\n", (unsigned long)s_book.today,
                (unsigned long)s_book.lifetime, rtcOk ? "rtc" : (nvOk ? "nvs" : "new"));
}

uint32_t stepsSvcFeed(const int16_t *xyz, int count, bool overflow, uint32_t nowDay) {
  if (count < 0) count = 0;
  Lock lk;
  if (overflow) s_det.gap();
  uint32_t added = 0;
  for (int i = 0; i < count; i++) {
    const int16_t *v = xyz + i * 3;
    added += s_det.push((float)v[0] / 4096.0f, (float)v[1] / 4096.0f, (float)v[2] / 4096.0f);
    if (s_recOn && s_rec) {
      if (s_recLen < s_recCap) {
        s_rec[s_recLen * 3] = v[0];
        s_rec[s_recLen * 3 + 1] = v[1];
        s_rec[s_recLen * 3 + 2] = v[2];
        s_recLen++;
      } else {
        s_recOn = false;
      }
    }
  }
  if (s_recOn) s_recSteps += added;
  const bool rolled = s_book.rollTo(nowDay);
  if (added) {
    s_book.add(added, nowDay);
    s_lastStepMs = millis();
  }
  if (added || rolled) {
    s_dirty = true;
    mirror();
  }
  return added;
}

void stepsSvcRollTo(uint32_t nowDay) {
  Lock lk;
  if (s_book.rollTo(nowDay)) { s_dirty = true; mirror(); }
}

StepsSnapshot stepsSvcSnapshot() {
  Lock lk;
  StepsSnapshot s;
  s.today = s_book.today;
  s.lifetime = s_book.lifetime;
  s.day = s_book.day;
  s.walking = s_det.walking();
  s.cadenceSpm = s_det.cadenceSpm();
  s.lastStepMs = s_lastStepMs;
  return s;
}

uint32_t stepsSvcOnDay(uint32_t day) {
  Lock lk;
  return s_book.stepsOn(day);
}

void stepsSvcResetDetector() {
  Lock lk;
  s_det.reset();
}

void stepsSvcCheckpoint(bool force) {
  steps::StepBook copy;
  {
    Lock lk;
    const bool due = force ||
                     (s_book.lifetime >= s_savedLifetime + 250) ||
                     (s_book.day != s_savedDay) ||
                     (s_dirty && millis() - s_lastSaveMs >= 10UL * 60UL * 1000UL);
    if (!due || (!s_dirty && !force)) return;
    copy = s_book;
    s_dirty = false;
    s_savedLifetime = s_book.lifetime;
    s_savedDay = s_book.day;
    s_lastSaveMs = millis();
  }
  addonstore::putBlob("steps", &copy, sizeof(copy));
}

void stepsSvcInject(uint32_t n, uint32_t nowDay) {
  Lock lk;
  s_book.add(n, nowDay);
  s_lastStepMs = millis();
  s_dirty = true;
  mirror();
}

// ---- recorder ----------------------------------------------------------------
bool stepsSvcRecStart(uint32_t seconds) {
  Lock lk;
  if (seconds > 2400) seconds = 2400;                 // ~40 min, 180 KB of PSRAM
  uint32_t cap = (uint32_t)((float)seconds * accel::kOdrHz) + 1;
  if (!s_rec || s_recCap < cap) {
    if (s_rec) free(s_rec);
    s_rec = (int16_t *)ps_malloc(cap * 3 * sizeof(int16_t));
    s_recCap = s_rec ? cap : 0;
  }
  if (!s_rec) return false;
  s_recCap = cap;
  s_recLen = 0;
  s_recSteps = 0;
  s_recOn = true;
  return true;
}

void stepsSvcRecStatus(uint32_t &samples, uint32_t &cap, uint32_t &stepsCounted, bool &on) {
  Lock lk;
  samples = s_recLen;
  cap = s_recCap;
  stepsCounted = s_recSteps;
  on = s_recOn;
}

void stepsSvcRecStop() {
  Lock lk;
  s_recOn = false;
}

uint32_t stepsSvcRecCopy(uint32_t from, int16_t *out, uint32_t maxSamples) {
  Lock lk;
  if (!s_rec || from >= s_recLen) return 0;
  uint32_t n = s_recLen - from;
  if (n > maxSamples) n = maxSamples;
  memcpy(out, s_rec + from * 3, n * 3 * sizeof(int16_t));
  return n;
}
