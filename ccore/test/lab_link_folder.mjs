// Link a folder in the lab (ticket LINKF, ADR 0079; ccore/session/links.h, web-ui/src/shell/LinksPanel.vue):
//   CCORE_PORT_BASE=auto node ccore/test/lab_link_folder.mjs
// A native host (out/tcc/bimble-ccore or $HOST_BIN, HOME a temp folder, BIMBLE_LINK_CACHE_MIN=0) on a new model, Vite
// and headless Chromium over raw CDP on /?native=1&gl=1. A folder of four synthetic IFC (golden/ifc copies) under
// HOME: Manage links… opens the Links group; Link a folder… opens the page's file dialog, the folder typed, "Link
// this folder" clicked: four rows within 200 ms of the click (ADR 0074), each becomes ready, its cache and report
// beside it; the viewport's mesh holds the four links and the panel's elements add up; the eye hides one (its
// triangles go); one link coloured red (palette 0) shows red pixels; the pin refuses a move, unpinned a move of
// (20000, 0, 0) puts the frame's column P1.00 (centred on (0, 0) in its file) under the point (20000, 0, 1000) and
// nothing of it under (0, 0, 1000) (pick); one undo removes all four (the mesh has no link); the project saved,
// one IFC made unreadable, reopened: the four links from their caches, drawn. No uncaught page error, no console
// error. Each timed click starts from a settled page (three frames under 25 ms apart; the eye: the
// four links drawn). LINKF_PROBE=1 (or =all) prints the frames, the long tasks and every call
// between a click and its rows. Prints "N passed, M failed"; exits non-zero on a failure. Fails before LNK… LINKF: no Link a folder…
// ([data-bb=link-add-folder]) and no link_folder action.
import { spawn } from "node:child_process";
import { chmodSync, copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { CHROMIUM, port, vitePort } from "./ports.mjs";
import { loops, settled } from "./lib/frames.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const G = join(CCORE, "golden");
const UI = resolve(CCORE, "../web-ui");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("lab_link_folder");
const VITE = vitePort("lab_link_folder");
const TOKEN = "lablinkfolder-token";
const PAGE = `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}`;
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-link-folder-"));
const home = join(tmp, "home"), dir = join(home, "consultants");
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

async function cleanup() {
  for (const sig of ["SIGTERM", "SIGKILL"]) {
    for (const p of procs) if (p.exitCode === null && p.signalCode === null) try { process.kill(-p.pid, sig); } catch {}
    await sleep(300);
  }
  try { chmodSync(join(dir, "b_frame2.ifc"), 0o644); } catch {}
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

async function main() {
  if (!existsSync(join(UI, "node_modules"))) throw new Error(`no ${UI}/node_modules: run npm ci in web-ui/`);
  mkdirSync(dir, { recursive: true });
  const FILES = { "a_frame.ifc": "ifc/frame.ifc", "b_frame2.ifc": "ifc/frame2.ifc", "c_mapped.ifc": "ifc/fixtures/mapped_item.ifc", "d_column.ifc": "ifc/fixtures/circle_column.ifc" };
  for (const [n, f] of Object.entries(FILES)) copyFileSync(join(G, f), join(dir, n));
  const host = start("host", BIN, [String(PORT)], {
    env: { ...process.env, BIMBLE_TOKEN: TOKEN, HOME: home, BIMBLE_CONFIG_DIR: join(tmp, "config"), BIMBLE_LINK_CACHE_MIN: "0",
      BIMBLE_CACHE_DIR: join(tmp, "cache"), BIMBLE_ORIGINS: `http://localhost:${VITE},http://127.0.0.1:${VITE}` },
  });
  await waitFor("host 'listening'", 5000, () => host.out.includes("listening") || (host.exitCode !== null && Promise.reject(new Error(host.out))));
  const hcall = async (op, name, body = {}) => {
    const r = await fetch(`http://127.0.0.1:${PORT}/${op}/${name}?t=${TOKEN}`, { method: "POST", body: JSON.stringify(body), headers: { "Content-Type": "text/plain" } });
    return { status: r.status, text: await r.text() };
  };
  const rows = async () => JSON.parse((await hcall("query", "links_panel")).text).models;
  check("a new model", (await hcall("action", "new")).status === 200);

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
  const consoleLog = [], exceptions = [];
  cdp.on((msg) => {
    if (msg.sessionId !== sessionId) return;
    if (msg.method === "Runtime.consoleAPICalled") consoleLog.push({ level: msg.params.type, text: msg.params.args.map((a) => a.value ?? a.description ?? a.type).join(" ") });
    else if (msg.method === "Runtime.exceptionThrown") exceptions.push(msg.params.exceptionDetails.exception?.description ?? msg.params.exceptionDetails.text);
  });
  await S("Runtime.enable");
  await S("Page.enable");
  await S("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  await S("Page.navigate", { url: PAGE });
  const evalJs = async (expr) => (await S("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true })).result.value;
  await waitFor("the first drawn frame", 30000, () => evalJs("window.__bbGl && window.__bbGl.firstFrameMs"));
  await sleep(300);
  const gl = async () => JSON.parse(await evalJs("JSON.stringify(window.__bbGl)"));
  const raf2 = () => evalJs("new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)))");

  // Manage links… opens the Links group; Link a folder… opens the dialog; the folder typed, linked
  await evalJs("(async () => (await import('/src/bimble.ts')).runAction('manage_links'))()");
  await waitFor("the Links panel", 5000, () => evalJs("!!document.querySelector('[data-bb=link-add-folder]')"));
  check("Link a folder… in the panel, enabled", await evalJs("!document.querySelector('[data-bb=link-add-folder]').disabled"));
  await evalJs("document.querySelector('[data-bb=link-add-folder]').click()");
  await waitFor("the file dialog", 5000, () => evalJs("document.querySelector('[data-bb=file-browser]')?.dataset.purpose === 'link_folder'"));
  await evalJs(`(() => { const i = document.querySelector('[data-bb=file-browser-path]'); i.value = ${JSON.stringify(dir)}; i.dispatchEvent(new Event('input'));
    i.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })); })()`);
  await waitFor("the folder listed", 5000, () => evalJs("[...document.querySelectorAll('[data-bb=file-browser-entry]')].length === 4 && !document.querySelector('[data-bb=file-browser-go]').disabled"));
  if (process.env.LINKF_PROBE) await evalJs(`(() => { window.__xlog = []; const o = XMLHttpRequest.prototype.open, sd = XMLHttpRequest.prototype.send;
    XMLHttpRequest.prototype.open = function (m, u, a) { this.__u = u; return o.apply(this, arguments); };
    XMLHttpRequest.prototype.send = function () { const t = performance.now(); const r = sd.apply(this, arguments); window.__xlog.push([performance.now() - window.__t0, performance.now() - t, String(this.__u).replace(/\\?t=.*/, '')]); return r; };
    const f = window.fetch; window.fetch = function (u) { window.__xlog.push([performance.now() - window.__t0, 0, 'fetch ' + String(u).replace(/[?&]t=[^&]*/, '')]); return f.apply(this, arguments); };
    new MutationObserver(() => { if (!window.__rowsAt && document.querySelectorAll('[data-bb=link-model]').length === 4) window.__rowsAt = performance.now() - window.__t0; }).observe(document.body, { childList: true, subtree: true }); })()`);
  await evalJs(`new Promise((res) => { let last = performance.now(), calm = 0; const f = (t) => { calm = t - last < 25 ? calm + 1 : 0; last = t; if (calm >= 3) res(); else requestAnimationFrame(f); }; requestAnimationFrame(f); })`); // a settled page first
  if (process.env.LINKF_PROBE) console.log(`  idle frames before the click (ms apart): ${await evalJs(`new Promise((res) => { const t = []; const f = (x) => { t.push(x); if (t.length < 30) requestAnimationFrame(f); else res(t.slice(1).map((v, i) => (v - t[i]).toFixed(0)).join(' ')); }; requestAnimationFrame(f); })`)}`);
  if (process.env.LINKF_PROBE) await evalJs(`(() => { window.__raf = []; const f = () => { window.__raf.push(performance.now() - (window.__t0 ?? 0)); if (window.__raf.length < 400) requestAnimationFrame(f); }; requestAnimationFrame(f);
    window.__lt = []; try { new PerformanceObserver((l) => { for (const e of l.getEntries()) window.__lt.push([e.startTime - (window.__t0 ?? 0), e.duration]); }).observe({ type: 'longtask', buffered: false }); } catch {} })()`);
  const ms = await evalJs(`new Promise((res) => { const t0 = performance.now(); window.__t0 = t0; document.querySelector('[data-bb=file-browser-go]').click();
    const tick = () => document.querySelectorAll('[data-bb=link-model]').length === 4 ? res(performance.now() - t0) : requestAnimationFrame(tick); tick(); })`);
  check("Link this folder: four rows within 200 ms of the click (ADR 0074)", ms < 200, `${ms.toFixed(1)} ms`);
  if (process.env.LINKF_PROBE) {
    console.log(`probe: rows in the DOM at ${(await evalJs("window.__rowsAt"))?.toFixed(1)} ms, loadavg ${readFileSync("/proc/loadavg", "utf8").trim()}`);
    console.log(`  frames at: ${(await evalJs("window.__raf.filter((t) => t > 0 && t < 1500).map((t) => t.toFixed(0)).join(' ')"))}`);
    console.log(`  long tasks: ${(await evalJs("window.__lt.filter(([t]) => t > -50 && t < 1500).map(([t, d]) => t.toFixed(0) + '+' + d.toFixed(0)).join(' ')"))}`);
    for (const [at, d, u] of await evalJs("window.__xlog")) if (at < 1500 && (process.env.LINKF_PROBE === "all" || /fetch|action/.test(u))) console.log(`  ${at.toFixed(1)} +${d.toFixed(1)} ${u}`);
  }
  const ready = await waitFor("each link ready", 60000, async () => { const r = await rows(); return r.length === 4 && r.every((m) => m.state === "ready") ? r : null; });
  check("each becomes ready", ready.length === 4);
  check("caches and reports beside each IFC", Object.keys(FILES).every((n) => existsSync(join(dir, `${n}.bimbleview`)) && existsSync(join(dir, `${n}.log.txt`))));
  const st = await waitFor("the viewport's mesh with four links", 15000, async () => { const s = await gl(); return s.links.length === 4 && s.linkTris > 0 ? s : null; });
  const sum = ready.reduce((a, m) => a + m.elements, 0), drawn = st.links.reduce((a, l) => a + l.elements, 0);
  check("the scene holds the sum of their elements", sum === drawn && sum > 0, `${sum} / ${drawn}`);
  await waitFor("rows ready in the panel", 5000, () => evalJs("[...document.querySelectorAll('[data-bb=link-model]')].every((r) => r.dataset.state === 'ready')"));

  // hide one through its eye, once the viewport drew the four links (a click is timed from a settled picture)
  await settled(evalJs, "the four links drawn", (await loops(evalJs)) - 1, { rev: "mesh" });
  await evalJs(`new Promise((res) => { let last = performance.now(), calm = 0; const f = (t) => { calm = t - last < 25 ? calm + 1 : 0; last = t; if (calm >= 3) res(); else requestAnimationFrame(f); }; requestAnimationFrame(f); })`);
  const t0 = await gl();
  if (process.env.LINKF_PROBE) await evalJs("window.__xlog = []; window.__lt = []; window.__raf = []; window.__rowsAt = 0; (() => { const f = () => { window.__raf.push(performance.now() - window.__t0); if (window.__raf.length < 200) requestAnimationFrame(f); }; requestAnimationFrame(f); })()");
  const hide = await evalJs(`new Promise((res) => { const t0 = performance.now(); window.__t0 = t0; let row = 0; document.querySelector('[data-bb=link-row][data-name="b_frame2.ifc"] [data-bb=link-eye]').click();
    const tick = () => { if (!row && document.querySelector('[data-bb=link-row][data-name="b_frame2.ifc"]')?.dataset.visible === '0') row = performance.now() - t0;
      if (row && window.__bbGl.links.find((l) => l.name === 'b_frame2.ifc')?.visible === false) res({ row, view: performance.now() - t0 }); else requestAnimationFrame(tick); }; tick(); })`);
  const t1 = await gl();
  if (process.env.LINKF_PROBE) {
    console.log(`  hide frames at: ${(await evalJs("window.__raf.filter((t) => t > 0 && t < 600).map((t) => t.toFixed(0)).join(' ')"))}; long tasks ${(await evalJs("window.__lt.filter(([t]) => t > -50 && t < 600).map(([t, d]) => t.toFixed(0) + '+' + d.toFixed(0)).join(' ')"))}`);
    for (const [at, d, u] of await evalJs("window.__xlog")) if (at < 600) console.log(`  hide ${at.toFixed(1)} +${d.toFixed(1)} ${u}`);
  }
  console.log(`hide: the row in ${hide.row.toFixed(1)} ms, the viewport (its /mesh fetched again) in ${hide.view.toFixed(1)} ms`);
  check("the eye hides one: the row answers within 200 ms, its triangles go", hide.row < 200 && t1.linkTris < t0.linkTris, `${hide.row.toFixed(1)} ms, ${t0.linkTris} -> ${t1.linkTris}`);

  // colour: only a_frame shown, red (palette 0)
  for (const n of ["c_mapped.ifc", "d_column.ifc"]) await hcall("action", "manage_links", { op: "hide", name: n });
  await evalJs("(async () => (await import('/src/bimble.ts')).runAction('fit'))()");
  const red = () => evalJs(`(async () => { await new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)));
    const c = document.getElementById('bimble'), g = c.getContext('webgl2'), p = new Uint8Array(c.width * c.height * 4);
    g.readPixels(0, 0, c.width, c.height, g.RGBA, g.UNSIGNED_BYTE, p); let n = 0;
    for (let i = 0; i < p.length; i += 4) if (p[i] > 100 && p[i] - p[i + 1] > 50 && p[i] - p[i + 2] > 50) n++; return n; })()`);
  await waitFor("two links hidden", 5000, async () => (await gl()).links.filter((l) => l.visible).length === 1);
  const r0 = await red();
  const cdone = await evalJs(`(async () => { document.querySelector('[data-bb=link-row][data-name="a_frame.ifc"]'); const s = document.querySelector('[data-bb=link-model][data-name="a_frame.ifc"] [data-bb=link-colour]');
    s.value = '0'; s.dispatchEvent(new Event('change')); return true; })()`);
  await waitFor("the colour in the mesh", 5000, async () => (await gl()).links.find((l) => l.name === "a_frame.ifc")?.colour === 0);
  await sleep(200);
  const r1 = await red();
  check("a colour override shows in pixels (palette 0, red)", cdone && r1 > r0 + 500, `${r0} -> ${r1} red px`);

  // pin, then a move by hand
  let r = await hcall("action", "manage_links", { op: "place", name: "a_frame.ifc", move_mm: [20000, 0, 0] });
  check("pinned: the move refused", r.status === 400 && /pinned/.test(r.text), r.text);
  await evalJs(`document.querySelector('[data-bb=link-model][data-name="a_frame.ifc"] [data-bb=link-pin]').click()`);
  await waitFor("unpinned", 5000, async () => (await rows()).find((m) => m.name === "a_frame.ifc").pinned === false);
  r = await hcall("action", "manage_links", { op: "place", name: "a_frame.ifc", move_mm: [20000, 0, 0] });
  check("unpinned: moved", r.status === 200, r.text);
  await sleep(300);
  await evalJs("(async () => (await import('/src/bimble.ts')).runAction('fit'))()");
  const hit = await waitFor("the moved column under its new place", 8000, async () => {
    await raf2();
    return evalJs(`(() => { const g = window.__bbGl, at = g.toScreen(20000, 0, 1000), old = g.toScreen(0, 0, 1000);
      if (!at || !old) return null; return { now: g.pickInfoAt(at[0], at[1]), before: g.pickInfoAt(old[0], old[1]) }; })()`).then((v) => v?.now?.link === "a_frame.ifc" ? v : null);
  }).catch(() => null);
  check("the moved link lands where the hand computation says ((20000, 0, 1000): P1.00; (0, 0, 1000): nothing of it)",
    hit?.now?.link === "a_frame.ifc" && hit?.before?.link !== "a_frame.ifc", JSON.stringify(hit));

  // one undo removes all four
  await evalJs("(async () => (await import('/src/bimble.ts')).runAction('undo'))()");
  await waitFor("no link after undo", 8000, async () => (await gl()).links.length === 0).catch(() => null);
  check("one undo removes all four", (await rows()).length === 0 && (await gl()).links.length === 0);
  await evalJs("(async () => (await import('/src/bimble.ts')).runAction('redo'))()");
  await waitFor("four again", 30000, async () => { const x = await rows(); return x.length === 4 && x.every((m) => m.state === "ready"); });

  // saved, one IFC unreadable, reopened: from the caches
  const saved = join(home, "project.bimble");
  check("save_as", (await hcall("action", "save_as", { path: saved })).status === 200);
  chmodSync(join(dir, "b_frame2.ifc"), 0o000);
  r = await hcall("action", "open", { path: saved, discard: true });
  const re = await waitFor("reopened, every link ready", 30000, async () => { const x = await rows(); return x.length === 4 && x.every((m) => m.state === "ready") ? x : null; }).catch(() => null);
  check("reopened: the four links from their caches (one IFC unreadable)", r.status === 200 && re?.every((m) => m.from_cache), JSON.stringify(re?.map((m) => [m.name, m.state, m.from_cache])));
  const st2 = await waitFor("drawn", 15000, async () => { const s = await gl(); return s.links.length === 4 ? s : null; }).catch(() => null);
  check("reopened: the viewport draws four links", st2?.links.length === 4);
  chmodSync(join(dir, "b_frame2.ifc"), 0o644);

  const errors = consoleLog.filter((m) => /error/.test(m.level) && !/favicon|Synchronous XMLHttpRequest/.test(m.text));
  check("no uncaught page error", exceptions.length === 0, exceptions.join(" | "));
  check("no console error", errors.length === 0, errors.map((m) => m.text).join(" | "));
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
