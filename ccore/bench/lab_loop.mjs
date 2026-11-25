// The C core's dev loop in the real Vue lab (ticket T12): edit a constant in C -> build.sh ->
// restart the host on the same port, token and session -> the lab page shows the change.
//   RUNS=10 MODES="tcc native" node ccore/bench/lab_loop.mjs   (MODES may add "host": build.sh host, dev.sh's build)     (or bench/lab_loop.sh)
// Needs web-ui/node_modules, /usr/bin/chromium, tcc, clang. Ports: host 47342, Vite 5197.
//
// Method (after spikes/native-twin/exp2/loop_browser.mjs, the 1.66 s / 4.66 s baselines):
// one Vite, one headless Chromium (raw CDP, SwiftShader, 1280x800), the lab opened ONCE at
// /?native=1&gl=1&port=&token= (no reload in any run). The host runs with --session on a copy of
// golden/frame.json; before the runs the page places a column and selects it, and every run
// checks that the page still shows that state after the restart (73 elements, the selection,
// one undo step). One untimed warm-up per mode, then the runs alternate the modes.
// Each run makes two edits at once, both reverted after the run (the source is restored on exit):
//   pixels: session/mesh.c writes a constant MaterialCategory byte instead of `e->category` (each
//           run a different one): every element changes colour in the WebGL canvas;
//   text:   session/calls.c `225.0` (bytes per stored triangle, `query status` model_bytes) becomes
//           a constant chosen so the status bar's "{mb} MB" shows a new number M.
// Times (wall clock; Node and page stamps are both Date.now(), same machine):
//   t0 edit written; build = build.sh <mode> (the whole script: library, test binaries, host);
//   stop = SIGTERM -> old host exited; spawn = spawn -> "listening" on stderr;
//   page times from the end of the build (stop + spawn + the page noticing):
//     query  in-page `query('status')` (bimble.ts, sync XHR every 20 ms, started at the
//            restart) returns the new model_bytes: the baselines' end point;
//     text   a MutationObserver sees "[data-bb=status-counts]" contain "M MB" (DOM updated;
//            the paint follows within a frame);
//     pixels a requestAnimationFrame watcher hashes the canvas (readPixels) after each
//            glviewport draw; the first hash that differs from the run's start (frame drawn;
//            presentation follows within a frame). Checked: /mesh then holds the new byte.
//   total = edit -> visible = build + page time.
// Probe (PROBE=0 to skip): "tcc-retry20" runs the tcc loop with the page's 1000 ms timers made
// 20 ms (window.setTimeout wrapped in the page, only during those runs): what the loop would
// take if bimble.ts pollChanges retried at once after the host went away. Not the lab as it is.
// Raw runs: out/t12-lab_loop.json; build logs out/t12-build-<mode>.log. Kills all it starts.
import { spawn } from "node:child_process";
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync, appendFileSync, renameSync } from "node:fs";
import { cpus, tmpdir, totalmem, loadavg } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { execFileSync } from "node:child_process";
import { CHROMIUM, port, vitePort } from "../test/ports.mjs";

/** Write a source file atomically (temp + rename): a background compile reading the old file must
 *  never see it truncated (clang mmaps sources; an in-place rewrite gave it SIGBUS). */
function writeAtomic(p, data) {
  const t = `${p}.tmp${process.pid}`;
  writeFileSync(t, data);
  renameSync(t, p);
}

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = resolve(CCORE, "../web-ui");
const RUNS = Number(process.env.RUNS ?? 10);
const MODES = (process.env.MODES ?? "tcc native").split(/\s+/).filter(Boolean);
const SPECS = [...MODES.map((m) => ({ name: m, mode: m, fast: false })),
  ...(process.env.PROBE === "0" || !MODES.includes("tcc") ? [] : [{ name: "tcc-retry20", mode: "tcc", fast: true }])];
const PORT = port("lab_loop", 47342);
const VITE = vitePort("lab_loop", 5197);
const TOKEN = "lab-loop";
const HOST = `http://127.0.0.1:${PORT}`;
const PAGE = `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}`;
const STOREY = "1uAZA0Yfn05f$xRd1k4uPR"; // golden/frame.json's lowest storey (host_test.mjs)
const OUT = join(CCORE, "out");
const RAW = join(OUT, "t12-lab_loop.json");
const MESH_C = join(CCORE, "session/mesh.c");
const CALLS_C = join(CCORE, "session/calls.c");
const PX_ANCHOR = "buf_putc(out, (char)(unsigned char)e->category);";
const TX_ANCHOR = "(uint64_t)(225.0 * (double)stored)";
const ORIG_MESH = readFileSync(MESH_C, "utf8");
const ORIG_CALLS = readFileSync(CALLS_C, "utf8");
for (const [f, text, a] of [[MESH_C, ORIG_MESH, PX_ANCHOR], [CALLS_C, ORIG_CALLS, TX_ANCHOR]])
  if (text.split(a).length !== 2) throw new Error(`anchor not found once in ${f}: ${a}`);

mkdirSync(OUT, { recursive: true });
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-loop-"));
const MODEL = join(tmp, "frame.bimble");
const procs = [];
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const now = () => Date.now();

function start(name, cmd, args, opts) {
  const p = spawn(cmd, args, { stdio: ["ignore", "pipe", "pipe"], detached: true, ...opts });
  p.out = "";
  p.stdout.on("data", (d) => (p.out += d));
  p.stderr.on("data", (d) => (p.out += d));
  p.label = name;
  procs.push(p);
  return p;
}
const alive = (p) => p && p.exitCode === null && p.signalCode === null;
async function stopProc(p) {
  if (!alive(p)) return;
  const gone = new Promise((r) => p.once("exit", r));
  try { process.kill(-p.pid, "SIGTERM"); } catch {}
  await Promise.race([gone, sleep(3000)]);
  if (alive(p)) try { process.kill(-p.pid, "SIGKILL"); } catch {}
}

async function waitFor(what, ms, fn, every = 20) {
  const until = Date.now() + ms;
  let last;
  while (Date.now() < until) {
    try {
      const v = await fn();
      if (v) return v;
    } catch (e) {
      last = e;
    }
    await sleep(every);
  }
  throw new Error(`timeout waiting for ${what}${last ? `: ${last.message ?? last}` : ""}`);
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

/** build.sh <mode>: {ms, steps: {label: ms}}; its output appended to out/t12-build-<mode>.log. */
function build(mode) {
  return new Promise((res, rej) => {
    const t = now();
    const p = spawn(join(CCORE, "build.sh"), [mode], { cwd: CCORE, stdio: ["ignore", "pipe", "pipe"] });
    let out = "";
    p.stdout.on("data", (d) => (out += d));
    p.stderr.on("data", (d) => (out += d));
    p.on("exit", (c) => {
      const ms = now() - t;
      appendFileSync(join(OUT, `t12-build-${mode}.log`), out);
      if (c !== 0) return rej(new Error(`build.sh ${mode} exit ${c}\n${out.slice(-3000)}`));
      const steps = {};
      for (const m of out.matchAll(/^ {2}(.+?)\s+(\d+) ms$/gm)) steps[m[1].trim()] = Number(m[2]);
      res({ ms, steps });
    });
  });
}

let host = null;
async function startHost(mode) {
  const t = now();
  host = start(`host-${mode}`, join(CCORE, `out/${mode === "host" ? "tcc" : mode}/bimble-ccore`), ["--session", String(PORT), MODEL], {
    env: { ...process.env, BIMBLE_TOKEN: TOKEN, BIMBLE_ORIGINS: `http://localhost:${VITE},http://127.0.0.1:${VITE}` },
  });
  await waitFor("host 'listening'", 5000, () => host.out.includes("listening") || (!alive(host) && Promise.reject(new Error(host.out))), 1);
  return now() - t;
}

const q = async (name, args) => {
  const r = await fetch(`${HOST}/query/${name}?t=${TOKEN}`, { method: "POST", body: args === undefined ? "" : JSON.stringify(args) });
  const text = await r.text();
  if (!r.ok) throw new Error(`query ${name}: ${r.status} ${text}`);
  return JSON.parse(text);
};
const hostChange = async () => (await (await fetch(`${HOST}/change?since=0&t=${TOKEN}`)).json()).change;
/** The MaterialCategory byte of the first element of GET /mesh (BMSH offset 52 + 22). */
const meshCat = async () => new Uint8Array(await (await fetch(`${HOST}/mesh?t=${TOKEN}`)).arrayBuffer())[52 + 22];

const median = (v) => { const s = [...v].sort((a, b) => a - b); const n = s.length; return n % 2 ? s[(n - 1) / 2] : (s[n / 2 - 1] + s[n / 2]) / 2; };
const p95 = (v) => { const s = [...v].sort((a, b) => a - b); return s[Math.min(s.length - 1, Math.ceil(0.95 * s.length) - 1)]; };

function restoreSource() {
  writeAtomic(MESH_C, ORIG_MESH);
  writeAtomic(CALLS_C, ORIG_CALLS);
}

async function main() {
  if (!existsSync(join(UI, "node_modules"))) throw new Error(`no ${UI}/node_modules: run npm ci in web-ui/`);
  for (const m of MODES) if (!["tcc", "host", "native"].includes(m)) throw new Error(`unknown mode ${m}`);
  for (const m of ["tcc", "host", "native"]) writeFileSync(join(OUT, `t12-build-${m}.log`), "");
  copyFileSync(join(CCORE, "golden/frame.json"), MODEL);
  // Every mode's host exists before the first restart.
  for (const m of MODES) await build(m);

  const machine = {
    cpu: `${cpus().length} x ${cpus()[0]?.model ?? "?"}`, mem_gib: (totalmem() / 2 ** 30).toFixed(1),
    node: process.version, loadavg_start: loadavg().map((x) => x.toFixed(2)).join(" "),
    tcc: execFileSync("tcc", ["-v"]).toString().trim(), clang: execFileSync("clang", ["--version"]).toString().split("\n")[0],
    chromium: execFileSync(CHROMIUM, ["--version"]).toString().trim(),
  };
  console.log(`machine: ${JSON.stringify(machine)}`);

  const first = MODES[0];
  console.log(`host start (${first}): ${await startHost(first)} ms`);
  const vite = start("vite", "npx", ["vite", "--port", String(VITE), "--strictPort"], { cwd: UI });
  await waitFor(`Vite on ${VITE}`, 30000, async () => {
    if (!alive(vite)) throw new Error(vite.out);
    return (await fetch(`http://localhost:${VITE}/`)).ok;
  }, 200);
  const chrome = start("chromium", CHROMIUM, [
    "--headless=new", "--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--remote-debugging-port=0",
    `--user-data-dir=${join(tmp, "chromium")}`, "--no-first-run", "--no-default-browser-check", "about:blank",
  ]);
  const wsUrl = await waitFor("DevTools line", 15000, () => chrome.out.match(/DevTools listening on (ws:\/\/\S+)/)?.[1], 100);
  const cdp = await cdpConnect(wsUrl);
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
  await S("Page.navigate", { url: PAGE });
  const evalJs = async (expr) => {
    const r = await S("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
    return r.result.value;
  };
  await waitFor("the first drawn frame", 30000, () => evalJs("window.__bbGl && window.__bbGl.firstFrameMs"), 100);

  // The state the page must keep: a column placed and selected from the page (bimble.ts).
  const before = new Set((await q("elements")).map((e) => e.id));
  const perr = await evalJs(`(async () => (await import('/src/bimble.ts')).runVerb('place', ${JSON.stringify({ a: [1500.0, 1500.0], kind: "column", storey: STOREY })}))()`);
  if (perr) throw new Error(`page place: ${perr}`);
  const col = (await q("elements")).map((e) => e.id).find((id) => !before.has(id));
  const serr = await evalJs(`(async () => (await import('/src/bimble.ts')).select([${JSON.stringify(col)}]))()`);
  if (serr) throw new Error(`page select: ${serr}`);
  await waitFor("page state shows the selection", 5000, () => evalJs(`(async () => (await import('/src/bimble.ts')).state.value.selection.includes(${JSON.stringify(col)}))()`));
  const st0 = await q("status");
  const stored = Math.round(st0.model_bytes / 225);
  if (stored * 225 !== st0.model_bytes) throw new Error(`model_bytes ${st0.model_bytes} is not 225 x stored`);
  console.log(`state kept across restarts: column ${col} placed and selected; 73 elements; stored triangles ${stored}`);

  // In-page recorders: pixels (after each glviewport draw), the status text (DOM mutations),
  // and the baselines' end point (query('status'), armed per run).
  await evalJs(`(async () => {
    const b = await import('/src/bimble.ts');
    const R = window.__t12 = { px: [], text: [], q: null, b, fast: false };
    const st = window.setTimeout.bind(window);
    window.setTimeout = (fn, ms, ...a) => st(fn, R.fast && ms === 1000 ? 20 : ms, ...a);
    const c = document.getElementById('bimble'), g = c.getContext('webgl2');
    let frames = -1, last = null, buf = null;
    const tick = () => {
      const f = window.__bbGl.frames;
      if (f !== frames) {
        frames = f;
        const w = c.width, h = c.height;
        if (!buf || buf.length !== w * h * 4) buf = new Uint8Array(w * h * 4);
        g.readPixels(0, 0, w, h, g.RGBA, g.UNSIGNED_BYTE, buf);
        let s = 2166136261;
        for (let i = 0; i < buf.length; i += 4) s = Math.imul(s ^ (buf[i] | (buf[i + 1] << 8) | (buf[i + 2] << 16)), 16777619) >>> 0;
        if (s !== last) { last = s; R.px.push({ t: Date.now(), hash: s, frames: f }); }
      }
      requestAnimationFrame(tick);
    };
    requestAnimationFrame(tick);
    const el = () => document.querySelector('[data-bb="status-counts"]')?.textContent ?? '';
    let text = el();
    R.text.push({ t: Date.now(), text });
    new MutationObserver(() => { const x = el(); if (x !== text) { text = x; R.text.push({ t: Date.now(), text: x }); } })
      .observe(document.body, { subtree: true, childList: true, characterData: true });
    R.arm = (mb) => {
      R.q = null;
      const id = setInterval(() => {
        try {
          const s = b.query('status');
          if (s && Math.round(s.model_bytes / 1048576) === mb) { R.q = Date.now(); clearInterval(id); }
        } catch {}
      }, 20);
      setTimeout(() => clearInterval(id), 20000);
    };
  })()`);
  await sleep(500);
  console.log(`status text before the runs: ${await evalJs("window.__t12.text.at(-1).text")}`);

  let k = 0; // edit counter: each edit shows a new category and a new MB value
  const runs = [];
  const one = async ({ name, mode, fast }, label) => {
    await evalJs(`window.__t12.fast = ${fast}`);
    k++;
    const cat = 1 + ((k - 1) % 6); // steel … not concrete (0): the original colour
    const mb = 100 + k;
    const c = ((mb * 1048576 + 1000) / stored).toFixed(6);
    const pxBefore = (await evalJs("window.__t12.px.at(-1)?.hash")) ?? null;
    const nPx = await evalJs("window.__t12.px.length"), nText = await evalJs("window.__t12.text.length");
    const t0 = now();
    writeAtomic(MESH_C, ORIG_MESH.replace(PX_ANCHOR, `buf_putc(out, (char)(unsigned char)${cat}); /* lab_loop edit */`));
    writeAtomic(CALLS_C, ORIG_CALLS.replace(TX_ANCHOR, `(uint64_t)(${c} * (double)stored)`));
    const b = await build(mode);
    const t1 = now();
    await evalJs(`window.__t12.arm(${mb})`);
    const ts = now();
    await stopProc(host);
    const stopMs = now() - ts;
    const spawnMs = await startHost(mode);
    const want = ` ${mb} MB`;
    const seen = await waitFor(`run ${label}: the page shows ${want} and new pixels`, 20000, async () => {
      const r = JSON.parse(await evalJs(`JSON.stringify({ q: window.__t12.q, px: window.__t12.px.slice(${nPx}), text: window.__t12.text.slice(${nText}) })`));
      const tx = r.text.find((e) => e.text.includes(want));
      const px = r.px.find((e) => e.hash !== pxBefore && e.t >= t1); // drawn after the restart began
      return r.q && tx && px ? { q: r.q, text: tx.t, px: px.t } : null;
    });
    restoreSource();
    // Checks: the host serves the new build; the page kept its state.
    const cat1 = await meshCat();
    const ch = await hostChange();
    const keep = JSON.parse(await evalJs(`(async () => { const b = window.__t12.b; return JSON.stringify({ sel: b.state.value.selection, undo: b.query('history').undo.length, n: b.query('elements').length }); })()`));
    const kept = cat1 === cat && ch.selection[0] === col && keep.sel[0] === col && keep.undo === 1 && keep.n === 73;
    if (!kept) throw new Error(`run ${label}: mesh category ${cat1} (want ${cat}), host selection ${JSON.stringify(ch.selection)}, page ${JSON.stringify(keep)}`);
    const r = {
      name, mode, label, build: b.ms, build_steps: b.steps, stop: stopMs, spawn: spawnMs,
      query: seen.q - t1, text: seen.text - t1, pixels: seen.px - t1,
      total_query: seen.q - t0, total_text: seen.text - t0, total_pixels: seen.px - t0,
    };
    console.log(`${name.padEnd(11)} run ${String(label).padEnd(6)} build ${String(r.build).padStart(5)}  stop ${String(r.stop).padStart(3)}  spawn->listening ${String(r.spawn).padStart(3)}` +
      `  page: query ${String(r.query).padStart(5)} text ${String(r.text).padStart(5)} pixels ${String(r.pixels).padStart(5)}` +
      `  total: query ${String(r.total_query).padStart(5)} text ${String(r.total_text).padStart(5)} pixels ${String(r.total_pixels).padStart(5)} ms`);
    return r;
  };

  for (const spec of SPECS) await one(spec, "warmup");
  for (let i = 1; i <= RUNS; i++) for (const spec of SPECS) runs.push(await one(spec, i));
  await evalJs("window.__t12.fast = false");

  const keys = ["build", "stop", "spawn", "query", "text", "pixels", "total_query", "total_text", "total_pixels"];
  const summary = {};
  for (const { name: m } of SPECS) {
    const rs = runs.filter((r) => r.name === m);
    summary[m] = {};
    for (const key of keys) summary[m][key] = { median: median(rs.map((r) => r[key])), p95: p95(rs.map((r) => r[key])) };
    const steps = {};
    for (const s of Object.keys(rs[0]?.build_steps ?? {})) steps[s] = median(rs.map((r) => r.build_steps[s] ?? 0));
    summary[m].build_steps_median = steps;
    console.log(`median ${m}: ${keys.map((x) => `${x} ${summary[m][x].median}`).join(", ")} ms`);
    console.log(`p95    ${m}: ${keys.map((x) => `${x} ${summary[m][x].p95}`).join(", ")} ms`);
    console.log(`build.sh ${m} steps (median ms): ${JSON.stringify(steps)}`);
  }
  machine.loadavg_end = loadavg().map((x) => x.toFixed(2)).join(" ");
  writeFileSync(RAW, JSON.stringify({ date: new Date().toISOString(), machine, runs_per_mode: RUNS, specs: SPECS, stored, runs, summary }, null, 1));
  console.log(`raw runs: ${RAW}; load average end: ${machine.loadavg_end}`);
  if (exceptions.length) throw new Error(`uncaught page exceptions: ${exceptions.join(" | ")}`);
  cdp.close();
}

let code = 1;
try {
  await main();
  code = 0;
} catch (e) {
  console.log(`FAIL: ${e.stack ?? e}`);
  for (const p of procs) console.log(`--- ${p.label} output ---\n${p.out.slice(-2000)}`);
} finally {
  restoreSource();
  for (const p of procs) await stopProc(p);
  rmSync(tmp, { recursive: true, force: true });
  // Leave the binaries built from the restored source.
  for (const m of MODES) await build(m).catch((e) => { console.log(`FAIL: rebuild ${m}: ${e.message}`); code = 1; });
}
process.exit(code);
