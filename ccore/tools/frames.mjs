// 3D speed of the C lab (tickets 3D1, 3D2, ADR 0078):
//   node ccore/tools/frames.mjs [--force] [--label NAME] [--builds wasm,native] [--models large,heavy]
//     [--frames 120] [--site DIR] [--pixels DIR] [--compare DIR] [--out FILE] [--no-history]
// Writes ccore/results/frames.md (never edit it by hand) and the raw numbers to out/frames/frames.json.
//
// ONE headless Chromium (SwiftShader: WebGL2 drawn on the CPU, 1280x800; LAB_GPU=gl|vulkan: the machine's
// GPU, test/lib/chromium.mjs, REAL1: the renderer string is logged and checked) opens the lab as tools/ux.mjs
// does: the production build of the C lab with out/wasm/ccore.wasm (the core in the page), and the same
// build against the native host (out/tcc/bimble-ccore, HOME and BIMBLE_CONFIG_DIR in a temporary
// folder). SwiftShader's numbers compare before and after on this machine; they are not what a GPU gives.
// Models: "large", the 20,026-element synthetic model of bench/large_model.py (results/large.md), and
// "heavy", the mesh-heavy synthetic model of bench/mesh_heavy_model.py (the frame plus 700 proxies of
// about 1,000 triangles: 772 elements). FRAMES_EXTRA_MODEL=<file> adds "local", a file of your own: its
// name and path are never written, and the report then goes to out/frames/frames-local.md (never
// committed) unless --out.
// Per build and model, on a freshly loaded page:
//   open -> first frame    __bbGl.firstFrameMs: navigation start to the first frame drawn with the mesh
//                          (wasm: the module, the model's parse, the scene and the mesh in the page;
//                          native: the host has the model open already, the page fetches /mesh).
//   orbit                  --frames (120) frames of a 360-degree orbit about the target, each drawn at
//                          once as a moving frame (__bbGl.bench.frame(3 deg, moving)) and finished (a 1x1
//                          readPixels waits for the pixels): median and p95 ms, triangles and draw calls
//                          of the last moving frame.
//   still                  then 5 frames with the camera still (the full picture): median ms, triangles,
//                          draw calls.
//   GPU bytes              what the model's GPU buffers hold (__bbGl.gpuBytes).
//   JS heap, wasm memory   Runtime.getHeapUsage (used) after a garbage collection; the core module's
//                          memory (wasm build).
//   orbit input -> frame   20 left-drag moves of 8 px by CDP input: from the pointermove's timeStamp to
//                          the end of the animation-frame callbacks of the frame that drew it (ADR 0074),
//                          median and p95.
//   pick click -> frame    5 clicks on the model (CDP input): from the pointerdown's timeStamp to the
//                          frame that drew the selection (the last frame before 300 ms without one).
// The 1-minute load (/proc/loadavg) is recorded at each model's start; over 4 before the run starts
// (waited for up to 10 minutes) nothing is measured unless --force (then the report is marked). The
// run's own SwiftShader threads raise it during the run. The machine waits for 3 GB of available memory before it starts the browser.
// --pixels DIR saves the picture of three fixed cameras per model (wasm build) as raw RGBA; --compare
// DIR compares them with an earlier --pixels run (a pixel counts when a channel differs by more than
// 40 levels, the threshold of Rust's culling_changes_nothing_on_screen) and fails over 0.1 % of pixels.
// --no-wait: do not wait for the load to fall under 4 (with --force: the report is marked as forced).
// --label NAME: the run's name in the report's history (one table row per label, kept from the
// committed report: before and after each step of ticket 3D1).
// --renderer webgl2|webgpu (GPU1, ADR 0083): webgpu opens the page with ?gpu=webgpu&gpuSoftware=1 in a
// Chromium with a WebGPU adapter (Dawn's Vulkan on SwiftShader; LAB_GPU=vulkan: the machine's GPU, no ?gpuSoftware=1), each frame finished by the
// queue's onSubmittedWorkDone instead of a 1x1 readPixels, the pictures read from the renderer's
// target; occlusion queries are WebGL2's only (not measured). The report goes to
// out/frames/frames-webgpu.md unless --out. It fails when the page does not draw with WebGPU.
// --links N (ticket LNK3D, ADR 0078 amendment 3): instead of the models, N synthetic links (bench/links_synth.mjs:
// mesh_heavy_model.py's model, --link-proxies (160) unique closed meshes of about --link-tris (600) triangles each, exported as
// IFC4 through the native host, linked as a folder into a new project, placed in rows of 5, 30 m apart), the
// native build only: open -> first picture (the links' proxies, or the first frame) and complete picture, the
// still and moving frames (triangles, draws), GPU bytes; then half the links hidden and the GPU budget (a page
// hook) set to half: GPU bytes with the budget. Printed and written to out/frames/links-<label>.json, never to
// frames.md. --pixels DIR / --compare DIR as above (three cameras of the links). FRAMES_UI=<web-ui dir> and
// HOST_BIN measure another tree (the code before a change).
import { spawn, execFileSync } from "node:child_process";
import { createServer } from "node:http";
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, readdirSync, readFileSync, rmSync, statSync, symlinkSync, writeFileSync } from "node:fs";
import { arch, cpus, loadavg, platform, release, tmpdir, totalmem } from "node:os";
import { sumSections } from "./cache_sections.mjs";
import { dirname, extname, join, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";
import { gzipSync } from "node:zlib";
import { port } from "../test/ports.mjs";
import { CHROMIUM, LAB_GPU, checkGpu, gpuFlags, rendererInfo, webgpuFlags } from "../test/lib/chromium.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = process.env.FRAMES_UI ? resolve(process.env.FRAMES_UI) : resolve(CCORE, "../web-ui");
const argv = process.argv.slice(2);
const opt = (n) => { const i = argv.indexOf(n); return i >= 0 ? argv[i + 1] : null; };
const flag = (n) => argv.includes(n);
const FORCE = flag("--force");
const LABEL = opt("--label") ?? "run";
const RENDERER = opt("--renderer") ?? "webgl2"; // GPU1
if (RENDERER !== "webgl2" && RENDERER !== "webgpu") throw new Error(`--renderer: webgl2 or webgpu, not ${RENDERER}`);
const GPU = RENDERER === "webgpu";
const BUILDS = (opt("--builds") ?? "wasm,native").split(",");
const EXTRA = process.env.FRAMES_EXTRA_MODEL ? resolve(process.env.FRAMES_EXTRA_MODEL) : null;
const LINKS = opt("--links") ? Number(opt("--links")) : 0;
const MODELS = LINKS ? ["links"] : (opt("--models") ?? (EXTRA ? "large,heavy,local" : "large,heavy")).split(",");
const NFRAMES = Number(opt("--frames") ?? 120);
const PIXELS = opt("--pixels") ? resolve(opt("--pixels")) : null;
const COMPARE = opt("--compare") ? resolve(opt("--compare")) : null;
const HEAVY = 700, HEAVY_TRIS = 1000, LARGE_N = 20000;
// REAL1: a run on the machine's GPU (LAB_GPU=gl|vulkan, test/lib/chromium.mjs) writes only the section "On the machine's
// GPU" of results/frames.md (one table per renderer, model and build; --links N rows), the SwiftShader tables kept; a
// SwiftShader run keeps that section as it is. The two are never mixed in one table.
const REAL = LAB_GPU !== "software";
const OUT = resolve(opt("--out") ?? (EXTRA ? join(CCORE, "out/frames/frames-local.md") : REAL ? join(CCORE, "results/frames.md") : GPU ? join(CCORE, "out/frames/frames-webgpu.md") : join(CCORE, "results/frames.md")));
const RAW = join(CCORE, "out/frames");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const WASM = join(CCORE, "out/wasm/ccore.wasm");
const FIXTURE = join(CCORE, "golden/frame.json");
const QUIET = 300;
const tmp = mkdtempSync(join(tmpdir(), "ccore-frames-"));
const HOME = join(tmp, "home"), CONFIG = join(tmp, "config"), FILES = join(tmp, "files");
for (const d of [RAW, HOME, CONFIG, FILES]) mkdirSync(d, { recursive: true });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const log = (s) => console.log(`frames: ${s}`);
const procs = [];
let server = null;

const loadNow = () => readFileSync("/proc/loadavg", "utf8").trim();
const load1 = () => Number(loadNow().split(" ")[0]);
const memGb = () => Number(/MemAvailable:\s+(\d+)/.exec(readFileSync("/proc/meminfo", "utf8"))[1]) / 1048576;
async function waitMemory() {
  for (let i = 0; ; i++) {
    const gb = memGb();
    if (gb >= 3) return gb;
    if (i % 12 === 0) log(`waiting: ${gb.toFixed(1)} GB available, under 3 GB`);
    if (i > 120) throw new Error("memory: under 3 GB available for 10 minutes");
    await sleep(5000);
  }
}

function start(name, cmd, args, opts = {}) {
  const p = spawn(cmd, args, { stdio: ["ignore", "pipe", "pipe"], detached: true, ...opts });
  p.out = "";
  const keep = (d) => { p.out += d; if (p.out.length > 200000) p.out = p.out.slice(-100000); };
  p.stdout.on("data", keep);
  p.stderr.on("data", keep);
  p.label = name;
  procs.push(p);
  return p;
}
async function stop(p) {
  for (const sig of ["SIGTERM", "SIGKILL"]) {
    if (p.exitCode !== null || p.signalCode !== null) return;
    try { process.kill(-p.pid, sig); } catch { /* gone */ }
    for (let i = 0; i < 20 && p.exitCode === null && p.signalCode === null; i++) await sleep(50);
  }
}
async function cleanup() {
  if (server) server.close();
  for (const p of procs) await stop(p);
  rmSync(tmp, { recursive: true, force: true });
}
// LNK3D: the temporary folder (models, links, their caches, Chromium's profile: GBs in /tmp, which is memory
// here) goes on every exit: a signal (timeout, Ctrl-C) too, and a crash; the children are killed first
let cleaning = false;
const onSignal = (sig) => { if (cleaning) return; cleaning = true; log(`${sig}: cleaning up`); cleanup().finally(() => process.exit(130)); };
process.on("SIGINT", onSignal), process.on("SIGTERM", onSignal), process.on("SIGHUP", onSignal);
process.on("exit", () => {
  for (const p of procs) if (p.exitCode === null && p.signalCode === null) try { process.kill(-p.pid, "SIGKILL"); } catch { /* gone */ }
  try { rmSync(tmp, { recursive: true, force: true }); } catch { /* best effort */ }
});
async function waitFor(what, ms, fn, every = 100) {
  const until = Date.now() + ms;
  let last;
  while (Date.now() < until) {
    try { const v = await fn(); if (v) return v; } catch (e) { last = e; }
    await sleep(every);
  }
  throw new Error(`timeout waiting for ${what}${last ? `: ${last.message ?? last}` : ""}`);
}

// ---- the site (tools/ux.mjs: the production build, served gzipped as GitHub Pages does) ----
const SITE_PORT = port("frames");
const ORIGIN = `http://localhost:${SITE_PORT}`;
const TYPES = { ".html": "text/html; charset=utf-8", ".js": "text/javascript", ".css": "text/css", ".wasm": "application/wasm",
  ".json": "application/json", ".bimble": "application/octet-stream", ".woff2": "font/woff2", ".svg": "image/svg+xml", ".png": "image/png", ".ico": "image/x-icon" };
const GZIP = new Set([".html", ".js", ".css", ".wasm", ".json", ".svg", ".bimble"]);
function serve(site) {
  const gz = new Map();
  server = createServer((req, res) => {
    const path = decodeURIComponent(new URL(req.url, ORIGIN).pathname);
    let file = resolve(site, "." + path);
    if (file !== site && !file.startsWith(site + sep)) return res.writeHead(404).end();
    if (existsSync(file) && statSync(file).isDirectory()) file = join(file, "index.html");
    if (!existsSync(file)) return res.writeHead(404).end("not found");
    const ext = extname(file), headers = { "Content-Type": TYPES[ext] ?? "application/octet-stream", "Cache-Control": "max-age=600" };
    let body;
    if (GZIP.has(ext) && /\bgzip\b/.test(req.headers["accept-encoding"] ?? "")) {
      if (!gz.has(file)) gz.set(file, gzipSync(readFileSync(file)));
      body = gz.get(file);
      headers["Content-Encoding"] = "gzip";
    } else body = readFileSync(file);
    res.writeHead(200, headers).end(body);
  });
  return new Promise((r) => server.listen(SITE_PORT, "127.0.0.1", r));
}
function buildSite() {
  if (opt("--site")) return resolve(opt("--site"));
  if (!LINKS && !FOLDER && !existsSync(WASM)) throw new Error(`no ${WASM}: ccore/build.sh wasm`); // (--links: the native build only)
  const site = join(tmp, "site"), t0 = Date.now();
  execFileSync("npx", ["vite", "build", "--base", "./", "--outDir", site, "--emptyOutDir", "--logLevel", "warn"],
    { cwd: UI, stdio: ["ignore", "pipe", "pipe"], env: { ...process.env, VITE_CCORE: "1", VITE_CCORE_REV: "" } });
  rmSync(join(site, "ccore"), { recursive: true, force: true });
  mkdirSync(join(site, "ccore"));
  if (existsSync(WASM)) copyFileSync(WASM, join(site, "ccore/ccore.wasm"));
  copyFileSync(FIXTURE, join(site, "ccore/frame.json"));
  log(`vite build ${Date.now() - t0} ms`);
  return site;
}
const MODEL_FILE = {
  large: () => {
    const f = join(RAW, `large-${LARGE_N}.bimble`);
    if (!existsSync(f)) execFileSync("python3", [join(CCORE, "bench/large_model.py"), String(LARGE_N), f], { stdio: ["ignore", "pipe", "pipe"] });
    return f;
  },
  heavy: () => {
    const f = join(RAW, `heavy-${HEAVY}-${HEAVY_TRIS}.json`);
    if (!existsSync(f)) execFileSync("python3", [join(CCORE, "bench/mesh_heavy_model.py"), String(HEAVY), String(HEAVY_TRIS), f], { stdio: ["ignore", "pipe", "pipe"] });
    return f;
  },
  local: () => { if (!EXTRA || !existsSync(EXTRA)) throw new Error("FRAMES_EXTRA_MODEL: no such file"); return EXTRA; },
  fixture: () => FIXTURE, // REAL1: golden/frame.json, 72 elements
};
const siteName = (model) => `ccore/${model}${extname(MODEL_FILE[model]())}`;
const MODEL_NAME = { large: "20,026 elements (synthetic, large.md)", heavy: "772 elements, mesh-heavy (synthetic)", local: "a local imported model", fixture: "72 elements (golden/frame.json)" };

// ---- the native host ----
const HOST_PORT = port("frames_host");
const TOKEN = "frames-token";
let host = null;
async function startHost(model) {
  if (host) await stop(host);
  const src = MODEL_FILE[model](), copy = join(FILES, `${model}${extname(src)}`);
  copyFileSync(src, copy);
  for (let attempt = 0; ; attempt++) {
    host = start("host", BIN, [String(HOST_PORT), copy], { cwd: FILES, env: { ...process.env, HOME, BIMBLE_CONFIG_DIR: CONFIG, BIMBLE_TOKEN: TOKEN, BIMBLE_ORIGINS: ORIGIN } });
    try {
      await waitFor("the host", 60000, async () => {
        if (host.exitCode !== null) throw new Error(host.out.slice(-400));
        return (await fetch(`http://127.0.0.1:${HOST_PORT}/query/state?t=${TOKEN}`, { method: "POST", body: "{}" })).ok;
      }, 30);
      return;
    } catch (e) {
      if (attempt >= 3) throw e;
      await stop(host);
      await sleep(300);
    }
  }
}

// ---- CDP ----
async function cdpConnect(wsUrl) {
  const ws = new WebSocket(wsUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = () => rej(new Error("CDP connect failed")); });
  let id = 0;
  const pending = new Map();
  // REAL1: a browser gone (the folder's watchdog kills it) fails the calls waiting, never hangs them
  ws.onclose = () => { for (const { reject } of pending.values()) reject(new Error("CDP: the browser is gone")); pending.clear(); };
  ws.onmessage = (ev) => {
    const msg = JSON.parse(ev.data);
    if (msg.id !== undefined && pending.has(msg.id)) {
      const { ok, reject } = pending.get(msg.id);
      pending.delete(msg.id);
      if (msg.error) reject(new Error(`${msg.error.message} ${msg.error.data ?? ""}`));
      else ok(msg.result);
    }
  };
  const send = (method, params = {}, sessionId) => new Promise((ok, reject) => {
    if (ws.readyState > 1) return reject(new Error("CDP: the browser is gone")); // REAL1 (closing or closed)
    const m = ++id;
    pending.set(m, { ok, reject });
    ws.send(JSON.stringify({ id: m, method, params, ...(sessionId ? { sessionId } : {}) }));
  });
  return { send, close: () => ws.close() };
}

// The page's instruments, before any of its scripts: input time stamps, the frames drawn (end of the
// animation-frame callbacks), the core module's memory.
const INSTR = String.raw`(() => {
  if (window.__fr) return;
  const fr = window.__fr = { tIn: null, kind: null, draws: [], moves: [], X: null, errors: [] };
  addEventListener("error", (e) => { if (fr.errors.length < 20) fr.errors.push(String(e.message)); }); // 3D2: the page's uncaught errors
  addEventListener("pointermove", (e) => { if (fr.kind === "move" && fr.tIn === null) fr.tIn = e.timeStamp; }, true);
  addEventListener("pointerdown", (e) => { if (fr.kind === "click" && fr.tIn === null) fr.tIn = e.timeStamp; }, true);
  addEventListener("pointermove", (e) => { if (fr.kind === "stream") fr.moves.push(e.timeStamp); }, true);
  let f = -1;
  const raf = window.requestAnimationFrame.bind(window);
  const tick = () => { const g = window.__bbGl; const n = g ? g.frames : -1; if (n !== f) { f = n; fr.draws.push(performance.now()); if (fr.draws.length > 4000) fr.draws.splice(0, 2000); } };
  window.requestAnimationFrame = (cb) => raf((ts) => { try { cb(ts); } finally { tick(); } });
  const inst0 = WebAssembly.instantiate;
  WebAssembly.instantiate = async function (src, imp) {
    const r = await inst0.call(this, src, imp);
    const X = r && r.instance && r.instance.exports;
    if (X && X.memory && !fr.X) fr.X = X;
    return r;
  };
})();`;

let cdp = null, S = null, browser = null, curTarget = null;
// Non-background pixels of the viewport's canvas as last drawn (preserveDrawingBuffer: what is shown).
const NONBG = `(() => { const c = document.getElementById("bimble"), gl = c.getContext("webgl2"), w = c.width, h = c.height, p = new Uint8Array(4 * w * h);
  gl.readPixels(0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, p); let n = 0;
  for (let i = 0; i < p.length; i += 4) if (Math.abs(p[i] - 247) + Math.abs(p[i + 1] - 247) + Math.abs(p[i + 2] - 245) > 12) n++; return n; })()`;
async function startBrowser() {
  const gb = await waitMemory();
  log(`memory available ${gb.toFixed(1)} GB; starting Chromium`);
  // REAL1: LAB_GPU (test/lib/chromium.mjs) picks SwiftShader (default), the machine's GL or its Vulkan
  browser = start("chromium", CHROMIUM, ["--headless=new", ...gpuFlags(), "--remote-debugging-port=0",
    `--user-data-dir=${join(tmp, "chromium")}`, "--no-first-run", "--no-default-browser-check", "--lang=en-US", "--window-size=1280,800",
    "--js-flags=--expose-gc", ...(GPU ? webgpuFlags() : []), // GPU1
    "about:blank"], { env: { ...process.env, HOME } });
  const wsUrl = await waitFor("DevTools line", 60000, () => browser.out.match(/DevTools listening on (ws:\/\/\S+)/)?.[1]);
  cdp = await cdpConnect(wsUrl);
}
async function openTab() {
  if (curTarget) await cdp.send("Target.closeTarget", { targetId: curTarget }).catch(() => {});
  await cdp.send("Storage.clearDataForOrigin", { origin: ORIGIN, storageTypes: "local_storage,indexeddb,cache_storage,service_workers" }).catch(() => {});
  const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank" });
  const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
  curTarget = targetId;
  S = (m, p) => Promise.race([cdp.send(m, p, sessionId), sleep(180000).then(() => { throw new Error(`CDP ${m}: no answer in 180 s`); })]);
  for (const d of ["Runtime", "Page"]) await S(`${d}.enable`);
  await S("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  await S("Page.addScriptToEvaluateOnNewDocument", { source: INSTR });
}
// GPU1: under --renderer webgpu the page's expressions finish a frame on the GPU's queue and read the
// renderer's pixels (window.__fr.F, .S, .N below), in async functions.
const GPU_HELPERS = `window.__fr.F = async (d, m) => { const t0 = performance.now(); window.__bbGl.bench.frame(d, m); await window.__bbGpu.done(); return performance.now() - t0; };
window.__fr.S = async (dy, dp) => { window.__bbGl.bench.snap(dy, dp); window.__bbGl.bench.snap(dy, dp); await window.__bbGpu.done(); return window.__bbGpu.pixels(); };
window.__fr.N = async () => { const p = await window.__bbGpu.pixels(); let n = 0; for (let i = 0; i < p.rgba.length; i += 4) if (Math.abs(p.rgba[i] - 247) + Math.abs(p.rgba[i + 1] - 247) + Math.abs(p.rgba[i + 2] - 245) > 12) n++; return n; };`;
const forGpu = (expr) => (expr === NONBG ? "window.__fr.N()" : expr.replace(/\(\(\) => \{/g, "(async () => {")
  .replace(/(?:window\.__bbGl\.bench|\bb)\.frame\(/g, "await window.__fr.F(").replace(/(?:window\.__bbGl\.bench|\bb)\.snap\(([^)]*)\)/g, "(await window.__fr.S($1))"));
async function ev(expr) {
  if (GPU) { expr = forGpu(expr); if (expr.includes("await ") && !expr.startsWith("(async")) expr = `(async () => (${expr}))()`; }
  const r = await S("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true });
  if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
  return r.result.value;
}

let BUILD = "wasm", MODEL = "large";
const pageUrl = () => (BUILD === "wasm" ? `${ORIGIN}/?model=${siteName(MODEL)}` : `${ORIGIN}/?ccore=0&native=1&gl=1&port=${HOST_PORT}&token=${TOKEN}`) + (GPU ? `&gpu=webgpu${LAB_GPU === "software" ? "&gpuSoftware=1" : ""}` : ""); // GPU1; REAL1: a real adapter needs no ?gpuSoftware=1
async function fresh() {
  if (BUILD === "native") await startHost(MODEL);
  await openTab();
  await S("Page.navigate", { url: pageUrl() });
  const ready = () => ev(`!!(document.querySelector('[data-bb-ready]') && window.__bbGl && window.__bbGl.firstFrameMs && (document.querySelector('.bb-app')?.getAttribute('data-bb-shell') || '').startsWith('model'))`);
  await waitFor("the page ready", 240000, ready, 50);
  if (GPU) { // GPU1: the WebGPU renderer must be the one drawing
    await waitFor("the renderer", 30000, () => ev("(window.__bbPerf?.snapshot().renderer.reason ?? '') !== 'starting…'"), 50);
    const r = await ev("window.__bbPerf.snapshot().renderer");
    if (r.using !== "WebGPU") throw new Error(`--renderer webgpu: ${r.using} draws (${r.reason})`);
    await S("Runtime.evaluate", { expression: GPU_HELPERS }); // (not through ev: its rewriting is for the measures)
  }
  await whichGpu();
  await quiet(30000);
}
/** REAL1 (spike NAT1): which GPU draws, from the page (WEBGL_debug_renderer_info; WebGPU's adapter when it
 *  draws), logged and kept for the report; a GPU asked by LAB_GPU that the page does not have stops the run. */
const GPU_SEEN = { webgl: null, webgpu: null };
async function whichGpu() {
  const r = checkGpu(await rendererInfo((e) => S("Runtime.evaluate", { expression: e, returnByValue: true }).then((x) => x.result.value)));
  const a = GPU ? (await ev("window.__bbPerf.snapshot().renderer.adapter")) : null;
  GPU_SEEN.webgl = r.renderer, GPU_SEEN.webgpu = a;
  log(`renderer (LAB_GPU=${LAB_GPU}): WebGL2 ${r.renderer}${GPU ? `; WebGPU adapter ${JSON.stringify(a)}` : ""}`);
}
/** Waits until no frame was drawn for QUIET ms; returns the last draw's time. */
async function quiet(maxMs) {
  const t0 = Date.now();
  for (;;) {
    const p = await ev("({ now: performance.now(), last: window.__fr.draws[window.__fr.draws.length - 1] ?? 0 })");
    if (p.now - p.last >= QUIET && Date.now() - t0 >= 120) return p.last;
    if (Date.now() - t0 > maxMs) return p.last;
    await sleep(30);
  }
}
const med = (a) => { const s = [...a].sort((x, y) => x - y); return s.length ? s[Math.floor((s.length - 1) / 2)] : null; };
const p95 = (a) => { const s = [...a].sort((x, y) => x - y); return s.length ? s[Math.min(s.length - 1, Math.ceil(0.95 * s.length) - 1)] : null; };

async function canvasCentre() {
  return ev(`(() => { const r = document.getElementById("bimble").getBoundingClientRect(); return [Math.round(r.left + r.width / 2), Math.round(r.top + r.height / 2)]; })()`);
}
const mouse = (type, x, y, extra = {}) => S("Input.dispatchMouseEvent", { type, x, y, button: "none", buttons: 0, ...extra });

async function measure() {
  const out = { build: BUILD, model: MODEL, load: loadNow() };
  await fresh();
  const g0 = await ev(`(() => { const g = window.__bbGl; return { first: g.firstFrameMs, tris: g.drawnTris, bytes: g.meshBytes, gpu: g.gpuBytes ?? null, elements: g.hiddenElements,
    meshAt: g.meshAt ?? null, full: g.fullFrameMs ?? null, fetch: g.meshFetchMs, ff: g.firstFrame ?? null, steps: g.loadSteps ?? null, preview: g.previewAt ?? null }; })()`);
  out.firstFrameMs = g0.first;
  // 3D2 (step 5): the mesh's bytes in the page → its first frame, and → the picture complete
  out.meshAt = g0.meshAt, out.fullFrameMs = g0.full, out.meshFetchMs = g0.fetch, out.firstFrame = g0.ff, out.loadSteps = g0.steps, out.previewAt = g0.preview;
  out.meshBytes = g0.bytes;
  out.gpuBytes = g0.gpu;
  out.trianglesShown = g0.tris;
  // 3D2: the HLOD proxies, asked in steps after the first frame: wait until they are uploaded (at most 5 min)
  const pr0 = Date.now();
  while ((await ev("window.__bbGl.proxiesPending === true")) && Date.now() - pr0 < 300000) await sleep(100);
  out.proxies = await ev("window.__bbGl.proxies ?? null");
  out.proxyWaitMs = Date.now() - pr0;
  await quiet(30000);
  // 3D2 (step 5): read after the fill and the proxies (a quiet page): the picture complete, the frames after the first
  const g1 = await ev(`(() => { const g = window.__bbGl; return { full: g.fullFrameMs ?? null, log: g.frameLog ?? null, loops: g.loops ?? null, errors: window.__fr.errors }; })()`);
  out.fullFrameMs = g1.full, out.frameLog = g1.log, out.loops = g1.loops, out.pageErrors = g1.errors;
  await S("HeapProfiler.collectGarbage").catch(() => {});
  const heap = await S("Runtime.getHeapUsage");
  out.jsHeap = heap.usedSize;
  out.arrayBuffers = heap.backingStorageSize ?? null;
  out.wasmMemory = await ev(`window.__fr.X ? window.__fr.X.memory.buffer.byteLength : null`);
  // Orbit: NFRAMES frames over 360 degrees, each a moving frame, finished.
  const orbit = await ev(`(() => { const b = window.__bbGl.bench, ms = []; const d = 2 * Math.PI / ${NFRAMES};
    for (let i = 0; i < ${NFRAMES}; i++) ms.push(b.frame(d, true));
    return { ms, tris: window.__bbGl.frameTris, draws: window.__bbGl.frameDraws, split: window.__bbGl.split, mv: window.__bbGl.moving }; })()`);
  out.orbitSplit = orbit.split, out.orbitMoving = orbit.mv;
  out.orbitMedian = med(orbit.ms), out.orbitP95 = p95(orbit.ms), out.movingTris = orbit.tris, out.movingDraws = orbit.draws;
  const still = await ev(`(() => { const b = window.__bbGl.bench, ms = []; for (let i = 0; i < 5; i++) ms.push(b.frame(0, false));
    return { ms, tris: window.__bbGl.frameTris, draws: window.__bbGl.frameDraws }; })()`);
  out.stillMedian = med(still.ms), out.stillTris = still.tris, out.stillDraws = still.draws;
  // 3D2 (step 2): the same still frame without its edge pass (what edges cost)
  const ne = await ev(`(() => { const b = window.__bbGl.bench, g = window.__bbGl, ms = []; g.noEdges = true; for (let i = 0; i < 5; i++) ms.push(b.frame(0, false));
    g.noEdges = false; b.frame(0, false); return { ms, segs: g.q2 ? g.q2.edgeSegs : null }; })()`);
  out.stillNoEdgesMedian = med(ne.ms), out.stillEdgeSegs = ne.segs;
  out.stillProxy = await ev("window.__bbGl.proxies ? { cells: window.__bbGl.proxies.drawnCells, tris: window.__bbGl.proxies.drawnTris } : null");
  // 3D2: the same still frame without proxies (the full detail), and the two pictures compared at the home camera
  if (out.proxies) {
    const full = await ev(`(() => { const b = window.__bbGl.bench, g = window.__bbGl, ms = []; g.noProxies = true; for (let i = 0; i < 5; i++) ms.push(b.frame(0, false));
      const r = { ms, tris: g.frameTris, draws: g.frameDraws }; g.noProxies = false; b.frame(0, false); return r; })()`);
    out.stillFullMedian = med(full.ms), out.stillFullTris = full.tris, out.stillFullDraws = full.draws;
    out.proxyPixels = await ev(`(() => { const b = window.__bbGl.bench, g = window.__bbGl; g.noProxies = true; const a = b.snap(0, 0).rgba; g.noProxies = false;
      const c = b.snap(0, 0).rgba; let n = 0, any = 0; for (let k = 0; k < a.length; k += 4) { const d = Math.max(Math.abs(a[k] - c[k]), Math.abs(a[k + 1] - c[k + 1]), Math.abs(a[k + 2] - c[k + 2]));
      if (d > 40) n++; if (d > 0) any++; } return { changed: n, any, pixels: a.length / 4, cells: g.proxies.drawnCells, tris: g.proxies.drawnTris }; })()`);
  }
  // 3D2 (step 3): occlusion queries on (WebGL2's only: not under --renderer webgpu). WebGL2 answers a query only after the page's task ends, so each frame
  // here is its own animation frame: 3 still frames to settle (each asks, the next uses the answers), then 5
  // timed; an orbit likewise; the queries asked and batches left out; the picture against the one without.
  if (!GPU) {
    const oc = await ev(`(async () => { const b = window.__bbGl.bench, g = window.__bbGl, ms = [], next = () => new Promise((r) => requestAnimationFrame(() => r()));
      g.occlusion = true; for (let i = 0; i < 3; i++) { b.frame(0, false); await next(); }
      for (let i = 0; i < 5; i++) { ms.push(b.frame(0, false)); await next(); } const r = { ms, tris: g.frameTris, draws: g.frameDraws, occ: g.occ };
      const orbit = []; const d = 2 * Math.PI / ${NFRAMES}; for (let i = 0; i < ${NFRAMES}; i++) { orbit.push(b.frame(d, true)); await next(); } r.orbit = orbit; r.orbitOcc = g.occ;
      g.occlusion = false; b.frame(0, false); return r; })()`);
    out.occStillMedian = med(oc.ms), out.occStillTris = oc.tris, out.occStillDraws = oc.draws, out.occ = oc.occ, out.occOrbitMedian = med(oc.orbit), out.occOrbit = oc.orbitOcc;
    out.occPixels = await ev(`(async () => { const b = window.__bbGl.bench, g = window.__bbGl, next = () => new Promise((r) => requestAnimationFrame(() => r()));
      const a = b.snap(0, 0).rgba; g.occlusion = true; for (let i = 0; i < 4; i++) { b.snap(0, 0); await next(); }
      const c = b.snap(0, 0).rgba; const occ = g.occ; g.occlusion = false; let n = 0, any = 0; for (let k = 0; k < a.length; k += 4) { const d = Math.max(Math.abs(a[k] - c[k]), Math.abs(a[k + 1] - c[k + 1]), Math.abs(a[k + 2] - c[k + 2]));
      if (d > 40) n++; if (d > 0) any++; } return { changed: n, any, occ }; })()`);
  }
  // Close orbit (Rust's bench "close": a fifth of the fitted distance): what culling leaves out.
  const close = await ev(`(() => { const b = window.__bbGl.bench, ms = []; b.snap(0, 0); b.zoom(0.2); const d = 2 * Math.PI / ${NFRAMES};
    for (let i = 0; i < ${NFRAMES}; i++) ms.push(b.frame(d, true));
    const moving = { tris: window.__bbGl.frameTris, draws: window.__bbGl.frameDraws, mv: window.__bbGl.moving }; const st = [];
    for (let i = 0; i < 3; i++) st.push(b.frame(0, false));
    return { ms, st, moving, tris: window.__bbGl.frameTris, draws: window.__bbGl.frameDraws }; })()`);
  out.closeMoving = close.moving.mv;
  out.closeMedian = med(close.ms), out.closeTris = close.moving.tris, out.closeStill = med(close.st), out.closeStillTris = close.tris, out.closeStillDraws = close.draws;
  out.gpuBytes = await ev("window.__bbGl.gpuBytes ?? null");
  await quiet(20000);
  // Orbit input -> frame (ADR 0074), streamed: 60 drag moves of 4 px sent at 60 Hz without waiting; each
  // input's timeStamp -> the end of the animation-frame callbacks of the first frame drawn after it. The
  // picture while dragging: its non-background pixels against the still picture's (never blank).
  await ev(`(window.__bbGl.bench.snap(0, 0), 0)`); // the home camera fitted (the close orbit left it near)
  await quiet(20000);
  const stillNonBg = await ev(NONBG);
  const [cx, cy] = await canvasCentre();
  await mouse("mouseMoved", cx, cy);
  await mouse("mousePressed", cx, cy, { button: "left", buttons: 1, clickCount: 1 });
  await ev(`(window.__fr.kind = "stream", window.__fr.moves = [], 0)`);
  const t0 = Date.now();
  let dragNonBg = null;
  for (let k = 1; k <= 60; k++) {
    void mouse("mouseMoved", cx + 4 * k, cy, { button: "left", buttons: 1 }); // not awaited: streamed
    const wait = t0 + k * (1000 / 60) - Date.now();
    if (wait > 0) await sleep(wait);
    if (k === 45) dragNonBg = await ev(NONBG); // the picture while the drag goes on
  }
  await sleep(50);
  const lat = await ev(`(() => { const f = window.__fr, out = []; for (const t of f.moves) { const d = f.draws.find((x) => x >= t); if (d !== undefined) out.push(d - t); } return out; })()`);
  await mouse("mouseReleased", cx + 240, cy, { button: "left", buttons: 0, clickCount: 1 });
  await ev(`(window.__fr.kind = null, 0)`);
  await quiet(30000);
  out.moveMedian = med(lat), out.moveP95 = p95(lat), out.moveInputs = lat.length;
  out.stillNonBg = stillNonBg, out.dragNonBg = dragNonBg;
  // Pick click -> frame: 5 clicks where the model is (the canvas centre after a fit).
  const picks = [], pickWhat = [];
  for (let k = 0; k < 5; k++) {
    await ev(`(window.__bbGl.bench.snap(0, 0), 0)`); // the home camera fitted: the model under the centre
    await quiet(20000);
    await ev(`(window.__fr.kind = "click", window.__fr.tIn = null, window.__fr.pick0 = { f: window.__bbGl.frames, o: window.__bbGl.overpaints ?? 0, d: window.__bbGl.drawn, n: window.__fr.draws.length }, 0)`);
    await mouse("mouseMoved", cx, cy);
    await mouse("mousePressed", cx, cy, { button: "left", buttons: 1, clickCount: 1 });
    await mouse("mouseReleased", cx, cy, { button: "left", buttons: 0, clickCount: 1 });
    const last = await quiet(60000);
    // 3D2 (step 4): what the click drew: frames, of them selections over the kept picture, the canvas size before and after, the frames' times after the input
    pickWhat.push(await ev(`(() => { const p = window.__fr.pick0, g = window.__bbGl; return { frames: g.frames - p.f, overpaints: (g.overpaints ?? 0) - p.o, size0: p.d ? p.d.w + "x" + p.d.h : null,
      size1: g.drawn ? g.drawn.w + "x" + g.drawn.h : null, at: window.__fr.draws.slice(p.n).map((x) => Math.round(x - window.__fr.tIn)) }; })()`));
    const t = await ev("window.__fr.tIn");
    picks.push(t !== null && last >= t ? last - t : null);
    await ev(`(window.__fr.kind = null, 0)`);
    // Esc clears the selection for the next click
    await S("Input.dispatchKeyEvent", { type: "keyDown", key: "Escape", code: "Escape", windowsVirtualKeyCode: 27 });
    await S("Input.dispatchKeyEvent", { type: "keyUp", key: "Escape", code: "Escape", windowsVirtualKeyCode: 27 });
    await quiet(20000);
  }
  out.pickMedian = med(picks.filter((x) => x !== null));
  out.pickWhat = pickWhat;
  out.pickScanMs = await ev("window.__bbGl.pickMs ?? null"); // 3D2: the page's ray scan of the last click
  out.selected = await ev("window.__bbGl.picks");
  if (PIXELS && BUILD === "wasm") await pixels();
  return out;
}

// The pictures of three fixed cameras (wasm build): home, turned 120 degrees, from above.
const CAMS = [[0, 0], [2.0944, 0], [0.5, 0.9]];
async function pixels() {
  mkdirSync(PIXELS, { recursive: true });
  await quiet(20000);
  for (let i = 0; i < CAMS.length; i++) {
    await ev(`(window.__bbGl.bench.snap(${CAMS[i][0]}, ${CAMS[i][1]}), 0)`); // the layout settled first
    await quiet(20000);
    const [dy, dp] = CAMS[i];
    const b64 = await ev(`(() => { const s = window.__bbGl.bench.snap(${dy}, ${dp}); let t = ""; const u = s.rgba; for (let k = 0; k < u.length; k += 32768) t += String.fromCharCode.apply(null, u.subarray(k, k + 32768)); return s.w + "x" + s.h + ":" + btoa(t); })()`);
    const [wh, data] = b64.split(":");
    writeFileSync(join(PIXELS, `${MODEL}-${i}.rgba`), Buffer.from(data, "base64"));
    writeFileSync(join(PIXELS, `${MODEL}-${i}.size`), wh);
  }
}
function comparePixels() {
  const rows = [];
  for (const model of MODELS)
    for (let i = 0; i < CAMS.length; i++) {
      const a = join(COMPARE, `${model}-${i}.rgba`), b = join(PIXELS, `${model}-${i}.rgba`);
      if (!existsSync(a) || !existsSync(b)) continue;
      const A = readFileSync(a), B = readFileSync(b);
      let n = 0, any = 0;
      if (A.length !== B.length) { rows.push({ model, cam: i, pixels: 0, changed: -1, any: -1 }); continue; }
      for (let k = 0; k < A.length; k += 4) {
        const d = Math.max(Math.abs(A[k] - B[k]), Math.abs(A[k + 1] - B[k + 1]), Math.abs(A[k + 2] - B[k + 2]));
        if (d > 40) n++;
        if (d > 0) any++;
      }
      rows.push({ model, cam: i, pixels: A.length / 4, changed: n, any });
    }
  return rows;
}

// ---- the report ----
const fmt = (v, d = 0) => (v === null || v === undefined || Number.isNaN(v) ? "–" : Number(v).toLocaleString("en-US", { maximumFractionDigits: d, minimumFractionDigits: d }));
const mb = (v) => (v === null || v === undefined ? "–" : (v / 1048576).toFixed(1));
function table(rows) {
  const L = [];
  L.push("| build | open → first frame ms (data → boxes / first / complete) | orbit frame ms median / p95 | moving: triangles / draws | still frame ms | still: triangles / draws | close orbit ms (triangles) | close still ms: triangles / draws | GPU MB | JS heap MB (+ array buffers) | wasm MB | orbit input → frame ms median / p95 (inputs) | pixels drawn while dragging / still | pick click → frame ms (the page's ray scan) |");
  L.push("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|");
  for (const r of rows)
    L.push(`| ${r.build} | ${fmt(r.firstFrameMs)}${r.meshAt ? ` (${r.previewAt ? fmt(r.previewAt - r.meshAt) : "–"} / ${fmt(r.firstFrameMs - r.meshAt)} / ${r.fullFrameMs ? fmt(r.fullFrameMs - r.meshAt) : "–"})` : ""} | ${fmt(r.orbitMedian, 1)} / ${fmt(r.orbitP95, 1)} | ${fmt(r.movingTris)} / ${fmt(r.movingDraws)} | ${fmt(r.stillMedian, 1)} | ${fmt(r.stillTris)} / ${fmt(r.stillDraws)} | ${fmt(r.closeMedian, 1)} (${fmt(r.closeTris)}) | ${fmt(r.closeStill, 1)}: ${fmt(r.closeStillTris)} / ${fmt(r.closeStillDraws)} | ${mb(r.gpuBytes)} | ${mb(r.jsHeap)}${r.arrayBuffers ? ` (+ ${mb(r.arrayBuffers)})` : ""} | ${r.wasmMemory ? mb(r.wasmMemory) : "–"} | ${fmt(r.moveMedian, 1)} / ${fmt(r.moveP95, 1)} (${fmt(r.moveInputs)}) | ${fmt(r.dragNonBg)} / ${fmt(r.stillNonBg)} | ${fmt(r.pickMedian)}${r.pickScanMs != null ? ` (scan ${fmt(r.pickScanMs, 1)})` : ""} |`);
  return L;
}
/** 3D2: the HLOD proxies per build and model (none: under the page's triangle threshold, or not served). */
function proxyTable(rows) {
  const L = ["| build | still frame ms | without its edge pass | edge segments drawn |", "|---|---|---|---|"];
  for (const r of rows) L.push(`| ${r.build} | ${fmt(r.stillMedian, 1)} | ${fmt(r.stillNoEdgesMedian, 1)} | ${fmt(r.stillEdgeSegs)} |`);
  L.push("");
  L.push("| build | cells (with a proxy) | proxy triangles | BPRX MB | steps (longest ms); first ask → uploaded ms | still frame ms with / without proxies | still: triangles / draws with / without | proxy cells (triangles) drawn still | pixels changed > 40 / any (of) |");
  L.push("|---|---|---|---|---|---|---|---|---|");
  for (const r of rows) {
    const p = r.proxies, x = r.proxyPixels;
    if (!p) { L.push(`| ${r.build} | none | – | – | – | – | – | – | – |`); continue; }
    L.push(`| ${r.build} | ${fmt(p.cells)} (${fmt(p.withProxy)}) | ${fmt(p.tris)} | ${mb(p.bytes)} | ${fmt(p.steps)} (${fmt(p.maxStepMs)}); ${fmt(p.loadMs)} | ${fmt(r.stillMedian, 1)} / ${fmt(r.stillFullMedian, 1)} | ${fmt(r.stillTris)} / ${fmt(r.stillDraws)} – ${fmt(r.stillFullTris)} / ${fmt(r.stillFullDraws)} | ${fmt(r.stillProxy?.cells)} (${fmt(r.stillProxy?.tris)}) | ${x ? `${fmt(x.changed)} / ${fmt(x.any)} (${fmt(x.pixels)})` : "–"} |`);
  }
  return L;
}
/** 3D2 (step 3): occlusion queries (off by default) against the same frames without. */
function occTable(rows) {
  const L = ["| build | still frame ms without / with queries | still with: triangles / draws | queries asked; batches hidden | orbit ms median without / with | pixels changed > 40 / any, settled |", "|---|---|---|---|---|---|"];
  for (const r of rows)
    L.push(`| ${r.build} | ${fmt(r.stillMedian, 1)} / ${fmt(r.occStillMedian, 1)} | ${fmt(r.occStillTris)} / ${fmt(r.occStillDraws)} | ${fmt(r.occ?.asked)}; ${fmt(r.occ?.hidden)} | ${fmt(r.orbitMedian, 1)} / ${fmt(r.occOrbitMedian, 1)} | ${r.occPixels ? `${fmt(r.occPixels.changed)} / ${fmt(r.occPixels.any)}` : "–"} |`);
  return L;
}
function report(rows, history, cmp, forced, loads) {
  const L = [];
  L.push("# 3D speed of the C lab (frames)", "");
  L.push("Generated by `node ccore/tools/frames.mjs` (ticket 3D1, ADR 0078); never edit by hand. Raw numbers in");
  L.push("`ccore/out/frames/frames.json` (not committed).", "");
  if (forced) L.push("**Forced:** written with the 1-minute load over 4 (`--force`): the numbers are slower than on an idle machine.", "");
  L.push(`Machine: ${platform()} ${release()} ${arch()}, ${cpus().length} CPUs (${cpus()[0]?.model ?? "?"}), ${(totalmem() / 1073741824).toFixed(1)} GB. ` +
    `Run ${new Date().toISOString().slice(0, 16)}Z, label \`${LABEL}\`, renderer ${RENDERER}, 1-minute load at each model's start: ${loads.join(", ")}.`, "");
  L.push("Headless Chromium draws WebGL2 with **SwiftShader** (on the CPU, 1280×800): the numbers compare before and");
  L.push("after on this machine; a GPU draws the same frames many times faster. Method in the tool's header: open →");
  L.push("first frame from the navigation start; an orbit of " + NFRAMES + " frames over 360°, each drawn as a moving frame and");
  L.push("finished (a 1×1 `readPixels`); the still frame (the full picture) after it; triangles and draw calls submitted");
  L.push("by that frame; the same orbit at a fifth of the fitted distance (close: what culling can leave out); GPU bytes of the model's buffers; JS heap used after a garbage collection; the core module's memory");
  L.push("(wasm build); 60 drag moves streamed at 60 Hz by CDP input (each pointermove → the first frame drawn after it, ADR 0074)");
  L.push("and the canvas's non-background pixels while dragging against the still picture's; 5 clicks on the");
  L.push("model (pointerdown → the frame that drew the selection).", "");
  for (const model of MODELS) {
    const rs = rows.filter((r) => r.model === model);
    if (!rs.length) continue;
    L.push(`## ${MODEL_NAME[model]}`, "");
    L.push(...table(rs), "");
    L.push("Ticket 3D2: the still frame without its edge pass (what edges cost); then the HLOD proxies: the tree the core served,", "the still frame at the home camera with and without them, and the two pictures compared (a pixel counts when a channel", "differs by more than 40 levels).", "");
    L.push(...proxyTable(rs), "");
    L.push("Occlusion queries (ticket 3D2 step 3; ADR 0036 #1, off by default as Rust on WebGL2): still frames settled with and", "without them, an orbit with them.", "");
    L.push(...occTable(rs), "");
  }
  if (history.length) {
    L.push("## Before and after (each step of tickets 3D1 and 3D2)", "");
    L.push("One row per label and model, wasm build: orbit frame median ms, still frame median ms, close orbit median ms,");
    L.push("open → first frame ms, GPU MB, orbit input → frame median ms, pick click → frame ms.", "");
    L.push("| label | model | orbit ms | still ms | close orbit ms | first frame ms | GPU MB | input → frame ms | pick ms | load |");
    L.push("|---|---|---|---|---|---|---|---|---|---|");
    for (const h of history)
      L.push(`| ${h.label} | ${h.model} | ${fmt(h.orbit, 1)} | ${fmt(h.still, 1)} | ${fmt(h.close, 1)} | ${fmt(h.first)} | ${mb(h.gpu)} | ${fmt(h.move, 1)} | ${fmt(h.pick)} | ${h.load} |`);
    L.push("");
  }
  if (cmp && cmp.length) {
    L.push("## Pictures against the previous run", "");
    L.push("Three fixed cameras per model (wasm build); a pixel counts as changed when a channel differs by more than 40 levels.", "");
    L.push("| model | camera | pixels | changed > 40 | any difference |", "|---|---|---|---|---|");
    for (const c of cmp) L.push(`| ${c.model} | ${c.cam} | ${fmt(c.pixels)} | ${fmt(c.changed)} | ${fmt(c.any)} |`);
    L.push("");
  }
  L.push(`<!-- frames-history ${JSON.stringify(history)} -->`, "");
  return L.join("\n") + realSection(readReal());
}

// ---- REAL1: the section "On the machine's GPU" (its data in a comment, the tables made from it) ----
const REAL_BEGIN = "<!-- real-gpu-begin -->", REAL_END = "<!-- real-gpu-end -->";
function readReal() {
  const t = existsSync(OUT) ? readFileSync(OUT, "utf8") : "";
  const d = /<!-- real-gpu-data (.*) -->/.exec(t)?.[1];
  return d ? JSON.parse(d) : { runs: {}, links: {} };
}
const linksHead = ["| run | links | detail triangles | caches MB (proxies MB, ms) | open → first picture ms (from) | → complete ms | still ms: triangles / draws (without edges ms; segments) | moving ms median (p95): triangles / draws | close still ms: triangles / draws | GPU MB | GPU MB, half hidden, budget ½ (detail loaded) | load start → end |",
  "|---|---|---|---|---|---|---|---|---|---|---|---|"];
const linksRow = (r, name = r.label) => { const f = (v, d = 0) => fmt(v, d); return `| ${name} | ${r.links} | ${f(r.detailTris)} | ${mb(r.cacheBytes)} (${mb(r.hlodBytes)}, ${f(r.hlodMs)}) | ${f(r.firstPicture)} (${r.fromProxies ? "proxies" : "first frame"}) | ${f(r.complete)} | ${f(r.still, 1)}: ${f(r.stillTris)} / ${f(r.stillDraws)} (${f(r.stillNoEdges, 1)}; ${f(r.edgeSegs)}) | ${f(r.moving, 1)} (${f(r.movingP95, 1)}): ${f(r.movingTris)} / ${f(r.movingDraws)} | ${f(r.closeStill, 1)}: ${f(r.closeTris)} / ${f(r.closeDraws)} | ${mb(r.gpuBytes)} | ${mb(r.gpuBudgetBytes)} (${r.detailLoaded ?? "–"}) | ${r.load.split(" ")[0]} → ${r.endLoad.split(" ")[0]} |`; };
function realSection(D) {
  const runs = Object.entries(D.runs ?? {}), links = Object.entries(D.links ?? {});
  if (!runs.length && !links.length) return "";
  const L = ["", REAL_BEGIN, "", "## On the machine's GPU (not SwiftShader: never compare with the tables above)", ""];
  L.push("Ticket REAL1: the same tool with `LAB_GPU=vulkan` (`ccore/test/lib/chromium.mjs`, DEV.md): headless Chromium draws on the");
  L.push("machine's GPU through ANGLE on Vulkan, and WebGPU through Dawn on the same Vulkan driver. The GPU and driver below are");
  L.push("what the page reports (`WEBGL_debug_renderer_info`, WebGPU's `GPUAdapterInfo`). Each frame is finished as above (WebGL2:");
  L.push("a 1×1 `readPixels`; WebGPU: `onSubmittedWorkDone`).", "");
  for (const [key, r] of runs) {
    L.push(`### ${r.renderer === "webgpu" ? "WebGPU" : "WebGL2"} (\`LAB_GPU=${r.gpu}\`)`, "");
    L.push(`GPU: WebGL2 \`${r.webgl}\`${r.webgpu ? `; WebGPU adapter \`${[r.webgpu.vendor, r.webgpu.architecture, r.webgpu.device, r.webgpu.description].filter(Boolean).join(" / ")}\` (${r.webgpu.evidence ?? "?"})` : ""}. ` +
      `Run ${r.date}, label \`${r.label}\`; 1-minute load (/proc/loadavg) at each model's start: ${r.loads.join(", ")}.`, "");
    for (const model of [...new Set(r.rows.map((x) => x.model))]) {
      L.push(`#### ${MODEL_NAME[model] ?? model}`, "");
      L.push(...table(r.rows.filter((x) => x.model === model)), "");
    }
    void key;
  }
  if (links.length) {
    L.push("### Synthetic links (`--links N`, native host)", "");
    L.push("Each link: `bench/links_synth.mjs` (160 unique closed meshes of about 600 triangles), converted by LINKF, linked as a folder.", "");
    L.push(...linksHead);
    for (const [key, r] of links) L.push(linksRow(r, `${key} (\`${r.gpu}\`, ${r.renderer})`));
    L.push("", `GPU: ${[...new Set(links.map(([, r]) => r.webgl))].map((x) => `\`${x}\``).join(", ")}.`, "");
  }
  L.push(`<!-- real-gpu-data ${JSON.stringify(D)} -->`, "", REAL_END, "");
  return L.join("\n");
}
/** Writes the real-GPU section into OUT, the rest of the file as it is. */
function writeReal(D) {
  const t = existsSync(OUT) ? readFileSync(OUT, "utf8") : "# 3D speed of the C lab (frames)\n";
  const a = t.indexOf(REAL_BEGIN), b = t.indexOf(REAL_END);
  const head = a >= 0 ? t.slice(0, a).trimEnd() + "\n" : t.trimEnd() + "\n", tail = a >= 0 && b > a ? t.slice(b + REAL_END.length).replace(/^\n+/, "") : "";
  mkdirSync(dirname(OUT), { recursive: true });
  writeFileSync(OUT, head + realSection(D) + tail);
  log(`wrote the real-GPU section of ${OUT}`);
}

// ---- LNK3D: --links N ----
async function linksMain() {
  const { makeLinkFolder } = await import("../bench/links_synth.mjs");
  const P = Number(opt("--link-proxies") ?? 160), T = Number(opt("--link-tris") ?? 600);
  log(`links: ${LINKS} synthetic links of ${P} unique meshes (load ${loadNow()})`);
  const site = buildSite();
  await serve(site);
  const dir = join(HOME, "links");
  mkdirSync(dir, { recursive: true });
  host = start("host", BIN, [String(HOST_PORT)], { cwd: FILES, env: { ...process.env, HOME, BIMBLE_CONFIG_DIR: CONFIG, BIMBLE_TOKEN: TOKEN, BIMBLE_ORIGINS: ORIGIN,
    BIMBLE_LINK_CACHE_MIN: "0", BIMBLE_CACHE_DIR: join(tmp, "cache") } });
  await waitFor("the host", 60000, async () => { if (host.exitCode !== null) throw new Error(host.out.slice(-400)); return host.out.includes("listening"); }, 30);
  const hcall = async (op, name, body = {}) => {
    const r = await fetch(`http://127.0.0.1:${HOST_PORT}/${op}/${name}?t=${TOKEN}`, { method: "POST", body: JSON.stringify(body) });
    return { status: r.status, text: await r.text() };
  };
  const files = await makeLinkFolder({ hcall, dir, work: tmp, n: LINKS, proxies: P, tris: T, unique: true });
  await hcall("action", "new");
  const t0 = Date.now();
  await hcall("action", "link_folder", { path: dir });
  await waitFor("the links converted", 600000, async () => { const r = JSON.parse((await hcall("query", "links_panel")).text).models; return r.length === LINKS && r.every((m) => m.state === "ready"); }, 200);
  const convertMs = Date.now() - t0;
  for (let k = 0; k < LINKS; k++) {
    await hcall("action", "manage_links", { op: "unpin", name: `link_${k}.ifc` });
    await hcall("action", "manage_links", { op: "place", name: `link_${k}.ifc`, move_mm: [30000 * (k % 5), 30000 * Math.floor(k / 5), 0] });
  }
  const logs = files.map((f) => readFileSync(`${f}.log.txt`, "utf8"));
  const detail = logs.reduce((n, t) => n + Number(/\ntriangles: (\d+)/.exec(t)?.[1] ?? 0), 0);
  const cacheBytes = files.reduce((n, f) => n + statSync(`${f}.bimbleview`).size, 0);
  const hlod = logs.map((t) => /proxies \(HLOD, baked for the 3D view\): (\d+) cells, (\d+) triangles, (\d+) bytes, built in (\d+) ms/.exec(t));
  const out = { label: LABEL, links: LINKS, load: loadNow(), detailTris: detail, cacheBytes, convertMs,
    hlodBytes: hlod.reduce((n, m) => n + Number(m?.[3] ?? 0), 0), hlodMs: hlod.reduce((n, m) => n + Number(m?.[4] ?? 0), 0) };
  await startBrowser();
  BUILD = "native", MODEL = "links";
  await openTab();
  await S("Page.navigate", { url: pageUrl() });
  let linksMaxHeap = 0; // SIZE3: the JS heap + array buffers at most during the open (polled every 250 ms)
  const linksHeapPoll = setInterval(() => { S("Runtime.getHeapUsage").then((h) => { linksMaxHeap = Math.max(linksMaxHeap, h.usedSize + (h.backingStorageSize ?? 0)); }).catch(() => {}); }, 250);
  await waitFor("the page ready", 240000, () => ev(`!!(document.querySelector('[data-bb-ready]') && window.__bbGl && window.__bbGl.firstFrameMs && window.__bbGl.links && window.__bbGl.links.length === ${LINKS})`), 50);
  if (GPU) { // GPU1: the WebGPU renderer must be the one drawing
    await waitFor("the renderer", 30000, () => ev("(window.__bbPerf?.snapshot().renderer.reason ?? '') !== 'starting…'"), 50);
    const r = await ev("window.__bbPerf.snapshot().renderer");
    if (r.using !== "WebGPU") throw new Error(`--renderer webgpu: ${r.using} draws (${r.reason})`);
    await S("Runtime.evaluate", { expression: GPU_HELPERS });
  }
  await whichGpu();
  await quiet(30000);
  const g0 = await ev(`(() => { const g = window.__bbGl; return { first: g.firstFrameMs, preview: g.proxyPreview ? g.proxyPreview.at : null, meshAt: g.meshAt ?? null, full: g.fullFrameMs ?? null,
    fetch: g.meshFetchMs, bytes: g.meshBytes, detailedAt: g.linksDetailedAt ?? null, peakDecoded: g.openPeakDecoded ?? null, decodes: g.decodes ?? null }; })()`);
  clearInterval(linksHeapPoll);
  out.maxHeap = linksMaxHeap, out.detailedAt = g0.detailedAt, out.openPeakDecoded = g0.peakDecoded, out.decodes = g0.decodes; // SIZE3
  out.firstPicture = g0.preview !== null && g0.preview !== undefined ? Math.min(g0.preview, g0.first) : g0.first;
  out.fromProxies = g0.preview !== null && g0.preview !== undefined;
  out.firstFrameMs = g0.first, out.meshAt = g0.meshAt, out.complete = g0.full, out.meshFetchMs = g0.fetch, out.meshBytes = g0.bytes;
  await ev("(window.__bbGl.bench.snap(0, 0), 0)");
  await quiet(20000);
  const orbit = await ev(`(() => { const b = window.__bbGl.bench, ms = []; const d = 2 * Math.PI / ${NFRAMES};
    for (let i = 0; i < ${NFRAMES}; i++) ms.push(b.frame(d, true));
    return { ms, tris: window.__bbGl.frameTris, draws: window.__bbGl.frameDraws }; })()`);
  out.moving = med(orbit.ms), out.movingP95 = p95(orbit.ms), out.movingTris = orbit.tris, out.movingDraws = orbit.draws;
  const still = await ev(`(() => { const b = window.__bbGl.bench, ms = []; for (let i = 0; i < 5; i++) ms.push(b.frame(0, false));
    return { ms, tris: window.__bbGl.frameTris, draws: window.__bbGl.frameDraws }; })()`);
  out.still = med(still.ms), out.stillTris = still.tris, out.stillDraws = still.draws;
  const ne = await ev(`(() => { const b = window.__bbGl.bench, g = window.__bbGl, ms = []; g.noEdges = true; for (let i = 0; i < 5; i++) ms.push(b.frame(0, false));
    g.noEdges = false; const r = { ms, segs: g.q2 ? g.q2.edgeSegs : null }; b.frame(0, false); r.segs = g.q2 ? g.q2.edgeSegs : null; return r; })()`);
  out.stillNoEdges = med(ne.ms), out.edgeSegs = ne.segs;
  const close = await ev(`(() => { const b = window.__bbGl.bench, ms = []; b.snap(0, 0); b.zoom(0.25); for (let i = 0; i < 5; i++) ms.push(b.frame(0, false));
    return { ms, tris: window.__bbGl.frameTris, draws: window.__bbGl.frameDraws }; })()`);
  out.closeStill = med(close.ms), out.closeTris = close.tris, out.closeDraws = close.draws;
  await ev("(window.__bbGl.bench.snap(0, 0), 0)");
  await quiet(20000);
  out.gpuBytes = await ev("window.__bbGl.gpuBytes ?? null");
  // SIZE2: what the links cost in memory and on disk: the caches' sections, the mesh bytes the page fetched (every
  // /mesh answer, a link's own included), the host's RSS, the tab's (renderer process) and the GPU process's RSS,
  // the JS heap and array buffers after a collection
  out.cacheSections = sumSections(files.map((f) => `${f}.bimbleview`));
  out.meshServed = await ev("window.__bbGl.meshBytesServed ?? window.__bbGl.meshBytes ?? null");
  out.hostRssMb = rssMb(host.pid), out.hostAnonMb = anonMb(host.pid); // SIZE3: anonymous apart (the caches' mapped pages are file pages)
  await S("HeapProfiler.collectGarbage").catch(() => {});
  await sleep(500);
  { const h = await S("Runtime.getHeapUsage"); out.jsHeap = h.usedSize, out.arrayBuffers = h.backingStorageSize ?? null; }
  out.rss = chromeRss();
  console.log(`SIZE3: open: JS heap + array buffers at most ${mb(out.maxHeap)} MB, decoded at most ${mb(out.openPeakDecoded)} MB, decodes ${JSON.stringify(out.decodes)}, ` +
    `links in view detailed at ${out.detailedAt?.toFixed(0)} ms (first picture ${out.firstPicture?.toFixed(0)} ms); host anonymous ${out.hostAnonMb.toFixed(0)} MB`);
  console.log(`memory: caches ${JSON.stringify(out.cacheSections)}; mesh served ${out.meshServed}; host RSS ${out.hostRssMb.toFixed(0)} MB; ` +
    `tab RSS ${out.rss.renderer.toFixed(0)} MB, GPU process ${out.rss.gpu.toFixed(0)} MB; JS heap ${mb(out.jsHeap)} MB (+ ${mb(out.arrayBuffers)} MB array buffers); GPU ${mb(out.gpuBytes)} MB`);
  if (PIXELS) { mkdirSync(PIXELS, { recursive: true }); await pixels(); }
  // a class hidden (every link's closed meshes): the page's work (visibility in the page, the state texture's
  // upload: elements in the rows sent), the /mesh fetches it made
  const f0 = await ev("window.__bbGl.meshFetches ?? null");
  await hcall("action", "view_rule", { op: "hide", class: "IfcBuildingElementProxy" });
  await sleep(500);
  await quiet(20000);
  out.hideClass = await ev(`(() => { const g = window.__bbGl; return { vis: g.visApplyMs ?? null, upload: g.stateUpload ?? null, fetches: (g.meshFetches ?? 0) - ${f0 ?? 0}, elements: g.links.reduce((n, l) => n + l.elements, 0) }; })()`);
  await hcall("action", "undo");
  await sleep(500);
  await quiet(20000);
  // half the links hidden, the budget at half: what the GPU keeps
  for (let k = 0; k < LINKS; k += 2) await hcall("action", "manage_links", { op: "hide", name: `link_${k}.ifc` });
  await ev(`(window.__bbGl.gpuBudget = Math.round(window.__bbGl.gpuBytes / 2), 0)`);
  await sleep(1500);
  await quiet(20000);
  out.gpuBudgetBytes = await ev("window.__bbGl.gpuBytes ?? null");
  out.detailLoaded = await ev("window.__bbGl.linkDetail ? window.__bbGl.linkDetail.filter((x) => x.detail).length : null");
  out.endLoad = loadNow();
  writeFileSync(join(RAW, `links-${LABEL}.json`), JSON.stringify(out, null, 1));
  console.log(`hide a class: ${JSON.stringify(out.hideClass)}`);
  out.stateCost30k = await ev("window.__bbGl.bench.stateCost ? window.__bbGl.bench.stateCost(30000, 100) : null");
  console.log(`a visibility change of 30,000 elements (100 vertices each): ${JSON.stringify(out.stateCost30k)}`);
  const r = out, f = (v, d = 0) => fmt(v, d);
  console.log([...linksHead, linksRow(r)].join("\n"));
  if (REAL && !opt("--out")) { const D = readReal(); D.links[`${RENDERER} ${LINKS}`] = { ...r, gpu: LAB_GPU, renderer: RENDERER, webgl: GPU_SEEN.webgl }; writeReal(D); }
  if (COMPARE && PIXELS) for (const c of comparePixels()) console.log(`pictures: camera ${c.cam}: ${c.changed} of ${c.pixels} pixels differ by more than 40 levels (${c.any} at all)`);
}

// ---- REAL1: FRAMES_LINK_FOLDER=<dir> --steps 5,10,14,17: a folder of your own IFC files linked ----
// The N smallest files of the folder, linked into a new project (link_folder on a scratch folder of symbolic
// links: the folder itself is only read, caches go to the run's cache folder, BIMBLE_LINK_CACHE_BESIDE=0),
// N growing by step, one host and one Chromium per step (the caches of earlier steps are reused). Measured:
// conversion, open → first picture (proxies) and complete, still and moving frames (triangles, draws),
// GPU bytes, the tab's JS heap, the renderer's and the GPU process's RSS, the host's RSS, the links' origins
// (a link far from the others: where Fit all puts the camera), and a screenshot after Fit all (raw rows,
// out/frames/folder-*.json and .png, never committed: no name or path is written). A watchdog stops the
// step (kills Chromium) when MemAvailable falls under FRAMES_MIN_AVAIL_GB (2.5) or the tab's JS heap passes
// FRAMES_MAX_HEAP_GB (3), and the next steps are not run.
const FOLDER = process.env.FRAMES_LINK_FOLDER ? resolve(process.env.FRAMES_LINK_FOLDER) : null;
const MIN_AVAIL = Number(process.env.FRAMES_MIN_AVAIL_GB ?? 2.5), MAX_HEAP = Number(process.env.FRAMES_MAX_HEAP_GB ?? 3);
function procTree(root) {
  const kids = new Map();
  for (const d of readdirSync("/proc")) {
    if (!/^\d+$/.test(d)) continue;
    try { const st = readFileSync(`/proc/${d}/stat`, "utf8"); const pp = Number(st.slice(st.lastIndexOf(")") + 2).split(" ")[1]); (kids.get(pp) ?? kids.set(pp, []).get(pp)).push(Number(d)); } catch { /* gone */ }
  }
  const out = [], todo = [root];
  while (todo.length) { const p = todo.pop(); out.push(p); todo.push(...(kids.get(p) ?? [])); }
  return out;
}
const anonMb = (pid) => { try { return Number(/RssAnon:\s+(\d+)/.exec(readFileSync(`/proc/${pid}/status`, "utf8"))?.[1] ?? 0) / 1024; } catch { return 0; } }; // SIZE3
const rssMb = (pid) => { try { return Number(/VmRSS:\s+(\d+)/.exec(readFileSync(`/proc/${pid}/status`, "utf8"))?.[1] ?? 0) / 1024; } catch { return 0; } };
const cmd = (pid) => { try { return readFileSync(`/proc/${pid}/cmdline`, "utf8"); } catch { return ""; } };
/** The browser's processes by kind (MB of RSS): renderer (the tab), gpu, others. */
function chromeRss() {
  const r = { renderer: 0, gpu: 0, other: 0 };
  if (!browser) return r;
  for (const p of procTree(browser.pid)) { const c = cmd(p), m = rssMb(p); if (/--type=renderer/.test(c)) r.renderer += m; else if (/--type=gpu-process/.test(c)) r.gpu += m; else r.other += m; }
  return r;
}
async function folderMain() {
  const steps = (opt("--steps") ?? "5,10,14,17").split(",").map(Number);
  const all = readdirSync(FOLDER).filter((f) => /\.ifc$/i.test(f)).map((f) => ({ f, bytes: statSync(join(FOLDER, f)).size })).sort((a, b) => a.bytes - b.bytes);
  log(`folder: ${all.length} IFC files, ${mb(all.reduce((n, x) => n + x.bytes, 0))} MB; steps ${steps.join(", ")} (load ${loadNow()})`);
  const site = buildSite();
  await serve(site);
  // the links' folder under the host's HOME (it lists only its roots, query fs_roots), at a path that stays the same
  // from run to run with FRAMES_CACHE_DIR (a cache is found by its IFC's path)
  const cache = process.env.FRAMES_CACHE_DIR ? resolve(process.env.FRAMES_CACHE_DIR) : join(tmp, "cache");
  const fhome = process.env.FRAMES_CACHE_DIR ? join(cache, "home") : HOME, dir = join(fhome, "folder");
  mkdirSync(dir, { recursive: true }), mkdirSync(cache, { recursive: true });
  const results = [];
  let minAvail = Infinity, maxHeap = 0, stopWhy = null, heapNow = 0, stopNow = () => {};
  // the waits end when the watchdog fires (waitFor retries a check that throws until its timeout)
  const stopped = new Promise((_, rej) => { stopNow = () => rej(new Error(stopWhy)); });
  stopped.catch(() => {});
  const W = (p) => Promise.race([p, stopped]);
  const watch = setInterval(() => {
    const a = memGb();
    minAvail = Math.min(minAvail, a);
    if (!stopWhy && (a < MIN_AVAIL || heapNow > MAX_HEAP * 2 ** 30)) {
      stopWhy = a < MIN_AVAIL ? `MemAvailable ${a.toFixed(2)} GB under ${MIN_AVAIL}` : `JS heap ${(heapNow / 2 ** 30).toFixed(2)} GB over ${MAX_HEAP}`;
      const rs = chromeRss();
      log(`WATCHDOG: ${stopWhy}: Chromium killed (tab RSS ${rs.renderer.toFixed(0)} MB, GPU process ${rs.gpu.toFixed(0)} MB, JS heap ${(heapNow / 2 ** 20).toFixed(0)} MB, host RSS ${host ? rssMb(host.pid).toFixed(0) : "-"} MB)`);
      if (browser) try { process.kill(-browser.pid, "SIGKILL"); } catch { /* gone */ }
      if (cdp) cdp.close();
      stopNow();
    }
  }, 250);
  try {
    for (const n of steps) {
      if (stopWhy) break;
      const take = all.slice(0, n);
      for (const x of take) if (!existsSync(join(dir, x.f))) symlinkSync(join(FOLDER, x.f), join(dir, x.f));
      const out = { files: take.length, ifcBytes: take.reduce((k, x) => k + x.bytes, 0), load: loadNow() };
      minAvail = memGb(), maxHeap = 0, heapNow = 0;
      host = start("host", BIN, [String(HOST_PORT)], { cwd: FILES, env: { ...process.env, HOME: fhome, BIMBLE_CONFIG_DIR: CONFIG, BIMBLE_TOKEN: TOKEN, BIMBLE_ORIGINS: ORIGIN,
        BIMBLE_CACHE_DIR: join(cache, "links-cache"), BIMBLE_LINK_CACHE_BESIDE: "0" } });
      await waitFor("the host", 60000, async () => { if (host.exitCode !== null) throw new Error(host.out.slice(-400)); return host.out.includes("listening"); }, 30);
      const hcall = async (op, name, body = {}) => {
        const r = await fetch(`http://127.0.0.1:${HOST_PORT}/${op}/${name}?t=${TOKEN}`, { method: "POST", body: JSON.stringify(body) });
        return { status: r.status, text: await r.text() };
      };
      await hcall("action", "new");
      const t0 = Date.now();
      const lf = await hcall("action", "link_folder", { path: dir });
      if (lf.status !== 200) throw new Error(`link_folder: ${lf.status} ${lf.text.slice(0, 200)}`);
      let panel = null;
      await W(waitFor("the links converted", 3600000, async () => {
        panel = JSON.parse((await hcall("query", "links_panel")).text).models;
        return panel.length === n && panel.every((m) => m.state !== "converting" && m.state !== "queued");
      }, 500));
      out.convertMs = Date.now() - t0;
      out.states = panel.map((m) => m.state).join(",");
      out.fromCache = panel.filter((m) => m.from_cache).length;
      out.elements = panel.reduce((k, m) => k + m.elements, 0), out.storedTris = panel.reduce((k, m) => k + m.triangles, 0);
      out.origins = panel.map((m) => m.origin);
      out.hostRssMb = rssMb(host.pid);
      log(`${n} files: converted in ${out.convertMs} ms (${out.fromCache} from cache), ${out.elements} elements, ${out.storedTris} stored triangles, host RSS ${out.hostRssMb.toFixed(0)} MB`);
      try {
        if (memGb() < 3) throw new Error(`memory: ${memGb().toFixed(1)} GB available before Chromium`);
        curTarget = null;
        await startBrowser();
        BUILD = "native", MODEL = "links";
        await openTab();
        const tNav = Date.now();
        await S("Page.navigate", { url: pageUrl() });
        const heapPoll = setInterval(() => { S("Runtime.getHeapUsage").then((h) => { heapNow = h.usedSize + (h.backingStorageSize ?? 0); maxHeap = Math.max(maxHeap, heapNow); }).catch(() => {}); }, 250); // SIZE3: 250 ms (1 s missed the open's peaks)
        try {
          await W(waitFor("the page ready", 900000, () => ev(`!!(document.querySelector('[data-bb-ready]') && window.__bbGl && window.__bbGl.firstFrameMs && window.__bbGl.links && window.__bbGl.links.length === ${n})`), 200));
          out.readyWallMs = Date.now() - tNav;
          await whichGpu();
          out.renderer = GPU_SEEN.webgl;
          await quiet(120000);
          const g0 = await ev(`(() => { const g = window.__bbGl; return { first: g.firstFrameMs, preview: g.proxyPreview ? g.proxyPreview.at : null, full: g.fullFrameMs ?? null,
            fetch: g.meshFetchMs, bytes: g.meshBytes, draw: (g.linkDraw ?? []).map((l) => l.xf ? l.xf.t : null), far: g.farLinks ?? null, detail: (g.linkDetail ?? []).filter((x) => x.detail).length,
            detailedAt: g.linksDetailedAt ?? null, peakDecoded: g.openPeakDecoded ?? null, decodes: g.decodes ?? null, linkGpu: g.linkGpuUsed ?? null, budget: g.linkGpuBudget ?? null }; })()`);
          out.detailedAt = g0.detailedAt, out.openPeakDecoded = g0.peakDecoded, out.decodes = g0.decodes, out.linkGpuUsed = g0.linkGpu, out.linkGpuBudget = g0.budget; // SIZE3
          out.firstPicture = g0.preview ?? g0.first, out.fromProxies = g0.preview != null, out.firstFrameMs = g0.first, out.complete = g0.full;
          out.meshBytes = g0.bytes, out.detailLoaded = g0.detail, out.linkT = g0.draw, out.farKm = g0.far ? g0.far.map((x) => x.km) : null;
          await ev("(window.__bbGl.bench.snap(0, 0), 0)");
          await quiet(60000);
          const shot = await S("Page.captureScreenshot", { format: "png" });
          writeFileSync(join(RAW, `folder-${n}.png`), Buffer.from(shot.data, "base64"));
          const still = await ev(`(() => { const b = window.__bbGl.bench, ms = []; for (let i = 0; i < 5; i++) ms.push(b.frame(0, false));
            return { ms, tris: window.__bbGl.frameTris, draws: window.__bbGl.frameDraws }; })()`);
          out.still = med(still.ms), out.stillMax = Math.max(...still.ms), out.stillTris = still.tris, out.stillDraws = still.draws;
          const orbit = await ev(`(() => { const b = window.__bbGl.bench, ms = []; const d = 2 * Math.PI / 60;
            for (let i = 0; i < 60; i++) ms.push(b.frame(d, true));
            return { ms, tris: window.__bbGl.frameTris, draws: window.__bbGl.frameDraws }; })()`);
          out.moving = med(orbit.ms), out.movingP95 = p95(orbit.ms), out.movingTris = orbit.tris, out.movingDraws = orbit.draws;
          await ev("(window.__bbGl.bench.snap(0, 0), 0)");
          await quiet(60000);
          out.gpuBytes = await ev("window.__bbGl.gpuBytes ?? null");
          await S("HeapProfiler.collectGarbage").catch(() => {});
          const h = await S("Runtime.getHeapUsage");
          out.jsHeap = h.usedSize, out.arrayBuffers = h.backingStorageSize ?? null;
          out.maxHeap = maxHeap;
          out.rss = chromeRss();
          out.hostRestMb = rssMb(host.pid), out.hostAnonMb = anonMb(host.pid); // SIZE3: the host at rest after the page's requests
          out.survived = (await ev("1 + 1")) === 2;
        } finally { clearInterval(heapPoll); }
      } catch (e) {
        out.error = String(e.message ?? e).slice(0, 300), out.survived = false;
        log(`${n} files: ${out.error}`);
      }
      out.minAvailGb = minAvail, out.stopWhy = stopWhy, out.endLoad = loadNow();
      results.push(out);
      log(`${n} files: ${JSON.stringify(out)}`);
      writeFileSync(join(RAW, "folder.json"), JSON.stringify({ label: LABEL, gpu: LAB_GPU, results }, null, 1));
      if (cdp) cdp.close(), cdp = null;
      if (browser) await stop(browser), browser = null;
      await stop(host), host = null;
    }
  } finally { clearInterval(watch); }
  const f = (v, d = 0) => fmt(v, d);
  const L = ["| IFC files (MB) | elements | stored triangles | conversion s (from cache) | open → first picture ms (from) | → complete ms | still ms: triangles / draws | moving ms median (p95): triangles / draws | detail links on GPU | GPU MB | JS heap MB (+ array buffers; max) | tab RSS MB | GPU process RSS MB | host RSS MB | min MemAvailable GB | survived |",
    "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"];
  for (const r of results)
    L.push(`| ${r.files} (${mb(r.ifcBytes)}) | ${f(r.elements)} | ${f(r.storedTris)} | ${f(r.convertMs / 1000, 1)} (${r.fromCache}) | ${f(r.firstPicture)} (${r.fromProxies ? "proxies" : "first frame"}) | ${f(r.complete)} | ${f(r.still, 1)}: ${f(r.stillTris)} / ${f(r.stillDraws)} | ${f(r.moving, 1)} (${f(r.movingP95, 1)}): ${f(r.movingTris)} / ${f(r.movingDraws)} | ${r.detailLoaded ?? "–"} | ${mb(r.gpuBytes)} | ${mb(r.jsHeap)} (+ ${mb(r.arrayBuffers)}; ${mb(r.maxHeap)}) | ${f(r.rss?.renderer)} | ${f(r.rss?.gpu)} | ${f(r.hostRssMb)} | ${f(r.minAvailGb, 2)} | ${r.survived ? "yes" : `no: ${r.stopWhy ?? r.error}`} |`);
  const md = [`# A local folder of IFC files linked, LAB_GPU=${LAB_GPU} (never committed)`, "", `Renderer: ${results.find((r) => r.renderer)?.renderer ?? "?"}. Load at the start of each step: ${results.map((r) => r.load.split(" ")[0]).join(", ")}.`, "", ...L, ""].join("\n");
  writeFileSync(join(RAW, "folder.md"), md);
  console.log(md);
}

async function main() {
  if (FOLDER) return folderMain();
  if (LINKS) return linksMain();
  const loads = [];
  // The load before the run (the run's own SwiftShader threads raise it: it is recorded per model, not checked).
  for (let i = 0; load1() > 4 && i < (flag("--no-wait") ? 0 : 60); i++) { if (i % 6 === 0) log(`waiting: 1-minute load ${load1()} over 4`); await sleep(10000); }
  if (load1() > 4 && !FORCE) throw new Error(`1-minute load ${load1()} over 4: not measured (--force to measure and mark the report)`);
  let forced = load1() > 4;
  const site = buildSite();
  await serve(site);
  for (const m of MODELS) copyFileSync(MODEL_FILE[m](), join(site, siteName(m)));
  await startBrowser();
  const rows = [];
  for (const model of MODELS)
    for (const build of BUILDS) {
      BUILD = build, MODEL = model;
      loads.push(`${model}/${build} ${load1()}`);
      log(`${build} ${model} (load ${loadNow()}) …`);
      const t0 = Date.now();
      const r = await measure();
      log(`${build} ${model}: ${JSON.stringify(r)} (${Date.now() - t0} ms)`);
      rows.push(r);
      if (host) { await stop(host); host = null; }
    }
  const cmp = COMPARE && PIXELS ? comparePixels() : null;
  const prev = existsSync(OUT) && !flag("--no-history") ? /<!-- frames-history (.*) -->/.exec(readFileSync(OUT, "utf8"))?.[1] : null;
  const history = (prev ? JSON.parse(prev) : []).filter((h) => h.label !== LABEL);
  for (const r of rows.filter((x) => x.build === "wasm"))
    history.push({ label: LABEL, model: r.model, orbit: r.orbitMedian, still: r.stillMedian, close: r.closeMedian, pick: r.pickMedian, first: r.firstFrameMs, gpu: r.gpuBytes, move: r.moveMedian, load: r.load.split(" ")[0] });
  writeFileSync(join(RAW, EXTRA ? "frames-local.json" : REAL ? `frames-${LAB_GPU}-${RENDERER}.json` : "frames.json"), JSON.stringify({ label: LABEL, rows, cmp }, null, 1));
  if (REAL && !EXTRA && !opt("--out")) {
    const D = readReal();
    D.runs[RENDERER] = { renderer: RENDERER, gpu: LAB_GPU, label: LABEL, date: new Date().toISOString().slice(0, 16) + "Z", loads, webgl: GPU_SEEN.webgl, webgpu: GPU_SEEN.webgpu, rows };
    writeReal(D);
  } else {
    mkdirSync(dirname(OUT), { recursive: true });
    writeFileSync(OUT, report(rows, history, cmp, forced, loads));
    log(`wrote ${OUT}`);
  }
  for (const r of rows) if (r.dragNonBg !== null && r.stillNonBg && r.dragNonBg < 0.2 * r.stillNonBg) { log(`blank while dragging: ${r.build} ${r.model} ${r.dragNonBg} of ${r.stillNonBg}`); process.exitCode = 1; }
  if (cmp) for (const c of cmp) if (c.changed < 0 || c.changed > 0.001 * c.pixels) { log(`pictures differ: ${JSON.stringify(c)}`); process.exitCode = 1; }
}

main().catch((e) => { console.error(`frames: ${e.stack ?? e}`); process.exitCode = 1; })
  .finally(async () => { await cleanup(); process.exit(process.exitCode ?? 0); }); // keep-alive sockets would hold the loop
