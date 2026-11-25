// Usage: node bench/pixels_loop.mjs [runs] [out.json]
// Edit → pixels: one static server and one headless Chromium (raw CDP, same flags as test/pixels.mjs).
// Each run: t0; flip SPIKE_K in main_wasm.c (1.0 <-> 2.0, the slab thickness); ./build.sh wasm-dev; t1;
// slab.reload() with a cache-busting query; poll each requestAnimationFrame until the frame differs
// from the previous one (the face-colour pixel count, and the centre pixel column); t2.
// Prints a JSON summary (build, reload→pixels, total: median and p95, ms). SPIKE_K is restored to 1.0.
import { createServer } from 'node:http';
import { readFileSync, writeFileSync } from 'node:fs';
import { readFile } from 'node:fs/promises';
import { execFileSync } from 'node:child_process';
import { dirname, extname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { launch, connect } from '../test/cdp.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const webDir = join(root, 'web');
const wasmFile = join(root, 'build', 'slab_dev.wasm');
const srcFile = join(root, 'main_wasm.c');
const RUNS = Number(process.argv[2] ?? process.env.RUNS ?? 10);
const outJson = process.argv[3];
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm' };
const KNOB = /^#define SPIKE_K [0-9.]+/m;

const original = readFileSync(srcFile, 'utf8');
if (!KNOB.test(original)) { console.error('main_wasm.c has no #define SPIKE_K'); process.exit(1); }
const setK = (k) => writeFileSync(srcFile, readFileSync(srcFile, 'utf8').replace(KNOB, `#define SPIKE_K ${k}`));
const restore = () => { try { setK('1.0'); } catch { /* best effort */ } };
const build = () => execFileSync(join(root, 'build.sh'), ['wasm-dev'], { stdio: ['ignore', 'ignore', 'inherit'] });

const server = createServer(async (req, res) => {
  const path = decodeURIComponent(new URL(req.url, 'http://x').pathname);
  const file = path === '/module.wasm' ? wasmFile : join(webDir, path === '/' ? 'index.html' : path);
  if (!file.startsWith(webDir) && file !== wasmFile) { res.writeHead(403).end(); return; }
  try {
    const body = await readFile(file);
    res.writeHead(200, { 'content-type': TYPES[extname(file)] ?? 'application/octet-stream', 'cache-control': 'no-store' });
    res.end(body);
  } catch { res.writeHead(404).end(); }
});

let chrome, cdp, code = 0;
const cleanup = async () => {
  restore();
  try { cdp?.close(); } catch { /* closed */ }
  try { await chrome?.close(); } catch { /* gone */ }
  server.close();
};
for (const sig of ['SIGINT', 'SIGTERM']) process.on(sig, async () => { await cleanup(); process.exit(130); });

// The frame signature, read in the page: face-colour pixel count (lit and unlit grey) plus a
// hash of the centre pixel column. Any change of thickness changes both.
const SIGNATURE = `(() => {
  const [w, h] = slab.size();
  const gl = document.getElementById('view').getContext('webgl2');
  const col = new Uint8Array(h * 4);
  gl.readPixels(w >> 1, 0, 1, h, gl.RGBA, gl.UNSIGNED_BYTE, col);
  let hash = 0; for (let i = 0; i < col.length; i++) hash = (hash * 31 + col[i]) >>> 0;
  return hash + ':' + slab.countNear(214, 214, 210, 60);
})()`;
// Reload, then poll each animation frame until the signature differs from the one before. Returns ms.
const RELOAD_AND_WAIT = (prev) => `new Promise((resolve, reject) => {
  const t = performance.now();
  slab.reload('module.wasm?bust=' + Date.now() + Math.random()).catch(reject);
  let n = 0;
  const tick = () => {
    const s = ${SIGNATURE};
    if (s !== ${JSON.stringify(prev)}) resolve({ sig: s, ms: performance.now() - t, frames: n });
    else if (++n > 600) reject(new Error('frame did not change after 600 frames'));
    else requestAnimationFrame(tick);
  };
  requestAnimationFrame(tick);
})`;

const q = (xs, p) => { const s = [...xs].sort((a, b) => a - b); return s[Math.min(s.length - 1, Math.ceil(p * s.length) - 1)]; };
const sum = (xs) => ({ median: q(xs, 0.5), p95: q(xs, 0.95), min: Math.min(...xs), max: Math.max(...xs) });

try {
  setK('1.0');
  build();
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  chrome = await launch();
  cdp = await connect(chrome.wsUrl);
  const { targetId } = await cdp.send('Target.createTarget', { url: 'about:blank' });
  const { sessionId } = await cdp.send('Target.attachToTarget', { targetId, flatten: true });
  const send = (m, p) => cdp.send(m, p, sessionId);
  const evaluate = async (expression) => {
    const r = await send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
    return r.result.value;
  };
  await send('Runtime.enable');
  await send('Page.enable');
  await send('Page.navigate', { url: `http://127.0.0.1:${server.address().port}/index.html?wasm=module.wasm` });
  let ready = null;
  for (let i = 0; i < 100 && !ready; i++) {
    ready = await evaluate('window.slab && window.slab.stats').catch(() => null);
    if (!ready) await new Promise((r) => setTimeout(r, 100));
  }
  if (!ready) throw new Error('page did not draw the first frame');
  let prev = await evaluate(SIGNATURE);

  const rows = [];
  let k = '1.0';
  for (let i = 0; i < RUNS; i++) {
    k = k === '1.0' ? '2.0' : '1.0';
    const t0 = performance.now();
    setK(k);
    build();
    const t1 = performance.now();
    const r = await evaluate(RELOAD_AND_WAIT(prev));
    const t2 = performance.now();
    prev = r.sig;
    rows.push({ k, build: t1 - t0, pixels: t2 - t1, total: t2 - t0, in_page_ms: r.ms, frames: r.frames });
    console.error(`run ${i + 1}/${RUNS} K=${k}: build ${(t1 - t0).toFixed(0)} ms, reload→pixels ${(t2 - t1).toFixed(0)} ms (${r.frames} rAF), total ${(t2 - t0).toFixed(0)} ms`);
  }
  const result = {
    runs: RUNS,
    build_ms: sum(rows.map((r) => r.build)),
    pixels_ms: sum(rows.map((r) => r.pixels)),
    total_ms: sum(rows.map((r) => r.total)),
    chromium: (await cdp.send('Browser.getVersion')).product,
    rows,
  };
  const text = JSON.stringify(result, null, 2);
  if (outJson) writeFileSync(outJson, text);
  console.log(text);
} catch (err) {
  console.error(`pixels_loop: ${err.stack ?? err}`);
  code = 1;
} finally {
  await cleanup();
}
process.exit(code);
