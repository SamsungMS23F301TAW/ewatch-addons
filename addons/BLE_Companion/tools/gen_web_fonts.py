#!/usr/bin/env python3
"""Embed the watch's font glyphs into web/index.html for the live preview.

The preview must look exactly like the watch, so it draws with the same
bitmaps the firmware uses:
  * the 6x8 "glcdfont" from GFX Library for Arduino (BSD licence, Adafruit),
  * the time glyphs ('-', '0'-'9', ':') of the four 24 pt FreeFonts used by
    the Sans / Bold / Serif / Mono face styles (GNU FreeFont, GPLv3 with the
    font-embedding exception; converted by Adafruit), from src/apps/assets/fonts.

The data replaces the block between the /* @@FONT-DATA:BEGIN */ and
/* @@FONT-DATA:END */ markers in web/index.html. Run after `pio run` (the GFX
library must be in .pio/libdeps):

    python3 tools/gen_web_fonts.py
"""
import base64
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GLCD = ROOT / ".pio/libdeps/ewatch/GFX Library for Arduino/src/font/glcdfont.h"
FONTS = ROOT / "src/apps/assets/fonts"
HTML = ROOT / "web/index.html"
BEGIN, END = "/* @@FONT-DATA:BEGIN */", "/* @@FONT-DATA:END */"
CHARS = [ord("-")] + list(range(ord("0"), ord(":") + 1))

FACE_FONTS = {          # face style index -> font file stem
    1: "FreeSans24pt7b",
    2: "FreeSansBold24pt7b",
    3: "FreeSerifBold24pt7b",
    4: "FreeMono24pt7b",
}


def b64(data: bytes) -> str:
    return base64.b64encode(bytes(data)).decode()


def glcd():
    src = GLCD.read_text()
    body = src[src.index("{") + 1:src.rindex("}")]
    vals = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", body)]
    if len(vals) != 1280:
        sys.exit(f"gen_web_fonts: unexpected glcdfont size {len(vals)}")
    return vals


def freefont(stem):
    src = (FONTS / f"{stem}.h").read_text()
    bm_src = src[src.index("Bitmaps[]"):]
    bm_src = bm_src[bm_src.index("{") + 1:bm_src.index("};")]
    bitmaps = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", bm_src)]
    gl_src = src[src.index("Glyphs[]"):]
    gl_src = gl_src[gl_src.index("{") + 1:gl_src.index("};")]
    glyphs = [tuple(int(v) for v in m.groups())
              for m in re.finditer(r"\{\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+)\s*\}", gl_src)]
    m = re.search(r"Glyphs,\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+),\s*(\d+)\s*\}", src)
    first, last, y_adv = int(m.group(1), 16), int(m.group(2), 16), int(m.group(3))
    if len(glyphs) != last - first + 1:
        sys.exit(f"gen_web_fonts: {stem}: glyph count mismatch")
    out = {}
    for c in CHARS:
        off, w, h, xa, xo, yo = glyphs[c - first]
        nbytes = (w * h + 7) // 8
        out[c] = [w, h, xa, xo, yo, b64(bitmaps[off:off + nbytes])]
    return {"first": first, "last": last, "yAdvance": y_adv, "glyphs": out}


def main():
    lines = ["const FONT_DATA = {",
             "  // 6x8 glcdfont, 256 glyphs x 5 column bytes (GFX Library for Arduino, BSD)",
             f"  glcd: '{b64(glcd())}',",
             "  // Time glyphs of the 24 pt FreeFonts (GNU FreeFont, GPLv3 + font exception)",
             "  // per char code: [width, height, xAdvance, xOffset, yOffset, bitmap]"]
    for style, stem in FACE_FONTS.items():
        f = freefont(stem)
        g = ", ".join(f"{c}: [{v[0]}, {v[1]}, {v[2]}, {v[3]}, {v[4]}, '{v[5]}']" for c, v in f["glyphs"].items())
        lines.append(f"  s{style}: {{ name: '{stem}', first: {f['first']}, last: {f['last']}, "
                     f"yAdvance: {f['yAdvance']}, glyphs: {{ {g} }} }},")
    lines.append("};")
    block = "\n".join(lines)

    html = HTML.read_text()
    a, b = html.find(BEGIN), html.find(END)
    if a < 0 or b < a:
        sys.exit("gen_web_fonts: markers not found in web/index.html")
    html = html[:a + len(BEGIN)] + "\n" + block + "\n" + html[b:]
    HTML.write_text(html)
    print(f"embedded font data ({len(block)} chars) into {HTML.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
