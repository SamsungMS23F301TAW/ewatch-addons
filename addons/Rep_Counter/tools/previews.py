#!/usr/bin/env python3
"""Turn rep_preview's PPM frames into PNGs, a contact sheet, and framed
marketplace screenshots in docs/.

    tools/host/build.sh && build/host/rep_preview build/previews
    python3 tools/previews.py build/previews docs
"""
import sys
from pathlib import Path
from PIL import Image, ImageDraw

def frame(img, scale=2):
    """Put a 240x280 frame in a rounded watch-like bezel."""
    w, h = img.size
    big = img.resize((w * scale, h * scale), Image.NEAREST)
    pad = 18 * scale // 2
    W, H = big.width + 2 * pad, big.height + 2 * pad
    out = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(out)
    d.rounded_rectangle([0, 0, W - 1, H - 1], radius=46 * scale // 2, fill=(28, 30, 34, 255))
    d.rounded_rectangle([pad - 4, pad - 4, W - pad + 3, H - pad + 3], radius=34 * scale // 2,
                        fill=(8, 8, 10, 255))
    mask = Image.new('L', big.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, big.width - 1, big.height - 1],
                                           radius=30 * scale // 2, fill=255)
    out.paste(big, (pad, pad), mask)
    return out

def main():
    src = Path(sys.argv[1] if len(sys.argv) > 1 else 'build/previews')
    dst = Path(sys.argv[2] if len(sys.argv) > 2 else 'docs')
    dst.mkdir(parents=True, exist_ok=True)
    frames = sorted(src.glob('*.ppm'))
    pngs = []
    for f in frames:
        im = Image.open(f).convert('RGB')
        p = src / (f.stem + '.png')
        im.save(p)
        pngs.append((f.stem, im))
    # contact sheet (1x scale, labelled) for quick review
    cols = 4
    rows = (len(pngs) + cols - 1) // cols
    sheet = Image.new('RGB', (cols * 260, rows * 310), (40, 42, 46))
    d = ImageDraw.Draw(sheet)
    for i, (name, im) in enumerate(pngs):
        x, y = (i % cols) * 260 + 10, (i // cols) * 310 + 22
        sheet.paste(im, (x, y))
        d.text((x, y - 16), name, fill=(220, 220, 220))
    sheet.save(src / 'sheet.png')
    print(f'wrote {src / "sheet.png"}')
    # framed screenshots for docs/ (the marketplace set)
    for name in ['04_lifting', '07_set_done', '08_rest', '11_history', '12_settings', '01_idle']:
        for stem, im in pngs:
            if stem == name:
                frame(im).save(dst / f'screen_{name[3:]}.png')
                print(f'wrote {dst}/screen_{name[3:]}.png')

if __name__ == '__main__':
    main()
