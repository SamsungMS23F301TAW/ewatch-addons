#!/usr/bin/env python3
"""Generate test/vectors/protocol_vectors.json for the EWatch Companion.

The vectors are the contract between the firmware codec (src/core/ble_proto.*,
src/core/face_slots.*) and the web page's JavaScript codec (web/index.html).
Both test suites load the same JSON:

    pio test -e native                 # C++ (test/test_protocol)
    node --test test/web/              # JavaScript

Byte layouts and civil-time conversions are computed HERE, independently, with
`struct` and `datetime`, so this script is a third implementation of the
wire format. Display formatting (glyph mapping, countdown text, slot layout)
is written out by hand below as the expected behaviour.

Usage: python3 tools/gen_vectors.py   (rewrites the JSON in place)
"""
import datetime as dt
import json
import struct
from pathlib import Path

OUT = Path(__file__).resolve().parent.parent / "test" / "vectors" / "protocol_vectors.json"

# Result codes (ble_proto.h)
RES_OK, RES_NOT_AUTH, RES_BAD_LENGTH, RES_BAD_VALUE, RES_BUSY, RES_UNSUPPORTED = range(6)


def hx(b: bytes) -> str:
    return b.hex()


# ---------------------------------------------------------------------------
# Colours
# ---------------------------------------------------------------------------
def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def rgb888(c):
    r5, g6, b5 = (c >> 11) & 0x1F, (c >> 5) & 0x3F, c & 0x1F
    return ((r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2))


def rgb_vectors():
    samples = ["#000000", "#ffffff", "#ff0000", "#00ff00", "#0000ff", "#ff8000",
               "#070307", "#080408", "#123456", "#e86b2b", "#7f7f7f", "#808080",
               "#f7f3f7", "#a1b2c3", "#00000f", "#0f0f0f"]
    out = []
    for s in samples:
        r, g, b = int(s[1:3], 16), int(s[3:5], 16), int(s[5:7], 16)
        c = rgb565(r, g, b)
        back = "#%02x%02x%02x" % rgb888(c)
        out.append({"rgb": s, "rgb565": c, "back": back})
    # Every RGB565 value must survive rgb565 -> rgb888 -> rgb565 unchanged.
    for c in (0x0000, 0xFFFF, 0x000F, 0x7BEF, 0xF800, 0x07E0, 0x001F, 0xEB45, 0x8410, 0x0841):
        back = "#%02x%02x%02x" % rgb888(c)
        r, g, b = rgb888(c)
        assert rgb565(r, g, b) == c
        out.append({"rgb": back, "rgb565": c, "back": back})
    return out


# ---------------------------------------------------------------------------
# Characteristics
# ---------------------------------------------------------------------------
def theme_vectors():
    out = []
    for t in [(0x0000, 0xFFFF, 0x000F, 0x7BEF), (0x18E3, 0xFFDF, 0xEB45, 0x4208),
              (0xFFFF, 0x0000, 0x001F, 0xC618)]:
        out.append({"bg": t[0], "fg": t[1], "accent": t[2], "line": t[3],
                    "hex": hx(struct.pack("<4H", *t))})
    errors = [{"hex": "0000ffff0f00ef", "error": RES_BAD_LENGTH},
              {"hex": "0000ffff0f00ef7b00", "error": RES_BAD_LENGTH},
              {"hex": "", "error": RES_BAD_LENGTH}]
    return out + errors


def brightness_vectors():
    return [{"hex": "c8", "value": 200}, {"hex": "ff", "value": 255},
            {"hex": "10", "value": 16}, {"hex": "0f", "value": 16},
            {"hex": "00", "value": 16}, {"hex": "", "error": RES_BAD_LENGTH},
            {"hex": "c8c8", "error": RES_BAD_LENGTH}]


def face_vectors():
    return [{"hex": "0000", "styleCount": 8, "style": 0, "options": 0},
            {"hex": "0501", "styleCount": 8, "style": 5, "options": 1},
            {"hex": "07ff", "styleCount": 8, "style": 7, "options": 1},   # unknown bits dropped
            {"hex": "0800", "styleCount": 8, "error": RES_BAD_VALUE},   # v1.0 firmware: no Halo
            {"hex": "0800", "styleCount": 9, "style": 8, "options": 0},  # v1.1: Halo
            {"hex": "0900", "styleCount": 9, "error": RES_BAD_VALUE},
            {"hex": "0300", "styleCount": 3, "error": RES_BAD_VALUE},
            {"hex": "00", "styleCount": 8, "error": RES_BAD_LENGTH}]


def slots_vectors():
    def pack(slots):
        return hx(b"".join(bytes(s) for s in slots))
    stock = [[0, 0, 0, 0], [1, 0, 0, 0], [2, 0, 0, 0], [0, 0, 0, 0]]
    custom = [[4, 0, 2, 0], [5, 3, 1, 0], [6, 1, 0, 1], [3, 0, 2, 0]]
    out = [
        {"hex": pack(stock), "slots": stock},
        {"hex": pack(custom), "slots": custom},
        # arg is normalised to 0 for non text/feed sources, unknown flags dropped
        {"hex": pack([[1, 9, 0, 0xFE], [2, 0, 0, 0], [0, 7, 1, 1], [4, 2, 0, 3]]),
         "slots": [[1, 0, 0, 0], [2, 0, 0, 0], [0, 0, 1, 1], [4, 0, 0, 1]]},
        {"hex": pack([[7, 0, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0]]), "error": RES_BAD_VALUE},
        {"hex": pack([[5, 4, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0]]), "error": RES_BAD_VALUE},
        {"hex": pack([[6, 4, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0]]), "error": RES_BAD_VALUE},
        {"hex": pack([[1, 0, 3, 0], [0, 0, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0]]), "error": RES_BAD_VALUE},
        {"hex": pack(stock)[:-2], "error": RES_BAD_LENGTH},
    ]
    return out


def text_write_vectors():
    out = []
    for idx, s in [(0, "Hello"), (3, "Café 14°C"), (1, ""), (2, "x" * 20)]:
        b = s.encode("utf-8")
        out.append({"hex": hx(bytes([idx]) + b), "index": idx, "text": s})
    out += [{"hex": hx(bytes([4]) + b"Hi"), "error": RES_BAD_VALUE},
            {"hex": hx(bytes([0]) + b"x" * 21), "error": RES_BAD_LENGTH},
            {"hex": "", "error": RES_BAD_LENGTH},
            {"hex": hx(bytes([0]) + b"a\x00b"), "error": RES_BAD_VALUE}]
    return out


def texts_read_vectors():
    texts = ["Kyle's EWatch", "", "Café", "x" * 20]
    b = b"".join(bytes([len(t.encode())]) + t.encode() for t in texts)
    return [{"texts": texts, "hex": hx(b)}]


def feed_bytes(f):
    return struct.pack("<BBII", f["icon"], f["kind"], f["expiresAt"], f["targetAt"]) + \
        f["text"].encode("utf-8")


def feed_write_vectors():
    feeds = [
        {"index": 0, "icon": 2, "kind": 0, "expiresAt": 1790870709, "targetAt": 0, "text": "14°C Cloudy"},
        {"index": 1, "icon": 9, "kind": 1, "expiresAt": 0, "targetAt": 1790861409, "text": "Standup"},
        {"index": 3, "icon": 0, "kind": 0, "expiresAt": 0, "targetAt": 0, "text": ""},
        {"index": 2, "icon": 15, "kind": 1, "expiresAt": 4102358400, "targetAt": 4102358399,
         "text": "y" * 20},
    ]
    out = []
    for f in feeds:
        out.append(dict(f, hex=hx(bytes([f["index"]]) + feed_bytes(f))))
    # A countdown's targetAt is kept; a value feed's targetAt is normalised to 0.
    v = {"index": 0, "icon": 1, "kind": 0, "expiresAt": 0, "targetAt": 123, "text": "Sun"}
    out.append(dict(v, hex=hx(bytes([0]) + feed_bytes(v)), targetAt=0))
    bad = {"icon": 1, "kind": 0, "expiresAt": 0, "targetAt": 0, "text": "x"}
    out += [
        {"hex": hx(bytes([4]) + feed_bytes(bad)), "error": RES_BAD_VALUE},
        {"hex": hx(bytes([0]) + feed_bytes(dict(bad, icon=16))), "error": RES_BAD_VALUE},
        {"hex": hx(bytes([0]) + feed_bytes(dict(bad, kind=2))), "error": RES_BAD_VALUE},
        {"hex": hx(bytes([0]) + feed_bytes(dict(bad, kind=1))), "error": RES_BAD_VALUE},
        {"hex": hx(bytes([0]) + feed_bytes(dict(bad, text="z" * 21))), "error": RES_BAD_LENGTH},
        {"hex": hx(bytes([0]) + feed_bytes(bad)[:9]), "error": RES_BAD_LENGTH},
    ]
    return out


def feeds_read_vectors():
    feeds = [
        {"icon": 2, "kind": 0, "expiresAt": 1790870709, "targetAt": 0, "text": "14°C Cloudy"},
        {"icon": 9, "kind": 1, "expiresAt": 0, "targetAt": 1790861409, "text": "Standup"},
        {"icon": 0, "kind": 0, "expiresAt": 0, "targetAt": 0, "text": ""},
        {"icon": 4, "kind": 0, "expiresAt": 1, "targetAt": 0, "text": "Rain"},
    ]
    b = b"".join(bytes([len(f["text"].encode())]) + feed_bytes(f) for f in feeds)
    return [{"feeds": feeds, "hex": hx(b)}]


def time_write_vectors():
    out = []
    for unix, tz, ms in [(1790859909, 60, 250), (946684800, 0, 0), (1709164800, -480, 999),
                         (1790859909, 345, 500), (4102444799 - 840 * 60, 840, 0)]:
        out.append({"unix": unix, "tz": tz, "ms": ms, "hex": hx(struct.pack("<IhH", unix, tz, ms))})
    out += [
        {"hex": hx(struct.pack("<IhH", 1790859909, 60, 1000)), "error": RES_BAD_VALUE},
        {"hex": hx(struct.pack("<IhH", 1790859909, 841, 0)), "error": RES_BAD_VALUE},
        {"hex": hx(struct.pack("<IhH", 1790859909, -721, 0)), "error": RES_BAD_VALUE},
        {"hex": hx(struct.pack("<IhH", 946684799, 0, 0)), "error": RES_BAD_VALUE},   # 1999
        {"hex": hx(struct.pack("<IhH", 946684800, -60, 0)), "error": RES_BAD_VALUE}, # local 1999
        {"hex": hx(struct.pack("<IhH", 4102444799, 60, 0)), "error": RES_BAD_VALUE}, # local 2100
        {"hex": hx(struct.pack("<Ih", 1790859909, 60)), "error": RES_BAD_LENGTH},
    ]
    return out


def time_read_vectors():
    return [{"unix": 1790859909, "tz": 60, "rtcOk": True, "hex": hx(struct.pack("<IhB", 1790859909, 60, 1))},
            {"unix": 0, "tz": -300, "rtcOk": False, "hex": hx(struct.pack("<IhB", 0, -300, 0))}]


def auth_vectors():
    w = [{"code": c, "hex": hx(struct.pack("<I", c))} for c in (0, 123456, 999999, 42)]
    w += [{"hex": hx(struct.pack("<I", 1000000)), "error": RES_BAD_VALUE},
          {"hex": "40e201", "error": RES_BAD_LENGTH}]
    r = [{"state": 0, "attempts": 3, "seconds": 60, "hex": "00033c"},
         {"state": 1, "attempts": 3, "seconds": 0, "hex": "010300"},
         {"state": 2, "attempts": 0, "seconds": 0, "hex": "020000"}]
    return {"write": w, "read": r}


def revision_vectors():
    out = []
    for rev, mask, src, res in [(1, 0x0001, 0, 0), (4242, 0x0128, 1, 0), (7, 0x0008, 0, 3),
                                (0xFFFFFFFF, 0x01FF, 2, 0)]:
        out.append({"revision": rev, "mask": mask, "source": src, "result": res,
                    "hex": hx(struct.pack("<IHBB", rev, mask, src, res))})
    return out


def message_vectors():
    out = []
    for icon, flags, text in [(13, 0, "Dinner is ready!"), (0, 1, "x"), (14, 0, "m" * 64),
                              (10, 0, "Build passed ✓")]:
        b = bytes([icon, flags]) + text.encode()
        out.append({"icon": icon, "flags": flags, "text": text, "hex": hx(b)})
    v = bytes([1, 0xFF]) + b"hi"
    out.append({"icon": 1, "flags": 1, "text": "hi", "hex": hx(v)})   # unknown flags dropped
    out += [{"hex": hx(bytes([1, 0])), "error": RES_BAD_LENGTH},
            {"hex": hx(bytes([1, 0]) + b"m" * 65), "error": RES_BAD_LENGTH},
            {"hex": hx(bytes([16, 0]) + b"hi"), "error": RES_BAD_VALUE},
            {"hex": hx(bytes([1, 0]) + b"h\x00i"), "error": RES_BAD_VALUE}]
    return out


def control_vectors():
    return [{"hex": "01", "op": 1}, {"hex": "02", "op": 2}, {"hex": "03", "op": 3},
            {"hex": "04", "op": 4}, {"hex": "05", "op": 5}, {"hex": "0500", "op": 5},
            {"hex": "06", "error": RES_UNSUPPORTED}, {"hex": "00", "error": RES_UNSUPPORTED},
            {"hex": "", "error": RES_BAD_LENGTH}, {"hex": "0100000000", "error": RES_BAD_LENGTH}]


def info_vectors():
    styles = ["Classic", "Sans", "Bold", "Serif", "Mono", "Digital", "Outline", "Shadow", "Halo"]
    caps = 0x17
    head = bytes([1, 1]) + struct.pack("<H", caps) + bytes([4, 4, 20, 4, 20, 64, 6, 15, 16, len(styles)])
    body = b"".join(bytes([len(s)]) + s.encode() for s in styles)
    tail = bytes([5]) + b"1.1.0" + bytes([11]) + b"EWatch-1A2B"
    return [{"caps": caps, "styles": styles, "firmware": "1.1.0", "device": "EWatch-1A2B",
             "hex": hx(head + body + tail)}]


def civil_vectors():
    out = []
    for unix, tz in [(0, 0), (946684800, 0), (1790859909, 60), (1790859909, -600),
                     (1709164800, 0), (1709164799, 0), (4102358400, 0), (2147472000, 330),
                     (1790812800, -1), (1790899199, 1), (1767225599, 840), (1767268800, -720)]:
        t = dt.datetime.fromtimestamp(unix + tz * 60, tz=dt.timezone.utc)
        out.append({"unix": unix, "tz": tz, "year": t.year, "month": t.month, "day": t.day,
                    "hour": t.hour, "minute": t.minute, "second": t.second,
                    "weekday": (t.weekday() + 1) % 7})
    return out


# ---------------------------------------------------------------------------
# Display formatting (hand-written expectations)
# ---------------------------------------------------------------------------
def glyph_vectors():
    v = [
        ("Hello", "48656c6c6f"),
        ("14°C", "3134f843"),
        ("Café", "43616682"),
        ("Zürich", "5a8172696368"),
        ("£5 ½", "9c3520ab"),
        ("→ ♥", "1a2003"),
        ("Wait…", "576169742e2e2e"),
        ("日本", "3f3f"),
        ("\U0001F44D\U0001F3FD ok", "3f206f6b"),
        ("☀️ Sunny", "0f2053756e6e79"),
        ("tab\there", "7461622068657265"),
        ("line\nbreak", "6c696e65627265616b"),
        ("‘q’ – “x”", "277127202d2022782 2".replace(" ", "")),
        ("€9", "4539"),
        ("⌂", "7f"),
        ("©", "3f"),
        ("ñ Ñ", "a420a5"),
        ("Ø ø", "4f206f"),
        ("♪♫", "0e0e"),
        ("Été", "907482"),
    ]
    out = [{"utf8": hx(s.encode("utf-8")), "text": s, "glyphs": g, "cap": 40} for s, g in v]
    # Raw byte cases (malformed UTF-8) and capacity limits.
    out += [
        {"utf8": "ff41", "glyphs": "3f41", "cap": 40},
        {"utf8": "e282", "glyphs": "3f3f", "cap": 40},
        {"utf8": "c080", "glyphs": "3f3f", "cap": 40},
        {"utf8": "eda080", "glyphs": "3f3f3f", "cap": 40},   # UTF-16 surrogate
        {"utf8": hx(b"abcdef"), "glyphs": hx(b"abc"), "cap": 3},
        {"utf8": hx("Wait…".encode()), "glyphs": hx(b"Wait."), "cap": 5},
    ]
    return out


def countdown_vectors():
    v = [(-5, "now"), (0, "now"), (1, "in 1m"), (59, "in 1m"), (60, "in 1m"), (61, "in 2m"),
         (1500, "in 25m"), (3540, "in 59m"), (3541, "in 1h"), (3600, "in 1h"),
         (3601, "in 1h1m"), (11100, "in 3h5m"), (172740, "in 47h59m"), (172741, "in 2d"),
         (259199, "in 3d"), (864000, "in 10d")]
    return [{"delta": d, "text": t} for d, t in v]


def date_vectors():
    return [
        {"year": 2026, "month": 10, "day": 1, "weekday": 4, "long": "Thu 1 Oct 2026", "short": "Thu 1 Oct"},
        {"year": 2024, "month": 2, "day": 29, "weekday": 4, "long": "Thu 29 Feb 2024", "short": "Thu 29 Feb"},
        {"year": 2025, "month": 13, "day": 3, "weekday": 9, "long": "--- 3 ??? 2025", "short": "--- 3 ???"},
    ]


def layout_vectors():
    now = {"year": 2026, "month": 10, "day": 1, "hour": 14, "minute": 5, "second": 9, "weekday": 4}
    unix = 1790859909  # 2026-10-01 14:05:09 at UTC+1
    texts = ["Kyle's EWatch", "Go team!", "", "A very long text ok!"]
    feeds = [
        {"icon": 2, "kind": 0, "expiresAt": 0, "targetAt": 0, "text": "14°C Cloudy"},
        {"icon": 9, "kind": 1, "expiresAt": 0, "targetAt": unix + 1500, "text": "Standup"},
        {"icon": 4, "kind": 0, "expiresAt": unix - 10, "targetAt": 0, "text": "Rain 3pm"},
        {"icon": 0, "kind": 1, "expiresAt": 0, "targetAt": unix + 11100, "text": "Quarterly planning!!"},
    ]
    base = {"rtcOk": True, "now": now, "unixNow": unix, "batOk": True, "batPct": 87,
            "texts": texts, "feeds": feeds}
    nortc = dict(base, rtcOk=False, unixNow=0)
    nobat = dict(base, batOk=False)

    def g(s):
        return hx(s.encode("latin-1"))

    cases = [
        # (data, slot [src,arg,color,flags], rowSize, width, glyphs, icon, iconArg, size, truncated)
        (base, [1, 0, 0, 0], 3, 240, g(":09"), 0, 255, 3, False),
        (base, [2, 0, 0, 0], 2, 240, g("Thu 1 Oct 2026"), 0, 255, 2, False),
        (base, [3, 0, 1, 0], 2, 240, g("Thu 1 Oct"), 0, 255, 2, False),
        (base, [4, 0, 2, 0], 2, 240, g("87%"), 0x80, 87, 2, False),
        (base, [4, 0, 0, 1], 2, 240, g("87%"), 0, 255, 2, False),
        (base, [5, 0, 0, 0], 3, 240, g("Kyle's EWatch"), 0, 255, 3, False),
        (base, [5, 3, 0, 0], 3, 240, g("A very long text ok!"), 0, 255, 2, False),
        (base, [5, 2, 0, 0], 2, 240, "", 0, 255, 0, False),
        (base, [6, 0, 0, 0], 2, 240, "3134f843" + g(" Cloudy"), 2, 255, 2, False),
        (base, [6, 1, 1, 0], 2, 240, g("Standup in 25m"), 9, 255, 2, False),
        (base, [6, 1, 0, 0], 3, 240, g("Standup in 25m"), 9, 255, 2, False),
        (base, [6, 1, 0, 1], 3, 240, g("Standup in 25m"), 0, 255, 2, False),
        (base, [6, 2, 0, 0], 2, 240, "", 0, 255, 0, False),
        (base, [6, 3, 0, 0], 2, 240, g("Quarterly.. in 3h5m"), 0, 255, 2, True),
        (base, [5, 3, 0, 0], 2, 100, g("A very.."), 0, 255, 2, True),
        (base, [0, 0, 0, 0], 2, 240, "", 0, 255, 0, False),
        (nortc, [2, 0, 0, 0], 2, 240, "", 0, 255, 0, False),
        (nortc, [6, 1, 0, 0], 2, 240, "", 0, 255, 0, False),
        (nortc, [6, 0, 0, 0], 2, 240, "3134f843" + g(" Cloudy"), 2, 255, 2, False),
        (nobat, [4, 0, 0, 0], 2, 240, g("--%"), 0x80, 255, 2, False),
    ]
    out = []
    for data, slot, row, width, glyphs, icon, icon_arg, size, trunc in cases:
        out.append({"data": data, "slot": slot, "rowSize": row, "width": width,
                    "glyphs": glyphs, "icon": icon if size else 0,
                    "iconArg": icon_arg, "color": slot[2], "size": size, "truncated": trunc})
    return out


def main():
    vectors = {
        "_comment": "Generated by tools/gen_vectors.py. Shared by the C++ and JS test suites.",
        "rgb": rgb_vectors(),
        "theme": theme_vectors(),
        "brightness": brightness_vectors(),
        "face": face_vectors(),
        "slots": slots_vectors(),
        "textWrite": text_write_vectors(),
        "textsRead": texts_read_vectors(),
        "feedWrite": feed_write_vectors(),
        "feedsRead": feeds_read_vectors(),
        "timeWrite": time_write_vectors(),
        "timeRead": time_read_vectors(),
        "auth": auth_vectors(),
        "revision": revision_vectors(),
        "message": message_vectors(),
        "control": control_vectors(),
        "info": info_vectors(),
        "civil": civil_vectors(),
        "glyphs": glyph_vectors(),
        "countdown": countdown_vectors(),
        "date": date_vectors(),
        "layout": layout_vectors(),
    }
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(vectors, indent=1, ensure_ascii=True) + "\n")
    n = sum(len(v) if isinstance(v, list) else sum(len(x) for x in v.values())
            for k, v in vectors.items() if not k.startswith("_"))
    print(f"wrote {OUT} ({n} vectors)")


if __name__ == "__main__":
    main()
