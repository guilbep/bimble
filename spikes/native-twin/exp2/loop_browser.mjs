// Experiment 2, T8: the whole dev loop as the developer sees it, edit -> the new value in the Vue lab.
//
// Usage (from the repository root, with rust stable on PATH):
//   MOLD_DIR=<mold libexec/mold dir> CMAKE_BIN=<cmake bin dir> node spikes/native-twin/exp2/loop_browser.mjs [runs]
//
// The edit is the one of loop.sh (T6): "spike": N in the "state" query (crates/bimble-viewer/src/calls.rs).
// End point for both paths: in the lab page, (await import('/src/bimble.ts')).query('state').spike === N
// (the module instance the lab itself uses; Vite serves source modules in dev).
//   wasm:   t0 edit; npm run -s wasm:dev (t1); Page.reload of /?url=/frame.bimble; wait for
//           data-bb-ready on the new document; poll query('state') every 20 ms (t2).
//   native: t0 edit; cargo build -p bimble-cli, mold (t1); kill and restart the headless twin;
//           no page reload: poll query('state') in the page opened once with ?native=1 (t2).
//           Sync XHR throws while the twin restarts: caught, retried.
// One Vite, one Chromium (two tabs) for the whole run. One untimed warm-up of each path, then
// the runs alternate wasm and native. Builds never overlap.
// calls.rs is restored, web-ui/public/frame.bimble removed, and the twin, Vite and Chromium killed on exit.
import { spawn, execFileSync } from "node:child_process";
import { appendFileSync, copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir, cpus, totalmem } from "node:os";
import { join, dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const need = (k, why) => {
  const v = process.env[k];
  if (!v) throw new Error(`set ${k}: ${why}`);
  return v;
};
const MOLD_DIR = need("MOLD_DIR", "the libexec/mold directory of mold, which holds an ld that is mold");
const CMAKE_BIN = need("CMAKE_BIN", "the directory of cmake");
const RUNS = Number(process.argv[2] ?? 5);

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "../../..");
const UI = join(ROOT, "web-ui");
const OUT = join(ROOT, "spikes/native-twin/exp2/results_browser.txt");
const CALLS = join(ROOT, "crates/bimble-viewer/src/calls.rs");
const WORK = join(ROOT, "target/exp2-loop-browser");
const FRAME = join(WORK, "frame.bimble");
const PUBLIC_FRAME = join(UI, "public/frame.bimble");
const BIN = join(ROOT, "target/debug/bimble");
const PORT = 7883;
const TOKEN = "loop";
const VITE = "http://localhost:5173";
const WASM_PAGE = `${VITE}/?url=/frame.bimble`;
const NATIVE_PAGE = `${VITE}/?native=1&port=${PORT}&token=${TOKEN}`;

const host = execFileSync("rustc", ["-vV"]).toString().match(/^host: (.*)$/m)[1];
const hostVar = `CARGO_TARGET_${host.toUpperCase().replace(/-/g, "_")}_RUSTFLAGS`;
const ENV = { ...process.env, PATH: `${CMAKE_BIN}:${process.env.PATH}`, [hostVar]: `-C link-arg=-B${MOLD_DIR}` };

mkdirSync(WORK, { recursive: true });
const ORIG = readFileSync(CALLS, "utf8");
const tmp = mkdtempSync(join(tmpdir(), "bimble-loop-browser-"));
const procs = [];
let twin = null;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const now = () => performance.now() / 1000;

function edit(n) {
  const anchor = '            "version": env!("CARGO_PKG_VERSION"),\n';
  if (ORIG.split(anchor).length !== 2) throw new Error("anchor not found once in calls.rs");
  const line = `            "spike": "bimblespike${n}".trim_start_matches("bimblespike").parse::<u64>().ok(),\n`;
  writeFileSync(CALLS, ORIG.replace(anchor, anchor + line));
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

async function kill(p) {
  if (!p || p.exitCode !== null) return;
  const gone = new Promise((r) => p.once("exit", r));
  try { process.kill(-p.pid, "SIGTERM"); } catch {}
  await Promise.race([gone, sleep(2000)]);
  if (p.exitCode === null) try { process.kill(-p.pid, "SIGKILL"); } catch {}
}

/** Run a build to completion; throws with its output on a non-zero exit. */
function build(cmd, args, cwd) {
  return new Promise((res, rej) => {
    const p = spawn(cmd, args, { cwd, env: ENV, stdio: ["ignore", "pipe", "pipe"] });
    let out = "";
    p.stdout.on("data", (d) => (out += d));
    p.stderr.on("data", (d) => (out += d));
    p.on("exit", (c) => (c === 0 ? res() : rej(new Error(`${cmd} ${args.join(" ")} exit ${c}\n${out.slice(-3000)}`))));
  });
}

function startTwin() {
  const env = { ...ENV, BIMBLE_SERVE: String(PORT), BIMBLE_TOKEN: TOKEN, BIMBLE_HEADLESS: "1" };
  delete env.WAYLAND_DISPLAY;
  delete env.DISPLAY;
  twin = start("twin", BIN, ["view", FRAME], { env });
}

async function waitFor(what, ms, fn, every = 200) {
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
  ws.onmessage = (ev) => {
    const msg = JSON.parse(ev.data);
    if (msg.id !== undefined && pending.has(msg.id)) {
      const { resolve, reject } = pending.get(msg.id);
      pending.delete(msg.id);
      if (msg.error) reject(new Error(`${msg.error.message} ${msg.error.data ?? ""}`));
      else resolve(msg.result);
    }
  };
  const send = (method, params = {}, sessionId) =>
    new Promise((resolve, reject) => {
      const m = ++id;
      pending.set(m, { resolve, reject });
      ws.send(JSON.stringify({ id: m, method, params, ...(sessionId ? { sessionId } : {}) }));
    });
  return { send, close: () => ws.close() };
}

async function tab(cdp, url) {
  // One window per tab: a background tab gets no animation frames, and the viewer draws on them.
  const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank", newWindow: true });
  const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
  const S = (m, p) => cdp.send(m, p, sessionId);
  await S("Page.enable");
  await S("Runtime.enable");
  await S("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  await S("Page.navigate", { url });
  const ev = async (expr) => {
    const r = await S("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
    return r.result.value;
  };
  const activate = () => cdp.send("Target.activateTarget", { targetId });
  return { S, ev, targetId, activate };
}

// In-page poll: the lab's own query('state') until spike === n (every 20 ms, 120 s budget).
// A throw (twin down: sync XHR fails; wasm not started) is caught and retried.
const pollSpike = (n) => `(async () => {
  const m = await import('/src/bimble.ts');
  const until = performance.now() + 120000;
  let last = '';
  while (performance.now() < until) {
    try { const s = m.query('state'); if (s.spike === ${n}) return 'ok'; last = JSON.stringify(s.spike); }
    catch (e) { last = String(e).slice(0, 200); }
    await new Promise((r) => setTimeout(r, 20));
  }
  return 'timeout: ' + last;
})()`;

const rnd = () => Math.floor(Math.random() * 1e9);

async function wasmRun(page) {
  await page.activate();
  const n = rnd();
  const t0 = now();
  edit(n);
  await build("npm", ["run", "-s", "wasm:dev"], UI);
  const t1 = now();
  await page.ev("window.__loopOld = 1");
  await page.S("Page.reload", {});
  // The new document: no __loopOld, and data-bb-ready set by deeplink.ts.
  await waitFor("data-bb-ready after reload", 120000,
    () => page.ev("!window.__loopOld && document.documentElement.dataset.bbReady === '1'"), 20);
  const tReady = now();
  const r = await page.ev(pollSpike(n));
  if (r !== "ok") throw new Error(`wasm: spike ${n} not seen (${r})`);
  const t2 = now();
  return { build: t1 - t0, ready: tReady - t1, value: t2 - t1, total: t2 - t0 };
}

async function nativeRun(page) {
  await page.activate();
  const n = rnd();
  const t0 = now();
  edit(n);
  await build("cargo", ["build", "-p", "bimble-cli"], ROOT);
  const t1 = now();
  await kill(twin);
  startTwin();
  const r = await page.ev(pollSpike(n));
  if (r !== "ok") throw new Error(`native: spike ${n} not seen (${r})\n${twin.out}`);
  const t2 = now();
  return { build: t1 - t0, value: t2 - t1, total: t2 - t0 };
}

const f = (x) => x.toFixed(3);
const median = (a) => { const s = [...a].sort((x, y) => x - y); const m = s.length >> 1; return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2; };
const log = (line) => { console.log(line); appendFileSync(OUT, line + "\n"); };

async function main() {
  // The model: bimble demo, served by Vite for the wasm page, opened by the twin for the native page.
  await build("cargo", ["build", "-p", "bimble-cli"], ROOT);
  if (!existsSync(FRAME)) execFileSync(BIN, ["demo", FRAME], { stdio: "ignore" });
  copyFileSync(FRAME, PUBLIC_FRAME);
  startTwin();
  await waitFor("twin 'listening'", 30000, () => twin.out.includes("listening") || (twin.exitCode !== null && Promise.reject(new Error(twin.out))));

  const vite = start("vite", "npx", ["vite", "--port", "5173", "--strictPort"], { cwd: UI });
  await waitFor("Vite on 5173", 30000, async () => {
    if (vite.exitCode !== null) throw new Error(vite.out);
    return (await fetch(`${VITE}/`)).ok;
  });
  const chrome = start("chromium", "/usr/bin/chromium", [
    "--headless=new", "--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--remote-debugging-port=0",
    `--user-data-dir=${join(tmp, "chromium")}`, "--no-first-run", "--no-default-browser-check", "about:blank",
  ]);
  const wsUrl = await waitFor("DevTools line", 15000, () => chrome.out.match(/DevTools listening on (ws:\/\/\S+)/)?.[1]);
  const cdp = await cdpConnect(wsUrl);
  const wasmPage = await tab(cdp, WASM_PAGE);
  const nativePage = await tab(cdp, NATIVE_PAGE);
  for (const [name, p] of [["wasm", wasmPage], ["native", nativePage]])
    await p.activate(), await waitFor(`${name} page data-bb-ready`, 120000, () => p.ev("document.documentElement.dataset.bbReady === '1'"));
  // The in-page read works in both modes (and the page never was reloaded for native).
  for (const [name, p] of [["wasm", wasmPage], ["native", nativePage]]) {
    const s = await p.ev("(async () => JSON.stringify((await import('/src/bimble.ts')).query('state')))()");
    console.log(`${name} page query('state'): ${s.slice(0, 200)}`);
  }

  const cpu = cpus()[0]?.model ?? "?";
  log(`=== exp2/T8 dev loop in the browser  ${new Date().toISOString()}`);
  log(`machine: ${process.arch}, ${cpus().length} cores (${cpu}), ${(totalmem() / 2 ** 30).toFixed(1)} GiB, ${execFileSync("rustc", ["-V"]).toString().trim()}, node ${process.version}, ${execFileSync("/usr/bin/chromium", ["--version"]).toString().trim()}`);
  log(`native: ${hostVar}=-C link-arg=-B<mold>; headless Chromium (swiftshader), one Vite, ${RUNS} runs each after one untimed warm-up, alternating`);
  log("includes:");
  log("  wasm build_s    = edit of calls.rs + npm run -s wasm:dev (cargo wasm build, wasm-bindgen, copy to public/bimble/)");
  log("  wasm ready_s    = Page.reload -> data-bb-ready on the new document (page load, wasm fetch + instantiate, model fetch + open, first refresh, deep link)");
  log("  wasm value_s    = Page.reload -> in-page query('state').spike === N (ready_s + the poll, 20 ms steps)");
  log("  wasm total_s    = edit -> value (build_s + value_s)");
  log("  native build_s  = edit of calls.rs + cargo build -p bimble-cli (mold)");
  log("  native value_s  = kill old twin + start headless twin (open frame.bimble) + in-page query('state').spike === N over sync XHR, no page reload (20 ms steps, failed XHR retried)");
  log("  native total_s  = edit -> value (build_s + value_s)");
  log("  all times: wall clock from Node (performance.now); CDP round trips (~ms) included");

  console.log("warm-up (untimed)...");
  await wasmRun(wasmPage);
  await nativeRun(nativePage);

  const W = [], N = [];
  for (let i = 1; i <= RUNS; i++) {
    const w = await wasmRun(wasmPage); W.push(w);
    log(`wasm   run ${i}: build_s ${f(w.build)}  ready_s ${f(w.ready)}  value_s ${f(w.value)}  total_s ${f(w.total)}`);
    const n = await nativeRun(nativePage); N.push(n);
    log(`native run ${i}: build_s ${f(n.build)}  value_s ${f(n.value)}  total_s ${f(n.total)}`);
  }
  const m = (a, k) => median(a.map((r) => r[k]));
  log(`median wasm:   build ${f(m(W, "build"))} s, reload->ready ${f(m(W, "ready"))} s, reload->value ${f(m(W, "value"))} s, total ${f(m(W, "total"))} s`);
  log(`median native: build ${f(m(N, "build"))} s, restart->value ${f(m(N, "value"))} s, total ${f(m(N, "total"))} s`);
  log(`ratio wasm/native total (medians): ${(m(W, "total") / m(N, "total")).toFixed(2)}`);
  cdp.close();
  return 0;
}

for (const sig of ["SIGINT", "SIGTERM"])
  process.on(sig, () => {
    writeFileSync(CALLS, ORIG);
    rmSync(PUBLIC_FRAME, { force: true });
    for (const p of procs) try { process.kill(-p.pid, "SIGKILL"); } catch {}
    process.exit(130);
  });

let code = 1;
try {
  code = await main();
} catch (e) {
  console.log(`FAIL: ${e.stack ?? e}`);
  for (const p of procs) console.log(`--- ${p.label} output (tail) ---\n${p.out.slice(-2000)}`);
} finally {
  writeFileSync(CALLS, ORIG);
  rmSync(PUBLIC_FRAME, { force: true });
  for (const p of procs) await kill(p);
  rmSync(tmp, { recursive: true, force: true });
}
process.exit(code);
