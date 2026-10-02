#!/usr/bin/env python3
"""Assemble story_NNN.png frames (from `build.sh <dir> story`) into an
animated GIF at 2x, then delete the frames.
   python3 tools/preview/story.py <frames_dir> <out.gif>"""
import glob, os, sys
from PIL import Image

src, out = sys.argv[1], sys.argv[2]
paths = sorted(glob.glob(os.path.join(src, "story_*.png")))
frames = []
for p in paths:
    im = Image.open(p).convert("RGB")
    im = im.resize((im.width * 2, im.height * 2), Image.NEAREST)
    frames.append(im.quantize(colors=128, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE))
frames[0].save(out, save_all=True, append_images=frames[1:], duration=100, loop=0, optimize=True, disposal=1)
for p in paths:
    os.remove(p)
print(f"wrote {out} ({len(frames)} frames, {os.path.getsize(out)} bytes)")
