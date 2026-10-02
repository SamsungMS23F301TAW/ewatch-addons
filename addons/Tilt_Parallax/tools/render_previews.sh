#!/bin/sh
# Builds the host preview tool from the real engine sources and renders the
# marketplace screenshots into docs/. Needs clang++, zlib (macOS has both) and
# python3 with Pillow for the contact sheets and GIFs. Run from anywhere:
#   tools/render_previews.sh            # build + render docs/
#   tools/render_previews.sh build      # just build .pio/preview/preview
set -e
cd "$(dirname "$0")/.."
ENGINE=src/apps/parallax/engine
BIN=.pio/preview
mkdir -p "$BIN"
clang++ -std=gnu++11 -O2 -Wall -Wextra -Wno-unused-parameter \
  -I "$ENGINE" -I tools/host_shim -I src/apps/assets/fonts \
  "$ENGINE"/*.cpp tools/preview/preview.cpp -lz -o "$BIN/preview"
[ "$1" = "build" ] && exit 0

P="$BIN/preview"
TMP=.pio/preview/frames
rm -rf "$TMP"
mkdir -p docs "$TMP"

# Hero shots, 2x: each scene at a flattering moment, wrist slightly tilted.
"$P" frame docs/alpine-morning.png --scene 0 --time 07:40 --date 2026-09-20 --tilt 0.3,-0.1
"$P" frame docs/coast-sunset.png   --scene 1 --time 20:05 --date 2026-07-14 --tilt -0.2,0.0
"$P" frame docs/city-night.png     --scene 2 --time 22:47 --date 2026-10-01 --tilt 0.25,0.1
"$P" frame docs/desert-afternoon.png --scene 3 --time 16:10 --date 2026-04-18 --tilt -0.35,0.0

# Contact sheets, 1x (true watch pixels).
for s in 0 1 2 3; do
  "$P" frame "$TMP/scene$s.png" --scene $s --time 10:10 --date 2026-07-14 --scale 1
done
"$P" sheet docs/day-cycle.png --scene 0 --date 2026-06-21 \
  --times "05:05,09:30,20:55,23:40" --scale 1
"$P" sheet docs/parallax-tilt.png --scene 0 --time 17:30 --date 2026-09-20 \
  --tilts "-1,0;0,0;1,0" --scale 1

# Animations: a figure-eight wrist tilt, and a scene change.
mkdir -p "$TMP/sweep" "$TMP/change"
"$P" sweep "$TMP/sweep" --scene 1 --time 19:40 --date 2026-06-12 --frames 32
"$P" transition "$TMP/change" --scene 0 --to 1 --time 18:40 --date 2026-09-20

python3 - "$TMP" <<'EOF'
import glob, sys
from PIL import Image
tmp = sys.argv[1]

def sheet(paths, out, cols, pad=6, bg=(18, 18, 18)):
    ims = [Image.open(p).convert("RGB") for p in paths]
    w, h = ims[0].size
    rows = (len(ims) + cols - 1) // cols
    canvas = Image.new("RGB", (cols * (w + pad) + pad, rows * (h + pad) + pad), bg)
    for i, im in enumerate(ims):
        canvas.paste(im, (pad + (i % cols) * (w + pad), pad + (i // cols) * (h + pad)))
    canvas.save(out)

def gif(frames, out, ms):
    ims = [Image.open(p).convert("RGB") for p in frames]
    pal = [im.quantize(colors=96, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
           for im in ims]
    pal[0].save(out, save_all=True, append_images=pal[1:], duration=ms, loop=0,
                optimize=True, disposal=1)

sheet([f"{tmp}/scene{i}.png" for i in range(4)], "docs/scenes.png", 4)
change = sorted(glob.glob(f"{tmp}/change/f*.png"))
sheet(change[4::4][:10], "docs/scene-change.png", 5)
gif(sorted(glob.glob(f"{tmp}/sweep/f*.png")), "docs/tilt.gif", 70)
gif(change, "docs/scene-change.gif", 33)
EOF
rm -rf "$TMP"
echo "previews written to docs/"
