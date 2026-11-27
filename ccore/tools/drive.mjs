// Drive the lab with real gestures on the machine's GPU and record where the time goes (ticket VIS1).
//
//   node ccore/tools/drive.mjs [what] [options] [--preset orbit-zoom|open|walk-inside | --script FILE.json | --do '<steps>']
// what (one):
//   --model PATH          a model (.bimble, .ifc, .json) opened by the native host (a copy in the run's temp folder)
//   --project PATH        a .bimble project of links: read where it is through a scratch folder of symbolic links
//                         (any file the host writes lands in the scratch folder, never beside your files)
//   --links N             N synthetic buildings (bench/buildings_synth.mjs) linked as a folder into a new project,
//                         placed in rows; --storeys S (2). Kept in ~/.cache/bimble-drive/synth-N-S between runs
//                         (--clean deletes it at the end)
// options:
//   --name NAME           the run's folder: ccore/out/drive/NAME (default: the preset and the time)
//   --size 1492x790       the window in CSS px (default 1280x800); --dpr 2 its device pixel ratio (default 1)
//   --settings JSON       stored settings for the run, e.g. '{"moving_fps":0}' (the host's settings.json, a temp copy)
//   --gpu vulkan|gl|software  LAB_GPU for this run (default vulkan; "gl" is ANGLE on OpenGL ES, as Chromium on Linux
//                         draws by default); numbers are labelled "real GPU" only when the adapter is not a CPU rasteriser
//   --panel               opens the performance panel (its own GPU wait per frame is then in every frame)
//   --strip N             a screenshot every N frames while moving and during the refill after a stop (0: none, the
//                         default: a screenshot costs tens of ms and changes what it measures)
//   --no-profile          no CPU profile (the Profiler domain samples at 100 µs and costs a few % of script time)
//   --query 'a=1&b=2'     more lab URL parameters
//   --compare A B         prints a before/after table from two run folders (their summary.json), nothing else
//   --mcp                 a stdio MCP server (JSON-RPC 2.0, protocol 2024-11-05): tools open, gesture, screenshot,
//                         report, profile, close; the same functions as the command line, one session held open
// Gesture steps (a JSON array of strings, or --do with steps separated by ';'):
//   wait ready|first-picture|complete|idle   ready: the page drew with every link's detail loaded; first-picture: any
//                         picture (stand-ins or detail); complete: the complete picture; idle: no frame for 500 ms
//   orbit DX DY MS [pause]  a held left drag of DX, DY CSS px over MS ms, moves at 60 Hz (fire and forget, as the OS
//                         sends them); "pause" adds 2 human pauses of 200 ms
//   pan DX DY MS          the same with the middle button
//   zoom STEPS MS         STEPS wheel notches (negative: out) spread over MS ms, at the centre
//   click X Y | click element GLOBALID | click centre     a left click (an element: at its box's centre on screen)
//   key KEY               a key press (KeyboardEvent.key, e.g. "Escape", "f")
//   fit                   the home camera fitted (the page's bench hook)
//   action NAME [JSON]    a host action (POST /action/NAME), e.g. "action hide {}" after a click selected something
//   open panel performance | close panel   the performance panel
//   screenshot NAME       a PNG (device px) and a copy at most 1280 px wide
//   sleep MS; mark LABEL  a pause; a label in the logs (and a screenshot)
//   repeat N { steps }    the steps N times (one level)
// Recorded into ccore/out/drive/NAME/ (git-ignored; files may hold your file names: never commit them):
//   report.txt (the performance panel's "Copy report" text, read from perf.ts), frames.json and frames.tsv (every frame:
//   kind, script ms, GPU ms by timer query or fence, draws, triangles, culled batches, proxies, outlines), inputs.json
//   (each input's timestamp and the end of the frame that showed it), profile.cpuprofile and profile.txt (the top 20
//   functions by self time), memory.tsv (every 250 ms: JS heap, GPU bytes, renderer, GPU process and host RSS),
//   network.json (every /mesh request: bytes, ms), console.txt, PNGs, summary.md and summary.json.
// Memory: waits for 3 GB available before Chromium starts; a watchdog kills Chromium under 2.5 GB available; one
// Chromium; a project or links over 1 GB takes the shared lock ~/.cache/bimble-measure.lock (flock) itself. Temp
// folders on disk under ~/.cache/bimble-drive/ (not /tmp: memory here), removed on any exit.
import { spawn } from "node:child_process";
import { createInterface } from "node:readline";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, statSync, symlinkSync, writeFileSync, copyFileSync, openSync, closeSync } from "node:fs";
import { basename, dirname, extname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { port, vitePort } from "../test/ports.mjs";
import { CHROMIUM, gpuFlags, RENDERER_EXPR } from "../test/lib/chromium.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = process.env.DRIVE_UI ? resolve(process.env.DRIVE_UI) : resolve(CCORE, "../web-ui"); // DRIVE_UI: another tree's web-ui (the code before a change)
const SCRATCH_ROOT = join(process.env.HOME, ".cache/bimble-drive");
const LOCK = join(process.env.HOME, ".cache/bimble-measure.lock");
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const memGb = () => Number(/MemAvailable:\s+(\d+)/.exec(readFileSync("/proc/meminfo", "utf8"))[1]) / 1048576;
const loadNow = () => readFileSync("/proc/loadavg", "utf8").trim().split(" ").slice(0, 3).join(" ");
const rssMb = (pid) => { try { return Number(/VmRSS:\s+(\d+)/.exec(readFileSync(`/proc/${pid}/status`, "utf8"))[1]) / 1024; } catch { return null; } };
const med = (xs) => { if (!xs.length) return null; const s = [...xs].sort((a, b) => a - b); return s[Math.floor(s.length / 2)]; };
const q95 = (xs) => { if (!xs.length) return null; const s = [...xs].sort((a, b) => a - b); return s[Math.min(s.length - 1, Math.floor(0.95 * s.length))]; };
const f1 = (v, d = 1) => (v === null || v === undefined || !Number.isFinite(v) ? "–" : Number(v).toLocaleString("en-US", { maximumFractionDigits: d, minimumFractionDigits: d }));
const f0 = (v) => f1(v, 0);

export const PRESETS = {
  // what the owner did: orbit 3 s, zoom in 5 steps, zoom out, stop, click an element, orbit again
  "orbit-zoom": ["wait ready", "wait idle", "screenshot start", "mark orbit", "orbit 600 120 3000 pause", "mark stop", "sleep 150", "screenshot after-stop-150ms",
    "wait idle", "screenshot after-stop-complete", "mark zoom-in", "zoom 5 1200", "wait idle", "mark zoom-out", "zoom -5 1200", "wait idle", "screenshot zoomed-out",
    "mark click", "click centre", "wait idle", "screenshot clicked", "mark orbit-again", "orbit -500 60 2500", "sleep 150", "screenshot after-orbit2-150ms", "wait idle",
    "screenshot end", "drawcalls"],
  open: ["wait first-picture", "screenshot first-picture", "wait ready", "screenshot ready", "wait complete", "wait idle", "screenshot complete"],
  "walk-inside": ["wait ready", "wait idle", "zoom 12 2000", "wait idle", "screenshot inside", "orbit 300 0 2000", "wait idle", "screenshot inside-turned", "zoom -12 2000", "wait idle"],
};

/** The page's instruments, before any of its scripts: inputs (timeStamp) and frame ends (end of the animation-frame
 *  callbacks in which the viewport's frame counter moved). */
const INSTR = String.raw`(() => {
  if (window.__drv) return;
  const d = window.__drv = { inputs: [], frames: [], errors: [] };
  addEventListener("error", (e) => { if (d.errors.length < 50) d.errors.push(String(e.message)); });
  const note = (what) => (e) => { if (d.inputs.length < 20000) d.inputs.push({ what, t: e.timeStamp, buttons: e.buttons ?? 0 }); };
  addEventListener("pointerdown", note("press"), true);
  addEventListener("pointermove", (e) => { if (e.buttons) note("drag")(e); }, true);
  addEventListener("pointerup", note("release"), true);
  addEventListener("wheel", note("wheel"), { capture: true, passive: true });
  addEventListener("keydown", note("key"), true);
  let f = -1;
  const raf = window.requestAnimationFrame.bind(window);
  window.requestAnimationFrame = (cb) => raf((ts) => { try { cb(ts); } finally { const g = window.__bbGl; const n = g ? g.frames : -1; if (n !== f) { f = n; d.frames.push({ n, end: performance.now() }); if (d.frames.length > 20000) d.frames.splice(0, 10000); } } });
})();`;

async function cdpConnect(wsUrl) {
  const ws = new WebSocket(wsUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = () => rej(new Error("CDP connect failed")); });
  let id = 0;
  const pending = new Map(), listeners = [];
  ws.onclose = () => { for (const { reject } of pending.values()) reject(new Error("CDP: the browser is gone")); pending.clear(); };
  ws.onmessage = (ev) => {
    const msg = JSON.parse(ev.data);
    if (msg.id !== undefined && pending.has(msg.id)) {
      const { ok, reject } = pending.get(msg.id);
      pending.delete(msg.id);
      if (msg.error) reject(new Error(`${msg.error.message} ${msg.error.data ?? ""}`));
      else ok(msg.result);
    } else if (msg.method) for (const fn of listeners) fn(msg);
  };
  const send = (method, params = {}, sessionId) => new Promise((ok, reject) => {
    if (ws.readyState > 1) return reject(new Error("CDP: the browser is gone"));
    const m = ++id;
    pending.set(m, { ok, reject });
    ws.send(JSON.stringify({ id: m, method, params, ...(sessionId ? { sessionId } : {}) }));
  });
  return { send, on: (fn) => listeners.push(fn), close: () => ws.close() };
}

/** One driven lab: the host, Vite, one Chromium, the page; everything it starts is killed by close(). */
export class Drive {
  constructor(o = {}) {
    this.o = { size: "1280x800", dpr: 1, gpu: "vulkan", storeys: 2, strip: 0, profile: true, ...o };
    this.procs = [];
    this.temps = [];
    this.log = (s) => (this.o.quiet ? null : console.error(`drive: ${s}`));
    this.marks = [];
    this.net = new Map();
    this.consoleLines = [];
    this.memory = [];
    this.shots = [];
    this.stopWhy = null;
    this.lockFd = null;
  }
  start(name, cmd, args, opts) {
    const p = spawn(cmd, args, { stdio: ["ignore", "pipe", "pipe"], detached: true, ...opts });
    p.out = "";
    const keep = (d) => { p.out += d; if (p.out.length > 400000) p.out = p.out.slice(-200000); };
    p.stdout.on("data", keep), p.stderr.on("data", keep);
    p.label = name;
    this.procs.push(p);
    return p;
  }
  async waitFor(what, ms, fn, every = 100) {
    const until = Date.now() + ms;
    let last;
    while (Date.now() < until) {
      if (this.stopWhy) throw new Error(`stopped: ${this.stopWhy}`);
      try { const v = await fn(); if (v) return v; } catch (e) { last = e; }
      await sleep(every);
    }
    throw new Error(`timeout waiting for ${what}${last ? `: ${last.message ?? last}` : ""}`);
  }
  hcall = async (op, name, body = {}) => {
    const r = await fetch(`http://127.0.0.1:${this.port}/${op}/${name}?t=${this.token}`, { method: "POST", body: JSON.stringify(body), headers: { "Content-Type": "text/plain" } });
    return { status: r.status, text: await r.text() };
  };
  async q(name, body = {}) { const r = await this.hcall("query", name, body); if (r.status !== 200) throw new Error(`${name}: ${r.text}`); return JSON.parse(r.text); }
  /** Takes the shared measuring lock: a child holds it (flock on its descriptor) until close() kills it. */
  async lock() {
    if (this.lockHolder) return;
    this.log(`taking the shared lock ${LOCK} (waits up to an hour)`);
    this.lockHolder = this.start("lock", "bash", ["-c", `exec 9>>"${LOCK}"; flock -w 3600 9 || exit 3; echo held; exec sleep 100000`]);
    await this.waitFor("the shared lock", 3700000, () => this.lockHolder.out.includes("held") || (this.lockHolder.exitCode !== null && Promise.reject(new Error("the shared lock was not free within an hour"))), 500);
  }

  async open() {
    const o = this.o;
    const [cw, ch] = String(o.size).split("x").map(Number);
    this.cw = cw, this.ch = ch;
    mkdirSync(SCRATCH_ROOT, { recursive: true });
    const tmp = mkdtempSync(join(SCRATCH_ROOT, "run-"));
    this.temps.push(tmp);
    const home = join(tmp, "home"), config = join(tmp, "config"), cache = join(tmp, "cache");
    for (const d of [home, config, cache]) mkdirSync(d, { recursive: true });
    if (o.settings) {
      const base = JSON.parse(readFileSync(join(CCORE, "golden/settings/default.json"), "utf8"));
      writeFileSync(join(config, "settings.json"), JSON.stringify({ ...base, ...JSON.parse(o.settings) }));
    }
    this.port = port(`drive_host_${process.pid}`), this.vite = vitePort(`drive_${process.pid}`), this.token = `drive-${process.pid}`;
    let what = null, bytes = 0, synthDir = null;
    if (o.links) {
      synthDir = join(SCRATCH_ROOT, `synth-${o.links}-${o.storeys}`);
      mkdirSync(join(synthDir, "links"), { recursive: true });
    } else if (o.project) {
      // a scratch folder of symbolic links to every file beside the project: what the host writes (a cache, a
      // session) replaces a link in the scratch folder (written to a temp file, then renamed), never the original
      const src = dirname(resolve(o.project)), mirror = join(tmp, "project");
      mkdirSync(mirror);
      for (const f of readdirSync(src)) {
        const p = join(src, f);
        if (!statSync(p).isFile()) continue;
        if (f === basename(o.project)) copyFileSync(p, join(mirror, f));
        else symlinkSync(p, join(mirror, f));
        bytes += statSync(p).size;
      }
      what = join(mirror, basename(o.project));
    } else if (o.model) {
      what = join(tmp, `model${extname(o.model)}`);
      copyFileSync(resolve(o.model), what);
      bytes = statSync(what).size;
    }
    if (bytes > 1e9 || o.lock) await this.lock(); // a project over 1 GB of files (or --lock)
    const bin = process.env.HOST_BIN ?? join(CCORE, "out/native/bimble-ccore");
    if (!existsSync(bin)) throw new Error(`no ${bin}: ccore/build.sh native`);
    const hostHome = o.links ? synthDir : home;
    this.host = this.start("host", bin, [String(this.port), ...(what ? [what] : [])], { cwd: tmp, env: { ...process.env, BIMBLE_TOKEN: this.token, HOME: hostHome, BIMBLE_CONFIG_DIR: config,
      BIMBLE_CACHE_DIR: o.links ? join(synthDir, "cache") : cache, BIMBLE_LINK_CACHE_MIN: "0", BIMBLE_ORIGINS: `http://localhost:${this.vite},http://127.0.0.1:${this.vite}` } });
    await this.waitFor("the host", 600000, () => this.host.out.includes("listening") || (this.host.exitCode !== null && Promise.reject(new Error(this.host.out.slice(-600)))), 50);
    if (o.links) {
      const dir = join(synthDir, "links");
      const have = readdirSync(dir).filter((f) => /^building_\d+\.ifc$/.test(f)).length;
      if (have !== Number(o.links)) {
        const { makeBuildingFolder } = await import("../bench/buildings_synth.mjs");
        const t0 = Date.now();
        const made = await makeBuildingFolder({ hcall: this.hcall, dir, work: synthDir, n: Number(o.links), storeys: Number(o.storeys) });
        this.log(`made ${o.links} buildings: ${made.elements} elements, ${made.tris} triangles, ${Date.now() - t0} ms`);
      }
      if ((await this.hcall("action", "new")).status !== 200) throw new Error("new");
      if ((await this.hcall("action", "link_folder", { path: dir })).status !== 200) throw new Error("link_folder");
      const { W, D } = await import("../bench/buildings_synth.mjs");
      await this.waitFor("the links converted", 3600000, async () => { const r = (await this.q("links_panel")).models; return r.length === Number(o.links) && r.every((m) => m.state === "ready"); }, 300);
      for (let k = 0; k < Number(o.links); k++) {
        await this.hcall("action", "manage_links", { op: "unpin", name: `building_${k}.ifc` });
        await this.hcall("action", "manage_links", { op: "place", name: `building_${k}.ifc`, move_mm: [(W + 14000) * (k % 6), (D + 16000) * Math.floor(k / 6), 0] });
      }
    }
    if (o.project) await this.waitFor("the links ready", 3600000, async () => { const r = (await this.q("links_panel")).models; return r.every((m) => m.state === "ready"); }, 500);
    this.nLinks = (await this.q("links_panel").catch(() => ({ models: [] }))).models.length;
    const vite = this.start("vite", "npx", ["vite", "--port", String(this.vite), "--strictPort"], { cwd: UI, env: { ...process.env, BIMBLE_VITE_FROZEN: "1" } }); // no reload by an edit during the run
    await this.waitFor(`Vite on ${this.vite}`, 60000, async () => { if (vite.exitCode !== null) throw new Error(vite.out); return (await fetch(`http://localhost:${this.vite}/`)).ok; });
    await this.waitFor("3 GB available", 600000, () => memGb() >= 3, 2000);
    this.log(`${memGb().toFixed(1)} GB available, load ${loadNow()}; starting Chromium (LAB_GPU=${o.gpu}) at ${cw}x${ch} css px, dpr ${o.dpr}`);
    const prof = join(tmp, "chromium");
    this.chrome = this.start("chromium", CHROMIUM, ["--headless=new", ...gpuFlags(o.gpu), "--remote-debugging-port=0", `--user-data-dir=${prof}`,
      "--no-first-run", "--no-default-browser-check", "--js-flags=--expose-gc", `--window-size=${cw},${ch}`, "about:blank"], { env: { ...process.env, TMPDIR: tmp } });
    this.minMem = memGb();
    this.dog = setInterval(() => this.sample(), 250);
    const wsUrl = await this.waitFor("DevTools line", 60000, () => this.chrome.out.match(/DevTools listening on (ws:\/\/\S+)/)?.[1]);
    this.cdp = await cdpConnect(wsUrl);
    const { targetId } = await this.cdp.send("Target.createTarget", { url: "about:blank" });
    const { sessionId } = await this.cdp.send("Target.attachToTarget", { targetId, flatten: true });
    this.sid = sessionId;
    this.cdp.on((m) => this.onEvent(m));
    for (const d of ["Runtime", "Page", "Network"]) await this.S(`${d}.enable`);
    await this.S("Emulation.setDeviceMetricsOverride", { width: cw, height: ch, deviceScaleFactor: Number(o.dpr), mobile: false });
    await this.S("Page.addScriptToEvaluateOnNewDocument", { source: INSTR });
    this.tNav = Date.now();
    await this.S("Page.navigate", { url: `http://localhost:${this.vite}/?native=1&gl=1&port=${this.port}&token=${this.token}${o.panel ? "&perf=1" : ""}${o.query ? `&${o.query}` : ""}` });
    await this.waitFor("the page", 120000, () => this.ev("!!window.__bbGl"));
    this.gpu = await this.ev(RENDERER_EXPR);
    this.gpu.software = !this.gpu.hardware;
    this.ext = await this.ev(`(() => { const gl = document.getElementById("bimble").getContext("webgl2"); return ["WEBGL_multi_draw", "EXT_disjoint_timer_query_webgl2", "EXT_color_buffer_float", "WEBGL_draw_instanced_base_vertex_base_instance", "WEBGL_multi_draw_instanced_base_vertex_base_instance", "OVR_multiview2"].filter((e) => gl.getExtension(e)); })()`);
    this.log(`adapter: ${this.gpu.renderer}${this.gpu.software ? " (SOFTWARE: a CPU rasteriser, not a GPU)" : ""}; extensions: ${this.ext.join(", ") || "none"}`);
    if (o.profile) { await this.S("Profiler.enable"); await this.S("Profiler.setSamplingInterval", { interval: 200 }); }
    return this;
  }
  S(m, p) { return this.cdp.send(m, p, this.sid); }
  async ev(expr) {
    const r = await this.S("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
    return r.result.value;
  }
  onEvent(m) {
    if (m.sessionId !== this.sid) return;
    if (m.method === "Runtime.consoleAPICalled" && (m.params.type === "error" || m.params.type === "warning")) this.consoleLines.push(`${m.params.type}: ${m.params.args.map((a) => a.value ?? a.description ?? a.type).join(" ")}`);
    else if (m.method === "Runtime.exceptionThrown") this.consoleLines.push(`exception: ${m.params.exceptionDetails.exception?.description ?? m.params.exceptionDetails.text}`);
    else if (m.method === "Network.requestWillBeSent" && /\/mesh\b/.test(m.params.request.url)) this.net.set(m.params.requestId, { url: m.params.request.url.replace(/t=[^&]*&?/, ""), t0: m.params.timestamp, bytes: 0, ms: null });
    else if (m.method === "Network.loadingFinished" && this.net.has(m.params.requestId)) { const r = this.net.get(m.params.requestId); r.bytes = m.params.encodedDataLength, r.ms = (m.params.timestamp - r.t0) * 1000; }
  }
  sample() {
    const g = memGb();
    this.minMem = Math.min(this.minMem ?? g, g);
    if (g < 2.5 && !this.stopWhy) {
      this.stopWhy = `MemAvailable ${g.toFixed(2)} GB`;
      this.log(`watchdog: ${this.stopWhy}: stopping Chromium`);
      try { process.kill(-this.chrome.pid, "SIGKILL"); } catch { /* gone */ }
    }
    if (!this.chrome || this.memory.length > 20000) return;
    // the renderer's and the GPU process's RSS: Chromium's children by their --type
    let renderer = 0, gpuP = 0;
    try {
      for (const pid of readdirSync("/proc").filter((x) => /^\d+$/.test(x))) {
        let cmd;
        try { cmd = readFileSync(`/proc/${pid}/cmdline`, "utf8"); } catch { continue; }
        if (!cmd.includes(this.temps[0])) continue;
        if (cmd.includes("--type=renderer")) renderer += rssMb(pid) ?? 0;
        else if (cmd.includes("--type=gpu-process")) gpuP += rssMb(pid) ?? 0;
      }
    } catch { /* best effort */ }
    const row = { t: Date.now(), avail: g, renderer, gpuProcess: gpuP, host: rssMb(this.host.pid), heap: null, gpuBytes: null };
    this.memory.push(row);
    if (this.cdp && this.memory.length % 4 === 0) this.ev("[performance.memory ? performance.memory.usedJSHeapSize : null, window.__bbGl ? window.__bbGl.gpuBytes ?? null : null]").then(([h, b]) => { row.heap = h, row.gpuBytes = b; }).catch(() => {});
  }

  // ---- gestures ----
  mouse(type, x, y, button = "none", buttons = 0, extra = {}) {
    return this.S("Input.dispatchMouseEvent", { type, x, y, button, buttons, clickCount: type === "mouseMoved" ? 0 : 1, pointerType: "mouse", ...extra });
  }
  async canvasBox() {
    return this.ev(`(() => { const r = document.getElementById("bimble").getBoundingClientRect(); return { x: r.left, y: r.top, w: r.width, h: r.height }; })()`);
  }
  async drag(dx, dy, ms, button, pause) {
    const b = await this.canvasBox(), x0 = b.x + b.w / 2, y0 = b.y + b.h / 2;
    const bits = button === "left" ? 1 : 4;
    await this.mouse("mouseMoved", x0, y0);
    await this.mouse("mousePressed", x0, y0, button, bits);
    const steps = Math.max(1, Math.round(ms / 16.7)), t0 = Date.now(), sent = [];
    const pauses = pause ? [Math.floor(steps / 3), Math.floor((2 * steps) / 3)] : [];
    let lag = 0;
    for (let i = 1; i <= steps; i++) {
      if (pauses.includes(i)) await sleep(200), lag += 200;
      const due = t0 + lag + i * 16.7;
      const wait = due - Date.now();
      if (wait > 0) await sleep(wait);
      sent.push(this.mouse("mouseMoved", x0 + (dx * i) / steps, y0 + (dy * i) / steps, button, bits)); // not awaited: the OS does not wait
      if (this.o.strip && i % this.o.strip === 0) await this.screenshot(`strip-move-${String(i).padStart(4, "0")}`, true);
    }
    await Promise.all(sent);
    await this.mouse("mouseReleased", x0 + dx, y0 + dy, button, 0);
  }
  async zoom(steps, ms) {
    const b = await this.canvasBox(), x = b.x + b.w / 2, y = b.y + b.h / 2, n = Math.abs(steps), sent = [];
    for (let i = 0; i < n; i++) {
      sent.push(this.mouse("mouseWheel", x, y, "none", 0, { deltaX: 0, deltaY: steps > 0 ? -100 : 100 }));
      if (i < n - 1) await sleep(ms / Math.max(1, n - 1));
    }
    await Promise.all(sent);
  }
  async click(x, y) {
    await this.mouse("mouseMoved", x, y);
    await this.mouse("mousePressed", x, y, "left", 1);
    await this.mouse("mouseReleased", x, y, "left", 0);
  }
  async waitState(what) {
    const g = "window.__bbGl";
    if (what === "ready") return this.waitFor("ready", 1800000, () => this.ev(`!!(${g}.firstFrameMs && (${this.nLinks} === 0 || (${g}.linkDetail && ${g}.linkDetail.length === ${this.nLinks} && ${g}.linkDetail.every((l) => l.detail))))`), 200);
    if (what === "first-picture") return this.waitFor("first picture", 600000, () => this.ev(`!!(${g}.proxyPreview || ${g}.firstFrameMs)`), 20);
    if (what === "complete") return this.waitFor("complete", 600000, () => this.ev(`${g}.fullFrameMs != null`), 50);
    if (what === "idle") {
      let last = -1, since = Date.now();
      return this.waitFor("idle", 120000, async () => { const n = await this.ev(`${g}.frames`); if (n !== last) last = n, since = Date.now(); return Date.now() - since >= 500; }, 50);
    }
    throw new Error(`wait ${what}?`);
  }
  async screenshot(name, small = false) {
    const dir = this.dir;
    const r = await this.S("Page.captureScreenshot", { format: "png" });
    const scale = Math.min(1, 1280 / (this.cw * Number(this.o.dpr)));
    const out = { name, at: Date.now(), full: null, small: null };
    if (!small) { out.full = join(dir, `${name}.png`); writeFileSync(out.full, Buffer.from(r.data, "base64")); }
    if (scale < 1 || small) {
      const s = await this.S("Page.captureScreenshot", { format: "png", clip: { x: 0, y: 0, width: this.cw, height: this.ch, scale: scale / Number(this.o.dpr) * Number(this.o.dpr) } });
      out.small = join(dir, `${name}.small.png`);
      writeFileSync(out.small, Buffer.from(s.data, "base64"));
    }
    this.shots.push(out);
    return out;
  }
  parse(script) {
    const steps = Array.isArray(script) ? script : String(script).split(";").map((s) => s.trim()).filter(Boolean);
    const out = [];
    for (let i = 0; i < steps.length; i++) {
      const s = steps[i];
      const m = /^repeat (\d+) \{(.*)\}$/.exec(s);
      if (m) { const inner = this.parse(m[2].split(",").map((x) => x.trim())); for (let k = 0; k < Number(m[1]); k++) out.push(...inner); }
      else out.push(s);
    }
    return out;
  }
  async gesture(script) {
    const done = [];
    for (const s of this.parse(script)) {
      if (this.stopWhy) throw new Error(`stopped: ${this.stopWhy}`);
      const a = s.split(/\s+/), t0 = Date.now();
      const tPage = await this.ev("performance.now()");
      this.marks.push({ step: s, at: tPage });
      switch (a[0]) {
        case "wait": await this.waitState(a[1]); break;
        case "orbit": await this.drag(Number(a[1]), Number(a[2]), Number(a[3]), "left", a[4] === "pause"); break;
        case "pan": await this.drag(Number(a[1]), Number(a[2]), Number(a[3]), "middle", a[4] === "pause"); break;
        case "zoom": await this.zoom(Number(a[1]), Number(a[2] ?? 500)); break;
        case "click": {
          const b = await this.canvasBox();
          if (a[1] === "centre" || a[1] === "center") await this.click(b.x + b.w / 2, b.y + b.h / 2);
          else if (a[1] === "element") {
            const p = await this.ev(`(() => { const g = window.__bbGl, bx = g.elementBox(${JSON.stringify(a[2])}); if (!bx) return null; const c = [0, 1, 2].map((k) => (bx.min[k] + bx.max[k]) / 2); return g.toScreen(c[0], c[1], c[2]); })()`);
            if (!p) throw new Error(`click element ${a[2]}: not on screen`);
            await this.click(p[0], p[1]);
          } else await this.click(Number(a[1]), Number(a[2]));
          break;
        }
        case "key": await this.S("Input.dispatchKeyEvent", { type: "keyDown", key: a[1] }); await this.S("Input.dispatchKeyEvent", { type: "keyUp", key: a[1] }); break;
        case "fit": await this.ev("(window.__bbGl.bench.snap(0, 0), 0)"); break;
        case "action": { const r = await this.hcall("action", a[1], a[2] ? JSON.parse(a.slice(2).join(" ")) : {}); if (r.status !== 200) throw new Error(`action ${a[1]}: ${r.text.slice(0, 200)}`); break; }
        case "open": await this.ev(`(() => { const b = [...document.querySelectorAll("button")].find((x) => x.getAttribute("aria-label") && /perf/i.test(x.className + x.innerHTML) && x.querySelector(".pft")); if (b && !document.querySelector('[data-bb="perf"]')) b.click(); return !!b; })()`); break;
        case "close": await this.ev(`(() => { const b = document.querySelector('[data-bb="perf-close"]'); if (b) b.click(); return !!b; })()`); break;
        case "screenshot": await this.screenshot(a[1] ?? `shot-${this.shots.length}`); break;
        case "sleep": {
          const ms = Number(a[1]);
          if (this.o.strip) { const until = Date.now() + ms; while (Date.now() < until) { await this.screenshot(`strip-${Date.now()}`, true); } }
          else await sleep(ms);
          break;
        }
        case "mark": await this.screenshot(`mark-${a.slice(1).join("-")}`, true); break;
        case "drawcalls": { // the same triangles in N draw calls (and one multi-draw a model): what a draw call costs here
          const rows = [];
          for (const n of [6000, 2000, 600, 200, 60, 17]) rows.push(await this.ev(`window.__bbGl.bench.drawCalls(${n}, 3, false)`));
          for (const n of [6000, 600]) { const r = await this.ev(`window.__bbGl.bench.drawCalls(${n}, 3, true)`); if (r) rows.push({ ...r, multi: true }); }
          this.drawcalls = rows;
          writeFileSync(join(this.dir, "drawcalls.json"), JSON.stringify(rows, null, 1));
          break;
        }
        default: throw new Error(`unknown step: ${s}`);
      }
      done.push({ step: s, ms: Date.now() - t0 });
      this.log(`${s} (${Date.now() - t0} ms)`);
    }
    return done;
  }
  async report() { return this.ev("window.__bbPerf ? window.__bbPerf.report(false, null) : null"); }
  async profile(seconds) {
    await this.S("Profiler.start");
    await sleep(seconds * 1000);
    return this.stopProfile();
  }
  async stopProfile() {
    const { profile } = await this.S("Profiler.stop");
    return profile;
  }

  /** Everything recorded so far into this.dir: the frame and input logs, the report, the summary. */
  async collect(profile = null) {
    const dir = this.dir;
    const page = await this.ev(`(() => { const g = window.__bbGl, d = window.__drv; return { frames: g.frameRecs || [], timer: !!g.gpuTimer, drv: d ? { inputs: d.inputs, frames: d.frames, errors: d.errors } : null,
      firstFrameMs: g.firstFrameMs, fullFrameMs: g.fullFrameMs ?? null, proxyPreview: g.proxyPreview ?? null, meshAt: g.meshAt ?? null, gpuBytes: g.gpuBytes ?? null, canvas: [document.getElementById("bimble").width, document.getElementById("bimble").height],
      linkProxies: (g.linkProxies || []).map((x) => ({ cells: x.cells, tris: x.tris, drawnCells: x.drawnCells, drawnTris: x.drawnTris })), linkDetail: (g.linkDetail || []).map((x) => x.detail),
      settings: null }; })()`);
    const report = await this.report();
    writeFileSync(join(dir, "report.txt"), report ?? "(no perf.ts)\n");
    writeFileSync(join(dir, "frames.json"), JSON.stringify(page.frames));
    writeFileSync(join(dir, "frames.tsv"), ["at\tkind\tjs_ms\tgpu_ms\tfence_ms\tdraws\ttris\tculled\tbatches\tproxied\toutlines\tlink_proxy_tris\tbudget",
      ...page.frames.map((f) => [f.at, f.kind, f.js, f.gpu, f.fence, f.draws, f.tris, f.culled, f.batches, f.proxied, f.outlines, f.linkPrxTris, f.budget].map((v) => (typeof v === "number" ? Math.round(v * 100) / 100 : v ?? "")).join("\t"))].join("\n") + "\n");
    // each input -> the end of the first frame (an animation frame where the viewport drew) after it
    const ins = page.drv?.inputs ?? [], fr = page.drv?.frames ?? [];
    const inputs = ins.filter((i) => i.what !== "release").map((i) => { const f = fr.find((x) => x.end >= i.t); return { ...i, frameEnd: f ? f.end : null, ms: f ? f.end - i.t : null }; });
    writeFileSync(join(dir, "inputs.json"), JSON.stringify(inputs));
    writeFileSync(join(dir, "network.json"), JSON.stringify([...this.net.values()], null, 1));
    writeFileSync(join(dir, "console.txt"), [...this.consoleLines, ...(page.drv?.errors ?? []).map((e) => `page error: ${e}`)].join("\n") + "\n");
    writeFileSync(join(dir, "memory.tsv"), ["t\tavail_gb\trenderer_mb\tgpu_process_mb\thost_mb\tjs_heap_mb\tgpu_bytes_mb", ...this.memory.map((r) => [r.t, r.avail.toFixed(2), r.renderer.toFixed(0), r.gpuProcess.toFixed(0), r.host?.toFixed(0) ?? "", r.heap ? (r.heap / 1048576).toFixed(0) : "", r.gpuBytes ? (r.gpuBytes / 1048576).toFixed(0) : ""].join("\t"))].join("\n") + "\n");
    let top = [];
    if (profile) {
      writeFileSync(join(dir, "profile.cpuprofile"), JSON.stringify(profile));
      top = profileTop(profile, 20);
      writeFileSync(join(dir, "profile.txt"), ["self ms\ttotal %\tfunction (url:line)", ...top.map((r) => `${r.self.toFixed(1)}\t${r.pct.toFixed(1)}\t${r.name} (${r.url}:${r.line})`)].join("\n") + "\n");
    }
    const kinds = (k) => page.frames.filter((f) => f.kind === k);
    const stat = (xs) => ({ n: xs.length, median: med(xs), p95: q95(xs), max: xs.length ? Math.max(...xs) : null });
    const gpuOf = (f) => f.gpu ?? f.fence;
    const moving = kinds("m"), still = [...kinds("w")], slices = kinds("s");
    const s = {
      name: basename(dir), at: new Date().toISOString(), load: loadNow(), adapter: this.gpu.renderer, software: this.gpu.software, extensions: this.ext, timer: page.timer,
      size: this.o.size, dpr: Number(this.o.dpr), canvas: page.canvas, settings: this.o.settings ?? null, panel: !!this.o.panel, minMemGb: this.minMem, stopped: this.stopWhy,
      open: { firstPictureMs: page.proxyPreview ? page.proxyPreview.at : page.firstFrameMs, firstDetailMs: page.firstFrameMs, completeMs: page.fullFrameMs }, // performance.now() of the page (navigation start = 0)
      moving: { js: stat(moving.map((f) => f.js)), gpu: stat(moving.map(gpuOf).filter((x) => x != null)), draws: med(moving.map((f) => f.draws)), tris: med(moving.map((f) => f.tris)), outlines: med(moving.map((f) => f.outlines)), proxied: med(moving.map((f) => f.proxied)) },
      whole: { js: stat(still.map((f) => f.js)), gpu: stat(still.map(gpuOf).filter((x) => x != null)), draws: med(still.map((f) => f.draws)), tris: med(still.map((f) => f.tris)) },
      slices: { n: slices.length, js: stat(slices.map((f) => f.js)), gpu: stat(slices.map(gpuOf).filter((x) => x != null)), draws: med(slices.map((f) => f.draws)), tris: med(slices.map((f) => f.tris)) },
      inputs: stat(inputs.map((i) => i.ms).filter((x) => x != null)),
      inputsByKind: Object.fromEntries(["press", "drag", "wheel", "key"].map((k) => [k, stat(inputs.filter((i) => i.what === k).map((i) => i.ms).filter((x) => x != null))])),
      refills: refills(page.frames, this.marks),
      network: { requests: this.net.size, bytes: [...this.net.values()].reduce((n, r) => n + r.bytes, 0) },
      memory: { rendererMaxMb: Math.max(0, ...this.memory.map((r) => r.renderer)), gpuProcessMaxMb: Math.max(0, ...this.memory.map((r) => r.gpuProcess)), hostMaxMb: Math.max(0, ...this.memory.map((r) => r.host ?? 0)), gpuBytes: page.gpuBytes },
      linkProxies: page.linkProxies.reduce((a, x) => ({ cells: a.cells + x.cells, drawnCells: a.drawnCells + x.drawnCells, drawnTris: a.drawnTris + x.drawnTris }), { cells: 0, drawnCells: 0, drawnTris: 0 }),
      profileTop: top.slice(0, 10), drawcalls: this.drawcalls ?? null, shots: this.shots.map((x) => x.small ?? x.full), errors: this.consoleLines.length + (page.drv?.errors?.length ?? 0),
    };
    writeFileSync(join(dir, "summary.json"), JSON.stringify(s, null, 1));
    writeFileSync(join(dir, "summary.md"), summaryMd(s));
    return s;
  }
  async close() {
    clearInterval(this.dog);
    try { this.cdp?.close(); } catch { /* gone */ }
    for (const sig of ["SIGTERM", "SIGKILL"]) {
      for (const p of this.procs) if (p.exitCode === null && p.signalCode === null) try { process.kill(-p.pid, sig); } catch { /* gone */ }
      await sleep(200);
    }
    for (const t of this.temps) rmSync(t, { recursive: true, force: true });
    this.temps = [];
    if (this.o.clean && this.o.links) rmSync(join(SCRATCH_ROOT, `synth-${this.o.links}-${this.o.storeys}`), { recursive: true, force: true });
  }
}

/** After each "mark stop"-like gesture end: how the picture was completed (the slices after the last moving frame). */
function refills(frames, marks) {
  const out = [];
  let i = 0;
  while (i < frames.length) {
    if (frames[i].kind !== "m") { i++; continue; }
    let j = i;
    while (j < frames.length && frames[j].kind === "m") j++;
    const lastMove = frames[j - 1];
    let k = j, n = 0;
    while (k < frames.length && frames[k].kind === "s") k++, n++;
    if (n) out.push({ slices: n, fromStopMs: frames[k - 1].at + (frames[k - 1].fence ?? frames[k - 1].js) - lastMove.at, tris: frames.slice(j, k).reduce((t, f) => t + f.tris, 0) });
    i = Math.max(k, j);
  }
  void marks;
  return out;
}

/** The top functions by self time of a .cpuprofile. */
export function profileTop(p, n) {
  const self = new Map(), byId = new Map(p.nodes.map((x) => [x.id, x]));
  const dt = new Map();
  for (let i = 0; i < p.samples.length; i++) dt.set(p.samples[i], (dt.get(p.samples[i]) ?? 0) + (p.timeDeltas[i + 1] ?? 0));
  let total = 0;
  for (const [id, us] of dt) {
    const x = byId.get(id), cf = x.callFrame, key = `${cf.functionName || "(anonymous)"}|${cf.url.replace(/^.*\/src\//, "src/").replace(/\?.*$/, "")}|${cf.lineNumber + 1}`;
    self.set(key, (self.get(key) ?? 0) + us);
    total += us;
  }
  return [...self.entries()].sort((a, b) => b[1] - a[1]).slice(0, n).map(([k, us]) => { const [name, url, line] = k.split("|"); return { name, url, line, self: us / 1000, pct: (100 * us) / Math.max(1, total) }; });
}

function summaryMd(s) {
  const st = (x) => (x && x.n ? `${f1(x.median)} / ${f1(x.p95)} / ${f1(x.max)} (n ${x.n})` : "–");
  const L = [`# drive: ${s.name}`, "", `${s.at}; load ${s.load}; adapter \`${s.adapter}\`${s.software ? " — SOFTWARE (a CPU rasteriser): not a GPU's numbers" : " (real GPU)"}; GPU time by ${s.timer ? "timer query" : "fence (draw start to the GPU's end, to a loop's resolution)"}`,
    `window ${s.size} css px at dpr ${s.dpr} (canvas ${s.canvas.join(" x ")}); settings ${s.settings ?? "default"}; panel ${s.panel ? "open" : "closed"}; extensions: ${s.extensions.join(", ") || "none"}`, "",
    "| | median / p95 / max ms |", "|---|---|",
    `| open → first picture / first detail / complete (page clock) | ${f0(s.open.firstPictureMs)} / ${f0(s.open.firstDetailMs)} / ${f0(s.open.completeMs)} |`,
    `| input → frame (all) | ${st(s.inputs)} |`,
    ...Object.entries(s.inputsByKind).filter(([, v]) => v.n).map(([k, v]) => `| input → frame: ${k} | ${st(v)} |`),
    `| moving frame: script | ${st(s.moving.js)} |`, `| moving frame: GPU | ${st(s.moving.gpu)} |`,
    `| whole frame: script | ${st(s.whole.js)} |`, `| whole frame: GPU | ${st(s.whole.gpu)} |`,
    `| fill slice: script | ${st(s.slices.js)} |`, `| fill slice: GPU | ${st(s.slices.gpu)} |`, "",
    `Moving frames: ${f0(s.moving.tris)} triangles, ${f0(s.moving.draws)} draws, ${f0(s.moving.outlines)} batch outlines, ${f0(s.moving.proxied)} proxy cells (medians). Whole frames: ${f0(s.whole.tris)} triangles, ${f0(s.whole.draws)} draws. Slices: ${s.slices.n}, ${f0(s.slices.tris)} triangles, ${f0(s.slices.draws)} draws (medians).`,
    `Refills after a move: ${s.refills.map((r) => `${r.slices} slices, ${f0(r.fromStopMs)} ms, ${f0(r.tris)} triangles`).join("; ") || "none"}.`,
    `Link proxies: ${f0(s.linkProxies.cells)} cells held, last frame drew ${f0(s.linkProxies.drawnCells)} (${f0(s.linkProxies.drawnTris)} triangles).`,
    `Memory: renderer ≤ ${f0(s.memory.rendererMaxMb)} MB, GPU process ≤ ${f0(s.memory.gpuProcessMaxMb)} MB, host ≤ ${f0(s.memory.hostMaxMb)} MB, GPU buffers ${f0((s.memory.gpuBytes ?? 0) / 1048576)} MB; min available ${f1(s.minMemGb, 2)} GB${s.stopped ? `; STOPPED: ${s.stopped}` : ""}.`,
    `Network: ${s.network.requests} /mesh requests, ${f1(s.network.bytes / 1048576)} MB. Console errors: ${s.errors}.`, ""];
  if (s.drawcalls) L.push("The same triangles in N draw calls (whole faces of every loaded model; best of 3; ms):", "", "| parts asked | draw calls | triangles | script submit | to the pixels |", "|---|---|---|---|---|",
    ...s.drawcalls.filter(Boolean).map((r) => `| ${r.n}${r.multi ? " (multi-draw)" : ""} | ${f0(r.draws)} | ${f0(r.tris)} | ${f1(r.submit)} | ${f1(r.total)} |`), "");
  if (s.profileTop.length) L.push("CPU profile, top self time:", "", "| ms | % | function |", "|---|---|---|", ...s.profileTop.map((r) => `| ${f1(r.self)} | ${f1(r.pct)} | \`${r.name}\` ${r.url}:${r.line} |`), "");
  if (s.shots.length) L.push("Screenshots:", ...s.shots.map((x) => `- ${x}`), "");
  return L.join("\n");
}

function compare(a, b) {
  const A = JSON.parse(readFileSync(join(a, "summary.json"), "utf8")), B = JSON.parse(readFileSync(join(b, "summary.json"), "utf8"));
  const rows = [["input → frame median", (s) => s.inputs.median], ["input → frame p95", (s) => s.inputs.p95], ["input → frame max", (s) => s.inputs.max],
    ["moving script median", (s) => s.moving.js.median], ["moving GPU median", (s) => s.moving.gpu.median], ["moving triangles", (s) => s.moving.tris], ["moving draws", (s) => s.moving.draws],
    ["whole script median", (s) => s.whole.js.median], ["whole GPU median", (s) => s.whole.gpu.median], ["whole triangles", (s) => s.whole.tris], ["whole draws", (s) => s.whole.draws],
    ["slices", (s) => s.slices.n], ["first picture", (s) => s.open.firstPictureMs], ["complete", (s) => s.open.completeMs]];
  console.log(`| | ${A.name} | ${B.name} |\n|---|---|---|`);
  for (const [k, fn] of rows) console.log(`| ${k} | ${f1(fn(A))} | ${f1(fn(B))} |`);
}

// ---- the command line and the MCP face ----
function args(argv) {
  const o = {};
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (!a.startsWith("--")) continue;
    const k = a.slice(2).replace(/-([a-z])/g, (_, c) => c.toUpperCase());
    if (["panel", "mcp", "clean", "noProfile", "quiet", "lock"].includes(k)) o[k] = true;
    else o[k] = argv[++i];
  }
  if (o.noProfile) o.profile = false;
  return o;
}

async function cli(o) {
  if (o.compare) { compare(o.compare, process.argv[process.argv.indexOf("--compare") + 2]); return 0; }
  const script = o.do ?? (o.script ? JSON.parse(readFileSync(o.script, "utf8")) : PRESETS[o.preset ?? "orbit-zoom"]);
  if (!script) throw new Error(`no preset ${o.preset}`);
  const name = o.name ?? `${o.preset ?? "script"}-${new Date().toISOString().replace(/[:.]/g, "-")}`;
  const d = new Drive(o);
  d.dir = join(CCORE, "out/drive", name);
  mkdirSync(d.dir, { recursive: true });
  const bye = async (code) => { await d.close(); process.exit(code); };
  for (const sig of ["SIGINT", "SIGTERM", "SIGHUP"]) process.on(sig, () => bye(130));
  try {
    await d.open();
    if (o.profile !== false) await d.S("Profiler.start");
    await d.gesture(script);
    const prof = o.profile !== false ? await d.stopProfile() : null;
    const s = await d.collect(prof);
    console.log(readFileSync(join(d.dir, "summary.md"), "utf8"));
    console.error(`drive: written to ${d.dir}`);
    await d.close();
    return s.stopped ? 2 : 0;
  } catch (e) {
    console.error(e);
    try { await d.collect(null); } catch { /* partial */ }
    await d.close();
    return 1;
  }
}

async function mcp() {
  let d = null, profiling = false;
  const TOOLS = [
    { name: "open", description: "Open the lab on a model, a project of links or N synthetic buildings, headless on the machine's GPU.", inputSchema: { type: "object", properties: { model: { type: "string" }, project: { type: "string" }, links: { type: "number" }, storeys: { type: "number" }, size: { type: "string" }, dpr: { type: "number" }, settings: { type: "string" }, gpu: { type: "string" }, panel: { type: "boolean" }, name: { type: "string" } } } },
    { name: "gesture", description: "Play gesture steps (drive.mjs's list: wait, orbit, pan, zoom, click, key, fit, action, screenshot, sleep, mark, repeat).", inputSchema: { type: "object", properties: { script: { type: "array", items: { type: "string" } } }, required: ["script"] } },
    { name: "screenshot", description: "A PNG of the page; returns its path.", inputSchema: { type: "object", properties: { name: { type: "string" } } } },
    { name: "report", description: "The performance report and the run's summary (frames, inputs, memory) so far.", inputSchema: { type: "object", properties: {} } },
    { name: "profile", description: "A CPU profile over the next N seconds (play gestures meanwhile from another call); returns the top functions.", inputSchema: { type: "object", properties: { seconds: { type: "number" } } } },
    { name: "close", description: "Close the browser, the host and Vite; remove the temp folders.", inputSchema: { type: "object", properties: {} } },
  ];
  const call = async (name, a) => {
    if (name === "open") {
      if (d) await d.close();
      d = new Drive({ ...a, quiet: true, profile: true });
      d.dir = join(CCORE, "out/drive", a.name ?? `mcp-${Date.now()}`);
      mkdirSync(d.dir, { recursive: true });
      await d.open();
      return { dir: d.dir, adapter: d.gpu.renderer, software: d.gpu.software, extensions: d.ext };
    }
    if (!d) throw new Error("open first");
    if (name === "gesture") return d.gesture(a.script);
    if (name === "screenshot") return d.screenshot(a.name ?? `shot-${d.shots.length}`);
    if (name === "report") return { report: await d.report(), summary: await d.collect(null) };
    if (name === "profile") { if (profiling) throw new Error("a profile is running"); profiling = true; try { return profileTop(await d.profile(a.seconds ?? 5), 20); } finally { profiling = false; } }
    if (name === "close") { await d.close(); d = null; return "closed"; }
    throw new Error(`no tool ${name}`);
  };
  const rl = createInterface({ input: process.stdin });
  const send = (m) => process.stdout.write(JSON.stringify(m) + "\n");
  rl.on("line", async (line) => {
    let m;
    try { m = JSON.parse(line); } catch { return; }
    if (m.method === "initialize") return send({ jsonrpc: "2.0", id: m.id, result: { protocolVersion: "2024-11-05", capabilities: { tools: {} }, serverInfo: { name: "bimble-drive", version: "1" } } });
    if (m.method === "tools/list") return send({ jsonrpc: "2.0", id: m.id, result: { tools: TOOLS } });
    if (m.method === "tools/call") {
      try { const r = await call(m.params.name, m.params.arguments ?? {}); send({ jsonrpc: "2.0", id: m.id, result: { content: [{ type: "text", text: typeof r === "string" ? r : JSON.stringify(r, null, 1) }] } }); }
      catch (e) { send({ jsonrpc: "2.0", id: m.id, result: { isError: true, content: [{ type: "text", text: String(e?.message ?? e) }] } }); }
      return;
    }
    if (m.id !== undefined && !m.method?.startsWith("notifications/")) send({ jsonrpc: "2.0", id: m.id, error: { code: -32601, message: `no method ${m.method}` } });
  });
  rl.on("close", async () => { if (d) await d.close(); process.exit(0); });
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const o = args(process.argv.slice(2));
  if (o.mcp) mcp();
  else cli(o).then((c) => process.exit(c), (e) => { console.error(e); process.exit(1); });
}
// unused imports kept minimal
void openSync, void closeSync;
