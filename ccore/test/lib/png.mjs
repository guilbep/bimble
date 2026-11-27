// A small PNG reader for the tests (ticket NUI1: the native app's screenshots): 8-bit RGB or RGBA,
// not interlaced (what SDL_SavePNG writes). Returns { width, height, rgba } or throws.
// Lives in test/lib/, not test/: ci/run.sh runs every test/*.mjs as a suite.
import { readFileSync } from "node:fs";
import { inflateSync } from "node:zlib";

export function readPng(path) {
  const b = readFileSync(path);
  if (b.readUInt32BE(0) !== 0x89504e47) throw new Error(`${path}: not a PNG`);
  let at = 8, width = 0, height = 0, type = 0, depth = 0, inter = 0;
  const idat = [];
  while (at < b.length) {
    const n = b.readUInt32BE(at), kind = b.toString("ascii", at + 4, at + 8), data = b.subarray(at + 8, at + 8 + n);
    if (kind === "IHDR") width = data.readUInt32BE(0), height = data.readUInt32BE(4), depth = data[8], type = data[9], inter = data[12];
    else if (kind === "IDAT") idat.push(data);
    else if (kind === "IEND") break;
    at += 12 + n;
  }
  if (depth !== 8 || (type !== 2 && type !== 6) || inter) throw new Error(`${path}: depth ${depth}, colour type ${type}, interlace ${inter}: not read here`);
  const ch = type === 6 ? 4 : 3, stride = width * ch, raw = inflateSync(Buffer.concat(idat));
  const out = Buffer.alloc(width * height * 4), line = Buffer.alloc(stride), prev = Buffer.alloc(stride);
  for (let y = 0; y < height; y++) {
    const f = raw[y * (stride + 1)], src = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));
    for (let x = 0; x < stride; x++) {
      const a = x >= ch ? line[x - ch] : 0, up = prev[x], c = x >= ch ? prev[x - ch] : 0;
      let v = src[x];
      if (f === 1) v += a;
      else if (f === 2) v += up;
      else if (f === 3) v += (a + up) >> 1;
      else if (f === 4) { const p = a + up - c, pa = Math.abs(p - a), pb = Math.abs(p - up), pc = Math.abs(p - c); v += pa <= pb && pa <= pc ? a : pb <= pc ? up : c; }
      line[x] = v & 255;
    }
    for (let x = 0; x < width; x++) {
      out[(y * width + x) * 4] = line[x * ch], out[(y * width + x) * 4 + 1] = line[x * ch + 1], out[(y * width + x) * 4 + 2] = line[x * ch + 2];
      out[(y * width + x) * 4 + 3] = ch === 4 ? line[x * ch + 3] : 255;
    }
    line.copy(prev);
  }
  return { width, height, rgba: out };
}

/** How many distinct colours a region holds (x0, y0, x1, y1 in pixels), counted to `cap`. */
export function colours(img, x0 = 0, y0 = 0, x1 = img.width, y1 = img.height, cap = 1000) {
  const seen = new Set();
  for (let y = y0; y < y1 && seen.size < cap; y += 2)
    for (let x = x0; x < x1 && seen.size < cap; x += 2) seen.add(img.rgba.readUInt32LE((y * img.width + x) * 4));
  return seen.size;
}

/** The share of pixels (0..1) whose RGB differs by more than `tol` (0..255) between two images of one size. */
export function diffShare(a, b, tol = 24) {
  if (a.width !== b.width || a.height !== b.height) return 1;
  let bad = 0;
  for (let i = 0; i < a.width * a.height; i++) {
    const d = Math.max(Math.abs(a.rgba[i * 4] - b.rgba[i * 4]), Math.abs(a.rgba[i * 4 + 1] - b.rgba[i * 4 + 1]), Math.abs(a.rgba[i * 4 + 2] - b.rgba[i * 4 + 2]));
    if (d > tol) bad++;
  }
  return bad / (a.width * a.height);
}
