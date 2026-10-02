#include "radar_ble.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include "nimble/nimble/host/include/host/ble_hs.h"

namespace rble {

static QueueHandle_t sQueue = nullptr;
static volatile uint32_t sDropped = 0, sSeen = 0;
static bool sInit = false;
static uint8_t sOwnAddrType = BLE_OWN_ADDR_PUBLIC;

static int gapEvent(struct ble_gap_event *ev, void *) {
  if (ev->type == BLE_GAP_EVENT_DISC) {
    Report r;
    if (fr::decodeAdv(ev->disc.data, ev->disc.length_data, r.b)) {
      r.rssi = ev->disc.rssi;
      r.ms = millis();
      if (sQueue && xQueueSend(sQueue, &r, 0) != pdPASS) sDropped = sDropped + 1;
      sSeen = sSeen + 1;
    }
  }
  return 0;
}

// A fresh random static address (the factory MAC never goes on the air).
static bool newRandomAddress() {
  ble_addr_t addr;
  if (ble_hs_id_gen_rnd(0, &addr) == 0 && ble_hs_id_set_rnd(addr.val) == 0) {
    sOwnAddrType = BLE_OWN_ADDR_RANDOM;
    return true;
  }
  sOwnAddrType = BLE_OWN_ADDR_PUBLIC;
  return false;
}

QueueHandle_t queue() {
  if (!sQueue) sQueue = xQueueCreate(32, sizeof(Report));
  return sQueue;
}

uint32_t droppedReports() { return sDropped; }
uint32_t seenReports() { return sSeen; }

bool begin() {
  queue();
  if (sInit) return ready();
  NimBLEDevice::init("EWatch");          // blocks until host/controller sync (~0.1-0.3 s)
  sInit = true;
  NimBLEDevice::setPower(ESP_PWR_LVL_P9, ESP_BLE_PWR_TYPE_ADV);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9, ESP_BLE_PWR_TYPE_DEFAULT);
  // A fresh random static address per session: the factory MAC (shared with
  // WiFi) never goes on the air. Identity on the radar is the beacon's id.
  newRandomAddress();
  return ready();
}

void end() {
  if (!sInit) return;
  stopScan();
  stopAdv();
  NimBLEDevice::deinit(true);
  sInit = false;
}

bool ready() { return sInit && ble_hs_synced(); }

bool setAdvData(const uint8_t *data, size_t len) {
  if (!ready()) return false;
  return ble_gap_adv_set_data(data, (int)len) == 0;
}

bool startAdv(uint16_t intervalMs) {
  if (!ready()) return false;
  if (ble_gap_adv_active()) return true;
  struct ble_gap_adv_params p;
  memset(&p, 0, sizeof p);
  p.conn_mode = BLE_GAP_CONN_MODE_NON;
  p.disc_mode = BLE_GAP_DISC_MODE_NON;
  p.itvl_min = (uint16_t)BLE_GAP_ADV_ITVL_MS(intervalMs);
  p.itvl_max = (uint16_t)BLE_GAP_ADV_ITVL_MS(intervalMs + intervalMs / 8 + 5);
  int rc = ble_gap_adv_start(sOwnAddrType, nullptr, BLE_HS_FOREVER, &p, gapEvent, nullptr);
  if (rc == BLE_HS_ENOADDR && newRandomAddress())      // address lost in a host reset
    rc = ble_gap_adv_start(sOwnAddrType, nullptr, BLE_HS_FOREVER, &p, gapEvent, nullptr);
  if (rc != 0 && intervalMs < 100) {
    // Some controllers insist on >= 100 ms for non-connectable adverts.
    p.itvl_min = (uint16_t)BLE_GAP_ADV_ITVL_MS(100);
    p.itvl_max = (uint16_t)BLE_GAP_ADV_ITVL_MS(115);
    rc = ble_gap_adv_start(sOwnAddrType, nullptr, BLE_HS_FOREVER, &p, gapEvent, nullptr);
  }
  return rc == 0 || rc == BLE_HS_EALREADY;
}

void stopAdv() {
  if (sInit && ble_gap_adv_active()) ble_gap_adv_stop();
}

bool advertising() { return sInit && ble_gap_adv_active(); }

bool startScan(uint16_t intervalMs, uint16_t windowMs) {
  if (!ready()) return false;
  if (ble_gap_disc_active()) return true;
  struct ble_gap_disc_params d;
  memset(&d, 0, sizeof d);
  d.itvl = (uint16_t)BLE_GAP_SCAN_ITVL_MS(intervalMs);
  d.window = (uint16_t)BLE_GAP_SCAN_WIN_MS(windowMs);
  d.filter_policy = BLE_HCI_SCAN_FILT_NO_WL;
  d.limited = 0;
  d.passive = 1;                 // never send scan requests
  d.filter_duplicates = 0;       // keep every report: RSSI must keep updating
  int rc = ble_gap_disc(sOwnAddrType, BLE_HS_FOREVER, &d, gapEvent, nullptr);
  if (rc == BLE_HS_ENOADDR && newRandomAddress())
    rc = ble_gap_disc(sOwnAddrType, BLE_HS_FOREVER, &d, gapEvent, nullptr);
  return rc == 0 || rc == BLE_HS_EALREADY;
}

void stopScan() {
  if (sInit && ble_gap_disc_active()) ble_gap_disc_cancel();
}

bool scanning() { return sInit && ble_gap_disc_active(); }

}  // namespace rble
