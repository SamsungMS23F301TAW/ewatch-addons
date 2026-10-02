// The web preview must be pixel-identical to the firmware's renderer.
//
// tools/preview/render.py compiles src/apps/face/* against the real
// Arduino_GFX + Arduino_Canvas and writes golden frames to
// test/vectors/frames/. This test re-renders every scenario with the page's
// JavaScript port and compares every pixel.
import assert from 'node:assert/strict';
import { existsSync } from 'node:fs';
import path from 'node:path';
import test from 'node:test';
import { ROOT, loadCore, loadJson, readPng } from './load-core.mjs';

const C = loadCore();
const S = loadJson('test/vectors/face_scenarios.json');
const enc = new TextEncoder();

function frameFor(over) {
  const pick = (k) => (k in over ? over[k] : S.defaults[k]);
  const th = pick('theme').map((h) => parseInt(h, 16));
  const n = pick('now');
  const now = { year: n[0], month: n[1], day: n[2], hour: n[3], minute: n[4], second: n[5], weekday: n[6] };
  const tz = pick('tz');
  const unix = C.civilToUnix(now, tz);
  const rtcOk = pick('rtcOk');
  const wifi = pick('wifi');
  const tm = pick('timers');
  return {
    bg: th[0], fg: th[1], accent: th[2], line: th[3], style: pick('style'), options: pick('options'),
    rtcOk, now, unixNow: rtcOk ? unix : 0, batOk: pick('batOk'), batPct: pick('batPct'),
    wifiShown: wifi[0], wifiEnabled: wifi[1], wifiAp: wifi[2], wifiConnected: wifi[3], wifiRssi: wifi[4],
    ble: pick('ble'), swRun: tm[0], swMs: tm[1], tmrOn: tm[2], tmrMs: tm[3],
    slots: pick('slots'),
    texts: pick('texts').map((t) => enc.encode(t).subarray(0, 20)),
    feeds: pick('feeds').map((f) => ({
      icon: f.icon, kind: f.kind, text: enc.encode(f.text).subarray(0, 20),
      expiresAt: f.expiresIn ? unix + f.expiresIn : 0,
      targetAt: f.kind === 1 ? unix + f.targetIn : 0,
    })),
  };
}

function compare(name, fb) {
  const file = path.join(ROOT, 'test/vectors/frames', `${name}.png`);
  if (!existsSync(file)) throw new Error(`missing ${file}; run python3 tools/preview/render.py`);
  const png = readPng(file);
  assert.equal(png.w, 240); assert.equal(png.h, 280);
  let diff = 0, first = -1;
  for (let i = 0; i < fb.length; i++) {
    const [r, g, b] = C.rgb888(fb[i]);
    if (r !== png.rgb[i * 3] || g !== png.rgb[i * 3 + 1] || b !== png.rgb[i * 3 + 2]) { if (first < 0) first = i; diff++; }
  }
  assert.equal(diff, 0, `${name}: ${diff} pixel(s) differ, first at (${first % 240}, ${Math.floor(first / 240)})`);
}

test('font data is embedded', () => assert.ok(C.fontsLoaded));
test('anti-aliased type and icon atlases are embedded', () => assert.ok(C.aaLoaded));

for (const sc of S.scenarios) {
  test(`face "${sc.name}" matches the firmware pixel for pixel`, () => {
    const g = new C.Canvas565(240, 280);
    C.renderFace(g, frameFor(sc));
    compare(sc.name, g.fb);
  });
}

test('icon sheet matches the firmware pixel for pixel', () => {
  // Mirror of renderIconSheet() in tools/preview/preview.cpp.
  const g = new C.Canvas565(240, 280);
  g.fillScreen(0x0000);
  g.fillRect(120, 0, 120, 280, 0xFFDF);
  const items = [];
  for (let id = 1; id <= 15; id++) items.push([id, 255]);
  items.push([C.ICON_BATTERY, 100], [C.ICON_BATTERY, 55], [C.ICON_BATTERY, 10], [C.ICON_BATTERY, 255]);
  items.forEach(([id, arg], i) => {
    const col = Math.floor(i / 10), row = i % 10;
    const y = 6 + row * 27;
    for (let side = 0; side < 2; side++) {
      const bg = side ? 0xFFDF : 0x0000, fg = side ? 0x18E3 : 0xFFFF;
      const x = side * 120 + 4 + col * 58;
      C.drawIcon(g, id, arg, x, y + 4, 2, fg, bg);
      C.drawIcon(g, id, arg, x + 24, y, 3, fg, bg);
    }
  });
  compare('icons', g.fb);
});
