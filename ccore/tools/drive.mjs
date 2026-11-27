// Drive the lab with real gestures on the machine's GPU and record where the time goes (ticket VIS1).
//
//   node ccore/tools/drive.mjs [what] [options] [--preset orbit-zoom|open|walk-inside | --script FILE.json | --do '<steps>']
// what (one):
//   --model PATH          a model (.bimble, .ifc, .json) opened by the native host (a copy in the run's temp folder)
//   --project PATH        a .bimble project of links: read where it is through a scratch folder of symbolic links;
//                         the links' caches in --cache-dir (default ~/.cache/bimble-drive/project-cache, refused inside
//                         the project's folder) with BIMBLE_LINK_CACHE_BESIDE=0: nothing is written beside your files
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
//   --need-gb G           waits for G GB available (outside the shared lock) before anything starts (default 3; a
//                         --project 6: its tab, host and GPU process measured 4.5 GB, Chromium capped below)
//   --wait-min M          gives up after M minutes without the memory or the lock (default 20): exit 3, "machine
//                         busy: not measured" (the lock is taken with flock -n, never waited on, never held while waiting)
//   --mem-cap 5G          Chromium in a systemd-run scope with MemoryMax (default 5G for a --project): it dies alone
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
//   js EXPR; redraw       a page expression (g is window.__bbGl, e.g. "js g.lineEdges = true"); one whole frame now
//   throughput; drawcalls whole frames under variants (edges, state, back faces, screen-size cut: GPU ms per million
//                         triangles); the same triangles in 6,000 .. 17 draw calls and as multi-draws
// Recorded into ccore/out/drive/NAME/ (git-ignored; files may hold your file names: never commit them):
//   report.txt (the performance panel's "Copy report" text, read from perf.ts), frames.json and frames.tsv (every frame:
//   kind, script ms, GPU ms by timer query or fence, draws, triangles, culled batches, proxies, outlines), inputs.json
//   (each input's timestamp and the end of the frame that showed it), profile.cpuprofile and profile.txt (the top 20
//   functions by self time), memory.tsv (every 250 ms: JS heap, GPU bytes, renderer, GPU process and host RSS),
//   network.json (every /mesh request: bytes, ms), console.txt, PNGs, summary.md and summary.json.
// Memory: waits for --need-gb available, then (a project over 1 GB, or --lock) takes ~/.cache/bimble-measure.lock with
// flock -n, its holder ending with this process; a watchdog kills Chromium under 2.5 GB available (1.5 GB when
// capped); one Chromium. Temp
// folders on disk under ~/.cache/bimble-drive/ (not /tmp: memory here), removed on any exit.
import { spawn } from "node:child_process";
import { createInterface } from "node:readline";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, statSync, symlinkSync, writeFileSync, copyFileSync, openSync, closeSync } from "node:fs";
import { basename, dirname, extname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { port, vitePort } from "../test/ports.mjs";
import { CHROMIUM, chromiumEnv, gpuFlags, RENDERER_EXPR } from "../test/lib/chromium.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = process.env.DRIVE_UI ? resolve(process.env.DRIVE_UI) : resolve(CCORE, "../web-ui"); // DRIVE_UI: another tree's web-ui (the code before a change)
const SCRATCH_ROOT = join(process.env.HOME, ".cache/bimble-drive");
const LOCK = process.env.DRIVE_LOCK ?? join(process.env.HOME, ".cache/bimble-measure.lock"); // (DRIVE_LOCK: another lock file, for its test)
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

/** The machine had not the memory (or the shared lock) within the wait budget: a result, not a hang. */
export class BusyError extends Error {}

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
  /** Waits for `need` GB available OUTSIDE the lock, then (when `lock`) takes the shared measuring lock without
   *  waiting on it (flock -n: busy, nothing is held, memory is checked again a little later), checks memory once more
   *  inside and keeps the lock until close(). The holder is `flock -n LOCK cat` reading this process's pipe: it ends
   *  when this process ends, however it ends (a killed or timed-out run never leaves the lock held). Never a lock held
   *  while waiting. After `waitMin` minutes: BusyError ("machine busy: not measured"). */
  async acquire(need, lock) {
    const until = Date.now() + 60000 * Number(this.o.waitMin ?? 20);
    let said = 0;
    for (;;) {
      if (Date.now() > until) throw new BusyError(`machine busy: not measured (${need} GB available${lock ? " and the shared lock" : ""} not had within ${this.o.waitMin ?? 20} minutes; ${memGb().toFixed(1)} GB available now)`);
      if (memGb() < need) {
        if (Date.now() - said > 60000) said = Date.now(), this.log(`waiting for ${need} GB available (${memGb().toFixed(1)} now), outside the lock`);
        await sleep(5000);
        continue;
      }
      if (!lock) return;
      const h = spawn("flock", ["-n", "-E", "75", LOCK, "cat"], { stdio: ["pipe", "ignore", "ignore"] });
      const code = await new Promise((r) => { const t = setTimeout(() => r(null), 600); h.on("exit", (c) => { clearTimeout(t); r(c); }); });
      if (code === null) { // held
        if (memGb() >= need) { this.lockHolder = h; this.log(`the shared lock taken (${memGb().toFixed(1)} GB available)`); return; }
        h.stdin.end(), h.kill(); // memory went: let the lock go at once, wait again outside it
      } else if (Date.now() - said > 60000) said = Date.now(), this.log("the shared lock is busy: not waiting on it; trying again in 5 s");
      await sleep(5000);
    }
  }
  release() { if (this.lockHolder) { try { this.lockHolder.stdin.end(); this.lockHolder.kill(); } catch { /* gone */ } this.lockHolder = null; } }

  async open() {
    const o = this.o;
    const [cw, ch] = String(o.size).split("x").map(Number);
    this.cw = cw, this.ch = ch;
    const root = o.scratchRoot ?? SCRATCH_ROOT; // (a test: its own temp folder)
    mkdirSync(root, { recursive: true });
    const tmp = mkdtempSync(join(root, "run-"));
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
      synthDir = join(root, `synth-${o.links}-${o.storeys}`);
      mkdirSync(join(synthDir, "links"), { recursive: true });
    } else if (o.project) {
      // a scratch folder of symbolic links to every file beside the project: what the host writes (a cache, a
      // session) replaces a link in the scratch folder (written to a temp file, then renamed), never the original
      const src = dirname(resolve(o.project)), mirror = join(tmp, "project");
      // a project's link caches go to a cache folder of the run's own (kept between runs: --cache-dir, default
      // ~/.cache/bimble-drive/project-cache), never beside the user's files (BIMBLE_LINK_CACHE_BESIDE=0): the links
      // of a project name their files by absolute path, and a cache found stale beside them was written there (27 Nov)
      this.cacheDir = resolve(o.cacheDir ?? join(root, "project-cache"));
      if (this.cacheDir === src || this.cacheDir.startsWith(src + "/")) throw new Error(`--cache-dir ${this.cacheDir} is inside the project's folder: refused (nothing is written beside your files)`);
      mkdirSync(this.cacheDir, { recursive: true });
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
    // memory first, outside the lock; then the lock for a project over 1 GB of files (or --lock), never waited on.
    // A project of links (before SIZE3): the tab 3.1 GB, the host 1.25 GB, the GPU process 0.14 GB at most (measured
    // 27 Nov): 6 GB with Chromium capped at 5 GB and the watchdog at 1.5 GB; else 3 GB.
    this.need = Number(o.needGb ?? (o.project ? 6 : 3));
    await this.acquire(this.need, bytes > 1e9 || !!o.lock);
    const bin = process.env.HOST_BIN ?? join(CCORE, "out/native/bimble-ccore");
    if (!existsSync(bin)) throw new Error(`no ${bin}: ccore/build.sh native`);
    const hostHome = o.links ? synthDir : home;
    this.host = this.start("host", bin, [String(this.port), ...(what ? [what] : [])], { cwd: tmp, env: { ...process.env, BIMBLE_TOKEN: this.token, HOME: hostHome, BIMBLE_CONFIG_DIR: config,
      BIMBLE_CACHE_DIR: o.links ? join(synthDir, "cache") : this.cacheDir ?? cache, BIMBLE_LINK_CACHE_BESIDE: "0", BIMBLE_LINK_CACHE_MIN: "0", BIMBLE_ORIGINS: `http://localhost:${this.vite},http://127.0.0.1:${this.vite}` } });
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
    if (memGb() < this.need - 1.5) this.log(`only ${memGb().toFixed(1)} GB available now (the host's open took some): starting anyway, the watchdog stops it`);
    this.log(`${memGb().toFixed(1)} GB available, load ${loadNow()}; starting Chromium (LAB_GPU=${o.gpu}) at ${cw}x${ch} css px, dpr ${o.dpr}`);
    const prof = join(tmp, "chromium");
    // a cgroup cap (--mem-cap, 5G for a project): Chromium dies alone when it is wrong, the machine does not
    const cap = o.memCap ?? (o.project ? "5G" : null), capArgs = cap && existsSync("/usr/bin/systemd-run") ? ["/usr/bin/systemd-run", "--user", "--scope", "-q", "-p", `MemoryMax=${cap}`, "-p", "MemorySwapMax=0"] : [];
    if (capArgs.length) this.log(`Chromium capped at ${cap} (systemd-run scope)`);
    this.chrome = this.start("chromium", capArgs.length ? capArgs[0] : CHROMIUM, [...capArgs.slice(1), ...(capArgs.length ? [CHROMIUM] : []), "--headless=new", ...gpuFlags(o.gpu), "--remote-debugging-port=0", `--user-data-dir=${prof}`,
      "--no-first-run", "--no-default-browser-check", "--js-flags=--expose-gc", `--window-size=${cw},${ch}`, "about:blank"], { env: chromiumEnv() }); // (a short TMPDIR: the singleton socket)
    this.minMem = memGb();
    this.dog = setInterval(() => this.sample(), 250);
    const wsUrl = await this.waitFor("DevTools line", 120000, () => this.chrome.out.match(/DevTools listening on (ws:\/\/\S+)/)?.[1] ?? (this.chrome.exitCode !== null && Promise.reject(new Error(`Chromium exited: ${this.chrome.out.slice(-800)}`)))).catch((e) => { throw new Error(`${e.message}; Chromium said: ${this.chrome.out.slice(-800)}`); });
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
    this.browserVersion = (await this.cdp.send("Browser.getVersion").catch(() => ({}))).product ?? "?"; // (numbers across a Chromium upgrade are not strictly comparable)
    this.gpu = await this.ev(RENDERER_EXPR);
    this.gpu.software = !this.gpu.hardware;
    this.ext = await this.ev(`(() => { const gl = document.getElementById("bimble").getContext("webgl2"); return ["WEBGL_multi_draw", "EXT_disjoint_timer_query_webgl2", "EXT_color_buffer_float", "WEBGL_draw_instanced_base_vertex_base_instance", "WEBGL_multi_draw_instanced_base_vertex_base_instance", "OVR_multiview2"].filter((e) => gl.getExtension(e)); })()`);
    this.log(`${this.browserVersion}; adapter: ${this.gpu.renderer}${this.gpu.software ? " (SOFTWARE: a CPU rasteriser, not a GPU)" : ""}; extensions: ${this.ext.join(", ") || "none"}`);
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
    if (g < (this.o.memCap ?? this.o.project ? 1.5 : 2.5) && !this.stopWhy) { // (capped by a cgroup: 1.5 GB)
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
    // ready: drawn with every link's detail, or (a GPU budget across links: SIZE3's link_gpu_mb) with the links' detail
    // unchanged for 3 s (those within the budget loaded, the others drawn as their proxies)
    if (what === "ready") {
      let last = "", since = Date.now();
      return this.waitFor("ready", 1800000, async () => {
        const r = await this.ev(`(() => { const g = ${g}; if (!g.firstFrameMs) return null; const d = g.linkDetail || []; return { all: ${this.nLinks} === 0 || (d.length === ${this.nLinks} && d.every((l) => l.detail)), key: d.map((l) => l.detail ? 1 : 0).join("") + "|" + d.length }; })()`);
        if (!r) return false;
        if (r.key !== last) last = r.key, since = Date.now();
        return r.all || (r.key.endsWith(`|${this.nLinks}`) && Date.now() - since >= 3000);
      }, 200);
    }
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
        case "camera": { // far: the home camera fitted; close: then 5 times nearer; look X Y Z D: at a world point from D mm (home angles)
          if (a[1] === "far") await this.ev("(window.__bbGl.bench.snap(0, 0), 0)");
          else if (a[1] === "close") await this.ev("(window.__bbGl.bench.snap(0, 0), window.__bbGl.bench.zoom(0.2), window.__bbGl.bench.frame(0, false), 0)");
          else if (a[1] === "look") await this.ev(`(window.__bbGl.bench.look([${a[2]}, ${a[3]}, ${a[4]}], ${a[5]}), window.__bbGl.bench.frame(0, false), 0)`);
          break;
        }
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
        case "eval": { const v = await this.ev(`(() => { const g = window.__bbGl; return (${a.slice(1).join(" ")}); })()`); console.log(`drive: eval ${a.slice(1).join(" ")} = ${JSON.stringify(v)}`); break; }
        case "js": await this.ev(`(() => { const g = window.__bbGl; ${a.slice(1).join(" ")}; return 0; })()`); break; // a page expression (g: window.__bbGl), e.g. "js g.lineEdges = true"
        case "redraw": await this.ev("(window.__bbGl.bench.frame(0, false), 0)"); break; // one whole frame now
        case "truth": { // what a still frame submits against what can be seen (the ground truth), and its time
          const label = a[1] ?? `t${(this.truths ?? []).length}`;
          const r = await this.ev(`(async () => { const g = window.__bbGl, b = g.bench, raf = () => new Promise((r) => requestAnimationFrame(r));
            for (let i = 0; i < 4; i++) { await raf(); b.frame(0, false); }
            const n0 = g.frameRecs.length, ms = [];
            for (let i = 0; i < 5; i++) { await raf(); ms.push(b.frame(0, false)); }
            for (let i = 0; i < 6; i++) await raf();
            const recs = g.frameRecs.slice(n0).filter((f) => f.kind === "w"), still = { tris: g.frameTris, draws: g.frameDraws, js: recs.map((f) => f.js), gpu: recs.map((f) => f.gpu).filter((x) => x != null), ms, vis: g.vis ?? null };
            const pic = b.read().rgba;
            const truth = b.idCount();
            await raf(); b.frame(0, false);
            const missing = b.missing();
            const n1 = g.frameRecs.length, mms = [];
            for (let i = 0; i < 10; i++) { await raf(); mms.push(b.frame(i % 2 ? -0.0105 : 0.0105, true)); }
            for (let i = 0; i < 6; i++) await raf();
            const mr = g.frameRecs.slice(n1).filter((f) => f.kind === "m");
            const moving = { ms: mms, tris: mr.map((f) => f.tris), gpu: mr.map((f) => f.gpu).filter((x) => x != null) };
            b.frame(0, false);
            let t = ""; for (let k = 0; k < pic.length; k += 32768) t += String.fromCharCode.apply(null, pic.subarray(k, k + 32768));
            return { still, truth, missing, moving, pic: btoa(t) }; })()`);
          const px = Buffer.from(r.pic, "base64");
          let changed = null, any = null;
          if (a[2] === "ref" || !this.refPic) this.refPic = px;
          else if (this.refPic.length === px.length) {
            changed = 0, any = 0;
            for (let k = 0; k < px.length; k += 4) { const d = Math.max(Math.abs(px[k] - this.refPic[k]), Math.abs(px[k + 1] - this.refPic[k + 1]), Math.abs(px[k + 2] - this.refPic[k + 2])); if (d > 40) changed++; if (d > 0) any++; }
          }
          const row = { label, stillTris: r.still.tris, stillDraws: r.still.draws, stillMs: med(r.still.ms), stillGpu: med(r.still.gpu), stillJs: med(r.still.js), vis: r.still.vis,
            truthElements: r.truth.elements, truthTris: r.truth.tris, truthPixels: r.truth.pixels, missing: r.missing?.n ?? null, missingIds: r.missing?.first ?? [],
            movingMs: med(r.moving.ms), movingGpu: med(r.moving.gpu), movingTris: med(r.moving.tris), changed, any };
          (this.truths ??= []).push(row);
          writeFileSync(join(this.dir, "truth.json"), JSON.stringify(this.truths, null, 1));
          this.log(`truth ${label}: ${JSON.stringify(row)}`);
          break;
        }
        case "throughput": { // whole frames under variants: GPU ms (timer query) and ms per million triangles
          const V = { base: "", lineEdges: "g.lineEdges = true;", "visible off": "g.visible = false;", "no front-to-back": "g.noFrontToBack = true;", noEdges: "g.noEdges = true;", "noEdges+cheapFrag": "g.noEdges = true; g.cheapFrag = true;", "noEdges+noState": "g.noEdges = true; g.noState = true;",
            "noEdges+cullBack": "g.noEdges = true; g.cullBack = true;", "noEdges+noProxies": "g.noEdges = true; g.noProxies = true;", "stillPx1": "g.stillPx = 1;", "noEdges+stillPx1": "g.noEdges = true; g.stillPx = 1;" };
          const rows = [];
          for (const [name, set] of Object.entries(V)) {
            const r = await this.ev(`(async () => { const g = window.__bbGl, raf = () => new Promise((r) => requestAnimationFrame(r));
              delete g.noEdges; delete g.cheapFrag; delete g.noState; delete g.cullBack; delete g.noProxies; delete g.stillPx; delete g.lineEdges; delete g.noFrontToBack; g.visible = true; ${set}
              for (let i = 0; i < 3; i++) { await raf(); g.bench.frame(0, false); }
              const ms = [], n0 = g.frameRecs.length; for (let i = 0; i < 7; i++) { await raf(); ms.push(g.bench.frame(0, false)); }
              for (let i = 0; i < 6; i++) await raf();
              const recs = g.frameRecs.slice(n0).filter((f) => f.kind === "w");
              delete g.noEdges; delete g.cheapFrag; delete g.noState; delete g.cullBack; delete g.noProxies; delete g.stillPx; delete g.lineEdges; delete g.noFrontToBack; g.visible = true;
              return { ms, gpu: recs.map((f) => f.gpu).filter((x) => x != null), tris: g.frameTris, draws: g.frameDraws, plain: g.plain ?? null }; })()`);
            const gm = med(r.gpu), tm = med(r.ms);
            rows.push({ variant: name, totalMs: tm, gpuMs: gm, tris: r.tris, draws: r.draws, msPerMTris: (gm ?? tm) / Math.max(1e-9, r.tris / 1e6), plain: r.plain });
          }
          this.throughput = rows;
          writeFileSync(join(this.dir, "throughput.json"), JSON.stringify(rows, null, 1));
          break;
        }
        case "drawcalls": { // the same triangles in N draw calls (and one multi-draw a model): what a draw call costs here
          if (!(await this.ev("!!window.__bbGl.bench.drawCalls"))) { this.log("drawcalls: this page has no drawCalls hook (skipped)"); break; }
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
      name: basename(dir), at: new Date().toISOString(), load: loadNow(), browser: this.browserVersion, adapter: this.gpu.renderer, software: this.gpu.software, extensions: this.ext, timer: page.timer,
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
      profileTop: top.slice(0, 10), drawcalls: this.drawcalls ?? null, throughput: this.throughput ?? null, truths: this.truths ?? null, shots: this.shots.map((x) => x.small ?? x.full), errors: this.consoleLines.length + (page.drv?.errors?.length ?? 0),
    };
    writeFileSync(join(dir, "summary.json"), JSON.stringify(s, null, 1));
    writeFileSync(join(dir, "summary.md"), summaryMd(s));
    return s;
  }
  async close() {
    this.release(); // the lock first: nothing else waits on our cleanup
    clearInterval(this.dog);
    try { this.cdp?.close(); } catch { /* gone */ }
    for (const sig of ["SIGTERM", "SIGKILL"]) {
      for (const p of this.procs) if (p.exitCode === null && p.signalCode === null) try { process.kill(-p.pid, sig); } catch { /* gone */ }
      await sleep(200);
    }
    for (const t of this.temps) rmSync(t, { recursive: true, force: true });
    this.temps = [];
    if (this.o.clean && this.o.links) rmSync(join(this.o.scratchRoot ?? SCRATCH_ROOT, `synth-${this.o.links}-${this.o.storeys}`), { recursive: true, force: true });
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
  const L = [`# drive: ${s.name}`, "", `${s.at}; load ${s.load}; ${s.browser ?? "?"}; adapter \`${s.adapter}\`${s.software ? " — SOFTWARE (a CPU rasteriser): not a GPU's numbers" : " (real GPU)"}; GPU time by ${s.timer ? "timer query" : "fence (draw start to the GPU's end, to a loop's resolution)"}`,
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
  if (s.truths) L.push("Still frames against the ground truth (every shown element's faces as ids, read back), then 10 moving frames:", "",
    "| label | still ms (GPU) | still triangles / draws | elements seen (pixels) | their triangles | submitted / seen | moving ms (GPU): triangles | pixels > 40 / any vs the first | seen but not drawn |", "|---|---|---|---|---|---|---|---|---|",
    ...s.truths.map((r) => `| ${r.label} | ${f1(r.stillMs)} (${f1(r.stillGpu)}) | ${f0(r.stillTris)} / ${f0(r.stillDraws)} | ${f0(r.truthElements)} (${f0(r.truthPixels)}) | ${f0(r.truthTris)} | ${f1(r.stillTris / Math.max(1, r.truthTris))} | ${f1(r.movingMs)} (${f1(r.movingGpu)}): ${f0(r.movingTris)} | ${f0(r.changed)} / ${f0(r.any)} | ${f0(r.missing)} |`), "");
  if (s.throughput) L.push("Whole frames under variants (median of 7; GPU by timer query; total: drawn then a 1x1 readPixels):", "", "| variant | GPU ms | total ms | triangles | draws | GPU ms per million triangles |", "|---|---|---|---|---|---|",
    ...s.throughput.map((r) => `| ${r.variant} | ${f1(r.gpuMs)} | ${f1(r.totalMs)} | ${f0(r.tris)} | ${f0(r.draws)} | ${f1(r.msPerMTris, 2)} |`), `Models drawn without the state texel (u_plain): ${s.throughput[0]?.plain ? s.throughput[0].plain.filter(Boolean).length + " of " + s.throughput[0].plain.length : "?"}.`, "");
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
    if (e instanceof BusyError) { console.error(`drive: ${e.message}`); await d.close(); return 3; } // (exit 3: machine busy, not measured)
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
