// Tiny serial console for Pixel Pet (115200 baud, newline-terminated).
// Optional (-DPIXELPET_CONSOLE=1); the watch never needs it. Note that real
// light sleep stops USB-CDC: plug in first (USB attached = simulated sleep),
// or wake the watch, before typing.
//
//   help            this list
//   status          clock, steps, pet, battery, background-power stats
//   pet             full pet state
//   hist            last 14 days of steps
//   steps <n>       pretend you walked n steps now (testing without walking)
//   full <0-100>    set the pet's fullness (testing moods)
//   rec <seconds>   record raw accelerometer samples (12.5 Hz) into PSRAM
//   rec stop        stop recording early
//   dump            print the recording as CSV: x,y,z in counts (4096 = 1 g)
//   buzz            play the snack haptic
//   off             turn the screen off now
#pragma once

void consolePoll();   // call from loop(); non-blocking
