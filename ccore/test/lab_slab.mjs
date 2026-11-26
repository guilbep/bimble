// Placing in the lab (ticket SLAB1): node ccore/test/lab_slab.mjs
// Starts out/tcc/bimble-ccore (or $HOST_BIN) on a copy of golden/frame.json, copies the wasm build
// (scripts/copy-ccore.mjs), Vite from web-ui/ and headless /usr/bin/chromium over raw CDP. Twice,
// on the native host (/?native=1&gl=1) and on ccore.wasm (/?ccore=1&gl=1), with real mouse and key
// events on the canvas, in the plan of Fondations (the placing storey), below the frame (a far column at (10000, -12000) widens the fitted view):
//   polygon slab: shape polygon; 5 corners clicked (Shift: no snap); from the second the ghost is
//     drawn (ghost triangles, blue pixels); Enter closes: one IfcSlab, one history step, its outline
//     the clicked corners (relative to the lower-left one, counter-clockwise), area by hand; a pixel
//     inside the outline changes from the background to the slab's colour, one outside does not.
//   Esc: 3 corners, Esc -> 2 corners, still placing; Esc twice more -> none, still placing; once
//     more -> placing off.
//   grid snap: a grid added (x 12000 15000, y -9000 -6000); a column clicked 30-40 mm off the
//     crossing (12000, -9000) lands on it exactly (snap "axis crossing").
//   one axis more: axis 2 chosen on the Levels & grids page, "Add an axis" adds axis 3 (x 18000),
//     one history step; back in the plan the grid layer draws 5 axes.
// Before SLAB1: the first polygon click answered "not placed by clicks yet" (no corner kept, no
// ghost), the column stayed where clicked (rounded to 10 mm: no grid in the snap targets) and the
// page's axis buttons were disabled. No uncaught page error; no console error or warning but the
// sync-XHR deprecation and the favicon 404. Prints "N passed, M failed"; exits non-zero on a failure.
import { spawn, execFileSync } from "node:child_process";
import { copyFileSync, existsSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { CHROMIUM, port, vitePort } from "./ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = resolve(CCORE, "../web-ui");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("lab_slab");
const VITE = vitePort("lab_slab");
const TOKEN = "labslab-token";
const PAGES = {
  native: `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}`,
  wasm: `http://localhost:${VITE}/?ccore=1&gl=1`,
};
const FOND = "04Aobkc8v6$94cGl_9HNVX"; // frame.json: Fondations, -800
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-slab-"));
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

async function run(mode, S, evalJs) {
  const B = (body) => evalJs(`(async () => { const b = await import('/src/bimble.ts'); ${body} })()`);
  const P = (body) => evalJs(`(async () => { const p = await import('/src/pages/index.ts'); const m = await import('/src/shell/send.ts'); ${body} })()`);
  const gl = async () => JSON.parse(await evalJs("JSON.stringify(window.__bbGl)"));
  const frames = () => evalJs("window.__bbGl ? window.__bbGl.frames : 0");
  const raf2 = () => evalJs("new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)))");
  const settle = async () => { const f0 = await frames(); await waitFor("a new frame", 3000, async () => (await frames()) > f0).catch(() => {}); for (let i = 0; i < 3; i++) await raf2(); };
  const undoN = async () => (await B("return b.query('history');")).undo.length;
  const rev = () => B("return b.state.value.rev;");
  const screen = async (x, y, z) => {
    const xy = JSON.parse(await evalJs(`JSON.stringify(window.__bbGl.toScreen(${x}, ${y}, ${z}))`));
    if (!xy) throw new Error(`${mode}: (${x}, ${y}) is behind the camera`);
    if (!(await evalJs(`document.elementFromPoint(${xy[0]}, ${xy[1]})?.id === 'bimble'`))) throw new Error(`${mode}: (${x}, ${y}) -> ${xy} is not on the canvas`);
    return xy;
  };
  const move = async ([x, y], shift = true) => {
    const f0 = await frames(), n0 = (await gl()).toolCalls;
    await S("Input.dispatchMouseEvent", { type: "mouseMoved", x, y, button: "none", buttons: 0, modifiers: shift ? 8 : 0 });
    await waitFor("a tool answer drawn", 5000, async () => (await gl()).toolCalls > n0 && (await frames()) > f0);
  };
  const click = async ([x, y], shift = true) => {
    const n0 = (await gl()).toolCalls;
    const m = shift ? 8 : 0;
    await S("Input.dispatchMouseEvent", { type: "mouseMoved", x, y, button: "none", buttons: 0, modifiers: m });
    await S("Input.dispatchMouseEvent", { type: "mousePressed", x, y, button: "left", buttons: 1, clickCount: 1, modifiers: m });
    await S("Input.dispatchMouseEvent", { type: "mouseReleased", x, y, button: "left", buttons: 0, clickCount: 1, modifiers: m });
    await waitFor("the click's tool answer", 5000, async () => (await gl()).toolCalls > n0);
  };
  const key = async (k, code, vk) => {
    await S("Input.dispatchKeyEvent", { type: "keyDown", key: k, code, windowsVirtualKeyCode: vk });
    await S("Input.dispatchKeyEvent", { type: "keyUp", key: k, code, windowsVirtualKeyCode: vk });
  };
  const points = () => B("return b.placing.value?.points.length ?? -1;");
  /** The canvas pixel (RGBA) under client px (x, y). */
  const pixel = ([x, y]) => evalJs(`(() => { const c = document.getElementById('bimble'), g = c.getContext('webgl2'), r = c.getBoundingClientRect();
    const px = Math.round((${x} - r.left) * c.width / r.width), py = c.height - 1 - Math.round((${y} - r.top) * c.height / r.height);
    const p = new Uint8Array(4); g.readPixels(px, py, 1, 1, g.RGBA, g.UNSIGNED_BYTE, p); return [...p]; })()`);
  const bluePx = () => evalJs(`(() => { const c = document.getElementById('bimble'), g = c.getContext('webgl2');
    const p = new Uint8Array(c.width * c.height * 4); g.readPixels(0, 0, c.width, c.height, g.RGBA, g.UNSIGNED_BYTE, p);
    let n = 0; for (let i = 0; i < p.length; i += 4) if (p[i + 2] - p[i] > 40 && p[i + 2] > p[i + 1]) n++; return n; })()`);
  const page = async (id) => {
    const ok = await P(`return m.send(${JSON.stringify(id === null ? { do: "showView" } : { do: "showPage", page: id })}) && p.currentPage.value === ${JSON.stringify(id)};`);
    if (!ok) throw new Error(`${mode}: the shell did not show ${id ?? "the view"}`);
    await settle();
  };
  const sameRgb = (a, b) => a.slice(0, 3).every((v, i) => Math.abs(v - b[i]) <= 6);

  await S("Page.navigate", { url: PAGES[mode] });
  await waitFor(`${mode}: data-bb-ready`, 30000, () => evalJs("document.documentElement.dataset.bbReady === '1'"));
  await waitFor(`${mode}: the first frame`, 30000, () => evalJs("window.__bbGl && window.__bbGl.firstFrameMs"));
  await waitFor(`${mode}: the model`, 15000, () => B("return b.placing.value !== null && b.state.value.rev >= 0;"));

  // The plan of Fondations, fitted, then panned so the free area x 22000..32000 is in sight.
  const views = (await B("return b.openViews.value;")).views;
  const plan = views.find((v) => v.type === "plan" && v.look === "down" && v.storey === FOND) ?? views.find((v) => v.type === "plan");
  const eo = await B(`return b.openView(${JSON.stringify(plan?.id)});`);
  check(`${mode}: the Fondations plan opens`, !eo && !!plan, String(eo));
  await waitFor(`${mode}: the plan camera`, 10000, async () => (await evalJs("window.__bbGl.camera && window.__bbGl.camera.mode")) === "plan").catch(() => {});
  // A wide model so the free area is in the fitted view: a far column, removed at the end.
  const far = await B(`return b.runVerb('place', { kind: 'column', storey: '${FOND}', a: [10000, -12000] });`);
  check(`${mode}: a far column (the view's extent)`, far === null, String(far));
  await waitFor(`${mode}: the far column in the mesh`, 8000, async () => (await B("return b.state.value.rev;")) >= 1 && (await gl()).frames > 0).catch(() => {});
  await settle();
  await B("return b.runAction('fit');");
  await settle();
  const Z = -800; // the placing storey's level (the tool's plane)
  const e0 = await B(`return b.setPlacing({ storey: '${FOND}', kind: 'slab', shape: 'polygon', top_offset_mm: -500 });`); // the plan of Fondations shows -800..-400
  check(`${mode}: placing a polygon slab on Fondations`, e0 === null, String(e0));
  await waitFor(`${mode}: placing slab`, 5000, () => B("return b.placing.value?.active === true && b.placing.value.kind === 'slab' && b.placing.value.shape === 'polygon';"));

  // ---- the 5-corner slab ----
  const corners = [[2000, -10000], [8000, -10000], [8000, -6000], [5000, -4000], [2000, -6000]];
  const inside = [5000, -7500], outside = [9000, -7500];
  const sIn = await screen(inside[0], inside[1], Z), sOut = await screen(outside[0], outside[1], Z);
  const bg = await pixel(sOut), in0 = await pixel(sIn);
  check(`${mode}: before: the inside pixel is the background`, sameRgb(in0, bg), `${in0} vs ${bg}`);
  const blue0 = await bluePx();
  const r0 = await rev(), u0 = await undoN();
  for (let i = 0; i < corners.length; i++) {
    const s = await screen(corners[i][0], corners[i][1], Z);
    if (i === 2) {
      await move(s);
      const g = await gl();
      check(`${mode}: 2 corners and the pointer: a ghost`, g.ghostTris > 0, `${g.ghostTris} triangles, ${g.toolError}`);
      check(`${mode}: the ghost is drawn (blue pixels)`, (await bluePx()) > blue0 + 50, `${blue0} -> ${await bluePx()}`);
    }
    await click(s);
    await waitFor(`${mode}: corner ${i + 1}`, 5000, async () => (await points()) === i + 1).catch(() => {});
  }
  const clicked = await B("return b.placing.value.points;");
  check(`${mode}: 5 corners kept, nothing placed yet`, clicked.length === 5 && (await rev()) === r0, JSON.stringify(clicked));
  const hint = await evalJs("document.querySelector('[data-bb=status-hint]')?.textContent ?? ''");
  check(`${mode}: the status bar hint names Enter`, /Enter|Entrée/.test(hint), hint);
  await key("Enter", "Enter", 13);
  await waitFor(`${mode}: rev + 1 after Enter`, 5000, async () => (await rev()) === r0 + 1).catch(() => {});
  let g = await gl();
  const slab = g.placed ? await B(`return b.element('${g.placed}');`) : null;
  const x0 = Math.min(...clicked.map((p) => p[0])), y0 = Math.min(...clicked.map((p) => p[1]));
  let area2 = 0;
  for (let i = 0; i < clicked.length; i++) { const p = clicked[i], q = clicked[(i + 1) % clicked.length]; area2 += p[0] * q[1] - q[0] * p[1]; }
  const ccw = area2 < 0 ? [...clicked].reverse() : clicked;
  const want = ccw.map((p) => [p[0] - x0 + 0, p[1] - y0 + 0]);
  const outline = slab?.generator_params?.outline;
  check(`${mode}: Enter closes: one IfcSlab, its outline the clicked corners`, slab?.class === "IfcSlab" && JSON.stringify(outline) === JSON.stringify(want),
    JSON.stringify({ cls: slab?.class, outline, want }));
  check(`${mode}: area by hand: 6000 x 4000 + 6000 x 2000 / 2 = 30 m²`, Math.abs(Math.abs(area2) / 2 - 30e6) < 2e5, `${Math.abs(area2) / 2}`);
  check(`${mode}: the slab on Fondations, 500 mm lower (top offset): z 550 - 500 from its level`, slab?.placement?.origin[2] === 50, JSON.stringify(slab?.placement?.origin));
  check(`${mode}: one history step "add D…"`, (await undoN()) === u0 + 1 && /^add D/.test((await B("return b.query('history').undo;"))[0]));
  check(`${mode}: still placing, no corner`, (await points()) === 0 && (await B("return b.placing.value.active;")) === true);
  await waitFor(`${mode}: the slab drawn`, 8000, async () => !sameRgb(await pixel(sIn), bg)).catch(() => {});
  await settle();
  if (process.env.SLAB1_SHOT) writeFileSync(`${process.env.SLAB1_SHOT}/${mode}.png`, Buffer.from((await S("Page.captureScreenshot", { format: "png" })).data, "base64"));
  const in1 = await pixel(sIn), out1 = await pixel(sOut);
  check(`${mode}: a pixel inside the outline is the slab's (not the background)`, !sameRgb(in1, bg), `${in1} vs ${bg}`);
  check(`${mode}: a pixel outside is still the background`, sameRgb(out1, bg), `${out1} vs ${bg}`);

  // ---- Esc steps back ----
  for (let i = 0; i < 3; i++) await click(await screen(11000 + 1000 * i, -9000 + 500 * (i % 2), Z));
  await waitFor(`${mode}: 3 corners`, 5000, async () => (await points()) === 3).catch(() => {});
  await key("Escape", "Escape", 27);
  await waitFor(`${mode}: 2 corners`, 3000, async () => (await points()) === 2).catch(() => {});
  check(`${mode}: Esc takes the last corner back, still placing`, (await points()) === 2 && (await B("return b.placing.value.active;")) === true, `${await points()}`);
  await key("Escape", "Escape", 27);
  await key("Escape", "Escape", 27);
  await waitFor(`${mode}: no corner`, 3000, async () => (await points()) === 0).catch(() => {});
  check(`${mode}: Esc twice more: no corner, still placing`, (await points()) === 0 && (await B("return b.placing.value.active;")) === true);
  await key("Escape", "Escape", 27);
  await waitFor(`${mode}: placing off`, 3000, () => B("return b.placing.value?.active === false;")).catch(() => {});
  check(`${mode}: Esc once more: placing off`, (await B("return b.placing.value?.active;")) === false);

  // ---- the column on the grid's crossing ----
  const eg = await B("return b.gridAdd('G', [12000, 15000], [-9000, -6000]);");
  check(`${mode}: a grid added`, eg === null, String(eg));
  await B(`return b.setPlacing({ storey: '${FOND}', kind: 'column' });`);
  await waitFor(`${mode}: placing column`, 5000, () => B("return b.placing.value?.active === true && b.placing.value.kind === 'column';"));
  await settle();
  const r1 = await rev();
  const sc = await screen(12030, -8960, Z);
  await move(sc, false);
  g = await gl();
  check(`${mode}: the hover snaps to the grid crossing (12000, -9000)`, g.hover?.snap === "axis crossing" && g.hover.at[0] === 12000 && g.hover.at[1] === -9000, JSON.stringify(g.hover));
  await click(sc, false);
  await waitFor(`${mode}: rev + 1 after the column`, 5000, async () => (await rev()) === r1 + 1).catch(() => {});
  g = await gl();
  const col = g.placed ? await B(`return b.element('${g.placed}');`) : null;
  check(`${mode}: the column lands exactly on the crossing`, col?.class === "IfcColumn" && col.placement.origin[0] === 12000 && col.placement.origin[1] === -9000,
    JSON.stringify(col?.placement?.origin));
  await key("Escape", "Escape", 27);

  // ---- one axis more, from the page ----
  await page("levels-grids");
  await evalJs("document.querySelector('[data-bb=grids-tab]')?.click()");
  await waitFor(`${mode}: the axis buttons`, 3000, () => evalJs("!!document.querySelector('[data-bb=axis-add]')")).catch(() => {});
  check(`${mode}: "Add an axis" is enabled`, (await evalJs("document.querySelector('[data-bb=axis-add]')?.disabled === false")) === true);
  const ax2 = (await B("return b.grids();"))[0]?.axes.find((a) => a.tag === "2");
  await evalJs(`document.querySelector('[data-bb-axis="${ax2?.id}"]')?.click()`); // the chosen axis: 2
  await raf2();
  const u2 = await undoN();
  await evalJs("document.querySelector('[data-bb=axis-add]')?.click()");
  const gr = await waitFor(`${mode}: 5 axes`, 5000, async () => { const x = await B("return b.grids();"); return x[0]?.axes.length === 5 ? x : null; }).catch(() => B("return b.grids();"));
  const ax3 = gr[0]?.axes.find((a) => a.tag === "3");
  check(`${mode}: the page adds axis 3 at x 18000 (after axis 2, the same spacing), one step`,
    ax3 && ax3.start_mm[0] === 18000 && ax3.end_mm[0] === 18000 && (await undoN()) === u2 + 1, JSON.stringify(gr[0]?.axes.map((a) => [a.tag, a.start_mm])));
  await page(null);
  await waitFor(`${mode}: 5 axes drawn`, 8000, async () => JSON.parse(await evalJs("JSON.stringify(window.__bbGrids ?? null)"))?.axes === 5).catch(() => {});
  check(`${mode}: the plan draws the 5 axes`, JSON.parse(await evalJs("JSON.stringify(window.__bbGrids ?? null)"))?.axes === 5);
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
  const consoleLog = [], exceptions = [];
  cdp.on((msg) => {
    if (msg.sessionId !== sessionId) return;
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
  for (const mode of ["native", "wasm"]) await run(mode, S, evalJs);

  const IGNORED = [
    /Synchronous XMLHttpRequest on the main thread is deprecated/,
    /404 \(Not Found\) http:\/\/localhost:\d+\/favicon\.ico$/,
  ];
  const errors = consoleLog.filter((m) => /error|warn/.test(m.level) && !IGNORED.some((r) => r.test(m.text)));
  check("no uncaught page error", exceptions.length === 0, exceptions.join(" | "));
  check("no console error or warning", errors.length === 0, errors.map((m) => m.text).join(" | "));
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
