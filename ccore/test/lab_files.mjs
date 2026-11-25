// Files from the lab's own UI (ticket FS1: web-ui/src/files.ts, dialogs/FileBrowser.vue, ConfirmDiscard.vue,
// ccore/host/fs.h, session/settings_file.h): node ccore/test/lab_files.mjs
// Starts out/tcc/bimble-ccore (or $HOST_BIN) with no model, HOME and BIMBLE_CONFIG_DIR in a temp folder
// (never the user's), Vite from web-ui/ and headless /usr/bin/chromium over raw CDP. A folder home/work holds
// frame.bimble (golden/frame.json saved as v2 by the host before the page opens).
// Native page (/?native=1&gl=1&port=…&token=…&unload=1):
//   File → Open shows the page's dialog on the home folder; a click on "work" lists it (.bimble shown,
//   the .txt filtered out); a click on frame.bimble and Open: the model shows (its elements, the mesh);
//   File → Save as proposes "frame.bimble", typed "saved" writes work/saved.bimble (the v2 magic), the
//   state names it; after a reload Home lists saved.bimble first (recent files) and settings.json has it;
//   an edit, then Home → New: the in-page question appears; "Keep editing" keeps the model and its change,
//   New again and "Discard" makes an Untitled model; an edit, then a click on a recent file asks, "Discard"
//   opens it; a recent entry removed from Home leaves the list and the file; the page's beforeunload
//   asks before leaving with changes (Page.javascriptDialogOpening, dismissed: the page stays); a PDF
//   dropped on the native page is refused with a message (a reference plan keeps a path); a .json dropped
//   asks (changes), then opens under its name (POST /stage), is no recent file, and its Save is Save as.
// Wasm page (/?ccore=1&gl=1): a .json dropped on the page opens under its name (the bytes).
// No uncaught page error; no console error or warning but the sync-XHR deprecation, the favicon 404 and
// the drop refusal logged on purpose (native: from the first open on; before it, the lab booted without a
// model answers 400 to the queries that need one: UX1's ground, not FS1's).
// Prints "N passed, M failed"; exits non-zero on a failure. Kills all it starts.
import { spawn } from "node:child_process";
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { execFileSync } from "node:child_process";
import { CHROMIUM, port, vitePort } from "./ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = resolve(CCORE, "../web-ui");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("lab_files", 47302);
const VITE = vitePort("lab_files", 5194);
const TOKEN = "fs1-lab-token";
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-files-"));
const home = join(tmp, "home"), work = join(home, "work"), conf = join(tmp, "config");
const PAGES = {
  native: `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}&unload=1`,
  wasm: `http://localhost:${VITE}/?ccore=1&gl=1`,
};
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

const hostCall = async (op, name = "-", body = "") => {
  const r = await fetch(`http://127.0.0.1:${PORT}/${op}/${encodeURIComponent(name)}?t=${TOKEN}`,
    { method: "POST", body, headers: { "Content-Type": "text/plain" } });
  return { status: r.status, text: await r.text() };
};
const MAGIC = Buffer.from("bimble\0", "latin1");

async function page(cdp, mode) {
  const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank" });
  const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
  const S = (m, p) => cdp.send(m, p, sessionId);
  const consoleLog = [], exceptions = [], dialogs = [];
  cdp.on((msg) => {
    if (msg.sessionId !== sessionId) return;
    if (msg.method === "Runtime.consoleAPICalled") {
      consoleLog.push({ level: msg.params.type, text: msg.params.args.map((a) => a.value ?? a.description ?? a.type).join(" ") });
    } else if (msg.method === "Runtime.exceptionThrown") {
      const d = msg.params.exceptionDetails;
      exceptions.push(d.exception?.description ?? d.text);
    } else if (msg.method === "Log.entryAdded") {
      consoleLog.push({ level: `log.${msg.params.entry.level}`, text: `${msg.params.entry.text} ${msg.params.entry.url ?? ""}`.trim() });
    } else if (msg.method === "Page.javascriptDialogOpening") {
      dialogs.push(msg.params.type);
      void S("Page.handleJavaScriptDialog", { accept: false }); // stay on the page
    }
  });
  for (const d of ["Runtime", "Log", "Page"]) await S(`${d}.enable`);
  await S("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  await S("Page.navigate", { url: PAGES[mode] });
  const evalJs = async (expr) => {
    const r = await S("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
    return r.result.value;
  };
  const B = (body) => evalJs(`(async () => { const b = await import('/src/bimble.ts'); ${body} })()`);
  const click = (sel) => evalJs(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) throw new Error('no ' + ${JSON.stringify(sel)}); e.click(); return true; })()`);
  const has = (sel) => evalJs(`!!document.querySelector(${JSON.stringify(sel)})`);
  /** A real click (CDP input: the page gets a user activation, which beforeunload needs). */
  const realClick = async (sel) => {
    const [x, y] = await evalJs(`(() => { const r = document.querySelector(${JSON.stringify(sel)}).getBoundingClientRect(); return [r.x + r.width / 2, r.y + r.height / 2]; })()`);
    for (const type of ["mousePressed", "mouseReleased"]) await S("Input.dispatchMouseEvent", { type, x, y, button: "left", clickCount: 1 });
  };
  // Started, and the actions read (a menu entry is disabled until then).
  await waitFor(`${mode}: the lab started`, 30000, () => B("await b.ready; return b.actions.value.some((a) => a.name === 'open' && a.enabled);"));
  const drop = (name, text) => evalJs(`(() => {
    const dt = new DataTransfer(); dt.items.add(new File([${JSON.stringify(text)}], ${JSON.stringify(name)}));
    window.dispatchEvent(new DragEvent('drop', { dataTransfer: dt, bubbles: true, cancelable: true })); return true; })()`);
  return { S, evalJs, B, click, has, realClick, drop, consoleLog, exceptions, dialogs, close: () => cdp.send("Target.closeTarget", { targetId }) };
}

function noErrors(P, mode, extra = []) {
  const IGNORED = [
    /Synchronous XMLHttpRequest on the main thread is deprecated/,
    /404 \(Not Found\) http:\/\/localhost:\d+\/favicon\.ico$/,
    ...extra,
  ];
  const errors = P.consoleLog.slice(P.mark ?? 0).filter((m) => /error|warn/.test(m.level) && !IGNORED.some((r) => r.test(m.text)));
  check(`${mode}: no uncaught page error`, P.exceptions.length === 0, P.exceptions.join(" | "));
  check(`${mode}: no console error or warning`, errors.length === 0, errors.map((m) => m.text).join(" | "));
}

async function native(cdp) {
  const P = await page(cdp, "native");
  const { B, click, has } = P;
  const file = () => B("return b.state.value.file;");

  // ---- File → Open: the page's dialog ----
  await click("[data-bb=quickbar-file]");
  await waitFor("the File menu", 5000, () => has("[data-bb=file-menu-open]"));
  await click("[data-bb=file-menu-open]");
  await waitFor("the file dialog", 5000, () => has("[data-bb=file-browser] [data-bb=file-browser-entry]")).catch(async (e) => {
    throw new Error(`${e.message}; page: ${await P.evalJs("document.querySelector('[data-bb=file-browser]')?.textContent ?? document.querySelector('[data-bb=file-menu]')?.outerHTML?.slice(0, 300) ?? 'no dialog'")}`);
  });
  const path0 = await P.evalJs("document.querySelector('[data-bb=file-browser-path]').value");
  check("Open: the dialog shows the home folder", path0 === home, path0);
  check("Open: its purpose is open", await P.evalJs("document.querySelector('[data-bb=file-browser]').dataset.purpose") === "open");
  await click("[data-bb=file-browser-entry][data-name=work]");
  await waitFor("work listed", 5000, () => P.evalJs("document.querySelector('[data-bb=file-browser-path]').value.endsWith('/work')"));
  const names = await P.evalJs("[...document.querySelectorAll('[data-bb=file-browser-entry]')].map((e) => e.dataset.name)");
  check("Open: work lists frame.bimble, not notes.txt", names.includes("frame.bimble") && !names.includes("notes.txt"), JSON.stringify(names));
  await click("[data-bb=file-browser-entry][data-name='frame.bimble']");
  await click("[data-bb=file-browser-go]");
  await waitFor("frame.bimble open", 10000, async () => (await file()) === "frame.bimble");
  // The page booted with no model: the queries that need one answered 400 and the viewport's /mesh 404
  // until now (the lab's boot without a model, not FS1's). From here on, no error.
  await sleep(300);
  P.mark = P.consoleLog.length;
  check("Open: the dialog closed", !(await has("[data-bb=file-browser]")));
  const els = await B("return b.query('elements', { limit: 10000 }).length;");
  check("Open: the model shows (its elements)", els > 0, `${els}`);
  const mesh = await fetch(`http://127.0.0.1:${PORT}/mesh?t=${TOKEN}`).then((r) => r.arrayBuffer());
  check("Open: the host's mesh has them", Buffer.from(mesh).readUInt32LE(12) === els, `${Buffer.from(mesh).readUInt32LE(12)} vs ${els}`);

  // ---- File → Save as ----
  await click("[data-bb=quickbar-file]");
  await waitFor("the File menu", 5000, () => has("[data-bb=file-menu-save_as]"));
  check("File menu: Save as enabled", !(await P.evalJs("document.querySelector('[data-bb=file-menu-save_as]').disabled")));
  await click("[data-bb=file-menu-save_as]");
  await waitFor("the save dialog", 5000, () => has("[data-bb=file-browser-name]"));
  const proposed = await P.evalJs("document.querySelector('[data-bb=file-browser-name]').value");
  check("Save as: proposes frame.bimble, in work (the folder listed last for… open: a new purpose starts at home)", proposed === "frame.bimble", proposed);
  await click("[data-bb=file-browser-entry][data-name=work]");
  await waitFor("work listed", 5000, () => P.evalJs("document.querySelector('[data-bb=file-browser-path]').value.endsWith('/work')"));
  check("Save as: an existing name is flagged", await has("[data-bb=file-browser-replace]"));
  await P.evalJs("(() => { const i = document.querySelector('[data-bb=file-browser-name]'); i.value = 'saved'; i.dispatchEvent(new Event('input')); })()");
  await click("[data-bb=file-browser-go]");
  const saved = join(work, "saved.bimble");
  await waitFor("saved.bimble written", 10000, async () => existsSync(saved) && (await file()) === "saved.bimble");
  check("Save as: work/saved.bimble, the v2 magic", Buffer.compare(readFileSync(saved).subarray(0, 7), MAGIC) === 0);
  check("Save as: the state names it, saved", (await file()) === "saved.bimble" && !(await B("return b.state.value.unsaved;")));

  // ---- Recent files after a reload ----
  await P.S("Page.reload");
  await waitFor("the lab again", 30000, () => B("await b.ready; return b.state.value.file === 'saved.bimble';"));
  await waitFor("the actions again", 10000, () => B("return b.actions.value.some((a) => a.name === 'open' && a.enabled);"));
  if (!(await has("[data-bb=home]"))) await click("[data-bb=quickbar-logo]");
  await waitFor("Home's recent files", 5000, () => has("[data-bb=home-recent-file]"));
  const rec = await P.evalJs("[...document.querySelectorAll('[data-bb=home-recent-file]')].map((e) => e.dataset.path)");
  check("Home after a reload: saved.bimble first, then frame.bimble", rec[0] === saved && rec[1] === join(work, "frame.bimble"), JSON.stringify(rec));
  const st = JSON.parse(readFileSync(join(conf, "settings.json"), "utf8"));
  check("settings.json: recent has it first (Rust's field)", st.recent[0] === saved, JSON.stringify(st));

  // ---- The unsaved guard: New, both answers ----
  const place = async () => {
    const sto = JSON.parse((await hostCall("query", "storeys", "{}")).text);
    return hostCall("verb", "place", JSON.stringify({ a: [1000, 0], kind: "column", storey: sto[0].id }));
  };
  const pl = await place();
  check("an edit (verb place)", pl.status === 200, pl.text);
  await waitFor("unsaved", 5000, () => B("return b.state.value.unsaved;"));
  await click("[data-bb=home-new]");
  await waitFor("the question", 5000, () => has("[data-bb=confirm-discard]"));
  check("New with changes: the in-page question (no window.confirm)", P.dialogs.length === 0);
  await click("[data-bb=confirm-discard-no]");
  await sleep(300);
  check("Keep editing: the model and its change stay", (await file()) === "saved.bimble" && (await B("return b.state.value.unsaved;")) && !(await has("[data-bb=confirm-discard]")));
  await click("[data-bb=home-new]");
  await waitFor("the question again", 5000, () => has("[data-bb=confirm-discard]"));
  await click("[data-bb=confirm-discard-yes]");
  await waitFor("Untitled", 5000, async () => (await file()) === "Untitled.bimble");
  check("Discard: a new Untitled model", true);

  // ---- A recent file with changes: asks, then opens ----
  await place();
  await waitFor("unsaved", 5000, () => B("return b.state.value.unsaved;"));
  await click(`[data-bb=home-recent-file][data-path=${JSON.stringify(saved)}]`);
  await waitFor("the question", 5000, () => has("[data-bb=confirm-discard]"));
  await click("[data-bb=confirm-discard-yes]");
  await waitFor("saved.bimble open", 5000, async () => (await file()) === "saved.bimble");
  check("a recent file with changes: asked, then opened", true);

  // ---- beforeunload: leaving with changes asks (the browser's own question) ----
  await place();
  await waitFor("unsaved", 5000, () => B("return b.state.value.unsaved;"));
  await P.realClick("[data-bb=home-filter]"); // a user activation: Chromium asks only then
  await P.S("Page.navigate", { url: "about:blank" }).catch(() => null);
  await waitFor("the beforeunload question", 5000, () => P.dialogs.includes("beforeunload"));
  await sleep(200);
  check("beforeunload with changes: asked, dismissed: the page stays", (await file()) === "saved.bimble", P.dialogs.join());

  // ---- Remove a recent entry ----
  if (!(await has("[data-bb=home]"))) await click("[data-bb=quickbar-logo]");
  await waitFor("Home's recent files", 5000, () => has("[data-bb=home-recent-file]"));
  const n0 = await P.evalJs("document.querySelectorAll('[data-bb=home-recent-file]').length");
  await click(`[data-bb=home-recent-file][data-path=${JSON.stringify(join(work, "frame.bimble"))}] [data-bb=home-recent-remove]`);
  await sleep(200);
  const rec2 = await P.evalJs("[...document.querySelectorAll('[data-bb=home-recent-file]')].map((e) => e.dataset.path)");
  check("remove: the entry leaves the list, the file stays", rec2.length === n0 - 1 && !rec2.includes(join(work, "frame.bimble")) && existsSync(join(work, "frame.bimble")), JSON.stringify(rec2));
  check("remove: settings.json follows", !JSON.parse(readFileSync(join(conf, "settings.json"), "utf8")).recent.includes(join(work, "frame.bimble")));

  // ---- Drop on the native page: the bytes staged by the host (POST /stage), opened as a dropped document ----
  await P.drop("plan.pdf", "%PDF-1.4");
  await sleep(200);
  const err = await P.evalJs("(async () => (await import('/src/files.ts')).fileError.value)()");
  check("a PDF dropped on the native page: refused with a message (a reference plan keeps a path)", (err ?? "").includes("Link PDF"), err);
  check("... and the model stays", (await file()) === "saved.bimble");
  await P.drop("dropped.json", readFileSync(join(CCORE, "golden/frame.json"), "utf8"));
  await waitFor("the question (the model has a change)", 5000, () => has("[data-bb=confirm-discard]"));
  await click("[data-bb=confirm-discard-yes]");
  await waitFor("dropped.json open", 10000, async () => (await file()) === "dropped.json");
  check("a .json dropped on the native page: asked, then open under its name", (await B("return b.query('elements', { limit: 10000 }).length;")) === els);
  check("... no recent file for it", !(await B("return b.recentFiles().map((f) => f.name);")).includes("dropped.json"));
  await hostCall("verb", "place", JSON.stringify({ a: [2000, 0], kind: "column", storey: JSON.parse((await hostCall("query", "storeys", "{}")).text)[0].id }));
  await waitFor("unsaved", 5000, () => B("return b.state.value.unsaved;"));
  const before = P.consoleLog.length;
  await B("b.runAction('save'); return true;");
  await waitFor("Save of a dropped model: the Save as dialog", 5000, () => P.evalJs("document.querySelector('[data-bb=file-browser]')?.dataset.purpose === 'save_as'"));
  // The page tries Save first; the host's 400 "a new model has no file yet" sends it to Save as (one log line).
  const late = P.consoleLog.splice(before);
  P.consoleLog.push(...late.filter((m) => !/status of 400/.test(m.text)));
  check("Save of a dropped model asks where (Save as)", await P.evalJs("document.querySelector('[data-bb=file-browser-name]').value") === "dropped.bimble");
  await click("[data-bb=file-browser-close]");
  noErrors(P, "native", [/drop: plan\.pdf: a reference plan keeps its file's path/]);
  await P.close();
}

async function wasm(cdp) {
  const P = await page(cdp, "wasm");
  await waitFor("wasm: the demo model", 30000, () => P.B("return b.state.value.file === 'frame.json';"));
  await P.drop("dropped.json", readFileSync(join(CCORE, "golden/frame.json"), "utf8"));
  await waitFor("wasm: the dropped file open", 10000, () => P.B("return b.state.value.file === 'dropped.json';"));
  check("wasm: a .json dropped on the page opens under its name", true);
  check("wasm: its elements", (await P.B("return b.query('elements', { limit: 10000 }).length;")) > 0);
  noErrors(P, "wasm");
  await P.close();
}

async function main() {
  if (!existsSync(join(UI, "node_modules"))) throw new Error(`no ${UI}/node_modules: run npm ci in web-ui/`);
  execFileSync("node", [join(UI, "scripts/copy-ccore.mjs")], { stdio: "inherit" });
  mkdirSync(work, { recursive: true });
  copyFileSync(join(CCORE, "golden/frame.json"), join(work, "frame.json"));
  writeFileSync(join(work, "notes.txt"), "not a model");
  const host = start("host", BIN, [String(PORT)], {
    env: { ...process.env, HOME: home, BIMBLE_CONFIG_DIR: conf, BIMBLE_TOKEN: TOKEN, BIMBLE_ORIGINS: `http://localhost:${VITE},http://127.0.0.1:${VITE}` },
  });
  await waitFor("host 'listening'", 5000, () => host.out.includes("listening") || (host.exitCode !== null && Promise.reject(new Error(host.out))));
  // frame.bimble: frame.json saved as v2 by the host, then closed (no model when the page opens).
  for (const [a, b] of [["open", { path: join(work, "frame.json") }], ["save_as", { path: join(work, "frame.bimble") }], ["close", {}]]) {
    const r = await hostCall("action", a, JSON.stringify(b));
    if (r.status !== 200) throw new Error(`${a}: ${r.text}`);
  }
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
  await native(cdp);
  await wasm(cdp);
  check("the user's settings never touched (HOME and BIMBLE_CONFIG_DIR in the temp folder)", !existsSync(join(home, ".config")));
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
console.log(`lab_files: ${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
