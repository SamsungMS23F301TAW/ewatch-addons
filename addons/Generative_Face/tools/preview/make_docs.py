#!/usr/bin/env python3
"""Regenerates the images in docs/ from the host build of the watch code.

    tools/preview/build.sh && python3 tools/preview/make_docs.py

Needs Pillow (host only). Every pixel of art and overlay comes from the same
C++ the watch runs; Pillow only lays the sheets out and adds labels.
"""
import datetime
import os
import random
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DOCS = os.path.join(ROOT, "docs")
PREVIEW = os.path.join(HERE, "preview")
TMP = tempfile.mkdtemp(prefix="dayprint-docs-")
BG = (17, 17, 20)
INK = (225, 225, 228)
DIM = (130, 130, 138)


def font(size, bold=False):
    for path in ("/System/Library/Fonts/Avenir Next.ttc", "/System/Library/Fonts/Helvetica.ttc"):
        try:
            return ImageFont.truetype(path, size, index=(5 if bold else 0) if "Avenir" in path else 0)
        except Exception:
            continue
    return ImageFont.load_default()


def run(*args):
    return subprocess.run([PREVIEW] + [str(a) for a in args], check=True,
                          capture_output=True, text=True).stdout


def art(date, steps):
    path = os.path.join(TMP, "a-%s-%d.png" % (date, steps))
    info = run("art", date, steps, path).split()
    return Image.open(path).convert("RGB"), info[1]


def face(date, steps, hhmm, mode=0):
    path = os.path.join(TMP, "f-%s-%d-%d.png" % (date, steps, mode))
    run("face", date, steps, hhmm, path, mode)
    return Image.open(path).convert("RGB")


def contact_sheet():
    days = [datetime.date(2026, 10, 5) + datetime.timedelta(days=i) for i in range(7)]
    steps = [0, 2500, 6000, 10000, 16000]
    s = 0.62
    cw, ch = int(240 * s), int(280 * s)
    gap, left, top = 10, 150, 64
    W = left + len(steps) * (cw + gap) + gap
    H = top + len(days) * (ch + gap) + gap
    sheet = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(sheet)
    d.text((16, 14), "Dayprint: one week x steps walked", fill=INK, font=font(22, True))
    for j, st in enumerate(steps):
        d.text((left + j * (cw + gap) + cw // 2, top - 14), "{:,} steps".format(st), fill=DIM,
               font=font(15), anchor="mm")
    for i, day in enumerate(days):
        y = top + i * (ch + gap)
        for j, st in enumerate(steps):
            im, fam = art(day.isoformat(), st)
            sheet.paste(im.resize((cw, ch), Image.LANCZOS), (left + j * (cw + gap), y))
        d.text((16, y + ch // 2 - 18), day.strftime("%a %d %b"), fill=INK, font=font(17, True))
        d.text((16, y + ch // 2 + 4), fam.split("/")[0].title() + " / " + fam.split("/")[1].title(),
               fill=DIM, font=font(14))
    sheet.save(os.path.join(DOCS, "contact-sheet.png"), optimize=True)


def faces_strip():
    days = [datetime.date(2026, 10, 5) + datetime.timedelta(days=i) for i in range(7)]
    times = ["07:12", "08:41", "10:05", "12:30", "15:47", "18:20", "21:58"]
    steps = [1800, 4200, 6100, 7400, 9300, 11800, 13200]
    gap = 14
    W = 7 * 240 + 8 * gap
    sheet = Image.new("RGB", (W, 280 + 2 * gap), BG)
    for i, day in enumerate(days):
        im = face(day.isoformat(), steps[i], times[i])
        sheet.paste(im, (gap + i * (240 + gap), gap))
        if i < 4:
            im.save(os.path.join(DOCS, "screenshot-%d.png" % (i + 1)), optimize=True)
    sheet.save(os.path.join(DOCS, "faces.png"), optimize=True)


def growth_strip():
    day = "2026-10-11"
    steps = [0, 1500, 4000, 7000, 11000, 16000, 26000]
    gap = 12
    W = len(steps) * 240 + (len(steps) + 1) * gap
    sheet = Image.new("RGB", (W, 280 + 2 * gap + 30), BG)
    d = ImageDraw.Draw(sheet)
    for i, st in enumerate(steps):
        im, _ = art(day, st)
        x = gap + i * (240 + gap)
        sheet.paste(im, (x, gap))
        d.text((x + 120, 280 + gap + 16), "{:,} steps".format(st), fill=DIM, font=font(16), anchor="mm")
    sheet.save(os.path.join(DOCS, "growth.png"), optimize=True)


def views_strip():
    tiles = [
        ("Art view (tap)", face("2026-10-07", 8600, "10:05", 1)),
        ("Gallery", face("2026-09-30", 11200, "00:00", 2)),
        ("Gallery: solstice", face("2026-12-21", 6400, "00:00", 2)),
        ("Weekly remix (hold)", None),
        ("Classic face", None),
    ]
    remix = os.path.join(TMP, "remix.png")
    run("remix", "2026-09-28", remix)
    tiles[3] = ("Weekly remix (hold)", Image.open(remix).convert("RGB"))
    tiles = tiles[:4]
    gap = 14
    W = len(tiles) * 240 + (len(tiles) + 1) * gap
    sheet = Image.new("RGB", (W, 280 + 2 * gap + 30), BG)
    d = ImageDraw.Draw(sheet)
    for i, (label, im) in enumerate(tiles):
        x = gap + i * (240 + gap)
        sheet.paste(im, (x, gap))
        d.text((x + 120, 280 + gap + 16), label, fill=DIM, font=font(16), anchor="mm")
    sheet.save(os.path.join(DOCS, "views.png"), optimize=True)


def year_poster():
    folder = os.path.join(TMP, "year")
    os.makedirs(folder, exist_ok=True)
    rng = random.Random(2026)
    day = datetime.date(2026, 1, 1)
    while day.year == 2026:
        if rng.random() < 0.93:
            steps = int(rng.triangular(1200, 16000, 7500))
            run("art", day.isoformat(), steps, os.path.join(folder, day.isoformat() + ".png"))
        day += datetime.timedelta(days=1)
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import snap  # noqa: E402
    png = os.path.join(TMP, "year-2026.png")
    snap.build_sheet(folder, png, 2026)
    # A JPEG keeps this showcase image light for the repository.
    Image.open(png).convert("RGB").save(os.path.join(DOCS, "year-2026.jpg"), quality=88,
                                        optimize=True, progressive=True)


def main():
    if not os.path.exists(PREVIEW):
        sys.exit("build the preview tool first: tools/preview/build.sh")
    os.makedirs(DOCS, exist_ok=True)
    contact_sheet()
    faces_strip()
    growth_strip()
    views_strip()
    year_poster()
    print("docs regenerated in", DOCS)


if __name__ == "__main__":
    main()
