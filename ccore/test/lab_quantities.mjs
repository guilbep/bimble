// Quantities, the command palette and the view switches in the lab (ticket Q2): node ccore/test/lab_quantities.mjs
// Starts out/tcc/bimble-ccore (or $HOST_BIN) on a copy of golden/frame.json, copies the wasm build
// (scripts/copy-ccore.mjs), Vite from web-ui/ and headless /usr/bin/chromium over raw CDP. Twice,
// on the native host (/?native=1&gl=1&page=quantities) and on ccore.wasm (/?ccore=1&gl=1&page=quantities):
//   Quantities: the page's schedule_rows("All elements") is golden/schedule/frame/all-elements.json
//     (Rust's text); the table shows "All elements", its 50 rows (truncated note: 50 of 72), its
//     group lines and the total line's cells as Rust's; Export CSV (the page's download path, the
//     anchor's Blob read back) gives golden/schedule/frame/all-elements.csv; a real click on a row
//     selects its element (state.selection, the row drawn selected); the group select set to "none"
//     (set_schedule, one history step) leaves no group line and the rows change; undo restores it.
//   Palette: Ctrl+K opens it (the query field focused), "section box" typed lists fewer items (the
//     C core's palette_match), Enter runs the first one: Section box, its pressed state turns
//     (query and the page's `actions`), recent_actions starts with it, an empty query lists it first.
//   View switches (glviewport.ts Q2 blocks): Edges off draws no edge (the dark pixels go) and on
//     again; Fit all after a wheel zoom gives the fitted camera back; Orthographic projects a line
//     of sight to one point; Top view sets pitch 1.5 and yaw -π/2; a click from that view picks
//     the top slab (parallel rays).
// Before Q2 the C core answered schedule_list [] and refused schedule_rows: the page showed its
// empty note ("quantities.empty"), which fails every check above; palette_match was refused (the
// palette threw from its computed list), recent_actions was [] and the five switches were never
// enabled (runAction answered "action … is not enabled now").
// No uncaught page error; no console error or warning but the sync-XHR deprecation and the favicon
// 404. Prints "N passed, M failed"; exits non-zero on a failure. Kills all it starts.
import { spawn, execFileSync } from "node:child_process";
import { copyFileSync, existsSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { CHROMIUM, port, vitePort } from "./ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = resolve(CCORE, "../web-ui");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("lab_quantities");
const VITE = vitePort("lab_quantities");
const TOKEN = "labquantities-token";
const PAGES = {
  native: `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}&page=quantities`,
  wasm: `http://localhost:${VITE}/?ccore=1&gl=1&page=quantities`,
};
const golden = (rel) => readFileSync(join(CCORE, "golden", rel), "utf8");
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-quantities-"));
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

/** The Quantities page on one client (native or wasm): see the header. */
async function quantities(mode, S, evalJs) {
  const B = (body) => evalJs(`(async () => { const b = await import('/src/bimble.ts'); ${body} })()`);
  await S("Page.navigate", { url: PAGES[mode] });
  await waitFor(`${mode}: data-bb-ready`, 30000, () => evalJs("document.documentElement.dataset.bbReady === '1'"));
  await waitFor(`${mode}: the table`, 10000, () => evalJs("!!document.querySelector('[data-bb=quantities-table] tbody tr')"));
  const want = JSON.parse(golden("schedule/frame/all-elements.json"));
  const rowsText = await B("return JSON.stringify(b.scheduleRows('All elements'));");
  check(`${mode}: the page's schedule_rows = golden all-elements.json`, rowsText === golden("schedule/frame/all-elements.json").trimEnd(), rowsText?.slice(0, 120));
  const dom = JSON.parse(await evalJs(`JSON.stringify((() => {
    const t = document.querySelector('[data-bb=quantities-table]');
    const cells = (tr) => [...tr.querySelectorAll('td')].map((td) => td.textContent.trim());
    return {
      title: document.querySelector('[data-bb=quantities-title]')?.textContent.trim(),
      head: [...t.querySelectorAll('thead th')].map((th) => th.textContent.trim()),
      groups: [...t.querySelectorAll('tbody tr.g')].map(cells),
      rows: [...t.querySelectorAll('tbody tr:not(.g)')].map(cells),
      total: [...t.querySelectorAll('tfoot td')].map((td) => td.textContent.trim()),
      truncated: !!document.querySelector('[data-bb=quantities-truncated]'),
      empty: document.querySelector('[data-bb=quantities]')?.textContent.includes('quantities.empty') ?? false,
    };
  })())`));
  check(`${mode}: the title is the schedule's name`, dom.title === want.name, dom.title);
  check(`${mode}: ${want.columns.length} columns`, dom.head.length === want.columns.length, JSON.stringify(dom.head));
  const wantRows = want.groups.flatMap((g) => g.rows);
  check(`${mode}: the rows are Rust's (${wantRows.length} of ${want.count}, truncated note)`,
    JSON.stringify(dom.rows) === JSON.stringify(wantRows) && dom.truncated === want.truncated, `${dom.rows.length} rows; ${JSON.stringify(dom.rows[0])}`);
  check(`${mode}: one group line per level`, dom.groups.length === want.groups.filter((g) => g.level).length, JSON.stringify(dom.groups));
  const span = (c) => { const i = c.findIndex((x) => x !== ""); return Math.max(1, i < 0 ? c.length : i); };
  check(`${mode}: the total line's values are Rust's`, JSON.stringify(dom.total.slice(1)) === JSON.stringify(want.total.slice(span(want.total))),
    `${JSON.stringify(dom.total)} vs ${JSON.stringify(want.total)}`);

  // Export CSV: the page's own path (an anchor on a Blob URL, clicked); the Blob read back.
  await evalJs(`(() => {
    window.__q2csv = null;
    const click = HTMLAnchorElement.prototype.click, mk = URL.createObjectURL, blobs = new Map();
    URL.createObjectURL = (b) => { const u = mk.call(URL, b); blobs.set(u, b); return u; };
    HTMLAnchorElement.prototype.click = function () {
      const b = blobs.get(this.href);
      if (b) { b.text().then((t) => { window.__q2csv = { name: this.download, text: t, type: b.type }; }); return; }
      return click.call(this);
    };
    return true;
  })()`);
  await evalJs("document.querySelector('[data-bb=quantities-export]').click(), true");
  const csv = await waitFor(`${mode}: the CSV`, 5000, async () => JSON.parse(await evalJs("JSON.stringify(window.__q2csv)")));
  check(`${mode}: Export CSV = golden all-elements.csv, named "All elements.csv"`, csv.text === golden("schedule/frame/all-elements.csv") && csv.name === "All elements.csv" && csv.type === "text/csv",
    `${csv.name} ${csv.type} ${csv.text.slice(0, 80)}`);

  // A real click on the third row selects its element.
  const id = want.groups[0].ids[2];
  const xy = JSON.parse(await evalJs(`JSON.stringify((() => { const tr = document.querySelectorAll('[data-bb=quantities-table] tbody tr:not(.g)')[2];
    tr.scrollIntoView({ block: 'center' }); const r = tr.querySelector('td').getBoundingClientRect(); return [r.left + 4, r.top + r.height / 2]; })())`));
  await S("Input.dispatchMouseEvent", { type: "mousePressed", x: xy[0], y: xy[1], button: "left", buttons: 1, clickCount: 1 });
  await S("Input.dispatchMouseEvent", { type: "mouseReleased", x: xy[0], y: xy[1], button: "left", buttons: 0, clickCount: 1 });
  const sel = await waitFor(`${mode}: the selection`, 5000, async () => { const s = await B("return b.state.value.selection;"); return s.length ? s : null; }).catch(() => []);
  check(`${mode}: a click on a row selects its element`, JSON.stringify(sel) === JSON.stringify([id]), JSON.stringify(sel));
  const drawn = await waitFor(`${mode}: the row drawn selected`, 3000, () => evalJs("document.querySelectorAll('[data-bb=quantities-table] tbody tr:not(.g)')[2].classList.contains('sl')")).catch(() => false);
  check(`${mode}: the row is drawn selected`, drawn === true);

  // Group by: none (set_schedule), then undo.
  const undo0 = await B("return b.query('history').undo.length;");
  await evalJs("(() => { const s = document.querySelector('[data-bb=quantities-group]'); s.value = 'none'; s.dispatchEvent(new Event('change')); return true; })()");
  const flat = await waitFor(`${mode}: no group line`, 5000, () => evalJs("document.querySelectorAll('[data-bb=quantities-table] tbody tr.g').length === 0 && document.querySelectorAll('[data-bb=quantities-table] tbody tr').length > 0")).catch(() => false);
  check(`${mode}: grouping none: no group line, rows shown`, flat === true);
  check(`${mode}: one history step`, (await B("return b.query('history').undo.length;")) === undo0 + 1);
  const listed = JSON.parse(await B("return JSON.stringify(b.scheduleList().find((d) => d.name === 'All elements'));"));
  check(`${mode}: schedule_list has All elements not grouped`, listed?.group_by_level === false, JSON.stringify(listed));
  const eu = await B("return b.undo();");
  check(`${mode}: undo -> null`, eu === null, String(eu));
  await waitFor(`${mode}: undo`, 5000, async () => (await B("return b.query('history').undo.length;")) === undo0).catch(() => {});
  const back = await waitFor(`${mode}: the group lines back`, 5000, () => evalJs("document.querySelectorAll('[data-bb=quantities-table] tbody tr.g').length > 0")).catch(() => false);
  check(`${mode}: undo: the group lines are back`, back === true);
}

/** The command palette on one client: Ctrl+K, a query typed, the list filtered by the C core's
 *  palette_match, Enter runs the first action (Section box: pressed, recent_actions). */
async function palette(mode, S, evalJs) {
  const B = (body) => evalJs(`(async () => { const b = await import('/src/bimble.ts'); ${body} })()`);
  await S("Page.navigate", { url: PAGES[mode].replace("&page=quantities", "") });
  await waitFor(`${mode}: data-bb-ready`, 30000, () => evalJs("document.documentElement.dataset.bbReady === '1' && !!window.__bbGl"));
  const key = async (k, code, vk, modifiers = 0) => {
    await S("Input.dispatchKeyEvent", { type: "keyDown", key: k, code, windowsVirtualKeyCode: vk, modifiers });
    await S("Input.dispatchKeyEvent", { type: "keyUp", key: k, code, windowsVirtualKeyCode: vk, modifiers });
  };
  await key("k", "KeyK", 75, 2); // Ctrl+K (QuickBar.vue)
  const open = await waitFor(`${mode}: the palette`, 5000, () => evalJs("document.activeElement?.dataset.bb === 'palette-query'")).catch(() => false);
  check(`${mode}: Ctrl+K opens the palette, the query field focused`, open === true);
  const all = await evalJs("document.querySelectorAll('[data-bb=palette-item]').length");
  await S("Input.insertText", { text: "section box" });
  const shown = await waitFor(`${mode}: the filtered list`, 5000, async () => { const n = await evalJs("document.querySelectorAll('[data-bb=palette-item]').length"); return n > 0 && n < all ? n : null; }).catch(() => 0);
  check(`${mode}: typing "section box" lists the matching actions (${shown} of ${all})`, shown >= 1 && shown < all, `${shown} of ${all}`);
  const before = await B("return b.query('actions').find((a) => a.name === 'section_box').pressed;");
  await key("Enter", "Enter", 13);
  const closed = await waitFor(`${mode}: the palette closed`, 5000, () => evalJs("!document.querySelector('[data-bb=palette]')")).catch(() => false);
  check(`${mode}: Enter runs the first one and closes the palette`, closed === true);
  const after = await B("return b.query('actions').find((a) => a.name === 'section_box').pressed;");
  check(`${mode}: the action run is Section box: its pressed state turned (${before} -> ${after})`, before === false && after === true);
  const follows = await waitFor(`${mode}: the page's actions`, 3000, () => B("return b.actions.value.find((a) => a.name === 'section_box').pressed;")).catch(() => false);
  check(`${mode}: the page's actions follow (the ribbon draws it)`, follows === true);
  const rec = await B("return b.recentActions();");
  check(`${mode}: recent_actions starts with section_box`, rec[0] === "section_box", JSON.stringify(rec));
  await key("k", "KeyK", 75, 2);
  await waitFor(`${mode}: the palette again`, 5000, () => evalJs("document.activeElement?.dataset.bb === 'palette-query'")).catch(() => false);
  const first = await evalJs("document.querySelector('[data-bb=palette-item] .lab')?.textContent ?? ''");
  const boxLabel = await evalJs("[...document.querySelectorAll('[data-bb=palette-item]')].find((a) => a.querySelector('.kbd')?.textContent === 'B')?.querySelector('.lab')?.textContent ?? null");
  check(`${mode}: an empty query lists the recent action first`, first !== "" && first === boxLabel, `${first} vs ${boxLabel}`);
  await key("Escape", "Escape", 27);
  await B("return b.runAction('section_box');"); // off again (the native host keeps it across pages)
}

/** The view switches on one client: Edges off and on (no edge drawn, the dark pixels go), Fit all
 *  after a wheel zoom, Orthographic (parallel projection: two points on a line of sight meet on
 *  screen), Top view (pitch 1.5, yaw -π/2 + north), and a click from the top picks the top slab. */
async function switches(mode, S, evalJs) {
  const B = (body) => evalJs(`(async () => { const b = await import('/src/bimble.ts'); ${body} })()`);
  await S("Page.navigate", { url: PAGES[mode].replace("&page=quantities", "") });
  await waitFor(`${mode}: the first frame`, 30000, () => evalJs("window.__bbGl && window.__bbGl.firstFrameMs && window.__bbGl.q2 && window.__bbGl.meshRev !== null"));
  await waitFor(`${mode}: data-bb-ready`, 30000, () => evalJs("document.documentElement.dataset.bbReady === '1'"));
  const gl = async () => JSON.parse(await evalJs("JSON.stringify(window.__bbGl)"));
  const frames = () => evalJs("window.__bbGl.frames");
  const settle = async () => { const f0 = await frames(); await waitFor("a new frame", 5000, async () => (await frames()) > f0).catch(() => {}); await sleep(100); };
  const dark = () => evalJs(`(() => { const c = document.getElementById('bimble'); const o = new OffscreenCanvas(c.width, c.height);
    const x = o.getContext('2d'); x.drawImage(c, 0, 0); const d = x.getImageData(0, 0, c.width, c.height).data; let n = 0;
    for (let i = 0; i < d.length; i += 4) if (d[i] < 70 && d[i + 1] < 70 && d[i + 2] < 70) n++; return n; })()`);
  const run = (name) => B(`return b.runAction(${JSON.stringify(name)});`);
  await B("return b.select([]);"); // the native host keeps the Quantities page's selection
  await waitFor(`${mode}: drawn`, 10000, async () => (await gl()).drawnTris > 0);
  await settle();
  const g0 = await gl();
  const d0 = await dark();
  check(`${mode}: edges drawn by default (${g0.q2.edgeSegs} segments, ${d0} dark pixels)`, g0.q2.edges === true && g0.q2.edgeSegs > 0 && d0 > 500, JSON.stringify(g0.q2));
  check(`${mode}: action edges -> null`, (await run("edges")) === null);
  await waitFor(`${mode}: edges off`, 3000, async () => (await gl()).q2.edges === false).catch(() => {});
  await settle();
  const d1 = await dark(), g1 = await gl();
  check(`${mode}: edges off: none drawn, the dark pixels gone (${d0} -> ${d1})`, g1.q2.edges === false && g1.q2.edgeSegs === 0 && d1 < d0 / 10, `${JSON.stringify(g1.q2)} ${d1}`);
  check(`${mode}: the Edges button not pressed`, (await B("return b.actions.value.find((a) => a.name === 'edges').pressed;")) === false);
  await run("edges");
  await waitFor(`${mode}: edges on`, 3000, async () => (await gl()).q2.edges === true).catch(() => {});
  await settle();
  check(`${mode}: edges on again`, (await dark()) > d0 / 2);

  // Fit all after a wheel zoom.
  const c0 = g0.camera;
  const r = JSON.parse(await evalJs("JSON.stringify((() => { const r = document.getElementById('bimble').getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; })())"));
  await S("Input.dispatchMouseEvent", { type: "mouseWheel", x: r[0], y: r[1], deltaX: 0, deltaY: -600 });
  await settle();
  const c1 = (await gl()).camera;
  check(`${mode}: the wheel zoomed in`, c1.distance < c0.distance * 0.95, `${c0.distance} -> ${c1.distance}`);
  check(`${mode}: action fit -> null`, (await run("fit")) === null);
  await settle();
  const c2 = (await gl()).camera;
  const near = (a, b) => Math.abs(a - b) <= 1e-6 * Math.max(1, Math.abs(b));
  check(`${mode}: Fit all: the fitted distance and target again`, near(c2.distance, c0.distance) && c2.target.every((v, k) => near(v, c0.target[k])),
    `${c2.distance} vs ${c0.distance}; ${c2.target} vs ${c0.target}`);

  // Orthographic: two points on a line of sight project to one point.
  const apart = () => evalJs(`(() => { const g = window.__bbGl, e = g.navEye(), c = g.camera, n = c.distance;
    const d = [-Math.cos(c.pitch) * Math.cos(c.yaw), -Math.cos(c.pitch) * Math.sin(c.yaw), -Math.sin(c.pitch)];
    const t = [0, 1, 2].map((k) => e[k] + n * d[k]); // the target, world mm
    const side = [-d[1], d[0], 0], A = [0, 1, 2].map((k) => t[k] + 3000 * side[k]), B = [0, 1, 2].map((k) => A[k] + 0.3 * n * d[k]);
    const a = g.toScreen(...A), b = g.toScreen(...B); return Math.hypot(a[0] - b[0], a[1] - b[1]); })()`);
  const p0 = await apart();
  check(`${mode}: action orthographic -> null`, (await run("orthographic")) === null);
  await waitFor(`${mode}: orthographic`, 3000, async () => (await gl()).q2.ortho === true).catch(() => {});
  await settle();
  const p1 = await apart();
  check(`${mode}: orthographic: a line of sight is one point on screen (${p0.toFixed(1)} px -> ${p1.toFixed(3)} px)`, p0 > 5 && p1 < 0.5);
  check(`${mode}: the Orthographic button pressed`, (await B("return b.actions.value.find((a) => a.name === 'orthographic').pressed;")) === true);

  // Top view, then a click picks the top slab (parallel rays straight down).
  check(`${mode}: action top -> null`, (await run("top")) === null);
  await settle();
  const c3 = (await gl()).camera;
  const twoPi = 2 * Math.PI, yawWant = -Math.PI / 2;
  check(`${mode}: Top view: pitch 1.5, yaw -π/2 (no project north)`, Math.abs(c3.pitch - 1.5) < 1e-9 && Math.abs((((c3.yaw - yawWant) % twoPi) + twoPi) % twoPi) < 1e-9, `${c3.pitch} ${c3.yaw}`);
  const D2 = "1khkLES9P8rPMAGC5wqOCk";
  const xy = JSON.parse(await evalJs(`JSON.stringify((() => { const b = window.__bbGl.elementBox('${D2}'); return window.__bbGl.toScreen((b.min[0] + b.max[0]) / 2 + 700, (b.min[1] + b.max[1]) / 2 + 700, b.max[2]); })())`));
  await S("Input.dispatchMouseEvent", { type: "mouseMoved", x: xy[0], y: xy[1], button: "none", buttons: 0 });
  await S("Input.dispatchMouseEvent", { type: "mousePressed", x: xy[0], y: xy[1], button: "left", buttons: 1, clickCount: 1 });
  await S("Input.dispatchMouseEvent", { type: "mouseReleased", x: xy[0], y: xy[1], button: "left", buttons: 0, clickCount: 1 });
  const sel = await waitFor(`${mode}: a selection`, 5000, async () => { const v = await B("return b.state.value.selection;"); return v.length ? v : null; }).catch(() => []);
  check(`${mode}: orthographic top view: a click picks the top slab D2`, JSON.stringify(sel) === JSON.stringify([D2]), JSON.stringify(sel));
  await run("orthographic");
}

async function main() {
  if (!existsSync(join(UI, "node_modules"))) throw new Error(`no ${UI}/node_modules: run npm ci in web-ui/`);
  execFileSync("node", [join(UI, "scripts/copy-ccore.mjs")], { stdio: "ignore" });
  const model = join(tmp, "frame.bimble");
  copyFileSync(join(CCORE, "golden/frame.json"), model);
  const host = start("host", BIN, [String(PORT), model], {
    env: { ...process.env, BIMBLE_TOKEN: TOKEN, BIMBLE_ORIGINS: `http://localhost:${VITE},http://127.0.0.1:${VITE}` },
  });
  await waitFor("host 'listening'", 5000, () => host.out.includes("listening") || (host.exitCode !== null && Promise.reject(new Error(host.out))));
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
  for (const mode of ["native", "wasm"]) {
    await quantities(mode, S, evalJs);
    await palette(mode, S, evalJs);
    await switches(mode, S, evalJs);
  }

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
}
console.log(`lab_quantities: ${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
