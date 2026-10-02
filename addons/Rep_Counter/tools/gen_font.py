#!/usr/bin/env python3
"""Generate Rep Counter's anti-aliased numeral fonts (src/apps/reps/rep_font.h).

The glyphs are an original geometric monoline design: round-capped strokes of
constant width, drawn at 8x and box-filtered down, stored as 4-bit coverage.
Digits are tabular (same advance) so a ticking counter never shifts sideways.

    python3 tools/gen_font.py            # writes src/apps/reps/rep_font.h
    python3 tools/gen_font.py --sheet x.png   # also writes a preview sheet
"""
import math
import sys
from pathlib import Path
from PIL import Image, ImageDraw

SS = 8                      # supersampling factor
W = 0.60                    # digit box width (fraction of height)
STROKE = 0.165              # stroke width (fraction of height)

def glyph_strokes(ch):
    """Return a list of ('line', [(x,y),...]) / ('arc', cx, cy, r, a0, a1) /
    ('dot', x, y, r) in unit coordinates (height 1, y down)."""
    s = STROKE / 2
    x0, x1 = s, W - s
    y0, y1 = s, 1 - s
    cx = W / 2
    r = (x1 - x0) / 2                 # bowl radius
    out = []
    if ch == '0':
        out.append(('arc', cx, y0 + r, r, 180, 360))
        out.append(('arc', cx, y1 - r, r, 0, 180))
        out.append(('line', [(x0, y0 + r), (x0, y1 - r)]))
        out.append(('line', [(x1, y0 + r), (x1, y1 - r)]))
    elif ch == '1':
        xm = cx + 0.05
        out.append(('line', [(xm - 0.20, y0 + 0.17), (xm, y0), (xm, y1)]))
    elif ch == '2':
        out.append(('arc', cx, y0 + r, r, 165, 360 + 25))
        a = math.radians(25)
        ex, ey = cx + r * math.cos(a), y0 + r + r * math.sin(a)
        out.append(('line', [(ex, ey), (x0, y1), (x1, y1)]))
    elif ch == '3':
        rb = r * 1.08
        bcy = y1 - rb
        out.append(('line', [(x0, y0), (x1 - 0.02, y0), (cx - 0.03, bcy - rb)]))
        out.append(('arc', cx - 0.01, bcy, rb, 270, 360 + 155))
    elif ch == '4':
        xs = x1 - 0.07
        yb = y0 + 0.63
        out.append(('line', [(xs, y1), (xs, y0), (x0, yb), (x1, yb)]))
    elif ch == '5':
        rb = r * 1.08
        bcy = y1 - rb
        xv = x0 + 0.03
        out.append(('line', [(x1, y0), (xv, y0), (xv, bcy - rb * 0.55)]))
        # bowl from its upper-left, over the top and round the right side
        out.append(('arc', cx - 0.01, bcy, rb, 215, 360 + 155))
    elif ch == '6':
        bcy = y1 - r
        out.append(('arc', cx, bcy, r, 0, 360))
        ang = math.radians(200)
        tx, ty = cx + r * math.cos(ang), bcy + r * math.sin(ang)
        out.append(('line', [(x1 - 0.05, y0), (tx, ty)]))
    elif ch == '7':
        out.append(('line', [(x0, y0), (x1, y0), (x0 + 0.13, y1)]))
    elif ch == '8':
        rt, rbt = r * 0.86, r * 1.0
        ct = y0 + rt
        cb = y1 - rbt
        out.append(('arc', cx, ct, rt, 0, 360))
        out.append(('arc', cx, cb, rbt, 0, 360))
    elif ch == '9':
        tcy = y0 + r
        out.append(('arc', cx, tcy, r, 0, 360))
        ang = math.radians(20)
        tx, ty = cx + r * math.cos(ang), tcy + r * math.sin(ang)
        out.append(('line', [(tx, ty), (x0 + 0.05, y1)]))
    elif ch == ':':
        out.append(('dot', 0.13, 0.36, STROKE * 0.62))
        out.append(('dot', 0.13, 0.80, STROKE * 0.62))
    elif ch == '.':
        out.append(('dot', 0.11, 1 - STROKE * 0.62, STROKE * 0.62))
    elif ch == '-':
        out.append(('line', [(x0 + 0.04, 0.55), (x1 - 0.04, 0.55)]))
    return out

ADV = {':': 0.26, '.': 0.22}

def render_glyph(ch, h):
    adv = ADV.get(ch, W)
    big_h = h * SS
    pad = int(STROKE * big_h)
    img = Image.new('L', (int(adv * big_h) + 2 * pad, big_h + 2 * pad), 0)
    d = ImageDraw.Draw(img)
    sw = STROKE * big_h
    def P(x, y):
        return (pad + x * big_h, pad + y * big_h)
    def cap(x, y):
        px, py = P(x, y)
        d.ellipse([px - sw / 2, py - sw / 2, px + sw / 2, py + sw / 2], fill=255)
    for st in glyph_strokes(ch):
        if st[0] == 'line':
            pts = [P(x, y) for x, y in st[1]]
            d.line(pts, fill=255, width=int(round(sw)), joint='curve')
            for x, y in st[1]:
                cap(x, y)
        elif st[0] == 'arc':
            _, cx, cy, r, a0, a1 = st
            px, py = P(cx, cy)
            rr = r * big_h
            box = [px - rr - sw / 2, py - rr - sw / 2, px + rr + sw / 2, py + rr + sw / 2]
            d.arc(box, a0, a1, fill=255, width=int(round(sw)))
            for a in (a0, a1):
                if (a1 - a0) % 360 != 0:
                    ar = math.radians(a)
                    cap(cx + r * math.cos(ar), cy + r * math.sin(ar))
        elif st[0] == 'dot':
            _, x, y, r = st
            px, py = P(x, y)
            rr = r * big_h
            d.ellipse([px - rr, py - rr, px + rr, py + rr], fill=255)
    small = img.resize((img.width // SS, img.height // SS), Image.BOX)
    return small, adv, pad // SS

def build(h, chars):
    glyphs = []
    for ch in chars:
        im, adv, pad = render_glyph(ch, h)
        bbox = im.getbbox() or (0, 0, 1, 1)
        crop = im.crop(bbox)
        x_off = bbox[0] - pad
        y_off = bbox[1] - pad          # from the top of the em box
        glyphs.append(dict(ch=ch, img=crop, w=crop.width, h=crop.height,
                           xoff=x_off, yoff=y_off, adv=int(round(adv * h))))
    return glyphs

def pack4(img):
    px = list(img.get_flattened_data()) if hasattr(img, 'get_flattened_data') else list(img.getdata())
    q = [min(15, (v * 15 + 127) // 255) for v in px]
    out = []
    for y in range(img.height):
        row = q[y * img.width:(y + 1) * img.width]
        if len(row) % 2:
            row.append(0)
        for i in range(0, len(row), 2):
            out.append((row[i] << 4) | row[i + 1])
    return out

FONTS = [
    ('kFontHero', 118, '0123456789'),      # rep count
    ('kFontTimer', 88, '0123456789:'),     # rest timer
    ('kFontLarge', 60, '0123456789:'),     # set-done card, long rests
    ('kFontMedium', 34, '0123456789:.-'),  # small numbers
]

def main():
    sheet = None
    if '--sheet' in sys.argv:
        sheet = sys.argv[sys.argv.index('--sheet') + 1]
    root = Path(__file__).resolve().parent.parent
    out = root / 'src/apps/reps/rep_font.h'
    lines = [
        '// Rep Counter numerals: original monoline design, 4-bit anti-aliased.',
        '// GENERATED by tools/gen_font.py - edit that script, not this file.',
        '#pragma once',
        '#include <stdint.h>',
        '#include "rep_gfx.h"',
        '',
        'namespace rgfx {',
        '',
    ]
    previews = []
    for name, h, chars in FONTS:
        gs = build(h, chars)
        data = []
        lines.append(f'static const AaGlyph {name}Glyphs[] = {{')
        for g in gs:
            off = len(data)
            data += pack4(g['img'])
            lines.append(f"  {{ {off}, {g['w']}, {g['h']}, {g['xoff']}, {g['yoff']}, {g['adv']} }},"
                         f"  // '{g['ch']}'")
        lines.append('};')
        lines.append(f'static const uint8_t {name}Data[] = {{')
        for i in range(0, len(data), 24):
            lines.append('  ' + ', '.join(f'0x{b:02X}' for b in data[i:i + 24]) + ',')
        lines.append('};')
        esc = chars.replace('\\', '\\\\')
        lines.append(f'static const AaFont {name} = {{ {name}Data, {name}Glyphs, "{esc}", {len(chars)}, {h} }};')
        lines.append(f'// {name}: {len(data)} bytes')
        lines.append('')
        previews.append((name, h, gs))
    lines.append('}  // namespace rgfx')
    out.write_text('\n'.join(lines) + '\n')
    print(f'wrote {out}')
    if sheet:
        Wd = 1000
        Ht = sum(h + 30 for _, h, _ in previews) + 20
        img = Image.new('RGB', (Wd, Ht), (12, 14, 18))
        y = 10
        for name, h, gs in previews:
            x = 10
            for g in gs:
                col = Image.new('RGB', g['img'].size, (240, 240, 240))
                img.paste(col, (x + g['xoff'], y + g['yoff']), g['img'])
                x += g['adv'] + 6
            y += h + 30
        img.save(sheet)
        print(f'wrote {sheet}')

if __name__ == '__main__':
    main()
