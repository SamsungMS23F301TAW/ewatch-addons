// Thin NimBLE wrapper for Friend Radar (device only).
//
// Uses NimBLE-Arduino 1.4.3 for stack bring-up and the raw NimBLE GAP API for
// the radio work, which keeps the scan path allocation-free:
//   * advertising: legacy, non-connectable, non-scannable (ADV_NONCONN_IND),
//     from a fresh random static address each session (never the chip MAC);
//   * scanning: passive, duplicates NOT filtered so RSSI keeps updating; the
//     GAP callback decodes each report and queues only EWatch Radar beacons.
// All functions except the queue are meant to be called from one task.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "fr_beacon.h"

namespace rble {

struct Report {
  fr::Beacon b;
  int8_t     rssi;
  uint32_t   ms;      // millis() at reception
};

bool begin();                       // init stack + random address; idempotent
void end();                         // full deinit (frees the controller)
bool ready();                       // initialised and host synced

bool setAdvData(const uint8_t *data, size_t len);
bool startAdv(uint16_t intervalMs);
void stopAdv();
bool advertising();

bool startScan(uint16_t intervalMs, uint16_t windowMs);
void stopScan();
bool scanning();

QueueHandle_t queue();              // Report items from the scan callback
uint32_t droppedReports();          // queue overflows (diagnostics)
uint32_t seenReports();             // EWatch beacons received (diagnostics)

}  // namespace rble
