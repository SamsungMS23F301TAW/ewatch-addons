// The web page's protocol codec and face-slot formatter against the shared
// vectors (the same file the C++ tests in test/test_protocol use).
//
//   node --test test/web/
import assert from 'node:assert/strict';
import test from 'node:test';
import { hex, loadCore, loadJson, unhex } from './load-core.mjs';

const C = loadCore();
const V = loadJson('test/vectors/protocol_vectors.json');
const bytes = (s) => new TextEncoder().encode(s);
// Values built inside the VM context have that realm's prototypes; compare as plain JSON.
const plain = (x) => JSON.parse(JSON.stringify(x));

function expectError(fn, code, label) {
  assert.throws(fn, (e) => e.code === code, label);
}

test('rgb565 packing matches the firmware', () => {
  for (const v of V.rgb) {
    const n = parseInt(v.rgb.slice(1), 16);
    const c = C.rgb565((n >> 16) & 255, (n >> 8) & 255, n & 255);
    assert.equal(c, v.rgb565, v.rgb);
    assert.equal(C.hexTo565(v.rgb), v.rgb565, v.rgb);
    assert.equal(C.hexFrom565(c), v.back, v.rgb);
  }
  for (let c = 0; c <= 0xFFFF; c++) {
    const [r, g, b] = C.rgb888(c);
    assert.equal(C.rgb565(r, g, b), c);
  }
});

test('theme / brightness / face', () => {
  for (const v of V.theme) {
    if ('error' in v) { expectError(() => C.decodeTheme(unhex(v.hex)), v.error, v.hex); continue; }
    const t = C.decodeTheme(unhex(v.hex));
    assert.deepEqual({ ...t }, { bg: v.bg, fg: v.fg, accent: v.accent, line: v.line });
    assert.equal(hex(C.encodeTheme(t)), v.hex);
  }
  for (const v of V.brightness) {
    if ('error' in v) { expectError(() => C.decodeBrightness(unhex(v.hex)), v.error, v.hex); continue; }
    assert.equal(C.decodeBrightness(unhex(v.hex)), v.value, v.hex);
  }
  for (const v of V.face) {
    if ('error' in v) { expectError(() => C.decodeFace(unhex(v.hex), v.styleCount), v.error, v.hex); continue; }
    const f = C.decodeFace(unhex(v.hex), v.styleCount);
    assert.equal(f.style, v.style); assert.equal(f.options, v.options);
  }
});

test('slots', () => {
  for (const v of V.slots) {
    if ('error' in v) { expectError(() => C.decodeSlots(unhex(v.hex)), v.error, v.hex); continue; }
    const s = C.decodeSlots(unhex(v.hex));
    assert.deepEqual(plain(s), v.slots, v.hex);
    if (hex(C.encodeSlots(v.slots)) !== v.hex) {
      // Only the normalising vector re-encodes differently.
      assert.equal(hex(C.encodeSlots(s)), hex(C.encodeSlots(v.slots)));
    }
  }
});

test('texts', () => {
  for (const v of V.textWrite) {
    if ('error' in v) { expectError(() => C.decodeTextWrite(unhex(v.hex)), v.error, v.hex); continue; }
    const t = C.decodeTextWrite(unhex(v.hex));
    assert.equal(t.index, v.index); assert.equal(t.text, v.text);
    assert.equal(hex(C.encodeTextWrite(v.index, v.text)), v.hex);
  }
  for (const v of V.textsRead) {
    assert.equal(hex(C.encodeTextsRead(v.texts)), v.hex);
    assert.deepEqual(Array.from(C.decodeTextsRead(unhex(v.hex))), v.texts);
  }
});

test('feeds', () => {
  for (const v of V.feedWrite) {
    if ('error' in v) { expectError(() => C.decodeFeedWrite(unhex(v.hex)), v.error, v.hex); continue; }
    const { index, feed } = C.decodeFeedWrite(unhex(v.hex));
    assert.equal(index, v.index);
    assert.deepEqual({ ...feed }, { icon: v.icon, kind: v.kind, expiresAt: v.expiresAt, targetAt: v.targetAt, text: v.text });
    const raw = unhex(v.hex);
    const normalised = v.targetAt === 0 && raw.length >= 11 && (raw[7] | raw[8] | raw[9] | raw[10]) !== 0;
    if (!normalised) assert.equal(hex(C.encodeFeedWrite(v.index, v)), v.hex);
  }
  for (const v of V.feedsRead) {
    assert.equal(hex(C.encodeFeedsRead(v.feeds)), v.hex);
    assert.deepEqual(plain(C.decodeFeedsRead(unhex(v.hex))), v.feeds);
  }
});

test('time / auth / revision / message / control / info', () => {
  for (const v of V.timeWrite) {
    if ('error' in v) { expectError(() => C.decodeTimeWrite(unhex(v.hex)), v.error, v.hex); continue; }
    assert.deepEqual({ ...C.decodeTimeWrite(unhex(v.hex)) }, { unix: v.unix, tz: v.tz, ms: v.ms });
    assert.equal(hex(C.encodeTimeWrite(v)), v.hex);
  }
  for (const v of V.timeRead) {
    assert.equal(hex(C.encodeTimeRead(v.unix, v.tz, v.rtcOk)), v.hex);
    assert.deepEqual({ ...C.decodeTimeRead(unhex(v.hex)) }, { unix: v.unix, tz: v.tz, rtcOk: v.rtcOk });
  }
  for (const v of V.auth.write) {
    if ('error' in v) { expectError(() => C.decodeAuthWrite(unhex(v.hex)), v.error, v.hex); continue; }
    assert.equal(C.decodeAuthWrite(unhex(v.hex)), v.code);
    assert.equal(hex(C.encodeAuthWrite(v.code)), v.hex);
  }
  for (const v of V.auth.read) {
    assert.equal(hex(C.encodeAuthRead(v.state, v.attempts, v.seconds)), v.hex);
    assert.deepEqual({ ...C.decodeAuthRead(unhex(v.hex)) }, { state: v.state, attempts: v.attempts, seconds: v.seconds });
  }
  for (const v of V.revision) {
    const r = { revision: v.revision, mask: v.mask, source: v.source, result: v.result };
    assert.equal(hex(C.encodeRevision(r)), v.hex);
    assert.deepEqual({ ...C.decodeRevision(unhex(v.hex)) }, r);
  }
  for (const v of V.message) {
    if ('error' in v) { expectError(() => C.decodeMessage(unhex(v.hex)), v.error, v.hex); continue; }
    assert.deepEqual({ ...C.decodeMessage(unhex(v.hex)) }, { icon: v.icon, flags: v.flags, text: v.text });
  }
  for (const v of V.control) {
    if ('error' in v) { expectError(() => C.decodeControl(unhex(v.hex)), v.error, v.hex); continue; }
    assert.equal(C.decodeControl(unhex(v.hex)), v.op);
  }
  for (const v of V.info) {
    assert.equal(hex(C.encodeInfo({ caps: v.caps, styles: v.styles, firmware: v.firmware, device: v.device })), v.hex);
    const d = C.decodeInfo(unhex(v.hex));
    assert.deepEqual(Array.from(d.styles), v.styles);
    assert.equal(d.firmware, v.firmware); assert.equal(d.device, v.device); assert.equal(d.caps, v.caps);
  }
});

test('civil time', () => {
  for (const v of V.civil) {
    const c = C.unixToCivil(v.unix, v.tz);
    assert.deepEqual({ ...c }, { year: v.year, month: v.month, day: v.day, hour: v.hour, minute: v.minute, second: v.second, weekday: v.weekday }, `${v.unix}@${v.tz}`);
    assert.equal(C.civilToUnix(c, v.tz), v.unix);
  }
  let wd = 6;
  for (let z = C.daysFromCivil(2000, 1, 1); z <= C.daysFromCivil(2099, 12, 31); z++) {
    const [y, m, d] = C.civilFromDays(z);
    assert.equal(C.daysFromCivil(y, m, d), z);
    assert.equal(C.weekdayFromDays(z), wd);
    wd = (wd + 1) % 7;
  }
});

test('glyph mapping, countdown and dates', () => {
  for (const v of V.glyphs) assert.equal(hex(C.utf8ToGlyphs(unhex(v.utf8), v.cap)), v.glyphs, v.text || v.utf8);
  for (const v of V.countdown) assert.equal(C.formatCountdown(v.delta), v.text, String(v.delta));
  for (const v of V.date) {
    const t = { year: v.year, month: v.month, day: v.day, weekday: v.weekday };
    assert.equal(C.formatDate(t, false), v.long);
    assert.equal(C.formatDate(t, true), v.short);
  }
  // Every 1- and 2-byte input decodes without control glyphs (as in C++).
  for (let v = 0; v <= 0xFFFF; v++) {
    for (const g of C.utf8ToGlyphs(Uint8Array.of(v >> 8, v & 255), 6)) assert.ok(g !== 0 && g !== 10 && g !== 13);
  }
});

test('slot layout matches the firmware', () => {
  for (const [i, v] of V.layout.entries()) {
    const d = v.data;
    const data = {
      rtcOk: d.rtcOk, now: d.now, unixNow: d.unixNow, batOk: d.batOk, batPct: d.batPct,
      texts: d.texts.map(bytes),
      feeds: d.feeds.map((f) => ({ ...f, text: bytes(f.text) })),
    };
    const c = C.layoutSlot(v.slot, data, v.rowSize, v.width);
    const label = `layout[${i}] ${JSON.stringify(v.slot)}`;
    assert.equal(hex(c.glyphs), v.glyphs, label);
    assert.equal(c.size, v.size, label);
    assert.equal(c.icon, v.icon, label);
    assert.equal(c.iconArg, v.iconArg, label);
    assert.equal(c.color, v.color, label);
    assert.equal(c.truncated, v.truncated, label);
    assert.ok(C.contentWidth(c) <= v.width, label);
  }
});

test('clipUtf8 never splits a character', () => {
  assert.equal(C.clipUtf8('héllo wörld, très bien', 20), 'héllo wörld, très');   // exactly 20 bytes
  assert.equal(C.clipUtf8('😀😀😀😀😀😀', 20), '😀😀😀😀😀');
  assert.equal(new TextEncoder().encode(C.clipUtf8('°'.repeat(30), 20)).length, 20);
});
