// A lab page against the native host, for the lab tests of ticket GPU1 (lab_perf, lab_webgpu):
//   import { lab } from "./lib/lab.mjs";
//   const L = await lab({ name: "lab_perf", model, query: "perf=1", chromiumFlags: [...] });
//   await L.evalJs("1 + 1"); ... await L.close();
// Starts out/tcc/bimble-ccore (or $HOST_BIN) on the model (a scratch copy), Vite from web-ui/ and one
// headless Chromium over raw CDP (1280 x 800), opens /?native=1&gl=1&port=…&token=…&<query>, and
// waits for the first frame drawn with the mesh. `reopen(query, flags)` closes Chromium and starts
// another with other flags (the host and Vite stay): one browser at a time. Kills all it starts.
// Lives in test/lib/, not test/: ci/run.sh runs every test/*.mjs as a suite.
import { spawn } from "node:child_process";
import { copyFileSync, existsSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { basename, dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { CHROMIUM, port, vitePort } from "../ports.mjs";
import { hostEnv } from "./host_env.mjs";

export const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const UI = resolve(CCORE, "../web-ui");
export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function waitFor(what, ms, fn) {
  const until = Date.now() + ms;
  let last;
  while (Date.now() < until) {
    try {
      const v = await fn();
      if (v) return v;
    } catch (e) {
      last = e;
    }
    await sleep(100);
  }
  throw new Error(`timeout waiting for ${what}${last ? `: ${last.message ?? last}` : ""}`);
}
export { waitFor };

async function cdpConnect(wsUrl) {
  const ws = new WebSocket(wsUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = () => rej(new Error("CDP connect failed")); });
  let id = 0;
  const pending = new Map();
  const listeners = [];
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

/** Starts the host, Vite and Chromium; `model`: a .bimble or .json path (copied to a scratch folder). */
export async function lab({ name, model, query = "", chromiumFlags = [], width = 1280, height = 800 }) {
  if (!existsSync(join(UI, "node_modules"))) throw new Error(`no ${UI}/node_modules: run npm ci in web-ui/`);
  const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
  if (!existsSync(BIN)) throw new Error(`no ${BIN}: run ccore/build.sh tcc`);
  const PORT = port(name), VITE = vitePort(name), TOKEN = `${name}-token`;
  const tmp = mkdtempSync(join(tmpdir(), `ccore-${name}-`));
  const procs = [];
  const start = (label, cmd, args, opts) => {
    const p = spawn(cmd, args, { stdio: ["ignore", "pipe", "pipe"], detached: true, ...opts });
    p.out = "";
    p.stdout.on("data", (d) => (p.out += d));
    p.stderr.on("data", (d) => (p.out += d));
    p.label = label;
    procs.push(p);
    return p;
  };
  const kill = async (ps) => {
    for (const sig of ["SIGTERM", "SIGKILL"]) {
      for (const p of ps) if (p.exitCode === null && p.signalCode === null) try { process.kill(-p.pid, sig); } catch {}
      await sleep(300);
    }
  };
  const scratch = join(tmp, basename(model));
  copyFileSync(model, scratch);
  const host = start("host", BIN, [String(PORT)], {
    env: hostEnv({ HOME: tmp, BIMBLE_TOKEN: TOKEN, CCORE_MODEL: scratch, BIMBLE_ORIGINS: `http://localhost:${VITE},http://127.0.0.1:${VITE}` }),
  });
  await waitFor("host 'listening'", 10000, () => host.out.includes("listening") || (host.exitCode !== null && Promise.reject(new Error(host.out))));
  const vite = start("vite", "npx", ["vite", "--port", String(VITE), "--strictPort"], { cwd: UI });
  await waitFor(`Vite on ${VITE}`, 60000, async () => {
    if (vite.exitCode !== null) throw new Error(vite.out);
    return (await fetch(`http://localhost:${VITE}/`)).ok;
  });
  const L = { tmp, host, vite, errors: [], console: [], chrome: null, cdp: null, S: null };
  L.url = (q) => `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}${q ? `&${q}` : ""}`;
  L.open = async (q, flags) => {
    if (L.chrome) { L.cdp?.close(); await kill([L.chrome]); }
    L.errors = [], L.console = [];
    const chrome = start("chromium", CHROMIUM, ["--headless=new", "--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--remote-debugging-port=0",
      `--user-data-dir=${join(tmp, `chromium-${procs.length}`)}`, "--no-first-run", "--no-default-browser-check", ...flags, "about:blank"], { env: { ...process.env, HOME: tmp } });
    L.chrome = chrome;
    const wsUrl = await waitFor("DevTools line", 60000, () => chrome.out.match(/DevTools listening on (ws:\/\/\S+)/)?.[1]);
    const cdp = await cdpConnect(wsUrl);
    L.cdp = cdp;
    const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank" });
    const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
    const S = (m, p) => cdp.send(m, p, sessionId);
    L.S = S;
    cdp.on((msg) => {
      if (msg.sessionId !== sessionId) return;
      if (msg.method === "Runtime.exceptionThrown") { const d = msg.params.exceptionDetails; L.errors.push(d.exception?.description ?? d.text); }
      else if (msg.method === "Runtime.consoleAPICalled") L.console.push(`${msg.params.type}: ${msg.params.args.map((a) => a.value ?? a.description ?? a.type).join(" ")}`);
    });
    await S("Runtime.enable");
    await S("Page.enable");
    await S("Emulation.setDeviceMetricsOverride", { width, height, deviceScaleFactor: 1, mobile: false });
    await S("Page.navigate", { url: L.url(q) });
    await waitFor("the first drawn frame", 60000, () => L.evalJs("!!(window.__bbGl && window.__bbGl.firstFrameMs && window.__bbGl.meshRev !== null)"));
  };
  L.evalJs = async (expr) => {
    const r = await L.S("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
    return r.result.value;
  };
  /** A left drag on the canvas from its centre by (dx, dy) px, in `steps` moves (an orbit). */
  L.drag = async (dx, dy, steps = 20) => {
    const r = await L.evalJs("(() => { const b = document.getElementById('bimble').getBoundingClientRect(); return [b.left + b.width / 2, b.top + b.height / 2]; })()");
    const [x, y] = r;
    const M = (type, px, py, buttons) => L.S("Input.dispatchMouseEvent", { type, x: px, y: py, button: "left", buttons, clickCount: type === "mouseMoved" ? 0 : 1 });
    await M("mousePressed", x, y, 1);
    for (let i = 1; i <= steps; i++) { await M("mouseMoved", x + (dx * i) / steps, y + (dy * i) / steps, 1); await sleep(16); }
    await M("mouseReleased", x + dx, y + dy, 0);
  };
  /** The viewport's pixels as shown (a screenshot of the canvas's box): { w, h, rgba } (PNG decoded). */
  L.close = async () => { L.cdp?.close(); await kill(procs); rmSync(tmp, { recursive: true, force: true }); };
  await L.open(query, chromiumFlags);
  return L;
}
