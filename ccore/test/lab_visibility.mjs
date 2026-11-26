// Show all and Show previous in the lab (GitHub issue #72, ticket ISS72): node ccore/test/lab_visibility.mjs
// Starts out/tcc/bimble-ccore (or $HOST_BIN) on a copy of golden/frame.json, copies the wasm build
// (scripts/copy-ccore.mjs), Vite from web-ui/ and headless /usr/bin/chromium over raw CDP. Twice,
// on the native host (/?native=1&gl=1) and on ccore.wasm (/?ccore=1&gl=1), with real mouse clicks
// on the View tab's Visibility buttons (shell/ribbon-tabs.ts):
//   - Show all and Show previous are drawn, both off (nothing hidden, nothing to go back to);
//   - column A selected, Hide: A not drawn (1 element left out, fewer pixels); column B selected,
//     Isolate: only B drawn;
//   - Show previous: A hidden only again (query visibility, __bbGl.hiddenElements 1, the pixels of
//     "A hidden" within 1 %); Show previous again: B isolated (the pixels of "B isolated");
//   - Show all: everything drawn (0 left out, the first picture's pixels within 1 %), Show all off,
//     Show previous still on; no history step for these for-now changes.
// Before ISS72 the two buttons do not exist (no ribbon tool, no action): the first check fails.
// No uncaught page error; no console error or warning but the sync-XHR deprecation and the favicon
// 404; no failed response. Prints "N passed, M failed"; exits non-zero on a failure. Kills all it starts.
import { spawn, execFileSync } from "node:child_process";
import { copyFileSync, existsSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { CHROMIUM, port, vitePort } from "./ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = resolve(CCORE, "../web-ui");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("lab_visibility", 47372);
const VITE = vitePort("lab_visibility", 5135);
const TOKEN = "labvisibility-token";
const PAGES = {
  native: `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}`,
  wasm: `http://localhost:${VITE}/?ccore=1&gl=1`,
};
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-visibility-"));
const procs = [];
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail && !ok ? `: ${detail}` : ""}`);
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

async function cleanup() {
  for (const sig of ["SIGTERM", "SIGKILL"]) {
    for (const p of procs) if (p.exitCode === null && p.signalCode === null) try { process.kill(-p.pid, sig); } catch {}
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
    await sleep(50);
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


async function visibility(mode, S, evalJs) {
  const B = (body) => evalJs(`(async () => { const b = await import('/src/bimble.ts'); ${body} })()`);
  const frames = () => evalJs("window.__bbGl ? window.__bbGl.frames : 0");
  const gl = async () => JSON.parse(await evalJs("JSON.stringify(window.__bbGl)"));
  // Settled: no new frame for 1200 ms (at most 6000 ms), as lab_section.
  const settle = async () => {
    const until = Date.now() + 6000;
    let f = await frames(), since = Date.now();
    while (Date.now() - since < 1200 && Date.now() < until) {
      await sleep(50);
      const g = await frames();
      if (g !== f) (f = g), (since = Date.now());
    }
  };
  /** Pixels that are not the background (247, 247, 245). */
  const modelPx = () => evalJs(`(() => { const c = document.getElementById('bimble'), g = c.getContext('webgl2');
    const p = new Uint8Array(c.width * c.height * 4); g.readPixels(0, 0, c.width, c.height, g.RGBA, g.UNSIGNED_BYTE, p);
    let n = 0; for (let i = 0; i < p.length; i += 4) if (Math.abs(p[i] - 247) > 3 || Math.abs(p[i + 1] - 247) > 3 || Math.abs(p[i + 2] - 245) > 3) n++; return n; })()`);
  const keepPx = (name) => evalJs(`(() => { const c = document.getElementById('bimble'), g = c.getContext('webgl2');
    const p = new Uint8Array(c.width * c.height * 4); g.readPixels(0, 0, c.width, c.height, g.RGBA, g.UNSIGNED_BYTE, p); (window.__keep ??= {})['${name}'] = p; return p.length; })()`);
  const diffPx = (name) => evalJs(`(() => { const c = document.getElementById('bimble'), g = c.getContext('webgl2'), q = window.__keep['${name}'];
    const p = new Uint8Array(c.width * c.height * 4); g.readPixels(0, 0, c.width, c.height, g.RGBA, g.UNSIGNED_BYTE, p);
    if (q.length !== p.length) return -1;
    let n = 0; for (let i = 0; i < p.length; i += 4) if (Math.abs(p[i] - q[i]) > 2 || Math.abs(p[i + 1] - q[i + 1]) > 2 || Math.abs(p[i + 2] - q[i + 2]) > 2) n++; return n; })()`);
  const shot = async (name) => { if (!process.env.SHOTS) return; const { data } = await S("Page.captureScreenshot", { format: "png" }); writeFileSync(join(process.env.SHOTS, `iss72-${mode}-${name}.png`), Buffer.from(data, "base64")); };
  const vis = () => B("return b.query('visibility');");
  const sel = () => B("return b.state.value.selection;");
  const undoN = async () => (await B("return b.query('history');")).undo.length;

  await S("Page.navigate", { url: PAGES[mode] });
  await waitFor(`${mode}: data-bb-ready`, 30000, () => evalJs("document.documentElement.dataset.bbReady === '1'"));
  await waitFor(`${mode}: the mesh`, 20000, async () => (await evalJs("window.__bbGl && window.__bbGl.meshRev != null")) === true);
  await evalJs("(async () => { const tb = await import('/src/shell/tabs.ts'); tb.activeTab.value = 'view'; return 1; })()");
  await sleep(300);
  // The View tab's tools by action: their data-bb markers (Ribbon.vue: ribbon-tool-<group>-<tool>).
  const bb = JSON.parse(await evalJs(`(async () => { const rb = await import('/src/shell/ribbon-tabs.ts'); const out = {};
    rb.RIBBON.view.forEach((g, gi) => g.tools.forEach((t, ti) => { if (t.action) out[t.action] = 'ribbon-tool-' + (gi + 1) + '-' + (ti + 1); }));
    return JSON.stringify(out); })()`));
  const tool = (action) => evalJs(`(() => { const e = document.querySelector('[data-bb=${bb[action] ?? "none"}]'); if (!e) return null;
    const r = e.getBoundingClientRect(); return JSON.stringify({ x: r.left + r.width / 2, y: r.top + r.height / 2, off: e.getAttribute('aria-disabled') === 'true',
      text: e.textContent.trim(), inside: r.right <= innerWidth && r.bottom <= innerHeight && r.width > 0 }); })()`).then((t) => (t ? JSON.parse(t) : null));
  const press = async (action) => {
    const t = await tool(action);
    if (!t) throw new Error(`${mode}: no ribbon tool for ${action}`);
    await S("Input.dispatchMouseEvent", { type: "mousePressed", x: t.x, y: t.y, button: "left", buttons: 1, clickCount: 1 });
    await S("Input.dispatchMouseEvent", { type: "mouseReleased", x: t.x, y: t.y, button: "left", buttons: 0, clickCount: 1 });
  };
  const all = await tool("show_all"), prev = await tool("show_previous");
  check(`${mode}: View › Visibility shows Show all and Show previous, both off (nothing hidden, nothing to go back to)`,
    !!all && !!prev && all.off && prev.off && all.inside && prev.inside, JSON.stringify({ all, prev }));
  if (!all || !prev) return;
  console.log(`${mode}: labels "${all.text}", "${prev.text}"`);

  const els = await B("return b.query('elements', { limit: 100000 });");
  const n = els.length;
  // Two columns the camera sees (picked at their middle).
  const cols = [];
  for (const e of els.filter((x) => x.class === "IfcColumn")) {
    const bx = JSON.parse(await evalJs(`JSON.stringify(window.__bbGl.elementBox(${JSON.stringify(e.id)}))`));
    if (!bx) continue;
    const xy = JSON.parse(await evalJs(`JSON.stringify(window.__bbGl.toScreen(${(bx.min[0] + bx.max[0]) / 2}, ${(bx.min[1] + bx.max[1]) / 2}, ${(bx.min[2] + bx.max[2]) / 2}))`));
    if (xy && (await evalJs(`window.__bbGl.pickAt(${xy[0]}, ${xy[1]})`)) === e.id) cols.push(e.id);
    if (cols.length === 2) break;
  }
  check(`${mode}: two columns the camera sees`, cols.length === 2, JSON.stringify(cols));
  if (cols.length < 2) return;
  const [A, Bc] = cols;
  const undo0 = await undoN();
  await settle();
  const px0 = await modelPx();
  await keepPx(`${mode}-all`);
  await shot("0-all");

  // Hide A, isolate B (the ribbon's own buttons).
  await B(`return b.select(['${A}']);`);
  await waitFor(`${mode}: A selected`, 5000, async () => (await sel())[0] === A);
  await press("hide_now");
  await waitFor(`${mode}: A hidden`, 5000, async () => (await vis()).hidden_now.count === 1);
  await waitFor(`${mode}: the selection cleared`, 5000, async () => (await sel()).length === 0);
  await settle();
  let g = await gl();
  const pxA = await modelPx();
  await keepPx(`${mode}-hideA`);
  check(`${mode}: Hide on column A: one element left out, fewer pixels`, g.hiddenElements === 1 && pxA < px0, `${g.hiddenElements} hidden, ${px0} -> ${pxA} px`);
  await B(`return b.select(['${Bc}']);`);
  await waitFor(`${mode}: B selected`, 5000, async () => (await sel())[0] === Bc);
  await press("isolate_now");
  await waitFor(`${mode}: B isolated`, 5000, async () => (await vis()).isolated_now.count === 1);
  await waitFor(`${mode}: the selection cleared`, 5000, async () => (await sel()).length === 0);
  await settle();
  g = await gl();
  const pxB = await modelPx();
  await keepPx(`${mode}-isoB`);
  check(`${mode}: Isolate on column B: only B drawn`, g.hiddenElements === n - 1 && pxB < 0.25 * px0, `${g.hiddenElements} of ${n} hidden, ${pxB} px`);
  const t1 = await tool("show_previous"), t2 = await tool("show_all");
  check(`${mode}: Show previous and Show all now on`, !t1.off && !t2.off, JSON.stringify({ t1, t2 }));
  await shot("1-isolated");

  // Show previous: A hidden only; again: B isolated.
  await press("show_previous");
  await waitFor(`${mode}: back to A hidden`, 5000, async () => { const v = await vis(); return v.hidden_now.count === 1 && v.isolated_now.count === 0; }).catch(() => {});
  await settle();
  let v = await vis();
  g = await gl();
  let d = await diffPx(`${mode}-hideA`);
  check(`${mode}: Show previous: A hidden only again (the picture of "A hidden")`,
    JSON.stringify(v.hidden_now.ids) === JSON.stringify([A]) && v.isolated_now.count === 0 && g.hiddenElements === 1 && d >= 0 && d < 0.01 * px0,
    `${JSON.stringify(v.hidden_now)} ${g.hiddenElements} hidden, ${d} px differ`);
  await shot("2-previous");
  await press("show_previous");
  await waitFor(`${mode}: B isolated again`, 5000, async () => (await vis()).isolated_now.count === 1).catch(() => {});
  await settle();
  v = await vis();
  g = await gl();
  d = await diffPx(`${mode}-isoB`);
  check(`${mode}: Show previous again: B isolated (back and forth; the picture of "B isolated")`,
    JSON.stringify(v.isolated_now.ids) === JSON.stringify([Bc]) && g.hiddenElements === n - 1 && d >= 0 && d < 0.01 * px0,
    `${JSON.stringify(v.isolated_now)} ${g.hiddenElements} hidden, ${d} px differ`);

  // Show all: everything drawn.
  await press("show_all");
  await waitFor(`${mode}: nothing hidden`, 5000, async () => { const w = await vis(); return w.isolated_now.count === 0 && w.hidden_now.count === 0; }).catch(() => {});
  await settle();
  g = await gl();
  d = await diffPx(`${mode}-all`);
  check(`${mode}: Show all: every element drawn (the first picture)`, g.hiddenElements === 0 && d >= 0 && d < 0.01 * px0, `${g.hiddenElements} hidden, ${d} px differ`);
  const t3 = await tool("show_all"), t4 = await tool("show_previous");
  check(`${mode}: after Show all: Show all off, Show previous on`, t3.off && !t4.off, JSON.stringify({ t3, t4 }));
  check(`${mode}: no history step (hidden for now is session state)`, (await undoN()) === undo0, `${undo0} -> ${await undoN()}`);
  await shot("3-all");
}

async function main() {
  if (!existsSync(join(UI, "node_modules"))) throw new Error(`no ${UI}/node_modules: run npm ci in web-ui/`);
  execFileSync("node", [join(UI, "scripts/copy-ccore.mjs")], { stdio: "ignore" });
  const model = join(tmp, "frame.bimble");
  copyFileSync(join(CCORE, "golden/frame.json"), model);
  const h = start("host", BIN, [String(PORT), model], {
    env: { ...process.env, BIMBLE_TOKEN: TOKEN, BIMBLE_ORIGINS: `http://localhost:${VITE},http://127.0.0.1:${VITE}` },
  });
  await waitFor("host 'listening'", 5000, () => h.out.includes("listening") || (h.exitCode !== null && Promise.reject(new Error(h.out))));
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
  const consoleLog = [], exceptions = [], failedResp = [];
  cdp.on((msg) => {
    if (msg.sessionId !== sessionId) return;
    if (msg.method === "Network.responseReceived" && msg.params.response.status >= 400) failedResp.push(`${msg.params.response.status} ${msg.params.response.url}`);
    if (msg.method === "Runtime.consoleAPICalled") {
      consoleLog.push({ level: msg.params.type, text: msg.params.args.map((a) => a.value ?? a.description ?? a.type).join(" ") });
    } else if (msg.method === "Runtime.exceptionThrown") {
      const d = msg.params.exceptionDetails;
      exceptions.push(d.exception?.description ?? d.text);
    } else if (msg.method === "Log.entryAdded") {
      consoleLog.push({ level: `log.${msg.params.entry.level}`, text: `${msg.params.entry.text} ${msg.params.entry.url ?? ""}`.trim() });
    }
  });
  for (const d of ["Runtime", "Log", "Page", "Network"]) await S(`${d}.enable`);
  await S("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  const evalJs = async (expr) => {
    const r = await S("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
    return r.result.value;
  };
  for (const mode of ["native", "wasm"]) await visibility(mode, S, evalJs);

  const IGNORED = [
    /Synchronous XMLHttpRequest on the main thread is deprecated/,
    /404 \(Not Found\) http:\/\/localhost:\d+\/favicon\.ico$/,
  ];
  const errors = consoleLog.filter((m) => /error|warn/.test(m.level) && !IGNORED.some((r) => r.test(m.text)));
  check("no uncaught page error", exceptions.length === 0, exceptions.join(" | "));
  check("no console error or warning", errors.length === 0, errors.map((m) => m.text).join(" | "));
  const badResp = failedResp.filter((r) => !/^404 http:\/\/localhost:\d+\/favicon\.ico$/.test(r));
  check("no failed response", badResp.length === 0, badResp.join(" | "));
  cdp.close();
}

try {
  await main();
} catch (e) {
  check("no exception in the test", false, e.stack ?? String(e));
  for (const p of procs) console.log(`--- ${p.label} output ---\n${p.out.slice(-2000)}`);
} finally {
  await cleanup();
  console.log(`${passed} passed, ${failed} failed`);
  process.exit(failed ? 1 : 0);
}
