// USB-serial console (115200 8N1, line based). Never required for normal
// use; tools/snap.py speaks it.
//
//   HELP
//   SNAP                     the current face (today's art + the clock)
//   SNAP YYYY-MM-DD [PLAIN]  a recorded day (or today) with its label, or bare
//   DAYS                     list the collection
//   ARTHASH YYYY-MM-DD STEPS determinism fingerprint (compare with the host)
//   STEPS / PWR              step detector and power-manager diagnostics
//   DAYADD YYYY-MM-DD STEPS  add or raise a record (testing the gallery)
//
// SNAP reply: "SNAP BEGIN w=240 h=280 fmt=RGB565LE bytes=134400 day=... steps=...
// algo=... family=...\n", then the raw pixels (row-major, little-endian
// RGB565), then "SNAP END crc32=xxxxxxxx\n". Errors: "SNAP ERR <reason>\n".
#pragma once

void consolePoll();     // call from loop()

// Dayprint's runtime log lines, from any task. Dropped while SNAP streams
// its binary payload, so a log line can never land inside the pixels.
void gfLog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
