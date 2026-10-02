#!/usr/bin/env python3
"""Render the EWatch Companion watch face on the host.

Builds tools/preview/preview.cpp together with the firmware's face renderer
(src/apps/face/*, src/core/face_slots.*, src/core/ble_proto.*) and the real
Arduino_GFX core from .pio/libdeps (run `pio run` once first so it exists),
renders every scenario in test/vectors/face_scenarios.json and writes:

  test/vectors/frames/<name>.png   exact 240x280 golden frames (used by the
                                   JavaScript preview pixel-diff test)
  docs/face-*.png, docs/faces.png  2x previews for the README / marketplace
  docs/halo-presets.png            the Halo face in all 12 theme presets (1x)
  .pio/preview/icons.png           every icon at sizes 2 and 3, zoomed 3x
  docs/screen-*.png                the Companion / Message screens (2x)

Only the Python standard library is used. Usage:
    python3 tools/preview/render.py
"""
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GFX = ROOT / ".pio" / "libdeps" / "ewatch" / "GFX Library for Arduino" / "src"
BUILD = ROOT / ".pio" / "preview"
FRAMES_RAW = BUILD / "frames"
GOLDEN = ROOT / "test" / "vectors" / "frames"
DOCS = ROOT / "docs"
W, H = 240, 280

# docs/face-<name>.png (underscores become dashes); the first six make faces.png.
DOC_SHOTS = ["halo", "halo_dashboard", "halo_paper_12h", "halo_p_ember", "halo_unicode", "halo_timers",
             "stock", "dashboard"]
DOC_NAMES = {"stock": "classic", "dashboard": "classic-dashboard", "halo_p_ember": "halo-ember"}
PRESET_SHOTS = ["halo_p_midnight", "halo_p_graphite", "halo_p_ocean", "halo_p_ember", "halo_p_forest",
                "halo_p_neon", "halo_p_amber", "halo_p_mint", "halo_p_paper", "halo_p_sakura",
                "halo_p_arctic", "halo_p_mono"]
SCREEN_SHOTS = ["companion_visible", "companion_pairing", "companion_connected",
                "companion_off", "companion_lockout", "companion_pairing_light",
                "companion_visible_ember", "companion_connected_ocean",
                "companion_connected_badtheme", "message", "message_sakura"]


def rgb565_to_rgb(data):
    out = bytearray(len(data) // 2 * 3)
    o = 0
    for i in range(0, len(data), 2):
        c = data[i] | (data[i + 1] << 8)
        r5, g6, b5 = (c >> 11) & 0x1F, (c >> 5) & 0x3F, c & 0x1F
        out[o] = (r5 << 3) | (r5 >> 2)
        out[o + 1] = (g6 << 2) | (g6 >> 4)
        out[o + 2] = (b5 << 3) | (b5 >> 2)
        o += 3
    return bytes(out)


def write_png(path, w, h, rgb):
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    path.write_bytes(png)


def scale(rgb, w, h, k):
    rows = []
    for y in range(h):
        row = bytearray()
        line = rgb[y * w * 3:(y + 1) * w * 3]
        for x in range(w):
            row += line[x * 3:x * 3 + 3] * k
        rows.extend([bytes(row)] * k)
    return b"".join(rows)


def rounded_watch(rgb, w, h, radius, bezel, bezel_rgb, page_rgb):
    """Put a frame inside a rounded 'watch glass' outline for presentation."""
    W2, H2 = w + 2 * bezel, h + 2 * bezel
    out = bytearray(page_rgb * (W2 * H2))
    R = radius + bezel
    for y in range(H2):
        for x in range(W2):
            # distance outside the rounded rectangle
            cx = min(max(x, R), W2 - 1 - R)
            cy = min(max(y, R), H2 - 1 - R)
            dx, dy = x - cx, y - cy
            if dx * dx + dy * dy > R * R:
                continue
            o = (y * W2 + x) * 3
            ix, iy = x - bezel, y - bezel
            inner = 0 <= ix < w and 0 <= iy < h
            if inner:
                icx = min(max(ix, radius), w - 1 - radius)
                icy = min(max(iy, radius), h - 1 - radius)
                ddx, ddy = ix - icx, iy - icy
                inner = ddx * ddx + ddy * ddy <= radius * radius
            if inner:
                i = (iy * w + ix) * 3
                out[o:o + 3] = rgb[i:i + 3]
            else:
                out[o:o + 3] = bezel_rgb
    return bytes(out), W2, H2


def main():
    if not (GFX / "Arduino_GFX.cpp").is_file():
        sys.exit("render.py: Arduino_GFX sources not found; run `pio run` once first.")
    BUILD.mkdir(parents=True, exist_ok=True)
    if FRAMES_RAW.exists():
        shutil.rmtree(FRAMES_RAW)
    FRAMES_RAW.mkdir(parents=True)
    obj = BUILD / "obj"
    obj.mkdir(exist_ok=True)

    inc = ["-I", str(ROOT / "tools/preview/shim"), "-I", str(GFX),
           "-I", str(ROOT / "src/core"), "-I", str(ROOT / "src/apps/face"),
           "-I", str(ROOT / "src/apps"), "-I", str(ROOT / "src/drivers"),
           "-I", str(ROOT / "src/apps/assets/fonts"), "-I", str(ROOT / "test/test_protocol")]
    lib_srcs = [GFX / "Arduino_GFX.cpp", GFX / "Arduino_G.cpp", GFX / "canvas/Arduino_Canvas.cpp",
                ROOT / "tools/preview/shim/host_time.cpp"]
    common = [ROOT / "src/apps/face/face_render.cpp", ROOT / "src/apps/face/face_icons.cpp",
              ROOT / "src/apps/face/face_halo.cpp", ROOT / "src/apps/face/aa_gfx.cpp",
              ROOT / "src/apps/face/aa_assets.cpp",
              ROOT / "src/core/face_slots.cpp", ROOT / "src/core/ble_proto.cpp"]
    face_only = [ROOT / "tools/preview/preview.cpp"]
    screens_only = [ROOT / "tools/preview/screens.cpp", ROOT / "src/apps/companion.cpp",
                    ROOT / "src/apps/companion_ui.cpp",
                    ROOT / "src/core/model.cpp"]
    objs = {}
    for src in lib_srcs + common + face_only + screens_only:
        o = obj / (src.stem + ".o")
        flags = ["-w"] if src in lib_srcs else ["-Wall", "-Wextra"]
        cmd = ["clang++", "-std=c++11", "-O1", "-c", str(src), "-o", str(o)] + flags + inc
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            sys.exit(f"compile failed: {src}\n{r.stderr}")
        if r.stderr.strip():
            print(r.stderr.strip())
        objs[src] = str(o)
    base = [objs[x] for x in lib_srcs + common]
    exe = BUILD / "preview"
    screens_exe = BUILD / "screens"
    for target, extra in ((exe, face_only), (screens_exe, screens_only)):
        r = subprocess.run(["clang++", "-o", str(target)] + base + [objs[x] for x in extra],
                           capture_output=True, text=True)
        if r.returncode:
            sys.exit(f"link failed: {target.name}\n{r.stderr}")

    r = subprocess.run([str(exe), str(ROOT / "test/vectors/face_scenarios.json"), str(FRAMES_RAW)],
                       capture_output=True, text=True)
    print(r.stdout.strip())
    if r.returncode:
        sys.exit(f"preview self-check failed (exit {r.returncode})\n{r.stderr}")

    r = subprocess.run([str(screens_exe), str(FRAMES_RAW)], capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"screens harness failed (exit {r.returncode})\n{r.stdout}\n{r.stderr}")

    GOLDEN.mkdir(parents=True, exist_ok=True)
    DOCS.mkdir(parents=True, exist_ok=True)
    rgb_by_name = {}
    for f in sorted(FRAMES_RAW.glob("*.rgb565")):
        rgb = rgb565_to_rgb(f.read_bytes())
        rgb_by_name[f.stem] = rgb
        if f.stem == "icons":
            write_png(BUILD / "icons.png", W * 3, H * 3, scale(rgb, W, H, 3))
            write_png(GOLDEN / "icons.png", W, H, rgb)
            continue
        if f.stem in SCREEN_SHOTS:
            write_png(DOCS / f"screen-{f.stem.replace('_', '-')}.png", W * 2, H * 2, scale(rgb, W, H, 2))
            continue
        write_png(GOLDEN / f"{f.stem}.png", W, H, rgb)

    # Presentation shots (2x, rounded glass) and a combined sheet.
    page = bytes((0x16, 0x18, 0x1D))
    bezel = bytes((0x30, 0x33, 0x3A))
    for old in DOCS.glob("face-*.png"):
        old.unlink()
    shots = []
    for name in DOC_SHOTS:
        big = scale(rgb_by_name[name], W, H, 2)
        framed, fw, fh = rounded_watch(big, W * 2, H * 2, 64, 10, bezel, page)
        write_png(DOCS / f"face-{DOC_NAMES.get(name, name.replace('_', '-'))}.png", fw, fh, framed)
        shots.append((framed, fw, fh))

    def sheet_of(images, cols, gap):
        fw, fh = images[0][1], images[0][2]
        rows = (len(images) + cols - 1) // cols
        sw, sh = cols * fw + (cols + 1) * gap, rows * fh + (rows + 1) * gap
        sheet = bytearray(page * (sw * sh))
        for idx, (img, _, _) in enumerate(images):
            ox = gap + (idx % cols) * (fw + gap)
            oy = gap + (idx // cols) * (fh + gap)
            for y in range(fh):
                o = ((oy + y) * sw + ox) * 3
                sheet[o:o + fw * 3] = img[y * fw * 3:(y + 1) * fw * 3]
        return sw, sh, bytes(sheet)

    sw, sh, sheet = sheet_of(shots[:6], 3, 24)
    write_png(DOCS / "faces.png", sw, sh, sheet)
    presets = [rounded_watch(rgb_by_name[n], W, H, 32, 6, bezel, page) for n in PRESET_SHOTS]
    sw, sh, sheet = sheet_of(presets, 6, 16)
    write_png(DOCS / "halo-presets.png", sw, sh, sheet)
    n_golden = len(rgb_by_name) - 1 - len([n for n in rgb_by_name if n in SCREEN_SHOTS])
    print(f"wrote {n_golden} golden frames to {GOLDEN.relative_to(ROOT)}, "
          f"{len(DOC_SHOTS)} face shots + faces.png + halo-presets.png + {len(SCREEN_SHOTS)} screen shots to docs/, "
          f"icons sheet to .pio/preview/icons.png")


if __name__ == "__main__":
    main()
