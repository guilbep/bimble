// The Levels & grids page, grid axes in plans and the setting-out panel in the lab (ticket G2):
//   node ccore/test/lab_levels.mjs
// Starts out/tcc/bimble-ccore (or $HOST_BIN) on a copy of golden/frame.json, copies the wasm build
// (scripts/copy-ccore.mjs), Vite from web-ui/ and headless /usr/bin/chromium over raw CDP. Twice,
// on the native host (/?native=1&gl=1&page=levels-grids) and on ccore.wasm (/?ccore=1&gl=1&page=levels-grids):
//   Levels (pages/LevelsGrids.vue, verbs of src/verbs_levels.c): the rows are the storeys highest
//     first; a level added by the page's form is one history step and a row; renamed in its row;
//     Niveau 2 set from +3.00 to +3.30 in its row moves its elements 300 mm up in the 3D mesh
//     (window.__bbGl.elementBox) and the row says +3.30; undo puts both back; × on Fondations (12
//     elements) shows the verb's refusal and keeps the row; × on the new empty level removes it.
//   Grids: the page's form adds a 3 x 2 grid (x 0 6000 12000, y 0 5000): 5 axes tagged 1 2 3 A B;
//     in the ground plan (gl/grids.ts) 5 axes and 10 bubbles are drawn, ink pixels lie on axis 2
//     where the viewport projects (6000, 2500) (window.__bbGl.toScreen) and none at (3000, 2500);
//     a bubble "2" sits just past the axis' end; axis 2 moved to 7000 by the page moves the pixels;
//     renamed to "2a" the bubble says so; the grid removed by the page leaves no axis drawn.
//   Setting out (shell/SettingOutSide.vue): EPSG:2056, east 2600000, north 1200000, height 432.1,
//     angle 30 typed in the panel: query north and coordinates read them back, the panel's base
//     point text is the status bar's "E 2'600'000.00 N 1'200'000.00 · 432.10 m (LV95)", project
//     north (viewcube.ts projectNorth) is 30° in radians.
//   Files: the model saved (native: save_as {path}; wasm: ccoreSaveBytes) and opened again has the
//     same storeys, grids and georeference; the IFC export holds IFCGRID and IFCGRIDAXIS('1',.
//   Read-only: golden/ifc/views/fixtures/storeys_grid.ifc opened read-only lists its 4 levels and 3
//     axes (A, B, 1) with no input, and its plan draws its axes.
// Before G2 the page's inputs were disabled and it had no add form (the add step fails), the grids
// tab listed nothing, no grid axis was drawn (no window.__bbGrids) and the setting-out fields were
// disabled. No uncaught page error; no console error or warning but the sync-XHR deprecation and
// the favicon 404. Prints "N passed, M failed"; exits non-zero on a failure. Kills all it starts.
import { spawn, execFileSync } from "node:child_process";
import { copyFileSync, existsSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { CHROMIUM, port, vitePort } from "./ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = resolve(CCORE, "../web-ui");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("lab_levels");
const VITE = vitePort("lab_levels");
const TOKEN = "lablevels-token";
const PAGES = {
  native: `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}&page=levels-grids`,
  wasm: `http://localhost:${VITE}/?ccore=1&gl=1&page=levels-grids`,
};
const RO_IFC = join(CCORE, "golden/ifc/views/fixtures/storeys_grid.ifc");
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-levels-"));
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

/** A host call from Node (native mode only): the answer's status and text. */
async function host(op, name, body = "") {
  const r = await fetch(`http://127.0.0.1:${PORT}/${op}/${encodeURIComponent(name)}?t=${TOKEN}`, { method: "POST", headers: { "Content-Type": "text/plain" }, body });
  return { status: r.status, text: await r.text() };
}

async function levels(mode, S, evalJs) {
  const B = (body) => evalJs(`(async () => { const b = await import('/src/bimble.ts'); ${body} })()`);
  const P = (body) => evalJs(`(async () => { const p = await import('/src/pages/index.ts'); const m = await import('/src/shell/send.ts'); ${body} })()`);
  const raf2 = () => evalJs("new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)))");
  const frames = () => evalJs("window.__bbGl ? window.__bbGl.frames : 0");
  const settle = async () => { const f0 = await frames(); await waitFor("a new frame", 3000, async () => (await frames()) > f0).catch(() => {}); for (let i = 0; i < 3; i++) await raf2(); };
  /** Sets an input's value and sends input and change, as typing then leaving the field does. */
  const type = (sel, value) => evalJs(`(() => { const el = document.querySelector(${JSON.stringify(sel)}); if (!el) return false;
    el.value = ${JSON.stringify(value)}; el.dispatchEvent(new Event('input', { bubbles: true })); el.dispatchEvent(new Event('change', { bubbles: true })); return true; })()`);
  const click = (sel) => evalJs(`(() => { const el = document.querySelector(${JSON.stringify(sel)}); if (!el) return false; el.click(); return true; })()`);
  const rowsDom = () => evalJs(`[...document.querySelectorAll('[data-bb=levels-table] tbody tr')].map((tr) => {
    const n = tr.querySelector('[data-bb=level-name]'), m = tr.querySelector('[data-bb=level-mm]');
    return [n ? (n.value ?? n.textContent).trim() : null, m ? (m.value ?? m.textContent).trim() : null, tr.dataset.bbLevel]; })`);
  const storeys = () => B("return b.query('storeys');");
  const undoN = async () => (await B("return b.query('history');")).undo.length;
  const errText = () => evalJs("document.querySelector('[data-bb=levels-error]')?.textContent.trim() ?? null");
  /** Ink pixels (dark, opaque) on the overlay in a client-px box [x, y, w, h]. */
  const inkPx = (box) => evalJs(`(() => { const o = document.querySelector('[data-bb-overlay]'); const c = o.getContext('2d');
    const r = document.getElementById('bimble').getBoundingClientRect(); const dpr = window.devicePixelRatio || 1; const b = ${JSON.stringify(box)};
    const x0 = Math.max(0, Math.floor((b[0] - r.left) * dpr)), y0 = Math.max(0, Math.floor((b[1] - r.top) * dpr));
    const d = c.getImageData(x0, y0, Math.max(1, Math.round(b[2] * dpr)), Math.max(1, Math.round(b[3] * dpr))).data; let n = 0;
    for (let i = 0; i < d.length; i += 4) if (d[i + 3] > 100 && d[i] < 90 && d[i + 1] < 90 && d[i + 2] < 90) n++;
    return n; })()`);
  const toScreen = (x, y, z = 0) => evalJs(`JSON.stringify(window.__bbGl.toScreen(${x}, ${y}, ${z}))`).then((t) => JSON.parse(t));
  const grids = () => evalJs("JSON.stringify(window.__bbGrids ?? null)").then((t) => JSON.parse(t));
  /** Shows a page, or the view (null), through the shell (UX1: shell/send.ts); fails the run when the shell refuses. */
  const page = async (id) => {
    const ok = await P(`return m.send(${JSON.stringify(id === null ? { do: "showView" } : { do: "showPage", page: id })}) && p.currentPage.value === ${JSON.stringify(id)};`);
    if (!ok) throw new Error(`${mode}: the shell did not show ${id ?? "the view"}`);
    await settle();
  };

  await S("Page.navigate", { url: PAGES[mode] });
  await waitFor(`${mode}: data-bb-ready`, 30000, () => evalJs("document.documentElement.dataset.bbReady === '1'"));
  await waitFor(`${mode}: the levels table`, 10000, () => evalJs("!!document.querySelector('[data-bb=levels-table] tbody tr')"));

  // ---- levels ----
  const st0 = await storeys();
  const byName = (s, n) => s.find((x) => x.name === n);
  const r0 = await rowsDom();
  check(`${mode}: editable model: the page says so`, (await evalJs("document.querySelector('[data-bb=levels-grids]').dataset.bbEditable")) === "1");
  check(`${mode}: rows = the storeys, highest first`, JSON.stringify(r0.map((r) => r[2])) === JSON.stringify([...st0].reverse().map((s) => s.id)) &&
    JSON.stringify(r0.map((r) => r[0])) === JSON.stringify(["Niveau 2", "Niveau 1", "Fondations"]), JSON.stringify(r0));
  const u0 = await undoN();
  const added = (await type("[data-bb=level-add-name]", "Niveau 3")) && (await type("[data-bb=level-add-mm]", "+6.00")) && (await click("[data-bb=level-add-go]"));
  check(`${mode}: the add form is there`, added);
  const st1 = await waitFor(`${mode}: a fourth level`, 5000, async () => { const s = await storeys(); return s.length === 4 ? s : null; }).catch(() => storeys());
  const n3 = byName(st1, "Niveau 3");
  check(`${mode}: level added by the page: Niveau 3 at 6000 mm, one history step`, n3?.elevation === 6000 && (await undoN()) === u0 + 1, JSON.stringify(st1));
  await waitFor(`${mode}: four rows`, 5000, async () => (await rowsDom()).length === 4).catch(() => {});
  check(`${mode}: the new row is first, +6.00`, JSON.stringify((await rowsDom())[0]?.slice(0, 2)) === JSON.stringify(["Niveau 3", "+6.00"]), JSON.stringify(await rowsDom()));
  await type(`[data-bb-level="${n3?.id}"] [data-bb=level-name]`, "Toiture");
  const st2 = await waitFor(`${mode}: renamed`, 5000, async () => { const s = await storeys(); return byName(s, "Toiture") ? s : null; }).catch(() => storeys());
  check(`${mode}: renamed in its row: Toiture`, byName(st2, "Toiture")?.id === n3?.id && !byName(st2, "Niveau 3"), JSON.stringify(st2.map((s) => s.name)));

  // Niveau 2 to +3.30: its elements move up 300 mm in the mesh.
  const n2 = byName(st2, "Niveau 2");
  const els = await B(`return b.query('elements', { limit: 500 }).filter((e) => e.storey === ${JSON.stringify(n2.id)}).map((e) => e.id);`);
  const el = els[0];
  const box0 = await evalJs(`JSON.stringify(window.__bbGl.elementBox(${JSON.stringify(el)}))`).then(JSON.parse);
  const u1 = await undoN();
  await type(`[data-bb-level="${n2.id}"] [data-bb=level-mm]`, "+3.30");
  await waitFor(`${mode}: Niveau 2 at 3300`, 5000, async () => byName(await storeys(), "Niveau 2")?.elevation === 3300).catch(() => {});
  check(`${mode}: Niveau 2 set to +3.30 by its row: elevation 3300, one step`, byName(await storeys(), "Niveau 2")?.elevation === 3300 && (await undoN()) === u1 + 1);
  const box1 = await waitFor(`${mode}: the mesh follows`, 8000, async () => {
    const b1 = JSON.parse(await evalJs(`JSON.stringify(window.__bbGl.elementBox(${JSON.stringify(el)}))`));
    return b1 && Math.abs(b1.min[2] - box0.min[2] - 300) < 0.01 ? b1 : null;
  }).catch(() => null);
  check(`${mode}: the 3D model follows: an element of Niveau 2 is 300 mm higher`, !!box1, `${els.length} elements; ${JSON.stringify(box0)}`);
  await waitFor(`${mode}: row +3.30`, 5000, async () => (await rowsDom()).some((r) => r[2] === n2.id && r[1] === "+3.30")).catch(() => {});
  check(`${mode}: the row says +3.30`, (await rowsDom()).some((r) => r[2] === n2.id && r[1] === "+3.30"), JSON.stringify(await rowsDom()));
  await B("return b.undo();");
  await waitFor(`${mode}: undo`, 5000, async () => byName(await storeys(), "Niveau 2")?.elevation === 3000).catch(() => {});
  const box2 = await waitFor(`${mode}: the mesh back`, 8000, async () => {
    const b2 = JSON.parse(await evalJs(`JSON.stringify(window.__bbGl.elementBox(${JSON.stringify(el)}))`));
    return b2 && Math.abs(b2.min[2] - box0.min[2]) < 0.01 ? b2 : null;
  }).catch(() => null);
  await waitFor(`${mode}: row +3.00`, 5000, async () => (await rowsDom()).some((r) => r[2] === n2.id && r[1] === "+3.00")).catch(() => {});
  check(`${mode}: undo: 3000 mm, the element back, the row +3.00`, byName(await storeys(), "Niveau 2")?.elevation === 3000 && !!box2 &&
    (await rowsDom()).some((r) => r[2] === n2.id && r[1] === "+3.00"));

  // Refused: Fondations holds 12 elements.
  const fd = byName(st2, "Fondations");
  const u2 = await undoN();
  await click(`[data-bb-level="${fd.id}"] [data-bb=level-remove]`);
  await waitFor(`${mode}: the refusal`, 3000, async () => !!(await errText())).catch(() => {});
  check(`${mode}: × on Fondations: the verb's refusal shown, the level kept, no step`,
    (await errText()) === "level_remove: the level Fondations holds 12 elements: move or delete them first" && !!byName(await storeys(), "Fondations") && (await undoN()) === u2,
    String(await errText()));
  await click(`[data-bb-level="${n3.id}"] [data-bb=level-remove]`);
  await waitFor(`${mode}: Toiture removed`, 5000, async () => (await storeys()).length === 3).catch(() => {});
  await waitFor(`${mode}: three rows`, 5000, async () => (await rowsDom()).length === 3).catch(() => {});
  check(`${mode}: × on the empty level removes it (row and storey)`, (await storeys()).length === 3 && (await rowsDom()).length === 3 && (await errText()) === null);

  // ---- grids ----
  await click("[data-bb=grids-tab]");
  await waitFor(`${mode}: grids tab`, 3000, () => evalJs("!!document.querySelector('[data-bb=grid-add]')")).catch(() => {});
  check(`${mode}: grids tab: no grid yet, the add form`, (await evalJs("!!document.querySelector('[data-bb=grids-empty]') && !!document.querySelector('[data-bb=grid-add]')")) === true);
  await type("[data-bb=grid-add-name]", "Grid G2");
  await type("[data-bb=grid-add-x]", "0 6000 12000");
  await type("[data-bb=grid-add-y]", "0 5000");
  await click("[data-bb=grid-add-go]");
  const g1 = await waitFor(`${mode}: a grid`, 5000, async () => { const g = await B("return b.grids();"); return g.length ? g : null; }).catch(() => []);
  const grid = g1[0];
  check(`${mode}: grid added by the page: 5 axes 1 2 3 A B`, grid?.name === "Grid G2" && JSON.stringify(grid.axes.map((a) => a.tag)) === JSON.stringify(["1", "2", "3", "A", "B"]), JSON.stringify(g1));
  await waitFor(`${mode}: 5 axis rows`, 3000, async () => (await evalJs("document.querySelectorAll('[data-bb=grid-axes] tbody tr').length")) === 5).catch(() => {});
  check(`${mode}: the page lists the 5 axes`, (await evalJs("document.querySelectorAll('[data-bb=grid-axes] tbody tr').length")) === 5);
  const ax2 = grid?.axes.find((a) => a.tag === "2");

  // The ground plan, the page closed: the axes drawn.
  const ov = await B("return b.openViews.value;");
  const plan1 = ov.views.find((v) => v.type === "plan" && v.look === "down" && v.storey === byName(st2, "Niveau 1").id);
  await page(null);
  const eo = await B(`return b.openView(${JSON.stringify(plan1?.id)});`);
  check(`${mode}: the ground plan opens`, !eo, String(eo));
  await waitFor(`${mode}: the plan camera`, 10000, async () => (await evalJs("window.__bbGl.camera && window.__bbGl.camera.mode")) === "plan").catch(() => false);
  await B("return b.runAction('fit');");
  await waitFor(`${mode}: 5 axes drawn`, 8000, async () => (await grids())?.axes === 5).catch(() => {});
  await settle();
  const gs = await grids();
  check(`${mode}: the plan draws 5 axes and 10 bubbles`, gs?.axes === 5 && gs.bubbles.length === 10, JSON.stringify(gs && { axes: gs.axes, n: gs.bubbles.length }));
  const at = async (x, y) => { const p = await toScreen(x, y, 0); return p ? inkPx([p[0] - 2, p[1] - 30, 5, 60]) : -1; };
  const on6000 = await at(6000, 2500), free = await at(3000, 2500);
  check(`${mode}: ink on axis 2 where (6000, 2500) is projected`, on6000 > 10, `${on6000} px`);
  check(`${mode}: control: no ink at (3000, 2500)`, free === 0, `${free} px`);
  // The bubble "2" just past the axis' start (the overlay's px; toScreen's are the page's).
  const rect = await evalJs("JSON.stringify(document.getElementById('bimble').getBoundingClientRect())").then(JSON.parse);
  const s2 = await toScreen(ax2.start_mm[0], ax2.start_mm[1], 0);
  const bub = gs?.bubbles.find((b) => b.tag === "2" && Math.hypot(b.at[0] + rect.left - s2[0], b.at[1] + rect.top - s2[1]) < 13);
  check(`${mode}: a bubble "2" at the axis' start (12 px past it)`, !!bub, JSON.stringify(gs?.bubbles.filter((b) => b.tag === "2")));
  // A bubble in sight: its circle and tag drawn in ink.
  const seen = gs?.bubbles.find((b) => b.at[0] > 15 && b.at[1] > 15 && b.at[0] < rect.width - 15 && b.at[1] < rect.height - 15);
  check(`${mode}: a bubble in sight is drawn (ink in it)`, seen ? (await inkPx([seen.at[0] + rect.left - 13, seen.at[1] + rect.top - 13, 26, 26])) > 20 : false, JSON.stringify(seen));

  // Move axis 2 to 7000 and rename it, by the page.
  await page("levels-grids");
  await click("[data-bb=grids-tab]");
  await waitFor(`${mode}: the axis rows`, 3000, () => evalJs(`!!document.querySelector('[data-bb-axis="${ax2.id}"] [data-bb=axis-pos]')`)).catch(() => {});
  const u3 = await undoN();
  await type(`[data-bb-axis="${ax2.id}"] [data-bb=axis-pos]`, "7000");
  await waitFor(`${mode}: axis 2 at 7000`, 5000, async () => (await B("return b.grids();"))[0]?.axes.find((a) => a.tag === "2")?.start_mm[0] === 7000).catch(() => {});
  const ax2b = (await B("return b.grids();"))[0]?.axes.find((a) => a.id === ax2.id);
  check(`${mode}: axis 2 moved to x 7000 (both ends), one step`, ax2b?.start_mm[0] === 7000 && ax2b?.end_mm[0] === 7000 && ax2b.start_mm[1] === ax2.start_mm[1] && (await undoN()) === u3 + 1, JSON.stringify(ax2b));
  await waitFor(`${mode}: the axis row`, 3000, () => evalJs(`!!document.querySelector('[data-bb-axis="${ax2.id}"] [data-bb=axis-tag]')`)).catch(() => {});
  await type(`[data-bb-axis="${ax2.id}"] [data-bb=axis-tag]`, "2a");
  await waitFor(`${mode}: tag 2a`, 5000, async () => (await B("return b.grids();"))[0]?.axes.some((a) => a.tag === "2a")).catch(() => {});
  check(`${mode}: axis renamed 2a`, (await B("return b.grids();"))[0]?.axes.find((a) => a.id === ax2.id)?.tag === "2a");
  await page(null);
  await waitFor(`${mode}: redrawn`, 5000, async () => (await grids())?.bubbles.some((b) => b.tag === "2a")).catch(() => {});
  await settle();
  const on7000 = await at(7000, 2500), gone6000 = await at(6000, 2500);
  check(`${mode}: the plan follows: ink at 7000, none left at 6000`, on7000 > 10 && gone6000 === 0, `${on7000} / ${gone6000} px`);
  check(`${mode}: the bubbles say 2a`, (await grids())?.bubbles.filter((b) => b.tag === "2a").length === 2);

  // ---- setting out ----
  await evalJs("(async () => { const t = await import('/src/shell/tabs.ts'); t.activeTab.value = 'settingOut'; })()");
  await waitFor(`${mode}: the setting-out panel`, 5000, () => evalJs("!!document.querySelector('[data-bb=settingout-crs]')")).catch(() => {});
  const fields = [["settingout-crs", "EPSG:2056"], ["settingout-east_m", "2600000"], ["settingout-north_m", "1200000"], ["settingout-height_m", "432.1"], ["settingout-rotation_deg", "30"]];
  const u4 = await undoN();
  for (const [k, v] of fields) {
    await waitFor(`${mode}: ${k}`, 3000, () => evalJs(`!!document.querySelector('[data-bb=${k}]')`)).catch(() => {});
    const n0 = await undoN();
    await type(`[data-bb=${k}]`, v);
    await waitFor(`${mode}: ${k} applied`, 5000, async () => (await undoN()) === n0 + 1).catch(() => {});
  }
  const north = await B("return b.query('north');");
  const co = await B("return b.query('coordinates', { at: [0, 0, 0] });");
  check(`${mode}: georef set by the panel (5 steps) and read back: north`, north?.crs === "EPSG:2056" && north.rotation_deg === 30 && (await undoN()) === u4 + 5, JSON.stringify(north));
  check(`${mode}: georef read back: coordinates of the origin`, JSON.stringify(co?.survey) === JSON.stringify([2600000, 1200000, 432.1]), JSON.stringify(co));
  await waitFor(`${mode}: the base point text`, 3000, async () => (await evalJs("document.querySelector('[data-bb=settingout-base]')?.textContent.trim()"))?.startsWith("E ")).catch(() => {});
  const baseText = await evalJs("document.querySelector('[data-bb=settingout-base]')?.textContent.trim()");
  check(`${mode}: the panel's base point text`, baseText === "E 2'600'000.00 N 1'200'000.00 · 432.10 m (LV95)", baseText);
  check(`${mode}: the panel shows the values`, (await evalJs("document.querySelector('[data-bb=settingout-rotation_deg]').value")) === "30" &&
    (await evalJs("document.querySelector('[data-bb=settingout-crs]').value")) === "EPSG:2056");
  const pn = await evalJs("(async () => (await import('/src/viewcube.ts')).projectNorth())()");
  check(`${mode}: project north on the view cube = 30° (radians)`, Math.abs(pn - Math.PI / 6) < 1e-12, String(pn));
  const bad = await type("[data-bb=settingout-rotation_deg]", "400");
  await waitFor(`${mode}: the refusal`, 3000, () => evalJs("!!document.querySelector('[data-bb=settingout-error]')")).catch(() => {});
  check(`${mode}: angle 400: the verb's refusal shown`, bad && (await evalJs("document.querySelector('[data-bb=settingout-error]')?.textContent.trim()")) === "bad arguments: `rotation_deg`: at most 360 degrees");
  await evalJs("(async () => { const t = await import('/src/shell/tabs.ts'); t.activeTab.value = 'view'; })()").catch(() => {});

  // ---- files: save and open again, IFC export ----
  const before = JSON.stringify(await B("return [b.query('storeys'), b.grids(), b.query('north')];"));
  let ifc = "";
  if (mode === "native") {
    const path = join(tmp, "g2.bimble");
    const sv = await host("action", "save_as", JSON.stringify({ path }));
    check(`${mode}: save_as g2.bimble`, sv.status === 200 && existsSync(path), sv.text);
    const ex = await host("action", "export_ifc4", JSON.stringify({ path: join(tmp, "g2.ifc") }));
    ifc = existsSync(join(tmp, "g2.ifc")) ? readFileSync(join(tmp, "g2.ifc"), "utf8") : `${ex.status} ${ex.text}`;
    const op = await host("action", "open", JSON.stringify({ path, discard: true }));
    check(`${mode}: g2.bimble opens`, op.status === 200, op.text);
  } else {
    const r = await evalJs(`(async () => { const c = await import('/src/ccore.ts'); const s = c.ccoreSaveBytes(); const x = c.ccoreExportIfc('ifc4');
      await c.ccoreOpenFile(new File([s.bytes], s.name), true); return { name: s.name, n: s.bytes.length, ifc: x.text }; })()`);
    check(`${mode}: saved (${r?.n} bytes) and opened again`, r?.n > 0, JSON.stringify(r && { name: r.name, n: r.n }));
    ifc = r?.ifc ?? "";
  }
  await waitFor(`${mode}: the model again`, 8000, async () => (await B("return b.grids();")).length === 1).catch(() => {});
  const after = JSON.stringify(await B("return [b.query('storeys'), b.grids(), b.query('north')];"));
  check(`${mode}: saved and opened again: the same storeys, grids (ids too) and georeference`, after === before, `\n  before ${before}\n  after  ${after}`);
  check(`${mode}: the IFC export holds the IfcGrid and its axes`, ifc.includes("=IFCGRID('") && ifc.includes("=IFCGRIDAXIS('1',") && ifc.includes("=IFCGRIDAXIS('2a',"), ifc.slice(0, 200));

  // ---- remove the grid by the page ----
  await page("levels-grids");
  await click("[data-bb=grids-tab]");
  await waitFor(`${mode}: the grid's remove`, 3000, () => evalJs("!!document.querySelector('[data-bb=grid-remove]')")).catch(() => {});
  await click("[data-bb=grid-remove]");
  await waitFor(`${mode}: no grid`, 5000, async () => (await B("return b.grids();")).length === 0).catch(() => {});
  check(`${mode}: grid removed by the page`, (await B("return b.grids();")).length === 0);
  await page(null);
  const pv = (await B("return b.openViews.value;")).views.find((v) => v.type === "plan" && v.look === "down" && v.storey === byName(st2, "Niveau 1").id);
  await B(`return b.openView(${JSON.stringify(pv?.id)});`);
  await waitFor(`${mode}: no axis drawn`, 5000, async () => (await grids())?.axes === 0).catch(() => {});
  await settle();
  check(`${mode}: no axis drawn and no ink at 7000`, (await grids())?.axes === 0 && (await at(7000, 2500)) === 0);

  // ---- a read-only IFC: the list without edit controls, its axes in its plan ----
  const ifcText = readFileSync(RO_IFC, "utf8");
  if (mode === "native") {
    const op = await host("action", "open", JSON.stringify({ path: RO_IFC, discard: true }));
    check(`${mode}: storeys_grid.ifc opens read-only`, op.status === 200, op.text);
  } else {
    await evalJs(`(async () => { const c = await import('/src/ccore.ts'); await c.ccoreOpenFile(new File([${JSON.stringify(ifcText)}], 'storeys_grid.ifc'), true); })()`);
  }
  await waitFor(`${mode}: the IFC`, 10000, async () => (await B("return b.state.value.file;")) === "storeys_grid.ifc").catch(() => {});
  await page("levels-grids");
  await waitFor(`${mode}: read-only rows`, 5000, async () => (await rowsDom()).length === 4).catch(() => {});
  const ro = await rowsDom();
  check(`${mode}: read-only: its 4 levels highest first (grid_axes), no input, no add form`,
    JSON.stringify(ro.map((r) => r[0])) === JSON.stringify(["Roof", "First", "Ground", "Basement"]) &&
    (await evalJs("document.querySelector('[data-bb=levels-grids]').dataset.bbEditable")) === "0" &&
    (await evalJs("document.querySelectorAll('[data-bb=levels-grids] input:not([type=checkbox]), [data-bb=level-add], [data-bb=level-remove]').length")) === 0, JSON.stringify(ro));
  await click("[data-bb=grids-tab]");
  await waitFor(`${mode}: read-only axes`, 3000, async () => (await evalJs("document.querySelectorAll('[data-bb=grid-axes] tbody tr').length")) === 3).catch(() => {});
  check(`${mode}: read-only: the 3 axes A, B, 1 listed`, JSON.stringify(await evalJs("[...document.querySelectorAll('[data-bb=grid-axes] tbody tr td:first-child')].map((t) => t.textContent.trim())")) === JSON.stringify(["A", "B", "1"]));
  await page(null);
  const rv = (await B("return b.openViews.value;")).views.find((v) => v.type === "plan" && v.look === "down");
  await B(`return b.openView(${JSON.stringify(rv?.id)});`);
  await waitFor(`${mode}: its axes drawn`, 8000, async () => (await grids())?.axes === 3).catch(() => {});
  check(`${mode}: read-only plan: its 3 axes drawn`, (await grids())?.axes === 3, JSON.stringify((await grids())?.axes));
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
  for (const mode of ["native", "wasm"]) await levels(mode, S, evalJs);

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
  const badResp = failedResp.filter((r) => !/^404 http:\/\/localhost:\d+\/favicon\.ico$/.test(r) &&
    !new RegExp(`^400 http://127\\.0\\.0\\.1:${PORT}/(verb/(level_remove|georef_set)|query/(storeys|materials|project|element))\\?`).test(r));
  check("no failed response but the refusals asked for and the model queries of a read-only scene", badResp.length === 0, badResp.join(" | "));
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
