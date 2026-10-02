# EWatch Config Service — protocol v1.1

The Bluetooth Low Energy GATT protocol between an EWatch running the
**EWatch Companion** addon and a web page (or any BLE central) that restyles
the watch live: theme colours, brightness, clock style, the four face
data-source slots, custom texts, pushed "live data", time sync, messages.

| | |
|---|---|
| Firmware implementation | `src/core/ble_proto.{h,cpp}` (codec), `src/core/ble_config.{h,cpp}` (GATT server) |
| Web implementation | `web/index.html`, the `CORE` script block |
| Shared test vectors | `test/vectors/protocol_vectors.json` (200 cases, used by both test suites) |
| Changes | **v1.1** (firmware 1.1.0): face style 8 **Halo** added and made the default; style 0 renamed *Classic*; *Reset colours* returns to the default style. Additive only: v1.0 clients keep working (see §6) |
| Generator for the vectors | `tools/gen_vectors.py` (independent Python implementation of the layouts) |

Conventions used throughout:

- **Little-endian** for every multi-byte integer.
- **Colours are RGB565** (`rrrrrggg gggbbbbb`) in a `u16`. Pages convert from
  8-bit channels by **truncation**: `((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)`,
  and back by bit replication (`r8 = (r5 << 3) | (r5 >> 2)`, `g8 = (g6 << 2) | (g6 >> 4)`).
  This is exactly what BaseOS's own web settings page does.
- **Times on the wire are Unix seconds (UTC)**, `u32`. The watch's RTC keeps
  *local* time; the UTC offset (`tzOffsetMin`, minutes, −720..+840) converts.
- **Text is UTF-8**. The watch draws it with its 6×8 CP437 bitmap font after a
  documented glyph mapping (below).
- `u8/u16/u32/i16` = unsigned / signed integers of that many bits.

---

## 1. Discovery and connection

| Item | Value |
|---|---|
| Service UUID | `e1280001-ca10-4c2f-babd-3d3dd6094bbc` (custom 128-bit base `e128xxxx-ca10-4c2f-babd-3d3dd6094bbc`) |
| Advertising data | Flags (LE General Discoverable, BR/EDR not supported), complete list of 128-bit service UUIDs (the service above), Appearance `0x00C2` (Smartwatch) |
| Scan response | Complete local name `EWatch-XXXX` (last two bytes of the BT MAC, hex) |
| Advertising interval | 100–150 ms, TX power +3 dBm |
| When it advertises | Only after the user opens **Companion** on the watch: for a 3-minute window (restarted each time the screen opens), never while a client is connected, never during a lock-out, never in deep sleep |
| Connections | One at a time. The watch requests a 30–50 ms interval, latency 0, 5 s supervision timeout |
| ATT MTU | Preferred 255 (the central decides). Every notification payload is ≤ 20 bytes, so nothing depends on the MTU |
| Long writes | Required for Texts (≤ 21 B), Feeds (≤ 31 B) and Message (≤ 66 B) at the default MTU; every Chrome platform performs these automatically |

A Web Bluetooth page finds the watch with:

```js
navigator.bluetooth.requestDevice({
  filters: [{ services: ['e1280001-ca10-4c2f-babd-3d3dd6094bbc'] }],
  optionalServices: ['battery_service', 'device_information'],
});
```

The service UUID is in the primary advertisement (not only the scan response)
because Chrome on Windows matches filters per packet.

## 2. Security model and the auth flow

**No OS-level pairing or bonding is used** (see README "Security" for the
research behind this choice). Instead every connection is gated by a
**6-digit code that the watch shows on its own screen**:

1. A central connects. The watch generates a fresh random code
   (`esp_random() % 1000000`), shows it on the Companion screen (switching to
   it if needed), and starts a **60 s** pairing timer.
2. The page reads **Auth** (open) to learn the state, asks the user for the
   code, and writes it as a `u32` to **Auth**.
3. Correct: the connection becomes *authorised* for its lifetime. Auth
   notifies `AUTHORIZED`.
4. Wrong: `attemptsLeft` decrements (3 attempts per connection). At 0, Auth
   notifies `LOCKED`, the watch drops the link after 400 ms and stays
   invisible for **30 s**.
5. No correct code within 60 s: the watch drops the link.
6. An authorised connection with no config write and no touch on the watch for
   **5 minutes** is dropped (it would otherwise keep the watch awake).

Before authorisation:

- **Open** characteristics (Info, Auth, the standard Battery Level and Device
  Information) behave normally.
- **Protected reads return an empty value** (0 bytes).
- **Protected writes are ignored** and answered with a Revision notification
  carrying `result = 1 (NOT_AUTH)`. NimBLE-Arduino 1.4.3 cannot return ATT
  error codes from write callbacks, and the ATT authorisation/encryption
  errors would make some operating systems start OS pairing, which this
  design deliberately avoids.

The code is single-use and bound to one connection, so a captured code is
worthless afterwards. The link itself is **not encrypted**: a nearby BLE
sniffer can read the settings being written. See README "Security" for the
threat model and the experimental `EWATCH_BLE_REQUIRE_ENCRYPTION` build flag
(which sets `capabilities` bit 3).

## 3. Characteristics

All in the EWatch Config Service. R = read, W = write with response,
N = notify.

| UUID (`e128____-…`) | Name | Props | Access | Length |
|---|---|---|---|---|
| `0002` | Info | R | open | 14 + strings (≤ 160) |
| `0003` | Auth | R W N | open | write 4, read 3 |
| `0004` | Revision | R N | protected | 8 |
| `0010` | Theme | R W N | protected | 8 |
| `0011` | Brightness | R W N | protected | 1 |
| `0012` | Face | R W N | protected | 2 |
| `0013` | Slots | R W N | protected | 16 |
| `0014` | Texts | R W | protected | write 1–21, read ≤ 84 |
| `0015` | Feeds | R W | protected | write 11–31, read ≤ 124 |
| `0016` | Time | R W N | protected | write 8, read 7 |
| `0017` | Message | W | protected | 3–66 |
| `0018` | Control | W | protected | 1–4 |

Standard services, both open:

| Service | Characteristics |
|---|---|
| Device Information `0x180A` | Manufacturer `0x2A29` = "EWatch", Model `0x2A24` = "EWatch v2 (ESP32-S3)", Firmware `0x2A26` = addon version, Software `0x2A28` = "BaseOS dbe73c2 + Companion". No serial number (Chrome block-lists `0x2A25`) |
| Battery `0x180F` | Battery Level `0x2A19`, `u8` 0–100 %, R N (notified when it changes, checked every 5 s while connected) |

### 3.1 Info (`0002`), read, open

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | protocol major (1) |
| 1 | u8 | protocol minor (1) |
| 2 | u16 | capabilities: bit0 messages, bit1 feeds, bit2 12-hour option, bit3 link encryption required, bit4 control |
| 4 | u8 | slot count (4) |
| 5 | u8 | text count (4) |
| 6 | u8 | text max bytes (20) |
| 7 | u8 | feed count (4) |
| 8 | u8 | feed text max bytes (20) |
| 9 | u8 | message max bytes (64) |
| 10 | u8 | highest slot source id (6) |
| 11 | u8 | highest icon id (15) |
| 12 | u8 | minimum brightness (16) |
| 13 | u8 | face style count *N* |
| 14 | … | *N* × (`u8` length + ASCII style name, ≤ 16), then (`u8` length + firmware version), then (`u8` length + device name) |

A client must refuse to continue if the major version differs from its own.
Style names come from the firmware, so a page can list whatever styles a
future firmware offers. Firmware 1.1.0 reports nine:

| Index | Name | Since | Notes |
|---|---|---|---|
| 0 | Classic | 1.0 (named "Default" in 1.0) | BaseOS's own face, 6×8 bitmap font |
| 1–7 | Sans, Bold, Serif, Mono, Digital, Outline, Shadow | 1.0 | BaseOS styles |
| 8 | Halo | 1.1 | the anti-aliased face; default on fresh installs and after *Reset colours* |

New styles are only ever **appended**: the index is what the watch stores, so
existing indices keep their meaning. Names are for display only.

### 3.2 Auth (`0003`), open

Write: `u32` code, 0..999999. Read / notify:

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | state: 0 = code required, 1 = authorised, 2 = locked |
| 1 | u8 | attempts left (3, 2, 1, 0) |
| 2 | u8 | seconds left to enter the code (0 when not pairing) |

### 3.3 Revision (`0004`), read and notify

Notified after **every** change to the watch's config, whatever its source,
and after every rejected write.

| Offset | Type | Field |
|---|---|---|
| 0 | u32 | config revision: increments on every applied config change, persisted in NVS (actions such as messages don't increment it) |
| 4 | u16 | changed mask: bit0 theme, 1 brightness, 2 face, 3 slots, 4 texts, 5 feeds, 6 time, 7 message, 8 control |
| 6 | u8 | source: 0 this client, 1 the watch's own UI (Settings pages, WiFi web page), 2 system (reset, boot) |
| 7 | u8 | result: 0 OK, 1 not authorised, 2 bad length, 3 bad value, 4 busy, 5 unsupported |

Texts and feeds don't notify their own values (they can exceed 20 bytes): a
client re-reads them when a Revision notification from another source sets
their bits.

### 3.4 Theme (`0010`)

| Offset | Type | Field |
|---|---|---|
| 0 | u16 | background |
| 2 | u16 | foreground (text) |
| 4 | u16 | accent (buttons, highlights, accent slots) |
| 6 | u16 | line (dividers, "dim" slots) |

Exactly 8 bytes. Applied live; saved to BaseOS's own NVS keys so the theme
survives addon changes. Example: BaseOS default `0000 ffff 0f00 ef7b`.

### 3.5 Brightness (`0011`)

`u8` backlight PWM duty. Values below 16 are clamped to 16 so the panel never
goes dark.

### 3.6 Face (`0012`)

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | style index (< style count from Info; otherwise result 3). 8 = Halo on firmware 1.1.0 |
| 1 | u8 | options: bit0 = 12-hour clock. Unknown bits are dropped |

### 3.7 Slots (`0013`)

Four 4-byte records, top to bottom on the face:

| Slot | Classic styles (0–7) | Halo (8) |
|---|---|---|
| 0 Top | y 68, above the time, text size 2 (12×16 px glyphs) | line above the time, rows 44–80 |
| 1 Upper | the stock seconds row (y 174, or 186 for FreeFont / Digital styles), size 3 | line below the time, rows 160–186 |
| 2 Lower | the stock date row (y 212 / 220), size 2 | the next line, rows 186–210 |
| 3 Bottom | y 250, size 2 | a glass card, rows 210–280 |

In both, the bottom slot is hidden while BaseOS's stopwatch / timer lines
show.

Each record is `[source u8][arg u8][color u8][flags u8]`:

| source | Shows | arg |
|---|---|---|
| 0 | nothing | – |
| 1 | seconds `:SS` | – |
| 2 | date `Thu 1 Oct 2026` | – |
| 3 | short date `Thu 1 Oct` | – |
| 4 | battery glyph + `87%` | – |
| 5 | custom text | text index 0–3 |
| 6 | live data (feed) | feed index 0–3 |

`color`: 0 theme foreground, 1 accent, 2 line ("dim"). `flags` bit0 hides the
icon of battery/feed slots; other bits are dropped. For sources other than 5
and 6, `arg` is normalised to 0. An out-of-range source, colour or index
rejects the whole write (result 3). The defaults `00000000 01000000 02000000
00000000` (seconds and date) reproduce the stock BaseOS face pixel for pixel in
the Classic style, and give Halo its seconds track and small-caps date.

**Layout, Classic styles.** A slot is drawn at its natural size if it fits in
240 px (an icon takes `8 × size` px, battery `11 × size`, plus a `3 × size`
gap; each glyph is `6 × size`). If not, it drops to size 2. If it still
doesn't fit, it is cut with `..` (a countdown keeps its `in 25m` suffix and
shortens the label). `faceslots::layoutSlot()` and its JavaScript mirror
implement this; the vectors pin it down.

**Layout, Halo.** The same content (`faceslots::slotRaw()`) is set in
anti-aliased Inter Display and fitted by measured pixel width: 204 px per
line, 172 px inside the bottom card, less 22 px for an icon (30 px for the
battery glyph). Text that doesn't fit is cut with `…`; a countdown keeps its
suffix (drawn in the accent) and shortens the label. Dates render as spaced
small caps (`THU 1 OCT 2026`); seconds render as a track with a moving knob
plus `30`. Colours are contrast-corrected against the background, so
`fg == bg` themes stay legible. The face renderer and its JavaScript mirror
are pinned down by the golden frames in `test/vectors/frames/`.

### 3.8 Texts (`0014`)

Write `[index u8][UTF-8 bytes 0..20]` (empty clears; NUL bytes rejected).
Read returns all four: 4 × `[length u8][UTF-8 bytes]`.

### 3.9 Feeds (`0015`): live data pushed by the page

Write:

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | index 0–3 |
| 1 | u8 | icon id (0 none, 1–15, table below) |
| 2 | u8 | kind: 0 value, 1 countdown |
| 3 | u32 | `expiresAt`, Unix UTC seconds; 0 = never. The slot hides at this time |
| 7 | u32 | `targetAt`, Unix UTC seconds (countdown only; must be non-zero) |
| 11 | 0..20 B | UTF-8 text (value) or label (countdown) |

Read returns 4 × `[text length u8][icon][kind][u32 expiresAt][u32 targetAt][text]`.

A **value** feed shows `icon text` until it expires; empty text clears it. A
**countdown** shows `label in 25m`, `in 3h5m`, `in 2d`, then `now`, computed
on the watch from its RTC, so it keeps counting after the page disconnects.
Minutes round up. Below 48 h it shows hours and minutes; from 48 h, whole
days. A countdown with `expiresAt = 0` hides 15 minutes after its target.
Without a valid clock, countdowns hide and value feeds still show.

Icons: 1 sun, 2 partly cloudy, 3 cloud, 4 rain, 5 storm, 6 snow, 7 fog,
8 moon, 9 calendar, 10 bell, 11 heart, 12 star, 13 chat, 14 check,
15 alert.

### 3.10 Time (`0016`)

Write `[u32 unix][i16 tzOffsetMin][u16 millis]`. The watch rounds to the
nearest second, converts to local time, writes the RV-3028 through
`requestSetRTC()` and saves `tzOffsetMin`. Rejected (result 3) if the offset
is outside −720..+840, `millis > 999`, or the *local* time is outside
2000–2099 (the RTC stores a two-digit year). Clients should send right after
a second boundary.

Read / notify `[u32 unix][i16 tzOffsetMin][u8 rtcOk]` (unix 0 when the RTC
isn't readable).

### 3.11 Message (`0017`), write

`[icon u8][flags u8][UTF-8 text 1..64]`. flags bit0 = silent (no buzz). The
watch buzzes twice and shows the message full screen for 20 s, or until
tapped. If the user is in a settings page, it waits until they return to
the face. A new message replaces the one on screen.

### 3.12 Control (`0018`), write

`[opcode u8][reserved…]` (1–4 bytes):

| Opcode | Action |
|---|---|
| 1 | Identify: buzz twice, wake the screen, greet on the Companion screen |
| 2 | Reset face: slots, texts, feeds and face options back to defaults |
| 3 | Reset colours: BaseOS theme, brightness 200, the default style (Halo, 8, on firmware 1.1.0; Classic, 0, on 1.0) |
| 4 | Show the watch face (useful right after pairing) |
| 5 | Show the Companion screen |

Unknown opcodes: result 5 (unsupported).

## 4. Text on the watch

The watch converts UTF-8 to glyphs of its CP437-layout 6×8 font
(`faceslots::utf8ToGlyphs`). Halo and the Companion screens draw the same
glyph codes with anti-aliased Inter Display atlases that cover the whole set,
so text maps identically in every style:

- Printable ASCII passes through; tab becomes a space; other control
  characters are dropped.
- Latin-1 letters and symbols the font has map to their glyphs, for example
  `° £ ¥ ¢ ½ ¼ ± ² µ · « » ¿ ¡ é è ê ë à á â ä å æ ç ì í î ï ñ ò ó ô ö ù ú û ü ÿ ß Ä Å Æ Ç É Ñ Ö Ü`.
  Accented capitals the font lacks become their base letter (`À → A`).
- Typographic quotes and dashes become ASCII, and `…` becomes `...`.
  `€ → E`, `× → x`, `← ↑ → ↓ ↔ ↕`, `♥ ♦ ♣ ♠ • ☺ ☻ ♪ ♫ ☼ ☀ ▲ ▼ ► ◄ ■ █ ░ ▒ ▓ √ ✓ ∞ ≈ ≤ ≥ ⌂` all map.
- Zero-width joiners, variation selectors and skin-tone modifiers are dropped.
- Anything else, including emoji, CJK and Cyrillic, becomes `?`. Malformed
  UTF-8 gives one `?` per bad byte.
- The output never contains NUL, LF or CR (GFX treats those as controls).

The web page runs the same mapping to preview text and warns when characters
would show as `?`.

## 5. Recommended client flow

```
requestDevice(filter: service)            user picks EWatch-XXXX
gatt.connect()
read Info                                  check major == 1, get style names
startNotifications(Auth); read Auth        state 0 -> ask for the code
write Auth(code)                           repeat while attempts > 0
on Auth == AUTHORIZED:
  startNotifications(Revision, Theme, Brightness, Face, Slots, Time, Battery)
  read Theme, Brightness, Face, Slots, Texts, Feeds, Time, Battery
  write Time(now) at a second boundary     (optional, recommended)
edit -> debounce ~120-350 ms -> write the one characteristic that changed
on Revision(source != 0): re-read the groups in its mask
on Revision(result != 0): show the error
```

GATT operations must be serialised: Chrome rejects overlapping operations.
Writes are idempotent, so retrying after a disconnect is safe.

## 6. Versioning

- `protocolMajor` changes on any incompatible layout change. Clients refuse
  other majors.
- `protocolMinor` changes on compatible additions: new characteristics, new
  enum values clients may not know, new capability bits. Clients must ignore
  unknown characteristics, and should hide features whose capability bit is
  clear.
- Reserved bits and bytes are written as 0 and ignored or dropped by the
  watch.
- **v1.0 → v1.1** added face style 8 (Halo). A v1.0 page talking to 1.1.0
  firmware lists nine styles from Info and can select Halo; its own preview
  falls back to drawing Classic for it. A v1.1 page talking to 1.0.0 firmware
  only offers the eight styles that firmware reports.
- NVS: the addon's own data lives in namespace `ble-companion` with its own
  `schema` byte (currently 1). Theme, brightness, style and UTC offset stay in
  BaseOS's `ewatch` namespace under their original keys.

## 7. Worked example

Connect, enter code 482913, make the background navy and put the weather in
the top slot:

```
W Auth      61 5e 07 00                                  482913
N Auth      01 03 00                                     authorised
W Theme     0f 00 ff ff 1f 00 ef 7b                      bg 0x000F (navy), fg white, accent blue, line grey
N Theme     0f 00 ff ff 1f 00 ef 7b
N Revision  2a 00 00 00 01 00 00 00                      rev 42, theme, by client, OK
W Feeds     00 01 00 b5 09 be 6a 00 00 00 00 31 38 c2 b0 43 20 53 75 6e 6e 79
            feed 0, sun, value, expires 1790839221, "18°C Sunny"
W Slots     06 00 00 00 01 00 00 00 02 00 00 00 00 00 00 00
            top = feed 0, upper = seconds, lower = date, bottom = nothing
```
