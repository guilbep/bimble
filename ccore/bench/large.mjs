// The C core on large synthetic models (ticket T16): node ccore/bench/large.mjs
//   SIZES="2000 20000 60000" BINS="native tcc" RUNS=5 LAB=1 LOOP_RUNS=5 LOOP_SIZES=20000
// Needs: out/native/bimble-ccore and out/tcc/bimble-ccore (build.sh native, build.sh tcc), python3,
// for LAB=1 also web-ui/node_modules, /usr/bin/chromium and tcc. Ports: hosts 47372-47374, Vite 5205.
// Models: bench/large_model.py <size> out/t16-large-<size>.bimble (synthetic, deterministic; made
// when missing). Raw results: out/t16-large.json. Kills all it starts; restores host/mesh.c.
//
// Method, per size and per binary (all wall clock in Node, `performance.now()`):
//  host     spawn -> "listening" on stderr (load + validate + canonical; median of RUNS starts);
//           RSS (VmRSS) and peak (VmHWM) from /proc/<pid>/status after load and after the first
//           /mesh; `query status` load_ms (the host's own clock around the load);
//           `query elements` (the default 1000 rows, and limit 10000: the hard cap, as Rust's
//           dispatch), `query element` (one id): median / p95 of RUNS calls,
//           response bytes; `verb place` (a column) then `POST /undo/-`: median of RUNS pairs.
//  mesh     first GET /mesh (scene build: the host's log line "built in N ms", bytes), then a
//           cached GET (same rev: no build), median of RUNS.
//  session  the host with --session on a fresh copy: place + undo pairs; per change, the call
//           time with --session minus the same call without (the median difference is the file
//           write); the file's size; then SIGTERM and restart: spawn -> "listening" with the
//           restore (and the log line "session restored") against a plain start.
//  lab      (LAB=1, native host): one Vite, one headless Chromium (raw CDP, SwiftShader, 1280x800).
//           navigation -> first drawn frame (`__bbGl.firstFrameMs`, page clock from navigation
//           start), mesh fetch time and bytes as the viewport counts them;
//           (an untimed navigation first: Vite compiles the modules on the first request);
//           orbit latency: 60 left-drag moves of 8 px; for each, the time from the move to a new
//           glviewport frame plus a 1x1 readPixels (forces SwiftShader to finish the frame), in
//           the page (from the pointermove event) and in Node (from the CDP dispatch): p50 / p95;
//           orbit throughput: moves streamed for 3 s, the intervals between drawn frames (rAF);
//           click-pick: a click on the canvas centre; in the page, window capture / bubble
//           listeners around glviewport's pointerup (CPU ray pick + select() sync XHR), median of 5
//           clicks; and Node time from the click to the recoloured frame;
//           edit -> pixels (LOOP_SIZES): dev.sh's loop, host/mesh.c edited (a constant category
//           byte), `build.sh host`, stop the host, start out/tcc/bimble-ccore with --session on the
//           same port, token and model; until an in-page rAF watcher sees a new canvas hash
//           (bench/lab_loop.mjs's method). LOOP_RUNS runs after one warm-up; build / stop / spawn /
//           pixels and total = edit written -> new pixels.
import { spawn } from "node:child_process";
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, statSync, writeFileSync, appendFileSync } from "node:fs";
import { cpus, loadavg, tmpdir, totalmem } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { execFileSync } from "node:child_process";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = resolve(CCORE, "../web-ui");
const OUT = join(CCORE, "out");
const SIZES = (process.env.SIZES ?? "2000 20000 60000").split(/\s+/).filter(Boolean).map(Number);
const BINS = (process.env.BINS ?? "native tcc").split(/\s+/).filter(Boolean);
const RUNS = Number(process.env.RUNS ?? 5);
const LAB = process.env.LAB !== "0";
const LOOP_RUNS = Number(process.env.LOOP_RUNS ?? 5);
const LOOP_SIZES = (process.env.LOOP_SIZES ?? "20000").split(/\s+/).filter(Boolean).map(Number);
const RAW = process.env.RAW ?? join(OUT, "t16-large.json");
const TOKEN = "t16-large";
const P_HOST = 47372, P_SES = 47373, P_LAB = 47374, VITE = 5205;
const MESH_C = join(CCORE, "host/mesh.c");
const PX_ANCHOR = "buf_putc(out, (char)(unsigned char)e->category);";
const ORIG_MESH = readFileSync(MESH_C, "utf8");
if (ORIG_MESH.split(PX_ANCHOR).length !== 2) throw new Error(`anchor not found once in ${MESH_C}`);

mkdirSync(OUT, { recursive: true });
const tmp = mkdtempSync(join(tmpdir(), "ccore-t16-"));
const procs = [];
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const now = () => performance.now();
const median = (v) => { const s = [...v].sort((a, b) => a - b); const n = s.length; return n ? (n % 2 ? s[(n - 1) / 2] : (s[n / 2 - 1] + s[n / 2]) / 2) : NaN; };
const p95 = (v) => { const s = [...v].sort((a, b) => a - b); return s[Math.min(s.length - 1, Math.ceil(0.95 * s.length) - 1)]; };
const r1 = (x) => Math.round(x * 10) / 10;
const log = (s) => console.log(s);

function start(name, cmd, args, opts = {}) {
  const p = spawn(cmd, args, { stdio: ["ignore", "pipe", "pipe"], detached: true, ...opts });
  p.out = "";
  p.stdout.on("data", (d) => (p.out += d));
  p.stderr.on("data", (d) => (p.out += d));
  p.label = name;
  procs.push(p);
  return p;
}
const alive = (p) => p && p.exitCode === null && p.signalCode === null;
async function stopProc(p, sig = "SIGTERM") {
  if (!alive(p)) return;
  const gone = new Promise((r) => p.once("exit", r));
  try { process.kill(-p.pid, sig); } catch {}
  await Promise.race([gone, sleep(5000)]);
  if (alive(p)) try { process.kill(-p.pid, "SIGKILL"); } catch {}
}
async function waitFor(what, ms, fn, every = 20) {
  const until = Date.now() + ms;
  let last;
  while (Date.now() < until) {
    try { const v = await fn(); if (v) return v; } catch (e) { last = e; }
    await sleep(every);
  }
  throw new Error(`timeout waiting for ${what}${last ? `: ${last.message ?? last}` : ""}`);
}

/** Starts a host; resolves {h, ms} at "listening" (spawn -> listening). */
async function startHost(bin, port, args, label) {
  const t = now();
  const h = start(label, bin, [...args.flags, String(port), args.model], {
    env: { ...process.env, BIMBLE_TOKEN: TOKEN, BIMBLE_ORIGINS: `http://localhost:${VITE},http://127.0.0.1:${VITE}` },
  });
  await waitFor(`${label} 'listening'`, 120000, () => h.out.includes("listening") || (!alive(h) && Promise.reject(new Error(h.out))), 1);
  return { h, ms: now() - t };
}
function rss(pid) {
  const s = readFileSync(`/proc/${pid}/status`, "utf8");
  const kb = (k) => Number(s.match(new RegExp(`^${k}:\\s+(\\d+) kB`, "m"))?.[1] ?? NaN);
  return { rss_mib: r1(kb("VmRSS") / 1024), hwm_mib: r1(kb("VmHWM") / 1024) };
}
const api = (port) => {
  const base = `http://127.0.0.1:${port}`;
  const call = async (op, name, body = "") => {
    const t = now();
    const r = await fetch(`${base}/${op}/${name}?t=${TOKEN}`, { method: "POST", body, headers: { "Content-Type": "text/plain" } });
    const text = await r.text();
    const ms = now() - t;
    if (r.status !== 200) throw new Error(`${op} ${name}: ${r.status} ${text.slice(0, 300)}`);
    return { text, ms };
  };
  const mesh = async () => {
    const t = now();
    const r = await fetch(`${base}/mesh?t=${TOKEN}`);
    const b = await r.arrayBuffer();
    if (r.status !== 200) throw new Error(`mesh ${r.status}`);
    return { ms: now() - t, bytes: b.byteLength, buf: b };
  };
  return { call, mesh, q: async (n, a) => JSON.parse((await call("query", n, a === undefined ? "" : JSON.stringify(a))).text) };
};

function ensureModel(n) {
  const f = join(OUT, `t16-large-${n}.bimble`);
  if (!existsSync(f)) execFileSync("python3", [join(CCORE, "bench/large_model.py"), String(n), f], { stdio: "inherit" });
  const m = JSON.parse(readFileSync(f, "utf8")).model;
  const storey = Object.values(m.storeys).sort((a, b) => a.elevation - b.elevation)[1].id; // Niveau 1
  return { file: f, bytes: statSync(f).size, elements: Object.keys(m.elements).length, storey, anyId: Object.keys(m.elements)[Math.floor(Object.keys(m.elements).length / 2)] };
}
const placeArgs = (storey, k) => JSON.stringify({ a: [3000.0 + 10 * k, 2500.0], kind: "column", storey });

async function hostBench(bin, model) {
  const binPath = join(OUT, bin, "bimble-ccore");
  const r = { bin };
  // Starts (load + validate), RUNS of them; the last host stays for the calls.
  const starts = [];
  let h;
  for (let i = 0; i < RUNS; i++) {
    if (h) await stopProc(h);
    const s = await startHost(binPath, P_HOST, { flags: [], model: model.file }, `host-${bin}`);
    h = s.h;
    starts.push(s.ms);
  }
  r.start_ms = { median: r1(median(starts)), p95: r1(p95(starts)), runs: starts.map(r1) };
  r.after_load = rss(h.pid);
  const A = api(P_HOST);
  const st = await A.q("status");
  r.status_load_ms = r1(st.load_ms);
  // query elements / element
  // `query elements`: the default limit (1000 rows) and the hard cap (limit 10000), as Rust.
  const qe = [], qx = [], q1 = [];
  let qeBytes = 0, qxBytes = 0, n = 0, nx = 0;
  for (let i = 0; i < RUNS; i++) {
    const x = await A.call("query", "elements");
    qe.push(x.ms), (qeBytes = x.text.length), (n = JSON.parse(x.text).length);
    const y = await A.call("query", "elements", JSON.stringify({ limit: 10000 }));
    qx.push(y.ms), (qxBytes = y.text.length), (nx = JSON.parse(y.text).length);
    q1.push((await A.call("query", "element", JSON.stringify({ id: model.anyId }))).ms);
  }
  if (n !== Math.min(1000, model.elements) || nx !== Math.min(10000, model.elements)) throw new Error(`query elements: ${n} / ${nx} rows`);
  r.query_elements_ms = { median: r1(median(qe)), p95: r1(p95(qe)), rows: n, bytes: qeBytes };
  r.query_elements_10k_ms = { median: r1(median(qx)), p95: r1(p95(qx)), rows: nx, bytes: qxBytes };
  r.query_element_ms = { median: r1(median(q1)), p95: r1(p95(q1)) };
  // mesh: first build, then cached fetches
  const before = h.out.length;
  const m1 = await A.mesh();
  const built = h.out.slice(before).match(/mesh rev \d+ built in ([\d.]+) ms, (\d+) bytes/);
  const cached = [];
  for (let i = 0; i < RUNS; i++) cached.push((await A.mesh()).ms);
  r.mesh = { build_ms: built ? r1(Number(built[1])) : null, first_fetch_ms: r1(m1.ms), bytes: m1.bytes, cached_fetch_ms: r1(median(cached)) };
  r.after_mesh = rss(h.pid);
  // place + undo
  const pl = [], un = [], rebuild = [];
  for (let i = 0; i < RUNS; i++) {
    pl.push((await A.call("verb", "place", placeArgs(model.storey, i))).ms);
    if ((await A.q("history")).undo.length !== 1) throw new Error("place: not one undo step");
    const b0 = h.out.length;
    await A.mesh();
    const mb = h.out.slice(b0).match(/built in ([\d.]+) ms/);
    if (mb) rebuild.push(Number(mb[1]));
    un.push((await A.call("undo", "-")).ms);
  }
  if ((await A.q("history")).undo.length !== 0) throw new Error("undo: steps left");
  r.place_ms = { median: r1(median(pl)), p95: r1(p95(pl)) };
  r.undo_ms = { median: r1(median(un)), p95: r1(p95(un)) };
  r.mesh_rebuild_after_place_ms = r1(median(rebuild));
  await stopProc(h);
  // Session: --session on a fresh copy.
  const sm = join(tmp, `s-${bin}-${model.elements}.bimble`);
  copyFileSync(model.file, sm);
  rmSync(`${sm}.ccore-session`, { force: true });
  let s = await startHost(binPath, P_SES, { flags: ["--session"], model: sm }, `ses-${bin}`);
  const B = api(P_SES);
  const spl = [], sun = [];
  for (let i = 0; i < RUNS; i++) {
    spl.push((await B.call("verb", "place", placeArgs(model.storey, i))).ms);
    sun.push((await B.call("undo", "-")).ms);
  }
  await B.call("verb", "place", placeArgs(model.storey, 99)); // the restart keeps one change
  const sessBytes = statSync(`${sm}.ccore-session`).size;
  await stopProc(s.h);
  const rs = await startHost(binPath, P_SES, { flags: ["--session"], model: sm }, `ses2-${bin}`);
  const restored = /session restored/.test(rs.h.out);
  const nAfter = (await B.q("history")).undo.length;
  await stopProc(rs.h);
  if (!restored || nAfter !== 1) throw new Error(`session restore: ${restored}, ${nAfter} undo steps`);
  r.session = {
    place_ms: r1(median(spl)), undo_ms: r1(median(sun)),
    write_ms_est: r1(median(spl) - r.place_ms.median), write_ms_est_undo: r1(median(sun) - r.undo_ms.median),
    file_bytes: sessBytes, restart_ms: r1(rs.ms), plain_start_ms: r.start_ms.median,
  };
  return r;
}

async function cdpConnect(wsUrl) {
  const ws = new WebSocket(wsUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = () => rej(new Error("CDP connect failed")); });
  let id = 0;
  const pending = new Map();
  const listeners = [];
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

function build(mode) {
  return new Promise((res, rej) => {
    const t = now();
    const p = spawn(join(CCORE, "build.sh"), [mode], { cwd: CCORE, stdio: ["ignore", "pipe", "pipe"] });
    let out = "";
    p.stdout.on("data", (d) => (out += d));
    p.stderr.on("data", (d) => (out += d));
    p.on("exit", (c) => {
      appendFileSync(join(OUT, `t16-build-${mode}.log`), out);
      c === 0 ? res(now() - t) : rej(new Error(`build.sh ${mode} exit ${c}\n${out.slice(-2000)}`));
    });
  });
}

let chrome, cdp;
async function labBench(model, size) {
  const r = {};
  const lm = join(tmp, `lab-${size}.bimble`);
  copyFileSync(model.file, lm);
  rmSync(`${lm}.ccore-session`, { force: true });
  let host = (await startHost(join(OUT, "native/bimble-ccore"), P_LAB, { flags: ["--session"], model: lm }, `lab-host-${size}`)).h;
  const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank" });
  const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
  const S = (m, p) => cdp.send(m, p, sessionId);
  const exceptions = [];
  cdp.on((msg) => {
    if (msg.sessionId === sessionId && msg.method === "Runtime.exceptionThrown")
      exceptions.push(msg.params.exceptionDetails.exception?.description ?? msg.params.exceptionDetails.text);
  });
  await S("Runtime.enable");
  await S("Page.enable");
  await S("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  const evalJs = async (expr) => {
    const x = await S("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true });
    if (x.exceptionDetails) throw new Error(x.exceptionDetails.exception?.description ?? x.exceptionDetails.text);
    return x.result.value;
  };
  const url = `http://localhost:${VITE}/?native=1&gl=1&port=${P_LAB}&token=${TOKEN}`;
  // An untimed first navigation (Vite compiles the modules on the first request), then the timed one.
  await S("Page.navigate", { url });
  await waitFor("first frame (warm-up)", 120000, () => evalJs("window.__bbGl && window.__bbGl.firstFrameMs"), 50);
  await S("Page.navigate", { url: "about:blank" });
  await sleep(300);
  const tNav = now();
  await S("Page.navigate", { url });
  const ff = await waitFor("first frame", 120000, () => evalJs("window.__bbGl && window.__bbGl.firstFrameMs"), 50);
  const g = await evalJs("({ ...window.__bbGl })");
  r.first_frame_ms = r1(ff); // page clock: from navigation start
  r.first_frame_node_ms = r1(now() - tNav); // Node: from Page.navigate (up to 50 ms of polling)
  r.mesh_fetch_ms = r1(g.meshFetchMs);
  r.mesh_bytes = g.meshBytes;
  await sleep(1500); // let the shell's own queries settle
  // In-page helpers: rAF intervals, frame waiter, pointerup timing, pixel hash watcher.
  await evalJs(`(() => {
    const c = document.getElementById('bimble'), g = c.getContext('webgl2');
    const R = window.__t16 = { raf: [], rec: false, up: [], px: [] };
    const tick = (t) => { if (R.rec) R.raf.push({ t, f: window.__bbGl.frames }); requestAnimationFrame(tick); };
    requestAnimationFrame(tick);
    const one = new Uint8Array(4);
    // Resolves at the first glviewport frame after f0, finished (1x1 readPixels): ms since the last pointermove.
    R.nextFrame = (f0) => new Promise((res) => { const w = () => { if (window.__bbGl.frames > f0) { g.readPixels(0, 0, 1, 1, g.RGBA, g.UNSIGNED_BYTE, one); res(performance.now() - R.move); } else requestAnimationFrame(w); }; requestAnimationFrame(w); });
    R.move = 0;
    window.addEventListener('pointermove', () => { R.move = performance.now(); }, true);
    let t0 = 0;
    window.addEventListener('pointerup', () => { t0 = performance.now(); }, true);
    window.addEventListener('pointerup', () => { R.up.push(performance.now() - t0); }, false);
    let frames = -1, last = null, buf = null;
    R.watch = () => {
      const f = window.__bbGl.frames;
      if (f !== frames) {
        frames = f;
        const w = c.width, h = c.height;
        if (!buf || buf.length !== w * h * 4) buf = new Uint8Array(w * h * 4);
        g.readPixels(0, 0, w, h, g.RGBA, g.UNSIGNED_BYTE, buf);
        let s = 2166136261;
        for (let i = 0; i < buf.length; i += 4) s = Math.imul(s ^ (buf[i] | (buf[i + 1] << 8) | (buf[i + 2] << 16)), 16777619) >>> 0;
        if (s !== last) { last = s; R.px.push({ t: Date.now(), hash: s }); }
      }
      requestAnimationFrame(R.watch);
    };
  })()`);
  const box = await evalJs("(() => { const b = document.getElementById('bimble').getBoundingClientRect(); return { x: b.left, y: b.top, w: b.width, h: b.height }; })()");
  const cx = Math.round(box.x + box.w / 2), cy = Math.round(box.y + box.h / 2);
  const mouse = (type, x, y, extra = {}) => S("Input.dispatchMouseEvent", { type, x, y, button: "left", clickCount: 1, ...extra });
  // Orbit, latency: 60 moves of 8 px, each waited to a finished frame.
  const mx = (i) => cx + 8 * i - (i > 30 ? 16 * (i - 30) : 0);
  await mouse("mousePressed", cx, cy, { buttons: 1 });
  const moves = [], inPage = [];
  for (let i = 1; i <= 60; i++) {
    const f0 = await evalJs("window.__bbGl.frames");
    const t = now();
    await mouse("mouseMoved", mx(i), cy + (i % 2 ? 2 : -2), { buttons: 1 });
    inPage.push(await evalJs(`window.__t16.nextFrame(${f0})`));
    moves.push(now() - t);
  }
  await mouse("mouseReleased", cx, cy, { buttons: 0 });
  // Orbit, throughput: moves sent for 3 s without waiting for frames; the intervals between the
  // rAF callbacks that saw a new glviewport frame (drawn frames per second while orbiting).
  await mouse("mousePressed", cx, cy, { buttons: 1 });
  await evalJs("window.__t16.raf.length = 0; window.__t16.rec = true");
  const tEnd = now() + 3000;
  for (let i = 1; now() < tEnd; i++) await mouse("mouseMoved", mx(1 + (i % 60)), cy + (i % 2 ? 2 : -2), { buttons: 1 });
  await evalJs("window.__t16.rec = false");
  await mouse("mouseReleased", cx, cy, { buttons: 0 });
  const raf = await evalJs("window.__t16.raf");
  const drawn = raf.filter((x, i) => i > 0 && x.f !== raf[i - 1].f);
  const dts = [];
  for (let i = 1; i < drawn.length; i++) dts.push(drawn[i].t - drawn[i - 1].t);
  r.orbit_move_to_frame_ms = { p50: r1(median(moves)), p95: r1(p95(moves)) };
  r.orbit_in_page_ms = { p50: r1(median(inPage)), p95: r1(p95(inPage)) };
  r.orbit_frame_interval_ms = { p50: r1(median(dts)), p95: r1(p95(dts)), frames: drawn.length };
  // Click-pick: 5 clicks on the centre (no drag).
  const picks = [], toFrame = [];
  for (let i = 0; i < 5; i++) {
    await evalJs("window.__t16.up.length = 0");
    const p0 = await evalJs("window.__bbGl.picks");
    const f0 = await evalJs("window.__bbGl.frames");
    const t = now();
    await mouse("mousePressed", cx, cy, { buttons: 1 });
    await mouse("mouseReleased", cx, cy, { buttons: 0 });
    const ok = await waitFor("pick", 20000, () => evalJs(`window.__bbGl.picks > ${p0}`), 1);
    picks.push((await evalJs("window.__t16.up"))[0]);
    // The first click selects (recolour frame); later clicks on the same element change nothing.
    if (i === 0) {
      await evalJs(`window.__t16.nextFrame(${f0})`);
      toFrame.push(now() - t);
    }
    if (!ok) throw new Error("no pick");
  }
  const sel = (await (await fetch(`http://127.0.0.1:${P_LAB}/change?since=0&t=${TOKEN}`)).json()).change.selection;
  r.pick_ms = { median: r1(median(picks)), runs: picks.map(r1) };
  r.click_to_selected_frame_ms = r1(toFrame[0]);
  r.pick_hit = sel.length === 1;
  // Edit -> pixels (dev.sh's loop), on the LOOP_SIZES.
  if (LOOP_SIZES.includes(size)) {
    await evalJs("requestAnimationFrame(window.__t16.watch)");
    await sleep(500);
    const runs = [];
    for (let k = 0; k <= LOOP_RUNS; k++) {
      const cat = 1 + (k % 6);
      const pxBefore = await evalJs("window.__t16.px.at(-1)?.hash ?? null");
      const nPx = await evalJs("window.__t16.px.length");
      const t0 = Date.now();
      writeFileSync(MESH_C, ORIG_MESH.replace(PX_ANCHOR, `buf_putc(out, (char)(unsigned char)${cat}); /* t16 edit */`));
      const b = await build("host");
      const t1 = Date.now();
      const ts = now();
      await stopProc(host);
      const stop = now() - ts;
      const s = await startHost(join(OUT, "tcc/bimble-ccore"), P_LAB, { flags: ["--session"], model: lm }, `loop-host-${size}`);
      host = s.h;
      if (!/session restored/.test(host.out)) throw new Error(`loop: no session restore: ${host.out}`);
      const px = await waitFor("new pixels", 120000, async () => {
        const all = await evalJs(`window.__t16.px.slice(${nPx})`);
        return all.find((e) => e.hash !== pxBefore && e.t >= t1);
      }, 10);
      writeFileSync(MESH_C, ORIG_MESH);
      const run = { build: r1(b), stop: r1(stop), spawn: r1(s.ms), page_pixels: px.t - t1, total_pixels: px.t - t0 };
      log(`  loop ${size} ${k === 0 ? "warmup" : `run ${k}`}: ${JSON.stringify(run)}`);
      if (k > 0) runs.push(run);
    }
    r.loop = {};
    for (const key of ["build", "stop", "spawn", "page_pixels", "total_pixels"])
      r.loop[key] = { median: r1(median(runs.map((x) => x[key]))), p95: r1(p95(runs.map((x) => x[key]))) };
    r.loop.runs = runs;
    await build("host"); // the binary from the restored source
  }
  if (exceptions.length) r.page_exceptions = exceptions;
  await cdp.send("Target.closeTarget", { targetId });
  await stopProc(host);
  return r;
}

async function main() {
  const machine = {
    cpu: `${cpus().length} x ${cpus()[0]?.model ?? "?"}`, mem_gib: (totalmem() / 2 ** 30).toFixed(1), node: process.version,
    loadavg_start: loadavg().map((x) => x.toFixed(2)).join(" "),
    clang: execFileSync("clang", ["--version"]).toString().split("\n")[0], tcc: execFileSync("tcc", ["-v"]).toString().trim(),
  };
  for (const b of BINS) if (!existsSync(join(OUT, b, "bimble-ccore"))) throw new Error(`no out/${b}/bimble-ccore: build.sh ${b}`);
  const result = { date: new Date().toISOString(), machine, runs: RUNS, sizes: {} };
  for (const n of SIZES) {
    const model = ensureModel(n);
    log(`model ${n}: ${model.elements} elements, ${model.bytes} bytes`);
    result.sizes[n] = { elements: model.elements, file_bytes: model.bytes, host: {} };
    for (const b of BINS) {
      result.sizes[n].host[b] = await hostBench(b, model);
      log(`  host ${b}: ${JSON.stringify(result.sizes[n].host[b])}`);
    }
  }
  if (LAB) {
    if (!existsSync(join(UI, "node_modules"))) throw new Error(`no ${UI}/node_modules: run npm ci in web-ui/`);
    machine.chromium = execFileSync("/usr/bin/chromium", ["--version"]).toString().trim();
    const vite = start("vite", "npx", ["vite", "--port", String(VITE), "--strictPort"], { cwd: UI });
    await waitFor(`Vite on ${VITE}`, 30000, async () => { if (!alive(vite)) throw new Error(vite.out); return (await fetch(`http://localhost:${VITE}/`)).ok; }, 200);
    chrome = start("chromium", "/usr/bin/chromium", [
      "--headless=new", "--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--remote-debugging-port=0",
      `--user-data-dir=${join(tmp, "chromium")}`, "--no-first-run", "--no-default-browser-check", "about:blank",
    ]);
    const wsUrl = await waitFor("DevTools line", 15000, () => chrome.out.match(/DevTools listening on (ws:\/\/\S+)/)?.[1], 100);
    cdp = await cdpConnect(wsUrl);
    for (const n of SIZES) {
      const model = ensureModel(n);
      result.sizes[n].lab = await labBench(model, n);
      log(`  lab ${n}: ${JSON.stringify({ ...result.sizes[n].lab, loop: result.sizes[n].lab.loop ? { ...result.sizes[n].lab.loop, runs: undefined } : undefined })}`);
    }
    cdp.close();
  }
  machine.loadavg_end = loadavg().map((x) => x.toFixed(2)).join(" ");
  writeFileSync(RAW, JSON.stringify(result, null, 1));
  log(`raw: ${RAW}; load average ${machine.loadavg_start} -> ${machine.loadavg_end}`);
}

let code = 1;
try {
  await main();
  code = 0;
} catch (e) {
  log(`FAIL: ${e.stack ?? e}`);
  for (const p of procs) log(`--- ${p.label} ---\n${p.out.slice(-1500)}`);
} finally {
  if (readFileSync(MESH_C, "utf8") !== ORIG_MESH) {
    writeFileSync(MESH_C, ORIG_MESH);
    await build("host").catch((e) => log(`FAIL: rebuild host: ${e.message}`));
  }
  for (const p of procs) await stopProc(p);
  rmSync(tmp, { recursive: true, force: true });
}
process.exit(code);
