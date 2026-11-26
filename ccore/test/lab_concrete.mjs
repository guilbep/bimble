// The Concrete tab in the lab (ticket CONC1, ADR 0077):
//   CCORE_PORT_BASE=auto node ccore/test/lab_concrete.mjs
// Starts out/tcc/bimble-ccore (or $HOST_BIN) on a copy of golden/frame.json (72 elements, saved
// as JSON), with HOME and BIMBLE_CONFIG_DIR in temporary folders, Vite from web-ui/ and one
// headless Chromium over raw CDP, opens /?native=1&gl=1&tab=concrete and drives, on the plan of
// Niveau 1 (a wall placed by the place verb first):
//   Footing: the ribbon tool, one click on the column at (0, 0): an IfcFooting pad_footing centred
//     under it, top at its base (-500); one history step; undo removes it;
//   Strip footing (the panel's Pad / Strip switch): one click on the wall: an IfcFooting along it,
//     6000 long, STRIP_FOOTING;
//   Opening: two clicks on the wall: the wall holds one opening, one step; one over it refused
//     with the reason; undo removes it;
//   Sleeve: one click on the slab: a round opening of the slab;
//   Raft: three corners and Enter: an IfcSlab BASESLAB on Fondations, top at -800 (z -400);
//   save, reopen, save: the same bytes; the IFC4 export holds IfcFooting STRIP_FOOTING, IfcSlab
//   BASESLAB, IfcOpeningElement and two IfcRelVoidsElement, the sleeve's 'SLEEVE';
//   Stairs and Ramp: "not available yet" (data-not-yet).
//   WAVE1B (B2): the opening tool's preview (a ghost after the first click), an opening selected by
//     a click in the plan, by its row in the wall's Properties and by a click in 3D (its box drawn in
//     the selection colour), its width and sill changed in Properties (opening_set, GlobalId kept).
// Volumes are checked by hand-computed values in the C suite `concrete` (test/concrete_test.c).
// Before CONC1 Footing, Raft, Opening and Sleeve had no action ("not yet"): the first check and
// every gesture fail there. Each step's time (click or key -> the end of the first frame after its
// effect, ADR 0074) is printed ("time <step> <ms>"), with /proc/loadavg; the test does not fail on
// time. CONC1_SHOTS=<folder>: a plan and the 3D view at 1280 px. CONC1_IFC=<file>: a copy of the
// export. No uncaught page error, no console error or warning but the sync-XHR deprecation, the
// favicon 404 and the refusal asked for. Prints "N passed, M failed"; exits non-zero on a failure.
import { spawn, execFileSync } from "node:child_process";
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { CHROMIUM, port, vitePort } from "./ports.mjs";
import { hostEnv } from "./lib/host_env.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = resolve(CCORE, "../web-ui");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("lab_concrete");
const VITE = vitePort("lab_concrete");
const TOKEN = "labconc-token";
const PAGE = `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}&tab=concrete`;
const SHOTS = process.env.CONC1_SHOTS ?? ""; // a folder: screenshots at 1280 px (a plan and the 3D view)
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-conc-"));
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

async function tab(S, evalJs, model) {
  const B = (body) => evalJs(`(async () => { const b = await import('/src/bimble.ts'); ${body} })()`);
  const gl = async () => JSON.parse(await evalJs("JSON.stringify(window.__bbGl)"));
  const frames = () => evalJs("window.__bbGl ? window.__bbGl.frames : 0");
  const raf2 = () => evalJs("new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)))");
  const settle = async () => { const f0 = await frames(); await waitFor("a new frame", 3000, async () => (await frames()) > f0).catch(() => {}); for (let i = 0; i < 3; i++) await raf2(); };
  const exists = (sel) => evalJs(`!!document.querySelector(${JSON.stringify(sel)})`);
  const tool = (g, m) => `[data-bb=ribbon] [data-bb=ribbon-tool-${g}-${m}]`;
  const el = (id) => B(`try { return b.query('element', { id: ${JSON.stringify(id)} }); } catch (e) { return null; }`);
  const rev = () => B("return b.state.value.rev;");
  /** WAVE1B (B2): `act` (a statement on `el`) on a control in the page: ms to the first frame holding `cond`. */
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
    return typeof ms === "number" && ms >= 0 ? ms : null;
  };
  const type = (v) => `el.value = ${JSON.stringify(v)}; el.dispatchEvent(new Event('input', { bubbles: true })); el.dispatchEvent(new Event('blur', { bubbles: true }))`;
  /** A ribbon control clicked in the page: ms from the click to the first frame holding `cond`. */
  const timedTool = async (name, sel, cond) => {
    const ms = await evalJs(`(async () => {
      const b = await import('/src/bimble.ts'); const g = () => window.__bbGl;
      const el = document.querySelector(${JSON.stringify(sel)}); if (!el) return -1;
      const f0 = g()?.frames ?? 0, t0 = performance.now(); el.click();
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
  /** A click on the canvas at world (x, y, z) with Shift (no snap): ms from its pointerdown's
   * timeStamp to the end of the first frame after `cond` holds (ADR 0074), or null after 5 s. */
  const screen = async (x, y, z) => {
    const xy = JSON.parse(await evalJs(`JSON.stringify(window.__bbGl.toScreen(${x}, ${y}, ${z}))`));
    if (!xy) throw new Error(`(${x}, ${y}) is behind the camera`);
    if (!(await evalJs(`document.elementFromPoint(${xy[0]}, ${xy[1]})?.id === 'bimble'`))) throw new Error(`(${x}, ${y}) -> ${xy} is not on the canvas`);
    return xy;
  };
  const click = async (name, [x, y, z], cond) => {
    const [sx, sy] = await screen(x, y, z);
    await evalJs(`(() => { window.__concT = null; document.addEventListener('pointerdown', (e) => { window.__concT = e.timeStamp; }, { capture: true, once: true }); window.__concF = window.__bbGl.frames; })()`);
    await S("Input.dispatchMouseEvent", { type: "mouseMoved", x: sx, y: sy, button: "none", buttons: 0, modifiers: 8 });
    await S("Input.dispatchMouseEvent", { type: "mousePressed", x: sx, y: sy, button: "left", buttons: 1, clickCount: 1, modifiers: 8 });
    await S("Input.dispatchMouseEvent", { type: "mouseReleased", x: sx, y: sy, button: "left", buttons: 0, clickCount: 1, modifiers: 8 });
    const ms = await evalJs(`(async () => {
      const b = await import('/src/bimble.ts'); const g = () => window.__bbGl;
      const raf = () => new Promise((r) => requestAnimationFrame(r)); const t0 = performance.now();
      while (performance.now() - t0 < 5000) {
        await raf();
        let ok = false; try { ok = await (async () => (${cond ?? "true"}))(); } catch { ok = false; }
        if (ok && window.__concT !== null) { if (g().frames === window.__concF) await raf(); return performance.now() - window.__concT; }
      }
      return null; })()`);
    if (name && typeof ms === "number") times.push([name, ms]);
    return ms;
  };
  const key = async (k, code, vk) => {
    await S("Input.dispatchKeyEvent", { type: "keyDown", key: k, code, windowsVirtualKeyCode: vk });
    await S("Input.dispatchKeyEvent", { type: "keyUp", key: k, code, windowsVirtualKeyCode: vk });
  };
  const shot = async (name) => {
    if (!SHOTS) return;
    await settle();
    const { data } = await S("Page.captureScreenshot", { format: "png" });
    writeFileSync(join(SHOTS, name), Buffer.from(data, "base64"));
  };

  await S("Page.navigate", { url: PAGE });
  await waitFor("data-bb-ready", 30000, () => evalJs("document.documentElement.dataset.bbReady === '1'"));
  await waitFor("the Concrete ribbon", 10000, () => exists(tool(1, 1)));
  await waitFor("the first frame", 10000, () => evalJs("(window.__bbGl?.frames ?? 0) > 0"));

  // ---- the controls: Stairs and Ramp alone say "not available yet" ----
  const notYet = await evalJs("[...document.querySelectorAll('[data-bb=ribbon] [data-not-yet]')].map((e) => e.getAttribute('data-bb'))");
  check("Concrete: only Stairs and Ramp are 'not available yet'", JSON.stringify(notYet) === JSON.stringify(["ribbon-tool-3-1", "ribbon-tool-3-2"]), JSON.stringify(notYet));
  await evalJs(`document.querySelector(${JSON.stringify(tool(3, 1))}).dispatchEvent(new MouseEvent('mouseenter', { bubbles: true }))`);
  check("Stairs: its tooltip says not available yet", await evalJs(`(() => { const t = document.querySelector(${JSON.stringify(tool(3, 1))})?.getAttribute('title') ?? document.querySelector(${JSON.stringify(tool(3, 1))})?.textContent ?? ''; return /not available yet|pas encore disponible/i.test(t) || !!document.querySelector('[data-bb=not-yet-tip]'); })()`));

  // ---- the plan of Niveau 1 with a wall to the south, fitted ----
  const N1 = "1uAZA0Yfn05f$xRd1k4uPR";
  const views = (await B("return b.openViews.value;")).views;
  const plan = views.find((v) => v.type === "plan" && v.look === "down" && v.storey === N1) ?? views.find((v) => v.type === "plan");
  check("the Niveau 1 plan opens", !(await B(`return b.openView(${JSON.stringify(plan?.id)});`)) && !!plan);
  await waitFor("the plan camera", 10000, async () => (await evalJs("window.__bbGl.camera && window.__bbGl.camera.mode")) === "plan").catch(() => {});
  check("a wall to cut (verb place)", (await B(`return b.runVerb('place', { kind: 'wall', storey: '${N1}', a: [0, -3000], b: [6000, -3000] });`)) === null);
  await settle();
  await B("return b.runAction('fit');");
  await settle();
  const elements = async () => { const r = await B("return b.query('elements');"); return Array.isArray(r) ? r : r.elements ?? []; };
  const wallId = (await elements()).find((e) => e.class === "IfcWall")?.id;
  check("the wall is in the model", !!wallId);
  const undoN = async () => (await B("return b.query('history');")).undo.length;

  // ---- Footing: pad under the column at (0, 0) ----
  let ms = await timedTool("Footing (tool on)", tool(2, 1), "b.placing.value?.active && b.placing.value.kind === 'footing'");
  check("Footing: the place tool is on (footing)", ms !== null);
  let u0 = await undoN();
  // WAVE1B (B5): WAVE1B_PROFILE=1 profiles the first footing click (CDP Profiler, self time per function)
  const prof = !!process.env.WAVE1B_PROFILE;
  if (prof) {
    await evalJs(`(() => { window.__w1calls = []; const o = XMLHttpRequest.prototype.open, sd = XMLHttpRequest.prototype.send; window.__w1orig = [o, sd];
      XMLHttpRequest.prototype.open = function (m, u, ...r) { this.__u = String(u).replace(/[?].*$/, ""); return o.call(this, m, u, ...r); };
      XMLHttpRequest.prototype.send = function (...a) { const t = performance.now(); try { return sd.apply(this, a); } finally { window.__w1calls.push([this.__u.split("/").slice(-2).join("/"), performance.now() - t]); } }; })()`);
    await S("Profiler.enable"); await S("Profiler.setSamplingInterval", { interval: 100 }); await S("Profiler.start");
  }
  ms = await click("Footing: click on a column -> footing drawn", [20, 20, 0], "b.state.value.rev > 0 && window.__bbGl.placed && (window.__bbGl.frames > window.__concF)");
  if (prof) {
    const { profile: p } = await S("Profiler.stop");
    const self = new Map(), dt = p.timeDeltas, byId = new Map(p.nodes.map((n) => [n.id, n]));
    for (let k = 0; k < p.samples.length; k++) {
      const n = byId.get(p.samples[k]), f = n.callFrame, nm = `${f.functionName || "(anonymous)"} ${f.url ? f.url.split("/").pop().split("?")[0] : ""}${f.lineNumber >= 0 ? `:${f.lineNumber + 1}` : ""}`.trim();
      self.set(nm, (self.get(nm) ?? 0) + (dt[k] ?? 0) / 1000);
    }
    const total = [...self.values()].reduce((a, b) => a + b, 0);
    console.log(`profile of the first footing click (${Math.round(ms ?? -1)} ms, load ${readFileSync("/proc/loadavg", "utf8").trim()}): ` +
      [...self.entries()].sort((a, b) => b[1] - a[1]).slice(0, 12).map(([n2, t]) => `${n2} ${t.toFixed(1)} ms (${Math.round((100 * t) / total)} %)`).join("; "));
    await S("Profiler.disable");
    const calls = await evalJs("(() => { [XMLHttpRequest.prototype.open, XMLHttpRequest.prototype.send] = window.__w1orig; return JSON.stringify(window.__w1calls); })()");
    const cl = JSON.parse(calls);
    console.log(`the click's core calls: ${cl.length}, ${cl.reduce((a, c) => a + c[1], 0).toFixed(1)} ms in all; the slowest: ` +
      [...cl].sort((a, b) => b[1] - a[1]).slice(0, 8).map((c) => `${c[0]} ${c[1].toFixed(1)} ms`).join(", "));
  }
  const pad = await gl().then((g) => g.placed);
  const padEl = await el(pad);
  check("Footing: an IfcFooting pad_footing under the column, top at its base", padEl?.class === "IfcFooting" && padEl?.generator === "pad_footing" &&
    padEl.placement.origin[0] === 0 && padEl.placement.origin[1] === 0 && padEl.placement.origin[2] === -500, JSON.stringify(padEl)?.slice(0, 300));
  check("Footing: one history step", (await undoN()) === u0 + 1);
  await B("return b.undo();");
  await settle();
  check("Footing: undo removes it in one step", (await el(pad)) === null && (await undoN()) === u0);
  await B("return b.redo();");
  await settle();

  // ---- Strip footing: one click on the wall ----
  check("Footing panel: Pad / Strip switch", await exists("[data-bb=place-footing-type]"));
  check("Strip footing chosen in the panel", (await B("return b.setPlacing({ kind: 'strip_footing' });")) === null);
  u0 = await undoN();
  const placed0 = (await gl()).placed;
  ms = await click("Strip footing: click on a wall -> footing drawn", [3000, -3000, 0], `window.__bbGl.placed && window.__bbGl.placed !== ${JSON.stringify(placed0)} && window.__bbGl.frames > window.__concF`);
  const strip = (await gl()).placed;
  const stripEl = await el(strip);
  check("Strip footing: IfcFooting along the wall, STRIP_FOOTING, 6000 long", stripEl?.class === "IfcFooting" && stripEl?.generator === "wall" &&
    stripEl.generator_params?.length === 6000 && stripEl.properties?.["Bimble/PredefinedType"] === "STRIP_FOOTING", JSON.stringify(stripEl)?.slice(0, 400));
  check("Strip footing: one step", (await undoN()) === u0 + 1);

  // ---- Opening: two clicks on the wall ----
  ms = await timedTool("Opening (tool on)", tool(4, 1), "b.placing.value?.active && b.placing.value.kind === 'opening'");
  check("Opening: the place tool is on", ms !== null);
  u0 = await undoN();
  await click("Opening: first click", [1000, -3000, 0], "b.placing.value?.points.length === 1");
  ms = await click("Opening: second click -> wall cut", [2000, -3000, 0], `(await (async () => { try { return b.query('element', { id: ${JSON.stringify(wallId)} }).openings === 1; } catch { return false; } })()) && window.__bbGl.frames > window.__concF`);
  let wallEl = await el(wallId);
  check("Opening: the wall has 1 opening, one step", wallEl?.openings === 1 && (await undoN()) === u0 + 1, JSON.stringify(wallEl)?.slice(0, 200));
  const refused = await B(`return b.runVerb('opening_add', { host: ${JSON.stringify(wallId)}, a: [1500, -3000], width: 400 });`);
  check("Opening: refused over another opening, with the reason", /overlaps another opening/.test(String(refused)), String(refused));
  await B("return b.undo();");
  await settle();
  check("Opening: undo removes it in one step", (await el(wallId))?.openings === 0);
  await B("return b.redo();");
  await settle();

  // ---- WAVE1B (B2): the opening's preview, its selection (a click in the plan, its row in
  // Properties, a click in 3D) and its Properties fields. Before B2 the tool answered no ghost for
  // an opening, `query openings` and `opening_at` were unknown and Properties had no opening rows:
  // every check of this block fails there. ----
  const plainClick = async (name, [x, y, z], cond) => {
    const [sx, sy] = await screen(x, y, z);
    await evalJs(`(() => { window.__concT = null; document.addEventListener('pointerdown', (e) => { window.__concT = e.timeStamp; }, { capture: true, once: true }); window.__concF = window.__bbGl.frames; })()`);
    await S("Input.dispatchMouseEvent", { type: "mouseMoved", x: sx, y: sy, button: "none", buttons: 0 });
    await S("Input.dispatchMouseEvent", { type: "mousePressed", x: sx, y: sy, button: "left", buttons: 1, clickCount: 1 });
    await S("Input.dispatchMouseEvent", { type: "mouseReleased", x: sx, y: sy, button: "left", buttons: 0, clickCount: 1 });
    const t = await evalJs(`(async () => {
      const b = await import('/src/bimble.ts'); const g = () => window.__bbGl;
      const raf = () => new Promise((r) => requestAnimationFrame(r)); const t0 = performance.now();
      while (performance.now() - t0 < 5000) {
        await raf();
        let ok = false; try { ok = await (async () => (${cond}))(); } catch { ok = false; }
        if (ok && window.__concT !== null) { if (g().frames === window.__concF) await raf(); return performance.now() - window.__concT; }
      }
      return null; })()`);
    if (typeof t === "number") times.push([name, t]);
    return t;
  };
  if (!(await B("return b.placing.value?.active === true && b.placing.value.kind === 'opening';")))
    await timedTool("Opening (tool on, preview)", tool(4, 1), "b.placing.value?.active && b.placing.value.kind === 'opening'");
  await click(null, [3500, -3000, 0], "b.placing.value?.points.length === 1");
  {
    const [sx, sy] = await screen(4500, -3000, 0);
    await evalJs(`(() => { window.__concT = null; document.addEventListener('pointermove', (e) => { window.__concT = e.timeStamp; }, { capture: true, once: true }); window.__concF = window.__bbGl.frames; })()`);
    await S("Input.dispatchMouseEvent", { type: "mouseMoved", x: sx, y: sy, button: "none", buttons: 0, modifiers: 8 });
    ms = await evalJs(`(async () => { const raf = () => new Promise((r) => requestAnimationFrame(r)); const t0 = performance.now();
      while (performance.now() - t0 < 5000) { await raf(); if ((window.__bbGl.ghostTris ?? 0) > 0 && window.__concT !== null && window.__bbGl.frames > window.__concF) return performance.now() - window.__concT; }
      return null; })()`);
    if (typeof ms === "number") times.push(["Opening: pointer move -> preview drawn", ms]);
    check("B2 Opening preview: the next opening's ghost is drawn while placing", ms !== null && (await gl()).ghostTris > 0, String((await gl()).ghostTris));
  }
  await key("Escape", "Escape", 27);
  await key("Escape", "Escape", 27);
  await settle();
  check("B2 Opening preview: Esc, nothing added", (await el(wallId))?.openings === 1);
  ms = await plainClick("Opening: click in the plan -> selected", [1500, -3000, 0], "!!b.selectedOpening.value && !!document.querySelector('[data-bb=props-opening][aria-pressed=true]')");
  const opId = await B("return b.selectedOpening.value?.id ?? null;");
  check("B2 a click on the opening in the plan selects it: the wall selected, its opening pressed in Properties, its box drawn",
    ms !== null && !!opId && (await B("return b.state.value.selection[0];")) === wallId && (await gl()).openingDrawn === true, String(opId));
  ms = await timed("Opening: width 800 in Properties", "[data-bb=props-opening-width]", type("800"), `b.openingsOf(${JSON.stringify(wallId)})[0]?.width === 800`);
  check("B2 Properties: the opening's width 800 (opening_set, one step), its GlobalId kept", ms !== null && (await B(`return b.openingsOf(${JSON.stringify(wallId)})[0].id;`)) === opId);
  ms = await timed("Opening: sill 1000 in Properties", "[data-bb=props-opening-sill]", type("1000"), `b.openingsOf(${JSON.stringify(wallId)})[0]?.sill === 1000`);
  check("B2 Properties: the opening's sill 1000", ms !== null);
  await B("b.selectedOpening.value = null; return null;");
  ms = await timed("Opening: its row in Properties -> selected", `[data-bb=props-opening][data-id="${opId}"]`, "el.click()", "!!b.selectedOpening.value");
  check("B2 Properties: the host's list of openings selects one", ms !== null && (await B("return b.selectedOpening.value?.id;")) === opId);
  await B("b.selectedOpening.value = null; return b.select([]);");
  await settle();
  { // in 3D: a click on the opening (through the hole, its reveal) selects it as in the plan
    const v3 = views.find((v) => v.type === "three_d");
    await B(`return b.openView(${JSON.stringify(v3?.id)});`);
    await waitFor("the 3D camera", 10000, async () => (await evalJs("window.__bbGl.camera && window.__bbGl.camera.mode")) !== "plan").catch(() => {});
    await settle();
    const sel = await B(`return b.openingsOf(${JSON.stringify(wallId)})[0];`);
    ms = await plainClick("Opening: click in 3D -> selected", sel.centre, "!!b.selectedOpening.value && !!document.querySelector('[data-bb=props-opening][aria-pressed=true]')");
    check("B2 a click on the opening in 3D selects it", ms !== null && (await B("return b.selectedOpening.value?.id;")) === opId);
    await shot("wave1b-opening-selected-3d-1280.png");
    await B("b.selectedOpening.value = null; return b.select([]);");
    await B(`return b.openView(${JSON.stringify(plan.id)});`);
    await waitFor("the plan camera", 10000, async () => (await evalJs("window.__bbGl.camera && window.__bbGl.camera.mode")) === "plan").catch(() => {});
    await settle();
  }

  // ---- Sleeve: one click on the slab of Niveau 1 ----
  ms = await timedTool("Sleeve (tool on)", tool(4, 2), "b.placing.value?.active && b.placing.value.kind === 'sleeve'");
  check("Sleeve: the place tool is on", ms !== null);
  u0 = await undoN();
  ms = await click("Sleeve: click on the slab -> slab cut", [3000, 3000, 0], "b.placing.value && window.__bbGl.placed && window.__bbGl.frames > window.__concF && b.state.value.rev > 0");
  const slabId = (await gl()).placed;
  const slabEl = await el(slabId);
  check("Sleeve: an opening of a slab, one step", slabEl?.class === "IfcSlab" && slabEl?.openings === 1 && (await undoN()) === u0 + 1, JSON.stringify(slabEl)?.slice(0, 200));

  // ---- WAVE1B (B1): the Quantities page lists the sleeve with the exact cylinder (pi r^2 t) ----
  {
    const t0 = await evalJs(`(async () => { const s = await import('/src/shell/send.ts'); const t = performance.now(); s.send({ do: "showPage", page: "quantities" });
      const raf = () => new Promise((r) => requestAnimationFrame(r));
      while (performance.now() - t < 5000) { await raf(); if (document.querySelector('[data-bb=quantities-opening][data-kind=sleeve]')) return performance.now() - t; }
      return null; })()`);
    if (typeof t0 === "number") times.push(["Quantities: the openings and sleeves table shown", t0]);
    const rowTxt = await evalJs("[...document.querySelectorAll('[data-bb=quantities-opening][data-kind=sleeve] td')].map((e) => e.textContent.trim())");
    const want = (Math.PI * 75 * 75 * 220 / 1e9).toFixed(4); // the default 150 mm sleeve through the 220 mm slab
    check("B1 Quantities: the sleeve's own row, Ø 150, its exact cylinder pi r^2 t = " + want + " m3", t0 !== null && rowTxt[3] === "Ø 150" && rowTxt[4] === want, JSON.stringify(rowTxt));
    await evalJs(`(async () => { const s = await import('/src/shell/send.ts'); s.send({ do: "showView" }); })()`);
    await settle();
  }

  // ---- Raft: three corners and Enter ----
  ms = await timedTool("Raft (tool on)", tool(2, 2), "b.placing.value?.active && b.placing.value.kind === 'raft'");
  check("Raft: the place tool is on", ms !== null);
  u0 = await undoN();
  await click("Raft: corner 1", [8000, -3500, 0], "b.placing.value?.points.length === 1");
  await click("Raft: corner 2", [14000, -3500, 0], "b.placing.value?.points.length === 2");
  await click("Raft: corner 3", [14000, -500, 0], "b.placing.value?.points.length === 3");
  const p0 = (await gl()).placed;
  await evalJs(`(() => { window.__concT = null; document.addEventListener('keydown', (e) => { window.__concT = e.timeStamp; }, { capture: true, once: true }); window.__concF = window.__bbGl.frames; })()`);
  await key("Enter", "Enter", 13);
  ms = await evalJs(`(async () => { const raf = () => new Promise((r) => requestAnimationFrame(r)); const t0 = performance.now();
    while (performance.now() - t0 < 5000) { await raf(); if (window.__bbGl.placed !== ${JSON.stringify(p0)} && window.__concT !== null && window.__bbGl.frames > window.__concF) return performance.now() - window.__concT; }
    return null; })()`);
  if (typeof ms === "number") times.push(["Raft: Enter -> raft drawn", ms]);
  const raft = (await gl()).placed;
  const raftEl = await el(raft);
  check("Raft: an IfcSlab BASESLAB on the lowest storey, top at its elevation", raftEl?.class === "IfcSlab" && raftEl?.storey_name === "Fondations" &&
    raftEl.properties?.["Bimble/PredefinedType"] === "BASESLAB" && raftEl.placement.origin[2] === -400, JSON.stringify(raftEl)?.slice(0, 300));
  check("Raft: one step", (await undoN()) === u0 + 1);
  // ---- WAVE1B (B5): the Quantities page's foundations list: pads, the strip, the raft (3.6 m3), no floor slab ----
  {
    const t0 = await evalJs(`(async () => { const s = await import('/src/shell/send.ts'); const t = performance.now(); s.send({ do: "showPage", page: "quantities" });
      const raf = () => new Promise((r) => requestAnimationFrame(r));
      while (performance.now() - t < 5000) { await raf(); if (document.querySelector('[data-bb=quantities-foundation][data-kind=raft]')) return performance.now() - t; }
      return null; })()`);
    if (typeof t0 === "number") times.push(["Quantities: the foundations table shown", t0]);
    const kinds = await evalJs("[...document.querySelectorAll('[data-bb=quantities-foundation]')].map((e) => e.getAttribute('data-kind'))");
    const raftTd = await evalJs("[...document.querySelectorAll('[data-bb=quantities-foundation][data-kind=raft] td')].map((e) => e.textContent.trim())");
    check("B5 Quantities: a foundations list with the pads, the strip and the raft (3.600 m3: 6 x 3 / 2 x 0.4), apart from the slabs",
      t0 !== null && kinds.includes("pad") && kinds.includes("strip") && raftTd[4] === "3.600" && !kinds.includes("slab"), JSON.stringify({ kinds, raftTd }));
    await evalJs(`(async () => { const s = await import('/src/shell/send.ts'); s.send({ do: "showView" }); })()`);
    await settle();
  }
  await key("Escape", "Escape", 27);
  await shot("conc1-plan-1280.png");

  // ---- save and reopen: the same bytes; the IFC export holds the classes and the relation ----
  check("save", (await B("return b.runAction('save');")) === null);
  await sleep(300);
  const saved1 = readFileSync(model);
  const reopened = await B(`try { b.actionBody('open', { path: ${JSON.stringify(model)} }); return null; } catch (e) { return String(e); }`);
  check("reopen the saved file", reopened === null, String(reopened));
  await settle();
  const model2 = join(dirname(model), "again.json");
  const sa = await B(`try { b.actionBody('save_as', { path: ${JSON.stringify(model2)} }); return null; } catch (e) { return String(e); }`);
  await sleep(300);
  check("save, reopen, save as: the same bytes", sa === null && existsSync(model2) && Buffer.compare(saved1, readFileSync(model2)) === 0, String(sa));
  const m = JSON.parse(saved1.toString("utf8")).model;
  const els = Object.values(m.elements);
  check("the file: footings, the raft, the wall's and the slab's openings", els.filter((e) => e.class === "IfcFooting").length === 12 + 2 &&
    els.some((e) => e.properties?.["Bimble/PredefinedType"] === "BASESLAB") && els.filter((e) => (e.openings ?? []).length).length === 2);
  const ifc = join(dirname(model), "conc1.ifc");
  const ex = await B(`try { b.actionBody('export_ifc4', { path: ${JSON.stringify(ifc)} }); return null; } catch (e) { return String(e); }`);
  const text = existsSync(ifc) ? readFileSync(ifc, "utf8") : "";
  check("IFC4 export: IfcFooting PAD_FOOTING and STRIP_FOOTING, IfcSlab BASESLAB, IfcOpeningElement, IfcRelVoidsElement, the sleeve",
    ex === null && /IFCFOOTING\(.*\.STRIP_FOOTING\.\)/.test(text) && /IFCSLAB\(.*\.BASESLAB\.\)/.test(text) && /IFCOPENINGELEMENT\(/.test(text) &&
    (text.match(/IFCRELVOIDSELEMENT\(/g) ?? []).length === 2 && /'SLEEVE'/.test(text), String(ex));
  if (process.env.CONC1_IFC) copyFileSync(ifc, process.env.CONC1_IFC);
  // ---- WAVE1B (B3): the export imported back: the strip footing a strip, the raft BASESLAB, the
  // wall's opening and the slab's sleeve parametric again (editable in Properties). Before B3 the
  // openings came back as meshes ("other") and the strip as a pad: these checks fail there. ----
  {
    const t0 = Date.now();
    const imp = await B(`try { b.actionBody('import_ifc', { path: ${JSON.stringify(ifc)} }); return null; } catch (e) { return String(e); }`);
    await settle();
    const ms3 = Date.now() - t0;
    times.push(["Import IFC (the export back, 79 elements)", ms3]);
    const els3 = await elements();
    const w3 = els3.find((e) => e.id === wallId), s3 = els3.find((e) => e.id === slabId), f3 = els3.find((e) => e.id === strip), r3 = els3.find((e) => e.id === raft);
    const ow = w3 ? await B(`return b.openingsOf(${JSON.stringify(wallId)});`) : [], os = s3 ? await B(`return b.openingsOf(${JSON.stringify(slabId)});`) : [];
    const fe = f3 ? await el(strip) : null, re = r3 ? await el(raft) : null;
    check("B3 import: the wall's opening a box again, the slab's sleeve a sleeve, the strip a strip, the raft BASESLAB",
      imp === null && ow.length === 1 && ow[0].kind === "opening" && os.some((o) => o.kind === "sleeve") && fe?.generator === "wall" &&
      fe?.properties?.["Bimble/PredefinedType"] === "STRIP_FOOTING" && re?.properties?.["Bimble/PredefinedType"] === "BASESLAB",
      JSON.stringify({ imp, ow: ow.map((o) => o.kind), os: os.map((o) => o.kind), fe: fe?.generator, re: re?.properties }));
  }

  // ---- the 3D view with the new elements ----
  if (SHOTS) {
    const v3 = views.find((v) => v.type === "three_d");
    if (v3) await B(`return b.openView(${JSON.stringify(v3.id)});`);
    await settle();
    await B("return b.runAction('fit');");
    await shot("conc1-3d-1280.png");
  }
  console.log(`load: ${readFileSync("/proc/loadavg", "utf8").trim()}`);
}

async function main() {
  if (!existsSync(join(UI, "node_modules"))) throw new Error(`no ${UI}/node_modules: run npm ci in web-ui/`);
  execFileSync("node", [join(UI, "scripts/copy-ccore.mjs")], { stdio: "ignore" });
  const model = join(tmp, "frame.json"); // JSON: saved as JSON, read back here
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
  await tab(S, evalJs, model);

  const IGNORED = [
    /Synchronous XMLHttpRequest on the main thread is deprecated/,
    /404 \(Not Found\) http:\/\/localhost:\d+\/favicon\.ico$/,
    // The refusal this test asks for (an opening over another): bimble.ts's sync XHR line.
    new RegExp(`400 \\(Bad Request\\) http://localhost:${VITE}/src/bimble\\.ts$`),
  ];
  const errors = consoleLog.filter((m) => /error|warn/.test(m.level) && !IGNORED.some((r) => r.test(m.text)));
  check("no uncaught page error", exceptions.length === 0, exceptions.join(" | "));
  check("no console error or warning", errors.length === 0, errors.map((m) => m.text).join(" | "));
  const badResp = failedResp.filter((r) => !/^404 http:\/\/localhost:\d+\/favicon\.ico$/.test(r) &&
    !new RegExp(`^400 http://127\\.0\\.0\\.1:${PORT}/verb/opening_add\\?`).test(r) &&
    !new RegExp(`^400 http://127\\.0\\.0\\.1:${PORT}/query/element\\?`).test(r)); // the undone footing asked for
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
