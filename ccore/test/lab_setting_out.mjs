// The Setting out tab in the lab, every control of it (ticket SETOUT, ADR 0075):
//   CCORE_PORT_BASE=auto node ccore/test/lab_setting_out.mjs
// Starts out/tcc/bimble-ccore (or $HOST_BIN) on a copy of golden/frame.json (72 elements), with
// HOME and BIMBLE_CONFIG_DIR in temporary folders, Vite from web-ui/ and one headless Chromium over
// raw CDP, then opens /?native=1&gl=1&tab=settingOut and works each control of the tab:
//   Ribbon (shell/ribbon-tabs.ts settingOut):
//     Survey point, Project base point: the markers appear on the overlay (window.__bbGl.setout:
//       survey_point; base_point and true_north) and go again on a second click (session state:
//       the rev does not move);
//     True / project north: off ("not now") while the model has no turn; with 30° typed in the side
//       panel it is on: in 3D it presses and turns the section box (box_north_true); in a plan it is
//       one history step (the view's true_north), undone by undo;
//     Project zero: typed 432.8 -> query setting_out project_zero_m 432.8, one history step;
//     Levels, Grids: the Levels & grids page opens; Measure: pressed;
//     Reference planes (WAVE1B): the work plane chosen in the side panel (a level), the ribbon's
//       three-point tool (three clicks on the canvas make a plane), a plane by origin + normal,
//       listed in the panel and the navigator, renamed, made the work plane, deleted, undone; the
//       outline drawn in 3D, the cut line in a plan.
//   Side panel (shell/SettingOutSide.vue): the system, east, north, height, angle and map angle
//     fields (query north / setting_out read them back), the orientation select (the active view's
//     true_north), the survey point's X, Y, Z (its survey coordinates move with it), "Back to the
//     base point" (unset), "Move the base point…" "not yet" (capability base_point_move false).
// Before SETOUT the three Points buttons, the Project zero box and Reference planes were drawn
// "not yet" (no action), the panel's Move button was disabled with data-missing and the panel had
// no survey point: the checks of the Points buttons, Project zero and the survey point fail there.
// Each control's time from the click (or the change) to the first frame after its effect (the
// viewport's frame that drew it, or the next animation frame for the page's own DOM) is printed ("time <control> <ms>"); the test does not fail on time. No uncaught page error, no
// console error or warning but the sync-XHR deprecation and the favicon 404. Prints
// "N passed, M failed"; exits non-zero on a failure. Kills all it starts.
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
const PORT = port("lab_setting_out");
const VITE = vitePort("lab_setting_out");
const TOKEN = "labsetout-token";
const PAGE = `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}&tab=settingOut`;
const SHOTS = process.env.SETOUT_SHOTS ?? ""; // a folder: screenshots of the tab at 1280 and 360 px
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-setout-"));
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
  const so = () => B("return b.query('setting_out');");
  const rev = () => B("return b.state.value.rev;");
  const markers = () => evalJs("JSON.stringify((window.__bbGl && window.__bbGl.setout) || [])").then((t) => JSON.parse(t).map((m) => m.kind));
  const attr = (sel, a) => evalJs(`document.querySelector(${JSON.stringify(sel)})?.getAttribute(${JSON.stringify(a)}) ?? null`);
  const exists = (sel) => evalJs(`!!document.querySelector(${JSON.stringify(sel)})`);
  /**
   * Runs `act` (a JS statement on `el`, the control) in the page and waits, frame by frame, until
   * `cond` (a JS expression, `b` = bimble.ts, `g` = window.__bbGl) holds and a frame was drawn after
   * it: the milliseconds from the action to that frame, or null after 5 s.
   */
  const timed = async (name, sel, act, cond) => {
    const ms = await evalJs(`(async () => {
      const b = await import('/src/bimble.ts'); const g = () => window.__bbGl;
      const el = document.querySelector(${JSON.stringify(sel)}); if (!el) return -1;
      const f0 = g()?.frames ?? 0, t0 = performance.now(); ${act};
      const raf = () => new Promise((r) => requestAnimationFrame(r));
      while (performance.now() - t0 < 5000) {
        await raf();
        let ok = false; try { ok = await (async () => (${cond}))(); } catch { ok = false; }
        // The effect is there: the viewport drew a frame since the action (markers, the model), or
        // it is the page's own (a pressed button, a page, a field): painted at the next frame.
        if (ok) { if ((g()?.frames ?? 0) === f0) await raf(); return performance.now() - t0; }
      }
      return null; })()`);
    if (typeof ms === "number" && ms >= 0) times.push([name, ms]);
    return ms;
  };
  const CLICK = "el.click()";
  const type = (v) => `el.value = ${JSON.stringify(v)}; el.dispatchEvent(new Event('input', { bubbles: true })); el.dispatchEvent(new Event('change', { bubbles: true }))`;
  const tool = (g, m) => `[data-bb=ribbon] [data-bb=ribbon-tool-${g}-${m}]`;

  await S("Page.navigate", { url: PAGE });
  await waitFor("data-bb-ready", 30000, () => evalJs("document.documentElement.dataset.bbReady === '1'"));
  await waitFor("the Setting out ribbon", 10000, () => exists(tool(1, 1)));
  await waitFor("the first frame", 10000, () => evalJs("(window.__bbGl?.frames ?? 0) > 0"));

  // ---- the controls, counted (none may be dead: an effect, or "not yet") ----
  const ribbon = await evalJs("[...document.querySelectorAll('[data-bb=ribbon] .tool, [data-bb=ribbon] .rbox')].map((e) => e.getAttribute('data-bb'))");
  check("the tab has 8 tools and 1 box (WAVE1B: Grid axis)", ribbon.length === 9, JSON.stringify(ribbon));
  const notYet = await evalJs("[...document.querySelectorAll('[data-bb=ribbon] [data-not-yet]')].map((e) => e.getAttribute('data-bb'))");
  check("nothing is 'not yet' in the ribbon (WAVE1B: Reference planes served)", notYet.length === 0, JSON.stringify(notYet));
  if (SHOTS) {
    for (const [w, h] of [[1280, 800], [360, 740]]) {
      await S("Emulation.setDeviceMetricsOverride", { width: w, height: h, deviceScaleFactor: 1, mobile: w < 500 });
      await sleep(400);
      const { data } = await S("Page.captureScreenshot", { format: "png" });
      writeFileSync(join(SHOTS, `setting-out-${w}-before.png`), Buffer.from(data, "base64"));
    }
    await S("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
    await sleep(300);
  }

  // ---- Survey point and Project base point: markers, session state ----
  const r0 = await rev();
  let ms = await timed("survey point on", tool(1, 1), CLICK, "(g()?.setout ?? []).some((m) => m.kind === 'survey_point')");
  check("Survey point: its marker is drawn, the button pressed, rev kept", ms !== null && (await attr(tool(1, 1), "aria-pressed")) === "true" && (await rev()) === r0,
    JSON.stringify(await markers()));
  ms = await timed("base point on", tool(1, 2), CLICK, "(g()?.setout ?? []).some((m) => m.kind === 'base_point')");
  check("Project base point: its marker and the true north arrow are drawn", ms !== null && (await markers()).includes("true_north") && (await attr(tool(1, 2), "aria-pressed")) === "true",
    JSON.stringify(await markers()));
  check("the base point marker sits where the world origin projects", await evalJs(`(() => { const g = window.__bbGl; const b = document.getElementById('bimble').getBoundingClientRect();
    const p = g.toScreen(0, 0, 0); const m = g.setout.find((x) => x.kind === 'base_point'); return !!p && !!m && Math.hypot(p[0] - b.left - m.at[0], p[1] - b.top - m.at[1]) < 1; })()`));
  ms = await timed("survey point off", tool(1, 1), CLICK, "!(g()?.setout ?? []).some((m) => m.kind === 'survey_point')");
  check("Survey point again: the marker goes", ms !== null && (await attr(tool(1, 1), "aria-pressed")) !== "true");

  // ---- True / project north: off without a turn; the side panel's angle turns the model ----
  check("True / project north: off while true north is project north", (await attr(tool(1, 3), "aria-disabled")) === "true" && !(await attr(tool(1, 3), "data-not-yet")));
  ms = await timed("angle 30°", "[data-bb=settingout-rotation_deg]", type("30"), "b.northInfo.value?.rotation_deg === 30 && b.actions.value.find((a) => a.name === 'view_true_north')?.enabled");
  check("side panel: angle 30 -> query north 30, True / project north on", ms !== null && (await so()).north.rotation_deg === 30);
  check("the arrows: project north drawn apart from true north", (await markers()).includes("project_north"), JSON.stringify(await markers()));
  ms = await timed("north (3D)", tool(1, 3), CLICK, "b.actions.value.find((a) => a.name === 'view_true_north')?.pressed");
  check("True / project north in 3D: pressed, the section box in true north", ms !== null && (await B("return b.actions.value.find((a) => a.name === 'box_north_true')?.pressed;")) === true);
  await timed("north back (3D)", tool(1, 3), CLICK, "!b.actions.value.find((a) => a.name === 'view_true_north')?.pressed");

  // ---- Project zero ----
  const r1 = await rev();
  ms = await timed("project zero", "[data-bb=ribbon-project-zero]", type("432.8"), "b.settingOut.value?.project_zero_m === 432.8");
  check("Project zero: 432.8 typed -> project_zero_m 432.8, one step", ms !== null && (await rev()) === r1 + 1 && (await so()).base_point.survey_m[2] === 432.8);
  check("Project zero: the box shows +432.800", (await evalJs("document.querySelector('[data-bb=ribbon-project-zero]').value")) === "+432.800");

  // ---- the side panel's georeference fields ----
  for (const [k, v, want] of [["crs", "EPSG:2056", "b.settingOut.value?.crs === 'EPSG:2056'"], ["east_m", "2600000", "b.settingOut.value?.base_point.survey_m[0] === 2600000"],
    ["north_m", "1200000", "b.settingOut.value?.base_point.survey_m[1] === 1200000"], ["height_m", "432.5", "b.settingOut.value?.project_zero_m === 432.5"],
    ["map_rotation_deg", "2", "b.northInfo.value?.map_rotation_deg === 2"]]) {
    ms = await timed(`side ${k}`, `[data-bb=settingout-${k}]`, type(v), want);
    check(`side panel: ${k} ${v} -> read back`, ms !== null);
  }
  ms = await timed("side angle 390 refused", "[data-bb=settingout-rotation_deg]", type("390"), "!!document.querySelector('[data-bb=settingout-error]')");
  check("side panel: 390° refused with the verb's reason", ms !== null && (await so()).north.rotation_deg === 30);

  // ---- the survey point ----
  ms = await timed("survey X", "[data-bb=settingout-survey-X]", type("12000"), "b.settingOut.value?.survey_point.set && b.settingOut.value.survey_point.at_mm[0] === 12000");
  check("survey point X 12000 -> placed, its survey coordinates move", ms !== null && (await so()).survey_point.survey_m[0] !== 2600000, JSON.stringify((await so()).survey_point));
  ms = await timed("survey Y", "[data-bb=settingout-survey-Y]", type("-5000"), "b.settingOut.value?.survey_point.at_mm[1] === -5000");
  check("survey point Y -5000", ms !== null);
  ms = await timed("survey Z", "[data-bb=settingout-survey-Z]", type("1500"), "b.settingOut.value?.survey_point.at_mm[2] === 1500");
  check("survey point Z 1500", ms !== null);
  await timed("survey point shown", tool(1, 1), CLICK, "(g()?.setout ?? []).some((m) => m.kind === 'survey_point')");
  check("the survey marker sits at (12000, -5000, 1500)", await evalJs(`(() => { const g = window.__bbGl; const b = document.getElementById('bimble').getBoundingClientRect();
    const p = g.toScreen(12000, -5000, 1500); const m = g.setout.find((x) => x.kind === 'survey_point'); return !!p && !!m && Math.hypot(p[0] - b.left - m.at[0], p[1] - b.top - m.at[1]) < 1; })()`));
  if (SHOTS) {
    for (const [w, h] of [[1280, 800], [360, 740]]) {
      await S("Emulation.setDeviceMetricsOverride", { width: w, height: h, deviceScaleFactor: 1, mobile: w < 500 });
      await sleep(500);
      const { data } = await S("Page.captureScreenshot", { format: "png" });
      writeFileSync(join(SHOTS, `setting-out-${w}.png`), Buffer.from(data, "base64"));
    }
    await S("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
    await sleep(300);
  }
  ms = await timed("survey back to base", "[data-bb=settingout-survey-remove]", CLICK, "b.settingOut.value?.survey_point.set === false");
  check("Back to the base point: the survey point unset", ms !== null);

  // ---- Move the base point: not served ----
  check("Move the base point: 'not yet', capability base_point_move false", (await attr("[data-bb=settingout-move]", "data-not-yet")) === "" &&
    (await B("return b.query('capabilities').base_point_move;")) === false);
  // ---- WAVE1B: Reference planes (ADR 0075): the work plane, the three-point tool, the panel ----
  // Before WAVE1B the ribbon's button was "not yet" and the panel had no such section: every check
  // below fails there (no data-bb=refplane-*, query reference_planes unknown).
  check("Reference planes: capability reference_planes true", (await B("return b.query('capabilities').reference_planes;")) === true);
  const lvl0 = await B("return b.storeys.value.find((s) => s.elevation === 0)?.id;");
  ms = await timed("work plane: level", "[data-bb=refplane-work]", type(`level:${lvl0}`), "b.refPlanes.value?.active?.kind === 'level'");
  check("Work plane: Level Niveau 1 chosen -> active", ms !== null);
  const nPlanes = async () => (await B("return b.query('reference_planes').planes.length;"));
  const p0 = await nPlanes();
  ms = await timed("reference planes tool on", tool(3, 3), CLICK, "b.refPlanes.value?.pick.on === true");
  check("Reference planes (ribbon): pressed, the pick tool on", ms !== null && (await attr(tool(3, 3), "aria-pressed")) === "true");
  const screen = async (x, y, z) => JSON.parse(await evalJs(`JSON.stringify(window.__bbGl.toScreen(${x}, ${y}, ${z}))`));
  const canvasClick = async (name, [x, y, z], cond, modifiers = 1) => {
    const [sx, sy] = await screen(x, y, z);
    await evalJs(`(() => { window.__rpT = null; document.addEventListener('pointerdown', (e) => { window.__rpT = e.timeStamp; }, { capture: true, once: true }); window.__rpF = window.__bbGl.frames; })()`);
    await S("Input.dispatchMouseEvent", { type: "mouseMoved", x: sx, y: sy, button: "none", buttons: 0, modifiers });
    await S("Input.dispatchMouseEvent", { type: "mousePressed", x: sx, y: sy, button: "left", buttons: 1, clickCount: 1, modifiers });
    await S("Input.dispatchMouseEvent", { type: "mouseReleased", x: sx, y: sy, button: "left", buttons: 0, clickCount: 1, modifiers });
    const t = await evalJs(`(async () => {
      const b = await import('/src/bimble.ts'); const g = () => window.__bbGl;
      const raf = () => new Promise((r) => requestAnimationFrame(r)); const t0 = performance.now();
      while (performance.now() - t0 < 5000) {
        await raf();
        let ok = false; try { ok = await (async () => (${cond}))(); } catch { ok = false; }
        if (ok && window.__rpT !== null) { if (g().frames === window.__rpF) await raf(); return performance.now() - window.__rpT; }
      }
      return null; })()`);
    if (typeof t === "number") times.push([name, t]);
    return t;
  };
  ms = await canvasClick("plane point 1", [-2000, 3000, 0], "b.refPlanes.value?.pick.points.length === 1");
  check("three-point tool: the first click takes a point on the work plane (drawn as a cross)", ms !== null &&
    (await evalJs("(window.__bbGl.refplanes ?? []).filter((d) => d.kind === 'point').length")) === 1);
  await canvasClick("plane point 2", [-2000, 8000, 0], "b.refPlanes.value?.pick.points.length === 2");
  const picked = await B("return b.refPlanes.value.pick.points;");
  ms = await canvasClick("plane point 3 (made)", [-6000, 5000, 0], `b.refPlanes.value?.planes.length === ${p0 + 1}`);
  check("the third click makes the plane (one step), drawn as an outline in 3D", ms !== null && (await nPlanes()) === p0 + 1 &&
    (await evalJs("(window.__bbGl.refplanes ?? []).filter((d) => d.kind === 'outline').length")) === p0 + 1);
  const made = await B("return b.query('reference_planes').planes[0];");
  // the points snap as Measure's (a face, an edge, a corner, else the work plane): the plane holds
  // the first two (its origin the first, rounded to 1 mm), its normal a unit vector
  const off = (q) => Math.abs((q[0] - made.origin[0]) * made.normal[0] + (q[1] - made.origin[1]) * made.normal[1] + (q[2] - made.origin[2]) * made.normal[2]);
  check("the plane through the picked points (origin the first, the second on it)", picked.length === 2 && off(picked[0]) < 1 && off(picked[1]) < 1 &&
    Math.abs(Math.hypot(...made.normal) - 1) < 1e-9, JSON.stringify({ made, picked }));
  await timed("reference planes tool off", tool(3, 3), CLICK, "b.refPlanes.value?.pick.on === false");
  // by origin + normal: a vertical plane x = -1000 (normal X)
  for (const [k, v] of [["origin-X", "-1000"], ["origin-Y", "5000"], ["origin-Z", "1500"], ["normal-X", "1"], ["normal-Z", "0"]])
    await evalJs(`(() => { const el = document.querySelector('[data-bb=refplane-${k}]'); ${type(v)}; })()`);
  ms = await timed("add plane (origin + normal)", "[data-bb=refplane-add]", CLICK, `b.refPlanes.value?.planes.length === ${p0 + 2}`);
  check("Add the plane: origin (-1000, 5000, 1500), normal X -> stored, listed in the panel and the navigator", ms !== null &&
    (await evalJs("document.querySelectorAll('[data-bb=refplane-row]').length")) === p0 + 2);
  const vert = await B("return b.query('reference_planes').planes.find((p) => Math.abs(p.normal[0] - 1) < 1e-9);");
  check("its frame: normal (1, 0, 0), origin (-1000, 5000, 1500)", !!vert && vert.origin[0] === -1000 && vert.origin[2] === 1500, JSON.stringify(vert));
  await evalJs("document.querySelector('[data-bb=nav-refplanes]')?.click()");
  check("the navigator lists the planes", (await evalJs("document.querySelectorAll('[data-bb=nav-refplane]').length")) === p0 + 2);
  ms = await timed("rename plane", `[data-bb=refplane-row][data-id="${vert.id}"] [data-bb=refplane-name]`, type("Face A"),
    `b.refPlanes.value?.planes.some((p) => p.name === 'Face A')`);
  check("rename: Face A", ms !== null);
  ms = await timed("work plane: Face A", "[data-bb=refplane-work]", type(`plane:${vert.id}`), `b.refPlanes.value?.active?.id === ${JSON.stringify(vert.id)}`);
  check("Work plane: Face A chosen (the navigator marks it)", ms !== null && (await evalJs(`!!document.querySelector('[data-bb=nav-refplane][data-id="${vert.id}"] .n')`)));
  const r3 = await rev();
  ms = await timed("delete plane", `[data-bb=refplane-row][data-id="${vert.id}"] [data-bb=refplane-delete]`, CLICK, `b.refPlanes.value?.planes.length === ${p0 + 1}`);
  check("Delete: one step; the work plane falls back to the storey's level", ms !== null && (await rev()) === r3 + 1 && (await B("return b.refPlanes.value.active;")) === null);
  await B("return b.undo();");
  await waitFor("undo of the delete", 5000, async () => (await nPlanes()) === p0 + 2).catch(() => {});
  check("undo: the plane is back (same GlobalId)", (await B(`return b.query('reference_planes').planes.some((p) => p.id === ${JSON.stringify(vert.id)});`)) === true);
  if (SHOTS) {
    const { data } = await S("Page.captureScreenshot", { format: "png" });
    writeFileSync(join(SHOTS, "setting-out-reference-planes-1280.png"), Buffer.from(data, "base64"));
  }

  // ---- Orientation (the active view's true_north) ----
  const vid = await B("return b.openViews.value.active;");
  ms = await timed("orientation select", "[data-bb=settingout-orientation]", type("true"), `b.openViews.value.views.find((v) => v.id === ${JSON.stringify(vid)})?.true_north === true`);
  check("Orientation: True north -> the active view's true_north", ms !== null, String(vid));
  await timed("orientation back", "[data-bb=settingout-orientation]", type("false"), `b.openViews.value.views.find((v) => v.id === ${JSON.stringify(vid)})?.true_north === false`);

  // ---- True / project north in a plan: one history step, undone ----
  const plan = await B("return b.openViews.value.views.find((v) => v.type === 'plan' && v.look === 'down')?.id;");
  await B(`return b.openView(${JSON.stringify(plan)});`);
  await waitFor("the plan shown", 8000, async () => (await B("return b.openViews.value.active;")) === plan).catch(() => {});
  const r2 = await rev();
  ms = await timed("north (plan)", tool(1, 3), CLICK, "b.actions.value.find((a) => a.name === 'view_true_north')?.pressed");
  check("True / project north in a plan: pressed, one history step", ms !== null && (await rev()) === r2 + 1);
  check("the plan's markers: the base point drawn in the plan too", (await markers()).includes("base_point"), JSON.stringify(await markers()));
  check("WAVE1B: the vertical plane is a line in the plan (its cut)", (await evalJs(`(window.__bbGl.refplanes ?? []).filter((d) => d.kind === 'line' && d.id === ${JSON.stringify(vert.id)}).length`)) === 1,
    await evalJs("JSON.stringify(window.__bbGl.refplanes ?? [])"));
  if (SHOTS) {
    const { data } = await S("Page.captureScreenshot", { format: "png" });
    writeFileSync(join(SHOTS, "setting-out-reference-plane-plan-1280.png"), Buffer.from(data, "base64"));
  }
  await B("return b.undo();");
  await waitFor("undo", 5000, async () => !(await B("return b.actions.value.find((a) => a.name === 'view_true_north')?.pressed;"))).catch(() => {});
  check("undo: the plan in project north again", !(await B("return b.actions.value.find((a) => a.name === 'view_true_north')?.pressed;")));

  // ---- WAVE1B (A2): the two-click grid axis tool in a plan (Shift: along x or y; Esc cancels) ----
  // Before A2 the ribbon had no such tool (the tab's count and every check below fail there).
  check("Grid axis: off while the model has no grid", (await attr(tool(3, 4), "aria-disabled")) === "true");
  check("a grid 1 / A (verb grid_add)", (await B(`return b.runVerb('grid_add', { name: 'G', axes: [{ tag: '1', start: [0, -1000], end: [0, 11000] }, { tag: 'A', start: [-1000, 0], end: [19000, 0] }] });`)) === null);
  await waitFor("the tool enabled", 5000, async () => (await attr(tool(3, 4), "aria-disabled")) !== "true").catch(() => {});
  ms = await timed("grid axis tool on", tool(3, 4), CLICK, "b.placing.value?.active === true && b.placing.value.kind === 'grid_axis'");
  check("Grid axis (ribbon): the tool on in the plan", ms !== null && (await attr(tool(3, 4), "aria-pressed")) === "true");
  const nAxes = async () => (await B("return b.query('grids')[0]?.axes.length ?? 0;"));
  await B("return b.runAction('fit');");
  await sleep(300);
  await canvasClick("grid axis: first click", [6000, 1000, 0], "b.placing.value?.points.length === 1", 0);
  ms = await canvasClick("grid axis: second click (Shift) -> axis 2", [6300, 9000, 0], "b.query('grids')[0]?.axes.length === 3", 8);
  const ax = await B("return b.query('grids')[0].axes.find((a) => a.tag === '2') ?? null;");
  check("two clicks, the second with Shift: axis '2' (after '1', parallel), vertical (x locked) at about x 6000 (the work plane Face A does not pull it)",
    ms !== null && !!ax && Math.abs(ax.start_mm[0] - ax.end_mm[0]) < 1e-6 && Math.abs(ax.start_mm[0] - 6000) < 400,
    JSON.stringify({ ax, placing: await B("return b.placing.value;"), err: await evalJs("window.__bbGl.toolError ?? null"), view: await B("return b.openViews.value.active;") }));
  await canvasClick(null, [12000, 1000, 0], "b.placing.value?.points.length === 1", 0);
  await evalJs(`(() => { window.__rpT = null; document.addEventListener('keydown', (e) => { window.__rpT = e.timeStamp; }, { capture: true, once: true }); })()`);
  await S("Input.dispatchKeyEvent", { type: "keyDown", key: "Escape", code: "Escape", windowsVirtualKeyCode: 27 });
  await S("Input.dispatchKeyEvent", { type: "keyUp", key: "Escape", code: "Escape", windowsVirtualKeyCode: 27 });
  await waitFor("Esc", 5000, async () => (await B("return b.placing.value?.active !== true;"))).catch(() => {});
  check("Esc: the first point goes, nothing added, the tool stops", (await nAxes()) === 3 && (await B("return b.placing.value?.active !== true;")));
  if (SHOTS) {
    const { data } = await S("Page.captureScreenshot", { format: "png" });
    writeFileSync(join(SHOTS, "setting-out-grid-axis-plan-1280.png"), Buffer.from(data, "base64"));
  }
  // ---- WAVE1B (A3): the grids in 3D at the active level's elevation, off with the view's IfcGrid
  // visibility. Before A3 the 3D view drew no grid (window.__bbGrids.axes 0 there). ----
  const v3d = await B("return b.openViews.value.views.find((v) => v.type === 'three_d')?.id;");
  ms = await timed("grids in 3D (the 3D view opened)", "body", `b.openView(${JSON.stringify(v3d)})`, "window.__bbGl.camera?.mode !== 'plan' && (window.__bbGrids?.axes ?? 0) === 3");
  const g3 = await evalJs("JSON.stringify(window.__bbGrids)");
  check("Grids in 3D: the three axes drawn with their bubbles (1, 2, A), at the active level", ms !== null && JSON.parse(g3).bubbles.length === 6, g3);
  const lvl = await B("return b.storeys.value.find((x) => x.id === b.placing.value?.storey)?.elevation ?? 0;");
  check("the axis 1's bubble sits where (0, -1000, level) projects (just past the start)", await evalJs(`(() => { const g = window.__bbGl, bb = window.__bbGrids; const r = document.getElementById('bimble').getBoundingClientRect();
    const p = g.toScreen(0, -1000, ${lvl}), q = g.toScreen(0, 11000, ${lvl}); if (!p || !q) return false; const b0 = bb.bubbles.find((x) => x.tag === '1'); if (!b0) return false;
    const ux = q[0] - p[0], uy = q[1] - p[1], l = Math.hypot(ux, uy); const ex = p[0] - r.left - ux / l * 12, ey = p[1] - r.top - uy / l * 12;
    return Math.hypot(ex - b0.at[0], ey - b0.at[1]) < 1.5; })()`));
  ms = await timed("grids in 3D hidden (view visibility IfcGrid)", "body", `b.runVerb('view_edit', { id: ${JSON.stringify(v3d)}, visibility: { hidden_classes: ['IfcGrid'] } })`, "(window.__bbGrids?.axes ?? 0) === 0");
  check("the view's visibility hides IfcGrid: no axis drawn in 3D", ms !== null);
  await B(`return b.runVerb('view_edit', { id: ${JSON.stringify(v3d)}, visibility: { hidden_classes: [] } });`);
  if (SHOTS) {
    await sleep(300);
    const { data } = await S("Page.captureScreenshot", { format: "png" });
    writeFileSync(join(SHOTS, "setting-out-grids-3d-1280.png"), Buffer.from(data, "base64"));
  }
  await B(`return b.openView(${JSON.stringify(plan)});`);
  await waitFor("the plan again", 8000, async () => (await evalJs("window.__bbGl.camera?.mode")) === "plan").catch(() => {});

  // ---- Levels, Grids, Measure ----
  for (const [m, name] of [[1, "Levels"], [2, "Grids"]]) {
    ms = await timed(name, tool(3, m), CLICK, "!!document.querySelector('[data-bb=levels-grids]')");
    check(`${name}: the Levels & grids page opens`, ms !== null);
    await evalJs(`(async () => { const s = await import('/src/shell/send.ts'); s.send({ do: "showView" }); })()`);
    await waitFor("the view again", 5000, async () => !(await exists("[data-bb=levels-grids]"))).catch(() => {});
  }
  ms = await timed("Measure", tool(4, 1), CLICK, "b.actions.value.find((a) => a.name === 'measure')?.pressed");
  check("Measure: pressed", ms !== null);
  await evalJs(`document.querySelector(${JSON.stringify(tool(4, 1))}).click()`);
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
    // The refusal this test asks for (390°, georef_set 400): bimble.ts's sync XHR line.
    new RegExp(`400 \\(Bad Request\\) http://localhost:${VITE}/src/bimble\\.ts$`),
  ];
  const errors = consoleLog.filter((m) => /error|warn/.test(m.level) && !IGNORED.some((r) => r.test(m.text)));
  check("no uncaught page error", exceptions.length === 0, exceptions.join(" | "));
  check("no console error or warning", errors.length === 0, errors.map((m) => m.text).join(" | "));
  const badResp = failedResp.filter((r) => !/^404 http:\/\/localhost:\d+\/favicon\.ico$/.test(r) &&
    !new RegExp(`^400 http://127\\.0\\.0\\.1:${PORT}/verb/georef_set\\?`).test(r));
  check("no failed response but the refusal asked for", badResp.length === 0, badResp.join(" | "));
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
