// The C lab's performance, measured (ticket PERF1): node ccore/tools/perf.mjs [--full] [--out FILE]
//   [--base FILE] [--site DIR | --prebuilt] [--runs N] [--no-lighthouse] [--no-remote]
// Writes ccore/results/perf.md (or FILE; never edit it by hand) and prints the change of each
// number against --base (default: ccore/results/perf.md, the committed one) as "perf: " lines.
// Modes:
//   (default, CI) 1 Lighthouse run per page and preset, the sample model only, no network.
//   --full        3 Lighthouse runs (median), the synthetic large model too (PERF_LARGE elements,
//                 default 20000, bench/large_model.py), the deployed pages when reachable.
// What it measures, on the PRODUCTION build (`npm run build:ccore -- --outDir <tmp>/site/c`, as
// test/lab_pages.mjs; --site DIR reuses a built site whose c/ holds it; --prebuilt, for CI after the lab
// suites: its Vite step only, with out/wasm/ccore.wasm and ccore-view.wasm), served under /c/ by a static
// server like GitHub Pages (gzip for html/js/css/wasm/json/svg, Cache-Control: max-age=600):
//   pages  editor  /c/          (the build default: ccore=1&gl=1, ccore.wasm)
//          viewer  /c/?view=1   (ccore-view.wasm)
//          and, --full and reachable only: the deployed Rust lab https://guilbep.github.io/bimble/
//          and the deployed C lab https://guilbep.github.io/bimble/c/ (offline: a note, never a failure).
//   a. Lighthouse 13 (web-ui devDependency) through its node API on its own headless Chromium
//      (CHROMIUM of test/ports.mjs, SwiftShader): categories performance, accessibility,
//      best-practices; the desktop preset and the default (mobile, simulated throttling); median
//      of the runs per number. Missing package: a "lighthouse: not installed" line, b. still runs.
//   b. our own marks over raw CDP (fresh tab per load, HTTP cache emptied, 1280x800), page clock
//      (performance.now(), 0 = navigation start):
//      wasm     __bbCcore.readyMs: WebAssembly.instantiate returned (ccore.ts);
//      model    __bbCcore.openMs: host_open_text returned (the model parsed and validated);
//      drawn    __bbGl.firstFrameMs: the end of the first glviewport draw() with a mesh (the WebGL
//               commands issued; SwiftShader rasterizes after);
//      shown    the first requestAnimationFrame callback that runs after `drawn` (the frame was
//               handed to the compositor: the first model pixels on screen, within a frame);
//      ready    data-bb-ready set (deeplink.ts: the model loaded and the deep link applied);
//      fcp/lcp  the browser's paint entries; long tasks: PerformanceObserver "longtask" (> 50 ms)
//               from navigation to ready + 1 s; heap: Performance.getMetrics JSHeapUsedSize after
//               a GC; wasm memory: the sum of every WebAssembly.Memory's buffer (Runtime.queryObjects).
//      Median of 3 loads per page and model (1 in CI for the large model: not run there).
//   c. frame time while orbiting: 120 left-drag steps of 3 px (real Input.dispatchMouseEvent), each
//      waiting for the next glviewport frame; the frame time = the requestAnimationFrame interval in
//      which glviewport drew (median, p95, max; all intervals also) and the step latency (dispatch -> frame drawn, Node clock); the CI mode
//      drags in the editor only (the viewer draws with the same glviewport.ts). In a plan
//      view (?view=plan:1, the deep link) a drag pans: the same 120 steps, panning. Triangles drawn
//      per frame (__bbGl.drawnTris) are reported so numbers compare across machines.
// Exit 0; 1 when a local page does not load or the tool throws. Never on a slower number.
import { spawn, execFileSync } from "node:child_process";
import { createServer } from "node:http";
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs";
import { cpus, loadavg, platform, release, tmpdir, totalmem, arch } from "node:os";
import { dirname, extname, join, relative, resolve, sep } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { gzipSync } from "node:zlib";
import { CHROMIUM, port } from "../test/ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = resolve(CCORE, "../web-ui");
const argv = process.argv.slice(2);
const opt = (name) => { const i = argv.indexOf(name); return i >= 0 ? argv[i + 1] : null; };
const FULL = argv.includes("--full");
const OUT = resolve(opt("--out") ?? join(CCORE, "results/perf.md"));
const BASE = resolve(opt("--base") ?? join(CCORE, "results/perf.md"));
const LH_RUNS = Number(opt("--runs") ?? (FULL ? 3 : 1));
const LOADS = 3;
const STEPS = 120;
const LARGE = FULL ? Number(process.env.PERF_LARGE ?? 20000) : 0;
const REMOTE = FULL && !argv.includes("--no-remote");
const PORT = port("perf", 47366);
const ORIGIN = `http://localhost:${PORT}`;
const DEPLOYED = [["deployed Rust lab", "https://guilbep.github.io/bimble/"], ["deployed C lab", "https://guilbep.github.io/bimble/c/"]];
const tmp = mkdtempSync(join(tmpdir(), "ccore-perf-"));
const SITE = opt("--site") ? resolve(opt("--site")) : join(tmp, "site");
const RAW = join(CCORE, "out/perf1");
mkdirSync(RAW, { recursive: true });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const notes = [];
const problems = []; // a local page that did not load: exit 1
const procs = [];
let server = null;
const log = (s) => console.log(`perf: ${s}`);

// ---- numbers ----
const median = (a) => { const s = a.filter((x) => Number.isFinite(x)).sort((p, q) => p - q); if (!s.length) return null; const m = s.length >> 1; return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2; };
const pct = (a, p) => { const s = a.filter(Number.isFinite).sort((x, y) => x - y); return s.length ? s[Math.min(s.length - 1, Math.ceil((p / 100) * s.length) - 1)] : null; };
const fmtN = (n) => (n === null || n === undefined || !Number.isFinite(n) ? "-" : String(Math.round(n)).replace(/\B(?=(\d{3})+(?!\d))/g, ","));
const ms = (n) => (n === null || n === undefined || !Number.isFinite(n) ? "-" : n < 100 ? n.toFixed(1) : fmtN(n));
const kb = (n) => (n === null || n === undefined || !Number.isFinite(n) ? "-" : `${(n / 1024).toFixed(0)} KB`);
const mb = (n) => (n === null || n === undefined || !Number.isFinite(n) ? "-" : `${(n / 1048576).toFixed(1)} MB`);

// ---- processes ----
function start(name, cmd, args) {
  const p = spawn(cmd, args, { stdio: ["ignore", "pipe", "pipe"], detached: true });
  p.out = "";
  p.stdout.on("data", (d) => (p.out = (p.out + d).slice(-20000)));
  p.stderr.on("data", (d) => (p.out = (p.out + d).slice(-20000)));
  p.label = name;
  procs.push(p);
  return p;
}
async function cleanup() {
  for (const sig of ["SIGTERM", "SIGKILL"]) {
    for (const p of procs) if (p.exitCode === null && p.signalCode === null) try { process.kill(-p.pid, sig); } catch {}
    await sleep(300);
  }
  if (server) await new Promise((r) => server.close(r));
  rmSync(tmp, { recursive: true, force: true });
}
async function waitFor(what, timeout, fn) {
  const until = Date.now() + timeout;
  let last;
  while (Date.now() < until) {
    try { const v = await fn(); if (v) return v; } catch (e) { last = e; }
    await sleep(50);
  }
  throw new Error(`timeout waiting for ${what}${last ? `: ${last.message ?? last}` : ""}`);
}
const chromeArgs = (dir) => ["--headless=new", "--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--remote-debugging-port=0",
  `--user-data-dir=${dir}`, "--no-first-run", "--no-default-browser-check", "about:blank"];
async function chromium(name) {
  const c = start(name, CHROMIUM, chromeArgs(join(tmp, name)));
  const ws = await waitFor(`${name}: DevTools line`, 60000, () => c.out.match(/DevTools listening on (ws:\/\/\S+)/)?.[1]);
  return { proc: c, ws, port: Number(new URL(ws).port) };
}

// ---- raw CDP (as the lab tests) ----
async function cdpConnect(wsUrl) {
  const ws = new WebSocket(wsUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = () => rej(new Error("CDP connect failed")); });
  let id = 0;
  const pending = new Map(), listeners = [];
  ws.onmessage = (ev) => {
    const msg = JSON.parse(ev.data);
    if (msg.id !== undefined && pending.has(msg.id)) {
      const { resolve, reject } = pending.get(msg.id);
      pending.delete(msg.id);
      if (msg.error) reject(new Error(`${msg.error.message} ${msg.error.data ?? ""}`));
      else resolve(msg.result);
    } else if (msg.method) for (const fn of listeners) fn(msg);
  };
  const send = (method, params = {}, sessionId) => new Promise((resolve, reject) => {
    const m = ++id;
    pending.set(m, { resolve, reject });
    ws.send(JSON.stringify({ id: m, method, params, ...(sessionId ? { sessionId } : {}) }));
  });
  return { send, on: (fn) => listeners.push(fn), close: () => ws.close() };
}

// ---- the site and its server (GitHub Pages: gzip, max-age=600) ----
const TYPES = { ".html": "text/html; charset=utf-8", ".js": "text/javascript", ".css": "text/css", ".wasm": "application/wasm",
  ".json": "application/json", ".woff2": "font/woff2", ".svg": "image/svg+xml", ".png": "image/png", ".ico": "image/x-icon" };
const GZIP = new Set([".html", ".js", ".css", ".wasm", ".json", ".svg"]);
const gzCache = new Map();
function serve() {
  server = createServer((req, res) => {
    const path = decodeURIComponent(new URL(req.url, ORIGIN).pathname);
    let file = resolve(SITE, "." + path);
    if (file !== SITE && !file.startsWith(SITE + sep)) return res.writeHead(404).end();
    if (existsSync(file) && statSync(file).isDirectory()) {
      if (!path.endsWith("/")) return res.writeHead(301, { Location: `${path}/` }).end();
      file = join(file, "index.html");
    }
    if (!existsSync(file)) return res.writeHead(404).end("not found");
    const ext = extname(file);
    const headers = { "Content-Type": TYPES[ext] ?? "application/octet-stream", "Cache-Control": "max-age=600" };
    let body;
    if (GZIP.has(ext) && /\bgzip\b/.test(req.headers["accept-encoding"] ?? "")) {
      if (!gzCache.has(file)) gzCache.set(file, gzipSync(readFileSync(file)));
      body = gzCache.get(file);
      headers["Content-Encoding"] = "gzip";
      headers["Vary"] = "Accept-Encoding";
    } else body = readFileSync(file);
    res.writeHead(200, headers).end(body);
  });
  return new Promise((r) => server.listen(PORT, "127.0.0.1", r));
}

function buildSite() {
  if (argv.includes("--prebuilt")) { // CI: build:ccore without its two wasm builds (ci/run.sh lab made them), as tools/weight.mjs
    const wasm = ["ccore.wasm", "ccore-view.wasm"].map((f) => join(CCORE, "out/wasm", f));
    if (wasm.every(existsSync)) {
      const t0 = Date.now(), dir = join(SITE, "c");
      execFileSync("npx", ["vite", "build", "--base", "./", "--outDir", dir, "--emptyOutDir", "--logLevel", "warn"], { cwd: UI, stdio: ["ignore", "pipe", "pipe"], env: { ...process.env, VITE_CCORE: "1", VITE_CCORE_REV: "" } });
      rmSync(join(dir, "ccore"), { recursive: true, force: true }); // public/ccore leftovers: replaced as copy-ccore.mjs does
      mkdirSync(join(dir, "ccore"));
      for (const f of wasm) copyFileSync(f, join(dir, "ccore", f.split(sep).pop()));
      copyFileSync(join(CCORE, "golden/frame.json"), join(dir, "ccore/frame.json"));
      notes.push("site: --prebuilt (vite build of build:ccore with out/wasm/ccore.wasm and ccore-view.wasm as built by ci/run.sh)");
      log(`vite build (prebuilt wasm) ${Date.now() - t0} ms`);
      return;
    }
    notes.push("site: --prebuilt asked but out/wasm/ has no ccore.wasm or ccore-view.wasm: npm run build:ccore");
  }
  if (opt("--site")) {
    if (!existsSync(join(SITE, "c/index.html"))) throw new Error(`--site ${SITE}: no c/index.html`);
    return;
  }
  if (!existsSync(join(UI, "node_modules"))) throw new Error(`no ${UI}/node_modules: run npm ci in web-ui/`);
  const t0 = Date.now();
  try {
    execFileSync("npm", ["run", "build:ccore", "--", "--outDir", join(SITE, "c"), "--emptyOutDir"], { cwd: UI, stdio: ["ignore", "pipe", "pipe"], env: { ...process.env, VITE_CCORE_REV: "" } });
  } catch (e) {
    console.log(`${e.stdout ?? ""}${e.stderr ?? ""}`.slice(-3000));
    throw new Error("npm run build:ccore failed");
  }
  log(`build:ccore ${Date.now() - t0} ms`);
}

// ---- the in-page probe (before any page script) ----
const PROBE = `(() => {
  const P = window.__perf = { longtasks: [], fcp: null, lcp: null, readyMs: null, shownMs: null, rec: null };
  const obs = (type, fn) => { try { new PerformanceObserver((l) => l.getEntries().forEach(fn)).observe({ type, buffered: true }); } catch (e) {} };
  obs("longtask", (e) => P.longtasks.push([e.startTime, e.duration]));
  obs("paint", (e) => { if (e.name === "first-contentful-paint") P.fcp = e.startTime; });
  obs("largest-contentful-paint", (e) => { P.lcp = e.startTime; });
  new MutationObserver(() => { if (P.readyMs === null && document.documentElement && document.documentElement.dataset.bbReady === "1") P.readyMs = performance.now(); })
    .observe(document, { subtree: true, attributes: true, attributeFilter: ["data-bb-ready"] });
  const shown = () => { const g = window.__bbGl; const now = performance.now();
    if (g && g.firstFrameMs !== null && now > g.firstFrameMs) P.shownMs = now; else requestAnimationFrame(shown); };
  requestAnimationFrame(shown);
  const tick = (t) => { if (P.rec) P.rec.push([t, window.__bbGl ? window.__bbGl.frames : 0]); requestAnimationFrame(tick); };
  requestAnimationFrame(tick);
  P.waitFrame = (n0, limit) => new Promise((res) => { const t0 = performance.now();
    const f = () => { const g = window.__bbGl; if ((g && g.frames > n0) || performance.now() - t0 > limit) res(g ? g.frames : -1); else requestAnimationFrame(f); };
    requestAnimationFrame(f); });
})();`;

let cdp = null, browserInfo = {};
/** A fresh tab on `url`, the probe in; returns helpers. Close with `done()`. */
async function openTab(url, readyTimeout) {
  const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank" });
  const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
  const S = (m, p) => cdp.send(m, p, sessionId);
  const exceptions = [];
  cdp.on((msg) => { if (msg.sessionId === sessionId && msg.method === "Runtime.exceptionThrown") exceptions.push(msg.params.exceptionDetails.exception?.description ?? msg.params.exceptionDetails.text); });
  for (const d of ["Runtime", "Page", "Network", "Performance"]) await S(`${d}.enable`);
  // A cold load: the HTTP cache emptied (not disabled: a disabled cache also turns off <link rel=preload> reuse).
  await S("Network.clearBrowserCache");
  await S("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  await S("Page.addScriptToEvaluateOnNewDocument", { source: PROBE });
  const evalJs = async (expr) => {
    const r = await S("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
    return r.result.value;
  };
  const t0 = Date.now();
  await S("Page.navigate", { url });
  let ready = await waitFor(`${url}: data-bb-ready`, readyTimeout, () => evalJs("window.__perf && window.__perf.readyMs")).then(() => true, () => false);
  const loadWall = Date.now() - t0;
  return { S, evalJs, ready, loadWall, exceptions, done: () => cdp.send("Target.closeTarget", { targetId }) };
}

/** One load: the marks, long tasks, memory. */
async function measureLoad(url, readyTimeout) {
  const tab = await openTab(url, readyTimeout);
  try {
    if (!tab.ready) return { ok: false, why: `no data-bb-ready in ${readyTimeout} ms${tab.exceptions.length ? `; ${tab.exceptions[0]}` : ""}` };
    await sleep(1000); // late long tasks
    const v = await tab.evalJs(`(() => {
      const P = window.__perf, c = window.__bbCcore ?? null, g = window.__bbGl ?? null;
      const res = performance.getEntriesByType("resource").map((e) => ({ name: e.name, end: e.responseEnd, start: e.startTime, transfer: e.transferSize, body: e.decodedBodySize }));
      const measures = Object.fromEntries(performance.getEntriesByType("measure").filter((m) => m.name.startsWith("bimble:")).map((m) => [m.name, m.startTime + m.duration]));
      return { readyMs: P.readyMs, shownMs: P.shownMs, fcp: P.fcp, lcp: P.lcp, longtasks: P.longtasks.filter(([s]) => s <= P.readyMs + 1000),
        cc: c && { readyMs: c.readyMs, openMs: c.openMs, wasmBytes: c.wasmBytes, modelBytes: c.modelBytes },
        gl: g && { firstFrameMs: g.firstFrameMs, drawnTris: g.drawnTris, meshBytes: g.meshBytes, frames: g.frames, error: g.error },
        res, measures, dcl: performance.getEntriesByType("navigation")[0]?.domContentLoadedEventEnd ?? null };
    })()`);
    await tab.S("HeapProfiler.enable");
    await tab.S("HeapProfiler.collectGarbage");
    const metrics = Object.fromEntries((await tab.S("Performance.getMetrics")).metrics.map((m) => [m.name, m.value]));
    let wasmMem = null;
    try {
      const { result: proto } = await tab.S("Runtime.evaluate", { expression: "WebAssembly.Memory.prototype" });
      const { objects } = await tab.S("Runtime.queryObjects", { prototypeObjectId: proto.objectId });
      const r = await tab.S("Runtime.callFunctionOn", { objectId: objects.objectId, returnByValue: true,
        functionDeclaration: "function () { return this.reduce((a, m) => a + m.buffer.byteLength, 0); }" });
      wasmMem = r.result.value;
    } catch {}
    const lt = v.longtasks;
    const wasmRes = v.res.find((r) => /\.wasm(\?|$)/.test(r.name));
    const modelRes = v.cc ? v.res.find((r) => /ccore\/frame\.json|perf\/large-/.test(r.name)) : null;
    return { ok: true, ...v, heap: metrics.JSHeapUsedSize ?? null, wasmMem, loadWall: tab.loadWall,
      longCount: lt.length, longTotal: lt.reduce((a, [, d]) => a + d, 0), longMax: lt.reduce((a, [, d]) => Math.max(a, d), 0),
      longTop: [...lt].sort((a, b) => b[1] - a[1]).slice(0, 3),
      wasmEnd: wasmRes?.end ?? null, wasmTransfer: wasmRes?.transfer ?? null, modelEnd: modelRes?.end ?? null, modelTransfer: modelRes?.transfer ?? null, exceptions: tab.exceptions };
  } finally {
    await tab.done();
  }
}

/** 120 drag steps; the rAF deltas and step latencies. `plan`: the page is in a plan view (pans). */
async function measureOrbit(url, readyTimeout, plan) {
  const tab = await openTab(url, readyTimeout);
  try {
    if (!tab.ready) return { ok: false, why: "page not ready" };
    if (plan) await waitFor("the plan camera", 10000, async () => (await tab.evalJs("window.__bbGl?.camera?.mode")) === "plan").catch(() => null);
    await sleep(500);
    const cam = () => tab.evalJs("JSON.stringify(window.__bbGl?.camera ?? null)").then(JSON.parse);
    const c0 = await cam();
    const r = await tab.evalJs("(() => { const r = document.getElementById('bimble').getBoundingClientRect(); return [r.left, r.top, r.width, r.height]; })()");
    let x = r[0] + r[2] * 0.35, y = r[1] + r[3] * 0.5;
    const mouse = (type, buttons) => tab.S("Input.dispatchMouseEvent", { type, x, y, button: type === "mouseMoved" && !buttons ? "none" : "left", buttons, clickCount: 1 });
    await mouse("mouseMoved", 0);
    await mouse("mousePressed", 1);
    // Untimed: past the click threshold (CLICK_PX), until the drag draws a frame.
    for (let i = 0; i < 4; i++) {
      const n0 = await tab.evalJs("window.__bbGl.frames");
      x += 3;
      await mouse("mouseMoved", 1);
      if ((await tab.evalJs(`window.__perf.waitFrame(${n0}, 1000)`)) > n0) break;
    }
    await tab.evalJs("window.__perf.rec = []");
    const lat = [];
    let misses = 0;
    for (let i = 0; i < STEPS; i++) {
      const n0 = await tab.evalJs("window.__bbGl.frames");
      x += 3;
      if (i % 4 === 0) y += 1;
      const t0 = performance.now();
      await mouse("mouseMoved", 1);
      const n1 = await tab.evalJs(`window.__perf.waitFrame(${n0}, 10000)`);
      if (!(n1 > n0)) misses++;
      lat.push(performance.now() - t0);
    }
    const rec = await tab.evalJs("(() => { const r = window.__perf.rec; window.__perf.rec = null; return r; })()");
    await mouse("mouseReleased", 0);
    const c1 = await cam();
    const g = await tab.evalJs("({ tris: window.__bbGl.drawnTris, frames: window.__bbGl.frames })");
    // Frame time: the rAF interval that ends at the callback after a draw (the draw ran in that
    // interval); idle intervals between steps (the CDP round trip) are not frame times.
    const d = [];
    for (let i = 2; i < rec.length; i++) if (rec[i - 1][1] > rec[i - 2][1]) d.push(rec[i][0] - rec[i - 1][0]);
    const all = rec.slice(1).map(([t], i) => t - rec[i][0]);
    const moved = plan ? JSON.stringify(c0?.target) !== JSON.stringify(c1?.target) : c0?.yaw !== c1?.yaw;
    return { ok: true, mode: c1?.mode ?? null, moved, misses, tris: g.tris, frames: d.length, allMed: median(all),
      dMed: median(d), dP95: pct(d, 95), dMax: d.length ? Math.max(...d) : null, latMed: median(lat), latP95: pct(lat, 95) };
  } finally {
    await tab.done();
  }
}

// ---- Lighthouse ----
let LH = null;
async function loadLighthouse() {
  const dir = join(UI, "node_modules/lighthouse");
  if (!existsSync(join(dir, "core/index.js"))) return null;
  const lighthouse = (await import(pathToFileURL(join(dir, "core/index.js")).href)).default;
  const desktop = (await import(pathToFileURL(join(dir, "core/config/desktop-config.js")).href)).default;
  const version = JSON.parse(readFileSync(join(dir, "package.json"), "utf8")).version;
  return { lighthouse, desktop, version };
}
let lhChrome = null;
async function lighthouseRun(url, preset, tag) {
  const flags = { port: lhChrome.port, output: "json", logLevel: "silent", onlyCategories: ["performance", "accessibility", "best-practices"], maxWaitForLoad: 90000 };
  const r = await LH.lighthouse(url, flags, preset === "desktop" ? LH.desktop : undefined);
  const lhr = r?.lhr;
  if (!lhr) throw new Error(`lighthouse ${url}: no result`);
  writeFileSync(join(RAW, `lh-${tag}.json`), JSON.stringify(lhr));
  const A = lhr.audits, cat = lhr.categories;
  const num = (id) => A[id]?.numericValue ?? null;
  const items = (id) => A[id]?.details?.items ?? [];
  const script = items("resource-summary").find((x) => x.resourceType === "script");
  const flagged = Object.values(A).filter((a) => a.scoreDisplayMode !== "notApplicable" && a.scoreDisplayMode !== "informative" && a.scoreDisplayMode !== "manual"
    && typeof a.score === "number" && a.score < 0.9).map((a) => `${a.id}${a.displayValue ? ` (${a.displayValue})` : ""}`);
  return {
    perf: cat.performance?.score != null ? 100 * cat.performance.score : null,
    a11y: cat.accessibility?.score != null ? 100 * cat.accessibility.score : null,
    bp: cat["best-practices"]?.score != null ? 100 * cat["best-practices"].score : null,
    fcp: num("first-contentful-paint"), lcp: num("largest-contentful-paint"), tbt: num("total-blocking-time"),
    cls: num("cumulative-layout-shift"), si: num("speed-index"), bytes: num("total-byte-weight"),
    js: script?.transferSize ?? null, unusedJs: A["unused-javascript"]?.details?.overallSavingsBytes ?? null,
    error: lhr.runtimeError?.message ?? null, warnings: lhr.runWarnings ?? [], flagged,
  };
}
const LH_KEYS = ["perf", "a11y", "bp", "fcp", "lcp", "tbt", "cls", "si", "bytes", "js", "unusedJs"];
const medianOf = (runs, keys) => Object.fromEntries(keys.map((k) => [k, median(runs.map((r) => r?.[k]).filter((v) => v !== null && v !== undefined))]));

// ---- main ----
const all = new Map(); // key -> number, for the delta and the "All numbers" table
const put = (k, v) => { if (v !== null && v !== undefined && Number.isFinite(v)) all.set(k, Math.round(v * 1000) / 1000); };

async function main() {
  const t00 = Date.now();
  const load0 = loadavg();
  buildSite();
  const C = join(SITE, "c");
  for (const f of ["index.html", "ccore/ccore.wasm", "ccore/ccore-view.wasm", "ccore/frame.json"])
    if (!existsSync(join(C, f))) throw new Error(`the site has no c/${f}`);
  if (!existsSync(join(SITE, "index.html"))) writeFileSync(join(SITE, "index.html"), "<!doctype html><title>root</title>");
  let large = null;
  if (LARGE) {
    const file = join(RAW, `large-${LARGE}.bimble`);
    const info = JSON.parse(execFileSync("python3", [join(CCORE, "bench/large_model.py"), String(LARGE), file], { encoding: "utf8" }));
    mkdirSync(join(C, "perf"), { recursive: true });
    copyFileSync(file, join(C, `perf/large-${LARGE}.json`)); // .json: served (and gzipped) as Pages serves frame.json
    large = { ...info, bytes: statSync(file).size, gz: gzipSync(readFileSync(file)).length };
  }
  await serve();

  const ch = await chromium("chromium");
  cdp = await cdpConnect(ch.ws);
  const ver = await cdp.send("Browser.getVersion");
  let gpu = "?";
  try {
    const info = await cdp.send("SystemInfo.getInfo");
    const dev = info.gpu?.devices?.[0];
    gpu = `${info.gpu?.auxAttributes?.glRenderer ?? dev?.deviceString ?? "?"}`;
  } catch {}
  browserInfo = { product: ver.product, gpu };

  const pages = [
    { key: "editor", name: "editor (/c/)", url: `${ORIGIN}/c/`, plan: `${ORIGIN}/c/?view=plan:1` },
    { key: "viewer", name: "viewer-only (/c/?view=1)", url: `${ORIGIN}/c/?view=1`, plan: `${ORIGIN}/c/?ccore_wasm=ccore/ccore-view.wasm&view=plan:1` },
  ];
  const remote = [];
  if (REMOTE) {
    for (const [name, url] of DEPLOYED) {
      try {
        const r = await fetch(url, { signal: AbortSignal.timeout(8000) });
        if (r.ok) remote.push({ key: name.includes("Rust") ? "deployed-rust" : "deployed-c", name: `${name} (${url})`, url, remote: true });
        else notes.push(`${url}: HTTP ${r.status}: skipped`);
      } catch (e) {
        notes.push(`${url}: not reachable (${e.cause?.code ?? e.name}): skipped (offline is not a failure)`);
      }
    }
  } else notes.push(FULL ? "deployed pages: --no-remote" : "deployed pages: not measured in the CI mode (no network; `--full` measures them)");

  // b. load marks. First the cold load: the first page of a fresh browser profile can pay the GPU
  // process and SwiftShader start in getContext("webgl2"); later loads (a new tab, the same browser)
  // do not. Reported apart; the medians below are warm loads.
  const cold = await measureLoad(pages[0].url, 60000);
  if (!cold.ok) problems.push(`${pages[0].name}: the cold load did not load: ${cold.why}`);
  else {
    for (const f of ["readyMs", "shownMs"]) put(`editor.sample.cold.${f.slice(0, -2)}`, cold[f]);
    put("editor.sample.cold.drawn", cold.gl?.firstFrameMs);
    put("editor.sample.cold.longTotal", cold.longTotal);
    log(`editor sample, cold browser: drawn ${ms(cold.gl?.firstFrameMs)} ms, shown ${ms(cold.shownMs)} ms, long tasks ${cold.longCount} (${ms(cold.longTotal)} ms)`);
  }
  const models = [{ key: "sample", name: "frame.json (sample)", q: "" }];
  if (large) models.push({ key: "large", name: `large-${LARGE} (synthetic, ${fmtN(large.elements)} elements)`, q: `model=perf/large-${LARGE}.json` });
  const loads = [];
  for (const p of [...pages, ...remote]) {
    for (const m of models) {
      if (p.remote && m.key !== "sample") continue;
      const url = m.q ? `${p.url}${p.url.includes("?") ? "&" : "?"}${m.q}` : p.url;
      const runs = [];
      for (let i = 0; i < LOADS; i++) {
        const r = await measureLoad(url, m.key === "large" ? 180000 : 60000);
        runs.push(r);
        if (!r.ok) break;
      }
      const bad = runs.find((r) => !r.ok);
      if (bad) {
        (p.remote ? notes : problems).push(`${p.name}, ${m.name}: did not load: ${bad.why}`);
        loads.push({ page: p, model: m, ok: false, why: bad.why });
        continue;
      }
      const M = (f) => median(runs.map(f));
      const row = { page: p, model: m, ok: true, n: runs.length,
        wasm: M((r) => r.cc?.readyMs ?? r.measures["bimble:wasm-ready"]), open: M((r) => r.cc?.openMs ?? null),
        drawn: M((r) => r.gl?.firstFrameMs ?? null), shown: M((r) => r.shownMs), ready: M((r) => r.readyMs),
        shownRust: M((r) => r.measures["bimble:model-shown"] ?? null),
        fcp: M((r) => r.fcp), lcp: M((r) => r.lcp), wasmEnd: M((r) => r.wasmEnd), modelEnd: M((r) => r.modelEnd),
        longCount: M((r) => r.longCount), longTotal: M((r) => r.longTotal), longMax: M((r) => r.longMax),
        heap: M((r) => r.heap), wasmMem: M((r) => r.wasmMem), tris: M((r) => r.gl?.drawnTris ?? null),
        wasmTransfer: runs[0].wasmTransfer, modelTransfer: runs[0].modelTransfer, wasmBytes: runs[0].cc?.wasmBytes ?? null,
        modelBytes: runs[0].cc?.modelBytes ?? null, meshBytes: runs[0].gl?.meshBytes ?? null, longTop: runs[runs.length >> 1].longTop };
      loads.push(row);
      const k = `${p.key}.${m.key}.load`;
      for (const f of ["wasm", "open", "drawn", "shown", "ready", "fcp", "longCount", "longTotal", "heap", "wasmMem", "tris"]) put(`${k}.${f}`, row[f]);
      log(`${p.key} ${m.key}: wasm ${ms(row.wasm)} ms, model ${ms(row.open)} ms, drawn ${ms(row.drawn)} ms, shown ${ms(row.shown)} ms, ready ${ms(row.ready)} ms, long tasks ${row.longCount} (${ms(row.longTotal)} ms)`);
    }
  }

  // c. frame time while orbiting
  const orbits = [];
  for (const p of FULL ? pages : pages.slice(0, 1)) { // CI: the editor only (the viewer draws with the same glviewport.ts)
    for (const m of models) {
      for (const view of ["3d", "plan"]) {
        const base = view === "plan" ? p.plan : p.url;
        const url = m.q ? `${base}${base.includes("?") ? "&" : "?"}${m.q}` : base;
        const r = await measureOrbit(url, m.key === "large" ? 180000 : 60000, view === "plan");
        if (!r.ok) { problems.push(`${p.name}, ${m.name}, ${view}: orbit: ${r.why}`); continue; }
        if (!r.moved || r.misses || (view === "plan" && r.mode !== "plan")) notes.push(`${p.key} ${m.key} ${view}: the drag ${r.moved ? "moved" : "did NOT move"} the camera, ${r.misses} steps without a new frame, camera mode ${r.mode}`);
        orbits.push({ page: p, model: m, view, ...r });
        const k = `${p.key}.${m.key}.${view}`;
        for (const f of ["dMed", "dP95", "dMax", "latMed", "latP95", "tris"]) put(`${k}.${f}`, r[f]);
        log(`${p.key} ${m.key} ${view}: ${fmtN(r.tris)} triangles, frame time median ${ms(r.dMed)} p95 ${ms(r.dP95)} max ${ms(r.dMax)} ms, step ${ms(r.latMed)} ms`);
      }
    }
  }
  cdp.close();

  // a. Lighthouse
  const lhRows = [];
  LH = await loadLighthouse().catch((e) => { notes.push(`lighthouse: failed to load (${e.message})`); return null; });
  if (argv.includes("--no-lighthouse")) { LH = null; notes.push("lighthouse: --no-lighthouse"); }
  else if (!LH) notes.push("lighthouse: not installed (web-ui devDependency missing: npm ci in web-ui/): Lighthouse numbers skipped, the CDP numbers above stand");
  if (LH) {
    lhChrome = await chromium("chromium-lh");
    // Warm-up (untimed), so the first run does not pay the GPU process start (see the cold load):
    // A tab opened over CDP first is not enough (measured: the next Lighthouse run still had the
    // ~1 s task); an untimed Lighthouse run of the viewer page is.
    await lighthouseRun(pages[1].url, "desktop", "warm-up").catch((e) => notes.push(`lighthouse warm-up: ${e.message}`));
    for (const p of [...pages, ...remote]) {
      for (const preset of ["desktop", "mobile"]) {
        const runs = [];
        for (let i = 0; i < LH_RUNS; i++) {
          try {
            const r = await lighthouseRun(p.url, preset, `${p.key}-${preset}-${i}`);
            if (r.error) notes.push(`lighthouse ${p.key} ${preset} run ${i}: ${r.error}`);
            runs.push(r);
          } catch (e) {
            notes.push(`lighthouse ${p.key} ${preset} run ${i}: ${e.message}`);
          }
        }
        if (!runs.length) continue;
        const row = { page: p, preset, n: runs.length, ...medianOf(runs, LH_KEYS), flagged: runs[0].flagged, warnings: [...new Set(runs.flatMap((r) => r.warnings))] };
        lhRows.push(row);
        for (const f of LH_KEYS) put(`${p.key}.lh.${preset}.${f}`, row[f]);
        log(`lighthouse ${p.key} ${preset}: performance ${fmtN(row.perf)}, FCP ${ms(row.fcp)} ms, LCP ${ms(row.lcp)} ms, TBT ${ms(row.tbt)} ms, ${kb(row.bytes)}`);
      }
    }
  }
  const load1 = loadavg();

  // ---- the page ----
  const rev = (() => { try { return execFileSync("git", ["rev-parse", "--short", "HEAD"], { cwd: CCORE, encoding: "utf8" }).trim(); } catch { return "?"; } })();
  const cpu = cpus();
  const L = [];
  L.push("# Performance of the C lab (ticket PERF1)", "");
  L.push(`Generated by \`node ccore/tools/perf.mjs${FULL ? " --full" : ""}\` (\`ccore/ci/run.sh lab|full\` runs the short mode and prints the change).`);
  L.push("Do not edit: run the tool. Method, and the exact definition of each mark, in the tool's header.", "");
  L.push("## Measuring conditions", "");
  L.push(`- ${new Date().toISOString().slice(0, 10)}, commit ${rev}, mode ${FULL ? `full (${LH_RUNS} Lighthouse runs, ${LOADS} loads, large model)` : `CI (${LH_RUNS} Lighthouse run, ${LOADS} loads, sample model only)`}, ${Math.round((Date.now() - t00) / 1000)} s.`);
  L.push(`- Machine: ${platform()} ${release().split("-")[0]} ${arch()}, ${cpu.length} CPUs (${cpu[0]?.model && cpu[0].model !== "unknown" ? cpu[0].model : "model not reported"}), ${(totalmem() / 2 ** 30).toFixed(0)} GiB; load average at start ${load0.map((x) => x.toFixed(2)).join(" / ")}, at end ${load1.map((x) => x.toFixed(2)).join(" / ")} (1 / 5 / 15 min).`);
  L.push(`- Browser: ${browserInfo.product}, headless, \`--use-angle=swiftshader\`; GL renderer: ${browserInfo.gpu}. Node ${process.version}. Lighthouse ${LH ? LH.version : "not run"}.`);
  L.push("- **SwiftShader is software rendering on the CPU.** The frame times below are NOT what a user's GPU gives: compare them across runs and machines through the triangles drawn per frame, not as user-facing fps.");
  L.push("- Served: the production build (`npm run build:ccore`) under `/c/` by a local static server that answers as GitHub Pages does (gzip for html, js, css, wasm, json; `Cache-Control: max-age=600`); every CDP load starts with the HTTP cache emptied (`Network.clearBrowserCache`). Viewport 1280x800 for the CDP numbers; Lighthouse uses its own presets.");
  if (large) L.push(`- Large model: \`bench/large_model.py ${LARGE}\` (synthetic): ${fmtN(large.elements)} elements (${Object.entries(large.by_class).map(([k, v]) => `${fmtN(v)} ${k}`).join(", ")}), ${mb(large.bytes)} (${mb(large.gz)} gzip), served as \`perf/large-${LARGE}.json\`, opened with \`?model=\`.`);
  L.push("");

  L.push("## Summary", "");
  L.push("Lighthouse: performance score desktop / mobile (median); our marks in ms from navigation start, sample model, median of the loads; orbit: frame time median in 3D, sample model.", "");
  L.push("| page | LH perf (desktop / mobile) | a11y | best practices | LCP desktop / mobile (ms) | TBT desktop / mobile (ms) | wasm instantiated | model opened | first model frame shown | ready | orbit frame (ms) |");
  L.push("|---|---|---|---|---|---|---:|---:|---:|---:|---:|");
  for (const p of [...pages, ...remote]) {
    const d = lhRows.find((r) => r.page === p && r.preset === "desktop"), mo = lhRows.find((r) => r.page === p && r.preset === "mobile");
    const ld = loads.find((r) => r.page === p && r.model.key === "sample" && r.ok);
    const ob = orbits.find((r) => r.page === p && r.model.key === "sample" && r.view === "3d");
    L.push(`| ${p.name} | ${fmtN(d?.perf)} / ${fmtN(mo?.perf)} | ${fmtN(d?.a11y)} | ${fmtN(d?.bp)} | ${ms(d?.lcp)} / ${ms(mo?.lcp)} | ${ms(d?.tbt)} / ${ms(mo?.tbt)} | ${ms(ld?.wasm)} | ${ms(ld?.open)} | ${ms(ld?.shown ?? ld?.shownRust)} | ${ms(ld?.ready)} | ${ms(ob?.dMed)} |`);
  }
  L.push("");

  L.push(`## Lighthouse (median of ${LH_RUNS} run${LH_RUNS > 1 ? "s" : ""} per page and preset)`, "");
  if (!lhRows.length) L.push("Not run: see the notes.", "");
  else {
    L.push("desktop: Lighthouse's desktop preset (simulated 40 ms RTT, 10 Mbit/s, no CPU slowdown); mobile: the default (simulated slow 4G, 4x CPU slowdown, a phone screen). Times in ms, bytes as transferred.", "");
    L.push("| page | preset | performance | a11y | best practices | FCP | LCP | TBT | CLS | Speed Index | transfer | JS | unused JS (estimate) |");
    L.push("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|");
    for (const r of lhRows) L.push(`| ${r.page.name} | ${r.preset} | ${fmtN(r.perf)} | ${fmtN(r.a11y)} | ${fmtN(r.bp)} | ${ms(r.fcp)} | ${ms(r.lcp)} | ${ms(r.tbt)} | ${r.cls === null ? "-" : r.cls.toFixed(3)} | ${ms(r.si)} | ${kb(r.bytes)} | ${kb(r.js)} | ${kb(r.unusedJs)} |`);
    L.push("", "Audits below 0.9 (the first run of each page and preset; raw reports in ccore/out/perf1/lh-*.json, not committed):", "");
    for (const r of lhRows) L.push(`- ${r.page.key} ${r.preset}: ${r.flagged.length ? r.flagged.join("; ") : "none"}${r.warnings.length ? `. Warnings: ${r.warnings.join(" | ")}` : ""}`);
    L.push("");
  }

  L.push(`## Load marks (CDP, median of ${LOADS} loads)`, "");
  L.push("ms from navigation start. wasm = `WebAssembly.instantiate` returned; model = `host_open_text` returned; drawn = the first `draw()` with a mesh ended; shown = the next animation frame after it; ready = `data-bb-ready`. Long tasks: > 50 ms, navigation to ready + 1 s (count, total and max: each the median of its own). Heap: JS heap used after a GC. Rust lab: `bimble:model-shown` (its own mark) in the shown column.", "");
  L.push("| page | model | wasm fetched | wasm instantiated | model fetched | model opened | drawn | shown | ready | FCP | long tasks (n / total / max) | JS heap | wasm memory | triangles | wasm / model / mesh bytes |");
  L.push("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|---|");
  for (const r of loads) {
    if (!r.ok) { L.push(`| ${r.page.name} | ${r.model.name} | did not load: ${r.why} |||||||||||||`); continue; }
    L.push(`| ${r.page.name} | ${r.model.name} | ${ms(r.wasmEnd)} | ${ms(r.wasm)} | ${ms(r.modelEnd)} | ${ms(r.open)} | ${ms(r.drawn)} | ${ms(r.shown ?? r.shownRust)} | ${ms(r.ready)} | ${ms(r.fcp)} | ${fmtN(r.longCount)} / ${ms(r.longTotal)} / ${ms(r.longMax)} | ${mb(r.heap)} | ${mb(r.wasmMem)} | ${fmtN(r.tris)} | ${fmtN(r.wasmBytes)} / ${fmtN(r.modelBytes)} / ${fmtN(r.meshBytes)} |`);
  }
  if (cold.ok) L.push("", `Cold browser (the first load of a fresh profile, editor, sample; not in the medians): drawn ${ms(cold.gl?.firstFrameMs)} ms, shown ${ms(cold.shownMs)} ms, ready ${ms(cold.readyMs)} ms, long tasks ${cold.longCount} / ${ms(cold.longTotal)} ms. When it is far above the warm loads, the difference is the GPU process and SwiftShader starting inside \`getContext("webgl2")\` (measured 0.8-0.9 s for that one call in a fresh headless browser while developing PERF1; it varies with the machine's load).`);
  L.push("", "The longest tasks of the median load (start ms, duration ms):", "");
  for (const r of loads.filter((x) => x.ok)) L.push(`- ${r.page.key} ${r.model.key}: ${r.longTop.length ? r.longTop.map(([s, d]) => `${ms(s)} (${ms(d)})`).join(", ") : "none"}`);
  L.push("");

  L.push(`## Frame time while orbiting (${STEPS} drag steps of 3 px, one frame each)`, "");
  L.push("Frame time = the requestAnimationFrame interval in which glviewport drew (the frame a user waits for; at least the display interval, 16.7 ms); every rAF interval during the drag is also given (idle frames between steps included). Step = dispatch of the mouse event -> the next glviewport frame drawn (Node clock, includes the CDP round trip). Plan: the drag pans (a plan has no orbit).", "");
  L.push("| page | model | view | triangles drawn | frames drawn | frame time median | p95 | max | all rAF intervals median | step median | step p95 |");
  L.push("|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|");
  for (const r of orbits) L.push(`| ${r.page.name} | ${r.model.name} | ${r.view === "3d" ? "3D perspective, orbit" : "plan, pan"} | ${fmtN(r.tris)} | ${fmtN(r.frames)} | ${ms(r.dMed)} | ${ms(r.dP95)} | ${ms(r.dMax)} | ${ms(r.allMed)} | ${ms(r.latMed)} | ${ms(r.latP95)} |`);
  L.push("");
  if (notes.length || problems.length) {
    L.push("## Notes", "");
    for (const n of [...problems.map((p) => `a local page failed: ${p}`), ...notes]) L.push(`- ${n}`);
    L.push("");
  }
  L.push("## All numbers", "", "The keys the delta reads (ms, bytes, counts, scores).", "", "| key | value |", "|---|---:|");
  for (const [k, v] of [...all.entries()].sort((a, b) => (a[0] < b[0] ? -1 : 1))) L.push(`| ${k} | ${v} |`);
  const page = L.join("\n") + "\n";

  // ---- the change against the base ----
  const parse = (txt) => new Map([...txt.matchAll(/^\| ([a-z][\w.-]+) \| (-?[\d.]+) \|$/gm)].map((m) => [m[1], Number(m[2])]));
  const old = existsSync(BASE) ? parse(readFileSync(BASE, "utf8")) : new Map();
  let changed = 0;
  for (const [k, v] of all) {
    const o = old.get(k);
    if (o === undefined) continue;
    const rel = o ? (v - o) / Math.abs(o) : v ? 1 : 0;
    if (Math.abs(rel) >= 0.1 && Math.abs(v - o) >= 1) { changed++; log(`${k}: ${o} -> ${v} (${rel > 0 ? "+" : ""}${(100 * rel).toFixed(0)} %)`); }
  }
  const fresh = [...all.keys()].filter((k) => !old.has(k)).length;
  log(`${changed} of ${all.size} numbers changed by 10 % or more against ${relative(process.cwd(), BASE)}${fresh ? `, ${fresh} not in it` : ""} (a slower number never fails: runner noise)`);
  mkdirSync(dirname(OUT), { recursive: true });
  writeFileSync(OUT, page);
  log(`wrote ${relative(process.cwd(), OUT)}`);
  for (const n of notes) log(`note: ${n}`);
}

let crashed = null;
try {
  await main();
} catch (e) {
  crashed = e;
}
await cleanup();
if (crashed) {
  console.log(`FAIL perf: the tool crashed: ${crashed.stack ?? crashed}`);
  for (const p of procs) console.log(`--- ${p.label} output ---\n${p.out.slice(-2000)}`);
  process.exit(1);
}
if (problems.length) {
  for (const p of problems) console.log(`FAIL perf: ${p}`);
  process.exit(1);
}
process.exit(0);
