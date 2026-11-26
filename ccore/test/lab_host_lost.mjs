// The native host lost under the page (ticket FIXUI 2): node ccore/test/lab_host_lost.mjs
// The test owns the host process: it starts out/tcc/bimble-ccore (or $HOST_BIN) with --session on a
// copy of golden/frame.json (HOME and the config folder in the test's own folder), Vite from web-ui/
// and headless Chromium over raw CDP, opens the native lab and:
//   1 an agent renames an element: the page says unsaved;
//   2 the host is killed (SIGKILL): within 6 s the page blocks with "Connection to the local Bimble
//     host lost", the file name and "with unsaved changes", a Retry button; the shell state is
//     `disconnected`; Retry while it is down keeps the notice;
//   3 the host is started again with --session (it replays the session: same file, rev, unsaved):
//     the notice goes, the page is back on the model's view, same rev, still unsaved;
//   4 killed again, then started WITHOUT the session (the saved file, rev 1): the page says
//     "Session lost" and that the unsaved changes of frame.json are lost; it does not jump to Home
//     (2 s later still the notice); "Show frame.json" leads to the model's view of the fresh host.
// Fails on the code before FIXUI: no notice at all (the page stayed as if live), and after the
// restart without the session the shell went to Home silently.
// Prints "N passed, M failed"; exits non-zero on a failure. Kills all it starts.
import { spawn, spawnSync, execFileSync } from "node:child_process";
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { CHROMIUM, port, vitePort } from "./ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = resolve(CCORE, "../web-ui");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("lab_host_lost");
const VITE = vitePort("lab_host_lost");
const TOKEN = "labhostlost-token";
const NATIVE = `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}`;
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-host-lost-"));
const procs = [];
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? `: ${detail}` : ""}`);
};

function start(name, cmd, args, opts) {
  const p = spawn(cmd, args, { stdio: ["ignore", "pipe", "pipe"], detached: true, ...opts });
  p.out = "";
  p.stdout.on("data", (d) => (p.out += d));
  p.stderr.on("data", (d) => (p.out += d));
  p.label = name;
  procs.push(p);
  return p;
}
const alive = (p) => p.exitCode === null && p.signalCode === null;
function kill(p, sig) {
  if (!alive(p)) return;
  if (process.platform === "win32") spawnSync("taskkill", ["/pid", String(p.pid), "/T", "/F"], { stdio: "ignore" });
  else try { process.kill(-p.pid, sig); } catch {}
}
async function cleanup() {
  for (const sig of ["SIGTERM", "SIGKILL"]) {
    for (const p of procs) kill(p, sig);
    await sleep(300);
  }
  rmSync(tmp, { recursive: true, force: true });
}
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
async function host(op, name, body = "") {
  const r = await fetch(`http://127.0.0.1:${PORT}/${op}/${name}?t=${TOKEN}`, { method: "POST", headers: { "Content-Type": "text/plain" }, body });
  return { status: r.status, text: await r.text() };
}

// The host, with or without the session log (HOME and the config in the test's folder).
async function startHost(session) {
  const h = start("host", BIN, [...(session ? ["--session"] : []), String(PORT), "frame.json"], {
    cwd: tmp, env: { ...process.env, HOME: tmp, BIMBLE_CONFIG_DIR: join(tmp, "config"), BIMBLE_TOKEN: TOKEN, BIMBLE_ORIGINS: `http://localhost:${VITE},http://127.0.0.1:${VITE}` },
  });
  await waitFor("host 'listening'", 8000, () => h.out.includes("listening") || (!alive(h) && Promise.reject(new Error(h.out))));
  return h;
}

const SHOWS = `(() => {
  const q = (s) => document.querySelector(s);
  const shown = (el) => !!el && el.getClientRects().length > 0;
  return JSON.stringify({ shell: q('.bb-app')?.dataset.bbShell ?? null,
    lost: shown(q('[data-bb=shell-disconnected]')) ? q('[data-bb=shell-disconnected]').innerText.replace(/\\s+/g, ' ') : null,
    retry: shown(q('[data-bb=shell-disconnected-retry]')),
    gone: shown(q('[data-bb=shell-session-lost]')) ? q('[data-bb=shell-session-lost]').innerText.replace(/\\s+/g, ' ') : null,
    home: !!q('[data-bb=home]'), viewport: shown(q('[data-bb=viewport]')) });
})()`;

async function main() {
  if (!existsSync(join(UI, "node_modules"))) throw new Error(`no ${UI}/node_modules: run npm ci in web-ui/`);
  mkdirSync(join(tmp, "config"), { recursive: true });
  copyFileSync(join(CCORE, "golden/frame.json"), join(tmp, "frame.json"));
  let h = await startHost(true);
  const vite = start("vite", "npx", ["vite", "--port", String(VITE), "--strictPort"], { cwd: UI });
  await waitFor(`Vite on ${VITE}`, 30000, async () => {
    if (vite.exitCode !== null) throw new Error(vite.out);
    return (await fetch(`http://localhost:${VITE}/`)).ok;
  });
  const chrome = start("chromium", CHROMIUM, [
    "--headless=new", "--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--remote-debugging-port=0",
    `--user-data-dir=${join(tmp, "chromium")}`, "--no-first-run", "--no-default-browser-check", "about:blank",
  ]);
  const wsUrl = await waitFor("DevTools line", 60000, () => chrome.out.match(/DevTools listening on (ws:\/\/\S+)/)?.[1]);
  const cdp = await cdpConnect(wsUrl);
  const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank" });
  const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
  const S = (m, p) => cdp.send(m, p, sessionId);
  const exceptions = [];
  cdp.on((msg) => {
    if (msg.sessionId === sessionId && msg.method === "Runtime.exceptionThrown") exceptions.push(msg.params.exceptionDetails.exception?.description ?? msg.params.exceptionDetails.text);
  });
  await S("Runtime.enable");
  await S("Page.enable");
  await S("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  const evalJs = async (expr) => (await S("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true })).result.value;
  const bb = (expr) => evalJs(`(async () => { const b = await import('/src/bimble.ts'); return ${expr}; })()`);
  const shows = async () => JSON.parse(await evalJs(SHOWS));
  const until = async (pred, ms) => { let v = await shows(); const end = Date.now() + ms; while (!pred(v) && Date.now() < end) { await sleep(100); v = await shows(); } return v; };
  const click = async (sel) => {
    const r = JSON.parse(await evalJs(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) return 'null';
      const b = e.getBoundingClientRect(); return b.width && b.height ? JSON.stringify({ x: b.x + b.width / 2, y: b.y + b.height / 2 }) : 'null'; })()`));
    if (!r) return false;
    for (const type of ["mousePressed", "mouseReleased"]) await S("Input.dispatchMouseEvent", { type, x: r.x, y: r.y, button: "left", buttons: type === "mousePressed" ? 1 : 0, clickCount: 1 });
    await sleep(250);
    return true;
  };

  await S("Page.navigate", { url: NATIVE });
  await waitFor("the model in the page", 30000, () => bb("b.state.value.file === 'frame.json' && b.actions.value.length > 0"));
  let v = await until((x) => x.shell === "model.view", 5000);
  check("boot: the model's view", v.shell === "model.view" && v.viewport, JSON.stringify(v));

  // 1: an edit (as an agent makes it): unsaved.
  const el = JSON.parse((await host("query", "elements", JSON.stringify({ limit: 1 }))).text)[0];
  const ren = await host("verb", "rename", JSON.stringify({ id: el.id, name: "lost-and-found" }));
  await waitFor("unsaved in the page", 5000, () => bb("b.state.value.unsaved === true"));
  const rev0 = await bb("b.state.value.rev");
  check("1 an agent's edit: the page says unsaved", ren.status === 200 && (await bb("b.state.value.unsaved")) === true, ren.text);

  // 2: the host dies.
  kill(h, "SIGKILL");
  v = await until((x) => x.lost !== null, 6000);
  check("2 host killed: the page blocks with the notice (shell disconnected)", v.shell === "disconnected" && /Connection to the local Bimble host lost/.test(v.lost ?? ""), JSON.stringify(v));
  check("2 the notice names the file and its unsaved changes, with Retry", /frame\.json/.test(v.lost ?? "") && /with unsaved changes/.test(v.lost ?? "") && v.retry, JSON.stringify(v));
  await click("[data-bb=shell-disconnected-retry]");
  await sleep(1500);
  v = await shows();
  check("2 Retry while the host is down: still the notice", v.shell === "disconnected" && v.lost !== null, JSON.stringify(v));

  // 3: back with its session: resumes.
  h = await startHost(true);
  v = await until((x) => x.shell === "model.view", 8000);
  const rev1 = await bb("b.state.value.rev");
  check("3 host back with the same session: the notice goes, the model's view again", v.shell === "model.view" && v.lost === null && v.gone === null && v.viewport, JSON.stringify(v));
  check("3 same file, same rev, still unsaved", (await bb("b.state.value.file")) === "frame.json" && rev1 === rev0 && (await bb("b.state.value.unsaved")) === true, `rev ${rev0} -> ${rev1}`);

  // 4: killed, then a fresh host without the session: said plainly, no silent Home.
  kill(h, "SIGKILL");
  v = await until((x) => x.lost !== null, 6000);
  check("4 killed again: the notice", v.shell === "disconnected", JSON.stringify(v));
  await sleep(300);
  h = await startHost(false);
  v = await until((x) => x.gone !== null, 8000);
  check("4 a fresh host without the session: \"Session lost\", the unsaved changes of frame.json lost",
    v.shell === "sessionLost" && /Session lost/.test(v.gone ?? "") && /unsaved changes of frame\.json are lost/.test(v.gone ?? ""), JSON.stringify(v));
  await sleep(2000);
  v = await shows();
  check("4 no silent jump to Home (2 s later: still the notice)", v.shell === "sessionLost" && v.gone !== null, JSON.stringify(v));
  const showed = await click("[data-bb=shell-lost-show]");
  v = await until((x) => x.shell === "model.view", 5000);
  check("4 \"Show frame.json\": the fresh host's model, saved", showed && v.shell === "model.view" && v.gone === null && (await bb("b.state.value.unsaved")) === false, JSON.stringify(v));
  check("no uncaught page error", exceptions.length === 0, exceptions.join(" | "));
  cdp.close();
}

try {
  await main();
} catch (e) {
  check("no exception in the test", false, e.stack ?? String(e));
  for (const p of procs) console.log(`--- ${p.label} output ---\n${p.out.slice(-2000)}`);
} finally {
  await cleanup();
}
console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
