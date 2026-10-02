#!/usr/bin/env python3
"""Convert the preview PPM frames to PNG.

For each NAME.ppm in the directory this writes:
  NAME.png        the raw 240x280 frame, pixel exact
  NAME@2x.png     a 2x nearest-neighbour upscale (easier to inspect)
  NAME_frame.png  the frame inside a simple watch-body mock-up (marketplace art)
and finally progression.png, a strip of the face states side by side.
"""
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter

SCREEN_RADIUS = 38          # visible-area corner radius assumed for the mock-up
S = 2                       # mock-up scale


def framed(img):
    w, h = img.size[0] * S, img.size[1] * S
    bezel = 26
    margin = 40
    bw, bh = w + 2 * bezel, h + 2 * bezel
    W, H = bw + 2 * margin, bh + 2 * margin
    out = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    # soft drop shadow
    sh = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(sh).rounded_rectangle((margin + 6, margin + 18, margin + bw - 6, margin + bh + 10),
                                         radius=104, fill=(0, 0, 0, 110))
    out.alpha_composite(sh.filter(ImageFilter.GaussianBlur(16)))
    d = ImageDraw.Draw(out)
    # side button (behind the body edge)
    d.rounded_rectangle((margin + bw - 8, margin + bh // 2 - 70, margin + bw + 10, margin + bh // 2 + 10),
                        radius=8, fill=(58, 62, 69, 255))
    # body + subtle rim
    d.rounded_rectangle((margin, margin, margin + bw, margin + bh), radius=104,
                        fill=(30, 33, 38, 255), outline=(78, 83, 90, 255), width=3)
    d.rounded_rectangle((margin + 10, margin + 10, margin + bw - 10, margin + bh - 10), radius=94,
                        fill=(8, 9, 11, 255))
    screen = img.resize((w, h), Image.LANCZOS).convert("RGBA")
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, w - 1, h - 1), radius=SCREEN_RADIUS * S, fill=255)
    out.paste(screen, (margin + bezel, margin + bezel), mask)
    return out


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    frames = {}
    for ppm in sorted(out.glob("*.ppm")):
        img = Image.open(ppm).convert("RGB")
        img.save(ppm.with_suffix(".png"))
        img.resize((img.size[0] * 2, img.size[1] * 2), Image.NEAREST).save(out / f"{ppm.stem}@2x.png")
        f = framed(img)
        f.save(out / f"{ppm.stem}_frame.png")
        frames[ppm.stem] = f
        ppm.unlink()
    order = ["face_far", "face_23min", "face_close", "face_final_minute", "face_meeting"]
    strip = [frames[k] for k in order if k in frames]
    if strip:
        fw, fh = strip[0].size
        scale = 0.6
        tw, th = int(fw * scale), int(fh * scale)
        sheet = Image.new("RGBA", (tw * len(strip) - 40 * (len(strip) - 1), th), (0, 0, 0, 0))
        for i, f in enumerate(strip):
            sheet.alpha_composite(f.resize((tw, th), Image.LANCZOS), (i * (tw - 40), 0))
        bg = Image.new("RGBA", sheet.size, (246, 247, 249, 255))
        bg.alpha_composite(sheet)
        bg.convert("RGB").save(out / "progression.png")
    print(f"wrote PNGs to {out}")


if __name__ == "__main__":
    main()
