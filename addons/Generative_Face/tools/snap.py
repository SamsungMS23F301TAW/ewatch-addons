#!/usr/bin/env python3
"""Dayprint snapshots: save the watch's artwork as PNGs over USB serial.

Run with PlatformIO's Python (it has pyserial), or `pip install pyserial`:

    ~/.platformio/penv/bin/python tools/snap.py now                 # current face
    ~/.platformio/penv/bin/python tools/snap.py day 2026-10-01      # one recorded day
    ~/.platformio/penv/bin/python tools/snap.py list                # the collection
    ~/.platformio/penv/bin/python tools/snap.py year 2026           # every recorded
                                                                    # day + a poster
    ~/.platformio/penv/bin/python tools/snap.py sheet snaps/2026    # poster from PNGs
    ~/.platformio/penv/bin/python tools/snap.py hash 2026-10-01 8000

Options: --port /dev/cu.usbmodemXXXX (auto-detected otherwise), --out DIR,
--plain (art without the date label). The watch must be awake or plugged in
(the screen-off sleep keeps the USB console alive while a computer is
attached). Offline testing: `--from-file stream.bin` parses a captured SNAP
stream instead of a serial port.

Protocol (see src/core/console.h): the reply to "SNAP ..." is a text line
"SNAP BEGIN w=240 h=280 fmt=RGB565LE bytes=134400 day=... steps=..." then
the raw little-endian RGB565 pixels, then "SNAP END crc32=xxxxxxxx".
Only the Python standard library is needed besides pyserial.
"""
import argparse
import datetime
import glob
import os
import re
import struct
import sys
import time
import zlib

W, H = 240, 280


# --------------------------------------------------------------------- PNG
def png_write(path, rgb, w, h):
    raw = bytearray()
    stride = w * 3
    for y in range(h):
        raw.append(0)
        raw += rgb[y * stride:(y + 1) * stride]

    def chunk(kind, data):
        c = kind + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        f.write(chunk(b"IEND", b""))


def png_read(path):
    """Reads 8-bit RGB/RGBA non-interlaced PNGs (what this tool writes)."""
    try:
        from PIL import Image  # optional and much faster
        im = Image.open(path).convert("RGB")
        return bytearray(im.tobytes()), im.width, im.height
    except ImportError:
        pass
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG: " + path)
    pos, idat, w = 8, b"", 0
    while pos < len(data):
        n = struct.unpack(">I", data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, inter = struct.unpack(">IIBBBBB", body)
            if depth != 8 or ctype not in (2, 6) or inter:
                raise ValueError("unsupported PNG format: " + path)
            bpp = 3 if ctype == 2 else 4
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * bpp
    out = bytearray(w * h * 3)
    prev = bytearray(stride)
    for y in range(h):
        ft = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if ft == 1:
                line[i] = (line[i] + a) & 255
            elif ft == 2:
                line[i] = (line[i] + b) & 255
            elif ft == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif ft == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        for x in range(w):
            out[(y * w + x) * 3:(y * w + x) * 3 + 3] = line[x * bpp:x * bpp + 3]
        prev = line
    return out, w, h


def rgb565le_to_rgb(payload):
    out = bytearray(len(payload) // 2 * 3)
    for i in range(len(payload) // 2):
        v = payload[2 * i] | (payload[2 * i + 1] << 8)
        r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
        out[3 * i] = (r << 3) | (r >> 2)
        out[3 * i + 1] = (g << 2) | (g >> 4)
        out[3 * i + 2] = (b << 3) | (b >> 2)
    return out


# --------------------------------------------------------------------- link
class Link:
    """A serial port, or a captured byte stream for offline tests."""

    def __init__(self, port=None, from_file=None, baud=115200):
        self.buf = b""
        self.file = None
        if from_file:
            with open(from_file, "rb") as f:
                self.buf = f.read()
            self.ser = None
            return
        try:
            import serial  # noqa: F401
            import serial.tools.list_ports
        except ImportError:
            sys.exit("pyserial is missing: run with ~/.platformio/penv/bin/python "
                     "or `pip install pyserial`")
        if not port:
            cands = [p.device for p in serial.tools.list_ports.comports()
                     if "usbmodem" in p.device or "ttyACM" in p.device or (p.vid == 0x303A)]
            if not cands:
                sys.exit("no watch found: plug it in and wake it, or pass --port")
            port = cands[0]
        self.ser = serial.Serial(port, baud, timeout=0.2)
        time.sleep(0.2)
        self.ser.reset_input_buffer()

    def send(self, line):
        if self.ser:
            self.ser.write((line + "\n").encode())

    def _fill(self, timeout):
        if not self.ser:
            return False
        t0 = time.time()
        while time.time() - t0 < timeout:
            chunk = self.ser.read(4096)
            if chunk:
                self.buf += chunk
                return True
        return False

    def readline(self, timeout=10.0):
        t0 = time.time()
        while b"\n" not in self.buf:
            if not self._fill(max(0.05, timeout - (time.time() - t0))) or time.time() - t0 > timeout:
                if b"\n" not in self.buf:
                    return None
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode(errors="replace").strip()

    def drain(self):
        """Discards the rest of a failed transfer before trying again."""
        self.buf = b""
        if self.ser:
            time.sleep(0.3)
            while self.ser.read(65536):
                pass

    def readexact(self, n, timeout=30.0):
        t0 = time.time()
        while len(self.buf) < n:
            if time.time() - t0 > timeout or not self._fill(1.0):
                if len(self.buf) < n and (not self.ser or time.time() - t0 > timeout):
                    raise IOError("timed out receiving pixels (%d of %d bytes)" % (len(self.buf), n))
        data, self.buf = self.buf[:n], self.buf[n:]
        return data


def snap(link, args_line):
    link.send(("SNAP " + args_line).strip())
    meta = None
    for _ in range(200):
        line = link.readline(timeout=40.0)
        if line is None:
            raise IOError("no reply from the watch (is it awake?)")
        if line.startswith("SNAP ERR"):
            raise IOError(line)
        if line.startswith("SNAP BEGIN"):
            meta = dict(kv.split("=", 1) for kv in line.split()[2:] if "=" in kv)
            break
    if meta is None:
        raise IOError("no SNAP header")
    n = int(meta["bytes"])
    w, h = int(meta["w"]), int(meta["h"])
    payload = link.readexact(n)
    for _ in range(20):
        line = link.readline(timeout=10.0)
        if line is None:
            raise IOError("no SNAP trailer")
        m = re.match(r"SNAP END crc32=([0-9a-fA-F]+)", line)
        if m:
            if int(m.group(1), 16) != (zlib.crc32(payload) & 0xFFFFFFFF):
                raise IOError("CRC mismatch: the transfer was corrupted, try again")
            break
    return rgb565le_to_rgb(payload), w, h, meta


def snap_retry(link, args_line, tries=3):
    """snap(), retried after a damaged transfer. "SNAP ERR" replies are final."""
    for attempt in range(tries):
        try:
            return snap(link, args_line)
        except IOError as e:
            if str(e).startswith("SNAP ERR") or not link.ser or attempt == tries - 1:
                raise
            print("transfer failed (%s); retrying" % e)
            link.drain()


def list_days(link):
    link.send("DAYS")
    days = []
    while True:
        line = link.readline(timeout=10.0)
        if line is None:
            raise IOError("no reply to DAYS")
        if line.startswith("DAY "):
            parts = line.split()
            days.append({"date": parts[1], "steps": int(parts[2]), "algo": parts[3],
                         "family": parts[4] if len(parts) > 4 else ""})
        elif line.startswith("TODAY "):
            pass
        elif line.startswith("DAYS END"):
            return days


# --------------------------------------------------------------------- poster
# A tiny 5x7 font for the poster's month and day labels.
FONT = {
    "A": "01110100011000111111100011000110001", "B": "11110100011000111110100011000111110",
    "C": "01110100011000010000100001000101110", "D": "11110100011000110001100011000111110",
    "E": "11111100001000011110100001000011111", "F": "11111100001000011110100001000010000",
    "G": "01110100011000010111100011000101111", "J": "00111000100001000010000101001001100",
    "L": "10000100001000010000100001000011111", "M": "10001110111010110101100011000110001",
    "N": "10001110011010110011100011000110001", "O": "01110100011000110001100011000101110",
    "P": "11110100011000111110100001000010000", "R": "11110100011000111110101001001010001",
    "S": "01111100001000001110000010000111110", "T": "11111001000010000100001000010000100",
    "U": "10001100011000110001100011000101110", "V": "10001100011000110001100010101000100",
    "Y": "10001100010101000100001000010000100", "0": "01110100011001110101110011000101110",
    "1": "00100011000010000100001000010001110", "2": "01110100010000100010001000100011111",
    "3": "11111000100010000010000011000101110", "4": "00010001100101010010111110001000010",
    "5": "11111100001111000001000011000101110", "6": "00110010001000011110100011000101110",
    "7": "11111000010001000100010000100001000", "8": "01110100011000101110100011000101110",
    "9": "01110100011000101111000010001001100", " ": "0" * 35,
    "H": "10001100011000111111100011000110001", "I": "01110001000010000100001000010001110",
    "K": "10001100101010011000101001001010001", "Q": "01110100011000110001101011001001101",
    "W": "10001100011000110101101011010101010", "X": "10001100010101000100010101000110001",
    "Z": "11111000010001000100010001000011111", "-": "00000000000000011111000000000000000",
}


def draw_text(img, iw, x, y, text, color, scale=1):
    for ch in text.upper():
        bits = FONT.get(ch, FONT[" "])
        for r in range(7):
            for c in range(5):
                if bits[r * 5 + c] == "1":
                    for dy in range(scale):
                        for dx in range(scale):
                            px, py = x + c * scale + dx, y + r * scale + dy
                            o = (py * iw + px) * 3
                            img[o:o + 3] = bytes(color)
        x += 6 * scale


def downscale(rgb, w, h, f):
    tw, th = w // f, h // f
    out = bytearray(tw * th * 3)
    try:
        from PIL import Image  # optional: nicer resampling when available
        im = Image.frombytes("RGB", (w, h), bytes(rgb)).resize((tw, th), Image.LANCZOS)
        return bytearray(im.tobytes()), tw, th
    except Exception:
        pass
    for y in range(th):
        for x in range(tw):
            acc = [0, 0, 0]
            for yy in range(f):
                row = ((y * f + yy) * w + x * f) * 3
                for xx in range(f):
                    o = row + xx * 3
                    acc[0] += rgb[o]; acc[1] += rgb[o + 1]; acc[2] += rgb[o + 2]
            n = f * f
            o = (y * tw + x) * 3
            out[o:o + 3] = bytes((acc[0] // n, acc[1] // n, acc[2] // n))
    return out, tw, th


def build_sheet(folder, out_path, year=None):
    """A year poster: one row per month, one column per day of the month."""
    files = sorted(glob.glob(os.path.join(folder, "*.png")))
    by_date = {}
    for p in files:
        m = re.match(r"(\d{4})-(\d{2})-(\d{2})", os.path.basename(p))
        if m and (year is None or int(m.group(1)) == year):
            by_date[(int(m.group(1)), int(m.group(2)), int(m.group(3)))] = p
    if not by_date:
        sys.exit("no YYYY-MM-DD*.png files in " + folder)
    year = year or min(by_date)[0]
    f = 4
    tw, th = W // f, H // f
    gap, left, top = 4, 34, 36
    iw = left + 31 * (tw + gap) + gap
    ih = top + 12 * (th + gap) + gap
    img = bytearray(bytes((16, 16, 18)) * (iw * ih))
    draw_text(img, iw, left, 7, "DAYPRINT %d" % year, (230, 230, 230), 2)
    for d in range(1, 32):
        draw_text(img, iw, left + (d - 1) * (tw + gap) + tw // 2 - 6, top - 10, "%2d" % d, (120, 120, 125))
    months = ["JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"]
    for mo in range(1, 13):
        y0 = top + (mo - 1) * (th + gap)
        draw_text(img, iw, 6, y0 + th // 2 - 3, months[mo - 1], (170, 170, 175))
        for d in range(1, 32):
            x0 = left + (d - 1) * (tw + gap)
            p = by_date.get((year, mo, d))
            if not p:
                try:
                    datetime.date(year, mo, d)
                    for yy in range(th):          # an empty slot for a real date
                        o = ((y0 + yy) * iw + x0) * 3
                        img[o:o + tw * 3] = bytes((28, 28, 32)) * tw
                except ValueError:
                    pass
                continue
            rgb, w, h = png_read(p)
            small, sw, sh = downscale(rgb, w, h, f)
            for yy in range(sh):
                o = ((y0 + yy) * iw + x0) * 3
                img[o:o + sw * 3] = small[yy * sw * 3:(yy + 1) * sw * 3]
    png_write(out_path, img, iw, ih)
    print("poster: %s (%d days)" % (out_path, len(by_date)))


# --------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description="Dayprint snapshots over USB serial")
    ap.add_argument("cmd", choices=["now", "day", "list", "year", "sheet", "hash"])
    ap.add_argument("arg", nargs="*")
    ap.add_argument("--port")
    ap.add_argument("--out", default="snaps")
    ap.add_argument("--plain", action="store_true", help="art without the date label")
    ap.add_argument("--from-file", help="parse a captured SNAP stream (testing)")
    a = ap.parse_args()

    if a.cmd == "sheet":
        folder = a.arg[0] if a.arg else a.out
        year = int(a.arg[1]) if len(a.arg) > 1 else None
        build_sheet(folder, os.path.join(folder, "poster-%s.png" % (year or "all")), year)
        return

    link = Link(a.port, a.from_file)
    os.makedirs(a.out, exist_ok=True)
    if a.cmd == "now":
        rgb, w, h, meta = snap_retry(link, "")
        path = os.path.join(a.out, "face-%s.png" % datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
        png_write(path, rgb, w, h)
        print("saved", path)
    elif a.cmd == "day":
        if not a.arg:
            sys.exit("usage: snap.py day YYYY-MM-DD")
        rgb, w, h, meta = snap_retry(link, a.arg[0] + (" PLAIN" if a.plain else ""))
        path = os.path.join(a.out, "%s%s.png" % (meta.get("day", a.arg[0]), "-plain" if a.plain else ""))
        png_write(path, rgb, w, h)
        print("saved %s (%s steps, %s)" % (path, meta.get("steps"), meta.get("family")))
    elif a.cmd == "list":
        for d in list_days(link):
            print("%s  %6d steps  %s  %s" % (d["date"], d["steps"], d["algo"], d["family"]))
    elif a.cmd == "year":
        year = int(a.arg[0]) if a.arg else datetime.date.today().year
        days = [d for d in list_days(link) if d["date"].startswith("%d-" % year)]
        folder = os.path.join(a.out, str(year))
        os.makedirs(folder, exist_ok=True)
        for i, d in enumerate(days):
            path = os.path.join(folder, d["date"] + ".png")
            if os.path.exists(path):
                continue
            rgb, w, h, _ = snap_retry(link, d["date"] + (" PLAIN" if a.plain else ""))
            png_write(path, rgb, w, h)
            print("[%d/%d] %s" % (i + 1, len(days), path))
        build_sheet(folder, os.path.join(folder, "poster-%d.png" % year), year)
    elif a.cmd == "hash":
        if len(a.arg) < 2:
            sys.exit("usage: snap.py hash YYYY-MM-DD STEPS")
        link.send("ARTHASH %s %s" % (a.arg[0], a.arg[1]))
        for _ in range(100):
            line = link.readline(timeout=40.0)
            if line is None or line.startswith("ARTHASH"):
                print(line)
                break


if __name__ == "__main__":
    main()
