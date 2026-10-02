#!/usr/bin/env python3
"""Push meetings to the EWatch Meeting Countdown face over the USB cable.

Run it with an interpreter that has pyserial, for example PlatformIO's:
    ~/.platformio/penv/bin/python tools/push_events.py --list
(or `pip install pyserial` into your own environment).

Examples
    # One event (local watch time), 30 minutes long, leave 10 min early:
    push_events.py --event "2026-10-01T09:30 +30m leave=10 loc=\"Room 4\" Standup"
    # Replace today's USB events with the lines of a file (# for comments):
    push_events.py --clear --file today.txt
    # Send a whole .ics file; the watch parses it (recurrence, time zones):
    push_events.py --ics ~/Downloads/calendar.ics
    # Look around / housekeeping:
    push_events.py --list --status
    push_events.py --feed 1 "https://calendar.google.com/calendar/ical/.../basic.ics"
    push_events.py --set offsets 10,5 --set lead 60 --sync
    push_events.py --test-alert

Event syntax (same as the serial protocol's EVENT command):
    <start> <end|+duration> [leave=<min>] [loc=<word>|loc="<words>"] <title>
    start/end: 2026-10-01T09:30, 2026-10-01T08:30:00Z, 2026-10-01T09:30+01:00,
               2026-10-02 2026-10-03 (all-day). Durations: +30m, 1h30m, PT45M.

The watch must be awake: USB serial disappears while it sleeps. The script
waits for the port to come back (press the button), and serial traffic keeps
the watch awake while it runs.
"""
import argparse
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial is missing: run with ~/.platformio/penv/bin/python, or pip install pyserial")

ESPRESSIF_VID = 0x303A


def find_port():
    ports = list(list_ports.comports())
    for p in ports:
        if p.vid == ESPRESSIF_VID:
            return p.device
    for p in ports:
        name = (p.device or "").lower()
        if "usbmodem" in name or "ttyacm" in name:
            return p.device
    return None


class Watch:
    def __init__(self, port, wait):
        self.port_name = port
        self.wait = wait
        self.ser = None

    def open(self):
        deadline = time.time() + self.wait
        told = False
        while True:
            port = self.port_name or find_port()
            if port:
                try:
                    s = serial.Serial()
                    s.port = port
                    s.baudrate = 115200
                    s.timeout = 0.2
                    # Keep DTR/RTS low: toggling them can reset an ESP32-S3.
                    s.dtr = False
                    s.rts = False
                    s.open()
                    self.ser = s
                    time.sleep(0.15)
                    self.ser.reset_input_buffer()
                    return
                except (serial.SerialException, OSError):
                    pass
            if time.time() > deadline:
                sys.exit("Could not reach the watch. Plug it in and press its button to wake it.")
            if not told:
                print("Waiting for the watch... press its button to wake it.", file=sys.stderr)
                told = True
            time.sleep(0.5)

    def send(self, line):
        self.ser.write((line + "\n").encode("utf-8"))
        self.ser.flush()

    def read_line(self, timeout):
        end = time.time() + timeout
        buf = b""
        while time.time() < end:
            c = self.ser.read(1)
            if not c:
                continue
            if c in (b"\n", b"\r"):
                if buf:
                    return buf.decode("utf-8", "replace")
                continue
            buf += c
        return None

    def command(self, line, timeout=6.0, echo=True):
        """Send one command; return (ok, data_lines, final_line)."""
        self.send(line)
        data = []
        while True:
            reply = self.read_line(timeout)
            if reply is None:
                return False, data, "ERR no reply (is the watch awake?)"
            if reply.startswith("OK") or reply.startswith("ERR"):
                return reply.startswith("OK"), data, reply
            if reply.startswith(("EV ", "KV ", "FEED ", "... ", "ACK")):
                data.append(reply)
                if echo and reply.startswith("... "):
                    print(reply)
            # anything else is firmware logging: ignore


def push_ics(w, path):
    with open(path, "rb") as f:
        text = f.read().decode("utf-8", "replace")
    ok, _, final = w.command("ICS BEGIN")
    if not ok:
        sys.exit(final)
    lines = text.replace("\r\n", "\n").replace("\r", "\n").split("\n")
    sent = 0
    for line in lines:
        w.send(line)
        sent += 1
        if sent % 32 == 0:
            # Flow control: the watch acknowledges every 32 lines.
            while True:
                reply = w.read_line(5.0)
                if reply is None:
                    sys.exit("The watch stopped acknowledging (did it fall asleep?)")
                if reply == "ACK":
                    break
                if reply.startswith("ERR"):
                    sys.exit(reply)
    ok, _, final = w.command("ICS END", timeout=10.0)
    print(final)
    if not ok:
        sys.exit(1)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("--port", help="serial port (default: auto-detect the EWatch)")
    ap.add_argument("--wait", type=float, default=30, help="seconds to wait for the watch (default 30)")
    ap.add_argument("--clear", action="store_true", help="remove previously pushed events first")
    ap.add_argument("--event", action="append", default=[], metavar="SPEC", help="add one event (repeatable)")
    ap.add_argument("--file", help="add events from a text file, one EVENT spec per line")
    ap.add_argument("--ics", help="send an .ics file (replaces pushed events)")
    ap.add_argument("--delete", action="append", default=[], metavar="ID", help="delete a pushed/web event by id")
    ap.add_argument("--feed", nargs=2, metavar=("N", "URL"), help="set calendar feed N (1-3); URL 'clear' removes it")
    ap.add_argument("--set", nargs=2, action="append", default=[], metavar=("KEY", "VALUE"),
                    help="change a setting, e.g. --set offsets 10,5")
    ap.add_argument("--sync", action="store_true", help="sync the calendar feeds now")
    ap.add_argument("--test-alert", action="store_true", help="show the alert screen with the next event")
    ap.add_argument("--list", action="store_true", help="list upcoming events")
    ap.add_argument("--status", action="store_true", help="show status")
    args = ap.parse_args()

    w = Watch(args.port, args.wait)
    w.open()
    ok, _, hello = w.command("HELLO")
    if not ok or "meeting-countdown" not in hello:
        sys.exit(f"Unexpected reply from the watch: {hello}")
    print(hello[3:])

    failures = 0

    def run(cmd, timeout=6.0, show=True):
        nonlocal failures
        ok, data, final = w.command(cmd, timeout=timeout)
        if show:
            for d in data:
                if not d.startswith("... "):
                    print(d)
        if not ok:
            failures += 1
            print(f"{cmd!r}: {final}", file=sys.stderr)
        return ok, final

    if args.clear:
        run("CLEAR")
    specs = list(args.event)
    if args.file:
        with open(args.file, encoding="utf-8") as f:
            for raw in f:
                line = raw.strip()
                if line and not line.startswith("#"):
                    specs.append(line[6:] if line.upper().startswith("EVENT ") else line)
    for spec in specs:
        ok, final = run("EVENT " + spec, show=False)
        if ok:
            print(f"added  {spec}  ({final[3:]})")
    for i in args.delete:
        run("DEL " + i)
    if args.ics:
        push_ics(w, args.ics)
    if args.feed:
        run(f"FEED {args.feed[0]} {args.feed[1]}")
    for key, value in args.set:
        run(f"SET {key} {value}")
    if args.sync:
        ok, final = run("SYNC", timeout=110.0)
        print(final)
    if args.test_alert:
        run("ALERT TEST")
    if args.list:
        run("LIST")
    if args.status:
        run("STATUS")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
