#!/usr/bin/env python3
"""Compose the README filmstrip and animation from oracle_preview frames.

Usage: compose_docs.py <frames_dir> <out_dir>

Reads <frames_dir>/classic_NNNN.png and classic_frames.csv (written by
`oracle_preview <dir> --frames`) and writes:
  <out_dir>/sequence.png  idle -> churning -> rising -> landed -> settled
  <out_dir>/oracle.gif    the classic run at 25 fps, from prompt to answer
"""
import csv
import sys
from pathlib import Path

from PIL import Image, ImageDraw


def load(frames_dir, name):
    rows = list(csv.reader(open(frames_dir / f"{name}_frames.csv")))
    return [(int(r[0]), int(r[1]), int(r[2])) for r in rows]


def frame_at(frames_dir, name, rows, t_ms):
    best = rows[0]
    for r in rows:
        if r[1] <= t_ms:
            best = r
    return Image.open(frames_dir / f"{name}_{best[0]:04d}.png").convert("RGB")


def main():
    frames_dir, out_dir = Path(sys.argv[1]), Path(sys.argv[2])
    rows = load(frames_dir, "classic")
    # When the answer started rising (phase 2), and when it landed.
    rise = next(r[1] for r in rows if r[2] == 2)

    # Filmstrip with captions.
    shots = [(1500, "Ask"), (2700, "Shake"), (rise + 230, "Rising"),
             (rise + 760, "Thunk"), (rise + 2600, "Answer")]
    w, h = 240, 280
    pad, cap = 10, 22
    strip = Image.new("RGB", (len(shots) * (w + pad) - pad, h + cap), (0, 0, 0))
    draw = ImageDraw.Draw(strip)
    for i, (t, label) in enumerate(shots):
        strip.paste(frame_at(frames_dir, "classic", rows, t), (i * (w + pad), 0))
        tw = draw.textlength(label)
        draw.text((i * (w + pad) + (w - tw) / 2, h + 5), label, fill=(170, 180, 200))
    strip.save(out_dir / "sequence.png", optimize=True)

    # Animation: prompt, shake, rise, settle; then hold the answer.
    start, end = 1200, rise + 2400
    frames = [frame_at(frames_dir, "classic", rows, t) for t in range(start, end, 40)]
    frames += [frames[-1]] * 25
    pal = [f.quantize(colors=128, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
           for f in frames]
    pal[0].save(out_dir / "oracle.gif", save_all=True, append_images=pal[1:],
                duration=40, loop=0, optimize=True, disposal=1)
    print(f"wrote {out_dir / 'sequence.png'} and {out_dir / 'oracle.gif'} ({len(frames)} frames)")


if __name__ == "__main__":
    main()
