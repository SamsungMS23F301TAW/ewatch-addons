// Meeting Countdown — USB serial line protocol (115200, newline-terminated).
// Lets EWatch Cloud or tools/push_events.py load the day over the cable.
//
//   HELLO                         -> OK meeting-countdown <ver> proto=1 tz=<+min>
//   TIME                          -> OK <watch local time, ISO-8601 with offset>
//   EVENT <start> <end|+dur> [leave=<min>] [loc=<x>|loc="<x y>"] <title>
//                                 -> OK id=<hex>          (adds/updates a USB event)
//   DEL <id>                      -> OK | ERR
//   LIST                          -> EV <id> <start> <end> <feed|web|usb> <title> ... OK n=<k>
//   CLEAR [ALL]                   -> OK cleared=<n>       (USB events; ALL = + web + feed cache)
//   ICS BEGIN                     -> OK; then raw .ics lines (ACK after every 32 lines),
//   ICS END                       -> OK events=<n>        (replaces USB events)
//   FEED                          -> FEED <n> <redacted url> ... OK
//   FEED <n> <url> | FEED <n> CLEAR
//   SYNC                          -> progress lines "... <step>", then OK|ERR <result>
//   STATUS                        -> KV <key> <value> ... OK
//   SET <key> <value>             -> alerts on|off, offsets 5,1, lead <min>, sync <min>,
//                                    night on|off, autotz on|off, snooze <min>,
//                                    face meeting|classic, insecure on|off
//   ALERT TEST                    -> shows the alert screen with the next event
//   HELP
// Every command ends with exactly one line starting "OK" or "ERR".
#pragma once

namespace mcconsole {
void begin();
void poll();      // call from loop(); never blocks except during SYNC
}
