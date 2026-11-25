// Viewport spike T4: is the streamed 3D view (native headless twin -> lab canvas) good enough to work in?
// node e2e/native-viewport.mjs   (needs /usr/bin/chromium, node_modules; builds target/debug/bimble)
// Per model (BIMBLE_FILE, else the small `bimble demo` and a 30x30x7 demo): starts the headless twin,
// Vite on 5173 and headless Chromium over raw CDP at 1920x1080 DPR 1, opens /?native=1, then:
//   latency: 50 drags (press at the canvas centre, one move +40 px, wait for a changed sample, release,
//            100 ms idle); p50/p95/max of window.__bbViewport latency_ms (pointer event -> after
//            putImageData; no compositor present), mean render/read/fetch ms, time-outs;
//   fps:     one press, 240 moves 8 ms apart (+2 px each), release; changed frames / elapsed s.
// Verdict: WORKABLE p95 <= 50 ms and fps >= 20; KEEP-WASM p95 > 100 ms; else MARGINAL.
// The verdict is a measurement: exit 0 means it measured. Screenshot /tmp/viewport-lab-<model>.png.
// Build: `cargo build -q -p bimble-cli` (dev profile); with mold when `mold` is on PATH. Kills all it starts.
import { spawn, execFileSync } from "node:child_process";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir, cpus, totalmem } from "node:os";
import { join, dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const UI = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const ROOT = resolve(UI, "..");
const BIN = join(ROOT, "target/debug/bimble");
const PORT = 7883;
const TOKEN = "viewtest";
const TWIN = `http://127.0.0.1:${PORT}`;
const PAGE = `http://localhost:5173/?native=1&port=${PORT}&token=${TOKEN}`;
const tmp = mkdtempSync(join(tmpdir(), "bimble-native-viewport-"));
const procs = [];
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function start(name, cmd, args, opts) {
  const p = spawn(cmd, args, { stdio: ["ignore", "pipe", "pipe"], detached: true, ...opts });
  p.out = "";
  p.stdout.on("data", (d) => (p.out += d));
  p.stderr.on("data", (d) => (p.out += d));
  p.label = name;
  procs.push(p);
  return p;
}

async function stop(p) {
  if (p.exitCode !== null || p.signalCode !== null) return;
  try { process.kill(-p.pid, "SIGTERM"); } catch {}
  for (let i = 0; i < 20 && p.exitCode === null && p.signalCode === null; i++) await sleep(50);
  if (p.exitCode === null && p.signalCode === null) try { process.kill(-p.pid, "SIGKILL"); } catch {}
  for (let i = 0; i < 20 && p.exitCode === null && p.signalCode === null; i++) await sleep(50);
}

async function cleanup() {
  for (const p of procs) await stop(p);
  rmSync(tmp, { recursive: true, force: true });
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
  await new Promise((res, rej) => {
    ws.onopen = res;
    ws.onerror = () => rej(new Error("CDP connect failed"));
  });
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

const pct = (xs, p) => {
  if (!xs.length) return NaN;
  const s = [...xs].sort((a, b) => a - b);
  return s[Math.min(s.length - 1, Math.ceil((p / 100) * s.length) - 1)];
};
const mean = (xs) => (xs.length ? xs.reduce((a, b) => a + b, 0) / xs.length : NaN);
const counts = {};
const demo = (name, args) => {
  const out = execFileSync(BIN, ["demo", ...args], { encoding: "utf8", stdio: ["ignore", "pipe", "inherit"] });
  process.stdout.write(out);
  counts[name] = Number(out.match(/(\d+) elements/)?.[1] ?? NaN);
};
const f1 = (x) => (Number.isFinite(x) ? x.toFixed(1) : "n/a");

async function measure(name, file, vite) {
  const env = { ...process.env, BIMBLE_SERVE: String(PORT), BIMBLE_TOKEN: TOKEN, BIMBLE_HEADLESS: "1" };
  delete env.WAYLAND_DISPLAY;
  delete env.DISPLAY;
  const tw0 = Date.now();
  const twin = start(`twin-${name}`, BIN, ["view", file], { env });
  await waitFor("twin 'listening'", 180000, () => twin.out.includes("listening") || (twin.exitCode !== null && Promise.reject(new Error(twin.out))));
  // The element count comes from `bimble demo`; the twin's query is printed raw when it is not a list.
  const qr = await (await fetch(`${TWIN}/query/elements?t=${TOKEN}`, { method: "POST", body: "{}" })).text();
  let count = counts[name] ?? NaN;
  try { const els = JSON.parse(qr); if (Array.isArray(els)) count = els.length; } catch { console.log(`[${name}] query elements answered: ${qr.slice(0, 120)}`); }
  console.log(`[${name}] twin listening after ${Date.now() - tw0} ms; elements ${count}`);

  const chrome = start(`chromium-${name}`, "/usr/bin/chromium", [
    "--headless=new", "--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--remote-debugging-port=0",
    `--user-data-dir=${join(tmp, `chromium-${name}`)}`, "--no-first-run", "--no-default-browser-check", "about:blank",
  ]);
  const wsUrl = await waitFor("DevTools line", 15000, () => chrome.out.match(/DevTools listening on (ws:\/\/\S+)/)?.[1]);
  const cdp = await cdpConnect(wsUrl);
  try {
    const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank" });
    const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
    const S = (m, p) => cdp.send(m, p, sessionId);
    await S("Runtime.enable");
    await S("Page.enable");
    await S("Emulation.setDeviceMetricsOverride", { width: 1920, height: 1080, deviceScaleFactor: 1, mobile: false });
    await S("Page.navigate", { url: PAGE });
    const evalJs = async (expr) => (await S("Runtime.evaluate", { expression: expr, returnByValue: true })).result.value;
    await waitFor("data-bb-ready", 60000, () => evalJs("document.documentElement.dataset.bbReady === '1'"));
    await waitFor("__bbViewport.frames >= 1", 60000, () => evalJs("(window.__bbViewport?.frames ?? 0) >= 1"));
    const c = await evalJs(`(() => { const c = document.getElementById('bimble'); const r = c.getBoundingClientRect();
      return { w: c.width, h: c.height, x: r.left + r.width / 2, y: r.top + r.height / 2 }; })()`);
    console.log(`[${name}] canvas ${c.w}x${c.h} px, centre (${Math.round(c.x)}, ${Math.round(c.y)})`);

    const mouse = (type, x, y) => S("Input.dispatchMouseEvent", { type, x, y, button: "left", buttons: type === "mouseReleased" ? 0 : 1, clickCount: type === "mouseMoved" ? 0 : 1 });
    const frames = () => evalJs("window.__bbViewport.frames");
    // Samples after frame number n (the array keeps the last 1000).
    const since = (n) => evalJs(`(() => { const v = window.__bbViewport; const k = v.frames - ${n}; return k > 0 ? v.samples.slice(-k) : []; })()`);
    const drag = async () => {
      const n = await frames();
      await mouse("mousePressed", c.x, c.y);
      await mouse("mouseMoved", c.x + 40, c.y);
      let hit = null;
      try {
        hit = await waitFor("changed sample", 2000, async () => (await since(n)).find((s) => s.changed === true && s.latency_ms !== null), 5);
      } catch {}
      await mouse("mouseReleased", c.x + 40, c.y);
      await sleep(100);
      return hit;
    };
    for (let i = 0; i < 5; i++) await drag();
    const hits = [];
    let timeouts = 0;
    for (let i = 0; i < 50; i++) {
      const h = await drag();
      if (h) hits.push(h); else timeouts++;
    }
    const lat = hits.map((s) => s.latency_ms);
    const r = {
      name, count, canvas: `${c.w}x${c.h}`, n: hits.length, timeouts,
      p50: pct(lat, 50), p95: pct(lat, 95), max: Math.max(...lat),
      render: mean(hits.map((s) => s.render_ms).filter((x) => x !== null)),
      read: mean(hits.map((s) => s.read_ms).filter((x) => x !== null)),
      fetch: mean(hits.map((s) => s.fetch_ms)),
    };
    const shotPath = `/tmp/viewport-lab-${name}.png`;
    const shot = await S("Page.captureScreenshot", { format: "png" });
    writeFileSync(shotPath, Buffer.from(shot.data, "base64"));
    r.shot = shotPath;
    console.log(`[${name}] latency (${r.n} drags, ${timeouts} timed out): p50 ${f1(r.p50)} ms, p95 ${f1(r.p95)} ms, max ${f1(r.max)} ms; mean render ${f1(r.render)} ms, read ${f1(r.read)} ms, fetch ${f1(r.fetch)} ms`);

    await sleep(300);
    const n0 = await frames();
    await mouse("mousePressed", c.x, c.y);
    const t0 = performance.now();
    for (let i = 1; i <= 240; i++) {
      await mouse("mouseMoved", c.x - 240 + 2 * i, c.y);
      await sleep(8);
    }
    await mouse("mouseReleased", c.x + 240, c.y);
    const secs = (performance.now() - t0) / 1000;
    const sp = await since(n0);
    const changed = sp.filter((s) => s.changed).length;
    r.fps = changed / secs;
    r.fpsFrames = changed;
    r.fpsAll = sp.length;
    r.fpsSecs = secs;
    const lat2 = sp.filter((s) => s.changed && s.latency_ms !== null).map((s) => s.latency_ms);
    r.fpsP95 = pct(lat2, 95);
    console.log(`[${name}] fps: ${changed} changed frames (${sp.length} frames) in ${secs.toFixed(2)} s = ${f1(r.fps)} fps; latency during the drag p50 ${f1(pct(lat2, 50))} ms, p95 ${f1(r.fpsP95)} ms`);

    r.verdict = r.p95 > 100 ? "KEEP-WASM" : r.p95 <= 50 && r.fps >= 20 ? "WORKABLE" : "MARGINAL";
    console.log(`[${name}] VERDICT ${r.verdict} (p95 ${f1(r.p95)} ms, ${f1(r.fps)} fps; WORKABLE p95<=50 and >=20 fps, KEEP-WASM p95>100)`);
    return r;
  } finally {
    cdp.close();
    await stop(chrome);
    await stop(twin);
    void vite;
  }
}

async function main() {
  const t0 = Date.now();
  const rustflags = process.env.RUSTFLAGS ?? (() => { try { execFileSync("mold", ["--version"], { stdio: "ignore" }); return "-C link-arg=-fuse-ld=mold"; } catch { return undefined; } })();
  execFileSync("cargo", ["build", "-q", "-p", "bimble-cli"], { cwd: ROOT, stdio: "inherit", env: { ...process.env, ...(rustflags ? { RUSTFLAGS: rustflags } : {}) } });
  console.log(`build: dev profile (target/debug/bimble), RUSTFLAGS=${JSON.stringify(rustflags ?? "")}, ${Date.now() - t0} ms`);
  console.log(`machine: ${cpus()[0]?.model ?? "?"} x${cpus().length}, ${(totalmem() / 2 ** 30).toFixed(1)} GiB, node ${process.version}`);

  const models = [];
  if (process.env.BIMBLE_FILE) models.push(["file", process.env.BIMBLE_FILE]);
  else {
    const small = join(tmp, "small.bimble");
    demo("small", [small]);
    models.push(["small", small]);
    const large = join(tmp, "large.bimble");
    demo("large", ["--bays-x", "30", "--bays-y", "30", "--storeys", "7", large]);
    models.push(["large", large]);
  }

  const vite = start("vite", "npx", ["vite", "--port", "5173", "--strictPort"], { cwd: UI });
  await waitFor("Vite on 5173", 30000, async () => {
    if (vite.exitCode !== null) throw new Error(vite.out);
    return (await fetch("http://localhost:5173/")).ok;
  });
  const results = [];
  for (const [name, file] of models) results.push(await measure(name, file, vite));
  console.log("summary:");
  for (const r of results) console.log(`  ${r.name}: ${r.count} elements, canvas ${r.canvas}, p50 ${f1(r.p50)} / p95 ${f1(r.p95)} / max ${f1(r.max)} ms, ${f1(r.fps)} fps, ${r.timeouts} time-outs -> ${r.verdict}; ${r.shot}`);
  return 0;
}

let code = 1;
try {
  code = await main();
} catch (e) {
  console.log(`FAIL: ${e.stack ?? e}`);
  for (const p of procs) console.log(`--- ${p.label} output ---\n${p.out.slice(-4000)}`);
} finally {
  await cleanup();
}
process.exit(code);
