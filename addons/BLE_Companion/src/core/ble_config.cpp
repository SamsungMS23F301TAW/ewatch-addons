// BleConfig — see ble_config.h for the lifecycle and threading model, and
// docs/PROTOCOL.md for the wire format.
#include "ble_config.h"

#include <string.h>

#if defined(EWATCH_ENABLE_BLE) && EWATCH_ENABLE_BLE

#include <Arduino.h>
#include <NimBLEDevice.h>
#include "nimble/nimble/host/services/gap/include/services/gap/ble_svc_gap.h"
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "controller.h"   // requestSetRTC
#include "display.h"      // backlightSet
#include "event.h"
#include "face_store.h"
#include "haptic.h"
#include "model.h"
#include "storage.h"
#include "view.h"         // watchFaceStyleCount / watchFaceStyleName
#include "face_render.h"  // kDefaultStyle

#ifndef EWATCH_BLE_REQUIRE_ENCRYPTION
#define EWATCH_BLE_REQUIRE_ENCRYPTION 0
#endif
#ifndef EWATCH_ADDON_VERSION
#define EWATCH_ADDON_VERSION "1.1.0"
#endif

using namespace bleproto;

namespace BleConfig {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static const uint16_t kAppearanceSmartwatch = 0x00C2;   // GAP appearance "Watch: Smartwatch"
static const uint32_t kServicePeriodMs  = 50;
static const uint32_t kWatchPollMs      = 250;           // watch-side change detection
static const uint32_t kBatteryPollMs    = 5000;
static const uint32_t kBaseSaveQuietMs  = 1500;          // Storage::save() debounce
static const uint32_t kLockDropDelayMs  = 400;           // let the LOCKED notify go out first

// BaseOS defaults (model.h) for the "reset theme" safety net.
static const uint16_t kDefBg = 0x0000, kDefFg = 0xFFFF, kDefAccent = 0x000F, kDefLine = 0x7BEF;
static const uint8_t  kDefBrightness = 200;

enum ChrId : uint8_t {
  C_INFO, C_AUTH, C_REVISION, C_THEME, C_BRIGHTNESS, C_FACE, C_SLOTS,
  C_TEXTS, C_FEEDS, C_TIME, C_MESSAGE, C_CONTROL, C_BATTERY, C_COUNT
};

// Internal "pending notify" bits that extend bleproto::ChangeBit.
static const uint32_t N_REVISION = 1UL << 16;
static const uint32_t N_AUTH     = 1UL << 17;
static const uint32_t N_BATTERY  = 1UL << 18;

// ---------------------------------------------------------------------------
// State (guarded by sMutex unless noted)
// ---------------------------------------------------------------------------
static SemaphoreHandle_t sMutex = nullptr;
static TaskHandle_t sTask = nullptr;

static State    sState = State::Off;
static Reason   sReason = Reason::None;
static bool     sStackUp = false;             // written by the service task only
static bool     sAdvertising = false;         // what we last told NimBLE
static uint32_t sAdvUntil = 0;                // millis() deadline of the visibility window
static bool     sAdvWanted = false;           // window open?
static bool     sUserStopped = false;         // stopAdvertising() asked for Off
static uint32_t sLockoutUntil = 0;
static bool     sLockedOut = false;
static uint16_t sConn = 0xFFFF;               // BLE_HS_CONN_HANDLE_NONE
static bool     sAuthorized = false;
static uint32_t sCode = 0;
static uint8_t  sAttempts = 0;
static uint32_t sPairDeadline = 0;
static uint32_t sSessionStart = 0;
static uint32_t sWrites = 0;
static uint16_t sMtu = 23;
static bool     sDropPending = false;
static uint32_t sDropAt = 0;
static Reason   sDropReason = Reason::None;
static char     sName[16] = "EWatch";

static uint32_t sPendingNotify = 0;
static bool     sPendingBacklight = false;
static bool     sBaseDirty = false;
static uint32_t sBaseDirtyAt = 0;
static Revision sLastRev = { 0, 0, BY_SYSTEM, RES_OK };

struct BaseSnap { Theme theme; uint8_t brightness; uint8_t style; int16_t tz; };
static BaseSnap sLastBase;
static uint8_t  sLastBattery = 0xFF;

static Message  sMsg;
static bool     sMsgValid = false;

static volatile uint32_t sLastClientWrite = 0;   // single-word, lock-free
static volatile uint32_t sLastUserActivity = 0;

static NimBLECharacteristic *sChr[C_COUNT] = { nullptr };

namespace {
struct Lock {
  Lock()  { xSemaphoreTake(sMutex, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(sMutex); }
};

bool before(uint32_t now, uint32_t deadline) { return (int32_t)(deadline - now) > 0; }
uint32_t remaining(uint32_t now, uint32_t deadline) {
  return before(now, deadline) ? deadline - now : 0;
}

void postState(State s, Reason r) {
  Event e = makeEvent(EventType::BleState);
  e.x = (uint16_t)s;
  e.y = (uint16_t)r;
  postEvent(e);
}

void postRequest(Request r) {
  Event e = makeEvent(EventType::BleRequest);
  e.x = (uint16_t)r;
  postEvent(e);
}

// Caller holds sMutex.
void setStateLocked(State s, Reason r) {
  sState = s;
  sReason = r;
  postState(s, r);
}

BaseSnap readBase() {
  BaseSnap b;
  ModelLock lk;
  b.theme.bg = model.bgColor;
  b.theme.fg = model.fgColor;
  b.theme.accent = model.accentColor;
  b.theme.line = model.lineColor;
  b.brightness = model.brightness;
  b.style = model.watchFaceStyle;
  b.tz = model.tzOffsetMin;
  return b;
}

bool sameBase(const BaseSnap &a, const BaseSnap &b) {
  return a.theme.bg == b.theme.bg && a.theme.fg == b.theme.fg &&
         a.theme.accent == b.theme.accent && a.theme.line == b.theme.line &&
         a.brightness == b.brightness && a.style == b.style && a.tz == b.tz;
}

uint32_t unixNowFromModel(bool &rtcOk, int16_t &tz) {
  CivilTime c;
  {
    ModelLock lk;
    rtcOk = model.rtcOk;
    tz = model.tzOffsetMin;
    c.year = model.year; c.month = model.month; c.day = model.day;
    c.hour = model.hour; c.minute = model.minute; c.second = model.second;
    c.weekday = model.weekday;
  }
  if (!rtcOk || c.year < 2000) return 0;
  return civilToUnix(c, tz);
}

void buzzPattern(uint8_t a, uint16_t aMs, uint16_t gapMs, uint8_t b, uint16_t bMs) {
  hapticBuzz(a, aMs);
  if (gapMs) hapticBuzz(0, gapMs);       // intensity 0 = a silent gap in the queue
  if (b) hapticBuzz(b, bMs);
}

// Record an applied change. Caller holds sMutex. `bump` = config changed.
void recordChangeLocked(uint16_t mask, uint8_t source, bool bump) {
  uint32_t rev = bump ? FaceStore::bumpConfigRevision() : FaceStore::configRevision();
  sLastRev.revision = rev;
  sLastRev.changedMask = mask;
  sLastRev.source = source;
  sLastRev.result = RES_OK;
  sPendingNotify |= (uint32_t)mask | N_REVISION;
}

// Caller holds sMutex.
void recordErrorLocked(uint16_t mask, Result r) {
  sLastRev.revision = sAuthorized ? FaceStore::configRevision() : 0;
  sLastRev.changedMask = mask;
  sLastRev.source = BY_CLIENT;
  sLastRev.result = (uint8_t)r;
  sPendingNotify |= N_REVISION;
}

void markBaseDirtyLocked() {
  sBaseDirty = true;
  sBaseDirtyAt = millis();
}

uint16_t maskFor(uint8_t id) {
  switch (id) {
    case C_THEME:      return CH_THEME;
    case C_BRIGHTNESS: return CH_BRIGHTNESS;
    case C_FACE:       return CH_FACE;
    case C_SLOTS:      return CH_SLOTS;
    case C_TEXTS:      return CH_TEXTS;
    case C_FEEDS:      return CH_FEEDS;
    case C_TIME:       return CH_TIME;
    case C_MESSAGE:    return CH_MESSAGE;
    case C_CONTROL:    return CH_CONTROL;
    default:           return 0;
  }
}

// ---------------------------------------------------------------------------
// Value builders (current state -> bytes)
// ---------------------------------------------------------------------------
size_t buildValue(uint8_t id, uint8_t *out, size_t cap) {
  switch (id) {
    case C_INFO: {
      const char *names[16];
      int n = watchFaceStyleCount();
      if (n > 16) n = 16;
      for (int i = 0; i < n; i++) names[i] = watchFaceStyleName(i);
      InfoParams p;
      p.capabilities = kCapMessages | kCapFeeds | kCap12h | kCapControl |
                       (EWATCH_BLE_REQUIRE_ENCRYPTION ? kCapEncrypted : 0);
      p.styleCount = (uint8_t)n;
      p.styleNames = names;
      p.firmware = EWATCH_ADDON_VERSION;
      p.deviceName = sName;
      return encodeInfo(p, out, cap);
    }
    case C_AUTH: {
      uint32_t now = millis();
      Lock lk;
      uint8_t st = sAuthorized ? AUTH_AUTHORIZED
                 : (sAttempts == 0 && sState == State::Pairing) ? AUTH_LOCKED
                 : AUTH_CODE_REQUIRED;
      uint32_t left = (sState == State::Pairing) ? remaining(now, sPairDeadline) / 1000 : 0;
      return encodeAuthRead(st, sAttempts, (uint8_t)(left > 255 ? 255 : left), out);
    }
    case C_REVISION: {
      Lock lk;
      return encodeRevision(sLastRev, out);
    }
    case C_THEME: {
      BaseSnap b = readBase();
      return encodeTheme(b.theme, out);
    }
    case C_BRIGHTNESS: {
      BaseSnap b = readBase();
      return encodeBrightness(b.brightness, out);
    }
    case C_FACE: {
      BaseSnap b = readBase();
      FaceCfg f;
      f.style = b.style;
      f.options = FaceStore::faceOptions();
      return encodeFace(f, out);
    }
    case C_SLOTS: {
      FaceStore::Snapshot s;
      FaceStore::snapshot(s);
      return encodeSlots(s.slots, out);
    }
    case C_TEXTS: {
      FaceStore::Snapshot s;
      FaceStore::snapshot(s);
      return encodeTextsRead(s.texts, out);
    }
    case C_FEEDS: {
      FaceStore::Snapshot s;
      FaceStore::snapshot(s);
      return encodeFeedsRead(s.feeds, out);
    }
    case C_TIME: {
      bool ok; int16_t tz;
      uint32_t unix = unixNowFromModel(ok, tz);
      return encodeTimeRead(unix, tz, ok, out);
    }
    case C_BATTERY: {
      ModelLock lk;
      out[0] = model.batOk ? model.batPct : 0;
      return 1;
    }
    default:
      return 0;
  }
}

bool isOpen(uint8_t id) { return id == C_INFO || id == C_AUTH || id == C_BATTERY; }

bool authorizedConn(uint16_t conn) {
  Lock lk;
  return sAuthorized && conn == sConn;
}

// ---------------------------------------------------------------------------
// Write handlers (NimBLE host task)
// ---------------------------------------------------------------------------
void handleAuth(uint16_t conn, const uint8_t *data, size_t len) {
  uint32_t code = 0;
  Result r = decodeAuthWrite(data, len, code);
  Lock lk;
  if (conn != sConn) return;
  if (r != RES_OK) { sPendingNotify |= N_AUTH; return; }
  if (sState != State::Pairing || sAuthorized || sAttempts == 0) { sPendingNotify |= N_AUTH; return; }
  uint32_t now = millis();
  if (code == sCode) {
    sAuthorized = true;
    sSessionStart = now;
    sWrites = 0;
    sLastBase = readBase();
    { ModelLock mlk; sLastBattery = model.batOk ? model.batPct : 0; }
    sLastRev.revision = FaceStore::configRevision();
    sLastRev.changedMask = 0;
    sLastRev.source = BY_SYSTEM;
    sLastRev.result = RES_OK;
    sLastClientWrite = now;
    setStateLocked(State::Connected, Reason::Paired);
    buzzPattern(140, 50, 70, 200, 80);
  } else {
    sAttempts--;
    if (sAttempts == 0) {
      sLockedOut = true;
      sLockoutUntil = now + kLockoutMs;
      sDropPending = true;
      sDropAt = now + kLockDropDelayMs;
      sDropReason = Reason::LockedOut;
      setStateLocked(State::Pairing, Reason::LockedOut);
      buzzPattern(220, 300, 0, 0, 0);
    } else {
      setStateLocked(State::Pairing, Reason::WrongCode);
      buzzPattern(180, 140, 0, 0, 0);
    }
  }
  sPendingNotify |= N_AUTH;
}

void applyTheme(const Theme &t) {
  ModelLock lk;
  model.bgColor = t.bg;
  model.fgColor = t.fg;
  model.accentColor = t.accent;
  model.lineColor = t.line;
  model.revision++;
}

void handleControlLocked(uint8_t op) {
  switch (op) {
    case CTRL_IDENTIFY:
      buzzPattern(255, 150, 100, 255, 150);
      postRequest(REQ_IDENTIFY);
      recordChangeLocked(CH_CONTROL, BY_CLIENT, false);
      break;
    case CTRL_RESET_FACE:
      FaceStore::resetToDefaults();
      recordChangeLocked(CH_CONTROL | CH_SLOTS | CH_TEXTS | CH_FEEDS | CH_FACE, BY_CLIENT, true);
      break;
    case CTRL_RESET_THEME: {
      Theme t = { kDefBg, kDefFg, kDefAccent, kDefLine };
      applyTheme(t);
      { ModelLock lk; model.brightness = kDefBrightness; model.watchFaceStyle = FaceRender::kDefaultStyle; model.revision++; }
      sLastBase = readBase();
      sPendingBacklight = true;
      markBaseDirtyLocked();
      recordChangeLocked(CH_CONTROL | CH_THEME | CH_BRIGHTNESS | CH_FACE, BY_CLIENT, true);
      break;
    }
    case CTRL_SHOW_FACE:
      postRequest(REQ_SHOW_FACE);
      recordChangeLocked(CH_CONTROL, BY_CLIENT, false);
      break;
    case CTRL_SHOW_COMPANION:
      postRequest(REQ_SHOW_COMPANION);
      recordChangeLocked(CH_CONTROL, BY_CLIENT, false);
      break;
    default:
      break;
  }
}

void handleWrite(uint8_t id, uint16_t conn, const uint8_t *data, size_t len) {
  if (id == C_AUTH) { handleAuth(conn, data, len); return; }
  uint16_t mask = maskFor(id);
  if (mask == 0) return;

  Lock lk;
  if (conn != sConn || !sAuthorized) { recordErrorLocked(mask, RES_NOT_AUTH); return; }

  Result r = RES_OK;
  bool bump = true;
  switch (id) {
    case C_THEME: {
      Theme t;
      r = decodeTheme(data, len, t);
      if (r == RES_OK) { applyTheme(t); sLastBase.theme = t; markBaseDirtyLocked(); }
      break;
    }
    case C_BRIGHTNESS: {
      uint8_t v = 0;
      r = decodeBrightness(data, len, v);
      if (r == RES_OK) {
        { ModelLock mlk; model.brightness = v; model.revision++; }
        sLastBase.brightness = v;
        sPendingBacklight = true;
        markBaseDirtyLocked();
      }
      break;
    }
    case C_FACE: {
      FaceCfg f;
      r = decodeFace(data, len, (uint8_t)watchFaceStyleCount(), f);
      if (r == RES_OK) {
        { ModelLock mlk; model.watchFaceStyle = f.style; model.revision++; }
        FaceStore::setFaceOptions(f.options);
        sLastBase.style = f.style;
        markBaseDirtyLocked();
      }
      break;
    }
    case C_SLOTS: {
      SlotCfg s[kSlotCount];
      r = decodeSlots(data, len, s);
      if (r == RES_OK) FaceStore::setSlots(s);
      break;
    }
    case C_TEXTS: {
      uint8_t idx = 0;
      TextEntry t;
      r = decodeTextWrite(data, len, idx, t);
      if (r == RES_OK) FaceStore::setText(idx, t);
      break;
    }
    case C_FEEDS: {
      uint8_t idx = 0;
      FeedEntry f;
      r = decodeFeedWrite(data, len, idx, f);
      if (r == RES_OK) FaceStore::setFeed(idx, f);
      break;
    }
    case C_TIME: {
      TimeSync t;
      r = decodeTimeWrite(data, len, t);
      if (r == RES_OK) {
        CivilTime c;
        unixToCivil(t.unix + (t.millis >= 500 ? 1u : 0u), t.tzOffsetMin, c);
        requestSetRTC(c.hour, c.minute, c.second, c.weekday, c.day, c.month, c.year);
        { ModelLock mlk; model.tzOffsetMin = t.tzOffsetMin; model.revision++; }
        sLastBase.tz = t.tzOffsetMin;
        markBaseDirtyLocked();
      }
      break;
    }
    case C_MESSAGE: {
      Message m;
      r = decodeMessage(data, len, m);
      if (r == RES_OK) {
        sMsg = m;
        sMsgValid = true;
        if (!(m.flags & kMsgFlagSilent)) buzzPattern(200, 80, 90, 200, 80);
        Event e = makeEvent(EventType::BleMessage);
        postEvent(e);
        bump = false;
      }
      break;
    }
    case C_CONTROL: {
      uint8_t op = 0;
      r = decodeControl(data, len, op);
      if (r == RES_OK) {
        sLastClientWrite = millis();
        sWrites++;
        handleControlLocked(op);
        return;
      }
      break;
    }
    default:
      r = RES_UNSUPPORTED;
      break;
  }
  if (r != RES_OK) { recordErrorLocked(mask, r); return; }
  sLastClientWrite = millis();
  sWrites++;
  recordChangeLocked(mask, BY_CLIENT, bump);
}

// ---------------------------------------------------------------------------
// NimBLE callbacks
// ---------------------------------------------------------------------------
class ServerCallbacks : public NimBLEServerCallbacks {
public:
  using NimBLEServerCallbacks::onConnect;
  using NimBLEServerCallbacks::onDisconnect;

  void onConnect(NimBLEServer *srv, ble_gap_conn_desc *desc) override {
    uint32_t now = millis();
    bool reject = false;
    {
      Lock lk;
      if (sConn != 0xFFFF || sLockedOut) {
        reject = true;
      } else {
        sConn = desc->conn_handle;
        sAuthorized = false;
        sCode = esp_random() % 1000000UL;
        sAttempts = kMaxAttempts;
        sPairDeadline = now + kPairTimeoutMs;
        sAdvertising = false;            // legacy advertising stops on connect
        sMtu = 23;
        setStateLocked(State::Pairing, Reason::ClientConnected);
        sPendingNotify |= N_AUTH;
      }
    }
    if (reject) {
      // The controller stopped advertising when this link formed; let the
      // service task restart it if the window is still open.
      { Lock lk; sAdvertising = false; }
      srv->disconnect(desc->conn_handle);
      return;
    }
    // 30-50 ms interval, no latency, 5 s supervision timeout: snappy edits.
    srv->updateConnParams(desc->conn_handle, 24, 40, 0, 500);
    hapticBuzz(90, 40);
  }

  void onDisconnect(NimBLEServer *, ble_gap_conn_desc *desc) override {
    Lock lk;
    if (desc->conn_handle != sConn) return;
    sConn = 0xFFFF;
    sAuthorized = false;
    sCode = 0;
    sDropPending = false;
    Reason why = (sDropReason != Reason::None) ? sDropReason : Reason::ClientLeft;
    sDropReason = Reason::None;
    bool resume = sAdvWanted && before(millis(), sAdvUntil) && !sLockedOut;
    setStateLocked(resume ? State::Advertising : State::Off, why);
    // The service task restarts advertising (after any lockout) and flushes saves.
  }

  void onMTUChange(uint16_t mtu, ble_gap_conn_desc *) override {
    Lock lk;
    sMtu = mtu;
  }
};

class ChrCallbacks : public NimBLECharacteristicCallbacks {
public:
  using NimBLECharacteristicCallbacks::onRead;
  using NimBLECharacteristicCallbacks::onWrite;

  ChrCallbacks() : id_(0) {}
  void setId(uint8_t id) { id_ = id; }

  void onRead(NimBLECharacteristic *c, ble_gap_conn_desc *desc) override {
    uint8_t buf[kInfoMax];
    size_t n = 0;
    if (isOpen(id_) || authorizedConn(desc->conn_handle)) n = buildValue(id_, buf, sizeof(buf));
    c->setValue(buf, n);              // unauthorised reads see an empty value
  }

  void onWrite(NimBLECharacteristic *c, ble_gap_conn_desc *desc) override {
    NimBLEAttValue v = c->getValue();
    uint8_t buf[kInfoMax];
    size_t n = v.length() > sizeof(buf) ? sizeof(buf) : v.length();
    memcpy(buf, v.data(), n);
    handleWrite(id_, desc->conn_handle, buf, n);
  }

private:
  uint8_t id_;
};

ServerCallbacks sServerCb;
ChrCallbacks sChrCb[C_COUNT];

// ---------------------------------------------------------------------------
// Stack bring-up (service task)
// ---------------------------------------------------------------------------
void makeName() {
  uint8_t mac[6] = { 0 };
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(sName, sizeof(sName), "EWatch-%02X%02X", mac[4], mac[5]);
}

void addStr(NimBLEService *svc, uint16_t uuid16, const char *value) {
  NimBLECharacteristic *c = svc->createCharacteristic(NimBLEUUID(uuid16), NIMBLE_PROPERTY::READ);
  c->setValue((const uint8_t *)value, strlen(value));
}

NimBLECharacteristic *addChr(NimBLEService *svc, uint8_t id, const char *uuid,
                             uint32_t props, uint16_t maxLen) {
  NimBLECharacteristic *c = svc->createCharacteristic(uuid, props, maxLen);
  sChrCb[id].setId(id);
  c->setCallbacks(&sChrCb[id]);
  sChr[id] = c;
  return c;
}

void bringUpStack() {
  makeName();
  NimBLEDevice::init(sName);
  // +3 dBm is plenty for a laptop or phone across a room, saves power and
  // shrinks the radius in which a passer-by can even see the watch.
  NimBLEDevice::setPower(ESP_PWR_LVL_P3, ESP_BLE_PWR_TYPE_DEFAULT);
  NimBLEDevice::setPower(ESP_PWR_LVL_P3, ESP_BLE_PWR_TYPE_ADV);
#if EWATCH_BLE_REQUIRE_ENCRYPTION
  NimBLEDevice::setSecurityAuth(/*bonding=*/false, /*mitm=*/false, /*sc=*/true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  const uint32_t ENC_R = NIMBLE_PROPERTY::READ_ENC, ENC_W = NIMBLE_PROPERTY::WRITE_ENC;
#else
  NimBLEDevice::setSecurityAuth(/*bonding=*/false, /*mitm=*/false, /*sc=*/false);
  const uint32_t ENC_R = 0, ENC_W = 0;
#endif
  ble_svc_gap_device_appearance_set(kAppearanceSmartwatch);

  NimBLEServer *srv = NimBLEDevice::createServer();
  srv->setCallbacks(&sServerCb, /*deleteCallbacks=*/false);
  srv->advertiseOnDisconnect(false);

  const uint32_t R = NIMBLE_PROPERTY::READ | ENC_R;
  const uint32_t W = NIMBLE_PROPERTY::WRITE | ENC_W;
  const uint32_t N = NIMBLE_PROPERTY::NOTIFY;

  NimBLEService *svc = srv->createService(kServiceUuid);
  addChr(svc, C_INFO,       kInfoUuid,       NIMBLE_PROPERTY::READ, kInfoMax);
  addChr(svc, C_AUTH,       kAuthUuid,       R | W | N, kAuthWriteLen);
  addChr(svc, C_REVISION,   kRevisionUuid,   R | N,     kRevisionLen);
  addChr(svc, C_THEME,      kThemeUuid,      R | W | N, kThemeLen);
  addChr(svc, C_BRIGHTNESS, kBrightnessUuid, R | W | N, kBrightnessLen);
  addChr(svc, C_FACE,       kFaceUuid,       R | W | N, kFaceLen);
  addChr(svc, C_SLOTS,      kSlotsUuid,      R | W | N, kSlotsLen);
  addChr(svc, C_TEXTS,      kTextsUuid,      R | W,     kTextsReadMax);
  addChr(svc, C_FEEDS,      kFeedsUuid,      R | W,     kFeedsReadMax);
  addChr(svc, C_TIME,       kTimeUuid,       R | W | N, kTimeWriteLen);
  addChr(svc, C_MESSAGE,    kMessageUuid,    W,         kMessageWriteMax);
  addChr(svc, C_CONTROL,    kControlUuid,    W,         kControlWriteMax);
  svc->start();

  // Standard Device Information (0x180A). Serial Number is deliberately
  // absent (Chrome blocks reads of 0x2A25 anyway).
  NimBLEService *dis = srv->createService(NimBLEUUID((uint16_t)0x180A));
  addStr(dis, 0x2A29, "EWatch");                        // Manufacturer Name
  addStr(dis, 0x2A24, "EWatch v2 (ESP32-S3)");          // Model Number
  addStr(dis, 0x2A26, EWATCH_ADDON_VERSION);            // Firmware Revision
  addStr(dis, 0x2A28, "BaseOS dbe73c2 + Companion");    // Software Revision
  dis->start();

  // Standard Battery Service (0x180F): level 0..100 %, readable + notify.
  NimBLEService *bas = srv->createService(NimBLEUUID((uint16_t)0x180F));
  NimBLECharacteristic *lvl = bas->createCharacteristic(
      NimBLEUUID((uint16_t)0x2A19), NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY, 1);
  sChrCb[C_BATTERY].setId(C_BATTERY);
  lvl->setCallbacks(&sChrCb[C_BATTERY]);
  sChr[C_BATTERY] = lvl;
  bas->start();

  srv->start();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData ad, sr;
  ad.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  ad.setCompleteServices(NimBLEUUID(kServiceUuid));     // 18 B: lets Chrome filter on it
  ad.setAppearance(kAppearanceSmartwatch);
  sr.setName(sName);                                     // name goes in the scan response
  adv->setAdvertisementData(ad);
  adv->setScanResponseData(sr);
  adv->setMinInterval(160);                              // 100 ms (0.625 ms units)
  adv->setMaxInterval(240);                              // 150 ms

  sStackUp = true;
  Serial.printf("BLE: up as %s\n", sName);
}

// ---------------------------------------------------------------------------
// Service task
// ---------------------------------------------------------------------------
void notifyValue(uint8_t id) {
  NimBLECharacteristic *c = sChr[id];
  if (!c) return;
  uint8_t buf[kInfoMax];
  size_t n = buildValue(id, buf, sizeof(buf));
  c->setValue(buf, n);
  c->notify();
}

void serviceNotifications() {
  uint32_t pend;
  bool authed, connected;
  {
    Lock lk;
    pend = sPendingNotify;
    sPendingNotify = 0;
    authed = sAuthorized;
    connected = sConn != 0xFFFF;
  }
  if (!pend || !connected || !sStackUp) return;
  if (pend & N_AUTH) notifyValue(C_AUTH);
  if (pend & N_BATTERY) notifyValue(C_BATTERY);
  if (authed) {
    if (pend & CH_THEME)      notifyValue(C_THEME);
    if (pend & CH_BRIGHTNESS) notifyValue(C_BRIGHTNESS);
    if (pend & CH_FACE)       notifyValue(C_FACE);
    if (pend & CH_SLOTS)      notifyValue(C_SLOTS);
    if (pend & CH_TIME)       notifyValue(C_TIME);
  }
  if (pend & N_REVISION) {
    // Revision is a protected characteristic, but an unauthorised client is
    // told its write was refused (RES_NOT_AUTH, revision 0).
    NimBLECharacteristic *c = sChr[C_REVISION];
    if (c) {
      uint8_t buf[kRevisionLen];
      size_t n;
      { Lock lk; n = encodeRevision(sLastRev, buf); }
      c->setValue(buf, n);
      c->notify();
    }
  }
}

void startAdvLocked() {
  if (sAdvertising || !sStackUp) return;
  if (NimBLEDevice::startAdvertising()) sAdvertising = true;
}

void stopAdvLocked() {
  if (!sAdvertising) return;
  NimBLEDevice::stopAdvertising();
  sAdvertising = false;
}

void serviceLifecycle(uint32_t now) {
  uint16_t dropConn = 0xFFFF;
  {
    Lock lk;
    if (sLockedOut && !before(now, sLockoutUntil)) sLockedOut = false;
    bool windowOpen = sAdvWanted && before(now, sAdvUntil);
    if (sAdvWanted && !windowOpen) sAdvWanted = false;

    if (sConn == 0xFFFF) {
      if (windowOpen && !sLockedOut) {
        startAdvLocked();
        if (sAdvertising && sState != State::Advertising) {
          setStateLocked(State::Advertising, Reason::Started);
        }
      } else {
        stopAdvLocked();
        if (sState != State::Off) {
          Reason why = sLockedOut ? Reason::LockedOut
                     : (sUserStopped ? Reason::Stopped : Reason::WindowEnded);
          setStateLocked(State::Off, why);
        }
        sUserStopped = false;
      }
    } else {
      stopAdvLocked();
      if (sState == State::Pairing && !sAuthorized && !sDropPending &&
          !before(now, sPairDeadline)) {
        sDropPending = true;
        sDropAt = now;
        sDropReason = Reason::PairTimeout;
      }
      if (sState == State::Connected) {
        uint32_t last = sLastClientWrite;
        uint32_t user = sLastUserActivity;
        if ((int32_t)(user - last) > 0) last = user;
        if (!sDropPending && (now - last) > kIdleTimeoutMs) {
          sDropPending = true;
          sDropAt = now;
          sDropReason = Reason::IdleTimeout;
        }
      }
      if (sDropPending && !before(now, sDropAt)) {
        dropConn = sConn;
        sDropPending = false;
      }
    }
  }
  if (dropConn != 0xFFFF) {
    NimBLEServer *srv = NimBLEDevice::getServer();
    if (srv) srv->disconnect(dropConn);
  }
}

void serviceWatchChanges(uint32_t now) {
  static uint32_t lastPoll = 0, lastBat = 0;
  if (now - lastPoll >= kWatchPollMs) {
    lastPoll = now;
    BaseSnap cur = readBase();
    Lock lk;
    if (sAuthorized && !sameBase(cur, sLastBase)) {
      uint16_t mask = 0;
      if (cur.theme.bg != sLastBase.theme.bg || cur.theme.fg != sLastBase.theme.fg ||
          cur.theme.accent != sLastBase.theme.accent || cur.theme.line != sLastBase.theme.line) {
        mask |= CH_THEME;
      }
      if (cur.brightness != sLastBase.brightness) mask |= CH_BRIGHTNESS;
      if (cur.style != sLastBase.style) mask |= CH_FACE;
      if (cur.tz != sLastBase.tz) mask |= CH_TIME;
      sLastBase = cur;
      recordChangeLocked(mask, BY_WATCH, true);
    }
  }
  if (now - lastBat >= kBatteryPollMs) {
    lastBat = now;
    uint8_t pct;
    { ModelLock mlk; pct = model.batOk ? model.batPct : 0; }
    Lock lk;
    if (sConn != 0xFFFF && pct != sLastBattery) {
      sLastBattery = pct;
      sPendingNotify |= N_BATTERY;
    }
  }
}

void servicePersistence(uint32_t now) {
  bool saveBase = false, backlight = false;
  {
    Lock lk;
    if (sBaseDirty && (now - sBaseDirtyAt) >= kBaseSaveQuietMs) { sBaseDirty = false; saveBase = true; }
    if (sPendingBacklight) { sPendingBacklight = false; backlight = true; }
  }
  if (backlight) {
    uint8_t b;
    { ModelLock lk; b = model.brightness; }
    backlightSet(b);
  }
  if (saveBase) Storage::save();
  FaceStore::flushIfDue(now);
}

void serviceTask(void *) {
  esp_task_wdt_add(nullptr);
  for (;;) {
    esp_task_wdt_reset();
    bool want;
    { Lock lk; want = sAdvWanted; }
    if (want && !sStackUp) bringUpStack();      // first use: ~0.3 s, off the UI task
    uint32_t now = millis();
    if (sStackUp) {
      serviceLifecycle(now);
      serviceWatchChanges(now);
      serviceNotifications();
    }
    servicePersistence(now);
    vTaskDelay(pdMS_TO_TICKS(kServicePeriodMs));
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void begin() {
  if (sMutex) return;
  sMutex = xSemaphoreCreateMutex();
  sLastRev.revision = FaceStore::configRevision();
  makeName();
  // 4 KiB is ample: the heavy lifting (NimBLE host) runs on its own task.
  xTaskCreatePinnedToCore(serviceTask, "blecfg", 4096, nullptr, 3, &sTask, 0);
}

void startAdvertising(uint32_t windowMs) {
  if (!sMutex) return;
  Lock lk;
  sAdvWanted = true;
  sUserStopped = false;
  sAdvUntil = millis() + (windowMs ? windowMs : kDefaultWindowMs);
}

void stopAdvertising() {
  if (!sMutex) return;
  Lock lk;
  sAdvWanted = false;
  sAdvUntil = millis();
  sUserStopped = true;
}

void disconnect() {
  if (!sMutex) return;
  Lock lk;
  if (sConn == 0xFFFF) return;
  sDropPending = true;
  sDropAt = millis();
  sDropReason = Reason::Kicked;
}

Status status() {
  Status s;
  memset(&s, 0, sizeof(s));
  if (!sMutex) { s.state = State::Off; strncpy(s.deviceName, sName, sizeof(s.deviceName) - 1); return s; }
  uint32_t now = millis();
  Lock lk;
  s.state = sState;
  s.reason = sReason;
  s.radioUp = sStackUp;
  s.code = sCode;
  s.attemptsLeft = sAttempts;
  s.pairRemainingMs = (sState == State::Pairing && !sAuthorized) ? remaining(now, sPairDeadline) : 0;
  s.advRemainingMs = (sAdvWanted) ? remaining(now, sAdvUntil) : 0;
  s.lockoutRemainingMs = sLockedOut ? remaining(now, sLockoutUntil) : 0;
  s.sessionMs = sAuthorized ? now - sSessionStart : 0;
  s.writes = sWrites;
  s.lastWriteMs = sLastClientWrite;
  s.mtu = sMtu;
  strncpy(s.deviceName, sName, sizeof(s.deviceName) - 1);
  return s;
}

bool clientConnected() {
  if (!sMutex) return false;
  Lock lk;
  return sConn != 0xFFFF;
}

uint32_t lastActivityMs() { return sLastClientWrite; }

void noteUserActivity() { sLastUserActivity = millis(); }

bool takeMessage(Message &out) {
  if (!sMutex) return false;
  Lock lk;
  if (!sMsgValid) return false;
  out = sMsg;
  return true;
}

void resetFaceFromWatch() {
  FaceStore::resetToDefaults();
  if (!sMutex) { FaceStore::bumpConfigRevision(); return; }
  Lock lk;
  recordChangeLocked(CH_SLOTS | CH_TEXTS | CH_FEEDS | CH_FACE, BY_WATCH, true);
}

void resetThemeFromWatch() {
  Theme t = { kDefBg, kDefFg, kDefAccent, kDefLine };
  applyTheme(t);
  { ModelLock lk; model.brightness = kDefBrightness; model.watchFaceStyle = FaceRender::kDefaultStyle; model.revision++; }
  backlightSet(kDefBrightness);
  Storage::save();
  if (!sMutex) return;
  Lock lk;
  sLastBase = readBase();
  recordChangeLocked(CH_THEME | CH_BRIGHTNESS | CH_FACE, BY_WATCH, true);
}

void prepareForSleep() {
  FaceStore::flushNow();
  if (!sMutex) return;
  bool saveBase;
  uint16_t conn;
  {
    Lock lk;
    saveBase = sBaseDirty;
    sBaseDirty = false;
    conn = sConn;
    sAdvWanted = false;
  }
  if (saveBase) Storage::save();
  if (!sStackUp) return;
  NimBLEDevice::stopAdvertising();
  if (conn != 0xFFFF) {
    NimBLEServer *srv = NimBLEDevice::getServer();
    if (srv) srv->disconnect(conn);
    vTaskDelay(pdMS_TO_TICKS(60));           // let the LL_TERMINATE go out
  }
}

}  // namespace BleConfig

#else  // EWATCH_ENABLE_BLE == 0: stubs so the rest of the firmware links.

#include "face_store.h"

namespace BleConfig {
void begin() {}
void startAdvertising(uint32_t) {}
void stopAdvertising() {}
void disconnect() {}
Status status() {
  Status s;
  memset(&s, 0, sizeof(s));
  s.state = State::Off;
  strncpy(s.deviceName, "BLE off", sizeof(s.deviceName) - 1);
  return s;
}
bool clientConnected() { return false; }
uint32_t lastActivityMs() { return 0; }
void noteUserActivity() {}
bool takeMessage(bleproto::Message &) { return false; }
void resetFaceFromWatch() { FaceStore::resetToDefaults(); }
void resetThemeFromWatch() {}
void prepareForSleep() { FaceStore::flushNow(); }
}  // namespace BleConfig

#endif  // EWATCH_ENABLE_BLE
