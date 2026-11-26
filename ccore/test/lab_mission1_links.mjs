// Mission 1 across links (ticket M1LINKS, ADR 0081; ccore/session/m1links.h, web-ui/src/shell/LinksTree.vue):
//   CCORE_PORT_BASE=auto node ccore/test/lab_mission1_links.mjs
// The user's path on a project made of links alone: a native host (HOME, BIMBLE_CONFIG_DIR in a temp folder,
// BIMBLE_LINK_CACHE_MIN=0), Vite and headless Chromium at 1280x800 over raw CDP. Four synthetic IFC in a folder:
// a_frame.ifc (IFC4, storeys -800, 0, 3000), b_frame2.ifc (IFC2X3, 0 and 3150.5), c_mapped.ifc and d_column.ifc
// (IFC4, one shared coordinate system, one storey at 0; d_column gets a Pset_ColumnCommon and a Qto here).
// New project -> link the folder -> the levels from links (Fondations -800, Niveau 2 3000: 3150.5 is within
// 200 mm of 3000) each have a floor plan, drawn on paper with the links -> the section box encloses every link ->
// a linked element hidden, a class isolated, show all (the mesh trailer shrinks and comes back) -> a measure from
// a_frame's column P1.00 (300 x 300 at (0, 0)) to d_column's C-r250 (r 250 at (4000, 2000), moved by 40 m):
// hand-computed (-150, 0, 500) to (44250, 2000, 500): 44 445.0 mm -> the property set of a linked element ->
// a note on it survives Reload -> the plan exported to PDF shows it (pdftotext), 1 page -> saved, reopened from
// the caches: plans, note, sets still there. In the page: the eye on a class row, a click on a linked element's
// row, a click on a plan from links, the note's Add: each one's own feedback within 200 ms (ADR 0074; the load
// average printed; loaded: one rerun under load 4 before concluding). Screenshots in M1_SHOTS when set.
// Fails on the code before M1LINKS: no query link_levels (no plans from links), no view_rule, no PROP.
import { spawn, spawnSync } from "node:child_process";
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { CHROMIUM, port, vitePort } from "./ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const G = join(CCORE, "golden");
const UI = resolve(CCORE, "../web-ui");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("lab_mission1_links");
const VITE = vitePort("lab_mission1_links");
const TOKEN = "labm1links-token";
const PAGE = `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}`;
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-m1links-"));
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


const shots = process.env.M1_SHOTS ?? null;
async function main() {
  if (!existsSync(join(UI, "node_modules"))) throw new Error(`no ${UI}/node_modules: run npm ci in web-ui/`);
  console.log(`loadavg ${readFileSync("/proc/loadavg", "utf8").trim()}`);
  mkdirSync(dir, { recursive: true });
  const FILES = { "a_frame.ifc": "ifc/frame.ifc", "b_frame2.ifc": "ifc/frame2.ifc2x3.ifc", "c_mapped.ifc": "ifc/fixtures/mapped_item.ifc", "d_column.ifc": "ifc/fixtures/circle_column.ifc" };
  for (const [n, f] of Object.entries(FILES)) copyFileSync(join(G, f), join(dir, n));
  // d_column: a property set and a quantity set on its column #37 (synthetic)
  const dc = readFileSync(join(dir, "d_column.ifc"), "utf8");
  const extra = "#900=IFCPROPERTYSINGLEVALUE('IsExternal',$,IFCBOOLEAN(.F.),$);\n#901=IFCPROPERTYSINGLEVALUE('FireRating',$,IFCLABEL('R60'),$);\n" +
    "#902=IFCPROPERTYSET('2T2cCircle0000000000P1',$,'Pset_ColumnCommon',$,(#900,#901));\n#903=IFCRELDEFINESBYPROPERTIES('2T2cCircle0000000000R1',$,$,$,(#37),#902);\n" +
    "#904=IFCQUANTITYLENGTH('Length',$,$,3200.,$);\n#905=IFCELEMENTQUANTITY('2T2cCircle0000000000Q1',$,'Qto_ColumnBaseQuantities',$,$,(#904));\n#906=IFCRELDEFINESBYPROPERTIES('2T2cCircle0000000000R2',$,$,$,(#37),#905);\n";
  const at = dc.lastIndexOf("ENDSEC;");
  writeFileSync(join(dir, "d_column.ifc"), dc.slice(0, at) + extra + dc.slice(at));
  check("fixtures: one IFC2X3, three IFC4", /IFC2X3/.test(readFileSync(join(dir, "b_frame2.ifc"), "utf8")) && /'IFC4'/.test(readFileSync(join(dir, "a_frame.ifc"), "utf8")));
  const host = start("host", BIN, [String(PORT)], {
    env: { ...process.env, BIMBLE_TOKEN: TOKEN, HOME: home, BIMBLE_CONFIG_DIR: join(tmp, "config"), BIMBLE_LINK_CACHE_MIN: "0",
      BIMBLE_CACHE_DIR: join(tmp, "cache"), BIMBLE_ORIGINS: `http://localhost:${VITE},http://127.0.0.1:${VITE}` },
  });
  await waitFor("host 'listening'", 5000, () => host.out.includes("listening") || (host.exitCode !== null && Promise.reject(new Error(host.out))));
  const hcall = async (op, name, body = {}) => {
    const r = await fetch(`http://127.0.0.1:${PORT}/${op}/${name}?t=${TOKEN}`, { method: "POST", body: JSON.stringify(body), headers: { "Content-Type": "text/plain" } });
    return { status: r.status, text: await r.text() };
  };
  const q = async (name, body = {}) => { const r = await hcall("query", name, body); if (r.status !== 200) throw new Error(`${name}: ${r.text}`); return JSON.parse(r.text); };
  const meshBytes = async () => (await (await fetch(`http://127.0.0.1:${PORT}/mesh?t=${TOKEN}`)).arrayBuffer()).byteLength;
  const rows = async () => (await q("links_panel")).models;

  // 1. a new project, the folder linked: nothing placed by hand
  check("a new project", (await hcall("action", "new")).status === 200);
  const lf = await hcall("action", "link_folder", { path: dir });
  check("link the folder", lf.status === 200, lf.text);
  const ready = await waitFor("four links ready", 60000, async () => { const r = await rows(); return r.length === 4 && r.every((m) => m.state === "ready") ? r : null; });
  check("four links ready, each from a cache written beside it", ready.length === 4 && Object.keys(FILES).every((n) => existsSync(join(dir, `${n}.bimbleview`))));

  // 2. levels and floor plans from links
  const lv = await q("link_levels");
  check("levels from links: Fondations -800, Niveau 2 3000 (3150.5 merged, 0 is the project's Level 0)",
    lv.length === 2 && lv[0].name === "Fondations" && lv[0].elevation_mm === -800 && lv[1].name === "Niveau 2" && lv[1].elevation_mm === 3000, JSON.stringify(lv));
  const views = (await q("open_views")).views;
  check("a floor plan per level from links among the views", lv.every((l) => views.some((v) => v.id === l.view && v.type === "plan" && v.look === "down")), views.map((v) => v.name).join(", "));
  const svg = await q("view_svg", { view: lv[1].view });
  check("the plan from links drawn on paper with the links (cut fills)", /Plan Niveau 2/.test(svg.svg) && (svg.svg.match(/<path|<polygon/g) ?? []).length > 20, `${svg.svg.length} bytes`);
  const ax = await q("grid_axes");
  check("the links' levels in grid_axes", ax.links.length === 4 && ax.links[0].levels.some((l) => l.elevation_mm === -800));

  // 3. the section box over every link
  await hcall("action", "section_box");
  const box = (await q("visibility")).section_box;
  check("the section box encloses every link (z -800 .. 6000, x to 18 600)", box.on && box.box && box.box.min[2] <= -800 && box.box.max[2] >= 6000 && box.box.max[0] >= 18600, JSON.stringify(box.box));
  await hcall("action", "section_box");

  // 4. hide one linked element, isolate a class, show all
  const m0 = await meshBytes();
  const cols = await q("scene_elements", { link: "a_frame.ifc", storey: "Niveau 1", class: "IfcColumn", limit: 1 });
  let r = await hcall("action", "view_rule", { op: "hide", element: cols[0].id });
  const m1 = await meshBytes();
  check("hide one element of a link: the view keeps its id, the mesh trailer leaves it out", r.status === 200 && (await q("view_visibility")).hidden_elements.includes(cols[0].id) && m1 < m0, `${m0} -> ${m1}`);
  r = await hcall("action", "view_rule", { op: "isolate", class: "IfcColumn" });
  const vv = await q("view_visibility"), m2 = await meshBytes();
  check("isolate IfcColumn: a rule per other class, not ids", r.status === 200 && vv.hidden_classes.includes("IfcBeam") && !vv.hidden_classes.includes("IfcColumn") && m2 < m1, JSON.stringify(vv.hidden_classes));
  await hcall("action", "undo");
  check("undo: the class rule goes (one undo step)", !(await q("view_visibility")).hidden_classes.length);
  await hcall("action", "show_all");
  check("show all: the mesh as before", (await meshBytes()) === m0);

  // 5. measure between two links (hand-computed)
  for (const op of [{ op: "unpin", name: "d_column.ifc" }, { op: "place", name: "d_column.ifc", move_mm: [40000, 0, 0] }, { op: "hide", name: "b_frame2.ifc" }, { op: "hide", name: "c_mapped.ifc" }])
    await hcall("action", "manage_links", op);
  await hcall("action", "measure");
  const ray = (x, dx) => ({ ray: [x, 0, 500, dx, 0, 0], ray_tol: [x, 24, 500, dx, 0, 0], free: true });
  await hcall("tool", "measure_click", ray(-90000, 1));
  const mr = JSON.parse((await hcall("tool", "measure_click", { ...ray(90000, -1), ray: [90000, 2000, 500, -1, 0, 0], ray_tol: [90000, 2024, 500, -1, 0, 0] })).text);
  const want = Math.hypot(44250 - -150, 2000 - 0, 0); // P1.00's west face x -150 (y 0) to C-r250's east point x 44 250 (y 2000), both at z 500
  const got = JSON.stringify(mr).match(/"(?:length_mm|distance_mm|value_mm|length)":\s*([0-9.]+)/g)?.map((x) => Number(x.split(":")[1])) ?? [];
  check(`measure between two links: ${want.toFixed(1)} mm (hand-computed)`, got.some((x) => Math.abs(x - want) < 0.6), JSON.stringify(mr).slice(0, 400));
  await hcall("action", "measure");
  for (const n of ["b_frame2.ifc", "c_mapped.ifc"]) await hcall("action", "manage_links", { op: "show", name: n });

  // 6. property sets of a linked element, a note on it, Reload
  const el = await q("element", { link: "d_column.ifc", id: "2T2cCircle000000000010" });
  const pset = el.property_sets?.find((s) => s.name === "Pset_ColumnCommon"), qto = el.property_sets?.find((s) => s.type === "IfcElementQuantity");
  check("property sets of a linked element: Pset_ColumnCommon FireRating R60, IsExternal false, Qto Length 3200",
    pset?.properties.some((p) => p.name === "FireRating" && p.value === "R60") && pset?.properties.some((p) => p.name === "IsExternal" && p.value === "false") &&
    qto?.properties.some((p) => p.name === "Length" && p.value === "3200.0"), JSON.stringify(el.property_sets));
  r = await hcall("action", "manage_links", { op: "note_add", name: "a_frame.ifc", element: cols[0].id, text: "M1 note: check this column" });
  check("a note on a linked element", r.status === 200 && (await q("link_notes")).length === 1, r.text);
  await hcall("action", "manage_links", { op: "reload", name: "a_frame.ifc" });
  await waitFor("a_frame ready again", 30000, async () => (await rows()).find((m) => m.name === "a_frame.ifc")?.state === "ready");
  const nt = await q("link_notes");
  check("the note survives Reload (not an orphan)", nt.length === 1 && !nt[0].orphan, JSON.stringify(nt));

  // 7. the plan with the note exported to PDF
  const plan0 = views.find((v) => v.type === "plan" && v.look === "down" && !lv.some((l) => l.view === v.id));
  const pdf = join(tmp, "plan.pdf");
  r = await hcall("query", "view_pdf", { view: plan0.id, path: pdf });
  const txt = r.status === 200 ? spawnSync("pdftotext", [pdf, "-"], { encoding: "utf8" }).stdout : "";
  const pages = r.status === 200 ? Number(spawnSync("pdfinfo", [pdf], { encoding: "utf8" }).stdout.match(/Pages:\s+(\d+)/)?.[1]) : 0;
  check("the plan exported to PDF shows the note, 1 page", /M1 note: check this column/.test(txt) && pages === 1, `${r.status} ${pages} pages`);

  // 8. saved, reopened from the caches
  const saved = join(home, "links-only.bimble");
  check("save a project of links alone", (await hcall("action", "save_as", { path: saved })).status === 200);
  r = await hcall("action", "open", { path: saved, discard: true });
  await waitFor("reopened, every link ready", 30000, async () => { const x = await rows(); return x.length === 4 && x.every((m) => m.state === "ready") ? x : null; });
  const re = await rows(), lv2 = await q("link_levels"), nt2 = await q("link_notes");
  const el2 = await q("element", { link: "d_column.ifc", id: "2T2cCircle000000000010" });
  check("reopened from the caches: plans from links, the note, the property sets, the placement", re.every((m) => m.from_cache) && lv2.length === 2 &&
    nt2.length === 1 && el2.property_sets?.length === 2 && re.find((m) => m.name === "d_column.ifc")?.move_mm?.[0] === 40000, JSON.stringify(re.map((m) => [m.name, m.from_cache, m.move_mm])));

  // 9. the page
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
  const shot = async (name) => {
    if (!shots) return;
    mkdirSync(shots, { recursive: true });
    await evalJs("new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)))");
    const { data } = await S("Page.captureScreenshot", { format: "png" });
    writeFileSync(join(shots, `${name}.png`), Buffer.from(data, "base64"));
  };
  const calm = () => evalJs(`new Promise((res) => { let last = performance.now(), calm = 0; const f = (t) => { calm = t - last < 25 ? calm + 1 : 0; last = t; if (calm >= 3) res(); else requestAnimationFrame(f); }; requestAnimationFrame(f); })`);
  await waitFor("the first drawn frame", 30000, () => evalJs("window.__bbGl && window.__bbGl.firstFrameMs"));
  const st = await waitFor("four links drawn", 20000, async () => { const s = JSON.parse(await evalJs("JSON.stringify(window.__bbGl)")); return s.links.length === 4 && s.linkTris > 0 ? s : null; });
  check("the page draws four links", st.links.length === 4, `${st.linkTris} triangles`);
  await evalJs("(async () => (await import('/src/bimble.ts')).runAction('fit'))()");
  await sleep(300);
  await shot("3d-four-links");
  check("the plans from links marked in the Navigator", await evalJs(`document.querySelectorAll('[data-bb=from-links]').length === 2`));

  // the eye on a class row: its own feedback (aria-pressed) within 200 ms
  await evalJs(`document.querySelector('[data-bb=nav-link][data-link="a_frame.ifc"] .lbl').click()`);
  await waitFor("a_frame's storeys", 3000, () => evalJs(`!!document.querySelector('[data-bb=nav-link-storey][data-storey="Niveau 1"] .lbl')`));
  await evalJs(`document.querySelector('[data-bb=nav-link-storey][data-storey="Niveau 1"] .lbl').click()`);
  await waitFor("its classes", 3000, () => evalJs(`!!document.querySelector('[data-bb=nav-link-class][data-class="IfcBeam"] [data-bb=eye-class]')`));
  await calm();
  const eyeMs = await evalJs(`new Promise((res) => { const t0 = performance.now(); document.querySelector('[data-bb=nav-link-class][data-class="IfcBeam"] [data-bb=eye-class]').click();
    const tick = () => document.querySelector('[data-bb=nav-link-class][data-class="IfcBeam"] [data-bb=eye-class]')?.getAttribute('aria-pressed') === 'false' ? res(performance.now() - t0) : requestAnimationFrame(tick); tick(); })`);
  check("the eye on a class of a link: its own feedback within 200 ms (ADR 0074)", eyeMs < 200, `${eyeMs.toFixed(1)} ms`);
  await hcall("action", "show_all");

  // a click on a linked element's row: its properties (with the property sets) within 200 ms
  await evalJs(`document.querySelector('[data-bb=nav-link][data-link="d_column.ifc"] .lbl').click()`);
  await waitFor("d_column's storey", 3000, () => evalJs(`!!document.querySelector('[data-bb=nav-link][data-link="d_column.ifc"] ~ [data-bb=nav-link-storey] .lbl')`));
  await evalJs(`[...document.querySelectorAll('[data-bb=nav-link-storey]')].find((r) => r.previousElementSibling?.dataset?.link === "d_column.ifc" || true) && null`);
  const openD = await evalJs(`(() => { const rows = [...document.querySelectorAll('[data-bb=nav-link], [data-bb=nav-link-storey]')]; const i = rows.findIndex((r) => r.dataset.link === 'd_column.ifc'); const s = rows[i + 1]; s?.querySelector('.lbl').click(); return !!s; })()`);
  await waitFor("d_column's classes", 3000, () => evalJs(`[...document.querySelectorAll('[data-bb=nav-link-class][data-class="IfcColumn"] .lbl')].length > 0`));
  await evalJs(`[...document.querySelectorAll('[data-bb=nav-link-class][data-class="IfcColumn"] .lbl')].pop().click()`);
  await waitFor("the column's row", 3000, () => evalJs(`!!document.querySelector('[data-bb=nav-link-element][data-id="2T2cCircle000000000010"] .lbl')`));
  await calm();
  const selMs = await evalJs(`new Promise((res) => { const t0 = performance.now(); document.querySelector('[data-bb=nav-link-element][data-id="2T2cCircle000000000010"] .lbl').click();
    const tick = () => document.querySelector('[data-bb=props-pset][data-set="Pset_ColumnCommon"]') ? res(performance.now() - t0) : requestAnimationFrame(tick); tick(); })`);
  check("a click on a linked element's row: its properties with Pset_ColumnCommon within 200 ms", openD && selMs < 200, `${selMs.toFixed(1)} ms`);
  check("the panel shows FireRating R60", await evalJs(`[...document.querySelectorAll('[data-bb=props-prop][data-prop="FireRating"]')].some((r) => r.textContent.includes('R60'))`));
  await shot("properties-pset");

  // a note through the panel: the note's row within 200 ms
  await evalJs(`(() => { const i = document.querySelector('[data-bb=note-text]'); i.value = 'Column C-r250: see the detail'; i.dispatchEvent(new Event('input')); })()`);
  await calm();
  const noteMs = await evalJs(`new Promise((res) => { const t0 = performance.now(); document.querySelector('[data-bb=note-add]').click();
    const tick = () => [...document.querySelectorAll('[data-bb=nav-note]')].some((r) => r.textContent.includes('C-r250')) ? res(performance.now() - t0) : requestAnimationFrame(tick); tick(); })`);
  check("Add a note: its row in the Navigator within 200 ms", noteMs < 200, `${noteMs.toFixed(1)} ms`);
  await shot("note");

  // a click on a plan from links: the plan's clip in the viewport within 200 ms
  await calm();
  const planMs = await evalJs(`new Promise((res) => { const t0 = performance.now(); document.querySelector('[data-view="${lv[0].view}"]').click();
    const tick = () => (window.__bbGl.clip && window.__bbGl.clip.max[2] <= -399) ? res(performance.now() - t0) : requestAnimationFrame(tick); tick(); })`);
  check("a click on a plan from links: the plan cut at -400 shown within 200 ms", planMs < 200, `${planMs.toFixed(1)} ms`);
  await evalJs("(async () => (await import('/src/bimble.ts')).runAction('fit'))()");
  await sleep(300);
  await shot("plan-from-links");
  // a section through the links (a section view, VIEWSHEET's): its clip
  const sec = await hcall("verb", "view_add", { kind: "section", facing: "north", origin: [5000, 2500] });
  const secId = JSON.parse(sec.text || "{}").id ?? (await q("open_views")).views.find((v) => v.type === "section")?.id;
  if (secId) await evalJs(`(async () => (await import('/src/bimble.ts')).openView(${JSON.stringify(secId)}))()`);
  const clip = await waitFor("the section's clip", 5000, async () => { const c = await q("view_clip"); return c.type === "section" && c.clip ? c : null; }).catch(() => null);
  check("a section through the links has its box (the links' bounds cut at its line)", clip !== null && clip.clip.max[2] >= 6000, JSON.stringify(clip?.clip));
  await evalJs("(async () => (await import('/src/bimble.ts')).runAction('fit'))()");
  await sleep(400);
  await shot("section");

  const errors = consoleLog.filter((m) => /error/.test(m.level) && !/favicon|Synchronous XMLHttpRequest/.test(m.text));
  check("no uncaught page error", exceptions.length === 0, exceptions.join(" | "));
  check("no console error", errors.length === 0, errors.map((m) => m.text).join(" | "));
  console.log(`loadavg ${readFileSync("/proc/loadavg", "utf8").trim()}`);
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
