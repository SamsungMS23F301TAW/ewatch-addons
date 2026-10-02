#include "radar_service.h"
#include <Arduino.h>
#include <Preferences.h>
#include <Wire.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>
#include <sys/time.h>
#include "apptimer.h"
#include "controller.h"
#include "haptic.h"
#include "haptic_pattern.h"
#include "model.h"
#include "pins.h"
#include "radar_ble.h"

using namespace fr;

namespace radar {

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------
static const uint16_t kFgAdvMs       = 100;    // foreground advertising interval
static const uint16_t kBgAdvMs       = 60;     // background windows: faster, shorter
static const uint16_t kScanItvlMs    = 100;
static const uint16_t kScanWinMs     = 95;     // ~95 % duty while open
static const uint32_t kDeinitIdleMs  = 20000;  // power the controller down after this idle
static const uint32_t kLazySaveMs    = 300000; // sightings are saved at most this often
static const uint8_t  kBgMinBattery  = 15;     // % below which background mode stands down
static const uint32_t kRtcMagic      = 0x46524431;   // "FRD1"

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static RadarEngine       gEng;
static RadarSettings     gSet;
static SemaphoreHandle_t gMu = nullptr;
static bool              gInit = false;
static TaskHandle_t      gTask = nullptr;

static volatile bool     gWantOpen = false;
static volatile RadioState gRadio = RadioState::Off;
static volatile bool     gShakeReq = false;
static volatile bool     gBeaconDirty = true;
static volatile bool     gBgWindow = false;      // a background window owns the radio

static PlayCmd gPlay;
static bool    gPlayPending = false;
static char    gPlayName[kMaxName + 1];

static RtcPhase gPhase;

// Survives deep sleep (not a power-off): alert cooldowns, the sub-second RTC
// phase estimate and background bookkeeping.
RTC_DATA_ATTR static uint32_t       rtcMagic = 0;
RTC_DATA_ATTR static MateAlertEntry rtcAlerts[kMaxMates];
RTC_DATA_ATTR static uint8_t        rtcAlertsN = 0;
RTC_DATA_ATTR static int64_t        rtcPhaseOff = 0;
RTC_DATA_ATTR static uint8_t        rtcPhaseValid = 0;
RTC_DATA_ATTR static uint8_t        rtcBgArmed = 0;
RTC_DATA_ATTR static uint8_t        rtcAlertBoot = 0;
RTC_DATA_ATTR static uint32_t       rtcBgWindows = 0;
RTC_DATA_ATTR static uint32_t       rtcBgMatches = 0;

struct Lock {
  Lock()  { xSemaphoreTake(gMu, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(gMu); }
  Lock(const Lock &) = delete;
  Lock &operator=(const Lock &) = delete;
};

static int64_t localUs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);      // keeps running through deep sleep
  return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

static void rtcCheckMagic() {
  if (rtcMagic != kRtcMagic) {
    memset(rtcAlerts, 0, sizeof rtcAlerts);
    rtcAlertsN = 0; rtcPhaseOff = 0; rtcPhaseValid = 0;
    rtcBgArmed = 0; rtcAlertBoot = 0; rtcBgWindows = 0; rtcBgMatches = 0;
    rtcMagic = kRtcMagic;
  }
}

// Push engine alert state to RTC memory (and the RTC phase estimate).
static void saveRtcStateLocked() {
  rtcAlertsN = (uint8_t)gEng.exportAlerts(rtcAlerts, kMaxMates);
  if (gPhase.valid()) { rtcPhaseOff = gPhase.offsetUs(); rtcPhaseValid = 1; }
}

static void applyEngineIdentityLocked(bool background) {
  gEng.setName(gSet.name);
  gEng.setRef1m(gSet.ref1m, gSet.calibrated);
  uint8_t f = 0;
  if (background) f |= kFlagBackground;
  if (gSet.bgAlerts) f |= kFlagAlerts;
  gEng.setBaseFlags(f);
  gEng.config().alertsEnabled = gSet.alerts;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void init() {
  if (gInit) return;
  rtcCheckMagic();
  gMu = xSemaphoreCreateMutex();
  RadarStore::load(gSet, gEng.mates());
  gEng.begin(gSet.id);
  gEng.importAlerts(rtcAlerts, rtcAlertsN);
  if (rtcPhaseValid) gPhase.restore(rtcPhaseOff);
  applyEngineIdentityLocked(false);
  rble::queue();
  gInit = true;
  Serial.printf("radar: id %08lX name \"%s\" ref %d dBm%s, %d mates, bg %s/%us\n",
                (unsigned long)gSet.id, gSet.name, (int)gSet.ref1m,
                gSet.calibrated ? " (calibrated)" : "", gEng.mates().count(),
                gSet.bgAlerts ? "on" : "off", (unsigned)gSet.bgPeriodSec);
}

uint32_t nowSec() {
  ModelLock lk;
  if (model.rtcOk)
    return rtcEpochSec(model.year, model.month, model.day, model.hour, model.minute, model.second);
  return 0x40000000u + millis() / 1000u;     // no RTC: boot-relative fallback
}

static uint64_t radarMsAt(uint32_t epochSec) {
  int64_t base = gPhase.valid() ? (int64_t)gPhase.rtcMs(localUs()) : (int64_t)epochSec * 1000;
  return (uint64_t)(base + (int64_t)gSet.clkOffsetSec * 1000);
}

static uint8_t clkNow(uint32_t epochSec) { return clkField(radarMsAt(epochSec)); }

static void pushBeacon(uint32_t nowMs, uint32_t epochSec) {
  Beacon b;
  {
    Lock lk;
    gEng.buildBeacon(b, nowMs, clkNow(epochSec));
  }
  uint8_t buf[kMaxAdvData];
  size_t n = encodeAdv(b, buf, sizeof buf);
  if (n) rble::setAdvData(buf, n);
}

// Carry out what the engine asked for.
static void apply(const Effects &fx, uint32_t nowMs) {
  if (fx.play.kind != AnimKind::None) {
    Lock lk;
    gPlay = fx.play;
    gPlayPending = true;
    gEng.labelFor(fx.play.peerId, gPlayName);
  }
  if (fx.buzzHello) hapticPatternPlay(kHapticMateNear, kHapticMateNearN, fx.play.startMs);
  if (fx.buzzCelebrate) hapticPatternPlay(kHapticCelebrate, kHapticCelebrateN, fx.play.startMs);
  if (fx.beaconDirty) gBeaconDirty = true;
  if (fx.clockAdjust) {
    RadarSettings copy;
    {
      Lock lk;
      gSet.clkOffsetSec += fx.clockDeltaSec;
      copy = gSet;
    }
    RadarStore::saveSettings(copy);
    gBeaconDirty = true;
    Serial.printf("radar: rendezvous clock %+d s (now %ld)\n", fx.clockDeltaSec, (long)copy.clkOffsetSec);
  }
  (void)nowMs;
}

static void feedReport(const rble::Report &r, uint32_t epochSec) {
  Effects fx;
  {
    Lock lk;
    gEng.onBeacon(r.b, r.rssi, r.ms, epochSec, clkNow(epochSec), fx);
  }
  apply(fx, r.ms);
}

// Copy the book under the lock, write NVS without holding it.
static MateBook gSaveCopy;
static void saveMatesNow() {
  {
    Lock lk;
    gSaveCopy = gEng.mates();
    gEng.mates().clearDirty();
  }
  RadarStore::saveMates(gSaveCopy);
}

// ---------------------------------------------------------------------------
// The radar task
// ---------------------------------------------------------------------------
static void taskRadar(void *) {
  esp_task_wdt_add(nullptr);
  uint32_t idleSince = millis();
  uint32_t lastTick = 0, lastPush = 0, lastSave = millis(), lastBeginTry = 0, lastStartTry = 0;
  uint32_t lastSecSampleMs = 0;
  uint8_t lastSecond = 99, lastClk = 0xFF;
  bool wasOpen = false;

  for (;;) {
    esp_task_wdt_reset();
    const bool want = gWantOpen;
    uint32_t now = millis();

    // ---- radio state machine --------------------------------------------
    if (want) {
      if (!wasOpen) {
        Lock lk;
        applyEngineIdentityLocked(false);
        gBeaconDirty = true;
        lastSecond = 99;                     // stale since the last session
      }
      wasOpen = true;
      if (!rble::ready()) {
        if (now - lastBeginTry > 2000 || lastBeginTry == 0) {
          lastBeginTry = now;
          gRadio = RadioState::Starting;
          if (!rble::begin()) gRadio = RadioState::Error;
        }
      }
      if (rble::ready()) {
        bool adv = rble::advertising(), scan = rble::scanning();
        if ((!adv || !scan) && now - lastStartTry > 1000) {
          // (Re)start whatever is not running: first open, or after a
          // controller/host reset stopped it.
          lastStartTry = now;
          if (!adv) { pushBeacon(now, nowSec()); rble::startAdv(kFgAdvMs); }
          if (!scan) rble::startScan(kScanItvlMs, kScanWinMs);
          adv = rble::advertising();
          scan = rble::scanning();
        }
        gRadio = (adv && scan) ? RadioState::Live
                               : (now - lastStartTry < 3000 ? RadioState::Starting : RadioState::Error);
      }
      idleSince = now;
    } else if (!gBgWindow) {
      if (wasOpen) {
        wasOpen = false;
        rble::stopAdv();
        rble::stopScan();
        bool dirty;
        {
          Lock lk;
          saveRtcStateLocked();
          gEng.calStop();
          dirty = gEng.mates().dirty() != MateBook::Clean;
        }
        if (dirty) { saveMatesNow(); lastSave = now; }
        idleSince = now;
      }
      gRadio = RadioState::Off;
      if (rble::ready() && now - idleSince > kDeinitIdleMs) {
        rble::end();
        Serial.println("radar: radio powered down");
      }
    }

    // ---- incoming beacons -------------------------------------------------------
    rble::Report r;
    int drained = 0;
    TickType_t wait = pdMS_TO_TICKS(want ? 20 : 250);
    uint32_t epoch = want ? nowSec() : 0;
    while (drained < 24 && xQueueReceive(rble::queue(), &r, drained ? 0 : wait) == pdPASS) {
      ++drained;
      if (want) feedReport(r, epoch);
    }
    if (!want) continue;

    // ---- periodic housekeeping (10 Hz) --------------------------------------------
    now = millis();
    if (now - lastTick >= 100) {
      lastTick = now;
      uint8_t sec;
      bool ok;
      {
        ModelLock lk;
        ok = model.rtcOk;
        sec = model.second;
        epoch = ok ? rtcEpochSec(model.year, model.month, model.day, model.hour, model.minute, model.second)
                   : 0x40000000u + now / 1000u;
      }
      Effects fx;
      {
        Lock lk;
        gEng.tick(now, epoch, fx);
      }
      apply(fx, now);
      // Track the RTC's second boundary for the rendezvous clock. Only two
      // consecutive samples bracket a rollover tightly; the model itself
      // trails the RTC by up to one taskIO read (~90 ms).
      if (ok) {
        if (sec != lastSecond) {
          uint32_t gap = now - lastSecSampleMs;
          if (lastSecond != 99 && gap <= 150)
            gPhase.observeRollover(epoch, localUs(), (int64_t)gap * 1000 + 90000);
          else
            gPhase.observe(epoch, localUs());
          lastSecond = sec;
          rtcPhaseOff = gPhase.offsetUs();
          rtcPhaseValid = 1;
        }
        lastSecSampleMs = now;
      }
      if (gShakeReq) {
        gShakeReq = false;
        Effects sf;
        {
          Lock lk;
          gEng.onLocalShake(now, sf);
        }
        apply(sf, now);
      }
      // Refresh the advertisement when it changed, every second (clock
      // field) and 4x a second while an event's age byte is ticking.
      uint8_t clk = clkNow(epoch);
      bool onAir;
      { Lock lk; onAir = gEng.eventOnAir(now); }
      if (gBeaconDirty || clk != lastClk || (onAir && now - lastPush >= 250)) {
        gBeaconDirty = false;
        lastClk = clk;
        lastPush = now;
        pushBeacon(now, epoch);
      }
      // Lazy save of sightings / encounter log.
      if (now - lastSave > kLazySaveMs) {
        bool dirty;
        { Lock lk; dirty = gEng.mates().dirty() != MateBook::Clean; }
        if (dirty) saveMatesNow();
        lastSave = now;
      }
    }
  }
}

void startTask() {
  init();
  if (gTask) return;
  // 8 KB: NimBLE controller bring-up/teardown runs on this task.
  xTaskCreatePinnedToCore(taskRadar, "radar", 8192, nullptr, 3, &gTask, 0);
}

// ---------------------------------------------------------------------------
// Session + UI API
// ---------------------------------------------------------------------------
void open() {
  init();
  gWantOpen = true;
  if (gRadio == RadioState::Off) gRadio = RadioState::Starting;
}

void close() {
  gWantOpen = false;
  hapticPatternStop();
  if (gInit) {
    Lock lk;
    saveRtcStateLocked();
    gPlayPending = false;
  }
}

RadioState radioState() { return gRadio; }

int snapshot(PeerView *out, int max) {
  Lock lk;
  return gEng.snapshot(out, max, millis());
}

bool takePlay(PlayCmd &cmd, char *peerName) {
  Lock lk;
  if (!gPlayPending) return false;
  gPlayPending = false;
  cmd = gPlay;
  memcpy(peerName, gPlayName, kMaxName + 1);
  return true;
}

void localShake() { gShakeReq = true; }

uint32_t myId() { Lock lk; return gSet.id; }

int mates(Mate *out, int max) {
  Lock lk;
  int n = gEng.mates().count();
  if (n > max) n = max;
  for (int i = 0; i < n; ++i) out[i] = gEng.mates().at(i);
  return n;
}

bool mateById(uint32_t id, Mate &out) {
  Lock lk;
  int i = gEng.mates().find(id);
  if (i < 0) return false;
  out = gEng.mates().at(i);
  return true;
}

bool isMate(uint32_t id) { Lock lk; return gEng.mates().isMate(id); }

bool addMate(uint32_t id) {
  bool ok;
  const uint32_t t = nowSec();   // takes ModelLock: never while holding gMu
  { Lock lk; ok = gEng.addMate(id, nullptr, t); saveRtcStateLocked(); }
  if (ok) saveMatesNow();
  return ok;
}

bool removeMate(uint32_t id) {
  bool ok;
  { Lock lk; ok = gEng.removeMate(id); saveRtcStateLocked(); }
  if (ok) saveMatesNow();
  return ok;
}

bool renameMate(uint32_t id, const char *nick) {
  bool ok;
  { Lock lk; ok = gEng.mates().rename(id, nick); }
  if (ok) saveMatesNow();
  return ok;
}

RadarSettings settings() { Lock lk; return gSet; }

static void commitSettings() {
  RadarSettings copy;
  {
    Lock lk;
    applyEngineIdentityLocked(false);
    copy = gSet;
  }
  gBeaconDirty = true;
  RadarStore::saveSettings(copy);
}

void setName(const char *name) {
  { Lock lk; if (sanitizeName(name, gSet.name) == 0) defaultName(gSet.id, gSet.name); }
  commitSettings();
}

void setAlerts(bool on) { { Lock lk; gSet.alerts = on; } commitSettings(); }

void setBackground(bool on, uint8_t periodSec) {
  if (periodSec != 30 && periodSec != 60 && periodSec != 120) periodSec = 60;
  { Lock lk; gSet.bgAlerts = on; gSet.bgPeriodSec = periodSec; }
  commitSettings();
}

void setCalibration(int8_t ref1m, bool calibrated) {
  { Lock lk; gSet.ref1m = ref1m; gSet.calibrated = calibrated; }
  commitSettings();
}

void resetId() {
  {
    Lock lk;
    gSet.id = RadarStore::newId();
    char oldDefault[kMaxName + 1];
    defaultName(gEng.myId(), oldDefault);
    if (strcmp(gSet.name, oldDefault) == 0) defaultName(gSet.id, gSet.name);
    gEng.begin(gSet.id);
    applyEngineIdentityLocked(false);
    memset(rtcAlerts, 0, sizeof rtcAlerts);
    rtcAlertsN = 0;
  }
  commitSettings();
}

bool calStart(char *peerName) {
  Lock lk;
  uint32_t id;
  if (!gEng.strongestPeer(millis(), id)) return false;
  gEng.calStart(id);
  gEng.labelFor(id, peerName);
  return true;
}

uint16_t calCount() { Lock lk; return gEng.calCount(); }
int calLastRssi() { Lock lk; return gEng.calLastRaw(); }
CalResult calResult() { Lock lk; return calibrateFromSamples(gEng.calSamples(), gEng.calCount()); }
void calStop() { Lock lk; gEng.calStop(); }

bool backgroundActive() {
#if FR_BACKGROUND_ALERTS
  init();
  uint8_t pct; bool ok;
  { ModelLock lk; pct = model.batPct; ok = model.batOk; }
  Lock lk;
  return gSet.bgAlerts && gEng.mates().count() > 0 && (!ok || pct >= kBgMinBattery);
#else
  return false;
#endif
}

// ---------------------------------------------------------------------------
// Serial diagnostics: type "radar" in the monitor.
// ---------------------------------------------------------------------------
void serialCommand(const char *line) {
  if (strncmp(line, "radar", 5) != 0) return;
  init();
  static const char *kRadio[] = {"off", "starting", "live", "error"};
  PeerView pv[kMaxPeers];
  int n = snapshot(pv, kMaxPeers);
  RadarSettings s = settings();
  Serial.printf("radar: id %08lX \"%s\" ref %d%s radio %s, adv %d scan %d, seen %lu dropped %lu\n",
                (unsigned long)s.id, s.name, (int)s.ref1m, s.calibrated ? "(cal)" : "",
                kRadio[(int)gRadio], (int)rble::advertising(), (int)rble::scanning(),
                (unsigned long)rble::seenReports(), (unsigned long)rble::droppedReports());
  Serial.printf("radar: alerts %s, bg %s every %us, clk offset %ld s, phase %s, bg windows %lu matches %lu\n",
                s.alerts ? "on" : "off", s.bgAlerts ? "on" : "off", (unsigned)s.bgPeriodSec,
                (long)s.clkOffsetSec, gPhase.valid() ? "ok" : "unknown",
                (unsigned long)rtcBgWindows, (unsigned long)rtcBgMatches);
  for (int i = 0; i < n; ++i) {
    Serial.printf("  %08lX %-12s %s %-10s rssi %6.1f ref %d pl %5.1f ~%.1fm age %lums n=%u%s\n",
                  (unsigned long)pv[i].id, pv[i].label, pv[i].mate ? "MATE" : "    ",
                  zoneName(pv[i].zone), pv[i].rssi, (int)pv[i].ref1m, pv[i].plDb, pv[i].distM,
                  (unsigned long)pv[i].ageMs, (unsigned)pv[i].samples,
                  (pv[i].flags & kFlagBackground) ? " bg" : "");
  }
  Serial.printf("radar: heap %lu (largest %lu), psram %lu\n",
                (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMaxAllocHeap(),
                (unsigned long)ESP.getFreePsram());
}

// ---------------------------------------------------------------------------
// Background mate alerts
// ---------------------------------------------------------------------------
#if FR_BACKGROUND_ALERTS

bool bgArmed() { rtcCheckMagic(); return rtcBgArmed != 0; }
void bgSetArmed(bool armed) { rtcCheckMagic(); rtcBgArmed = armed ? 1 : 0; }

bool takeAlertBoot() {
  bool b = rtcAlertBoot != 0;
  rtcAlertBoot = 0;
  return b;
}

uint32_t bgPlanSleepMs(uint32_t epochSec, uint8_t batteryPct, bool batteryKnown) {
  init();
  RadarSettings s;
  int mateCount;
  {
    Lock lk;
    s = gSet;
    mateCount = gEng.mates().count();
    saveRtcStateLocked();
  }
  if (!s.bgAlerts || mateCount == 0) return 0;
  if (batteryKnown && batteryPct < kBgMinBattery) return 0;
  if (!gPhase.valid()) gPhase.observe(epochSec, localUs());
  RendezvousConfig rc;
  rc.periodSec = s.bgPeriodSec;
  return msUntilNextWake(radarMsAt(epochSec), rc);
}

static bool touchOrButton() {
  return digitalRead(PIN_BTN) == HIGH || digitalRead(PIN_TOUCH_INT) == LOW;
}

BgOutcome bgRunWindow() {
  rtcCheckMagic();
  rtcBgArmed = 0;
  ++rtcBgWindows;
  pinMode(PIN_BTN, INPUT);
  pinMode(PIN_TOUCH_INT, INPUT_PULLUP);
  if (touchOrButton()) return BgOutcome::UserWake;

  // Wall clock straight from the RV-3028 (taskIO is not running yet).
  uint8_t h = 0, mi = 0, se = 0, wd = 0, d = 1, mo = 1;
  uint16_t y = 2025;
  bool rtcOk = readRTC(h, mi, se, wd, d, mo, y);
  uint32_t epoch0 = rtcOk ? rtcEpochSec(y, mo, d, h, mi, se) : 0;
  uint32_t t0 = millis();
  if (rtcOk) gPhase.observe(epoch0, localUs());

  // Battery: stand down below the cutoff so the stock power-off takes over.
  pinMode(PIN_BAT_MON_EN, OUTPUT);
  analogSetPinAttenuation(PIN_BAT_MON_ADC, ADC_11db);
  float v = 0; uint8_t pct = 100;
  bool batOk = readBattery(v, pct);

  init();
  {
    Lock lk;
    if (!gSet.bgAlerts || gEng.mates().count() == 0 || (batOk && pct < kBgMinBattery)) {
      Serial.printf("radar bg: standing down (bg %d, mates %d, battery %u%%)\n",
                    (int)gSet.bgAlerts, gEng.mates().count(), (unsigned)pct);
      return BgOutcome::Sleep;
    }
    applyEngineIdentityLocked(true);
  }
  auto epochNow = [&]() { return epoch0 + (millis() - t0) / 1000u; };

  gBgWindow = true;
  if (!rble::begin()) { gBgWindow = false; return BgOutcome::Sleep; }
  pushBeacon(millis(), epochNow());
  rble::startAdv(kBgAdvMs);
  rble::startScan(kScanItvlMs, kScanWinMs);
  RendezvousConfig rc;
  uint32_t radioOn = millis();
  uint32_t lastPush = radioOn;
  BgOutcome out = BgOutcome::Sleep;

  while (millis() - radioOn < rc.windowMs) {
    if (touchOrButton()) { out = BgOutcome::UserWake; break; }
    rble::Report r;
    while (xQueueReceive(rble::queue(), &r, pdMS_TO_TICKS(10)) == pdPASS) {
      Effects fx;
      {
        Lock lk;
        gEng.onBeacon(r.b, r.rssi, r.ms, epochNow(), clkNow(epochNow()), fx);
      }
      if (fx.play.kind != AnimKind::None) {
        // The buzz starts now, before setup() would normally start the
        // haptic driver and apply the user's strength setting.
        hapticBegin();                       // idempotent
        Preferences hp;
        hp.begin("ewatch", /*readOnly=*/true);
        hapticSetStrengthPct(hp.getUChar("haptStr", 100));
        hp.end();
        apply(fx, millis());
        out = BgOutcome::Alert;
      } else {
        apply(fx, millis());
      }
    }
    if (out == BgOutcome::Alert) break;
    uint32_t now = millis();
    if (gBeaconDirty || now - lastPush >= 250) {
      gBeaconDirty = false;
      lastPush = now;
      pushBeacon(now, epochNow());
    }
  }
  {
    Lock lk;
    saveRtcStateLocked();
  }
  if (out == BgOutcome::Alert) {
    // Keep the radio up: the full boot continues into the radar app, which
    // goes on advertising our wave so the mate's watch can join in.
    ++rtcBgMatches;
    rtcAlertBoot = 1;
    gWantOpen = true;
    gBgWindow = false;
    {
      Lock lk;
      applyEngineIdentityLocked(false);
    }
    return out;
  }
  rble::stopAdv();
  rble::stopScan();
  gBgWindow = false;
  return out;
}

void bgSleep() {
  // Same wake sources as enterDeepSleep(), from the user's saved settings.
  Preferences p;
  p.begin("ewatch", /*readOnly=*/true);
  bool wkT = p.getBool("wkTouch", true);
  bool wkB = p.getBool("wkButton", true);
  bool wkI = p.getBool("wkImu", false);
  uint16_t toOff = p.getUShort("sleepOff", 30);
  p.end();

  // Next rendezvous (or the stock power-off timer if background stood down).
  uint8_t h = 0, mi = 0, se = 0, wd = 0, d = 1, mo = 1;
  uint16_t y = 2025;
  uint32_t epoch = readRTC(h, mi, se, wd, d, mo, y) ? rtcEpochSec(y, mo, d, h, mi, se) : 0;
  pinMode(PIN_BAT_MON_EN, OUTPUT);
  analogSetPinAttenuation(PIN_BAT_MON_ADC, ADC_11db);
  float v = 0; uint8_t pct = 100;
  bool batOk = readBattery(v, pct);
  uint32_t ms = bgPlanSleepMs(epoch, pct, batOk);

  // Drain the touch controller so its INT line is idle before arming ext0.
  Wire.beginTransmission(I2C_ADDR_TOUCH);
  Wire.write(0x01);
  if (Wire.endTransmission(false) == 0) {
    Wire.requestFrom((int)I2C_ADDR_TOUCH, 6);
    while (Wire.available()) (void)Wire.read();
  }
  if (wkT) {
    rtc_gpio_pullup_en((gpio_num_t)PIN_TOUCH_INT);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_TOUCH_INT, 0);
  }
  uint64_t mask = 0;
  if (wkB) mask |= 1ULL << PIN_BTN;
  if (wkI) mask |= 1ULL << PIN_MMA_INT1;
  if (mask) esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_HIGH);
  if (ms) {
    rtcBgArmed = 1;
    esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000ULL);
  } else {
    rtcBgArmed = 0;
    if (toOff > 0) esp_sleep_enable_timer_wakeup((uint64_t)toOff * 1000000ULL);
  }
  Serial.printf("radar bg: window %lu done, next in %lu ms\n",
                (unsigned long)rtcBgWindows, (unsigned long)ms);
  Serial.flush();
  gpio_hold_en((gpio_num_t)PIN_LDO_LATCH);
  gpio_deep_sleep_hold_en();
  esp_deep_sleep_start();
}

#endif  // FR_BACKGROUND_ALERTS

}  // namespace radar
