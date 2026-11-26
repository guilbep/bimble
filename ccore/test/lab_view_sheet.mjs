// The View, Sheets and Annotate tabs' new tools in the lab, each through its whole gesture
// (ticket VIEWSHEET, ADR 0076):
//   CCORE_PORT_BASE=auto node ccore/test/lab_view_sheet.mjs
// Starts out/tcc/bimble-ccore (or $HOST_BIN) on a copy of golden/frame.json (72 elements), with
// HOME and BIMBLE_CONFIG_DIR in temporary folders, Vite from web-ui/ and one headless Chromium over
// raw CDP, then opens /?native=1&gl=1&tab=view and works:
//   View → New view…: the dialog (kind, level, name, scale; Elevation listed "not available yet"),
//     a plan of Niveau 2 named "Formwork 2" at 1:50: one view_add (one undo step), the view opens;
//   View → Section: off in 3D with its reason; in the plan, two clicks (real mouse events) draw the
//     line (2000, 5000) → (16000, 5000): a section looking south through (9000, 5000) is listed, the
//     plan stays shown and its drawing (view_svg) changes (the mark); Esc drops the first point,
//     then stops the tool;
//   Sheets → Place view…: off without a sheet, with its reason; with a new sheet the dialog lists
//     the views, a click places the section at the paper's centre (sheet_layout, sheet_svg grows,
//     the placed view selected); placed again: a second copy (Rust's layout places a view again);
//   Sheets → Revision…: off without a sheet; "R7" applied: the sheet's {revision}, printed in the
//     sheet's SVG title block; one undo step;
//   Annotate → Text type, Dimension type: two selects of the standards' types (25 text types),
//     the choice read back by query annotation_types; a text placed then in the plan by the
//     annotate tool has kind "Arial 5 mm black" and 5 mm.
//   And no tool of the three tabs is dead: each has an action, a screen or "not yet".
// Before VIEWSHEET the four tools had `action: null` (drawn "not yet", data-missing="action") and the
// Types group two dead "not yet" tools: the first checks of each part fail there (run on 8d4ac2a).
// Each control's time from the click (or the change) to the first frame after its effect is printed
// ("time <control> <ms>"); the test does not fail on time. No uncaught page error, no console error
// or warning but the sync-XHR deprecation and the favicon 404. Prints "N passed, M failed"; exits
// non-zero on a failure. Kills all it starts.
import { spawn, execFileSync } from "node:child_process";
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { CHROMIUM, port, vitePort } from "./ports.mjs";
import { hostEnv } from "./lib/host_env.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = resolve(CCORE, "../web-ui");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("lab_view_sheet");
const VITE = vitePort("lab_view_sheet");
const TOKEN = "labviewsheet-token";
const PAGE = `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}&tab=view`;
const SHOTS = process.env.VIEWSHEET_SHOTS ?? ""; // a folder: screenshots at 1280 px
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-viewsheet-"));
const procs = [];
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail && !ok ? `: ${detail}` : ""}`);
};
const times = [];

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

async function tab(S, evalJs) {
  const B = (body) => evalJs(`(async () => { const b = await import('/src/bimble.ts'); ${body} })()`);
  const rev = () => B("return b.state.value.rev;");
  const attr = (sel, a) => evalJs(`document.querySelector(${JSON.stringify(sel)})?.getAttribute(${JSON.stringify(a)}) ?? null`);
  const exists = (sel) => evalJs(`!!document.querySelector(${JSON.stringify(sel)})`);
  const timed = async (name, sel, act, cond) => {
    const ms = await evalJs(`(async () => {
      const b = await import('/src/bimble.ts'); const g = () => window.__bbGl;
      const el = document.querySelector(${JSON.stringify(sel)}); if (!el) return -1;
      const f0 = g()?.frames ?? 0, t0 = performance.now(); ${act};
      const raf = () => new Promise((r) => requestAnimationFrame(r));
      while (performance.now() - t0 < 5000) {
        await raf();
        let ok = false; try { ok = await (async () => (${cond}))(); } catch { ok = false; }
        if (ok) { if ((g()?.frames ?? 0) === f0) await raf(); return performance.now() - t0; }
      }
      return null; })()`);
    if (typeof ms === "number" && ms >= 0) times.push([name, ms]);
    return ms;
  };
  const CLICK = "el.click()";
  const pick = (v) => `el.value = ${JSON.stringify(v)}; el.dispatchEvent(new Event('change', { bubbles: true }))`;
  const tool = (g, m) => `[data-bb=ribbon] [data-bb=ribbon-tool-${g}-${m}]`;
  const setTab = (id) => evalJs(`(async () => { const t = await import('/src/shell/tabs.ts'); t.activeTab.value = ${JSON.stringify(id)}; await new Promise((r) => requestAnimationFrame(r)); })()`);
  const shot = async (name) => {
    if (!SHOTS) return;
    await sleep(300);
    const { data } = await S("Page.captureScreenshot", { format: "png" });
    writeFileSync(join(SHOTS, `${name}.png`), Buffer.from(data, "base64"));
  };
  /** A real mouse click (CDP) at the world point (x, y) of the plan shown. */
  const screenOf = (x, y) => evalJs(`(() => { const p = window.__bbGl.toScreen(${x}, ${y}, window.__bbPlanZ ?? 0); return p ? [p[0], p[1]] : null; })()`);
  const mouse = async (type, p) => S("Input.dispatchMouseEvent", { type, x: p[0], y: p[1], button: type === "mouseMoved" ? "none" : "left", clickCount: type === "mouseMoved" ? 0 : 1 });
  const clickAt = async (p) => { await mouse("mouseMoved", p); await mouse("mousePressed", p); await mouse("mouseReleased", p); };
  /** No tool of the tab shown is dead: an action, a screen, or "not yet". */
  const dead = () => evalJs("[...document.querySelectorAll('[data-bb=ribbon] .tool, [data-bb=ribbon] .rbox')].filter((e) => e.getAttribute('data-missing') === 'action' && !e.hasAttribute('data-not-yet') || (e.classList.contains('rbox') && e.hasAttribute('data-not-yet'))).map((e) => e.getAttribute('data-bb'))");
  const deadTools = () => evalJs("[...document.querySelectorAll('[data-bb=ribbon] [data-not-yet]')].map((e) => (e.querySelector('span')?.textContent ?? e.textContent).trim())");

  await S("Page.navigate", { url: PAGE });
  await waitFor("data-bb-ready", 30000, () => evalJs("document.documentElement.dataset.bbReady === '1'"));
  await waitFor("the View ribbon", 10000, () => exists(tool(5, 1)));
  await waitFor("the first frame", 10000, () => evalJs("(window.__bbGl?.frames ?? 0) > 0"));
  await waitFor("the actions", 10000, () => B("return b.actions.value.length > 0;"));
  await sleep(1500); // the page settles (the first frames, the mesh): the times below are the controls', not the load's

  // VIEWSHEET_PROBE=2: the session's first switch from 3D to a plan alone (no verb), before the controls
  if (process.env.VIEWSHEET_PROBE === "2") {
    const p = await evalJs(`(async () => {
      const b = await import('/src/bimble.ts'); const g = window.__bbGl; const raf = () => new Promise((r) => requestAnimationFrame(r)); const out = {};
      const d3 = b.openViews.value.active, pl = b.openViews.value.views.find((v) => v.type === 'plan')?.id;
      for (const k of [1, 2, 3]) {
        let t = performance.now(); b.openView(pl); out['open_call' + k] = performance.now() - t; const f1 = g.frames; let n = 0;
        while (g.frames === f1 && n++ < 300) await raf();
        out['first_plan_frame' + k] = performance.now() - t;
        b.openView(d3); for (let i = 0; i < 30; i++) await raf();
      }
      return JSON.stringify(out); })()`);
    console.log(`probe2 ${p}`);
  }

  // VIEWSHEET_PROBE=3: a CPU profile of the session's first switch from 3D to a plan (the top self times)
  if (process.env.VIEWSHEET_PROBE === "3") {
    await S("Profiler.enable");
    await S("Profiler.setSamplingInterval", { interval: 200 });
    await S("Profiler.start");
    const ms3 = await evalJs(`(async () => {
      const b = await import('/src/bimble.ts'); const raf = () => new Promise((r) => requestAnimationFrame(r));
      const pl = b.openViews.value.views.find((v) => v.type === 'plan')?.id; const t = performance.now();
      b.openView(pl);
      while (b.openViews.value.active !== pl && performance.now() - t < 5000) await raf();
      for (let i = 0; i < 3; i++) await raf();
      return performance.now() - t; })()`);
    const { profile } = await S("Profiler.stop");
    const self = new Map(), byId = new Map(profile.nodes.map((n) => [n.id, n]));
    const dt = profile.timeDeltas;
    profile.samples.forEach((id, i) => {
      const n = byId.get(id); const f = n.callFrame;
      const k = `${f.functionName || "(anon)"} ${f.url.replace(/^.*\/src\//, "src/").replace(/\?.*$/, "")}:${f.lineNumber + 1}`;
      self.set(k, (self.get(k) ?? 0) + (dt[i] ?? 0) / 1000);
    });
    console.log(`probe3 first 3D -> plan ${ms3.toFixed(1)} ms; top self times (ms):`);
    for (const [k, v] of [...self.entries()].sort((a, b) => b[1] - a[1]).slice(0, 14)) console.log(`probe3   ${v.toFixed(1)} ${k}`);
  }

  // ======== View → New view… ========
  check("View: New view… and Section are real tools (not 'not yet')", (await attr(tool(5, 1), "data-not-yet")) === null && (await attr(tool(5, 2), "data-not-yet")) === null);
  check("View: no dead control", JSON.stringify(await dead()) === "[]", JSON.stringify(await dead()));
  let ms = await timed("New view… (dialog)", tool(5, 1), CLICK, "!!document.querySelector('[data-bb=new-view-dialog]')");
  check("New view…: the dialog opens", ms !== null);
  check("New view…: Elevation listed 'not available yet', disabled", (await evalJs("document.querySelector('[data-bb=new-view-kind-elevation]')?.disabled === true && !!document.querySelector('[data-bb=new-view-dialog] [data-not-yet]')")));
  const lv2 = await B("return b.storeys.value.find((s) => s.name === 'Niveau 2')?.id;");
  await evalJs(`(() => { const el = document.querySelector('[data-bb=new-view-level]'); ${pick(lv2)}; })()`);
  await evalJs(`(() => { const el = document.querySelector('[data-bb=new-view-name]'); el.value = 'Formwork 2'; el.dispatchEvent(new Event('input', { bubbles: true })); })()`);
  await evalJs(`(() => { const el = document.querySelector('[data-bb=new-view-scale]'); el.value = '50'; el.dispatchEvent(new Event('input', { bubbles: true })); })()`);
  await shot("new-view-dialog-1280");
  const u0 = await B("return b.query('history').undo.length;");
  ms = await timed("New view… create", "[data-bb=new-view-create]", CLICK,
    "(() => { const ov = b.openViews.value; const v = ov.views.find((x) => x.name === 'Formwork 2'); return !!v && ov.active === v.id && v.scale === 50; })()");
  const fw = await B("return b.openViews.value.views.find((x) => x.name === 'Formwork 2') ?? null;");
  check("New view…: plan 'Formwork 2' of Niveau 2 at 1:50, opened and active", ms !== null && fw?.type === "plan" && fw?.storey === lv2 && fw?.scale === 50, JSON.stringify(fw));
  const u0b = await B("return b.query('history').undo.length;");
  check("New view…: one undo step, the dialog closed", u0b === u0 + 1 && !(await exists("[data-bb=new-view-dialog]")), `${u0} -> ${u0b}, dialog ${await evalJs("(async () => (await import('/src/dialogs/index.ts')).currentDialog.value)()")} ${await evalJs("document.querySelector('[data-bb=new-view-error]')?.textContent ?? ''")}`);

  // ======== View → Section ========
  // In 3D: off, with the reason on a click.
  const three = await B("return b.openViews.value.views.find((v) => v.type === 'three_d')?.id;");
  await B(`return b.openView(${JSON.stringify(three)});`);
  await waitFor("3D shown", 8000, async () => (await B("return b.viewClip()?.type ?? 'three_d';")) === "three_d");
  await waitFor("Section off in 3D", 5000, async () => (await attr(tool(5, 2), "aria-disabled")) === "true");
  await evalJs(`document.querySelector(${JSON.stringify(tool(5, 2))}).click()`);
  check("Section in 3D: off, its reason shown on a click", (await exists("[data-bb=off-tip]")) && /plan/i.test(await evalJs("document.querySelector('[data-bb=off-tip]')?.textContent ?? ''")));
  const plan1 = await B("return b.openViews.value.views.find((v) => v.type === 'plan' && v.look === 'down' && v.name.includes('Niveau 1'))?.id ?? b.openViews.value.views.find((v) => v.type === 'plan' && v.look === 'down')?.id;");
  await B(`return b.openView(${JSON.stringify(plan1)});`);
  await waitFor("the plan shown", 8000, async () => (await B("return b.viewClip()?.type;")) === "plan");
  await evalJs("(async () => { const b = await import('/src/bimble.ts'); window.__bbPlanZ = b.viewClip()?.elevation ?? 0; })()");
  await sleep(400);
  const svg0 = await B(`return b.viewSvg(${JSON.stringify(plan1)}).svg.length;`);
  ms = await timed("Section (tool on)", tool(5, 2), CLICK, "b.actions.value.find((a) => a.name === 'draw_section')?.pressed");
  check("Section in a plan: the tool is on (pressed)", ms !== null && (await attr(tool(5, 2), "aria-pressed")) === "true");
  // Esc: the first point goes, then the tool.
  const pA = await screenOf(2000, 5000), pB = await screenOf(16000, 5000);
  check("the plan projects the two points", !!pA && !!pB, JSON.stringify([pA, pB]));
  await clickAt(pA);
  check("first click: the line starts at (2000, 5000)", JSON.stringify(await evalJs("window.__bbGl.section.a")) === "[2000,5000]", JSON.stringify(await evalJs("window.__bbGl.section.a")));
  await S("Input.dispatchKeyEvent", { type: "keyDown", key: "Escape", code: "Escape", windowsVirtualKeyCode: 27 });
  await S("Input.dispatchKeyEvent", { type: "keyUp", key: "Escape", code: "Escape", windowsVirtualKeyCode: 27 });
  await sleep(100);
  check("Esc: the first point dropped, the tool still on", (await evalJs("window.__bbGl.section.a")) === null && (await B("return b.actions.value.find((a) => a.name === 'draw_section')?.pressed;")) === true);
  const nViews = await B("return b.openViews.value.views.filter((v) => v.type === 'section').length;");
  const u1 = await B("return b.query('history').undo.length;");
  await clickAt(pA);
  await mouse("mouseMoved", [(pA[0] + pB[0]) / 2 + 40, pA[1] + 30]);
  await sleep(150);
  await mouse("mouseMoved", pB);
  await sleep(150);
  await shot("section-line-1280");
  await clickAt(pB);
  await waitFor("the section made", 5000, () => evalJs("window.__bbGl.section.drawnAt !== null"));
  const sec = await evalJs("JSON.stringify(window.__bbGl.section)").then(JSON.parse);
  if (sec.drawnAt !== null) times.push(["Section (second click → frame)", sec.drawnAt - sec.clickAt]);
  const made = await B(`return b.openViews.value.views.find((v) => v.id === ${JSON.stringify(sec.added)}) ?? null;`);
  check("two clicks: a section looking south through (9000, 5000), listed; one undo step", !!made && made.type === "section" && made.facing === "south" &&
    JSON.stringify(made.origin) === "[9000,5000]" && (await B("return b.openViews.value.views.filter((v) => v.type === 'section').length;")) === nViews + 1 &&
    (await B("return b.query('history').undo.length;")) === u1 + 1, JSON.stringify(made ?? sec));
  check("the plan stays shown, the tool is off", (await B("return b.openViews.value.active;")) === plan1 && !(await B("return b.actions.value.find((a) => a.name === 'draw_section')?.pressed;")));
  check("the plan's drawing changed (its new section mark)", (await B(`return b.viewSvg(${JSON.stringify(plan1)}).svg.length;`)) !== svg0);
  await evalJs(`document.querySelector(${JSON.stringify(tool(5, 2))}).click()`);
  await sleep(100);
  await S("Input.dispatchKeyEvent", { type: "keyDown", key: "Escape", code: "Escape", windowsVirtualKeyCode: 27 });
  await S("Input.dispatchKeyEvent", { type: "keyUp", key: "Escape", code: "Escape", windowsVirtualKeyCode: 27 });
  await waitFor("Esc stops the tool", 3000, async () => !(await B("return b.actions.value.find((a) => a.name === 'draw_section')?.pressed;"))).catch(() => {});
  check("Esc with no point: the tool stops", !(await B("return b.actions.value.find((a) => a.name === 'draw_section')?.pressed;")));

  // ======== Sheets → Place view…, Revision… ========
  await setTab("sheets");
  await waitFor("the Sheets ribbon", 5000, () => exists(tool(3, 1)));
  check("Sheets: Place view… and Revision… are real tools", (await attr(tool(1, 2), "data-not-yet")) === null && (await attr(tool(3, 1), "data-not-yet")) === null);
  check("Sheets: no dead control", JSON.stringify(await dead()) === "[]", JSON.stringify(await dead()));
  check("no sheet shown: Place view… and Revision… off", (await attr(tool(1, 2), "aria-disabled")) === "true" && (await attr(tool(3, 1), "aria-disabled")) === "true");
  await evalJs(`document.querySelector(${JSON.stringify(tool(1, 2))}).click()`);
  check("Place view… off: its reason on a click, no dialog", (await exists("[data-bb=off-tip]")) && !(await exists("[data-bb=place-view]")));
  ms = await timed("New sheet", tool(1, 1), CLICK, "!!document.querySelector('[data-bb=sheet-paper]') || !!document.querySelector('[data-bb=sheet-paper-rect]')");
  await waitFor("Place view on", 5000, async () => (await attr(tool(1, 2), "aria-disabled")) === null);
  const sheetId = await evalJs("(async () => { const m = await import('/src/pages/SheetsSide.vue'); return m.currentSheet.value?.id ?? null; })()");
  check("a sheet shown: Place view… and Revision… on", !!sheetId && (await attr(tool(1, 2), "aria-disabled")) === null && (await attr(tool(3, 1), "aria-disabled")) === null);
  const svgS0 = await B(`return b.query('sheet_svg', { id: ${JSON.stringify(sheetId)} }).svg.length;`);
  ms = await timed("Place view… (dialog)", tool(1, 2), CLICK, "!!document.querySelector('[data-bb=place-view-list]')");
  check("Place view…: the dialog lists the views, the new section among them", ms !== null && (await exists(`[data-bb="place-view-${sec.added}"]`)));
  await shot("place-view-dialog-1280");
  ms = await timed("Place view… pick", `[data-bb="place-view-${sec.added}"]`, CLICK,
    `b.sheetLayout(${JSON.stringify(sheetId)}).placed.some((p) => p.view === ${JSON.stringify(sec.added)})`);
  const lay = await B(`return b.sheetLayout(${JSON.stringify(sheetId)});`);
  const pl = lay.placed.find((p) => p.view === sec.added);
  check("Place view…: the section placed at the paper's centre, selected", ms !== null && !!pl && Math.abs(pl.at_mm[0] - lay.width_mm / 2) < 1e-6 && Math.abs(pl.at_mm[1] - lay.height_mm / 2) < 1e-6 &&
    (await evalJs("(async () => (await import('/src/pages/SheetsSide.vue')).selectedPlaced.value)()")) === pl.id, JSON.stringify(lay));
  check("the sheet's SVG grew (the view drawn on it)", (await B(`return b.query('sheet_svg', { id: ${JSON.stringify(sheetId)} }).svg.length;`)) > svgS0);
  await shot("sheet-placed-1280");
  await evalJs(`document.querySelector(${JSON.stringify(tool(1, 2))}).click()`);
  await waitFor("the dialog again", 3000, () => exists(`[data-bb="place-view-${sec.added}"]`));
  check("the list says where a view already is", /1/.test(await evalJs(`document.querySelector('[data-bb="place-view-${sec.added}"] .on')?.textContent ?? ''`)));
  await evalJs(`document.querySelector('[data-bb="place-view-${sec.added}"]').click()`);
  await sleep(200);
  check("placed again: a second copy (as Rust's layout)", (await B(`return b.sheetLayout(${JSON.stringify(sheetId)}).placed.filter((p) => p.view === ${JSON.stringify(sec.added)}).length;`)) === 2);

  ms = await timed("Revision… (dialog)", tool(3, 1), CLICK, "!!document.querySelector('[data-bb=sheet-revision]')");
  check("Revision…: the dialog opens", ms !== null);
  await evalJs(`(() => { const el = document.querySelector('[data-bb=sheet-revision-index]'); el.value = 'R7'; el.dispatchEvent(new Event('input', { bubbles: true })); })()`);
  await shot("revision-dialog-1280");
  const u2 = await B("return b.query('history').undo.length;");
  ms = await timed("Revision… apply", "[data-bb=sheet-revision-apply]", CLICK, `b.sheets().find((s) => s.id === ${JSON.stringify(sheetId)})?.fields.revision === 'R7'`);
  check("Revision…: R7 is the sheet's revision, one undo step", ms !== null && (await B("return b.query('history').undo.length;")) === u2 + 1);
  check("Revision…: R7 printed in the sheet's title block (sheet_svg)", (await B(`return b.query('sheet_svg', { id: ${JSON.stringify(sheetId)} }).svg;`)).includes(">R7<"));

  // ======== Annotate → Text type, Dimension type ========
  await setTab("annotate");
  await waitFor("the Annotate ribbon", 5000, () => exists("[data-bb=annotation-type-text]"));
  check("Annotate: no dead control (the sheet tools are 'not yet': capability sheet_tools false)", JSON.stringify(await dead()) === "[]", JSON.stringify(await dead()));
  check("Annotate: what is 'not yet' is the three sheet tools only", JSON.stringify(await evalJs("[...document.querySelectorAll('[data-bb=ribbon] [data-not-yet]')].map((e) => e.getAttribute('data-bb'))")) === '["ribbon-tool-1-1","ribbon-tool-1-2","ribbon-tool-1-3"]',
    JSON.stringify(await evalJs("[...document.querySelectorAll('[data-bb=ribbon] [data-not-yet]')].map((e) => e.getAttribute('data-bb'))")));
  const nText = await evalJs("document.querySelectorAll('[data-bb=annotation-type-text] option').length");
  check("Text type: Default and the standards' 25 text types", nText === 26, String(nText));
  check("Dimension type: Default and the standards' dimension types", (await evalJs("document.querySelectorAll('[data-bb=annotation-type-dimension] option').length")) > 1);
  ms = await timed("Text type", "[data-bb=annotation-type-text]", pick("Arial 5 mm black"), "b.annotationTypes()?.text === 'Arial 5 mm black'");
  check("Text type: Arial 5 mm black in use (query annotation_types)", ms !== null);
  const dim1 = await evalJs("document.querySelectorAll('[data-bb=annotation-type-dimension] option')[1].value");
  ms = await timed("Dimension type", "[data-bb=annotation-type-dimension]", pick(dim1), `b.annotationTypes()?.dimension === ${JSON.stringify(dim1)}`);
  check("Dimension type: the first type in use", ms !== null, dim1);
  await shot("annotate-types-1280");
  // The next text placed in the plan gets it.
  await evalJs(`(async () => { const s = await import('/src/shell/send.ts'); s.send({ do: "showView" }); })()`);
  await B(`return b.openView(${JSON.stringify(plan1)});`);
  await waitFor("the plan again", 8000, async () => (await B("return b.viewClip()?.type;")) === "plan");
  await sleep(300);
  check("annotate tool: a text", (await B("return b.startText('Typed');")) === null);
  const pT = await screenOf(4000, 2000);
  await clickAt(pT);
  await sleep(200);
  const anns = await B(`return JSON.stringify(b.query('view_annotations', { view: ${JSON.stringify(plan1)} }));`);
  check("the text placed has kind 'Arial 5 mm black' and 5 mm", /"kind":"Arial 5 mm black"/.test(anns) && /"size_mm":5(\.0)?[,}]/.test(anns), anns.slice(0, 600));
  // ---- where the time goes (VIEWSHEET_PROBE=1): a call alone, a dialog alone, a GL frame alone ----
  if (process.env.VIEWSHEET_PROBE) {
    const p = await evalJs(`(async () => {
      const b = await import('/src/bimble.ts'); const d = await import('/src/dialogs/index.ts'); const g = window.__bbGl;
      const raf = () => new Promise((r) => requestAnimationFrame(r)); const out = {};
      let t = performance.now(); b.runAction('new_view'); out.action_new_view = performance.now() - t;
      t = performance.now(); b.query('actions'); out.query_actions = performance.now() - t;
      t = performance.now(); d.currentDialog.value = 'new-view'; await raf(); out.dialog_to_raf = performance.now() - t;
      t = performance.now(); await raf(); out.idle_raf = performance.now() - t;
      d.currentDialog.value = null; await raf(); await raf();
      const f0 = g.frames; t = performance.now(); b.runAction('draw_section'); out.action_draw_section = performance.now() - t;
      await raf(); out.draw_section_raf1 = performance.now() - t; out.frames1 = g.frames - f0;
      await raf(); out.draw_section_raf2 = performance.now() - t; out.frames2 = g.frames - f0;
      b.runAction('draw_section'); await raf(); await raf();
      const lv = b.storeys.value[1]?.id ?? b.storeys.value[0]?.id;
      for (const k of [1, 2]) {
        t = performance.now(); const r = b.viewAdd({ kind: 'plan', storey: lv, scale: 50 }); out['view_add' + k] = performance.now() - t;
        t = performance.now(); b.openView(r.id); out['open_view' + k] = performance.now() - t;
        const f1 = g.frames; let n = 0;
        while (g.frames === f1 && n++ < 300) await raf();
        out['open_to_frame' + k] = performance.now() - t;
        t = performance.now(); b.query('view_clip'); out['q_view_clip' + k] = performance.now() - t;
        t = performance.now(); b.query('view_annotations', { view: r.id }); out['q_view_annotations' + k] = performance.now() - t;
      }
      const d3 = b.openViews.value.views.find((v) => v.type === 'three_d')?.id, pl = b.openViews.value.views.find((v) => v.type === 'plan')?.id;
      for (const k of [1, 2]) { // 3D, then a plan: the first switch of the session against a later one
        b.openView(d3); for (let n = 0; n < 30; n++) await raf();
        t = performance.now(); b.openView(pl); const f1 = g.frames; let n = 0;
        while (g.frames === f1 && n++ < 300) await raf();
        out['three_d_to_plan' + k] = performance.now() - t;
      }
      const sh = b.sheets()[0]?.id;
      if (sh) { t = performance.now(); b.query('sheet_svg', { id: sh }); out.q_sheet_svg = performance.now() - t; }
      return JSON.stringify(out); })()`);
    console.log(`probe ${p}`);
  }
}

async function main() {
  if (!existsSync(join(UI, "node_modules"))) throw new Error(`no ${UI}/node_modules: run npm ci in web-ui/`);
  execFileSync("node", [join(UI, "scripts/copy-ccore.mjs")], { stdio: "ignore" });
  const model = join(tmp, "frame.bimble");
  copyFileSync(join(CCORE, "golden/frame.json"), model);
  const home = join(tmp, "home");
  mkdirSync(home);
  const h = start("host", BIN, [String(PORT), model], {
    env: hostEnv({ HOME: home, BIMBLE_TOKEN: TOKEN, BIMBLE_ORIGINS: `http://localhost:${VITE},http://127.0.0.1:${VITE}` }),
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
  await tab(S, evalJs);

  const IGNORED = [
    /Synchronous XMLHttpRequest on the main thread is deprecated/,
    /404 \(Not Found\) http:\/\/localhost:\d+\/favicon\.ico$/,
    // The refusal this test asks for (a section of zero length is not sent; none expected): nothing else.
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
  for (const [name, ms] of times) console.log(`time ${name} ${ms.toFixed(1)} ms`);
  if (times.length) {
    const v = times.map((t) => t[1]).sort((a, b) => a - b);
    console.log(`time: ${v.length} controls, median ${v[Math.floor(v.length / 2)].toFixed(1)} ms, max ${v[v.length - 1].toFixed(1)} ms, over 200 ms: ${v.filter((x) => x > 200).length}`);
  }
  await cleanup();
  console.log(`${passed} passed, ${failed} failed`);
  process.exit(failed ? 1 : 0);
}
