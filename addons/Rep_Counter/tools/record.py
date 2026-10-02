#!/usr/bin/env python3
"""Record raw accelerometer data from Rep Counter for tuning and regression tests.

The watch streams CSV over USB when it receives `REC on` (see README,
"Recording sets for tuning"). This script starts the stream, saves it with a
label and the expected rep counts, shows live progress (including the watch's
own rep detections), and stops it again on Ctrl-C.

Needs pyserial. It ships inside PlatformIO's Python, so either:
    ~/.platformio/penv/bin/python tools/record.py --label "curls 12 kg" --expect 12,10
or  pip install pyserial  and run it with python3.

Examples:
    record.py                                   # auto-detect the watch, Ctrl-C to stop
    record.py --expect 12,10,8 --exercise Curl,Curl,Curl --label "db curl 10kg"
    record.py --expect "" --label "walking to the rack"   # expect no sets at all
    record.py --duration 120 --out recordings/press1.csv

Afterwards, replay it through the host-built detector:
    tools/host/build.sh && build/host/rep_replay recordings/<file>.csv --events
and, once the expectation looks right, copy it into test/data/ so it becomes a
regression test (pio test -e native).
"""
import argparse
import datetime as dt
import os
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
        if 'usbmodem' in p.device or 'ttyACM' in p.device:
            return p.device
    names = ', '.join(p.device for p in ports) or 'none'
    sys.exit(f"couldn't find the watch (ports: {names}); pass --port")


def open_port(path):
    s = serial.Serial()
    s.port = path
    s.baudrate = 115200
    s.timeout = 0.2
    # Keep DTR/RTS low: toggling them on the ESP32-S3's USB serial can reset
    # the watch (and it would come back up on the watch face).
    s.dtr = False
    s.rts = False
    s.open()
    return s


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--port', help='serial port (default: auto-detect)')
    ap.add_argument('--out', help='output CSV (default: recordings/rec_<time>.csv)')
    ap.add_argument('--label', default='', help='free text saved in the header')
    ap.add_argument('--expect', default=None,
                    help='expected reps per set, comma separated ("" = no sets)')
    ap.add_argument('--exercise', default='', help='expected exercise per set, e.g. Curl,Press')
    ap.add_argument('--tol', type=int, default=1, help='allowed error per set (default 1)')
    ap.add_argument('--duration', type=float, default=0, help='stop after N seconds')
    args = ap.parse_args()

    port = args.port or find_port()
    out = args.out or os.path.join('recordings', dt.datetime.now().strftime('rec_%Y%m%d_%H%M%S.csv'))
    os.makedirs(os.path.dirname(out) or '.', exist_ok=True)

    ser = open_port(port)
    time.sleep(0.3)
    ser.reset_input_buffer()
    ser.write(b'REC off\n')            # in case a previous session was left running
    time.sleep(0.3)
    ser.reset_input_buffer()

    with open(out, 'w', newline='\n') as f:
        f.write(f'# recorded={dt.datetime.now().isoformat(timespec="seconds")} port={port}\n')
        if args.label:
            f.write('# label=' + args.label.replace(' ', '_') + '\n')
        if args.expect is not None:
            line = f'# expect reps={args.expect}'
            if args.exercise:
                line += f' exercise={args.exercise}'
            line += f' tol={args.tol}\n'
            f.write(line)
        ser.write(b'REC on\n')
        print(f'recording from {port} -> {out}  (Ctrl-C to stop)')
        started = False
        samples = 0
        t0 = time.time()
        last_print = 0
        reps_seen = ''
        buf = b''
        try:
            while True:
                chunk = ser.read(4096)
                if chunk:
                    buf += chunk
                    *lines, buf = buf.split(b'\n')
                    for raw in lines:
                        line = raw.decode('ascii', 'replace').rstrip('\r')
                        if not started:
                            if line.startswith('# rep-counter rec'):
                                started = True
                            else:
                                continue
                        f.write(line + '\n')
                        if line and line[0].isdigit():
                            samples += 1
                        elif line.startswith('# ev '):
                            kv = dict(p.split('=', 1) for p in line[5:].split() if '=' in p)
                            if kv.get('type') in ('set_start', 'rep', 'set_end'):
                                reps_seen = f"{kv.get('type')} {kv.get('reps')} {kv.get('ex')}"
                        elif line.startswith('# gap'):
                            print(f'\n  {line}')
                now = time.time()
                if now - last_print > 0.5:
                    last_print = now
                    el = now - t0
                    rate = samples / el if el > 0 else 0
                    status = 'waiting for watch' if not started else f'{samples} samples ({rate:.0f}/s)'
                    print(f'\r  {el:6.1f}s  {status}  {reps_seen:28s}', end='', flush=True)
                if args.duration and now - t0 >= args.duration:
                    break
        except KeyboardInterrupt:
            pass
        ser.write(b'REC off\n')
        end_by = time.time() + 1.5
        while time.time() < end_by:
            chunk = ser.read(4096)
            if not chunk:
                continue
            buf += chunk
            *lines, buf = buf.split(b'\n')
            for raw in lines:
                line = raw.decode('ascii', 'replace').rstrip('\r')
                if started:
                    f.write(line + '\n')
                if line.startswith('# end'):
                    end_by = 0
    ser.close()
    print(f'\nsaved {samples} samples to {out}')
    if not started:
        print("no data: is Rep Counter's firmware on the watch, and the port right?")
        return 1
    print(f'replay:  build/host/rep_replay {out} --events')
    return 0


if __name__ == '__main__':
    sys.exit(main())
