// Usage: node test/pixels.mjs web/stub.wasm
// Serves web/, loads the page in headless Chromium through raw CDP, checks pixels. Exit 0 or 1.
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { basename, dirname, extname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { launch, connect } from './cdp.mjs';

const webDir = resolve(dirname(fileURLToPath(import.meta.url)), '..', 'web');
const wasmArg = process.argv[2] ?? 'web/stub.wasm';
const wasmFile = resolve(wasmArg);
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm' };

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
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const port = server.address().port;

const failures = [];
const check = (ok, what) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}`); if (!ok) failures.push(what); };

const chrome = await launch();
let cdp;
try {
  cdp = await connect(chrome.wsUrl);
  const { targetId } = await cdp.send('Target.createTarget', { url: 'about:blank' });
  const { sessionId } = await cdp.send('Target.attachToTarget', { targetId, flatten: true });
  const errors = [];
  cdp.on((m) => {
    if (m.sessionId !== sessionId) return;
    if (m.method === 'Runtime.consoleAPICalled' && (m.params.type === 'error' || m.params.type === 'assert')) {
      errors.push(m.params.args.map((a) => a.value ?? a.description).join(' '));
    } else if (m.method === 'Runtime.exceptionThrown') {
      const d = m.params.exceptionDetails;
      errors.push(d.exception?.description ?? d.text);
    }
  });
  const send = (method, params) => cdp.send(method, params, sessionId);
  await send('Runtime.enable');
  await send('Page.enable');
  await send('Page.navigate', { url: `http://127.0.0.1:${port}/index.html?wasm=module.wasm` });

  const evaluate = async (expression) => {
    const r = await send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
    return r.result.value;
  };
  let stats = null;
  for (let i = 0; i < 100 && !stats; i++) {
    stats = await evaluate('window.slab && window.slab.stats').catch(() => null);
    if (!stats) await new Promise((r) => setTimeout(r, 100));
  }
  check(!!stats, `window.slab.stats is set (${basename(wasmFile)})`);
  if (stats) {
    console.log('stats', JSON.stringify(stats));
    const [w, h] = await evaluate('slab.size()');
    const corner = await evaluate('slab.pixel(2, 2)');
    check(corner.every((c, i) => Math.abs(c - [247, 247, 245][i]) <= 2), `corner (2,2) is background: ${corner}`);
    const edgePx = await evaluate('slab.countNear(26, 26, 28, 8)');
    check(edgePx >= 100, `edge pixels within 8 of 26,26,28: ${edgePx} >= 100`);
    const [r, g, b] = await evaluate(`slab.pixel(${w >> 1}, ${h >> 1})`);
    const isBg = Math.abs(r - 247) <= 2 && Math.abs(g - 247) <= 2 && Math.abs(b - 245) <= 2;
    check(!isBg && Math.abs(r - g) <= 3 && b <= r, `centre pixel is a grey face: ${[r, g, b]}`);
    // reload without a page reload: the stats object must be replaced, and the page must survive
    const again = await evaluate(`slab.reload('module.wasm').then(() => performance.timeOrigin)`);
    const origin = await evaluate('performance.timeOrigin');
    check(again === origin, 'reload(url) does not reload the page');
    console.log('stats after reload', JSON.stringify(await evaluate('slab.stats')));
  }
  check(errors.length === 0, `no console errors${errors.length ? ': ' + errors.join(' | ') : ''}`);
} catch (err) {
  check(false, `harness: ${err.message}`);
} finally {
  cdp?.close();
  await chrome.close();
  server.close();
}
console.log(failures.length ? `FAILED (${failures.length})` : 'PASS');
process.exit(failures.length ? 1 : 0);
