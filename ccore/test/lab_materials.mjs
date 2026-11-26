// The Materials page in the lab (ticket MAT1): CCORE_PORT_BASE=auto node ccore/test/lab_materials.mjs
// Starts out/tcc/bimble-ccore (or $HOST_BIN) on a copy of golden/frame.json, copies the wasm build
// (scripts/copy-ccore.mjs), Vite from web-ui/ and headless Chromium over raw CDP. Twice, on the
// native host (/?native=1&gl=1) and on ccore.wasm (/?ccore=1&gl=1):
//   - the ribbon's Materials tool is not drawn "not yet" (FAKE1) and its action is enabled;
//   - the page (pages/Materials.vue, query materials_page) lists C30/37 with its 72 elements;
//   - S235 added from the library is a row with 0 elements, one undo step;
//   - a column, selected, gets S235 by the page's assign button: query element says S235, and
//     the column's pixel in the 3D view turns from concrete grey to the steel family's blue;
//   - renamed in the page ("Steel S235 JR"); its remove is refused while the column is made of it
//     (the refusal shown, the row kept); the column given C30/37 again, the remove goes through;
//   - the undo chain (5 steps) gives the materials and the column's material of the start, the
//     redo chain the end; a GL24h added and its density typed (480) and family changed in the page;
//   - saved and opened again: query materials_full the same (native: save_as and open; wasm:
//     ccoreSaveBytes and ccoreOpenFile);
//   - (native) golden/ifc/frame.ifc opened read only: the page lists C30/37 (72), no input.
// Before MAT1 the tool was drawn "not yet", the action never enabled and query materials_page
// unknown: the first checks fail. No uncaught page error; no console error or warning but the
// sync-XHR deprecation, the favicon 404 and the 400s this test asks for. Prints "N passed, M
// failed"; exits non-zero on a failure. Kills all it starts.
import { spawn, execFileSync } from "node:child_process";
import { copyFileSync, existsSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { CHROMIUM, port, vitePort } from "./ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = resolve(CCORE, "../web-ui");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("lab_materials", 47387);
const VITE = vitePort("lab_materials", 5150);
const TOKEN = "labmaterials-token";
const PAGES = {
  native: `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}`,
  wasm: `http://localhost:${VITE}/?ccore=1&gl=1`,
};
const RO_IFC = join(CCORE, "golden/ifc/frame.ifc");
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-materials-"));
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

async function host(op, name, body = "") {
  const r = await fetch(`http://127.0.0.1:${PORT}/${op}/${encodeURIComponent(name)}?t=${TOKEN}`, { method: "POST", headers: { "Content-Type": "text/plain" }, body });
  return { status: r.status, text: await r.text() };
}

async function materials(mode, evalJs) {
  const B = (body) => evalJs(`(async () => { const b = await import('/src/bimble.ts'); ${body} })()`);
  const P = (body) => evalJs(`(async () => { const p = await import('/src/pages/index.ts'); const m = await import('/src/shell/send.ts'); ${body} })()`);
  const raf2 = () => evalJs("new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)))");
  const frames = () => evalJs("window.__bbGl ? window.__bbGl.frames : 0");
  const settle = async () => { const f0 = await frames(); await waitFor("a new frame", 3000, async () => (await frames()) > f0).catch(() => {}); for (let i = 0; i < 3; i++) await raf2(); };
  const type = (sel, value) => evalJs(`(() => { const el = document.querySelector(${JSON.stringify(sel)}); if (!el) return false;
    el.value = ${JSON.stringify(value)}; el.dispatchEvent(new Event('input', { bubbles: true })); el.dispatchEvent(new Event('change', { bubbles: true })); return true; })()`);
  const click = (sel) => evalJs(`(() => { const el = document.querySelector(${JSON.stringify(sel)}); if (!el || el.disabled) return false; el.click(); return true; })()`);
  const rows = () => evalJs("[...document.querySelectorAll('[data-bb=material-row]')].map((b) => [b.dataset.name, Number(b.dataset.count)])");
  const rowSel = (name) => `[data-bb=material-row][data-name=${JSON.stringify(name)}]`;
  const undoN = async () => (await B("return b.query('history');")).undo.length;
  const errText = () => evalJs("document.querySelector('[data-bb=material-error]')?.textContent.trim() ?? null");
  const page = async (id) => {
    const ok = await P(`return m.send(${JSON.stringify(id === null ? { do: "showView" } : { do: "showPage", page: id })}) && p.currentPage.value === ${JSON.stringify(id)};`);
    if (!ok) throw new Error(`${mode}: the shell did not show ${id ?? "the view"}`);
    await settle();
  };
  /** The colour of the canvas pixel at page CSS point (x, y) (WebGL2, preserveDrawingBuffer). */
  const pixelAt = (x, y) => evalJs(`(() => { const c = document.getElementById('bimble'); const g = c.getContext('webgl2'); const r = c.getBoundingClientRect();
    const sx = c.width / c.clientWidth, sy = c.height / c.clientHeight; const px = Math.round((${x} - r.left) * sx), py = c.height - 1 - Math.round((${y} - r.top) * sy);
    const p = new Uint8Array(4); g.readPixels(px, py, 1, 1, g.RGBA, g.UNSIGNED_BYTE, p); return [p[0], p[1], p[2]]; })()`);
  const elementMat = (id) => B(`return b.query('element', { id: ${JSON.stringify(id)} }).material_name;`);

  await S_navigate(PAGES[mode]);
  await waitFor(`${mode}: data-bb-ready`, 30000, () => evalJs("document.documentElement.dataset.bbReady === '1'"));
  await settle();

  // ---- the ribbon tool and the action ----
  const act = (await B("return b.actions.value;")).find((a) => a.name === "materials");
  check(`${mode}: action materials enabled (ribbon.rs:611 \`_ => loaded\`)`, act?.enabled === true, JSON.stringify(act));
  const ny = await evalJs(`(async () => { const c = await import('/src/shell/capability.ts'); const r = await import('/src/shell/ribbon-tabs.ts');
    const t = Object.values(r.RIBBON).flat().flatMap((g) => g.tools).find((x) => x.action === 'materials'); return t ? { page: t.page ?? null, ny: c.toolNotYet(t) } : null; })()`);
  check(`${mode}: the ribbon's Materials tool opens the page and is not "not yet"`, ny?.page === "materials" && ny?.ny === false, JSON.stringify(ny));

  // ---- a column seen in the 3D view: its pixel ----
  const cols = await B("return b.query('elements', { class: 'IfcColumn', limit: 500 }).map((e) => e.id);");
  let col = null, at = null;
  for (const id of cols) {
    const box = await evalJs(`JSON.stringify(window.__bbGl.elementBox(${JSON.stringify(id)}))`).then((t) => JSON.parse(t));
    if (!box) continue;
    const c = [0, 1, 2].map((k) => (box.min[k] + box.max[k]) / 2);
    const s = await evalJs(`JSON.stringify(window.__bbGl.toScreen(${c[0]}, ${c[1]}, ${c[2]}))`).then((t) => JSON.parse(t));
    if (!s) continue;
    if ((await evalJs(`window.__bbGl.pickAt(${s[0]}, ${s[1]})`)) === id) { col = id; at = s; break; }
  }
  check(`${mode}: a column seen at its centre`, col !== null, `${cols.length} columns`);
  const px0 = at ? await pixelAt(at[0], at[1]) : [0, 0, 0];
  check(`${mode}: the column is concrete grey on screen`, Math.abs(px0[2] - px0[0]) < 10 && px0[0] > 100, JSON.stringify(px0));

  // ---- the page ----
  await page("materials");
  await waitFor(`${mode}: the list`, 5000, async () => (await rows()).length > 0).catch(() => {});
  check(`${mode}: editable: the page says so`, (await evalJs("document.querySelector('[data-bb=materials]')?.dataset.bbEditable")) === "1");
  check(`${mode}: the list: C30/37 with its 72 elements`, JSON.stringify(await rows()) === JSON.stringify([["C30/37", 72]]), JSON.stringify(await rows()));
  const u0 = await undoN();
  check(`${mode}: S235's Add in the library`, await click("[data-bb=material-library-add][data-name=S235]"));
  await waitFor(`${mode}: S235 row`, 5000, async () => (await rows()).some((r) => r[0] === "S235")).catch(() => {});
  check(`${mode}: S235 added: a row with 0 elements, one undo step, selected`, JSON.stringify(await rows()) === JSON.stringify([["C30/37", 72], ["S235", 0]]) &&
    (await undoN()) === u0 + 1 && (await evalJs("document.querySelector('[data-bb=material-detail]')?.dataset.name")) === "S235", JSON.stringify(await rows()));
  check(`${mode}: S235's values from the library (library.rs:100)`, (await evalJs("document.querySelector('[data-bb=material-strength_mpa]')?.value")) === "235" &&
    (await evalJs("document.querySelector('[data-bb=material-source]')?.textContent.trim()")) === "EN 1993-1-1 Table 3.1 (t ≤ 40 mm)");
  check(`${mode}: S235's Add is off now`, (await evalJs("document.querySelector('[data-bb=material-library-add][data-name=S235]')?.disabled")) === true);

  // assign to the selected column
  if (col) await B(`return b.select([${JSON.stringify(col)}]);`);
  await waitFor(`${mode}: assign on`, 3000, () => evalJs("document.querySelector('[data-bb=material-assign]')?.disabled === false")).catch(() => {});
  check(`${mode}: assign the selection`, await click("[data-bb=material-assign]"));
  await waitFor(`${mode}: the column is S235`, 5000, async () => col && (await elementMat(col)) === "S235").catch(() => {});
  check(`${mode}: the column's material is S235, the row says 1`, col && (await elementMat(col)) === "S235" &&
    JSON.stringify(await rows()) === JSON.stringify([["C30/37", 71], ["S235", 1]]), JSON.stringify(await rows()));
  check(`${mode}: the swatch is the steel family's colour`, (await evalJs("document.querySelector('[data-bb=material-swatch]')?.dataset.colour")) === "#96aac4");
  await B("return b.select([]);");
  await page(null);
  await settle();
  const px1 = at ? await pixelAt(at[0], at[1]) : [0, 0, 0];
  console.log(`${mode}: the column's pixel at (${at?.map((v) => v.toFixed(0))}): ${JSON.stringify(px0)} (C30/37) -> ${JSON.stringify(px1)} (S235)`);
  check(`${mode}: the column turned steel blue on screen (the material's family colour)`, px1[2] - px1[0] > 15, `${JSON.stringify(px0)} -> ${JSON.stringify(px1)}`);

  // rename, refused remove, reassign, remove
  await page("materials");
  await click(rowSel("S235"));
  check(`${mode}: renamed in the page`, await type("[data-bb=material-name]", "Steel S235 JR"));
  await waitFor(`${mode}: renamed row`, 5000, async () => (await rows()).some((r) => r[0] === "Steel S235 JR")).catch(() => {});
  check(`${mode}: the row is "Steel S235 JR"`, JSON.stringify(await rows()) === JSON.stringify([["C30/37", 71], ["Steel S235 JR", 1]]), JSON.stringify(await rows()));
  const u3 = await undoN();
  await click(rowSel("Steel S235 JR"));
  await click("[data-bb=material-remove]");
  await waitFor(`${mode}: the refusal`, 3000, async () => !!(await errText())).catch(() => {});
  check(`${mode}: remove refused while in use: the refusal shown, the row kept, no step`, /is the material of 1 element/.test((await errText()) ?? "") &&
    (await rows()).some((r) => r[0] === "Steel S235 JR") && (await undoN()) === u3, await errText());
  if (col) await B(`return b.select([${JSON.stringify(col)}]);`);
  await click(rowSel("C30/37"));
  await waitFor(`${mode}: assign on`, 3000, () => evalJs("document.querySelector('[data-bb=material-assign]')?.disabled === false")).catch(() => {});
  await click("[data-bb=material-assign]");
  await waitFor(`${mode}: the column is C30/37 again`, 5000, async () => col && (await elementMat(col)) === "C30/37").catch(() => {});
  await B("return b.select([]);");
  await click(rowSel("Steel S235 JR"));
  await click("[data-bb=material-remove]");
  await waitFor(`${mode}: removed`, 5000, async () => !(await rows()).some((r) => r[0] === "Steel S235 JR")).catch(() => {});
  check(`${mode}: removed after the column got C30/37 back`, JSON.stringify(await rows()) === JSON.stringify([["C30/37", 72]]) && (await errText()) === null, JSON.stringify(await rows()));

  // undo chain: remove, reassign, rename, assign, add
  const steps = (await undoN()) - u0;
  check(`${mode}: five steps (add, assign, rename, reassign, remove)`, steps === 5, String(steps));
  const want = [
    [["C30/37", 72], ["Steel S235 JR", 0]], // undo remove
    [["C30/37", 71], ["Steel S235 JR", 1]], // undo reassign
    [["C30/37", 71], ["S235", 1]], // undo rename
    [["C30/37", 72], ["S235", 0]], // undo assign
    [["C30/37", 72]], // undo add
  ];
  let chain = true;
  for (const w of want) {
    await B("return b.undo();");
    await waitFor(`${mode}: undo`, 3000, async () => JSON.stringify(await rows()) === JSON.stringify(w)).catch(() => {});
    if (JSON.stringify(await rows()) !== JSON.stringify(w)) { chain = false; console.log(`  undo: ${JSON.stringify(await rows())}, want ${JSON.stringify(w)}`); }
  }
  check(`${mode}: the undo chain, row by row, back to the start`, chain && (await elementMat(col)) === "C30/37");
  for (let i = 0; i < 5; i++) await B("return b.redo();");
  await waitFor(`${mode}: redo`, 3000, async () => JSON.stringify(await rows()) === JSON.stringify([["C30/37", 72]])).catch(() => {});
  check(`${mode}: the redo chain: the end again`, JSON.stringify(await rows()) === JSON.stringify([["C30/37", 72]]) && (await undoN()) === u0 + 5);

  // values and family typed in the page
  await click("[data-bb=material-library-add][data-name=GL24h]");
  await waitFor(`${mode}: GL24h`, 5000, async () => (await rows()).some((r) => r[0] === "GL24h")).catch(() => {});
  await click(rowSel("GL24h"));
  await type("[data-bb=material-density_kg_m3]", "480");
  await waitFor(`${mode}: density`, 5000, async () => (await B("return b.query('materials_full');")).find((m) => m.name === "GL24h")?.props?.density_kg_m3 === 480).catch(() => {});
  const fam = await evalJs(`(() => { const s = document.querySelector('[data-bb=material-family]'); if (!s) return false; s.value = 'masonry'; s.dispatchEvent(new Event('change', { bubbles: true })); return true; })()`);
  await waitFor(`${mode}: family`, 5000, async () => (await B("return b.query('materials_full');")).find((m) => m.name === "GL24h")?.category === "masonry").catch(() => {});
  const gl = (await B("return b.query('materials_full');")).find((m) => m.name === "GL24h");
  check(`${mode}: GL24h: density 480 typed, family masonry chosen, the other values kept`, fam && gl?.props?.density_kg_m3 === 480 && gl?.category === "masonry" &&
    gl?.props?.e_mpa === 11500 && gl?.props?.strength_mpa === 24, JSON.stringify(gl));

  // save and open again
  const before = JSON.stringify(await B("return b.query('materials_full');"));
  if (mode === "native") {
    const path = join(tmp, "mat1.bimble");
    const sv = await host("action", "save_as", JSON.stringify({ path }));
    check(`${mode}: save_as mat1.bimble`, sv.status === 200 && existsSync(path), sv.text);
    const op = await host("action", "open", JSON.stringify({ path, discard: true }));
    check(`${mode}: mat1.bimble opens`, op.status === 200, op.text);
    await waitFor(`${mode}: the file again`, 8000, async () => (await B("return b.state.value.file;")) === "mat1.bimble").catch(() => {});
  } else {
    const r = await evalJs(`(async () => { const c = await import('/src/ccore.ts'); const s = c.ccoreSaveBytes();
      await c.ccoreOpenFile(new File([s.bytes], s.name), true); return { name: s.name, n: s.bytes.length }; })()`);
    check(`${mode}: saved (${r?.n} bytes) and opened again`, r?.n > 0, JSON.stringify(r));
  }
  await sleep(300);
  const after = JSON.stringify(await B("return b.query('materials_full');"));
  check(`${mode}: saved and opened again: the same materials, every field`, after === before, `\n  before ${before}\n  after  ${after}`);

  // a file read as it is: the list, no edit control
  if (mode === "native") {
    const op = await host("action", "open", JSON.stringify({ path: RO_IFC, discard: true }));
    check(`${mode}: frame.ifc opens read only`, op.status === 200, op.text);
    await waitFor(`${mode}: the IFC`, 10000, async () => (await B("return b.state.value.file;")) === "frame.ifc").catch(() => {});
    await page("materials");
    await waitFor(`${mode}: read-only rows`, 5000, async () => (await rows()).length > 0).catch(() => {});
    check(`${mode}: read only: C30/37 (72), no input, no add, no remove`, JSON.stringify(await rows()) === JSON.stringify([["C30/37", 72]]) &&
      (await evalJs("document.querySelector('[data-bb=materials]').dataset.bbEditable")) === "0" &&
      (await evalJs("document.querySelectorAll('[data-bb=materials] input:not([data-bb=material-find]), [data-bb=materials] select, [data-bb=material-remove], [data-bb=material-library-add], [data-bb=material-assign]').length")) === 0,
      JSON.stringify(await rows()));
    check(`${mode}: read only: values from the grade C30/37`, (await evalJs("document.querySelector('[data-bb=material-props]')?.textContent")).includes("33000 MPa"));
  }
}

let S_navigate;
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
  S_navigate = (url) => S("Page.navigate", { url });
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
  for (const mode of ["native", "wasm"]) await materials(mode, evalJs);

  const IGNORED = [
    /Synchronous XMLHttpRequest on the main thread is deprecated/,
    /404 \(Not Found\) http:\/\/localhost:\d+\/favicon\.ico$/,
    // Chrome's line for a call the native host answered 400 (bimble.ts's sync XHR): each is checked below.
    new RegExp(`400 \\(Bad Request\\) http://localhost:${VITE}/src/bimble\\.ts$`),
  ];
  const errors = consoleLog.filter((m) => /error|warn/.test(m.level) && !IGNORED.some((r) => r.test(m.text)));
  check("no uncaught page error", exceptions.length === 0, exceptions.join(" | "));
  check("no console error or warning", errors.length === 0, errors.map((m) => m.text).join(" | "));
  const badResp = failedResp.filter((r) => !/^404 http:\/\/localhost:\d+\/favicon\.ico$/.test(r) &&
    // FIXUI: only the refusal the test asks for; a read-only scene's model queries are no longer
    // asked (FAKE1) and its elements answer through scene_element, so a 400 there is a regression.
    !new RegExp(`^400 http://127\\.0\\.0\\.1:${PORT}/verb/material_remove\\?`).test(r));
  check("no failed response but the refusal asked for", badResp.length === 0, badResp.join(" | "));
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
