# Friend Radar beacon protocol, version 1.0

Every EWatch running Friend Radar sends one Bluetooth LE advertisement while
the radar app is open (and, if background alerts are enabled, during short
background windows). This document is the complete over-the-air format. The
reference implementation is `src/apps/radar/logic/fr_beacon.cpp`. The example
below is checked byte-for-byte by `test/test_beacon` (`test_known_answer_vector`).

## Advertising parameters

| Item | Value |
|---|---|
| PDU type | legacy `ADV_NONCONN_IND`: non-connectable, non-scannable, no scan response |
| Address | a fresh *random static* address each radio session. The chip's public MAC is never used. |
| Interval | 100 ms foreground, 60 ms in background windows (falls back to 100 ms if the controller refuses) |
| TX power | +9 dBm (`ESP_PWR_LVL_P9`), set explicitly so distance estimates are consistent |
| AD structures | exactly one: Manufacturer Specific Data. No Flags AD (not needed for non-connectable adverts). |

Receivers scan passively with duplicate filtering **off**, so every packet
updates the sender's RSSI.

## Layout

All multi-byte integers are little-endian. Offsets are from the start of the
advertising data.

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0 | 1 | AD length | `3 + 15 + name length` (18..30) |
| 1 | 1 | AD type | `0xFF`, Manufacturer Specific Data |
| 2 | 2 | Company ID | `0xFFFF`, reserved by the Bluetooth SIG for testing |
| 4 | 3 | Magic | ASCII `E` `W` `R`, for "EWatch Radar" |
| 7 | 1 | Version | high nibble = major (1), low nibble = minor (0) → `0x10` |
| 8 | 4 | Watch ID | random, persistent per watch; never `0x00000000` or `0xFFFFFFFF` |
| 12 | 1 | Ref 1 m | `int8` dBm: the RSSI a receiver should see 1 m from this watch |
| 13 | 1 | Flags | bit 0 background window, bit 1 calibrated, bit 2 background alerts on; bits 3–7 reserved (send 0, ignore) |
| 14 | 1 | Clock | radar clock, whole seconds mod 120 (0..119), for background rendezvous |
| 15 | 1 | Event | bits 0–1 kind, bits 2–7 sequence number (see below) |
| 16 | 2 | Target | 16-bit tag of the addressed watch; `0` = untargeted |
| 18 | 1 | Event age | time since the event went on air, in 100 ms units, saturating at 255 |
| 19 | 0–12 | Name | display name, printable ASCII (0x20–0x7E); ends at the AD end or the first `0x00` |

Total size is 19 + name length ≤ 31 bytes, the legacy advertising limit.

### Field details

**Watch ID.** Generated once with the hardware RNG and stored in the addon's
NVS namespace. "New radar ID" in the menu replaces it, after which existing
mates no longer recognise the watch until they add it again.

**Ref 1 m.** The default is −60 dBm. Calibration measures the median RSSI of
a friend's watch held 1 m away. Assuming the radio channel is reciprocal (the
same hardware and the same path in both directions), that is also the RSSI
the friend sees from us at 1 m, so we advertise it. Valid range: −90..−25
dBm. Decoders reject values outside it.

**Clock.** The sender's *radar time* = RTC local time + a per-watch offset,
in whole seconds, mod 120. Background windows open at the same radar-time
slots on every watch, so two sleeping watches listen at the same moment.
When a watch hears a saved mate with a **lower** watch ID whose clock differs
by 2 s or more, it shifts its own offset to match. A group of mates therefore
converges on the clock of its lowest-ID member. Strangers never move your
clock.

**Event.** One event at a time. A new event always gets a new 6-bit sequence
number, so receivers spot new events by comparing sequence numbers per sender
and per kind.

| Kind | Value | Target | Meaning |
|---|---:|---|---|
| None | 0 | 0 | nothing on air |
| Wave | 1 | addressee | "I just noticed you nearby": start the shared hello animation |
| Shake | 2 | 0 | "my wearer just shook their wrist" (bump to celebrate) |
| Celebrate | 3 | addressee | "we both shook while right here": join the celebration |

Waves stay on air for 30 s, so a sleeping mate's background window can
still catch one. Shakes stay on air for 3 s and celebrations for 4 s. If a
shake or celebration interrupts a wave, the wave resumes afterwards with its
original sequence number, so it is not a new event.

**Target tag.** `t = (mix32(id ^ 0x74616721) >> 16)`, with 0 replaced by 1.
`mix32` is the MurmurHash3 32-bit finaliser. A 16-bit tag keeps the packet
short. A collision between two watches near the same mate is about 1 in
65,536, and is harmless because the sender must also be one of your mates.

**Event age.** The sender refreshes the advertisement about four times a
second while an event is on air, so a late listener can work out when the
event started and join the animation at the matching frame.

## Annotated example

A watch with ID `0x1A2B3C4D`, calibrated at −59 dBm, background alerts on,
radar clock 77, waving at a mate whose tag is `0xBEEF`, wave 1.3 s old,
named "Kyle":

```
16 FF FF FF  45 57 52 10  4D 3C 2B 1A  C5  06  4D  A9  EF BE  0D  4B 79 6C 65
│  │  └─┬─┘   E  W  R  │  └───┬────┘  │   │   │   │   └─┬─┘  │   K  y  l  e
│  │ company     magic v1.0  id        ref flg clk evt target age  name
│  └ MSD
└ AD length 22
```

* `C5` = −59 dBm
* `06` = calibrated (bit 1) + background alerts (bit 2)
* `4D` = 77 s
* `A9` = `(42 << 2) | 1`: sequence 42, kind 1 (wave)
* `0D` = 13 × 100 ms

## Decoding rules (receivers MUST)

1. Walk the AD structures and stop at a zero length or the end of the data.
   A structure that overruns the data invalidates the packet.
2. Take the first Manufacturer Specific Data structure with company `0xFFFF`
   and magic `EWR`. Ignore other structures.
3. Reject a major version other than 1. Accept any minor version: minor
   revisions keep this layout exactly, and anything that changes it bumps the
   major version.
4. Reject IDs `0` and `0xFFFFFFFF`, Ref 1 m outside −90..−25, and clock ≥ 120.
5. Ignore unknown flag bits. Drop non-printable name bytes.
6. Treat the first event seen from a sender as **baseline** for untargeted
   kinds. Targeted events (wave, celebrate) addressed to you count even on
   first sight, so a waking watch can answer a wave already on the air.

## Shared animation timing

* The **initiator** starts its animation 80 ms after its event goes on air.
  80 ms is about the typical scan-reception latency.
* A **receiver** of a fresh event (age under 300 ms) starts immediately.
  Older events are joined at `now − age + 80 ms`, so both screens show the
  same frame.
* If two watches start a hello on their own at about the same time, each
  re-aligns to the other's earlier start when it hears the other's wave.
* Animations are seeded with `pairSeed(min(idA, idB), max(idA, idB), salt)`.
  Both watches compute the same seed, so the shapes and colours match. Only
  the caption (the other person's name) differs.

## Privacy and security notes

* The watch advertises **only** while the radar app is open, or for about
  3.5 s per background window when background alerts are switched on (off
  by default).
* The Watch ID is a stable identifier on the air: anyone scanning can
  recognise the watch while it advertises. It is not linked to the hardware
  MAC and can be replaced with "New radar ID".
* Beacons are not authenticated. A device could copy a mate's ID. The worst
  case is a spurious hello on your watch, which is rate limited: at most one
  accepted wave per mate every 2 minutes, and our own alerts at most once per
  mate every 10 minutes. Only watches you have saved as mates can trigger
  buzzes or animations.
