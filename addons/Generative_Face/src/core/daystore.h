// The day collection on flash: a gf::DayLog persisted to LittleFS on the
// huge_app "spiffs" partition at /generative-face/days.bin. Mounted lazily
// on first use; saves go to a temp file that is renamed over the old one,
// so a power cut mid-write never loses the collection. Thread-safe.
//
// Nothing here may throw the collection away:
//   * mount: only a partition that has never mounted (NVS "fsSeen") is
//     formatted straight away. One that has mounted before is formatted
//     only after failing on three separate boots (NVS "fsFail").
//   * load: a file that fails its checks is salvaged record by record and
//     kept as days.bad. An I/O or memory failure leaves the collection
//     offline for this boot (no saves) rather than saving an empty one.
//   * a day that closes while the collection is offline waits in
//     steps_hw's parked list ("pend" in NVS) and is filed later.
#pragma once
#include <stdint.h>
#include "dayrec.h"

void    daystoreInit();                               // create the lock (setup)
bool    daystoreAdd(const gf::DayRecord &r);          // upsert + save; false = not saved
int32_t daystoreCount();
bool    daystoreAt(int32_t i, gf::DayRecord &out);    // 0 = oldest
bool    daystoreFind(uint16_t day, gf::DayRecord &out);
bool    daystoreMounted();
bool    daystoreAttempted();                          // mount tried (ok or not)
bool    daystoreAvailable();                          // loaded; false if offline this boot
