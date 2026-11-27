// The Links panel in the lab (ticket LNK1, GitHub issue #69; web-ui/src/shell/LinksPanel.vue, shell/links.ts;
// session/links_panel.h, links.h, session/paper_scene.h): node ccore/test/lab_links_manage.mjs.
// Starts out/tcc/bimble-ccore (or $HOST_BIN) on a copy of golden/frame.json with HOME in a temp folder, copies the
// wasm build into web-ui/public/ccore/ (scripts/copy-ccore.mjs; needs out/wasm/ccore.wasm) with the same model as
// public/ccore/lnk1-model.json, starts Vite and headless Chromium over raw CDP, and on two pages:
//   native  /?native=1&gl=1&port=…&token=…   (the page's file dialog: dialogs/FileBrowser.vue)
//   wasm    /?ccore=1&gl=1&model=ccore/lnk1-model.json   (the browser's picker, filled through CDP)
// both: Manage links… opens the Navigator's Links group; Link IFC… twice through the dialog / picker (m1.ifc =
// golden/ifc/fixtures/mapped_item.ifc, f2.ifc = golden/ifc/frame2.ifc): two rows, two links in the viewport; the eye
// hides f2 (its pixels in the link tint go), shows it again; Reload (native: read again; wasm: the picker asks
// again) keeps the row; remove f2 (its row and its link go). In the ground plan: Link DXF… (r12.dxf): a row with its
// view; its (0, 0) is drawn at its offset (window.__bbGl.toScreen, within 1 px); ΔX 1000 + Move: the offset moves by
// 1000 mm and the corner is drawn there, that many pixels further (toScreen); Angle 90 + Rotate: 90°; undo: 0° again.
// native only: Save as coord.bimble (through the core), open frame.json, open coord.bimble: the rows are back (m1.ifc,
// r12.dxf); golden/ifc/frame.ifc opened as it is: a plan On paper shows an SVG with the cut of its elements (paths);
// a links-only .bimble (a new model with frame.ifc linked, saved by the core): File → Export view → PDF: pdftotext
// finds the view's title and the PDF has drawing operators beyond the frame's.
// No uncaught page error; no console error but the listed ones. Prints "N passed, M failed"; exits non-zero on a
// failure. Fails before LNK1: the Links group had no buttons (no [data-bb=links-panel]), the read-only IFC had no
// On paper switch, the links-only .bimble had nothing to draw.
import { execFileSync, spawn } from "node:child_process";
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { CHROMIUM, port, vitePort } from "./ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const G = join(CCORE, "golden");
const UI = resolve(CCORE, "../web-ui");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("lab_links_manage");
const VITE = vitePort("lab_links_manage");
const TOKEN = "lablinksmanage-token";
const MODEL_PUBLIC = join(UI, "public/ccore/lnk1-model.json");
const PAGES = {
  native: `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}`,
  wasm: `http://localhost:${VITE}/?ccore=1&gl=1&model=ccore/lnk1-model.json`,
};
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-links-manage-"));
const home = join(tmp, "home"), work = join(home, "work");
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
  rmSync(MODEL_PUBLIC, { force: true });
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

/** The native host directly (the page sees its changes through the change feed). */
async function host(op, name, body = {}) {
  const r = await fetch(`http://127.0.0.1:${PORT}/${op}/${name}?t=${TOKEN}`, { method: "POST", body: JSON.stringify(body), headers: { "Content-Type": "text/plain" } });
  return { status: r.status, text: await r.text() };
}

function pdftotext(file) {
  try {
    return execFileSync("pdftotext", [file, "-"], { encoding: "utf8" });
  } catch (e) {
    return e.code === "ENOENT" ? null : `pdftotext failed: ${e.message}`;
  }
}

async function onPage(cdp, mode) {
  const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank" });
  const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
  const S = (m, p) => cdp.send(m, p, sessionId);
  const consoleLog = [], exceptions = [], meshResponses = []; // SIZE2FIX: every /mesh answer (url, status)
  const downloads = new Map();
  let chooser = null;
  cdp.on((msg) => {
    if (msg.method === "Browser.downloadWillBegin") downloads.set(msg.params.guid, { name: msg.params.suggestedFilename, state: "started" });
    else if (msg.method === "Browser.downloadProgress" && downloads.has(msg.params.guid)) downloads.get(msg.params.guid).state = msg.params.state;
    if (msg.sessionId !== sessionId) return;
    if (msg.method === "Page.fileChooserOpened") chooser = msg.params;
    else if (msg.method === "Runtime.consoleAPICalled") {
      consoleLog.push({ level: msg.params.type, text: msg.params.args.map((a) => a.value ?? a.description ?? a.type).join(" ") });
    } else if (msg.method === "Runtime.exceptionThrown") {
      const d = msg.params.exceptionDetails;
      exceptions.push(d.exception?.description ?? d.text);
    } else if (msg.method === "Network.responseReceived" && /\/mesh\?/.test(msg.params.response.url)) {
      meshResponses.push({ url: msg.params.response.url.replace(/^.*\/mesh\?/, "").replace(/t=[^&]*&?/, ""), status: msg.params.response.status });
    } else if (msg.method === "Log.entryAdded") {
      consoleLog.push({ level: `log.${msg.params.entry.level}`, text: `${msg.params.entry.text} ${msg.params.entry.url ?? ""}`.trim() });
    }
  });
  for (const d of ["Runtime", "Log", "Page", "Network"]) await S(`${d}.enable`);
  await S("Emulation.setDeviceMetricsOverride", { width: 1400, height: 1000, deviceScaleFactor: 1, mobile: false });
  if (mode === "wasm") await S("Page.setInterceptFileChooserDialog", { enabled: true });
  await S("Page.navigate", { url: PAGES[mode] });
  const evalJs = async (expr) => {
    const r = await S("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
    return r.result.value;
  };
  const B = (body) => evalJs(`(async () => { const b = await import('/src/bimble.ts'); ${body} })()`);
  const has = (sel) => evalJs(`!!document.querySelector(${JSON.stringify(sel)})`);
  const raf2 = () => evalJs("new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)))");
  const settle = async () => { for (let i = 0; i < 4; i++) await raf2(); };
  /** A real mouse click on the element (a user gesture: the browser's picker opens only so). */
  const click = async (sel) => {
    const at = JSON.parse(await evalJs(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) return 'null';
      e.scrollIntoView({ block: 'center' }); const r = e.getBoundingClientRect(); return JSON.stringify({ x: r.x + r.width / 2, y: r.y + r.height / 2 }); })()`));
    if (!at) throw new Error(`no ${sel}`);
    for (const type of ["mousePressed", "mouseReleased"]) await S("Input.dispatchMouseEvent", { type, x: at.x, y: at.y, button: "left", buttons: type === "mousePressed" ? 1 : 0, clickCount: 1 });
  };
  const setInput = (sel, v) => evalJs(`(() => { const i = document.querySelector(${JSON.stringify(sel)}); i.value = ${JSON.stringify(String(v))}; i.dispatchEvent(new Event('input')); return true; })()`);
  /** Answers the picker the last click opened: the page's dialog (native) or the browser's (wasm). */
  const pick = async (file) => {
    if (mode === "wasm") {
      const c = await waitFor("Page.fileChooserOpened", 5000, () => chooser);
      chooser = null;
      await S("DOM.setFileInputFiles", { files: [file], backendNodeId: c.backendNodeId });
      return;
    }
    await waitFor("the file dialog", 5000, () => has("[data-bb=file-browser]"));
    const dir = dirname(file), name = file.slice(dir.length + 1);
    await evalJs(`(() => { const i = document.querySelector('[data-bb=file-browser-path]'); i.value = ${JSON.stringify(dir)}; i.dispatchEvent(new Event('input'));
      i.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })); return true; })()`);
    await waitFor(`${name} listed`, 5000, () => has(`[data-bb=file-browser-entry][data-name=${JSON.stringify(name)}]`));
    await click(`[data-bb=file-browser-entry][data-name=${JSON.stringify(name)}]`);
    await click("[data-bb=file-browser-go]");
    await waitFor("the dialog closed", 5000, async () => !(await has("[data-bb=file-browser]")));
  };
  const rows = () => evalJs("[...document.querySelectorAll('[data-bb=link-row]')].map((e) => ({ name: e.dataset.name, visible: e.dataset.visible === '1', missing: e.dataset.missing === '1' }))");
  const panelError = () => evalJs("document.querySelector('[data-bb=links-error]')?.textContent ?? null");
  const gl = async () => JSON.parse(await evalJs("JSON.stringify(window.__bbGl)"));
  const pixels = () => evalJs(`(async () => {
    await new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)));
    const c = document.getElementById('bimble'), g = c.getContext('webgl2');
    const w = c.width, h = c.height, p = new Uint8Array(w * h * 4);
    g.readPixels(0, 0, w, h, g.RGBA, g.UNSIGNED_BYTE, p);
    let tint = 0;
    for (let i = 0; i < p.length; i += 4) { const r = p[i], gg = p[i + 1], b = p[i + 2]; if (b > gg && gg > r && b - r >= 25 && r >= 140) tint++; }
    return tint; })()`);
  /** Overlay pixels in the reference edge colour (52,110,180) in a box (lab_refs_dxf). */
  const refPx = (box) => evalJs(`(() => { const o = document.querySelector('[data-bb-overlay]'); const c = o.getContext('2d');
    const r = document.getElementById('bimble').getBoundingClientRect(); const b = ${JSON.stringify(box)};
    const x0 = Math.max(0, Math.floor(b[0] - r.left)), y0 = Math.max(0, Math.floor(b[1] - r.top)); const d = c.getImageData(x0, y0, b[2], b[3]).data; let n = 0;
    for (let i = 0; i < d.length; i += 4) if (d[i + 3] > 100 && Math.abs(d[i] - 52) < 40 && Math.abs(d[i + 1] - 110) < 40 && Math.abs(d[i + 2] - 180) < 40) n++;
    return n; })()`);

  await waitFor(`${mode}: the first drawn frame`, 30000, () => evalJs("window.__bbGl && window.__bbGl.firstFrameMs"));
  await sleep(300);

  // ---- Manage links… opens the panel ----
  check(`${mode}: no panel before Manage links…`, !(await has("[data-bb=links-panel]")));
  await B("return b.runAction('manage_links');");
  await waitFor(`${mode}: the Links panel`, 5000, () => has("[data-bb=links-panel]"));
  check(`${mode}: Manage links… opens the panel; Link IFC… enabled, Link DXF… off in 3D`,
    await evalJs("!document.querySelector('[data-bb=link-add-model]').disabled && document.querySelector('[data-bb=link-add-dxf]').disabled"));

  // ---- two IFC links through the dialog ----
  const tint0 = await pixels();
  for (const f of ["m1.ifc", "f2.ifc"]) {
    await click("[data-bb=link-add-model]");
    await pick(join(work, f));
    await waitFor(`${mode}: the row of ${f}`, 10000, () => has(`[data-bb=link-row][data-name='${f}']`)).catch(() => null);
  }
  const r1 = await rows();
  check(`${mode}: two rows, visible`, r1.map((r) => `${r.name}:${r.visible}`).join() === "m1.ifc:true,f2.ifc:true", JSON.stringify(r1) + (await panelError()));
  await waitFor(`${mode}: two links in the viewport`, 10000, async () => (await gl()).links.length === 2).catch(() => null);
  await settle();
  const tint1 = await pixels();
  check(`${mode}: the links drawn (more pixels in the link tint)`, tint1 > tint0 + 200, `${tint0} -> ${tint1}`);
  // ---- hide f2 (the larger) with the eye: its pixels go; show again ----
  await click("[data-bb=link-row][data-name='f2.ifc'] [data-bb=link-eye]");
  await waitFor(`${mode}: f2 hidden in the viewport`, 10000, async () => (await gl()).links.find((l) => l.name === "f2.ifc")?.visible === false).catch(() => null);
  await settle();
  const tint2 = await pixels();
  check(`${mode}: the eye hides f2: its row says so, its pixels go`, (await rows())[1]?.visible === false && tint2 < tint1 - 100, `${tint1} -> ${tint2}`);
  await click("[data-bb=link-row][data-name='f2.ifc'] [data-bb=link-eye]");
  await waitFor(`${mode}: f2 shown`, 10000, async () => (await gl()).links.find((l) => l.name === "f2.ifc")?.visible === true).catch(() => null);
  await settle();
  check(`${mode}: the eye shows it again`, (await rows())[1]?.visible === true && (await pixels()) > tint2 + 100);
  // ---- reload (wasm: the picker again) ----
  const gen0 = await B("return b.state.value.links ?? 0;");
  await click("[data-bb=link-row][data-name='m1.ifc'] [data-bb=link-reload]");
  if (mode === "wasm") await pick(join(work, "m1.ifc"));
  await waitFor(`${mode}: reloaded`, 10000, async () => (await B("return b.state.value.links ?? 0;")) > gen0).catch(() => null);
  check(`${mode}: Reload: no error, the row kept, in its place`, (await panelError()) === null && (await rows()).map((r) => r.name).join() === "m1.ifc,f2.ifc",
    `${await panelError()} ${JSON.stringify(await rows())}`);
  // ---- remove f2 ----
  await click("[data-bb=link-row][data-name='f2.ifc'] [data-bb=link-remove]");
  await waitFor(`${mode}: f2 removed`, 10000, async () => (await gl()).links.length === 1).catch(() => null);
  check(`${mode}: remove: the row and the link go`, (await rows()).map((r) => r.name).join() === "m1.ifc" && (await gl()).links.length === 1);

  // ---- SIZE2FIX2: a link's mesh asked with its name and the key is never a 404, whatever happened to the link: the
  // host asked directly (the heads' key read first), the link unloaded (no geometry, the key read again after), of
  // another name, of an older key, out of range, removed while its request is held (CDP Fetch); a bare index out of
  // range stays 404. Fails on 7233611: an unloaded link asked with the current key answered 404 ----
  if (mode === "native") {
    const H = `http://127.0.0.1:${PORT}`;
    const heads = async () => { const ab = new Uint8Array(await (await fetch(`${H}/mesh?t=${TOKEN}&shapes=1&lazy=1`)).arrayBuffer()); const dv = new DataView(ab.buffer);
      let o = 52 + 48 * dv.getUint32(12, true) + 12 * dv.getUint32(16, true) + 4 * dv.getUint32(20, true) + 32 * dv.getUint32(24, true) + 52 * dv.getUint32(12, true);
      const n = dv.getUint32(o + 8, true); o += 12; const out = [];
      for (let k = 0; k < n; k++) { const L = dv.getUint32(o, true), name = new TextDecoder().decode(ab.subarray(o + 4, o + 4 + L)); o += 4 + L + ((4 - (L % 4)) % 4);
        out.push({ name, nE: dv.getUint32(o + 48, true), key: dv.getUint32(o + 52, true) }); o += 56; }
      return out; };
    const ask = async (k, q) => (await fetch(`${H}/mesh?t=${TOKEN}&link=${k}${q}`)).status;
    const h0 = await heads();
    const m1 = h0.findIndex((h) => h.name === "m1.ifc"), key0 = h0[m1]?.key;
    const st = {};
    st.ready = await ask(m1, `&name=m1.ifc&key=${key0}`);
    st.otherName = await ask(m1, `&name=zz.ifc&key=${key0}`);
    st.olderKey = await ask(m1, `&name=m1.ifc&key=${key0 + 1000}`);
    st.outOfRange = await ask(99, `&name=m1.ifc&key=${key0}`);
    st.bareOutOfRange = await ask(99, "");
    await B(`b.actionBody("manage_links", { op: "unload", name: "m1.ifc" }); return 1;`);
    const h1 = await heads();
    st.unloadedNow = await ask(m1, `&name=m1.ifc&key=${h1[m1]?.key}`);
    await B(`b.actionBody("manage_links", { op: "load", name: "m1.ifc" }); return 1;`);
    // the page's own request held while its link is removed, then let go
    await S("Fetch.enable", { patterns: [{ urlPattern: "*mesh?*link=*", requestStage: "Request" }] });
    let held = null;
    const onPause = (msg) => { if (msg.method === "Fetch.requestPaused" && !held) held = msg.params; else if (msg.method === "Fetch.requestPaused") S("Fetch.continueRequest", { requestId: msg.params.requestId }).catch(() => {}); };
    cdp.on(onPause);
    await B(`b.manageLink("reload", "m1.ifc"); return 1;`);
    const paused = await waitFor("a link's mesh request held", 10000, () => held).catch(() => null);
    const r0 = meshResponses.length;
    if (paused) {
      await B(`b.actionBody("manage_links", { op: "remove", name: "m1.ifc" }); return 1;`);
      await sleep(300);
      await S("Fetch.continueRequest", { requestId: paused.requestId });
    }
    await waitFor("its answer", 5000, () => meshResponses.slice(r0).some((r) => /(^|&)link=/.test(r.url))).catch(() => null);
    st.removedWhileHeld = meshResponses.slice(r0).find((r) => /(^|&)link=/.test(r.url))?.status ?? null;
    await S("Fetch.disable");
    held = { requestId: "" }; // (no more holds)
    await B(`b.actionBody("link_model", { path: ${JSON.stringify(join(work, "m1.ifc"))} }); return 1;`);
    await waitFor(`${mode}: m1 back`, 10000, async () => (await gl()).links.some((l) => l.name === "m1.ifc")).catch(() => null);
    check(`${mode}: a named, keyed request for a link's mesh is never a 404 (superseded: 204), a bare one out of range is`,
      st.ready === 200 && st.otherName === 204 && st.olderKey === 204 && st.outOfRange === 204 && st.unloadedNow === 204 && st.removedWhileHeld === 204 && st.bareOutOfRange === 404,
      JSON.stringify(st));
  }

  // ---- SIZE2FIX: links removed, unloaded, loaded and read again while their meshes are in flight (each request
  // 250 ms late): a link's mesh is asked by its name and the heads' key; one asked after the links changed is
  // superseded (204), dropped quietly, the heads asked again: no 404, no console error (checked at the end) ----
  if (mode === "native") {
    const m0 = meshResponses.length, c0 = consoleLog.length;
    await S("Network.emulateNetworkConditions", { offline: false, latency: 250, downloadThroughput: -1, uploadThroughput: -1 });
    const f2 = JSON.stringify(join(work, "f2.ifc"));
    for (let i = 0; i < 4; i++) {
      for (const body of [`b.actionBody("link_model", { path: ${f2} })`, `b.actionBody("manage_links", { op: "unload", name: "m1.ifc" })`,
        `b.actionBody("manage_links", { op: "load", name: "m1.ifc" })`, `b.actionBody("manage_links", { op: "remove", name: "f2.ifc" })`,
        `b.manageLink("reload", "m1.ifc")`]) {
        await B(`${body}; return 1;`);
        await sleep(90 + 70 * (i % 3));
      }
    }
    await S("Network.emulateNetworkConditions", { offline: false, latency: 0, downloadThroughput: -1, uploadThroughput: -1 });
    const lastLink = () => meshResponses.slice(m0).filter((r) => /(^|&)link=/.test(r.url)).pop();
    const settled = await waitFor(`${mode}: the links settled`, 20000, async () => { const g = await gl(); return g.links.length === 1 && g.links[0].name === "m1.ifc" && g.linkTris > 0 &&
      lastLink()?.status === 200 && (await rows()).map((r) => r.name).join() === "m1.ifc" ? g : null; }).catch(() => null);
    const asked = meshResponses.slice(m0), links = asked.filter((r) => /(^|&)link=/.test(r.url)), bad = asked.filter((r) => r.status >= 400);
    const said = consoleLog.slice(c0).filter((m) => /error|warn/.test(m.level)).map((m) => m.text);
    check(`${mode}: links changed while their meshes were in flight: no /mesh answer of 400 or more, nothing in the console, the page settles on the links as they are`,
      bad.length === 0 && said.length === 0 && links.length > 0 && settled !== null,
      `${asked.length} /mesh answers (${links.length} of a link: ${[...new Set(links.map((r) => r.status))].join(", ")}; the last ${links.at(-1)?.status}), bad ${JSON.stringify(bad.slice(0, 3))}, console ${JSON.stringify(said.slice(0, 2))}, superseded ${settled?.linkSuperseded ?? 0}, links ${JSON.stringify(settled?.links?.map((l) => l.name))}`);
  }

  // ---- a DXF in the ground plan: move by numbers, rotate, undo ----
  const plan = await B("return b.openViews.value.views.find((v) => v.type === 'plan' && v.look === 'down');");
  await B(`return b.openView(${JSON.stringify(plan.id)});`);
  await waitFor(`${mode}: the plan camera`, 10000, async () => (await evalJs("window.__bbGl.camera && window.__bbGl.camera.mode")) === "plan").catch(() => null);
  await B("return b.runAction('manage_links');");
  await waitFor(`${mode}: Link DXF… enabled in the plan`, 5000, () => evalJs("document.querySelector('[data-bb=link-add-dxf]')?.disabled === false")).catch(() => null);
  await click("[data-bb=link-add-dxf]");
  await pick(join(work, "r12.dxf"));
  await waitFor(`${mode}: the DXF's row`, 10000, () => has("[data-bb=ref-row][data-name='r12.dxf']")).catch(() => null);
  const ref0 = (await B("return b.query('links_panel');")).references[0];
  check(`${mode}: the DXF's row names its view`, ref0?.view === plan.id && ref0.name === "r12.dxf", JSON.stringify(ref0));
  await waitFor(`${mode}: the reference drawn`, 10000, async () => (await evalJs("window.__bbRefs && window.__bbRefs.segments")) > 0).catch(() => null);
  await settle();
  const c0 = JSON.parse(await evalJs(`JSON.stringify(window.__bbGl.toScreen(${ref0?.offset_mm[0] ?? 0}, ${ref0?.offset_mm[1] ?? 0}, 0))`));
  check(`${mode}: its (0, 0) is drawn at its offset (within 1 px)`, !!c0 && (await refPx([c0[0] - 1, c0[1] - 1, 3, 3])) > 0, JSON.stringify(c0));
  await setInput("[data-bb=ref-row] [data-bb=ref-dx]", 1000);
  await setInput("[data-bb=ref-row] [data-bb=ref-dy]", 0);
  const undo0 = await B("return b.query('history').undo.length;");
  await click("[data-bb=ref-row] [data-bb=ref-move]");
  const ref1 = await waitFor(`${mode}: moved`, 5000, async () => {
    const r = (await B("return b.query('links_panel');")).references[0];
    return r && r.offset_mm[0] !== ref0.offset_mm[0] ? r : null;
  }).catch(() => null);
  check(`${mode}: ΔX 1000 + Move: the offset moves by 1000 mm, one undo step`, ref1 && ref1.offset_mm[0] === ref0.offset_mm[0] + 1000 &&
    ref1.offset_mm[1] === ref0.offset_mm[1] && (await B("return b.query('history').undo.length;")) === undo0 + 1, JSON.stringify(ref1));
  await settle();
  const c1 = JSON.parse(await evalJs(`JSON.stringify(window.__bbGl.toScreen(${ref1?.offset_mm[0] ?? 0}, ${ref1?.offset_mm[1] ?? 0}, 0))`));
  const per = JSON.parse(await evalJs(`(() => { const a = window.__bbGl.toScreen(0, 0, 0), b = window.__bbGl.toScreen(1000, 0, 0); return JSON.stringify([b[0] - a[0], b[1] - a[1]]); })()`));
  check(`${mode}: the corner is drawn 1000 mm further (${per?.map((v) => v.toFixed(1)).join(", ")} px), within 1 px`,
    !!c1 && !!c0 && Math.abs(c1[0] - c0[0] - per[0]) < 0.5 && Math.abs(c1[1] - c0[1] - per[1]) < 0.5 && (await refPx([c1[0] - 1, c1[1] - 1, 3, 3])) > 0,
    `${JSON.stringify(c0)} -> ${JSON.stringify(c1)}`);
  await setInput("[data-bb=ref-row] [data-bb=ref-angle]", 90);
  await click("[data-bb=ref-row] [data-bb=ref-rotate]");
  const rot = await waitFor(`${mode}: rotated`, 5000, async () => (await B("return b.query('links_panel');")).references[0]?.rotation_deg === 90).catch(() => false);
  check(`${mode}: Angle 90 + Rotate: 90°`, rot);
  await B("return b.undo();");
  const back = await waitFor(`${mode}: undone`, 5000, async () => (await B("return b.query('links_panel');")).references[0]?.rotation_deg === 0).catch(() => false);
  check(`${mode}: undo: 0° again`, back);

  if (mode === "native") {
    // ---- save, reopen: the rows are back ----
    const saved = join(work, "coord.bimble");
    const se = await B(`try { b.actionBody('save_as', { path: ${JSON.stringify(saved)} }); return null; } catch (e) { return String(e); }`);
    check("native: saved", se === null && existsSync(saved), String(se));
    const f = await import("node:fs");
    check("native: the file names the link (section links)", existsSync(saved) && f.statSync(saved).size > 0);
    await evalJs(`(async () => (await import('/src/files.ts')).openPath(${JSON.stringify(join(work, "host.json"))}))()`);
    await waitFor("native: host.json open", 10000, async () => (await B("return b.state.value.file;")) === "host.json").catch(() => null);
    await evalJs(`(async () => (await import('/src/files.ts')).openPath(${JSON.stringify(saved)}))()`);
    await waitFor("native: coord.bimble open", 10000, async () => (await B("return b.state.value.file;")) === "coord.bimble").catch(() => null);
    await B("return b.runAction('manage_links');");
    await waitFor("native: the panel", 5000, () => has("[data-bb=links-panel]")).catch(() => null);
    await waitFor("native: rows", 5000, async () => (await rows()).length > 0).catch(() => null);
    check("native: reopened: m1.ifc and r12.dxf are back", (await rows()).map((r) => `${r.name}:${r.missing}`).join() === "m1.ifc:false" &&
      (await has("[data-bb=ref-row][data-name='r12.dxf']")), JSON.stringify(await rows()));

    // ---- an IFC opened as it is: a plan On paper has the cut of its elements ----
    await evalJs(`(async () => (await import('/src/files.ts')).openPath(${JSON.stringify(join(work, "frame.ifc"))}))()`);
    await waitFor("native: frame.ifc open read-only", 10000, async () => (await B("return b.state.value.file;")) === "frame.ifc").catch(() => null);
    const p2 = await B("return b.openViews.value.views.find((v) => v.type === 'plan' && v.look === 'down');");
    await B(`return b.openView(${JSON.stringify(p2?.id)});`);
    await waitFor("native: the switch offered", 10000, () => has("[data-bb=view-picture]")).catch(() => null);
    check("native: read-only IFC: the Model / On paper switch is offered", await has("[data-bb=view-picture]"));
    if (await has("[data-bb=view-picture]")) {
      await evalJs("document.querySelector('[data-bb=view-picture]').children[1].click()");
      await waitFor("native: the paper picture", 10000, () => has("[data-bb=paper-view-image]")).catch(() => null);
      const svg = await evalJs("decodeURIComponent(document.querySelector('[data-bb=paper-view-image]')?.getAttribute('href')?.replace(/^data:image\\/svg\\+xml;charset=utf-8,/, '') ?? '')");
      const paths = (svg.match(/<path/g) ?? []).length;
      check(`native: read-only IFC On paper: an SVG with the cut of its elements (${paths} paths)`, svg.includes(p2.name) && paths > 20, svg.slice(0, 120));
      await evalJs("document.querySelector('[data-bb=view-picture]').children[0].click()");
    }

    // ---- a links-only .bimble: Export view → PDF ----
    const lonly = join(work, "links-only.bimble");
    const mk = [await host("action", "new", { discard: true }), await host("action", "link_model", { path: join(work, "frame.ifc") }),
      await host("action", "save_as", { path: lonly })];
    check("native: a links-only .bimble made (new, link frame.ifc, save as)", mk.every((r) => r.status === 200), mk.map((r) => r.text).join("|"));
    await evalJs(`(async () => (await import('/src/files.ts')).openPath(${JSON.stringify(join(work, "host.json"))}))()`);
    await waitFor("native: host.json open", 10000, async () => (await B("return b.state.value.file;")) === "host.json").catch(() => null);
    await evalJs(`(async () => (await import('/src/files.ts')).openPath(${JSON.stringify(lonly)}))()`);
    await waitFor("native: links-only.bimble open", 10000, async () => (await B("return b.state.value.file;")) === "links-only.bimble").catch(() => null);
    const p3 = await B("return b.openViews.value.views.find((v) => v.type === 'plan' && v.look === 'down');");
    await B(`return b.openView(${JSON.stringify(p3?.id)});`);
    await sleep(300);
    const dl = join(tmp, "dl");
    mkdirSync(dl, { recursive: true });
    downloads.clear();
    await cdp.send("Browser.setDownloadBehavior", { behavior: "allow", downloadPath: dl, eventsEnabled: true });
    await evalJs("document.querySelector('[data-bb=quickbar-file]').click()");
    await waitFor("native: the File menu", 5000, () => has("[data-bb=file-menu-on_paper]")).catch(() => null);
    await evalJs("document.querySelector('[data-bb=file-menu-on_paper]').click()");
    await waitFor("native: the Export dialog", 5000, () => evalJs("document.querySelector('[data-bb=export-what-view]')?.classList.contains('on') === true")).catch(() => null);
    await evalJs("document.querySelector('[data-bb=export-pdf]')?.click()");
    await evalJs("document.querySelector('[data-bb=export-go]')?.click()");
    const done = await waitFor("native: the PDF download", 20000, async () => {
      const err = await evalJs("document.querySelector('[data-bb=export-failed]')?.textContent ?? ''");
      if (err) throw new Error(err);
      return [...downloads.values()].find((d) => d.state === "completed") ?? null;
    }).catch((e) => ({ error: e.message }));
    const file = done.name ? join(dl, done.name) : "";
    const bytes = file && existsSync(file) ? readFileSync(file) : Buffer.alloc(0);
    const text = bytes.length ? pdftotext(file) : "";
    const title = `${p3?.name}, 1:100`;
    check(`native: links-only .bimble: Export view PDF, pdftotext finds "${title}"`, text !== null && (text ?? "").replace(/\s+/g, " ").includes(title),
      text === null ? "pdftotext is not installed (poppler)" : `${JSON.stringify(done)} ${(text ?? "").slice(0, 120).replace(/\s+/g, " ")}`);
    // The frame and title block draw a few dozen strokes; the cut of the linked frame draws hundreds.
    const ops = (bytes.toString("latin1").match(/ (l|re)\n/g) ?? []).length;
    check(`native: links-only .bimble: the PDF draws beyond the frame (${ops} line and rectangle operators)`, ops > 150, String(ops));
    await evalJs("(async () => { const d = await import('/src/dialogs/index.ts'); d.currentDialog.value = null; })()");
  }

  const IGNORED = [
    /Synchronous XMLHttpRequest on the main thread is deprecated/,
    /404 \(Not Found\) http:\/\/localhost:\d+\/favicon\.ico$/,
    /^bimble-ccore: /, // the core's log lines (console.info in wasm)
    /Canvas2D: Multiple readback operations using getImageData/, // the test's own pixel reads of the overlay
  ];
  const errors = consoleLog.filter((m) => /error|warn/.test(m.level) && !IGNORED.some((r) => r.test(m.text)));
  check(`${mode}: no uncaught page error`, exceptions.length === 0, exceptions.join(" | "));
  check(`${mode}: no console error or warning`, errors.length === 0, errors.map((m) => m.text).join(" | "));
  await cdp.send("Target.closeTarget", { targetId });
}

async function main() {
  if (!existsSync(join(UI, "node_modules"))) throw new Error(`no ${UI}/node_modules: run npm ci in web-ui/`);
  execFileSync("node", [join(UI, "scripts/copy-ccore.mjs")], { stdio: "inherit" });
  copyFileSync(join(G, "frame.json"), MODEL_PUBLIC);
  mkdirSync(work, { recursive: true });
  copyFileSync(join(G, "frame.json"), join(work, "host.json"));
  copyFileSync(join(G, "ifc/fixtures/mapped_item.ifc"), join(work, "m1.ifc"));
  copyFileSync(join(G, "ifc/frame2.ifc"), join(work, "f2.ifc"));
  copyFileSync(join(G, "ifc/frame.ifc"), join(work, "frame.ifc"));
  copyFileSync(join(G, "dxf/fixtures/r12.dxf"), join(work, "r12.dxf"));
  const h = start("host", BIN, [String(PORT), join(work, "host.json")], {
    env: { ...process.env, HOME: home, BIMBLE_CONFIG_DIR: join(tmp, "config"), BIMBLE_TOKEN: TOKEN, BIMBLE_FONTS: "",
      BIMBLE_ORIGINS: `http://localhost:${VITE},http://127.0.0.1:${VITE}` },
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
  for (const mode of ["native", "wasm"]) {
    try { await onPage(cdp, mode); } catch (e) { check(`${mode}: the walk ran to the end`, false, e.stack ?? String(e)); }
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
console.log(`lab_links_manage: ${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
