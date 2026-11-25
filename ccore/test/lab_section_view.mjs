// Section views and per-view visibility in the lab (ticket SEC1):
//   node ccore/test/lab_section_view.mjs
// Starts out/tcc/bimble-ccore (or $HOST_BIN) on a copy of golden/frame.json, copies the wasm build
// (scripts/copy-ccore.mjs), Vite from web-ui/ and headless /usr/bin/chromium over raw CDP. Twice,
// on the native host (/?native=1&gl=1) and on ccore.wasm (/?ccore=1&gl=1):
//   - the Navigator's New view menu (shell/Navigator.vue) makes "Section 1 (looking north)" through
//     the middle of the frame (9000, 5000): one undo step, its tab opens and is current, view_clip
//     is a section, the camera is the flat one (gl/section.ts);
//   - the column at (6000, 5000) (cut by the section) projects where the camera says (±1 px, from
//     the camera's target and half height, independently of toScreen), is drawn there (not the
//     background) and picked there; the column at (6000, 0), between the eye and the cut, is not
//     (the pick finds the cut one: what lies before the cut is clipped);
//   - the cut moved to y 7500 (Properties: Through Y): the column at (6000, 10000) is seen there and
//     the one at (6000, 5000), now behind the cut plane on the eye's side, is not; depth 2000
//     (Properties: Depth): the column at 10000, beyond the depth, is not drawn nor picked;
//   - IfcSlab hidden in the section (Properties: its class checkbox): the slabs are hidden there
//     (visibility_sets by_view, the slab no longer picked), not in the plan of Niveau 2; back in
//     the section, still hidden; undo shows them again (one step).
// Before SEC1 the New view button was disabled (no view_add verb: the menu step fails), a section's
// view_clip had no box and the viewport stayed 3D, and the view properties had no class checkbox.
// No uncaught page error; no console error or warning but the sync-XHR deprecation and the favicon
// 404. Prints "N passed, M failed"; exits non-zero on a failure. Kills all it starts.
import { spawn, execFileSync } from "node:child_process";
import { copyFileSync, existsSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { CHROMIUM, port, vitePort } from "./ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = resolve(CCORE, "../web-ui");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("lab_section_view", 47379);
const VITE = vitePort("lab_section_view", 5131);
const TOKEN = "labsecview-token";
const PAGES = {
  native: `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}`,
  wasm: `http://localhost:${VITE}/?ccore=1&gl=1`,
};
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-secview-"));
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


async function sectionView(mode, S, evalJs) {
  const B = (body) => evalJs(`(async () => { const b = await import('/src/bimble.ts'); ${body} })()`);
  const raf2 = () => evalJs("new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)))");
  const frames = () => evalJs("window.__bbGl ? window.__bbGl.frames : 0");
  const settle = async () => { const f0 = await frames(); await waitFor("a new frame", 3000, async () => (await frames()) > f0).catch(() => {}); for (let i = 0; i < 3; i++) await raf2(); };
  const click = (sel) => evalJs(`(() => { const el = document.querySelector(${JSON.stringify(sel)}); if (!el) return false; el.click(); return true; })()`);
  /** Sets an input's value and sends Enter (the Properties fields commit on Enter or blur). */
  const type = (sel, value) => evalJs(`(() => { const el = document.querySelector(${JSON.stringify(sel)}); if (!el) return false;
    el.value = ${JSON.stringify(value)}; el.dispatchEvent(new Event('input', { bubbles: true }));
    el.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })); return true; })()`);
  const undoN = async () => (await B("return b.query('history');")).undo.length;
  const cam = () => evalJs("JSON.stringify(window.__bbGl.camera)").then(JSON.parse);
  const toScreen = (x, y, z) => evalJs(`JSON.stringify(window.__bbGl.toScreen(${x}, ${y}, ${z}))`).then(JSON.parse);
  const pickAt = (p) => evalJs(`window.__bbGl.pickAt(${p[0]}, ${p[1]})`);
  const pixelAt = ([x, y]) => evalJs(`(() => { const c = document.getElementById('bimble'), g = c.getContext('webgl2'), r = c.getBoundingClientRect();
    const p = new Uint8Array(4); g.readPixels(Math.round(${x} - r.left), c.height - 1 - Math.round(${y} - r.top), 1, 1, g.RGBA, g.UNSIGNED_BYTE, p); return [p[0], p[1], p[2]]; })()`);
  const isBg = (p) => Math.abs(p[0] - 247) <= 3 && Math.abs(p[1] - 247) <= 3 && Math.abs(p[2] - 245) <= 3;
  const byView = async () => (await B("return b.visibilitySets();"))?.by_view?.length ?? -1;

  await S("Page.navigate", { url: PAGES[mode] });
  await waitFor(`${mode}: data-bb-ready`, 30000, () => evalJs("document.documentElement.dataset.bbReady === '1'"));
  await waitFor(`${mode}: the mesh`, 20000, async () => (await evalJs("window.__bbGl && window.__bbGl.meshRev != null")) === true);
  const els = await B("return b.query('elements', { limit: 1000 });");
  const st = await B("return b.query('storeys');");
  const lvl = (n) => st.find((s) => s.name === n);
  // Columns of Niveau 1 by their centre (elementBox: world mm).
  const boxOf = (id) => evalJs(`JSON.stringify(window.__bbGl.elementBox(${JSON.stringify(id)}))`).then(JSON.parse);
  const colAt = async (x, y) => {
    for (const e of els.filter((e) => e.class === "IfcColumn" && e.storey === lvl("Niveau 1")?.id)) {
      const b = await boxOf(e.id);
      if (b && Math.abs((b.min[0] + b.max[0]) / 2 - x) < 1 && Math.abs((b.min[1] + b.max[1]) / 2 - y) < 1) return { id: e.id, box: b };
    }
    return null;
  };
  const c5 = await colAt(6000, 5000), c0 = await colAt(6000, 0), c10 = await colAt(6000, 10000);
  check(`${mode}: the frame's columns at (6000, 0 / 5000 / 10000) on Niveau 1`, !!(c5 && c0 && c10));

  // ---- New view… > Section looking north ----
  const u0 = await undoN();
  const opened = await click("[data-bb=new-view]");
  await waitFor(`${mode}: the menu`, 3000, () => evalJs("!!document.querySelector('[data-bb=new-view-menu]')")).catch(() => {});
  check(`${mode}: New view… opens Rust's menu (plans, ceilings, 4 sections, 3D)`, opened &&
    (await evalJs("document.querySelectorAll('[data-bb=new-view-menu] button').length")) === 2 * st.length + 5);
  await click("[data-bb=new-section-north]");
  const ov = await waitFor(`${mode}: the section listed`, 5000, async () => {
    const o = await B("return b.openViews.value;");
    return o.views?.some((v) => v.name === "Section 1 (looking north)") ? o : null;
  }).catch(() => B("return b.openViews.value;"));
  const sec = ov.views?.find((v) => v.name === "Section 1 (looking north)");
  check(`${mode}: "Section 1 (looking north)" through the middle (9000, 5000), one undo step`,
    !!sec && JSON.stringify(sec.origin) === "[9000,5000]" && sec.facing === "north" && (await undoN()) === u0 + 1, JSON.stringify(sec));
  check(`${mode}: its tab opens, current`, ov.active === sec?.id && (ov.tabs ?? []).some((t) => t.id === sec?.id), JSON.stringify({ active: ov.active, tabs: ov.tabs }));
  await waitFor(`${mode}: the flat camera`, 8000, async () => (await cam())?.view === sec?.id && (await cam()).mode === "plan").catch(() => {});
  const vc = await B("return b.viewClip();");
  check(`${mode}: view_clip: a section, its box from the cut on`, vc?.type === "section" && !!vc.clip && JSON.stringify(vc.dir) === "[0,1]", JSON.stringify(vc));
  await settle();

  if (process.env.SHOTS) { // SHOTS=<dir>: the section as drawn, for a look
    const { data } = await S("Page.captureScreenshot", { format: "png" });
    (await import("node:fs")).writeFileSync(join(process.env.SHOTS, `sec1-${mode}.png`), Buffer.from(data, "base64"));
  }
  // ---- a known column where expected ----
  const o = (await B("return b.query('visibility');")).section_box.origin_mm;
  const expect = async (x, y, z) => { // orthographic looking north: right = +x, up = +z
    const c = await cam(), r = await evalJs("JSON.stringify((() => { const c = document.getElementById('bimble'); const b = c.getBoundingClientRect(); return [b.left, b.top, c.clientWidth, c.clientHeight, c.width / c.height]; })())").then(JSON.parse);
    const dx = x - o[0] - c.target[0], dz = z - o[2] - c.target[2];
    return [r[0] + ((dx / (c.half * r[4])) + 1) / 2 * r[2], r[1] + (1 - dz / c.half) / 2 * r[3]];
  };
  const mid = (b) => [(b.min[0] + b.max[0]) / 2, (b.min[1] + b.max[1]) / 2, (b.min[2] + b.max[2]) / 2];
  const p5 = c5 ? mid(c5.box) : [6000, 5000, 1500];
  const at5 = await toScreen(...p5), want5 = await expect(...p5);
  check(`${mode}: the column at (6000, 5000) projects where the section camera puts it (±1 px)`,
    !!at5 && Math.abs(at5[0] - want5[0]) <= 1 && Math.abs(at5[1] - want5[1]) <= 1, JSON.stringify({ at5, want5 }));
  check(`${mode}: drawn there (not the background)`, !!at5 && !isBg(await pixelAt(at5)), JSON.stringify(at5 && (await pixelAt(at5))));
  check(`${mode}: picked there: the cut column, not the one at y 0 before the cut`, !!at5 && (await pickAt(at5)) === c5?.id, String(at5 && (await pickAt(at5))));

  // ---- the cut moved to y 7500: behind the cut plane not drawn; then a depth ----
  const typed = await type("[data-bb=view-origin-y]", "7500");
  await waitFor(`${mode}: origin y 7500`, 5000, async () => JSON.stringify((await B("return b.viewClip();"))?.origin) === "[9000,7500]").catch(() => {});
  await settle();
  const at10 = await toScreen(...(c10 ? mid(c10.box) : [6000, 10000, 1500]));
  check(`${mode}: Properties' Through Y moves the cut to 7500`, typed && JSON.stringify((await B("return b.viewClip();"))?.origin) === "[9000,7500]");
  check(`${mode}: the column at y 10000 seen beyond the cut, the one at 5000 (eye side) not`,
    !!at10 && (await pickAt(at10)) === c10?.id && !isBg(await pixelAt(at10)), String(at10 && (await pickAt(at10))));
  await type("[data-bb=view-depth]", "2000");
  await waitFor(`${mode}: depth 2000`, 5000, async () => (await B("return b.viewClip();"))?.depth_mm === 2000).catch(() => {});
  await settle();
  const at10b = await toScreen(...(c10 ? mid(c10.box) : [6000, 10000, 1500]));
  check(`${mode}: depth 2 m: the column at y 10000, beyond it, is neither drawn nor picked`,
    !!at10b && (await pickAt(at10b)) === null && isBg(await pixelAt(at10b)), JSON.stringify({ pick: at10b && (await pickAt(at10b)), px: at10b && (await pixelAt(at10b)) }));
  // Back through the middle, no depth: the slabs are cut there.
  await type("[data-bb=view-origin-y]", "5000");
  await type("[data-bb=view-depth]", "0");
  await waitFor(`${mode}: back at 5000`, 5000, async () => { const v = await B("return b.viewClip();"); return JSON.stringify(v?.origin) === "[9000,5000]" && v.depth_mm === 0; }).catch(() => {});
  await settle();

  // ---- IfcSlab hidden in this view only ----
  const slab = els.find((e) => e.class === "IfcSlab" && e.storey === lvl("Niveau 2")?.id) ?? els.find((e) => e.class === "IfcSlab");
  const sb = slab ? await boxOf(slab.id) : null;
  const ps = sb ? await toScreen(3000, 5000, (sb.min[2] + sb.max[2]) / 2) : null;
  check(`${mode}: the slab is picked in the section`, !!ps && (await pickAt(ps)) === slab?.id, String(ps && (await pickAt(ps))));
  const slabs = els.filter((e) => e.class === "IfcSlab").length;
  const u1 = await undoN();
  const hasBox = await click("input[data-class=IfcSlab]");
  await waitFor(`${mode}: slabs hidden`, 5000, async () => (await byView()) === slabs).catch(() => {});
  await settle();
  check(`${mode}: IfcSlab unticked in the view's properties: every slab hidden in the section, one step`,
    hasBox && (await byView()) === slabs && (await undoN()) === u1 + 1, `${await byView()} of ${slabs}`);
  check(`${mode}: the slab is no longer picked there`, !!ps && (await pickAt(ps)) !== slab?.id);
  const plan2 = ov.views.find((v) => v.type === "plan" && v.look === "down" && v.storey === lvl("Niveau 2")?.id);
  await B(`return b.openView(${JSON.stringify(plan2?.id)});`);
  await waitFor(`${mode}: the plan`, 8000, async () => (await cam())?.view === plan2?.id).catch(() => {});
  await settle();
  check(`${mode}: the plan of Niveau 2 still shows its slabs`, (await byView()) === 0 && (await B("return b.viewVisibility()?.hidden_classes.length;")) === 0);
  await B(`return b.openView(${JSON.stringify(sec?.id)});`);
  await waitFor(`${mode}: the section again`, 8000, async () => (await cam())?.view === sec?.id).catch(() => {});
  check(`${mode}: back in the section: still hidden (stored in the view)`, (await byView()) === slabs);
  await B("return b.undo();");
  await waitFor(`${mode}: undo`, 5000, async () => (await byView()) === 0).catch(() => {});
  await settle();
  check(`${mode}: undo: the slabs shown again in the section`, (await byView()) === 0 && !!ps && (await pickAt(ps)) === slab?.id, String(await byView()));
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
  for (const mode of ["native", "wasm"]) await sectionView(mode, S, evalJs);

  const IGNORED = [
    /Synchronous XMLHttpRequest on the main thread is deprecated/,
    /404 \(Not Found\) http:\/\/localhost:\d+\/favicon\.ico$/,
    // Chrome's line for a call the native host answered 400 (bimble.ts's sync XHR): every one is
    // checked below to be a refusal this test asks for or a model query of a read-only scene.
    new RegExp(`400 \\(Bad Request\\) http://localhost:${VITE}/src/bimble\\.ts$`),
    // The test's own pixel reads (getImageData on the overlay), not the page's.
    /^Canvas2D: Multiple readback operations using getImageData/,
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
