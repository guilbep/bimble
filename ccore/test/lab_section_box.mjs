// The section box of issue #71 in the lab's WebGL2 viewport (ticket ISS71): node ccore/test/lab_section_box.mjs
// Runs the same scenario on the native host (out/tcc/bimble-ccore, or $HOST_BIN) and on the C core's wasm
// module in the page (LAB_SB_MODES=native,wasm: both by default). Model: golden/frame.json with a
// georeference whose map turns 30° (true north = +30°, project north = the model's axes), and, south of
// the frame, a slab and three columns placed through the verb `place`: C, E 0.5 m beside C, D 2 m beside
// C. Vite from web-ui/, headless Chromium over raw CDP, real mouse events, looking down (action top):
//   box to selection: C clicked, the View tab's Box to selection -> the box = C's bounds + 1 m: D (2 m
//     away) is clipped (background at its pixel), E (0.5 m away) and C are drawn;
//   handles: exactly one arrowhead per face (__bbGl.boxHandles, faces 0..5), each at its face centre
//     on screen, drawn in the theme's --bb-clay on the overlay; hover on the +x one: boxHover 1;
//     dragging it moves the +x face (max x grows, min x kept);
//   the right-click menu: the box around D, then off; a right click on C selects it and opens the menu
//     (data-bb=viewport-menu: Box to selection, Hide, Isolate, then ISS72's Show all, Show previous as
//     enabled), its Box to selection item -> the box around C again, on;
//   true north: the View tab's "Géo" (box_north_true) -> yaw 30°, the box still holds C; a point of
//     the slab 250 mm inside a corner of the turned box (outside the unturned one) is drawn, one 250 mm
//     outside its +x side (inside the unturned box) is not; the arrowheads at the turned face centres.
// No uncaught page error; no console error or warning but the sync-XHR deprecation and the favicon 404.
// Prints "N passed, M failed"; exits non-zero on a failure. Kills all it starts.
import { execFileSync, spawn } from "node:child_process";
import { existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { CHROMIUM, port, vitePort } from "./ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = resolve(CCORE, "../web-ui");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("lab_section_box", 47389);
const VITE = vitePort("lab_section_box", 5152);
const TOKEN = "labsectionbox-token";
const MODES = (process.env.LAB_SB_MODES ?? "native,wasm").split(",").filter(Boolean);
const STOREY = "1uAZA0Yfn05f$xRd1k4uPR"; // frame.json's Niveau 1 (elevation 0)
const STOREY_BELOW = "04Aobkc8v6$94cGl_9HNVX"; // Fondations (elevation -800)
const MARGIN = 1000; // ccore/session/visibility.h CC_BOX_SELECTION_MARGIN_MM
const X0 = 3000, Y0 = -9000; // south of the frame: nothing else near
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-section-box-"));
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

const turnZ = (p, a) => [Math.cos(a) * p[0] - Math.sin(a) * p[1], Math.sin(a) * p[0] + Math.cos(a) * p[1], p[2]];

/** One run of the scenario in a new tab: `mode` native (the host on `model`) or wasm (the model opened in the page). */
async function scenario(cdp, mode, geoText) {
  const tag = `[${mode}]`;
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
  for (const d of ["Runtime", "Log", "Page"]) await S(`${d}.enable`);
  await S("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  const page = mode === "native" ? `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}` : `http://localhost:${VITE}/?ccore=1&gl=1`;
  await S("Page.navigate", { url: page });
  const evalJs = async (expr) => {
    const r = await S("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
    return r.result.value;
  };
  const B = (body) => evalJs(`(async () => { const b = await import('/src/bimble.ts'); ${body} })()`);
  await waitFor("the first drawn frame", 60000, () => evalJs("window.__bbGl && window.__bbGl.firstFrameMs"));
  await waitFor("a model", 30000, () => B("return b.state.value.file !== null && b.placing.value !== null;"));
  if (mode === "wasm") { // the georeferenced model, opened in the page
    const ok = await evalJs(`(async () => { const c = await import('/src/ccore.ts');
      try { return await c.ccoreOpenFile(new File([${JSON.stringify(geoText)}], 'geo30.bimble'), true); } catch (e) { return String(e); } })()`);
    if (ok !== true) throw new Error(`ccoreOpenFile: ${ok}`);
  }
  await waitFor("geo30.bimble", 15000, () => B("return b.state.value.file === 'geo30.bimble';"));
  const gl = async () => JSON.parse(await evalJs("JSON.stringify(window.__bbGl)"));
  const frames = () => evalJs("window.__bbGl.frames");
  const settle = async () => {
    const until = Date.now() + 5000;
    let f = await frames(), since = Date.now();
    while (Date.now() - since < 1200 && Date.now() < until) {
      await sleep(50);
      const g = await frames();
      if (g !== f) (f = g), (since = Date.now());
    }
  };
  const vis = () => B("return b.query('visibility');");
  const sel = () => B("return b.state.value.selection;");
  const screen = async (w) => {
    const xy = JSON.parse(await evalJs(`JSON.stringify(window.__bbGl.toScreen(${w[0]}, ${w[1]}, ${w[2]}))`));
    if (!xy) throw new Error(`${w} is behind the camera`);
    return xy;
  };
  const pixelAt = ([x, y]) => evalJs(`(() => { const c = document.getElementById('bimble'), g = c.getContext('webgl2'), r = c.getBoundingClientRect();
    const p = new Uint8Array(4); g.readPixels(Math.round(${x} - r.left), c.height - 1 - Math.round(${y} - r.top), 1, 1, g.RGBA, g.UNSIGNED_BYTE, p); return [p[0], p[1], p[2]]; })()`);
  const bg = (p) => Math.abs(p[0] - 247) <= 3 && Math.abs(p[1] - 247) <= 3 && Math.abs(p[2] - 245) <= 3;
  const ink = (p) => !bg(p);
  const mouse = async (type, [x, y], button = "left", buttons = 0) =>
    S("Input.dispatchMouseEvent", { type, x, y, button, buttons, clickCount: type === "mouseMoved" ? 0 : 1 });
  const clickAt = async (xy, button = "left") => {
    await mouse("mousePressed", xy, button, button === "left" ? 1 : 2);
    await mouse("mouseReleased", xy, button, 0);
  };
  const clickEl = async (sel2) => {
    const xy = await evalJs(`(() => { const el = document.querySelector(${JSON.stringify(sel2)}); if (!el) return null; const r = el.getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; })()`);
    if (!xy) throw new Error(`no ${sel2}`);
    await clickAt(xy);
  };

  // ---- the slab and the three columns ----
  const before = new Set(await B("return b.query('elements', { limit: 100000 }).map((e) => e.id);"));
  // The slab on the storey below (its top under the columns' feet: never level with a column's top).
  for (const [kind, a, bb] of [["slab", [X0 - 3000, Y0 - 3000], [X0 + 3000, Y0 + 3000]], ["column", [X0, Y0]], ["column", [X0 + 800, Y0]], ["column", [X0 + 2300, Y0]]]) {
    const err = await B(`return b.runVerb('place', ${JSON.stringify({ kind, storey: kind === "slab" ? STOREY_BELOW : STOREY, a, ...(bb ? { b: bb } : {}) })});`);
    if (err) throw new Error(`place ${kind}: ${err}`);
  }
  await waitFor("the four new elements drawn", 10000, async () => {
    const ids = await B("return b.query('elements', { limit: 100000 }).map((e) => e.id);");
    const n = ids.filter((id) => !before.has(id));
    return n.length === 4 && (await evalJs(`${JSON.stringify(n)}.every((id) => window.__bbGl.elementBox(id) !== null)`));
  });
  const fresh = (await B("return b.query('elements', { limit: 100000 }).map((e) => e.id);")).filter((id) => !before.has(id));
  const boxOf = async (id) => evalJs(`window.__bbGl.elementBox(${JSON.stringify(id)})`);
  const placed = [];
  for (const id of fresh) placed.push({ id, box: await boxOf(id) });
  const cx = (b) => (b.min[0] + b.max[0]) / 2;
  const slab = placed.find((e) => e.box.max[0] - e.box.min[0] > 5000);
  const cols = placed.filter((e) => e !== slab).sort((a, b) => cx(a.box) - cx(b.box));
  const [C, E, D] = cols;
  check(`${tag} placed: a slab and columns C, E (0.5 m beside), D (2 m beside)`, slab && C && E && D &&
    Math.abs((E.box.min[0] - C.box.max[0]) - 500) < 1 && Math.abs((D.box.min[0] - C.box.max[0]) - 2000) < 1, JSON.stringify(placed.map((e) => e.box)));
  await B("return b.runAction('top');");
  await B("return b.runAction('fit');");
  await B("return b.select([]);");
  await settle();
  const topOf = (e) => [cx(e.box), (e.box.min[1] + e.box.max[1]) / 2, e.box.max[2] - 1];
  const px = async (e) => pixelAt(await screen(topOf(e)));
  // D clipped, E and C drawn, read in one sample (a layout change moving the canvas fails all three),
  // again until true or 5 s: the modifier panel a selection opens resizes the canvas a moment later.
  const cut3 = async () => { const p = [await px(C), await px(E), await px(D)]; return { ok: ink(p[0]) && ink(p[1]) && bg(p[2]), p }; };
  const cutNow = async () => (await waitFor("D clipped, E and C drawn", 5000, async () => (await cut3()).ok && (await cut3())).catch(() => null)) ?? (await cut3());

  // ---- Box to selection from the View tab ----
  await clickAt(await screen(topOf(C)));
  await waitFor("C selected", 5000, async () => (await sel()).includes(C.id));
  const viewTab = await evalJs(`(async () => { const { t } = await import('/src/i18n/index.ts'); const label = t('tab.view');
    const el = [...document.querySelectorAll('[role=tab], .tab, button, a')].find((e) => e.textContent.trim() === label); if (!el) return null;
    const r = el.getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; })()`);
  if (viewTab) await clickAt(viewTab);
  const toolXY = async (key) => evalJs(`(async () => { const { t } = await import('/src/i18n/index.ts'); const label = t(${JSON.stringify(key)});
    const el = [...document.querySelectorAll('.tool')].find((e) => e.textContent.trim() === label); if (!el) return null;
    const r = el.getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; })()`);
  const bts = await waitFor("the Box to selection button", 5000, () => toolXY("ribbon.view.g2.t2"));
  const tipText = await evalJs(`(async () => { const { t } = await import('/src/i18n/index.ts'); const label = t('ribbon.view.g2.t2');
    return [...document.querySelectorAll('.tool')].find((e) => e.textContent.trim() === label)?.title ?? null; })()`);
  check(`${tag} the Box to selection button's tooltip says the 1 m`, /\b1 m\b/.test(tipText ?? ""), tipText);
  await clickAt(bts);
  await waitFor("the box on", 5000, async () => (await vis()).section_box.on === true);
  await settle();
  let v = (await vis()).section_box, o = v.origin_mm;
  const wantMin = [0, 1, 2].map((k) => C.box.min[k] - o[k] - MARGIN), wantMax = [0, 1, 2].map((k) => C.box.max[k] - o[k] + MARGIN);
  check(`${tag} ribbon Box to selection: the box = C's bounds ± 1000 mm, on`, v.on && v.box.yaw === 0 &&
    [0, 1, 2].every((k) => Math.abs(v.box.min[k] - wantMin[k]) < 1 && Math.abs(v.box.max[k] - wantMax[k]) < 1), JSON.stringify(v.box));
  const c3 = await cutNow(), [pC, pE, pD] = c3.p;
  check(`${tag} D, 2 m away, is clipped (background at its pixel)`, bg(pD), JSON.stringify(pD));
  check(`${tag} E, 0.5 m away, and C are drawn`, ink(pE) && ink(pC), `${JSON.stringify(pE)} ${JSON.stringify(pC)}`);

  // ---- the arrowheads ----
  const expectHandles = async (box) => {
    const r = await evalJs("(() => { const r = document.getElementById('bimble').getBoundingClientRect(); return [r.left, r.top]; })()");
    const out = [];
    for (let f = 0; f < 6; f++) {
      const c = [0, 1, 2].map((k) => (box.min[k] + box.max[k]) / 2), a = Math.floor(f / 2);
      c[a] = f & 1 ? box.max[a] : box.min[a];
      const s = turnZ(c, box.yaw);
      const xy = await screen([s[0] + o[0], s[1] + o[1], s[2] + o[2]]);
      out.push([xy[0] - r[0], xy[1] - r[1]]);
    }
    return { out, rect: r };
  };
  let g = await gl();
  let ex = await expectHandles(v.box);
  const faces = g.boxHandles.map((h) => h.face).sort().join(",");
  check(`${tag} exactly one arrowhead per face (6)`, g.boxHandles.length === 6 && faces === "0,1,2,3,4,5", JSON.stringify(g.boxHandles));
  check(`${tag} each arrowhead at its face centre on screen (1.5 px)`, g.boxHandles.every((h) => Math.hypot(h.x - ex.out[h.face][0], h.y - ex.out[h.face][1]) <= 1.5),
    JSON.stringify(g.boxHandles.map((h) => [h.face, Math.round(h.x), Math.round(h.y), ex.out[h.face].map(Math.round)])));
  const h1 = g.boxHandles.find((h) => h.face === 1);
  const ov = await evalJs(`(() => { const c = document.querySelector('canvas[data-bb-overlay]'), d = window.devicePixelRatio || 1;
    const p = c.getContext('2d').getImageData(Math.round((${h1.x} + ${h1.dx} * 3) * d), Math.round((${h1.y} + ${h1.dy} * 3) * d), 1, 1).data;
    const clay = getComputedStyle(document.documentElement).getPropertyValue('--bb-clay').trim(); return [p[0], p[1], p[2], p[3], clay]; })()`);
  const clayRgb = ((h) => [1, 3, 5].map((i) => parseInt(h.slice(i, i + 2), 16)))(ov[4] || "#C06A41");
  check(`${tag} the +x arrowhead is drawn in the theme's --bb-clay (${ov[4]})`, ov[3] > 200 && [0, 1, 2].every((k) => Math.abs(ov[k] - clayRgb[k]) <= 12), JSON.stringify(ov));
  const h1c = [h1.x + ex.rect[0], h1.y + ex.rect[1]];
  await mouse("mouseMoved", h1c);
  await waitFor("the hover", 3000, async () => (await gl()).boxHover === 1).catch(() => null);
  g = await gl();
  check(`${tag} hover on the +x arrowhead: boxHover 1, grab cursor`, g.boxHover === 1 && (await evalJs("document.getElementById('bimble').style.cursor")) === "grab", `${g.boxHover}`);
  // Drag it 60 px further out along its screen direction.
  const max0 = v.box.max[0], min0 = v.box.min[0];
  await mouse("mousePressed", h1c, "left", 1);
  g = await gl();
  check(`${tag} press on the +x arrowhead: the box tool holds face 1`, g.boxFace === 1, `${g.boxFace} ${g.boxError ?? ""}`);
  for (let i = 1; i <= 6; i++) {
    const f0 = await frames();
    await mouse("mouseMoved", [h1c[0] + h1.dx * 10 * i, h1c[1] + h1.dy * 10 * i], "left", 1);
    await waitFor("a frame", 5000, async () => (await frames()) > f0);
  }
  await mouse("mouseReleased", [h1c[0] + h1.dx * 60, h1c[1] + h1.dy * 60], "left", 0);
  await waitFor("the box released", 5000, async () => (await vis()).section_box.box.max[0] > max0 + 100);
  v = (await vis()).section_box;
  check(`${tag} dragging the +x arrowhead moves the +x face out (min x kept)`, v.box.max[0] > max0 + 100 && v.box.min[0] === min0, `${max0} -> ${v.box.max[0]}, min ${min0} -> ${v.box.min[0]}`);

  // ---- the right-click menu ----
  await B(`return b.select([${JSON.stringify(D.id)}]);`);
  await B("return b.runAction('box_to_selection');");
  await B("return b.runAction('section_box');"); // off: C drawn again, to be right-clicked
  await waitFor("the box off", 5000, async () => (await vis()).section_box.on === false);
  await settle();
  const m0 = (await gl()).menus;
  await clickAt(await screen(topOf(C)), "right");
  await waitFor("the menu", 5000, async () => (await gl()).menus > m0 && (await evalJs("!!document.querySelector('[data-bb=viewport-menu]')")));
  await waitFor("C selected by the right click", 5000, async () => (await sel()).includes(C.id));
  const items = await evalJs("[...document.querySelectorAll('[data-bb=viewport-menu] [role=menuitem]')].map((e) => e.dataset.bb + (e.disabled ? ':off' : ''))");
  // ISS72's Show all and Show previous follow, greyed out as query actions says.
  const on72 = await B("const a = b.query('actions'); return ['show_all', 'show_previous'].map((n) => `ctx-${n}` + (a.find((x) => x.name === n)?.enabled ? '' : ':off'));");
  check(`${tag} right click on C: the menu (Box to selection, Hide, Isolate, Show all, Show previous), C selected`,
    JSON.stringify(items) === JSON.stringify(["ctx-box_to_selection", "ctx-hide_now", "ctx-isolate_now", ...on72]), JSON.stringify(items));
  await clickEl("[data-bb=ctx-box_to_selection]");
  await waitFor("the box on, around C", 5000, async () => { const b = (await vis()).section_box; return b.on && Math.abs(b.box.min[0] - wantMin[0]) < 1; }).catch(() => null);
  await settle();
  // FIXUI 7: the title's three claims, each tested (the predicate checked only the menu).
  const bxm = (await vis()).section_box;
  check(`${tag} the menu's Box to selection: the box around C again, on; the menu closed`,
    bxm.on && [0, 1, 2].every((k) => Math.abs(bxm.box.min[k] - wantMin[k]) < 1 && Math.abs(bxm.box.max[k] - wantMax[k]) < 1) &&
    !(await evalJs("!!document.querySelector('[data-bb=viewport-menu]')")), JSON.stringify(bxm));
  const c3m = await cutNow();
  check(`${tag} after the menu: D clipped, E and C drawn`, c3m.ok, JSON.stringify(c3m.p));

  // ---- true north (the map turns 30°) ----
  await waitFor("the orientation control", 5000, () => evalJs("!!document.querySelector('[data-bb=box-north-box_north_true]:not([disabled])')"));
  await clickEl("[data-bb=box-north-box_north_true]");
  await waitFor("the box turned", 5000, async () => Math.abs((await vis()).section_box.box.yaw - Math.PI / 6) < 1e-6);
  await settle();
  v = (await vis()).section_box;
  const bx = v.box, inFrame = (w) => turnZ([w[0] - o[0], w[1] - o[1], w[2] - o[2]], -bx.yaw);
  const holdsC = [0, 1].every((i) => [C.box.min, C.box.max].every((cc, j) => {
    const q = inFrame([i ? C.box.max[0] : C.box.min[0], j ? C.box.max[1] : C.box.min[1], C.box.min[2]]);
    return q[0] >= bx.min[0] && q[0] <= bx.max[0] && q[1] >= bx.min[1] && q[1] <= bx.max[1];
  }));
  check(`${tag} Géo: yaw 30°, pressed, the box still holds C`, Math.abs(bx.yaw - Math.PI / 6) < 1e-6 && holdsC &&
    (await evalJs("document.querySelector('[data-bb=box-north-box_north_true]').getAttribute('aria-checked')")) === "true", JSON.stringify(bx));
  const zs = slab.box.max[2] - o[2] - 1; // just under the slab's top (scene)
  const toWorld = (q) => { const s = turnZ([q[0], q[1], zs], bx.yaw); return [s[0] + o[0], s[1] + o[1], s[2] + o[2]]; };
  // 250 mm (6 px or so) from the turned box's sides: clear of its edges drawn over the model.
  const pIn = toWorld([bx.max[0] - 250, bx.max[1] - 250]), pOut = toWorld([bx.max[0] + 250, (bx.min[1] + bx.max[1]) / 2 + 300]);
  // The same two points against the unturned box around C (what the old axis-aligned shader cut).
  const un = (w) => [0, 1].every((k) => w[k] - o[k] >= wantMin[k] && w[k] - o[k] <= wantMax[k]);
  const two = async () => [await pixelAt(await screen(pIn)), await pixelAt(await screen(pOut))];
  const [qIn, qOut] = (await waitFor("the turned cut", 5000, async () => { const q = await two(); return ink(q[0]) && bg(q[1]) && q; }).catch(() => null)) ?? (await two());
  check(`${tag} a slab point just inside the turned box's corner is drawn (outside the unturned box: ${!un(pIn)})`, ink(qIn) && !un(pIn), JSON.stringify(qIn));
  check(`${tag} a slab point just outside the turned box's +x side is not (inside the unturned box: ${un(pOut)})`, bg(qOut) && un(pOut), JSON.stringify(qOut));
  g = await gl();
  ex = await expectHandles(bx);
  check(`${tag} the turned box: one arrowhead per face at its turned face centre (1.5 px)`, g.boxHandles.length === 6 &&
    g.boxHandles.every((h) => Math.hypot(h.x - ex.out[h.face][0], h.y - ex.out[h.face][1]) <= 1.5), JSON.stringify(g.boxHandles.map((h) => [h.face, Math.round(h.x), Math.round(h.y)])));

  if (process.env.SHOTS) { const { data } = await S("Page.captureScreenshot", { format: "png" }); writeFileSync(join(process.env.SHOTS, `iss71-${mode}-true-north.png`), Buffer.from(data, "base64")); }
  const IGNORED = [/Synchronous XMLHttpRequest on the main thread is deprecated/, /404 \(Not Found\) http:\/\/localhost:\d+\/favicon\.ico$/];
  const errors = consoleLog.filter((m) => /error|warn/.test(m.level) && !IGNORED.some((r) => r.test(m.text)));
  check(`${tag} no uncaught page error`, exceptions.length === 0, exceptions.join(" | "));
  check(`${tag} no console error or warning`, errors.length === 0, errors.map((m) => m.text).join(" | "));
  await cdp.send("Target.closeTarget", { targetId });
}

async function main() {
  if (!existsSync(join(UI, "node_modules"))) throw new Error(`no ${UI}/node_modules: run npm ci in web-ui/`);
  const fj = JSON.parse(readFileSync(join(CCORE, "golden/frame.json"), "utf8"));
  fj.model.georef = { map_rotation_deg: -30 }; // true north: plan_turn +30° (georef.h)
  const geoText = JSON.stringify(fj);
  const model = join(tmp, "geo30.bimble");
  writeFileSync(model, geoText);
  if (MODES.includes("native")) {
    const host = start("host", BIN, [String(PORT), model], {
      env: { ...process.env, BIMBLE_TOKEN: TOKEN, BIMBLE_ORIGINS: `http://localhost:${VITE},http://127.0.0.1:${VITE}` },
    });
    await waitFor("host 'listening'", 5000, () => host.out.includes("listening") || (host.exitCode !== null && Promise.reject(new Error(host.out))));
  }
  if (MODES.includes("wasm")) execFileSync("node", [join(UI, "scripts/copy-ccore.mjs")], { stdio: "ignore" });
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
  for (const mode of MODES) {
    try {
      await scenario(cdp, mode, geoText);
    } catch (e) {
      check(`[${mode}] no exception in the scenario`, false, e.stack ?? String(e));
    }
  }
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
