#!/usr/bin/env python3
"""Plot what the detector saw, from `rep_replay --trace`.

    build/host/rep_replay rec.csv --trace /tmp/trace.csv
    python3 tools/plot_trace.py /tmp/trace.csv out.png [t0 t1]

Panels: raw acceleration (g), rotation channel (angle from home, deg),
linear channel (excursion from home, m), and the live rep count. Shaded bands
show where the locomotion gate was active. Needs Pillow (no matplotlib).
"""
import csv
import sys
from PIL import Image, ImageDraw, ImageFont

BG = (250, 250, 250)
GRID = (226, 228, 232)
AXIS = (120, 124, 130)
TEXT = (40, 44, 52)
COLS = {'ax': (214, 69, 65), 'ay': (46, 160, 67), 'az': (52, 101, 214),
        'theta': (230, 126, 34), 'linExc': (142, 68, 173), 'reps': (20, 20, 20)}


def font(size):
    for name in ('/System/Library/Fonts/SFNS.ttf', '/System/Library/Fonts/Helvetica.ttc',
                 '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    rows = list(csv.DictReader(open(sys.argv[1])))
    t = [float(r['t']) for r in rows]
    t0, t1 = (float(sys.argv[3]), float(sys.argv[4])) if len(sys.argv) >= 5 else (t[0], t[-1])
    idx = [i for i, x in enumerate(t) if t0 <= x <= t1]
    if len(idx) > 4000:                       # thin long traces for drawing
        step = len(idx) // 4000 + 1
        idx = idx[::step]
    panels = [('acceleration (g)', ['ax', 'ay', 'az']),
              ('rotation channel: angle from home (deg)', ['theta']),
              ('linear channel: excursion from home (m)', ['linExc']),
              ('reps in current set', ['reps'])]
    W, PH, L, R = 1400, 170, 70, 20
    H = PH * len(panels) + 40
    img = Image.new('RGB', (W, H), BG)
    d = ImageDraw.Draw(img)
    f, fs = font(15), font(13)

    def X(x):
        return L + (W - L - R) * (x - t0) / max(1e-9, t1 - t0)

    # gate shading
    gated = [i for i in idx if rows[i].get('gated') == '1']
    for i in gated:
        d.line([X(t[i]), 10, X(t[i]), PH * len(panels) + 10], fill=(255, 238, 220))
    for p, (title, cols) in enumerate(panels):
        y0, y1 = p * PH + 22, (p + 1) * PH - 4
        vals = [float(rows[i][c]) for c in cols for i in idx]
        lo, hi = min(vals), max(vals)
        if hi - lo < 1e-6:
            hi = lo + 1
        pad = (hi - lo) * 0.08
        lo, hi = lo - pad, hi + pad

        def Y(v):
            return y1 - (y1 - y0) * (v - lo) / (hi - lo)

        d.rectangle([L, y0, W - R, y1], outline=GRID)
        if lo < 0 < hi:
            d.line([L, Y(0), W - R, Y(0)], fill=GRID)
        d.text((L + 6, y0 - 18), title, fill=TEXT, font=f)
        d.text((6, y0), f'{hi:.2f}', fill=AXIS, font=fs)
        d.text((6, y1 - 14), f'{lo:.2f}', fill=AXIS, font=fs)
        for c in cols:
            pts = [(X(t[i]), Y(float(rows[i][c]))) for i in idx]
            d.line(pts, fill=COLS.get(c, TEXT), width=2)
            if len(cols) > 1:
                d.text((W - R - 36 * (len(cols) - cols.index(c)), y0 - 18), c, fill=COLS[c], font=fs)
    # time axis
    step = 5 if t1 - t0 > 20 else 1
    s = int(t0 // step + 1) * step
    while s <= t1:
        d.line([X(s), PH * len(panels), X(s), PH * len(panels) + 6], fill=AXIS)
        d.text((X(s) - 8, PH * len(panels) + 8), f'{s}s', fill=AXIS, font=fs)
        s += step
    img.save(sys.argv[2])
    print(f'wrote {sys.argv[2]}')


if __name__ == '__main__':
    main()
