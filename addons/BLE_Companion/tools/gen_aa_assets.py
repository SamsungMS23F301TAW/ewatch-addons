#!/usr/bin/env python3
"""Generate the anti-aliased type and icon atlases for the v2 UI.

The watch (C++) and the web preview (JavaScript) must draw the new face pixel
for pixel alike, so neither rasterises fonts at run time: both blend the same
pre-computed 4-bit coverage masks with the same integer maths. This script
makes those masks once, offline, and writes them to

  src/apps/face/aa_assets.h / .cpp   C++ tables (const, so they stay in flash)
  web/index.html                     the block between /* @@AA-DATA:BEGIN */
                                     and /* @@AA-DATA:END */ (base64)
  .pio/preview/aa_assets.png         a contact sheet to eyeball the result

Type: Inter Display (c) The Inter Project Authors, SIL Open Font License 1.1,
https://rsms.me/inter . Glyphs are rendered 4x oversized with FreeType and box
filtered down, so edges carry true area coverage. Symbols Inter lacks (card
suits, smileys, box shades, a few dingbats that the CP437 slot text can hold)
and every icon are drawn here from simple geometry, 8x oversized.

Needs Pillow + numpy and the Inter Display TTFs (default: ~/Library/Fonts,
or pass --font-dir). Usage:

    python3 tools/gen_aa_assets.py [--font-dir DIR]
"""
import argparse
import base64
import math
import os
import re
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
OUT_H = ROOT / "src/apps/face/aa_assets.h"
OUT_CPP = ROOT / "src/apps/face/aa_assets.cpp"
HTML = ROOT / "web/index.html"
SHEET = ROOT / ".pio/preview/aa_assets.png"
BEGIN, END = "/* @@AA-DATA:BEGIN */", "/* @@AA-DATA:END */"

# ---------------------------------------------------------------------------
# Character sets. Glyph codes are the CP437 codes faceslots::utf8ToGlyphs()
# produces, so slot and message text needs no second mapping.
# ---------------------------------------------------------------------------
CP437 = {
    0x01: "☺", 0x02: "☻", 0x03: "♥", 0x04: "♦", 0x05: "♣",
    0x06: "♠", 0x07: "•", 0x09: "○", 0x0B: "♂", 0x0C: "♀",
    0x0E: "♫", 0x0F: "☼", 0x10: "►", 0x11: "◄", 0x12: "↕",
    0x14: "¶", 0x15: "§", 0x18: "↑", 0x19: "↓", 0x1A: "→",
    0x1B: "←", 0x1D: "↔", 0x1E: "▲", 0x1F: "▼", 0x7F: "⌂",
    0x80: "Ç", 0x81: "ü", 0x82: "é", 0x83: "â", 0x84: "ä", 0x85: "à", 0x86: "å",
    0x87: "ç", 0x88: "ê", 0x89: "ë", 0x8A: "è", 0x8B: "ï", 0x8C: "î", 0x8D: "ì",
    0x8E: "Ä", 0x8F: "Å", 0x90: "É", 0x91: "æ", 0x92: "Æ", 0x93: "ô", 0x94: "ö",
    0x95: "ò", 0x96: "û", 0x97: "ù", 0x98: "ÿ", 0x99: "Ö", 0x9A: "Ü", 0x9B: "¢",
    0x9C: "£", 0x9D: "¥", 0xA0: "á", 0xA1: "í", 0xA2: "ó", 0xA3: "ú", 0xA4: "ñ",
    0xA5: "Ñ", 0xA6: "ª", 0xA7: "º", 0xA8: "¿", 0xAA: "¬", 0xAB: "½", 0xAC: "¼",
    0xAD: "¡", 0xAE: "«", 0xAF: "»", 0xB0: "░", 0xB1: "▒", 0xB2: "▓",
    0xDB: "█", 0xE1: "ß", 0xE6: "µ", 0xEC: "∞", 0xF1: "±", 0xF2: "≥",
    0xF3: "≤", 0xF6: "÷", 0xF7: "≈", 0xF8: "°", 0xFA: "·", 0xFB: "√",
    0xFD: "²", 0xFE: "■",
}
ASCII = {c: chr(c) for c in range(0x20, 0x7F)}
FULL = {**ASCII, **CP437, 0x1C: "\u2026"}   # 0x1C: ellipsis used when fitting text
DIGITS = {c: chr(c) for c in b"0123456789"}

# name, weight, px size, charset, options
FONTS = [
    ("time",  "Regular",  76, {**DIGITS, ord(":"): ":", ord("-"): "-"}, {"raise_colon": True}),
    ("code",  "SemiBold", 40, {**DIGITS, ord(" "): " "}, {"tabular": True}),
    ("title", "SemiBold", 20, ASCII, {}),
    ("ui",    "SemiBold", 15, FULL, {}),
    ("small", "Medium",   13, {**ASCII, 0xFA: "\u00B7"}, {}),
    ("label", "SemiBold", 12, {**ASCII, 0xFA: "\u00B7"}, {}),
    ("msg",   "Medium",   19, FULL, {}),
]
SS = 4          # font oversampling
ISS = 8         # icon oversampling


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def quant4(a):
    """0..255 coverage -> 0..15, rounded."""
    return np.clip((a.astype(np.int32) * 15 + 127) // 255, 0, 15).astype(np.uint8)


def pack4(q):
    """2-D array of 0..15 -> bytes, row-major nibble stream, high nibble first."""
    flat = q.flatten().tolist()
    if len(flat) % 2:
        flat.append(0)
    return bytes((flat[i] << 4) | flat[i + 1] for i in range(0, len(flat), 2))


def downsample(img, k):
    a = np.asarray(img, dtype=np.float64)
    h, w = a.shape
    a = a[: h - h % k, : w - w % k]
    return a.reshape(h // k, k, w // k, k).mean(axis=(1, 3))


def crop(a):
    ys, xs = np.nonzero(a > 0)
    if len(xs) == 0:
        return a[0:0, 0:0], 0, 0
    return a[ys.min(): ys.max() + 1, xs.min(): xs.max() + 1], xs.min(), ys.min()


# ---------------------------------------------------------------------------
# Procedural glyphs for symbols Inter lacks. Drawn into an 'L' image `d` of
# the oversampled glyph canvas; (ox, base) is the pen origin, u = font em in
# oversampled px. Returns the advance in em.
# ---------------------------------------------------------------------------
def proc_glyph(ch, d, ox, base, u, cap):
    def P(x, y):                       # em units, y up from baseline
        return (ox + x * u, base - y * u)

    def ell(cx, cy, r, fill=255):
        d.ellipse([P(cx - r, cy + r), P(cx + r, cy - r)], fill=fill)

    def line(pts, w, fill=255):
        q = [P(*p) for p in pts]
        d.line(q, fill=fill, width=max(1, int(w * u)), joint="curve")
        for p in (q[0], q[-1]):
            r = w * u / 2
            d.ellipse([p[0] - r, p[1] - r, p[0] + r, p[1] + r], fill=fill)

    c = cap / u                        # cap height in em
    if ch in "░▒▓█":
        level = {"░": 64, "▒": 128, "▓": 192, "█": 255}[ch]
        d.rectangle([P(0.02, c + 0.16), P(0.58, -0.2)], fill=level)
        return 0.60
    if ch == "⌂":                 # house
        d.polygon([P(0.06, 0), P(0.06, c * 0.55), P(0.33, c * 0.95), P(0.60, c * 0.55), P(0.60, 0)], fill=255)
        d.polygon([P(0.14, 0.08 * c), P(0.14, c * 0.5), P(0.33, c * 0.8), P(0.52, c * 0.5), P(0.52, 0.08 * c)], fill=0)
        return 0.68
    if ch in "☺☻":           # smileys
        r = c * 0.52
        cx, cy = 0.08 + r, c * 0.5
        ell(cx, cy, r)
        inner = ch == "☺"
        if inner:
            ell(cx, cy, r * 0.82, 0)
        f = 255 if inner else 0
        ell(cx - r * 0.34, cy + r * 0.25, r * 0.13, f)
        ell(cx + r * 0.34, cy + r * 0.25, r * 0.13, f)
        d.arc([P(cx - r * 0.5, cy + r * 0.25), P(cx + r * 0.5, cy - r * 0.65)], 20, 160, fill=f,
              width=max(1, int(r * 0.16 * u)))
        return 0.16 + 2 * r
    if ch in "♀♂":
        r = c * 0.30
        w = c * 0.12
        if ch == "♀":
            cx, cy = 0.36, c * 0.66
            d.ellipse([P(cx - r, cy + r), P(cx + r, cy - r)], outline=255, width=int(w * u))
            line([(cx, cy - r), (cx, -0.12)], w)
            line([(cx - r * 0.7, 0.10), (cx + r * 0.7, 0.10)], w)
        else:
            cx, cy = 0.30, c * 0.36
            d.ellipse([P(cx - r, cy + r), P(cx + r, cy - r)], outline=255, width=int(w * u))
            a = (cx + r * 0.7, cy + r * 0.7)
            b = (cx + r * 1.9, cy + r * 1.9)
            line([a, b], w)
            line([b, (b[0] - r * 0.9, b[1])], w)
            line([b, (b[0], b[1] - r * 0.9)], w)
        return 0.74
    if ch == "♫":                 # beamed notes
        w = c * 0.10
        line([(0.22, 0.12), (0.22, c * 0.92)], w)
        line([(0.56, 0.22), (0.56, c * 1.02)], w)
        d.polygon([P(0.22, c * 0.92), P(0.56, c * 1.02), P(0.56, c * 0.84), P(0.22, c * 0.74)], fill=255)
        ell(0.14, 0.12, c * 0.14)
        ell(0.48, 0.22, c * 0.14)
        return 0.70
    # card suits
    cx, cy, r = 0.36, c * 0.5, c * 0.5
    if ch == "♦":
        d.polygon([P(cx, cy + r), P(cx + r * 0.72, cy), P(cx, cy - r), P(cx - r * 0.72, cy)], fill=255)
    elif ch == "♥":
        ell(cx - r * 0.42, cy + r * 0.30, r * 0.46)
        ell(cx + r * 0.42, cy + r * 0.30, r * 0.46)
        d.polygon([P(cx - r * 0.86, cy + r * 0.18), P(cx + r * 0.86, cy + r * 0.18), P(cx, cy - r)], fill=255)
    elif ch == "♠":
        ell(cx - r * 0.38, cy - r * 0.10, r * 0.40)
        ell(cx + r * 0.38, cy - r * 0.10, r * 0.40)
        d.polygon([P(cx - r * 0.76, cy - r * 0.05), P(cx + r * 0.76, cy - r * 0.05), P(cx, cy + r)], fill=255)
        d.polygon([P(cx, cy - r * 0.2), P(cx - r * 0.32, cy - r), P(cx + r * 0.32, cy - r)], fill=255)
    elif ch == "♣":
        ell(cx, cy + r * 0.48, r * 0.34)
        ell(cx - r * 0.44, cy - r * 0.06, r * 0.34)
        ell(cx + r * 0.44, cy - r * 0.06, r * 0.34)
        d.polygon([P(cx, cy + r * 0.1), P(cx - r * 0.30, cy - r), P(cx + r * 0.30, cy - r)], fill=255)
    else:
        return None
    return 0.72


# ---------------------------------------------------------------------------
# Fonts
# ---------------------------------------------------------------------------
def build_font(font_dir, name, weight, px, charset, opts):
    path = Path(font_dir) / f"InterDisplay-{weight}.ttf"
    if not path.is_file():
        sys.exit(f"gen_aa_assets: {path} not found (install Inter Display or pass --font-dir)")
    hi = ImageFont.truetype(str(path), px * SS)
    ascent, descent = hi.getmetrics()
    cap_box = hi.getbbox("H", anchor="ls")
    cap = -cap_box[1]
    pad = 4 * px * SS // 10 + 2 * SS
    cw = ch_ = (px * 3) * SS

    def render(chs, shift_x=0, shift_y=0):
        im = Image.new("L", (cw, ch_), 0)
        d = ImageDraw.Draw(im)
        base = pad + ascent
        adv = None
        if chs is not None:
            proc = None
            if missing(chs):
                proc = proc_glyph(chs, d, pad + shift_x, base + shift_y, px * SS, cap)
            if proc is not None:
                adv = proc * px
            else:
                d.text((pad + shift_x, base + shift_y), chs, font=hi, fill=255, anchor="ls")
                adv = hi.getlength(chs) / SS
        return im, adv

    probe = Image.new("L", (cw, ch_), 0)
    ImageDraw.Draw(probe).text((pad, pad + ascent), "͸", font=hi, fill=255, anchor="ls")
    notdef = np.asarray(probe)

    def missing(c):
        im = Image.new("L", (cw, ch_), 0)
        ImageDraw.Draw(im).text((pad, pad + ascent), c, font=hi, fill=255, anchor="ls")
        return c.strip() != "" and np.array_equal(np.asarray(im), notdef)

    codes = sorted(charset)
    advs = {c: render(charset[c])[1] for c in codes}
    cell = None
    if opts.get("tabular"):
        cell = max(advs[c] for c in codes if chr(c).isdigit())
    colon_dy = 0
    if opts.get("raise_colon"):
        zb = hi.getbbox("0", anchor="ls")
        cb = hi.getbbox(":", anchor="ls")
        colon_dy = round(((zb[1] + zb[3]) - (cb[1] + cb[3])) / 2)

    glyphs, bits, index = [], bytearray(), [0xFF] * 256
    for c in codes:
        chs = charset[c]
        adv = advs[c]
        sx = 0
        if cell is not None and chr(c).isdigit():
            sx = round((cell - adv) * SS / 2)
            adv = cell
        if cell is not None and chs == " ":
            adv = cell * 0.42
        elif chs == " ":
            adv = max(adv, px * 0.27)       # Inter Display's space is tight at text sizes
        sy = colon_dy if chs == ":" else 0
        im, _ = render(chs, sx, sy)
        a = downsample(im, SS)
        g, x0, y0 = crop(a)
        q = quant4(g)
        gw, gh = (q.shape[1], q.shape[0]) if q.size else (0, 0)
        dx = int(x0) - pad // SS if q.size else 0
        dy = int(y0) - (pad + ascent) // SS if q.size else 0
        index[c] = len(glyphs)
        glyphs.append((len(bits), gw, gh, dx, dy, int(round(adv))))
        bits += pack4(q) if q.size else b""
        assert gw < 256 and gh < 256 and -128 <= dx < 128 and -128 <= dy < 128
    fallback = index[ord("?")] if index[ord("?")] != 0xFF else 0
    cap_px = round(cap / SS)
    return {
        "name": name, "weight": weight, "px": px,
        "ascent": round(ascent / SS), "descent": round(descent / SS), "cap": cap_px,
        "glyphs": glyphs, "bits": bytes(bits), "index": index, "fallback": fallback,
    }


# ---------------------------------------------------------------------------
# Icons. Each draws into an oversampled square of side n = S*ISS with helper
# closures in unit coordinates (0..1, y down).
# ---------------------------------------------------------------------------
class Pen:
    def __init__(self, n):
        self.n = n
        self.im = Image.new("L", (n, n), 0)
        self.d = ImageDraw.Draw(self.im)

    def p(self, x, y):
        return (x * self.n, y * self.n)

    def disc(self, cx, cy, r, fill=255):
        n = self.n
        self.d.ellipse([(cx - r) * n, (cy - r) * n, (cx + r) * n, (cy + r) * n], fill=fill)

    def ring(self, cx, cy, r, w, fill=255):
        n = self.n
        self.d.ellipse([(cx - r) * n, (cy - r) * n, (cx + r) * n, (cy + r) * n], outline=fill, width=max(1, int(w * n)))

    def cap(self, pts, w, fill=255):
        q = [self.p(*pt) for pt in pts]
        self.d.line(q, fill=fill, width=max(1, int(round(w * self.n))), joint="curve")
        r = w * self.n / 2
        for x, y in (q[0], q[-1]):
            self.d.ellipse([x - r, y - r, x + r, y + r], fill=fill)

    def arc(self, cx, cy, r, a0, a1, w, fill=255, caps=True):
        """Arc in degrees, 0 = 3 o'clock, clockwise (PIL convention)."""
        n = self.n
        rr = r + w / 2
        self.d.arc([(cx - rr) * n, (cy - rr) * n, (cx + rr) * n, (cy + rr) * n], a0, a1,
                   fill=fill, width=max(1, int(round(w * n))))
        if caps:
            for a in (a0, a1):
                t = math.radians(a)
                self.disc(cx + r * math.cos(t), cy + r * math.sin(t), w / 2, fill)

    def poly(self, pts, fill=255):
        self.d.polygon([self.p(*pt) for pt in pts], fill=fill)

    def rrect(self, x0, y0, x1, y1, r, fill=255, outline=None, w=0):
        n = self.n
        if outline is not None:
            self.d.rounded_rectangle([x0 * n, y0 * n, x1 * n, y1 * n], radius=r * n, outline=outline,
                                     width=max(1, int(round(w * n))))
        else:
            self.d.rounded_rectangle([x0 * n, y0 * n, x1 * n, y1 * n], radius=r * n, fill=fill)


def cloud(pen, ox=0.0, oy=0.0, s=1.0, fill=255):
    def T(x, y):
        return ox + x * s, oy + y * s
    for cx, cy, r in ((0.34, 0.55, 0.19), (0.58, 0.45, 0.25)):
        x, y = T(cx, cy)
        pen.disc(x, y, r * s, fill)
    x0, y0 = T(0.10, 0.52)
    x1, y1 = T(0.92, 0.80)
    pen.rrect(x0, y0, x1, y1, 0.14 * s, fill)


def sun(pen, cx, cy, s, fill=255, rays=8):
    pen.disc(cx, cy, 0.19 * s, fill)
    for k in range(rays):
        t = 2 * math.pi * k / rays
        pen.cap([(cx + math.cos(t) * 0.33 * s, cy + math.sin(t) * 0.33 * s),
                 (cx + math.cos(t) * 0.44 * s, cy + math.sin(t) * 0.44 * s)], 0.085 * s, fill)


def icon_sun(p): sun(p, 0.5, 0.5, 1.0)


def icon_partly(p):
    sun(p, 0.36, 0.36, 0.78)
    cloud(p, 0.13, 0.22, 0.86, 0)          # gap around the cloud
    cloud(p, 0.17, 0.26, 0.80)


def icon_cloud(p): cloud(p, 0.0, 0.0, 1.0)


def icon_rain(p):
    cloud(p, 0.02, -0.14, 0.96)
    for x in (0.32, 0.52, 0.72):
        p.cap([(x, 0.74), (x - 0.07, 0.92)], 0.085)


def icon_storm(p):
    cloud(p, 0.02, -0.14, 0.96)
    bolt = [(0.56, 0.50), (0.36, 0.76), (0.50, 0.76), (0.42, 0.98), (0.68, 0.68), (0.53, 0.68), (0.62, 0.50)]
    grown = [(0.56, 0.42), (0.27, 0.81), (0.43, 0.81), (0.33, 1.06), (0.80, 0.63), (0.62, 0.63), (0.72, 0.42)]
    p.poly(grown, 0)
    p.poly(bolt)


def icon_snow(p):
    cloud(p, 0.02, -0.14, 0.96)
    for x, y in ((0.30, 0.80), (0.50, 0.90), (0.70, 0.80)):
        p.disc(x, y, 0.065)


def icon_fog(p):
    cloud(p, 0.02, -0.18, 0.96)
    p.cap([(0.14, 0.72), (0.70, 0.72)], 0.085)
    p.cap([(0.30, 0.88), (0.88, 0.88)], 0.085)


def icon_moon(p):
    p.disc(0.48, 0.52, 0.36)
    p.disc(0.66, 0.38, 0.30, 0)


def icon_calendar(p):
    p.rrect(0.12, 0.18, 0.88, 0.88, 0.14)
    p.rrect(0.20, 0.38, 0.80, 0.80, 0.06, 0)
    p.cap([(0.33, 0.08), (0.33, 0.24)], 0.09)
    p.cap([(0.67, 0.08), (0.67, 0.24)], 0.09)
    for i, (x, y) in enumerate(((0.33, 0.50), (0.50, 0.50), (0.67, 0.50), (0.33, 0.67), (0.50, 0.67))):
        p.rrect(x - 0.05, y - 0.05, x + 0.05, y + 0.05, 0.02)


def icon_bell(p):
    p.poly([(0.16, 0.72), (0.84, 0.72), (0.74, 0.56), (0.72, 0.40), (0.28, 0.40), (0.26, 0.56)])
    p.disc(0.5, 0.42, 0.22)
    p.rrect(0.12, 0.66, 0.88, 0.76, 0.05)
    p.disc(0.5, 0.84, 0.09)
    p.disc(0.5, 0.17, 0.06)


def icon_heart(p):
    p.disc(0.32, 0.38, 0.21)
    p.disc(0.68, 0.38, 0.21)
    p.poly([(0.12, 0.45), (0.88, 0.45), (0.5, 0.88)])
    p.poly([(0.30, 0.30), (0.70, 0.30), (0.86, 0.50), (0.5, 0.86), (0.14, 0.50)])


def icon_star(p):
    pts = []
    for k in range(10):
        r = 0.45 if k % 2 == 0 else 0.19
        t = -math.pi / 2 + k * math.pi / 5
        pts.append((0.5 + r * math.cos(t), 0.53 + r * math.sin(t)))
    p.poly(pts)


def icon_chat(p):
    p.rrect(0.08, 0.14, 0.92, 0.70, 0.18)
    p.poly([(0.24, 0.62), (0.46, 0.66), (0.20, 0.90)])


def icon_check(p):
    p.cap([(0.16, 0.52), (0.40, 0.76), (0.86, 0.26)], 0.13)


def icon_alert(p):
    p.poly([(0.5, 0.10), (0.94, 0.88), (0.06, 0.88)])
    p.d.rounded_rectangle([0.06 * p.n, 0.70 * p.n, 0.94 * p.n, 0.90 * p.n], radius=0.08 * p.n, fill=255)
    p.cap([(0.5, 0.40), (0.5, 0.60)], 0.10, 0)
    p.disc(0.5, 0.75, 0.055, 0)


def icon_bt(p):
    w = 0.085
    p.cap([(0.28, 0.30), (0.70, 0.66), (0.50, 0.84), (0.50, 0.16), (0.70, 0.34), (0.28, 0.70)], w)


def wifi_part(k):
    def draw(p):
        cx, cy = 0.5, 0.86
        if k == 0:
            p.disc(cx, cy - 0.02, 0.085)
        else:
            r = (0.22, 0.43, 0.64)[k - 1] * 0.92
            p.arc(cx, cy, r, 225, 315, 0.10)
    return draw


def icon_power(p):
    p.arc(0.5, 0.54, 0.32, -50, 230, 0.10)
    p.cap([(0.5, 0.10), (0.5, 0.46)], 0.10)


def icon_lock(p):
    p.arc(0.5, 0.42, 0.20, 180, 360, 0.10, caps=False)
    p.cap([(0.30, 0.42), (0.30, 0.50)], 0.10)
    p.cap([(0.70, 0.42), (0.70, 0.50)], 0.10)
    p.rrect(0.18, 0.46, 0.82, 0.92, 0.10)
    p.disc(0.5, 0.64, 0.07, 0)
    p.rrect(0.47, 0.64, 0.53, 0.80, 0.02, 0)


def icon_back(p):
    p.cap([(0.62, 0.18), (0.30, 0.50), (0.62, 0.82)], 0.13)


def icon_eye_off(p):
    p.d.ellipse([0.08 * p.n, 0.26 * p.n, 0.92 * p.n, 0.74 * p.n], outline=255, width=int(0.09 * p.n))
    p.disc(0.5, 0.5, 0.13)
    p.cap([(0.16, 0.16), (0.84, 0.84)], 0.18, 0)
    p.cap([(0.16, 0.16), (0.84, 0.84)], 0.085)


FEED_ICONS = [None, icon_sun, icon_partly, icon_cloud, icon_rain, icon_storm, icon_snow, icon_fog,
              icon_moon, icon_calendar, icon_bell, icon_heart, icon_star, icon_chat, icon_check, icon_alert]
FEED_NAMES = [None, "SUN", "PARTLY", "CLOUD", "RAIN", "STORM", "SNOW", "FOG", "MOON", "CALENDAR",
              "BELL", "HEART", "STAR", "CHAT", "CHECK", "ALERT"]

# (enum name, draw fn, size px)
MASKS = []
for size in (16, 34):
    for i in range(1, 16):
        MASKS.append((f"{FEED_NAMES[i]}_{size}", FEED_ICONS[i], size))
MASKS += [
    ("BT_16", icon_bt, 16), ("BT_44", icon_bt, 44),
    ("WIFI0_22", wifi_part(0), 22), ("WIFI1_22", wifi_part(1), 22),
    ("WIFI2_22", wifi_part(2), 22), ("WIFI3_22", wifi_part(3), 22),
    ("POWER_20", icon_power, 20), ("LOCK_36", icon_lock, 36), ("BACK_18", icon_back, 18),
    ("CHECK_40", icon_check, 40), ("EYEOFF_36", icon_eye_off, 36),
]


def build_mask(fn, size):
    pen = Pen(size * ISS)
    fn(pen)
    a = downsample(pen.im, ISS)
    q = quant4(a)
    return {"w": size, "h": size, "bits": pack4(q)}


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------
def c_bytes(data, indent="  "):
    out, line = [], indent
    for i, b in enumerate(data):
        line += f"0x{b:02x},"
        if (i + 1) % 20 == 0:
            out.append(line)
            line = indent
    if line.strip():
        out.append(line)
    return "\n".join(out)


def write_cpp(fonts, masks):
    h = [
        "// GENERATED by tools/gen_aa_assets.py - do not edit.",
        "// Anti-aliased glyph and icon coverage masks (4 bit) for the v2 UI.",
        "// Type: Inter Display (c) The Inter Project Authors, SIL OFL 1.1.",
        "#pragma once",
        '#include "aa_gfx.h"',
        "",
        "namespace aa_assets {",
        "",
    ]
    for f in fonts:
        h.append(f"extern const aa::Font kFont{f['name'].capitalize()};   // Inter Display {f['weight']} {f['px']} px")
    h += ["", "enum MaskId : uint8_t {"]
    for i, (name, _, _) in enumerate(MASKS):
        h.append(f"  M_{name} = {i},")
    h += [f"  M_COUNT = {len(MASKS)}", "};", "extern const aa::Mask kMasks[M_COUNT];", "",
          "}  // namespace aa_assets", ""]
    OUT_H.write_text("\n".join(h))

    c = ["// GENERATED by tools/gen_aa_assets.py - do not edit.", '#include "aa_assets.h"', "",
         "namespace aa_assets {", ""]
    for f in fonts:
        n = f["name"].capitalize()
        c.append(f"static const uint8_t k{n}Bits[] = {{\n{c_bytes(f['bits'])}\n}};")
        c.append(f"static const aa::Glyph k{n}Glyphs[] = {{")
        for g in f["glyphs"]:
            c.append(f"  {{ {g[0]}, {g[1]}, {g[2]}, {g[3]}, {g[4]}, {g[5]} }},")
        c.append("};")
        c.append(f"static const uint8_t k{n}Index[256] = {{\n{c_bytes(bytes(f['index']))}\n}};")
        c.append(f"const aa::Font kFont{n} = {{ k{n}Bits, k{n}Glyphs, k{n}Index, {f['ascent']}, "
                 f"{f['descent']}, {f['cap']}, {f['fallback']} }};")
        c.append("")
    for i, m in enumerate(masks):
        c.append(f"static const uint8_t kMask{i}[] = {{\n{c_bytes(m['bits'])}\n}};")
    c.append("const aa::Mask kMasks[M_COUNT] = {")
    for i, m in enumerate(masks):
        c.append(f"  {{ kMask{i}, {m['w']}, {m['h']} }},   // {MASKS[i][0]}")
    c += ["};", "", "}  // namespace aa_assets", ""]
    OUT_CPP.write_text("\n".join(c))


def b64(data):
    return base64.b64encode(bytes(data)).decode()


def write_js(fonts, masks):
    lines = [BEGIN, "const AA_DATA = {", "  fonts: {"]
    for f in fonts:
        flat = ",".join(str(v) for g in f["glyphs"] for v in g)
        lines.append(f"    {f['name']}: {{ ascent: {f['ascent']}, descent: {f['descent']}, cap: {f['cap']}, "
                     f"fallback: {f['fallback']}, index: '{b64(f['index'])}', glyphs: [{flat}], bits: '{b64(f['bits'])}' }},")
    lines.append("  },")
    lines.append("  maskNames: [" + ",".join(f"'{m[0]}'" for m in MASKS) + "],")
    lines.append("  masks: [")
    for m in masks:
        lines.append(f"    [{m['w']}, {m['h']}, '{b64(m['bits'])}'],")
    lines += ["  ],", "};", END]
    html = HTML.read_text()
    a, b = html.find(BEGIN), html.find(END)
    if a < 0 or b < a:
        sys.exit("gen_aa_assets: AA-DATA markers not found in web/index.html")
    HTML.write_text(html[:a] + "\n".join(lines) + html[b + len(END):])


def unpack4(bits, w, h):
    n = w * h
    out = np.zeros(n, dtype=np.uint8)
    for i in range(n):
        b = bits[i >> 1]
        out[i] = (b >> 4) if (i & 1) == 0 else (b & 15)
    return out.reshape(h, w)


def write_sheet(fonts, masks):
    W = 960
    rows = []
    for f in fonts:
        codes = [c for c in range(256) if f["index"][c] != 0xFF]
        line_h = f["ascent"] + f["descent"] + 6
        x, y = 8, f["ascent"] + 4
        canvas = np.zeros((line_h * 8, W), dtype=np.uint8)
        for c in codes:
            off, gw, gh, dx, dy, adv = f["glyphs"][f["index"][c]]
            if x + adv + 8 > W:
                x = 8
                y += line_h
            if gw and gh:
                g = unpack4(f["bits"][off:], gw, gh) * 17
                y0, x0 = y + dy, x + dx
                canvas[y0:y0 + gh, x0:x0 + gw] = np.maximum(canvas[y0:y0 + gh, x0:x0 + gw], g)
            x += adv + 2
        used = y + f["descent"] + 4
        rows.append(canvas[:used])
    x, y, mh = 8, 8, 0
    mcanvas = np.zeros((200, W), dtype=np.uint8)
    for m in masks:
        if x + m["w"] + 8 > W:
            x = 8
            y += mh + 8
            mh = 0
        g = unpack4(m["bits"], m["w"], m["h"]) * 17
        mcanvas[y:y + m["h"], x:x + m["w"]] = g
        x += m["w"] + 8
        mh = max(mh, m["h"])
    rows.append(mcanvas[:y + mh + 8])
    sheet = np.vstack(rows)
    SHEET.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(sheet).resize((sheet.shape[1] * 2, sheet.shape[0] * 2), Image.NEAREST).save(SHEET)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--font-dir", default=os.path.expanduser("~/Library/Fonts"))
    args = ap.parse_args()
    fonts = [build_font(args.font_dir, *spec) for spec in FONTS]
    masks = [build_mask(fn, size) for _, fn, size in MASKS]
    write_cpp(fonts, masks)
    write_js(fonts, masks)
    write_sheet(fonts, masks)
    total = sum(len(f["bits"]) + 7 * len(f["glyphs"]) + 256 for f in fonts) + sum(len(m["bits"]) for m in masks)
    for f in fonts:
        print(f"  {f['name']:<6} {f['weight']:<8} {f['px']:>3}px  {len(f['glyphs']):>3} glyphs  "
              f"{len(f['bits']):>6} B  ascent {f['ascent']} cap {f['cap']}")
    print(f"  {len(masks)} masks, {sum(len(m['bits']) for m in masks)} B; total ~{total} B")
    print(f"wrote {OUT_H.relative_to(ROOT)}, {OUT_CPP.relative_to(ROOT)}, web/index.html AA-DATA, "
          f"{SHEET.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
