// The lab's controls against what the C core does, and how fast (ticket UXMAP):
//   node ccore/tools/ux.mjs [--check] [--force] [--builds wasm,native] [--models small,large]
//     [--reps N] [--no-timing] [--no-measure | --measure-only | --timing-only] [--merge out/ux/ux.json]
//     [--fresh-verdicts] [--report-only] [--site DIR] [--out FILE] [--only <surface>]
// Writes ccore/results/ux.md (never edit it by hand) and the raw answers to out/ux/ux.json.
//
// What it does. ONE headless Chromium (SwiftShader, 1280x800, English) over raw CDP opens the lab:
//   wasm    the production build of the C lab (`vite build` with VITE_CCORE=1, what GitHub Pages
//           serves at its root) with out/wasm/ccore.wasm, served like Pages (gzip), the model in the
//           page's wasm module;
//   native  the same build with ?ccore=0&native=1&gl=1 against out/tcc/bimble-ccore (build.sh tcc)
//           on a copy of the model in a temporary folder (HOME and BIMBLE_CONFIG_DIR there too).
// The model: golden/frame.json (72 elements) and, for the timings, the synthetic 20,000-element
// model of bench/large_model.py (results/large.md).
//
// Enumeration, from the page itself (no hand list): the title row (quick bar), the 14 tab links, every
// ribbon tab's tools and boxes, the left panel (navigator, properties), the view tabs, the viewport's
// buttons (view cube's Home and Fit all), the status bar, the selection's panels (an element selected),
// the viewport's right-click menu, the key table (window.__bbKeys.KEYS) and, discovered when a control
// opens them, every dialog (File menu, palette, Settings…), page (Levels & grids, Materials…) and right
// panel, two levels deep (a dialog opened from the File menu, a page from the palette). A ribbon tool's binding (action / page / panel / dialog) is read from
// web-ui/src/shell/ribbon-tabs.ts by its place (data-bb ribbon-tool-<group>-<tool>); other controls are
// bound by their label to the core's `query actions` label when one matches.
//
// Verdict, from what happens when the control is activated with real input (CDP Input.dispatch*) on a
// freshly loaded page (and a freshly started host): compared before / after: the core (`query state`,
// `actions`, `measure`, `visibility`, `open_views`, `history`), the shell (data-bb-shell), the dialogs,
// page and right panel shown, the page's text (the pointer readout left out), the camera (__bbGl.navEye,
// q2, clip), a file chooser, a download, a confirm():
//   works               an observable effect (one of the above changed). A tool counts only when its
//                       gesture gives its result: Measure two clicks -> one more measurement; Place
//                       clicks (and Enter) -> the rev changes.
//   says not available the "not yet" label (data-not-yet, the tooltip, NotAvailable).
//   dead                drawn enabled, and nothing above changed (the core may have been called: the
//                       count is in the note).
//   error               a console error, an uncaught exception, an HTTP status >= 400, or an alert line.
//   disabled            drawn disabled (aria-disabled / disabled); the core's action and its enabled are
//                       in the note when the control has one.
//   not exercised       not driven (a text field or a select: typing a value is not driven; a control
//                       that could not be found again after the reload).
//
// Response time (ADR 0074): from the input event (the page's pointerdown / keydown / wheel timeStamp)
// to the end of the animation-frame callbacks of the first frame that holds the last effect (the last
// DOM change, the last WebGL draw or the last core call, then 300 ms with none): the frame is then
// handed to the compositor. The verdict pass's own activation gives one number per control and build
// ("ms 1 run"); the timing pass (--timing-only, or after the verdicts) the median of --reps (default 5)
// activations, each on a freshly loaded page, one control per binding.
// Numbers are not written when the 1-minute load (/proc/loadavg) is above 4, unless --force.
// The machine waits for 3 GB of available memory before it starts the browser.
//
// Measure (ticket UXMAP 3): 3D view, the tool on, 20 pointer moves over the model and two clicks;
// for each: the response as above, the core calls it made (the wasm module's exports are wrapped
// before the page loads: name and ms of each call) and the frames drawn; one CDP Profiler run of the
// moves gives the functions with the most self time.
//
// --merge FILE (an earlier run's out/ux/ux.json): its verdicts are reused (unless --fresh-verdicts) and
// its Measure rows and times kept, so verdicts, Measure (--measure-only) and timings (--timing-only) can
// run one at a time; --report-only writes the report from FILE without a browser.
// --check: the wasm build on the small model, verdicts only; exits 1 when it finds more `dead` or
// `error` controls than the committed report counts (not in ci/run.sh yet: UXMAP study).
import { spawn, execFileSync } from "node:child_process";
import { createServer } from "node:http";
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs";
import { arch, cpus, loadavg, platform, release, tmpdir, totalmem } from "node:os";
import { dirname, extname, join, resolve, sep } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { gzipSync } from "node:zlib";
import { port } from "../test/ports.mjs";
import { CHROMIUM, LAB_GPU, checkGpu, gpuFlags, rendererInfo } from "../test/lib/chromium.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = resolve(CCORE, "../web-ui");
const argv = process.argv.slice(2);
const opt = (n) => { const i = argv.indexOf(n); return i >= 0 ? argv[i + 1] : null; };
const flag = (n) => argv.includes(n);
const CHECK = flag("--check"), FORCE = flag("--force");
const BUILDS = (opt("--builds") ?? (CHECK ? "wasm" : "wasm,native")).split(",");
// UX_EXTRA_MODEL=<path>: a third model, "local" (a file of your own: its name and path are never
// written; the report then goes to out/ux/ux-local.md, never committed, unless --out).
const EXTRA = process.env.UX_EXTRA_MODEL ? resolve(process.env.UX_EXTRA_MODEL) : null;
const MODELS = (opt("--models") ?? (CHECK ? "small" : EXTRA ? "small,large,local" : "small,large")).split(",");
const REPS = Number(opt("--reps") ?? 5);
const TIMING = !CHECK && !flag("--no-timing");
const MEASURE = !CHECK && !flag("--no-measure");
const MEASURE_ONLY = flag("--measure-only"); // the Measure study alone (no verdicts, no timings)
// The Measure study's models: also "heavy", a mesh-heavy stand-in (bench/mesh_heavy_model.py: the frame
// plus HEAVY proxies of about HEAVY_TRIS triangles each), and "local" with UX_EXTRA_MODEL.
const HEAVY = 700, HEAVY_TRIS = 1000;
const MEASURE_MODELS = (opt("--measure-models") ?? (EXTRA ? "small,large,heavy,local" : "small,large,heavy")).split(",");
const LARGE_N = Number(opt("--large") ?? 20000);
const OUT = resolve(opt("--out") ?? (EXTRA ? join(CCORE, "out/ux/ux-local.md") : join(CCORE, "results/ux.md")));
const RAW = join(CCORE, "out/ux");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const WASM = join(CCORE, "out/wasm/ccore.wasm");
const FIXTURE = join(CCORE, "golden/frame.json");
const QUIET = 300; // ms with no effect: settled
const tmp = mkdtempSync(join(tmpdir(), "ccore-ux-"));
const HOME = join(tmp, "home"), CONFIG = join(tmp, "config"), DL = join(tmp, "downloads"), FILES = join(tmp, "files");
for (const d of [RAW, HOME, CONFIG, DL, FILES]) mkdirSync(d, { recursive: true });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const log = (s) => console.log(`ux: ${s}`);
const T0 = Date.now();
const procs = [];
let server = null;

// ---- the machine ----
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

// ---- processes ----
function start(name, cmd, args, opts = {}) {
  const p = spawn(cmd, args, { stdio: ["ignore", "pipe", "pipe"], detached: true, ...opts });
  p.out = "";
  p.stdout.on("data", (d) => { p.out += d; if (p.out.length > 200000) p.out = p.out.slice(-100000); });
  p.stderr.on("data", (d) => { p.out += d; if (p.out.length > 200000) p.out = p.out.slice(-100000); });
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
async function waitFor(what, ms, fn, every = 100) {
  const until = Date.now() + ms;
  let last;
  while (Date.now() < until) {
    try { const v = await fn(); if (v) return v; } catch (e) { last = e; }
    await sleep(every);
  }
  throw new Error(`timeout waiting for ${what}${last ? `: ${last.message ?? last}` : ""}`);
}

// ---- the site (as tools/perf.mjs: GitHub Pages' gzip) ----
const SITE_PORT = port("ux");
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
  if (!existsSync(WASM)) throw new Error(`no ${WASM}: ccore/build.sh wasm`);
  const site = join(tmp, "site"), t0 = Date.now();
  execFileSync("npx", ["vite", "build", "--base", "./", "--outDir", site, "--emptyOutDir", "--logLevel", "warn"],
    { cwd: UI, stdio: ["ignore", "pipe", "pipe"], env: { ...process.env, VITE_CCORE: "1", VITE_CCORE_REV: "" } });
  rmSync(join(site, "ccore"), { recursive: true, force: true });
  mkdirSync(join(site, "ccore"));
  copyFileSync(WASM, join(site, "ccore/ccore.wasm"));
  copyFileSync(FIXTURE, join(site, "ccore/frame.json"));
  log(`vite build ${Date.now() - t0} ms`);
  return site;
}
function largeModel(site) {
  const f = join(RAW, `large-${LARGE_N}.bimble`);
  if (!existsSync(f)) execFileSync("python3", [join(CCORE, "bench/large_model.py"), String(LARGE_N), f], { stdio: ["ignore", "pipe", "pipe"] });
  copyFileSync(f, join(site, "ccore/large.bimble"));
  return f;
}

function localModel(site) {
  if (!EXTRA || !existsSync(EXTRA)) throw new Error("UX_EXTRA_MODEL: no such file");
  copyFileSync(EXTRA, join(site, "ccore/local" + extname(EXTRA)));
}
function heavyModel(site) {
  const f = join(RAW, `heavy-${HEAVY}-${HEAVY_TRIS}.json`);
  if (!existsSync(f)) execFileSync("python3", [join(CCORE, "bench/mesh_heavy_model.py"), String(HEAVY), String(HEAVY_TRIS), f], { stdio: ["ignore", "pipe", "pipe"] });
  copyFileSync(f, join(site, "ccore/heavy.json"));
}
const MODEL_FILE = { small: () => FIXTURE, large: () => join(RAW, `large-${LARGE_N}.bimble`), heavy: () => join(RAW, `heavy-${HEAVY}-${HEAVY_TRIS}.json`), local: () => EXTRA };
const MODEL_URL = { small: "", large: "model=ccore/large.bimble", heavy: "model=ccore/heavy.json", local: () => `model=ccore/local${extname(EXTRA)}` };

// ---- the native host ----
const HOST_PORT = port("ux_host");
const TOKEN = "ux-token";
let host = null;
async function startHost(model) {
  if (host) await stop(host);
  const src = MODEL_FILE[model](), copy = join(FILES, model === "small" ? "frame.json" : `${model}${extname(src)}`);
  copyFileSync(src, copy);
  for (let attempt = 0; ; attempt++) {
    host = start("host", BIN, [String(HOST_PORT), copy], { cwd: FILES, env: { ...process.env, HOME, BIMBLE_CONFIG_DIR: CONFIG, BIMBLE_TOKEN: TOKEN, BIMBLE_ORIGINS: ORIGIN } }); // the site's origin may call it (host/main.c)
    try {
      await waitFor("the host", 20000, async () => {
        if (host.exitCode !== null) throw new Error(host.out.slice(-400));
        return (await fetch(`http://127.0.0.1:${HOST_PORT}/query/state?t=${TOKEN}`, { method: "POST", body: "{}" })).ok;
      }, 30);
      return;
    } catch (e) {
      if (attempt >= 5) throw e;
      await stop(host);
      await sleep(300); // the port in TIME_WAIT
    }
  }
}

// ---- CDP ----
async function cdpConnect(wsUrl) {
  const ws = new WebSocket(wsUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = () => rej(new Error("CDP connect failed")); });
  let id = 0;
  const pending = new Map(), listeners = [];
  ws.onmessage = (ev) => {
    const msg = JSON.parse(ev.data);
    if (msg.id !== undefined && pending.has(msg.id)) {
      const { resolve: ok, reject } = pending.get(msg.id);
      pending.delete(msg.id);
      if (msg.error) reject(new Error(`${msg.error.message} ${msg.error.data ?? ""}`));
      else ok(msg.result);
    } else if (msg.method) for (const fn of listeners) fn(msg);
  };
  const send = (method, params = {}, sessionId) => new Promise((ok, reject) => {
    const m = ++id;
    pending.set(m, { resolve: ok, reject });
    ws.send(JSON.stringify({ id: m, method, params, ...(sessionId ? { sessionId } : {}) }));
  });
  return { send, on: (fn) => listeners.push(fn), close: () => ws.close() };
}

// The page's instruments, installed before any of its scripts (Page.addScriptToEvaluateOnNewDocument).
const INSTR = String.raw`(() => {
  if (window.__ux) return;
  const ux = window.__ux = { calls: [], lastMut: 0, muts: 0, lastDraw: 0, lastCall: 0, tIn: null, arm: false, ticks: [], f: -1, X: null, res: 0, own: false };
  const now = () => performance.now();
  new MutationObserver((recs) => {
    for (const r of recs) {
      const el = r.target.nodeType === 1 ? r.target : r.target.parentElement;
      if (el && el.closest && el.closest('[data-bb="status-coords"]')) continue;
      ux.muts++; ux.lastMut = now();
      // MEAS1: what changed last (the element's data-bb, else its tag and class), for the Measure study's moves
      const bb = el && el.closest ? el.closest("[data-bb]") : null;
      ux.lastMutWhat = (bb ? bb.getAttribute("data-bb") : "") + " " + (el ? el.tagName.toLowerCase() + (el.className && typeof el.className === "string" ? "." + el.className.split(" ")[0] : "") : "") + " " + r.type + (r.attributeName ? ":" + r.attributeName : "")
        + [...r.addedNodes].slice(0, 1).map((n) => " +" + (n.nodeType === 1 ? n.tagName.toLowerCase() + (n.getAttribute("data-bb") ? "[" + n.getAttribute("data-bb") + "]" : "") + (typeof n.className === "string" && n.className ? "." + n.className.split(" ")[0] : "") : "#" + n.nodeName)).join("")
        + [...r.removedNodes].slice(0, 1).map((n) => " -" + (n.nodeType === 1 ? n.tagName.toLowerCase() + (n.getAttribute("data-bb") ? "[" + n.getAttribute("data-bb") + "]" : "") + (typeof n.className === "string" && n.className ? "." + n.className.split(" ")[0] : "") : "#" + n.nodeName)).join("");
      break;
    }
  }).observe(document, { subtree: true, childList: true, attributes: true, characterData: true });
  for (const t of ["pointerdown", "keydown", "wheel", "mousedown", "pointermove"]) addEventListener(t, (e) => { if (ux.arm) { ux.arm = false; ux.tIn = e.timeStamp; } }, true);
  const raf = window.requestAnimationFrame.bind(window);
  const tick = () => {
    const n = now(), f = window.__bbGl ? window.__bbGl.frames : -1;
    if (f !== ux.f) { ux.f = f; ux.lastDraw = n; }
    ux.ticks.push(n);
    if (ux.ticks.length > 4000) ux.ticks.splice(0, 2000);
  };
  window.requestAnimationFrame = (cb) => raf((ts) => { try { cb(ts); } finally { tick(); } });
  const loop = () => { raf(() => { tick(); loop(); }); };
  loop();
  ux.probe = () => {
    const t = ux.tIn, last = Math.max(ux.lastMut, ux.lastDraw, ux.lastCall);
    const eff = t !== null && last >= t ? last : null;
    const shown = eff === null ? null : ux.ticks.find((x) => x >= eff);
    return { now: now(), tIn: t, last, eff, ms: shown === undefined || shown === null ? null : shown - t };
  };
  // The wasm module: each export call timed and named (op/name of call()); ux.q asks the core directly.
  const inst0 = WebAssembly.instantiate;
  WebAssembly.instantiate = async function (src, imp) {
    const r = await inst0.call(this, src, imp);
    const X = r && r.instance && r.instance.exports;
    if (!X || typeof X.call !== "function" || !X.memory || ux.X) return r;
    ux.X = X;
    const dec = new TextDecoder();
    const s = (p, n) => dec.decode(new Uint8Array(X.memory.buffer, p, n).slice());
    const W = {};
    for (const k of Object.keys(X)) {
      const f = X[k];
      if (typeof f !== "function" || ["cc_alloc", "cc_free", "change_json", "host_log"].includes(k)) { W[k] = f; continue; }
      W[k] = (...a) => {
        if (ux.own) return f(...a);
        let name = k;
        try { if (k === "call") name = s(a[0], a[1]) + "/" + s(a[2], a[3]); } catch { /* keep k */ }
        const t0 = now();
        try { return f(...a); } finally { const t1 = now(); ux.calls.push([name, t0, t1 - t0]); ux.lastCall = t1; if (ux.calls.length > 20000) ux.calls.splice(0, 10000); }
      };
    }
    return { instance: { exports: W }, module: r.module };
  };
  ux.q = (op, name, body) => {
    const X = ux.X; if (!X) return null;
    ux.own = true;
    try {
      const enc = new TextEncoder();
      const put = (str) => { const b = enc.encode(str); const p = X.cc_alloc(Math.max(1, b.length)); new Uint8Array(X.memory.buffer).set(b, p); return [p, b.length]; };
      if (!ux.res) ux.res = X.cc_alloc(16);
      const a = [op, name, body || "{}"].map(put);
      const st = X.call(a[0][0], a[0][1], a[1][0], a[1][1], a[2][0], a[2][1], ux.res);
      for (const [p] of a) X.cc_free(p);
      const dv = new DataView(X.memory.buffer), ptr = dv.getUint32(ux.res, true), len = dv.getUint32(ux.res + 4, true);
      const text = new TextDecoder().decode(new Uint8Array(X.memory.buffer, ptr, len).slice());
      if (ptr) X.cc_free(ptr);
      return { status: st, text };
    } finally { ux.own = false; }
  };
  // Interactive elements under a root, in document order, visible only.
  const SEL = 'button, a[href], [role=button], [role=menuitem], [role=menuitemradio], [role=menuitemcheckbox], [role=option], [role=tab], [role=radio], [role=switch], input, select, textarea, summary, [data-not-yet]';
  const vis = (e) => { const r = e.getBoundingClientRect(); return r.width > 0 && r.height > 0 && getComputedStyle(e).visibility !== "hidden"; };
  const labelOf = (e) => {
    const t = (e.getAttribute("aria-label") || "").trim() || (e.innerText || "").trim() || (e.getAttribute("title") || "").trim() || (e.getAttribute("placeholder") || "").trim() || (e.value || "").toString().trim() || (e.querySelector("img") && e.querySelector("img").alt) || "";
    return t.replace(/\s+/g, " ").slice(0, 70);
  };
  ux.els = (roots) => {
    const out = [], seen = new Set();
    for (const root of roots) for (const r of document.querySelectorAll(root)) for (const e of r.querySelectorAll(SEL)) {
      if (seen.has(e) || !vis(e)) continue;
      seen.add(e); out.push(e);
    }
    return out;
  };
  ux.list = (roots) => ux.els(roots).map((e, i) => {
    const rb = e.closest("[data-bb]");
    return { i, tag: e.tagName.toLowerCase(), type: e.getAttribute("type") || e.getAttribute("role") || "", label: labelOf(e),
      bb: e.getAttribute("data-bb") || "", near: rb ? rb.getAttribute("data-bb") : "", disabled: e.disabled === true || e.getAttribute("aria-disabled") === "true",
      notYet: e.hasAttribute("data-not-yet"), missing: e.getAttribute("data-missing") || "", pressed: e.getAttribute("aria-pressed") || e.getAttribute("aria-checked") || "",
      current: e.getAttribute("aria-current") === "page" || e.getAttribute("aria-selected") === "true" || (e.classList.contains("on") && !e.classList.contains("tool")),
      group: (e.closest(".grp") && e.closest(".grp").querySelector(".gname") ? e.closest(".grp").querySelector(".gname").innerText.trim() : "") };
  });
  ux.at = (roots, i, label) => {
    const e = ux.els(roots)[i];
    if (!e || labelOf(e) !== label) return null;
    e.scrollIntoView({ block: "center", inline: "center" });
    const r = e.getBoundingClientRect();
    return [r.left + r.width / 2, r.top + r.height / 2];
  };
  const hash = (s) => { let h = 0; for (let i = 0; i < s.length; i++) h = (h * 31 + s.charCodeAt(i)) | 0; return h; };
  ux.dom = () => {
    const app = document.querySelector(".bb-app");
    let text = document.body.innerText || "";
    const co = document.querySelector('[data-bb="status-coords"]');
    if (co && co.innerText) text = text.split(co.innerText).join("");
    const g = window.__bbGl || {};
    const r = (a) => (Array.isArray(a) ? a.map((x) => Math.round(x)) : a ?? null);
    const box = (sel) => { const e = document.querySelector(sel); return e ? (e.getAttribute("aria-label") || e.getAttribute("data-bb") || "") + ":" + hash(e.innerText || "") : null; };
    return {
      shell: app ? app.getAttribute("data-bb-shell") : null,
      dialog: box(".bb-backdrop [role=dialog], .bb-backdrop > *"), page: box('[data-bb="page"]'), panel: box('[data-bb="right-panel"]'),
      menu: box('[role="menu"]'), props: box('[data-bb="properties"]'), modifier: box('[data-bb="modifier"]'),
      tab: (document.querySelector('a.tab.on') || {}).innerText || null,
      pageName: (() => { const p = document.querySelector('[data-bb="page"]'); if (!p) return null; const h = p.querySelector('h1, h2, [data-bb$="-title"], .title'); const c = p.querySelector('[data-bb]'); return (h ? h.innerText.trim() : "") || (c ? c.getAttribute("data-bb") : "") || "page"; })(),
      notYetTip: !!document.querySelector('[data-bb="not-yet-tip"]'),
      // the screen a control opens is "not available" as a whole (FAKE1: App.vue puts NotAvailable in its place)
      naMain: ['[data-bb="page"] > [data-bb="not-available"]', '[data-bb="right-panel"] > [data-bb="not-available"]', '.bb-backdrop > [role="dialog"] > [data-bb="not-available"]'].filter((q) => document.querySelector(q)).join(",") || null,
      naSide: !!document.querySelector('[data-bb="properties"] [data-bb="not-available"]'),
      alert: [...document.querySelectorAll('[role="alert"]')].map((e) => e.innerText.trim()).filter(Boolean).join(" | "),
      text: hash(text),
      // the state of the form controls (a radio or a checkbox shows no text)
      inputs: hash([...document.querySelectorAll("input, select")].map((e) => (e.type === "checkbox" || e.type === "radio" ? (e.checked ? "1" : "0") : e.value)).join("|")),
      camera: JSON.stringify([r(g.navEye ? g.navEye() : null), g.q2 || null, g.clip ? JSON.stringify(g.clip).length : null]),
      frames: g.frames ?? -1, measureCalls: g.measureCalls ?? 0, url: location.search,
    };
  };
})();`;

// ---- one browser, one tab ----
let cdp = null, S = null, browser = null;
const events = { console: [], exceptions: [], http: [], chooser: 0, downloads: 0, jsdialogs: 0, requests: 0, tabs: 0 };
async function startBrowser() {
  const gb = await waitMemory();
  log(`memory available ${gb.toFixed(1)} GB; starting Chromium`);
  browser = start("chromium", CHROMIUM, ["--headless=new", ...gpuFlags(), "--remote-debugging-port=0", // REAL1: LAB_GPU (test/lib/chromium.mjs)
    `--user-data-dir=${join(tmp, "chromium")}`, "--no-first-run", "--no-default-browser-check", "--lang=en-US", "--window-size=1280,800", "about:blank"],
    { env: { ...process.env, HOME } });
  const wsUrl = await waitFor("DevTools line", 60000, () => browser.out.match(/DevTools listening on (ws:\/\/\S+)/)?.[1]);
  cdp = await cdpConnect(wsUrl);
  cdp.on((msg) => {
    if (msg.method === "Browser.downloadWillBegin") events.downloads++;
    if (msg.method === "Target.targetCreated" && msg.params.targetInfo.type === "page" && msg.params.targetInfo.openerId) events.tabs++; // a link to a new tab
    if (msg.sessionId !== curSession) return;
    const p = msg.params;
    if (msg.method === "Runtime.consoleAPICalled" && (p.type === "error" || p.type === "assert")) events.console.push(p.args.map((a) => a.value ?? a.description ?? a.type).join(" ").slice(0, 300));
    else if (msg.method === "Runtime.exceptionThrown") events.exceptions.push((p.exceptionDetails.exception?.description ?? p.exceptionDetails.text).slice(0, 300));
    else if (msg.method === "Log.entryAdded" && p.entry.level === "error" && !/favicon/.test(p.entry.url ?? "") && !/Synchronous XMLHttpRequest/.test(p.entry.text)) events.console.push(`${p.entry.text} ${p.entry.url ?? ""}`.slice(0, 300));
    else if (msg.method === "Network.responseReceived" && p.response.status >= 400 && !/favicon/.test(p.response.url)) events.http.push(`${p.response.status} ${p.response.url.replace(/\?t=[^&]+/, "")}`);
    else if (msg.method === "Network.requestWillBeSent" && /^http:\/\/127\.0\.0\.1:/.test(p.request.url) && !/\/change\b/.test(p.request.url)) events.requests++;
    else if (msg.method === "Page.fileChooserOpened") events.chooser++;
    else if (msg.method === "Page.javascriptDialogOpening") { events.jsdialogs++; S("Page.handleJavaScriptDialog", { accept: true }).catch(() => {}); }
  });
  await cdp.send("Target.setDiscoverTargets", { discover: true });
  await cdp.send("Browser.setDownloadBehavior", { behavior: "allowAndName", downloadPath: DL, eventsEnabled: true });
}
let curSession = null, curTarget = null;
/** A new tab for each fresh page (the old one closed: no beforeunload question, no state carried). */
async function openTab() {
  if (curTarget) await cdp.send("Target.closeTarget", { targetId: curTarget }).catch(() => {});
  await cdp.send("Storage.clearDataForOrigin", { origin: ORIGIN, storageTypes: "local_storage,indexeddb,cache_storage,service_workers" }).catch(() => {});
  const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank" });
  const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
  curTarget = targetId, curSession = sessionId;
  S = (m, p) => Promise.race([cdp.send(m, p, sessionId), sleep(20000).then(() => { throw new Error(`CDP ${m}: no answer in 20 s`); })]);
  for (const d of ["Runtime", "Log", "Page", "Network"]) await S(`${d}.enable`);
  await S("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  await S("Emulation.setLocaleOverride", { locale: "en-US" }).catch(() => {});
  await S("Page.addScriptToEvaluateOnNewDocument", { source: INSTR });
  await S("Page.setInterceptFileChooserDialog", { enabled: true });
}
async function ev(expr) {
  const r = await S("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true });
  if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
  return r.result.value;
}

// ---- the page and the core ----
let BUILD = "wasm", MODEL = "small";
const pageUrl = (extra = "") => (BUILD === "wasm"
  ? `${ORIGIN}/?${typeof MODEL_URL[MODEL] === "function" ? MODEL_URL[MODEL]() : MODEL_URL[MODEL]}${extra}`
  : `${ORIGIN}/?ccore=0&native=1&gl=1&port=${HOST_PORT}&token=${TOKEN}${extra}`);
let loadMs = [], navs = 0;
let GPU_SEEN = null; // REAL1: the WebGL2 renderer string, read once from the page
async function fresh(extra = "") {
  if (BUILD === "native") await startHost(MODEL);
  const t0 = Date.now();
  const nonce = `&ux=${++navs}`, url = pageUrl(extra + nonce);
  for (let k = 0; ; k++) { // a tab that does not navigate (a print dialog, a hung page): a new one
    try {
      await openTab();
      await S("Page.navigate", { url });
      await waitFor("the navigation", 10000, () => ev(`location.search.includes(${JSON.stringify(nonce)})`), 50);
      break;
    } catch (e) {
      log(`navigation failed (${e.message}), new tab`);
      if (k >= 2) throw e;
      curTarget = null; // left open: closing it may hang as well
    }
  }
  const ready = () => ev(`!!(document.querySelector('[data-bb-ready]') && window.__bbGl && window.__bbGl.firstFrameMs && (document.querySelector('.bb-app')?.getAttribute('data-bb-shell') || '').startsWith('model'))`);
  try {
    await waitFor("the page ready", MODEL !== "small" ? 120000 : 30000, ready, 50);
    if (GPU_SEEN === null) { GPU_SEEN = checkGpu(await rendererInfo(ev)).renderer; log(`renderer (LAB_GPU=${LAB_GPU}): ${GPU_SEEN}`); } // REAL1
  } catch (e) {
    const why = await ev(`JSON.stringify({ url: location.href, shell: document.querySelector('.bb-app')?.getAttribute('data-bb-shell'), ready: !!document.querySelector('[data-bb-ready]'), gl: !!window.__bbGl, first: window.__bbGl?.firstFrameMs, text: (document.body.innerText || '').slice(0, 200) })`).catch((x) => String(x));
    log(`page not ready: ${why}`);
    throw e;
  }
  await settle(MODEL !== "small" ? 20000 : 5000, false);
  loadMs.push(Date.now() - t0);
}
async function core(op, name, body = "{}") {
  if (BUILD === "wasm") {
    const r = await ev(`JSON.stringify(window.__ux.q(${JSON.stringify(op)}, ${JSON.stringify(name)}, ${JSON.stringify(body)}))`);
    const a = JSON.parse(r ?? "null");
    return a && a.status === 200 ? a.text : null;
  }
  const r = await fetch(`http://127.0.0.1:${HOST_PORT}/${op}/${name}?t=${TOKEN}`, { method: "POST", body });
  return r.ok ? r.text() : null;
}
async function coreSnap() {
  const o = {};
  for (const q of ["state", "actions", "measure", "visibility", "open_views", "history", "settings"]) o[q] = await core("query", q);
  // `query state` has no rev: a model change is a new undo step (or a changed redo list / unsaved flag)
  try { const h = JSON.parse(o.history), st = JSON.parse(o.state); o.rev = `${h.undo.length}/${h.redo.length}/${st.unsaved}/${st.file}`; } catch { o.rev = null; }
  try { o.measurements = JSON.parse(o.measure)?.items?.length ?? 0; } catch { o.measurements = 0; }
  return o;
}
async function snap() {
  return { dom: await ev("window.__ux.dom()"), core: await coreSnap(), ev: { c: events.console.length, x: events.exceptions.length, h: events.http.length, ch: events.chooser, dl: events.downloads, js: events.jsdialogs, rq: events.requests, tabs: events.tabs },
    calls: BUILD === "wasm" ? await ev("window.__ux.calls.length") : 0 };
}
/** Waits until nothing changed for QUIET ms (DOM, WebGL draw, core call); returns the probe. */
async function settle(maxMs = 5000, armed = true) {
  const t0 = Date.now();
  for (;;) {
    const p = await ev("window.__ux.probe()");
    const since = p.now - Math.max(p.last, armed && p.tIn !== null ? p.tIn : 0);
    if (since >= QUIET && Date.now() - t0 >= 120) return p;
    if (Date.now() - t0 > maxMs) return { ...p, timeout: true };
    await sleep(30);
  }
}
const DIFF_KEYS = ["shell", "dialog", "page", "panel", "menu", "props", "modifier", "tab", "text", "inputs", "camera", "url"];
function effects(a, b) {
  const out = [];
  if (a.core.rev !== b.core.rev) out.push("model");
  for (const q of ["state", "actions", "measure", "visibility", "open_views", "history", "settings"]) if (q !== "state" || a.core.rev === b.core.rev) if (a.core[q] !== b.core[q]) out.push(q === "state" ? "state" : q);
  if (b.ev.tabs > a.ev.tabs) out.push("new tab");
  for (const k of DIFF_KEYS) if (a.dom[k] !== b.dom[k]) out.push(k);
  if (b.ev.ch > a.ev.ch) out.push("file chooser");
  if (b.ev.dl > a.ev.dl) out.push("download");
  if (b.ev.js > a.ev.js) out.push("confirm");
  return out;
}
function errorsBetween(a, b) {
  return [...events.console.slice(a.ev.c, b.ev.c), ...events.exceptions.slice(a.ev.x, b.ev.x), ...events.http.slice(a.ev.h, b.ev.h)]
    .concat(b.dom.alert && b.dom.alert !== a.dom.alert ? [`alert: ${b.dom.alert}`] : []);
}

// ---- input ----
const MOD = { alt: 1, ctrl: 2, meta: 4, shift: 8 };
async function arm() { await ev("(window.__ux.arm = true, window.__ux.tIn = null, document.activeElement && document.activeElement.blur && document.activeElement !== document.body ? 0 : 0)"); }
async function click([x, y], { button = "left", count = 1 } = {}) {
  await S("Input.dispatchMouseEvent", { type: "mouseMoved", x, y, button: "none", buttons: 0 });
  await S("Input.dispatchMouseEvent", { type: "mousePressed", x, y, button, buttons: button === "right" ? 2 : 1, clickCount: count });
  await S("Input.dispatchMouseEvent", { type: "mouseReleased", x, y, button, buttons: 0, clickCount: count });
}
const KEYCODE = { escape: ["Escape", "Escape", 27], delete: ["Delete", "Delete", 46], enter: ["Enter", "Enter", 13] };
async function key(k, mods = {}) {
  const [kk, code, vk] = KEYCODE[k] ?? [k, `Key${k.toUpperCase()}`, k.toUpperCase().charCodeAt(0)];
  const modifiers = (mods.ctrl ? MOD.ctrl : 0) | (mods.shift ? MOD.shift : 0) | (mods.alt ? MOD.alt : 0);
  const text = !mods.ctrl && kk.length === 1 ? (mods.shift ? kk.toUpperCase() : kk) : undefined;
  await S("Input.dispatchKeyEvent", { type: text ? "keyDown" : "rawKeyDown", key: mods.shift && kk.length === 1 ? kk.toUpperCase() : kk, code, windowsVirtualKeyCode: vk, modifiers, ...(text ? { text } : {}) });
  await S("Input.dispatchKeyEvent", { type: "keyUp", key: kk, code, windowsVirtualKeyCode: vk, modifiers });
}
/** A point on the canvas over the model: a world point (mm) on screen, else the canvas centre. */
async function modelPoint(x, y, z = 0) {
  const p = await ev(`(() => { const g = window.__bbGl; const p = g && g.toScreen ? g.toScreen(${x}, ${y}, ${z}) : null; const c = document.getElementById('bimble').getBoundingClientRect();
    if (p && document.elementFromPoint(p[0], p[1])?.id === 'bimble') return p; return [c.left + c.width / 2, c.top + c.height / 2]; })()`);
  return p;
}
/** A canvas point that picks an element (scans a grid with __bbGl.pickAt). */
async function elementPoint() {
  return ev(`(() => { const g = window.__bbGl, c = document.getElementById('bimble'), r = c.getBoundingClientRect();
    for (let fy = 0.5; fy < 0.95; fy += 0.07) for (let fx = 0.5; fx < 0.95; fx += 0.07) for (const s of [1, -1]) {
      const x = r.left + r.width * (s > 0 ? fx : 1 - fx), y = r.top + r.height * fy;
      if (document.elementFromPoint(x, y)?.id !== 'bimble') continue;
      const id = g.pickAt ? g.pickAt(x, y) : null; // client coordinates
      if (id) return [x, y];
    }
    return [r.left + r.width / 2, r.top + r.height / 2]; })()`);
}

// ---- surfaces ----
// A step to reach a surface: {click: {roots, i, label}}, {key, mods}, {select}, {right}, {url}.
async function runSteps(steps) {
  const extra = steps.find((s) => s.url)?.url ?? "";
  await fresh(extra);
  for (const s of steps) {
    if (s.url) continue;
    if (s.click) {
      const at = await ev(`window.__ux.at(${JSON.stringify(s.click.roots)}, ${s.click.i}, ${JSON.stringify(s.click.label)})`);
      if (!at) return `step: "${s.click.label}" not found again`;
      await click(at);
    } else if (s.key) await key(s.key, s.mods ?? {});
    else if (s.select) await click(await elementPoint());
    else if (s.zoom) { const p = await elementPoint(); await S("Input.dispatchMouseEvent", { type: "mouseWheel", x: p[0], y: p[1], deltaX: 0, deltaY: -400 }); }
    else if (s.right) { const p = await elementPoint(); await click(p); await settle(3000, false); await click(p, { button: "right" }); }
    await settle(MODEL !== "small" ? 15000 : 4000, false);
  }
  return null;
}

const RIBBON = (await import(pathToFileURL(join(UI, "src/shell/ribbon-tabs.ts")).href)).RIBBON;
const TAB_IDS = [...readFileSync(join(UI, "src/shell/tabs.ts"), "utf8").matchAll(/\{ id: "(\w+)", group/g)].map((m) => m[1]);
let ACTIONS = [];

function bindingOf(surface, e) {
  const m = /^ribbon-tool-(\d+)-(\d+)$/.exec(e.bb);
  if (m && surface.tabId) {
    const t = RIBBON[surface.tabId]?.[m[1] - 1]?.tools?.[m[2] - 1];
    if (t) return t.page ? `page ${t.page}${t.action ? ` (action ${t.action})` : ""}` : t.panel ? `panel ${t.panel}` : t.dialog ? `dialog ${t.dialog}` : t.action ? `action ${t.action}` : "nothing (action null)";
  }
  if (/^ribbon-box-/.test(e.bb)) return `nothing (box; missing: ${e.missing})`;
  const fm = /^file-menu-(.+)$/.exec(e.bb);
  if (fm) return `action ${fm[1]}`;
  if (surface.kind === "key") return surface.binding;
  const a = ACTIONS.find((x) => x.label === e.label || x.label.replace(/…$/, "") === e.label.replace(/…$/, ""))
    // a palette row: "<label> <tab> <shortcut>" (the longest label that starts it)
    ?? (e.bb === "palette-item" ? ACTIONS.filter((x) => e.label.startsWith(`${x.label} `)).sort((p, q) => q.label.length - p.label.length)[0] : undefined);
  return a ? `action ${a.name}` : "";
}

const controls = [];
const discovered = new Map(); // signature -> surface
const surfaces = [];

function addSurface(s) { if (!discovered.has(s.sig)) { discovered.set(s.sig, s); surfaces.push(s); } }

async function enumerateBase() {
  await fresh();
  ACTIONS = JSON.parse((await core("query", "actions")) ?? "[]");
  const tabs = await ev(`window.__ux.list(['[data-bb="tabsbar"]'])`);
  addSurface({ sig: "quickbar", where: "Title row", roots: ['[data-bb="quickbar"]'], steps: [] });
  addSurface({ sig: "tabsbar", where: "Tabs", roots: ['[data-bb="tabsbar"]'], steps: [] });
  tabs.forEach((t, k) => addSurface({ sig: `ribbon:${t.label}`, where: `Ribbon › ${t.label.replace(/PREVIEW$/i, "").trim()}`, tabId: TAB_IDS[k], roots: ['[data-bb="ribbon"]'],
    steps: [{ click: { roots: ['[data-bb="tabsbar"]'], i: t.i, label: t.label } }] }));
  addSurface({ sig: "left", where: "Left panel", roots: ['[data-bb="left-panel"]'], steps: [] });
  addSurface({ sig: "viewtabs", where: "View tabs", roots: ['[data-bb="viewtabs"]'], steps: [] });
  addSurface({ sig: "viewport", where: "Viewport (view cube, buttons)", roots: ['[data-bb="viewport"]'], steps: [] });
  addSurface({ sig: "status", where: "Status bar", roots: ['[data-bb="statusbar"]'], steps: [] });
  addSurface({ sig: "selection", where: "Left panel, an element selected", roots: ['[data-bb="left-panel"]', '[data-bb="modifier"]'], steps: [{ select: true }] });
  addSurface({ sig: "ctx", where: "Viewport right-click menu", roots: ['[data-bb="viewport-menu"]'], steps: [{ right: true }] });
  addSurface({ sig: "plan", where: "Plan view (Niveau 1)", roots: ['[data-bb="viewport"]', '[data-bb="viewtabs"]'], steps: [{ url: "&view=plan:1" }] });
  addSurface({ sig: "keys", where: "Keys", kind: "keys", steps: [] });
  addSurface({ sig: "canvas", where: "Viewport gestures", kind: "canvas", steps: [] });
}

/** Which container appeared: a dialog, a page, a right panel or a menu (the next surface). */
function opened(a, b) {
  const out = [];
  if (b.dom.dialog && b.dom.dialog !== a.dom.dialog) out.push({ roots: [".bb-backdrop"], what: "dialog", sig: `dialog:${b.dom.dialog.split(":")[0]}` });
  if (b.dom.page && (b.dom.shell !== a.dom.shell || b.dom.pageName !== a.dom.pageName)) out.push({ roots: ['[data-bb="page"]', '[data-bb="properties"]'], what: "page", sig: `page:${b.dom.pageName}` });
  if (b.dom.panel && b.dom.panel !== a.dom.panel) out.push({ roots: ['[data-bb="right-panel"]'], what: "panel", sig: `panel:${b.dom.panel.split(":")[0]}` });
  if (b.dom.menu && !a.dom.menu && !b.dom.dialog) out.push({ roots: ['[role="menu"]'], what: "menu", sig: `menu:${b.dom.menu.split(":")[0]}` });
  // a tab with its own left panel (Setting out: the survey point's fields)
  if (b.dom.tab !== a.dom.tab && b.dom.props !== a.dom.props && b.dom.props) out.push({ roots: ['[data-bb="properties"]'], what: "side", sig: `side:${b.dom.tab}` });
  return out;
}

/** Drives a tool's gesture after it started: returns {ok, note, ms}. */
async function gesture(name, before) {
  // Measure: two column foot corners (snapped); Place: free spots inside the first bay (a corner
  // snaps to an axis crossing where a column stands already).
  const pts = name === "measure" ? [[-150, -150, 0], [5850, -150, 0]] : [[3000, 2530, 0], [4500, 4000, 0], [3000, 4000, 0], [4500, 2530, 0]]; // a slab: a diagonal
  const want = name === "measure" ? (c) => c.measurements > before.core.measurements : (c) => c.rev !== before.core.rev;
  let ms = null;
  for (let k = 0; k < (name === "measure" ? 2 : 4); k++) {
    const p = await modelPoint(...pts[k]);
    await S("Input.dispatchMouseEvent", { type: "mouseMoved", x: p[0], y: p[1], button: "none", buttons: 0 });
    await settle(4000, false);
    await arm();
    await click(p);
    const pr = await settle(MODEL !== "small" ? 15000 : 4000);
    const c = await coreSnap();
    if (want(c)) return { ok: true, note: `${k + 1} click(s) ${name === "measure" ? "store a measurement" : "place an element"}`, ms: pr.ms };
    ms = pr.ms;
  }
  if (name !== "measure") {
    await arm();
    await key("enter");
    const pr = await settle(4000);
    if (want(await coreSnap())) return { ok: true, note: "4 clicks and Enter place an element", ms: pr.ms };
  }
  const why = await ev(`JSON.stringify({ toolError: window.__bbGl?.toolError ?? null, measureError: window.__bbGl?.measureError ?? null, hover: window.__bbGl?.hover ?? null, toolCalls: window.__bbGl?.toolCalls, calls: window.__ux.calls.slice(-6).map((c) => c[0]) })`);
  const placing = await core("query", "placing");
  return { ok: false, note: `the tool starts, but ${name === "measure" ? "two clicks store no measurement" : "4 clicks and Enter place nothing"} (page: ${why}; placing: ${String(placing).slice(0, 200)})`, ms };
}

function verdictOf(e, before, after, eff, errs) {
  if (e.notYet || after.dom.notYetTip || (after.dom.naMain && after.dom.naMain !== before.dom.naMain)) return "says not available";
  if (errs.length) return "error";
  if (e.disabled) return eff.length ? "works" : "disabled";
  return eff.length ? "works" : "dead";
}

async function activate(surface, e) {
  if (surface.kind === "keys") { await arm(); await key(e.key, e.mods); return; }
  if (surface.kind === "canvas") return e.run();
  const at = await ev(`window.__ux.at(${JSON.stringify(surface.roots)}, ${e.i}, ${JSON.stringify(e.label)})`);
  if (!at) throw new Error("not found again");
  await S("Input.dispatchMouseEvent", { type: "mouseMoved", x: at[0], y: at[1], button: "none", buttons: 0 });
  await arm();
  await click(at);
}

function canvasGestures() {
  const g = [];
  g.push({ label: "Click an element (select)", binding: "pick, query select", run: async () => { const p = await elementPoint(); await arm(); await click(p); } });
  g.push({ label: "Wheel zoom", binding: "camera (nav.ts)", run: async () => { const p = await elementPoint(); await arm(); await S("Input.dispatchMouseEvent", { type: "mouseWheel", x: p[0], y: p[1], deltaX: 0, deltaY: -150 }); } });
  g.push({ label: "Drag (orbit, 10 steps of 6 px)", binding: "camera (nav.ts)", run: async () => {
    const p = await elementPoint(); await arm();
    await S("Input.dispatchMouseEvent", { type: "mousePressed", x: p[0], y: p[1], button: "left", buttons: 1, clickCount: 1 });
    for (let k = 1; k <= 10; k++) await S("Input.dispatchMouseEvent", { type: "mouseMoved", x: p[0] + 6 * k, y: p[1], button: "left", buttons: 1 });
    await S("Input.dispatchMouseEvent", { type: "mouseReleased", x: p[0] + 60, y: p[1], button: "left", buttons: 0, clickCount: 1 });
  } });
  g.push({ label: "Double-click empty ground", binding: "fit (nav.ts)", run: async () => { const p = await ev("(() => { const r = document.getElementById('bimble').getBoundingClientRect(); return [r.left + 12, r.bottom - 12]; })()"); await arm(); await click(p, { count: 2 }); } });
  g.push({ label: "View cube: click its centre", binding: "camera turn (viewcube.ts)", run: async () => { const p = await ev("(() => { const c = document.querySelector('canvas[aria-label=\"View cube\"]'); if (!c) return null; const r = c.getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; })()"); if (!p) throw new Error("no view cube"); await arm(); await click(p); } });
  g.push({ label: "View cube: click the letter N", binding: "camera turn (viewcube.ts)", run: async () => { const p = await ev("(() => { const g = window.__bbGl, c = document.querySelector('canvas[aria-label=\"View cube\"]'); const q = g.cubeLetterAt ? g.cubeLetterAt('N') : null; if (!q || !c) return null; const r = c.getBoundingClientRect(); return q[0] > r.left - 1 && q[0] < r.right + 1 ? q : [r.left + q[0], r.top + q[1]]; })()"); if (!p) throw new Error("no letter N"); await arm(); await click(p); } });
  return g.map((x, i) => ({ ...x, i, tag: "canvas", type: "gesture", bb: "", near: "viewport", disabled: false, notYet: false, missing: "", pressed: "" }));
}

async function keyRows() {
  const rows = await ev("(window.__bbKeys?.KEYS ?? []).map((r) => ({ keys: r.keys, key: r.key, ctrl: !!r.ctrl, shift: !!r.shift, action: r.action ?? null, effect: r.effect ?? null, context: r.context }))");
  return rows.map((r, i) => ({ i, tag: "key", type: "key", label: r.keys, bb: "", near: r.context, disabled: false, notYet: false, missing: "", pressed: "",
    key: r.key, mods: { ctrl: r.ctrl, shift: r.shift }, binding: r.action ? `action ${r.action}` : `effect ${r.effect}`, context: r.context }));
}

async function exploreSurface(surface, depth) {
  const err = await runSteps(surface.steps);
  if (err) { controls.push({ where: surface.where, label: "(surface)", verdict: "not exercised", note: err }); return; }
  let list;
  if (surface.kind === "keys") list = await keyRows();
  else if (surface.kind === "canvas") list = canvasGestures();
  else list = await ev(`window.__ux.list(${JSON.stringify(surface.roots)})`);
  if (surface.skipLabels) list = list.filter((e) => !surface.skipLabels.has(`${e.bb}|${e.label}`));
  log(`${surface.where}: ${list.length} controls`);
  let dirty = false;
  for (const e of list) {
    try { dirty = await exploreControl(surface, depth, list, e, dirty); } catch (x) {
      const rec = controls[controls.length - 1];
      if (rec && rec.verdict === null) { rec.verdict = "not exercised"; rec.note = `the tool failed here: ${String(x.message ?? x).slice(0, 120)}`; }
      log(`${surface.where} › ${e.label}: ${x.message}`);
      dirty = true;
    }
  }
}
async function exploreControl(surface, depth, list, e, dirty) {
  {
    const rec = { where: surface.where, group: e.group || "", label: e.label || `(${e.tag} ${e.bb || e.type})`, bb: e.bb, binding: e.binding ?? bindingOf(surface, e),
      enabled: !e.disabled && !e.notYet, notYet: e.notYet, missing: e.missing, steps: surface.steps, roots: surface.roots, i: e.i, kind: surface.kind ?? "dom", verdict: null, effects: [], errors: [], note: "", ms: [] };
    controls.push(rec);
    if (["input", "select", "textarea"].includes(e.tag) && !["checkbox", "radio", "file"].includes(e.type)) { rec.verdict = "not exercised"; rec.note = `a ${e.tag === "input" ? `${e.type || "text"} field` : e.tag}: typing a value is not driven`; return dirty; }
    if (surface.kind === "keys" && e.context === "2d") rec.steps = [{ url: "&view=plan:1" }];
    if (surface.kind === "keys" && (e.label === "Delete" || e.label === "Escape")) rec.steps = [{ select: true }];
    if (rec.steps !== surface.steps || dirty) {
      const se = await runSteps(rec.steps);
      if (se) { rec.verdict = "not exercised"; rec.note = se; dirty = true; return true; }
    }
    const before = await snap();
    try { await activate(surface, e); } catch (x) { rec.verdict = "not exercised"; rec.note = String(x.message ?? x).slice(0, 120); dirty = true; return true; }
    const pr = await settle(MODEL !== "small" ? 20000 : 5000);
    const after = await snap();
    rec.effects = effects(before, after);
    rec.errors = errorsBetween(before, after);
    rec.calls = BUILD === "wasm" ? after.calls - before.calls : after.ev.rq - before.ev.rq;
    rec.verdict = verdictOf(e, before, after, rec.effects, rec.errors);
    if (rec.verdict === "dead" && e.current) { rec.verdict = "not exercised"; rec.note = "already the current one: a click changes nothing by design"; }
    // a key whose action the core does not enable now (undo with nothing to undo): disabled, not dead
    const ka = ACTIONS.find((x) => `action ${x.name}` === rec.binding);
    if (rec.verdict === "dead" && surface.kind === "keys" && ka && !ka.enabled) { rec.verdict = "disabled"; rec.note = `core: action ${ka.name} enabled=false now`; }
    // a camera control on a fresh page (already fitted, at Home): again after a wheel zoom
    if (rec.verdict === "dead" && /\bfit\b|\bhome\b|camera|view cube/i.test(`${rec.binding} ${rec.label}`)) {
      await runSteps(rec.steps);
      const p = await elementPoint();
      await S("Input.dispatchMouseEvent", { type: "mouseWheel", x: p[0], y: p[1], deltaX: 0, deltaY: -400 });
      await settle(MODEL !== "small" ? 15000 : 4000, false);
      const b2 = await snap();
      await activate(surface, e);
      const pr2 = await settle(MODEL !== "small" ? 20000 : 5000);
      const a2 = await snap();
      const eff2 = effects(b2, a2), err2 = errorsBetween(b2, a2);
      if (eff2.length || err2.length) {
        rec.effects = eff2, rec.errors = err2, rec.verdict = err2.length ? "error" : "works", rec.note = "after a wheel zoom (a fresh page is fitted already)";
        rec.steps = [...rec.steps, { zoom: true }];
        if (pr2.ms !== null) rec.ms = [pr2.ms];
      }
    }
    if (after.dom.naSide && !before.dom.naSide) rec.note = [rec.note, "the left panel then says not available"].filter(Boolean).join("; ");
    if (rec.verdict === "works" && pr.ms !== null) rec.ms.push(pr.ms);
    if (pr.timeout) rec.note = "still changing after the wait (timeout)";
    // a tool: its gesture decides
    const tool = ACTION_OF(rec.binding);
    if (rec.verdict === "works" && tool && (tool === "measure" || tool.startsWith("place_"))) {
      const on = tool === "measure" ? JSON.parse(after.core.measure ?? "null")?.active === true : true;
      if (on) {
        const g = await gesture(tool, after);
        rec.gesture = g;
        if (!g.ok) { rec.verdict = "dead"; rec.note = g.note; } else rec.note = g.note;
      }
    }
    if (e.disabled) { const a = ACTIONS.find((x) => `action ${x.name}` === rec.binding); rec.note = [rec.note, a ? `core: action ${a.name} enabled=${a.enabled}` : "core: no action"].filter(Boolean).join("; "); }
    if (rec.verdict === "dead" && rec.calls) rec.note = [rec.note, `${rec.calls} core call(s), nothing changed`].filter(Boolean).join("; ");
    if (rec.verdict === "works" && depth < 2 && surface.kind !== "keys" && surface.kind !== "canvas") {
      for (const o of opened(before, after)) {
        const steps = [...surface.steps, { click: { roots: surface.roots, i: e.i, label: e.label } }];
        addSurface({ sig: o.sig, where: `${o.what === "page" ? "Page" : o.what === "dialog" ? "Dialog" : o.what === "panel" ? "Right panel" : o.what === "side" ? "Left panel of the tab" : "Menu"} ${e.label} (from ${surface.where})`, roots: o.roots, steps, depth: depth + 1,
          skipLabels: o.what === "page" ? new Set(list.map((x) => `${x.bb}|${x.label}`)) : undefined });
      }
    }
    return rec.effects.length > 0 || rec.errors.length > 0 || !!rec.gesture;
  }
}
const ACTION_OF = (b) => (/^(?:action|page \S+ \(action) (\w+)/.exec(b ?? "")?.[1] ?? null);

// ---- timings ----
async function timeControl(rec, reps) {
  const out = [];
  for (let r = 0; r < reps; r++) {
    const se = await runSteps(rec.steps);
    if (se) return { ms: out, note: se };
    const surface = { kind: rec.kind === "dom" ? undefined : rec.kind, roots: rec.roots };
    let e;
    if (rec.kind === "keys") e = (await keyRows())[rec.i];
    else if (rec.kind === "canvas") e = canvasGestures()[rec.i];
    else e = { i: rec.i, label: rec.label };
    try { await activate(surface, e); } catch (x) { return { ms: out, note: String(x.message ?? x) }; }
    const pr = await settle(MODEL !== "small" ? 30000 : 6000);
    out.push(pr.ms);
  }
  return { ms: out };
}
const median = (a) => { const s = a.filter((x) => Number.isFinite(x)).sort((p, q) => p - q); if (!s.length) return null; const m = s.length >> 1; return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2; };

// ---- Measure, profiled ----
async function measureStudy() {
  const load = loadNow();
  await fresh();
  await arm();
  await key("m");
  await settle(5000);
  const on = JSON.parse((await core("query", "measure")) ?? "null")?.active === true;
  const tris = await ev("window.__bbGl.drawnTris");
  const rect = await ev("(() => { const r = document.getElementById('bimble').getBoundingClientRect(); return [r.left, r.top, r.width, r.height]; })()");
  const start = await elementPoint();
  const moves = [];
  const step = async (x, y, clickIt) => {
    const c0 = BUILD === "wasm" ? await ev("window.__ux.calls.length") : 0, f0 = await ev("window.__bbGl.frames"), rq0 = events.requests;
    await arm();
    if (clickIt) await click([x, y]);
    else await S("Input.dispatchMouseEvent", { type: "mouseMoved", x, y, button: "none", buttons: 0 });
    const pr = await settle(MODEL !== "small" ? 30000 : 6000);
    const calls = BUILD === "wasm" ? await ev(`window.__ux.calls.slice(${c0}).map((c) => [c[0], Math.round(c[2] * 10) / 10])`) : [];
    const f1 = await ev("window.__bbGl.frames");
    const what = pr.ms !== null && pr.ms > 100 ? await ev("window.__ux.lastMutWhat ?? null") : undefined; // MEAS1: a slow one's last DOM change
    return { ms: pr.ms, calls, frames: f1 - f0, requests: events.requests - rq0, click: clickIt, ...(what ? { what } : {}) };
  };
  for (let k = 0; k < 20; k++) {
    const x = Math.min(rect[0] + rect[2] - 5, Math.max(rect[0] + 5, start[0] + (k - 10) * 9)), y = start[1] + ((k % 5) - 2) * 7;
    moves.push(await step(x, y, false));
  }
  const clicks = [await step(start[0] - 40, start[1], true), await step(start[0] + 40, start[1], true)];
  // one profile over 10 more moves
  let profile = [];
  try {
    await S("Profiler.enable");
    await S("Profiler.setSamplingInterval", { interval: 200 });
    await S("Profiler.start");
    for (let k = 0; k < 10; k++) { await S("Input.dispatchMouseEvent", { type: "mouseMoved", x: start[0] + k * 11 - 50, y: start[1] + 3, button: "none", buttons: 0 }); await settle(MODEL !== "small" ? 30000 : 6000, false); }
    const { profile: p } = await S("Profiler.stop");
    const self = new Map(), dt = p.timeDeltas, byId = new Map(p.nodes.map((n) => [n.id, n]));
    for (let k = 0; k < p.samples.length; k++) {
      const n = byId.get(p.samples[k]), f = n.callFrame, name = `${f.functionName || "(anonymous)"} ${f.url ? f.url.split("/").pop() : ""}${f.lineNumber >= 0 ? `:${f.lineNumber + 1}` : ""}`.trim();
      self.set(name, (self.get(name) ?? 0) + (dt[k] ?? 0) / 1000);
    }
    const total = [...self.values()].reduce((a, b) => a + b, 0);
    profile = [...self.entries()].sort((a, b) => b[1] - a[1]).slice(0, 12).map(([n, ms]) => ({ fn: n, ms: Math.round(ms), share: total ? Math.round((100 * ms) / total) : 0 }));
    await S("Profiler.disable");
  } catch (x) { profile = [{ fn: `profiler: ${x.message}`, ms: 0, share: 0 }]; }
  let count = null;
  try { count = JSON.parse((await core("query", "storeys")) ?? "[]").reduce((a, x) => a + (x.element_count ?? 0), 0); } catch { /* unknown */ }
  return { on, tris, count, moves, clicks, profile, load, loadAfter: loadNow() };
}

// ---- the report ----
const VERDICTS = ["works", "says not available", "dead", "error", "disabled", "not exercised"];
const fmt = (n) => (n === null || n === undefined || !Number.isFinite(n) ? "–" : n >= 200 ? `**${Math.round(n)}** ⚠` : `${Math.round(n)}`);
const esc = (s) => String(s ?? "").replace(/\|/g, "\\|").replace(/\n/g, " ");
function writeReport(R) {
  const L = [];
  L.push("# The lab's controls: what works, and how fast (ticket UXMAP)", "",
    "Generated by `node ccore/tools/ux.mjs`. Do not edit by hand. The study: `docs/research/ux-map.md`; the goal: ADR 0074.", "",
    "**What this report supports.** Each control the lab shows (enumerated from the page: ribbon tabs and tools, title row, tabs, left panel, view tabs, viewport buttons, status bar, the selection's panels, the right-click menu, the key table, and every dialog, page and right panel a control opens, two levels deep) was activated with real input on a freshly loaded page, and its verdict says what changed: the core's state (the undo list, actions, measure, visibility, open views, history), the shell, a dialog, page or panel, the page's text, the camera, a file chooser or a download. A tool counts as working only when its gesture gives its result.", "",
    `**What it does not support.** That a working control does the *right* thing (only that something observable happened); controls three levels deep (inside a dialog opened from a page opened from a menu); text fields and selects (not typed into); the small-screen layout; French; any machine but this one. A response time is headless Chromium with ${LAB_GPU === "software" ? "SwiftShader (CPU rasterization): a GPU would draw faster" : `the machine's GPU (LAB_GPU=${LAB_GPU}, ${GPU_SEEN ?? "?"})`}.`, "",
    "**Response** (ADR 0074): from the input event (the page's pointerdown / keydown / wheel timeStamp) to the end of the animation-frame callbacks of the first frame that holds the last effect (the last DOM change, WebGL draw or core call, followed by 300 ms with none); the frame is then handed to the compositor. Median of the repetitions, each on a freshly loaded page (and a freshly started host). Over 200 ms: **bold** ⚠.", "");
  L.push(`- Commit: \`${R.commit}\`${R.dirty ? " (with uncommitted changes)" : ""}; run ${R.date}; ${R.runS} s.`,
    `- Machine: ${R.machine}; load at start \`${R.load0}\`, before the timings \`${R.loadT ?? "-"}\`.${R.numbersNote ? ` ${R.numbersNote}` : ""}`,
    `- Builds: ${R.builds.join(", ")}; models: small = \`golden/frame.json\` (72 elements)${R.models.includes("large") ? `, large = \`bench/large_model.py ${LARGE_N}\` (${R.measure?.find((m) => m.model === "large")?.count ?? R.largeCount ?? "?"} elements)` : ""}${R.models.includes("local") ? `, local = a local model, ${R.localCount ?? "?"} elements (not committed)` : ""}; repetitions: ${REPS}.`,
    ...(R.merged ? [`- Runs: ${R.merged}.`] : []),
    `- Page loads (fresh page per activation): median ${R.loadMedian} ms (${R.loads} loads).`, "");
  // counts per where
  const C = R.passes[0]?.controls ?? [];
  L.push(`## Verdicts (${R.passes[0]?.build ?? "-"}, small model)`, "");
  const wheres = [...new Set(C.map((c) => c.where))];
  L.push(`| Where | ${VERDICTS.join(" | ")} | total |`, `|---|${VERDICTS.map(() => "---:").join("|")}|---:|`);
  const tot = Object.fromEntries(VERDICTS.map((v) => [v, 0]));
  for (const w of wheres) {
    const row = VERDICTS.map((v) => C.filter((c) => c.where === w && c.verdict === v).length);
    row.forEach((n, k) => (tot[VERDICTS[k]] += n));
    L.push(`| ${esc(w)} | ${row.join(" | ")} | ${row.reduce((a, b) => a + b, 0)} |`);
  }
  L.push(`| **all** | ${VERDICTS.map((v) => `**${tot[v]}**`).join(" | ")} | **${C.length}** |`, "");
  for (const p of R.passes.slice(1)) {
    const t = Object.fromEntries(VERDICTS.map((v) => [v, p.controls.filter((c) => c.verdict === v).length]));
    L.push(`${p.build}, ${p.model} model: ${VERDICTS.map((v) => `${v} ${t[v]}`).join(" · ")} (of ${p.controls.length}).`, "");
    const diff = p.controls.filter((c) => { const o = C.find((x) => x.where === c.where && x.label === c.label && x.i === c.i); return o && o.verdict !== c.verdict; });
    if (diff.length) L.push(`Verdicts that differ from ${R.passes[0].build}: ${diff.map((c) => `${esc(c.where)} › ${esc(c.label)}: ${C.find((x) => x.where === c.where && x.label === c.label && x.i === c.i).verdict} → ${c.verdict}${c.errors?.length ? ` (${esc(c.errors[0]).slice(0, 100)})` : ""}`).join("; ")}.`, "");
  }
  for (const v of ["dead", "error", "says not available", "disabled", "not exercised"]) {
    const xs = C.filter((c) => c.verdict === v);
    L.push(`## ${v[0].toUpperCase() + v.slice(1)} (${xs.length})`, "");
    if (!xs.length) { L.push("None.", ""); continue; }
    L.push("| Where | Group | Control | data-bb | Bound to | Note |", "|---|---|---|---|---|---|");
    for (const c of xs) L.push(`| ${esc(c.where)} | ${esc(c.group)} | ${esc(c.label)} | ${esc(c.bb)} | ${esc(c.binding)} | ${esc([c.note, ...(c.errors ?? []).slice(0, 2)].filter(Boolean).join("; ")).slice(0, 300)} |`);
    L.push("");
  }
  // works + timings
  const works = C.filter((c) => c.verdict === "works");
  const cols = R.timingCols;
  L.push(`## Works (${works.length}) and response times`, "", "One control per binding is timed (the palette, the keys and the ribbon often run the same action: the others show its number). On the large model only the ribbon, the keys, the viewport and the title row are timed (a page load takes seconds there); \"–\": not timed.", "");
  if (R.numbersNote && !cols.length) L.push(R.numbersNote, "");
  // one sample per build from the verdict pass (the activation that gave the verdict), then the medians
  const others = R.passes.slice(1).filter((p) => p.model === "small");
  if (R.oneRunLoad === undefined) { R.oneRunLoad = Number(String(R.load0).split("this run ").pop().split(" ")[0]); R.noOneRun = R.oneRunLoad > 4 && !FORCE; }
  const one = (p, c) => { const o = p.controls.find((x) => x.where === c.where && x.label === c.label && x.i === c.i); return o?.verdict === "works" && !R.noOneRun ? o.ms?.[0] : undefined; };
  if (R.noOneRun) { L.push(`The verdict pass started at a 1-minute load of ${R.oneRunLoad} (> 4): its one-run times are not written (--force).`, ""); for (const c of works) c.ms = []; }
  else L.push(`"ms 1 run": the one activation of the verdict pass (a fresh page; 1-minute load at its start ${R.oneRunLoad}${R.oneRunLoad > 4 ? ", above 4: written with --force, indicative only" : ""}): an indication, not a median. "(median)": --reps repetitions.`, "");
  const heads = [`ms 1 run ${R.passes[0]?.build ?? "-"}`, ...others.map((p) => `ms 1 run ${p.build}`), ...cols.map((c) => `ms ${c} (median)`)];
  L.push(`| Where | Control | Bound to | Effect | ${heads.join(" | ")} |`, `|---|---|---|---|${heads.map(() => "---:").join("|")}|`);
  for (const c of works) {
    const k = `${c.where}|${c.label}|${c.i}`;
    const cells = [fmt(c.ms?.[0]), ...others.map((p) => fmt(one(p, c))), ...cols.map((col) => fmt(R.times[col]?.[k]))];
    L.push(`| ${esc(c.where)} | ${esc(c.label)} | ${esc(c.binding)} | ${esc(c.effects.slice(0, 4).join(", "))}${c.gesture ? `; ${esc(c.gesture.note)}` : ""} | ${cells.join(" | ")} |`);
  }
  L.push("");
  const over = [];
  for (const c of works) {
    if (Number.isFinite(c.ms?.[0]) && c.ms[0] > 200) over.push([c, `1 run ${R.passes[0]?.build ?? "-"}`, c.ms[0]]);
    for (const p of others) { const v = one(p, c); if (Number.isFinite(v) && v > 200) over.push([c, `1 run ${p.build}`, v]); }
    for (const col of cols) { const v = R.times[col]?.[`${c.where}|${c.label}|${c.i}`]; if (Number.isFinite(v) && v > 200) over.push([c, col, v]); }
  }
  L.push(`### Over 200 ms (${over.length} cells)`, "");
  L.push(over.length ? over.map(([c, col, v]) => `- ${esc(c.where)} › ${esc(c.label)} (${col}): ${Math.round(v)} ms`).join("\n") : "None.", "");
  // measure
  L.push("## Measure, per pointer move (ticket UXMAP 3)", "");
  if (!R.measure.length) L.push("Not run (--no-measure or --check).", "");
  for (const m of R.measure) {
    const mv = m.moves.map((x) => x.ms), cl = m.clicks.map((x) => x.ms);
    const callMs = (xs) => { const t = new Map(); for (const s of xs) for (const [n, ms] of s.calls) t.set(n, [(t.get(n)?.[0] ?? 0) + 1, (t.get(n)?.[1] ?? 0) + ms]); return [...t.entries()].map(([n, [k, ms]]) => `${n} ×${(k / xs.length).toFixed(1)} ${(ms / xs.length).toFixed(1)} ms`).join(", "); };
    const named = { small: "small (golden/frame.json)", large: `large (bench/large_model.py ${LARGE_N})`, heavy: `heavy (bench/mesh_heavy_model.py ${HEAVY} ${HEAVY_TRIS})`, local: "a local model (not committed)" }[m.model] ?? m.model;
    L.push(`### ${m.build}, ${named}: ${m.count ?? "?"} elements, ${m.tris} triangles drawn, tool on: ${m.on}`, "",
      `- Load (/proc/loadavg) before / after: \`${m.load ?? "?"}\` / \`${m.loadAfter ?? "?"}\`${Number(String(m.load).split(" ")[0]) > 4 ? " (above 4: the numbers are indicative, the shares of the profile hold)" : ""}.`,
      `- Pointer move → frame: median ${fmt(median(mv))} ms, max ${fmt(Math.max(...mv.filter(Number.isFinite)))} ms (${mv.length} moves); frames drawn per move: median ${median(m.moves.map((x) => x.frames))}.`,
      `- Click → frame: ${cl.map(fmt).join(", ")} ms.`,
      ...(m.moves.some((x) => x.what) ? [`- Moves over 100 ms and their last DOM change: ${m.moves.map((x, k) => (x.what ? `move ${k + 1} ${Math.round(x.ms)} ms: ${esc(x.what)}` : null)).filter(Boolean).join("; ")}.`] : []),
      m.build === "wasm" ? `- Core calls per move (mean count and ms): ${callMs(m.moves) || "none"}.` : `- Requests to the host per move: median ${median(m.moves.map((x) => x.requests))}.`,
      `- Profile of 10 more moves (CDP Profiler, self time): ${m.profile.map((p) => `${esc(p.fn)} ${p.ms} ms (${p.share} %)`).join("; ")}.`, "");
  }
  writeFileSync(OUT, L.join("\n"));
}

// ---- main ----
async function main() {
  if (flag("--report-only")) { // the report again from a run's JSON (--merge FILE), no browser
    const R = JSON.parse(readFileSync(resolve(opt("--merge")), "utf8"));
    if (!/UTC/.test(R.date)) R.date += " UTC";
    writeReport(R);
    log(`wrote ${OUT} from ${opt("--merge")}`);
    return;
  }
  const load0 = loadNow();
  let numbersNote = "";
  if (TIMING && load1() > 4 && !FORCE) numbersNote = `The 1-minute load was ${load1()} (> 4) at the start: no response times written (--force to write them anyway).`;
  const site = buildSite();
  let largeCount = null;
  const all = new Set([...MODELS, ...(MEASURE ? MEASURE_MODELS : [])]);
  if (all.has("large")) { const f = largeModel(site); largeCount = LARGE_N; log(`large model ${f}`); }
  if (all.has("heavy")) heavyModel(site);
  if (all.has("local")) localModel(site);
  await serve(site);
  await startBrowser();
  // --merge FILE: an earlier run's JSON (out/ux/ux.json). Its verdicts are reused (no new verdict pass
  // for a build it has), its Measure rows and times kept where this run has none: verdicts, the
  // Measure study and the timings can be run one at a time (one browser, a load that changes).
  const M = opt("--merge") ? JSON.parse(readFileSync(resolve(opt("--merge")), "utf8")) : null;
  const passes = M && !flag("--fresh-verdicts") ? [...M.passes] : []; // --fresh-verdicts: new verdicts, the merged times and Measure
  const times = M ? { ...(M.times ?? {}) } : {};
  const timingCols = M ? [...(M.timingCols ?? [])] : [];
  const measure = [];
  let loadT = M?.loadT ?? null;
  for (const build of BUILDS) {
    BUILD = build; MODEL = "small";
    if (build === "native" && !existsSync(BIN)) { log(`no ${BIN}: native pass skipped`); continue; }
    let pass = passes.find((p) => p.build === build && p.controls.length);
    if (!pass && !MEASURE_ONLY && !flag("--timing-only")) {
      controls.length = 0; surfaces.length = 0; discovered.clear();
      await enumerateBase();
      for (let s = 0; s < surfaces.length; s++) {
        if (opt("--only") && !surfaces[s].where.includes(opt("--only"))) continue; // a debugging aid: one surface
        try { await exploreSurface(surfaces[s], surfaces[s].depth ?? 0); } catch (x) {
          log(`${surfaces[s].where}: ${x.message}`);
          controls.push({ where: surfaces[s].where, label: "(the rest of this surface)", verdict: "not exercised", note: `the tool failed here: ${String(x.message ?? x).slice(0, 120)}`, effects: [], errors: [], ms: [] });
        }
        writeFileSync(join(RAW, "ux-partial.json"), JSON.stringify(controls, null, 1));
      }
      pass = { build, model: "small", controls: controls.map((c) => ({ ...c })) };
      passes.push(pass);
      writeFileSync(join(RAW, "ux.json"), JSON.stringify({ passes, times, timingCols, measure }, null, 1));
      log(`${build}/small: ${VERDICTS.map((v) => `${v} ${pass.controls.filter((c) => c.verdict === v).length}`).join(", ")}`);
    }
    if (MEASURE) for (const model of MEASURE_MODELS) { MODEL = model; log(`measure ${build}/${model}`); measure.push({ build, model, ...(await measureStudy()) }); }
    if (TIMING && !MEASURE_ONLY && pass) {
      loadT = loadNow();
      if (load1() > 4 && !FORCE) { numbersNote = `The 1-minute load was ${load1()} (> 4) before the timings: no response times written (--force to write them anyway).`; continue; }
      // one control per binding: the first that works (the palette, the keys and the ribbon often run the same action)
      const seen = new Set();
      const todo = pass.controls.filter((c) => c.verdict === "works" && c.binding !== undefined).filter((c) => { const b = c.binding && /^(action|page|panel|dialog|effect)/.test(c.binding) ? c.binding : `${c.where}|${c.label}`; if (seen.has(b)) return false; seen.add(b); return true; });
      for (const model of MODELS) {
        MODEL = model;
        const col = `${model} ${build}`;
        if (!timingCols.includes(col)) timingCols.push(col);
        times[col] = {};
        // the large model: the ribbon, the keys, the viewport and the title row only (a page load is
        // seconds there: every control would take an hour); --large-all for all of them
        const set = model === "small" || flag("--large-all") ? todo : todo.filter((c) => /^(Ribbon|Keys|Viewport|Title row)/.test(c.where));
        log(`timing ${col}: ${set.length} controls × ${REPS}`);
        for (const c of set) {
          const k = `${c.where}|${c.label}|${c.i}`;
          const reps = model === "small" ? Math.max(0, REPS - (c.ms?.length ?? 0)) : REPS;
          const r = await timeControl(c, reps);
          const all = model === "small" ? [...(c.ms ?? []), ...r.ms] : r.ms;
          times[col][k] = median(all);
          for (const p of pass.controls) if (p.binding === c.binding && p.verdict === "works" && p !== c && /^(action|page|panel|dialog|effect)/.test(c.binding ?? "")) times[col][`${p.where}|${p.label}|${p.i}`] ??= times[col][k];
        }
        writeFileSync(join(RAW, "ux.json"), JSON.stringify({ passes, times, timingCols, measure }, null, 1));
      }
      MODEL = "small";
    }
  }
  for (const m of M?.measure ?? []) if (!measure.some((x) => x.build === m.build && x.model === m.model)) measure.push(m);
  const commit = execFileSync("git", ["rev-parse", "--short", "HEAD"], { cwd: CCORE }).toString().trim();
  const dirty = execFileSync("git", ["status", "--porcelain", "--", "../web-ui/src", "src", "session"], { cwd: CCORE }).toString().trim() !== "";
  const R = { commit, dirty, date: `${new Date().toISOString().slice(0, 16).replace("T", " ")} UTC`, runS: Math.round((Date.now() - T0) / 1000) + (M?.runS ?? 0),
    machine: `${platform()} ${release()} ${arch()}, ${cpus().length} CPUs, ${Math.round(totalmem() / 2 ** 30)} GB`, load0: M ? `${M.load0}; this run ${load0}` : load0, loadT, numbersNote,
    // the verdict pass's one-run times obey the same rule as the medians: none above load 4
    oneRunLoad: Number(load0.split(" ")[0]), noOneRun: Number(load0.split(" ")[0]) > 4 && !FORCE,
    builds: [...new Set([...(M?.builds ?? []), ...BUILDS])], models: [...new Set([...(M?.models ?? []), ...MODELS])], largeCount, localCount: measure.find((m) => m.model === "local")?.count ?? null,
    loadMedian: median([...loadMs, ...(M?.loadMs ?? [])]), loads: loadMs.length + (M?.loadMs?.length ?? 0), loadMs: [...loadMs, ...(M?.loadMs ?? [])], passes, times, timingCols, measure };
  if (M) R.merged = `this report merges runs (--merge): ${M.merged ?? `one at commit ${M.commit} (${M.date})`}; and this one at ${commit} (${R.date})`;
  writeFileSync(join(RAW, EXTRA ? "ux-local.json" : "ux.json"), JSON.stringify(R, null, 1));
  if (CHECK) {
    const C = passes[0].controls, n = (v) => C.filter((c) => c.verdict === v).length;
    const old = existsSync(OUT) ? readFileSync(OUT, "utf8") : "";
    const was = (v) => Number(new RegExp(`^## ${v[0].toUpperCase() + v.slice(1)} \\((\\d+)\\)`, "m").exec(old)?.[1] ?? Infinity);
    log(`check: dead ${n("dead")} (report ${was("dead")}), error ${n("error")} (report ${was("error")})`);
    if (n("dead") > was("dead") || n("error") > was("error")) process.exitCode = 1;
    return;
  }
  writeReport(R);
  log(`wrote ${OUT} in ${R.runS} s`);
}

process.on("SIGINT", async () => { await cleanup(); process.exit(130); });
try { await main(); } catch (e) { console.error(`ux: ${e.stack ?? e}`); process.exitCode = 1; } finally { await cleanup(); }
