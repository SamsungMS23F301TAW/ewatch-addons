// Rep Counter: serial console (USB CDC, 115200), polled from loop().
//
//   help        list commands
//   REC on      stream raw accelerometer CSV (see tools/record.py)
//   REC off     stop streaming
//   status      what the app is doing right now
//   log         today's sets as CSV
//
// Nothing here is needed for normal use. While REC is on the watch stays
// awake and the accelerometer runs in its 100 Hz FIFO mode.
#pragma once
#include <stdint.h>

void repsConsolePoll();              // loopTask
bool repsConsoleBusy();              // REC streaming: poll faster
bool repsRecActive();                // any task
// From the render task: one "# ev ..." line for the recording (dropped when
// no recording is running or the queue is full).
void repsConsoleEvent(const char *line);
// From the render task: the latest one-line status for `status`.
void repsConsoleSetStatus(const char *line);
