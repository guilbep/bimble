// The Rust twin's dev loop in the real Vue lab (ADR 0066): edit a constant in Rust -> cargo build ->
// restart the headless twin on the same port and token -> the lab page shows the change.
//   node web-ui/e2e/twin_loop.mjs        (npm run e2e:twin-loop from web-ui/)
//   RUNS=10 LINKERS="mold lld" node web-ui/e2e/twin_loop.mjs
// Needs web-ui/node_modules, /usr/bin/chromium, cargo. Ports: twin 47362, Vite 5203.
// Appends the result to spikes/native-twin/exp2/results_browser.txt; raw runs in target/tw1-twin_loop.json.
//
// Method (after spikes/native-twin/exp2/loop_browser.mjs, the 1.66 s / 4.66 s baselines, and the C
// core's ccore/bench/lab_loop.mjs): one Vite, one headless Chromium (raw CDP, SwiftShader, 1280x800),
// the lab opened ONCE at /?native=1&port=&token= (no reload in any run), the twin on `bimble demo`.
// Linkers (LINKERS, default "mold"; each also: lld, default) as scripts/twin-dev.sh picks them:
// CARGO_TARGET_<host>_RUSTFLAGS. A linker change rebuilds every crate: one untimed build per linker,
// then one untimed warm-up run, then RUNS runs.
// Each run makes two edits at once in crates/bimble-viewer/src/lib.rs, reverted after the run
// (and on exit):
//   text:   `status()` reports app_bytes = M MiB (M = 100 + run counter): the status bar
//           ([data-bb=status-counts]) shows "M MB";
//   pixels: `draw_options()` sets the 3D background to a new colour: every streamed frame changes.
// Times (wall clock; Node and page stamps are both Date.now(), same machine):
//   t0 edit written; build = cargo build -p bimble-cli (dev profile) until exit (t1);
//   stop = SIGTERM -> old twin exited; spawn = spawn -> "listening" on stderr; restart = stop + spawn;
//   page times from t1 (restart + the page noticing):
//     value  in-page `query('status')` (bimble.ts sync XHR every 20 ms, armed at t1) gives M MB:
//            the baselines' end point;
//     text   a MutationObserver sees "[data-bb=status-counts]" contain " M MB" (DOM updated);
//     pixels a requestAnimationFrame watcher hashes the canvas after each streamed frame
//            (window.__bbViewport.frames); the first hash that differs from the run's start,
//            stamped after t1 (frame drawn with putImageData; presentation follows in a frame).
//   total = edit -> visible = build + page time.
// Checks per run: the twin serves the new status (M MB) and the frame's corner pixel is the new
// background. Kills all it starts; the source is restored on exit.
import { spawn, execFileSync } from "node:child_process";
import { appendFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { cpus, tmpdir, totalmem, loadavg } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const UI = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const ROOT = resolve(UI, "..");
const RUNS = Number(process.env.RUNS ?? 10);
const LINKERS = (process.env.LINKERS ?? "mold").split(/\s+/).filter(Boolean);
const PORT = 47362;
const VITE = 5203;
const TOKEN = "twin-loop";
const TWIN = `http://127.0.0.1:${PORT}`;
const PAGE = `http://localhost:${VITE}/?native=1&port=${PORT}&token=${TOKEN}`;
const BIN = join(ROOT, "target/debug/bimble");
const LIB = join(ROOT, "crates/bimble-viewer/src/lib.rs");
const OUT = join(ROOT, "spikes/native-twin/exp2/results_browser.txt");
const RAW = join(ROOT, "target/tw1-twin_loop.json");
const BUILD_LOG = join(ROOT, "target/tw1-twin_loop-build.log");
const TX_ANCHOR = "        let app_bytes = big::in_use();\n";
const PX_ANCHOR = "            screen_edges: self.moving_lean && !self.gpu_safe,\n            ..self.opts.clone()\n";
const ORIG = readFileSync(LIB, "utf8");
for (const a of [TX_ANCHOR, PX_ANCHOR]) if (ORIG.split(a).length !== 2) throw new Error(`anchor not found once in ${LIB}: ${a}`);

const tmp = mkdtempSync(join(tmpdir(), "bimble-twin-loop-"));
const MODEL = join(tmp, "frame.bimble");
const procs = [];
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const now = () => Date.now();
const restoreSource = () => writeFileSync(LIB, ORIG);

const host = execFileSync("rustc", ["-vV"]).toString().match(/^host: (.*)$/m)[1];
const HOSTVAR = `CARGO_TARGET_${host.toUpperCase().replace(/-/g, "_")}_RUSTFLAGS`;
/** The rustflags of scripts/twin-dev.sh for this linker. */
function linkerFlags(name) {
  if (name === "default") return "";
  if (name === "lld") {
    execFileSync("sh", ["-c", "command -v ld.lld"]);
    return "-C link-arg=-fuse-ld=lld";
  }
  if (name === "mold") {
    try {
      const d = execFileSync("mise", ["where", "mold"], { stdio: ["ignore", "pipe", "ignore"] }).toString().trim();
      const ld = execFileSync("find", [d, "-path", "*/libexec/mold/ld", "-print", "-quit"]).toString().trim();
      if (ld) return `-C link-arg=-B${dirname(ld)}`;
    } catch {}
    execFileSync("sh", ["-c", "command -v mold"]);
    return "-C link-arg=-fuse-ld=mold";
  }
  throw new Error(`unknown linker ${name}`);
}

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

let env = { ...process.env };
/** cargo build -p bimble-cli: ms; output appended to the build log; throws on failure. */
function build() {
  return new Promise((res, rej) => {
    const t = now();
    const p = spawn("cargo", ["build", "-p", "bimble-cli"], { cwd: ROOT, env, stdio: ["ignore", "pipe", "pipe"] });
    let out = "";
    p.stdout.on("data", (d) => (out += d));
    p.stderr.on("data", (d) => (out += d));
    p.on("exit", (c) => {
      appendFileSync(BUILD_LOG, out);
      if (c !== 0) rej(new Error(`cargo build exit ${c}\n${out.slice(-3000)}`));
      else res(now() - t);
    });
  });
}

let twin = null;
async function startTwin() {
  const t = now();
  const e = { ...env, BIMBLE_SERVE: String(PORT), BIMBLE_TOKEN: TOKEN, BIMBLE_HEADLESS: "1",
    BIMBLE_ORIGINS: `http://localhost:${VITE},http://127.0.0.1:${VITE}` };
  delete e.WAYLAND_DISPLAY;
  delete e.DISPLAY;
  twin = start("twin", BIN, ["view", MODEL], { env: e });
  await waitFor("twin 'listening'", 10000,
    () => twin.out.includes("bimble twin: listening") || (!alive(twin) && Promise.reject(new Error(twin.out))), 1);
  return now() - t;
}

const q = async (name) => {
  const r = await fetch(`${TWIN}/query/${name}?t=${TOKEN}`, { method: "POST", body: "" });
  const text = await r.text();
  if (!r.ok) throw new Error(`query ${name}: ${r.status} ${text}`);
  return JSON.parse(text);
};
const mbOf = (s) => Math.round((s.app_bytes || s.model_bytes) / 1048576);
/** The first pixel of a 64x48 frame from the twin. */
const corner = async () => {
  const r = await fetch(`${TWIN}/frame?w=64&h=48&t=${TOKEN}`, { cache: "no-store" });
  if (!r.ok) throw new Error(`frame: ${r.status}`);
  return [...new Uint8Array(await r.arrayBuffer()).slice(0, 3)];
};

const median = (v) => { const s = [...v].sort((a, b) => a - b); const n = s.length; return n % 2 ? s[(n - 1) / 2] : (s[n / 2 - 1] + s[n / 2]) / 2; };
const p95 = (v) => { const s = [...v].sort((a, b) => a - b); return s[Math.min(s.length - 1, Math.ceil(0.95 * s.length) - 1)]; };
const log = (line) => { console.log(line); appendFileSync(OUT, line + "\n"); };

async function main() {
  if (!existsSync(join(UI, "node_modules"))) throw new Error(`no ${UI}/node_modules: run npm ci in web-ui/`);
  writeFileSync(BUILD_LOG, "");
  env = { ...process.env, [HOSTVAR]: linkerFlags(LINKERS[0]) };
  console.log(`untimed build (${LINKERS[0]})...`);
  await build();
  execFileSync(BIN, ["demo", MODEL], { stdio: "ignore" });
  console.log(`twin start: ${await startTwin()} ms`);
  const vite = start("vite", "npx", ["vite", "--port", String(VITE), "--strictPort"], { cwd: UI });
  await waitFor(`Vite on ${VITE}`, 30000, async () => {
    if (!alive(vite)) throw new Error(vite.out);
    return (await fetch(`http://localhost:${VITE}/`)).ok;
  }, 200);
  const chrome = start("chromium", "/usr/bin/chromium", [
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
  await waitFor("data-bb-ready and a streamed frame", 60000,
    () => evalJs("document.documentElement.dataset.bbReady === '1' && (window.__bbViewport?.frames ?? 0) > 0"), 100);

  // In-page recorders: pixels (after each streamed frame), the status text (DOM mutations),
  // and the baselines' end point (query('status'), armed per run).
  await evalJs(`(async () => {
    const b = await import('/src/bimble.ts');
    const R = window.__tw1 = { px: [], text: [], q: null };
    const c = document.getElementById('bimble'), g = c.getContext('2d');
    let frames = -1, last = null;
    const tick = () => {
      const f = window.__bbViewport?.frames ?? 0;
      if (f !== frames) {
        frames = f;
        const d = g.getImageData(0, 0, c.width, c.height).data;
        let s = 2166136261;
        for (let i = 0; i < d.length; i += 4) s = Math.imul(s ^ (d[i] | (d[i + 1] << 8) | (d[i + 2] << 16)), 16777619) >>> 0;
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
          if (s && Math.round((s.app_bytes || s.model_bytes) / 1048576) === mb) { R.q = Date.now(); clearInterval(id); }
        } catch {}
      }, 20);
      setTimeout(() => clearInterval(id), 60000);
    };
  })()`);
  await sleep(500);
  const text0 = await evalJs("window.__tw1.text.at(-1).text");
  console.log(`status text before the runs: ${text0}`);
  if (!/ \d+ MB/.test(text0)) throw new Error(`the status bar shows no "N MB": ${JSON.stringify(text0)}`);

  let k = 0;
  const runs = [];
  const one = async (linker, label) => {
    k++;
    const mb = 100 + k;
    const bg = [200 + (k % 40), 210, 220];
    const pxBefore = (await evalJs("window.__tw1.px.at(-1)?.hash")) ?? null;
    const nPx = await evalJs("window.__tw1.px.length"), nText = await evalJs("window.__tw1.text.length");
    const t0 = now();
    writeFileSync(LIB, ORIG
      .replace(TX_ANCHOR, `        let app_bytes = { let _ = big::in_use(); ${mb} * 1048576 }; // twin_loop edit\n`)
      .replace(PX_ANCHOR, `            screen_edges: self.moving_lean && !self.gpu_safe,\n            style: bimble_core::style::GraphicStyle { background: bimble_core::style::Rgb([${bg.join(", ")}]), ..self.opts.style.clone() }, // twin_loop edit\n            ..self.opts.clone()\n`));
    const buildMs = await build();
    const t1 = now();
    await evalJs(`window.__tw1.arm(${mb})`);
    const ts = now();
    await stopProc(twin);
    const stopMs = now() - ts;
    const spawnMs = await startTwin();
    const want = ` ${mb} MB`;
    const seen = await waitFor(`run ${label}: the page shows${want} and new pixels`, 30000, async () => {
      const r = JSON.parse(await evalJs(`JSON.stringify({ q: window.__tw1.q, px: window.__tw1.px.slice(${nPx}), text: window.__tw1.text.slice(${nText}) })`));
      const tx = r.text.find((e) => e.text.includes(want));
      const px = r.px.find((e) => e.hash !== pxBefore && e.t >= t1);
      return r.q && tx && px ? { q: r.q, text: tx.t, px: px.t } : null;
    });
    restoreSource();
    // Checks: the twin serves the new build.
    const st = await q("status");
    const c = await corner();
    if (mbOf(st) !== mb || c.some((v, i) => Math.abs(v - bg[i]) > 2))
      throw new Error(`run ${label}: twin status ${mbOf(st)} MB (want ${mb}), corner ${c} (want ${bg})`);
    const r = {
      linker, label, build: buildMs, stop: stopMs, spawn: spawnMs, restart: stopMs + spawnMs,
      value: seen.q - t1, text: seen.text - t1, pixels: seen.px - t1,
      total_value: seen.q - t0, total_text: seen.text - t0, total_pixels: seen.px - t0,
    };
    console.log(`${linker.padEnd(7)} run ${String(label).padEnd(6)} build ${String(r.build).padStart(5)}  stop ${String(r.stop).padStart(3)}  spawn->listening ${String(r.spawn).padStart(3)}` +
      `  page: value ${String(r.value).padStart(5)} text ${String(r.text).padStart(5)} pixels ${String(r.pixels).padStart(5)}` +
      `  total: value ${String(r.total_value).padStart(5)} text ${String(r.total_text).padStart(5)} pixels ${String(r.total_pixels).padStart(5)} ms`);
    return r;
  };

  const machine = `${process.arch}, ${cpus().length} cores, ${(totalmem() / 2 ** 30).toFixed(1)} GiB, ` +
    `${execFileSync("rustc", ["-V"]).toString().trim()}, node ${process.version}, ` +
    `${execFileSync("/usr/bin/chromium", ["--version"]).toString().trim()}, load ${loadavg().map((x) => x.toFixed(2)).join(" ")}`;
  for (const [i, linker] of LINKERS.entries()) {
    if (i > 0) {
      env = { ...process.env, [HOSTVAR]: linkerFlags(linker) };
      console.log(`untimed build (${linker}: new rustflags, every crate)...`);
      restoreSource();
      await build();
    }
    await one(linker, "warmup");
    for (let n = 1; n <= RUNS; n++) runs.push(await one(linker, n));
  }

  const keys = ["build", "stop", "spawn", "restart", "value", "text", "pixels", "total_value", "total_text", "total_pixels"];
  log(`=== TW1 twin dev loop in the lab (web-ui/e2e/twin_loop.mjs)  ${new Date().toISOString()}`);
  log(`machine: ${machine}`);
  log(`method: lab opened once at /?native=1 (no reload), headless Chromium (SwiftShader) 1280x800, twin on bimble demo; ` +
    `edit = status() app_bytes (status bar "M MB") + draw_options() background (every frame); ${RUNS} runs per linker after one untimed warm-up; ` +
    `bimble.ts pollChanges with the reconnect back-off (20 ms doubling to 1 s)`);
  log("phases (ms): build = edit -> cargo build -p bimble-cli exits; restart = stop (SIGTERM -> exit) + spawn (-> 'listening');");
  log("  value/text/pixels = end of build -> in-page query('status') / status bar text / first changed canvas frame; total_* = edit -> that");
  const summary = {};
  for (const linker of LINKERS) {
    const rs = runs.filter((r) => r.linker === linker);
    summary[linker] = {};
    for (const key of keys) summary[linker][key] = { median: median(rs.map((r) => r[key])), p95: p95(rs.map((r) => r[key])) };
    for (const r of rs) log(`${linker} run ${r.label}: ${keys.map((x) => `${x} ${r[x]}`).join(", ")}`);
    log(`median ${linker}: ${keys.map((x) => `${x} ${summary[linker][x].median}`).join(", ")} ms`);
    log(`p95    ${linker}: ${keys.map((x) => `${x} ${summary[linker][x].p95}`).join(", ")} ms`);
  }
  writeFileSync(RAW, JSON.stringify({ date: new Date().toISOString(), machine, runs_per_linker: RUNS, linkers: LINKERS, runs, summary }, null, 1));
  console.log(`raw runs: ${RAW}`);
  if (exceptions.length) throw new Error(`uncaught page exceptions: ${exceptions.join(" | ")}`);
  cdp.close();
}

let code = 1;
for (const sig of ["SIGINT", "SIGTERM"])
  process.on(sig, () => {
    restoreSource();
    for (const p of procs) try { process.kill(-p.pid, "SIGKILL"); } catch {}
    rmSync(tmp, { recursive: true, force: true });
    process.exit(130);
  });
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
}
process.exit(code);
