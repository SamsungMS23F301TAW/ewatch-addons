// Loads the pure CORE block out of web/index.html (between the
// "// @@CORE:BEGIN" and "// @@CORE:END" markers) into an isolated VM context,
// so the tests run exactly the code the page ships.
import { readFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import vm from 'node:vm';
import zlib from 'node:zlib';

const here = path.dirname(fileURLToPath(import.meta.url));
export const ROOT = path.resolve(here, '..', '..');

export function loadCore() {
  const html = readFileSync(path.join(ROOT, 'web', 'index.html'), 'utf8');
  const a = html.indexOf('// @@CORE:BEGIN');
  const b = html.indexOf('// @@CORE:END');
  if (a < 0 || b < a) throw new Error('CORE markers not found in web/index.html');
  const ctx = vm.createContext({ TextEncoder, TextDecoder, atob, Buffer });
  vm.runInContext(html.slice(a, b) + '\nthis.Core = Core;', ctx, { filename: 'web/index.html#CORE' });
  return ctx.Core;
}

export function loadJson(rel) {
  return JSON.parse(readFileSync(path.join(ROOT, rel), 'utf8'));
}

export const hex = (bytes) => Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');
export const unhex = (h) => Uint8Array.from(h.match(/../g) || [], (x) => parseInt(x, 16));

// Minimal PNG reader for the 8-bit RGB, filter-0 files tools/preview writes.
export function readPng(file) {
  const buf = readFileSync(file);
  let pos = 8, w = 0, h = 0;
  const idat = [];
  while (pos < buf.length) {
    const len = buf.readUInt32BE(pos);
    const type = buf.toString('latin1', pos + 4, pos + 8);
    const data = buf.subarray(pos + 8, pos + 8 + len);
    if (type === 'IHDR') { w = data.readUInt32BE(0); h = data.readUInt32BE(4); }
    if (type === 'IDAT') idat.push(data);
    pos += 12 + len;
  }
  const raw = zlib.inflateSync(Buffer.concat(idat));
  const rgb = new Uint8Array(w * h * 3);
  for (let y = 0; y < h; y++) {
    if (raw[y * (w * 3 + 1)] !== 0) throw new Error('unexpected PNG filter');
    rgb.set(raw.subarray(y * (w * 3 + 1) + 1, (y + 1) * (w * 3 + 1)), y * w * 3);
  }
  return { w, h, rgb };
}
