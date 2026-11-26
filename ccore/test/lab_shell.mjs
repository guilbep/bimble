// The lab shell's paths (ticket UX1, web-ui/src/shell/machine.ts): node ccore/test/lab_shell.mjs
// Clicks the real page through every path in and out of Home and asks, after each, whether the user
// still has a way on. Starts out/tcc/bimble-ccore (or $HOST_BIN) with no model, Vite from web-ui/
// (needs web-ui/node_modules) and headless /usr/bin/chromium over raw CDP, with real mouse and key
// events, and copies both wasm builds into web-ui/public/ccore/ (scripts/copy-ccore.mjs).
// Native host (/?native=1&gl=1&port=…&token=…):
//   N1 boot with nothing open: Home;
//   N2 Home -> New: the model view;
//   N3 unsaved changes, Home -> New: the question; Cancel -> Home, unchanged; New, Discard -> the view;
//   N4 Home -> Open (FS1): asked (unsaved), then the page's own file dialog; closed: Home, the model kept;
//      again, a bad file picked in it: the error, Back -> Home;
//   N5 an agent opens a file (POST /action/open) while Home shows: the model view;
//   N6 open of a bad file (send open with a path): the error, Back -> where it was;
//   N7 model -> Home -> "Back to the model": the view;
//   N8 a page (Quantities): its tab's close, Escape, Home and back: the view each time;
//   N9 the last view tab closed: Home (the core has no current tab); back to the model: a tab again;
//   N10 an agent closes the model while a page shows: Home;
//   N11 reload with a live session: the model view; reload with nothing open: Home;
//   N12 Home's "change" link: disabled while Settings is not available.
// Wasm (/?ccore=1&gl=1) and the viewer-only wasm (&ccore_wasm=ccore/ccore-view.wasm):
//   W1 boot: the model view; W2 Home -> Open, the picker cancelled: Home; the picker never answers:
//   Cancel -> Home; W3 a bad file picked: the error, Back -> Home; W4 a good file picked: the view;
//   W5 view-only: Home -> New refused: the error, Back -> Home -> back to the model.
// Ends with the table of the paths (a dead end: a FAIL row). No uncaught page error.
// Prints "N passed, M failed"; exits non-zero on a failure. Kills all it starts.
import { spawn, spawnSync, execFileSync } from "node:child_process";
import { copyFileSync, existsSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { CHROMIUM, port, vitePort } from "./ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = resolve(CCORE, "../web-ui");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("lab_shell", 47307);
const VITE = vitePort("lab_shell", 5220);
const TOKEN = "labshell-token";
const NATIVE = `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}`;
const WASM = `http://localhost:${VITE}/?ccore=1&gl=1`;
const VIEWONLY = `${WASM}&ccore_wasm=ccore/ccore-view.wasm`;
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-shell-"));
const procs = [];
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let passed = 0, failed = 0;
const table = []; // [path, ok, detail]
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  table.push([name, ok, detail]);
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
    for (const p of procs) {
      if (p.exitCode !== null || p.signalCode !== null) continue;
      if (process.platform === "win32") spawnSync("taskkill", ["/pid", String(p.pid), "/T", "/F"], { stdio: "ignore" });
      else try { process.kill(-p.pid, sig); } catch {}
    }
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

// The host's HTTP calls (as an agent would make them).
async function host(op, name, body = "") {
  const r = await fetch(`http://127.0.0.1:${PORT}/${op}/${name}?t=${TOKEN}`, { method: "POST", headers: { "Content-Type": "text/plain" }, body });
  return { status: r.status, text: await r.text() };
}

// What the page shows, from the DOM only (old and new page alike).
const SHOWS = `(() => {
  const q = (s) => document.querySelector(s);
  const vp = q('[data-bb=viewport]');
  const shown = (el) => !!el && getComputedStyle(el).display !== 'none' && el.getClientRects().length > 0;
  return JSON.stringify({
    home: !!q('[data-bb=home]'), back: shown(q('[data-bb=home-back]')),
    page: q('[data-bb=page]') ? (q('[data-bb=viewtab-page]')?.textContent ?? '?') : null,
    viewport: shown(vp), error: q('[data-bb=shell-error]')?.textContent ?? null,
    errorBack: shown(q('[data-bb=shell-error-back]')), confirm: shown(q('[data-bb=shell-confirm]')),
    opening: shown(q('[data-bb=shell-opening]')), tabs: document.querySelectorAll('[data-bb=viewtabs] .vtab:not([data-bb=viewtab-page]):not([aria-hidden])').length,
    shell: q('.bb-app')?.dataset.bbShell ?? null, fileDialog: shown(q('[data-bb=file-browser]')),
  });
})()`;

async function session(cdp, url, { files = false } = {}) {
  const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank" });
  const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
  const S = (m, p) => cdp.send(m, p, sessionId);
  const exceptions = [];
  let chooser = null;
  cdp.on((msg) => {
    if (msg.sessionId !== sessionId) return;
    if (msg.method === "Runtime.exceptionThrown") { const d = msg.params.exceptionDetails; exceptions.push(d.exception?.description ?? d.text); }
    if (msg.method === "Page.fileChooserOpened") chooser = msg.params;
  });
  await S("Runtime.enable");
  await S("Page.enable");
  await S("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  if (files) await S("Page.setInterceptFileChooserDialog", { enabled: true });
  const evalJs = async (expr) => (await S("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true })).result.value;
  const bb = (expr) => evalJs(`(async () => { const b = await import('/src/bimble.ts'); return ${expr}; })()`);
  const shows = async () => JSON.parse(await evalJs(SHOWS));
  const go = async (u) => {
    await S("Page.navigate", { url: u });
    await waitFor("the page's actions", 20000, () => bb("b.actions.value.length > 0"));
    await sleep(400);
  };
  // A real click at the element's centre; false when it is not there (or has no box).
  const click = async (sel) => {
    const r = JSON.parse(await evalJs(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) return 'null';
      const b = e.getBoundingClientRect(); return b.width && b.height ? JSON.stringify({ x: b.x + b.width / 2, y: b.y + b.height / 2 }) : 'null'; })()`));
    if (!r) return false;
    for (const type of ["mousePressed", "mouseReleased"]) await S("Input.dispatchMouseEvent", { type, x: r.x, y: r.y, button: "left", buttons: type === "mousePressed" ? 1 : 0, clickCount: 1 });
    await sleep(250);
    return true;
  };
  const key = async (k) => {
    for (const type of ["keyDown", "keyUp"]) await S("Input.dispatchKeyEvent", { type, key: k, code: k, windowsVirtualKeyCode: k === "Escape" ? 27 : 0 });
    await sleep(250);
  };
  // Settles: waits until `pred(shows)` or 3 s; answers the last view either way.
  const until = async (pred, ms = 3000) => {
    let v = await shows();
    const end = Date.now() + ms;
    while (!pred(v) && Date.now() < end) { await sleep(100); v = await shows(); }
    return v;
  };
  const takeChooser = () => { const c = chooser; chooser = null; return c; };
  return { S, evalJs, bb, shows, go, click, key, until, exceptions, takeChooser, close: () => cdp.send("Target.closeTarget", { targetId }) };
}

const isView = (v) => !v.home && v.page === null && v.viewport && v.error === null && !v.confirm && !v.opening;
const isHome = (v) => v.home && v.error === null && !v.confirm && !v.opening && !v.fileDialog;
const brief = (v) => JSON.stringify(v);
// Reaches Home the way a user does: the quick bar's logo.
const toHome = async (P) => { await P.click("[data-bb=quickbar-logo]"); return P.until(isHome); };

async function nativePaths(cdp) {
  const P = await session(cdp, NATIVE);
  await P.go(NATIVE);
  let v = await P.until(isHome);
  check("N1 native, nothing open: the page shows Home", isHome(v), brief(v));

  // N2: Home -> New.
  v = await toHome(P);
  await P.click("[data-bb=home-new]");
  v = await P.until(isView);
  check("N2 Home -> New: the model view", isView(v) && (await P.bb("b.state.value.file")) === "Untitled.bimble", brief(v));

  // N3: unsaved changes, then New from Home: the question, Cancel, then Discard.
  const sto = JSON.parse((await host("query", "storeys", "{}")).text);
  const placed = await host("verb", "place", JSON.stringify({ kind: "column", a: [1000, 1000], storey: sto[0]?.id }));
  const count = async () => JSON.parse((await host("query", "elements", "{}")).text).length;
  const before = await count();
  await waitFor("unsaved", 5000, () => P.bb("b.state.value.unsaved"));
  v = await toHome(P);
  await P.click("[data-bb=home-new]");
  v = await P.until((x) => x.confirm);
  const asked = v.confirm;
  await P.click("[data-bb=shell-confirm-cancel]");
  const afterCancel = await P.until(isHome);
  const stillUnsaved = await P.bb("b.state.value.unsaved");
  await P.click("[data-bb=home-new]");
  await P.until((x) => x.confirm);
  await P.click("[data-bb=shell-confirm-discard]");
  v = await P.until(isView);
  const after = await count();
  check("N3 unsaved, Home -> New: asked; Cancel -> Home, kept; Discard -> a new model's view",
    placed.status === 200 && before === 1 && asked && isHome(afterCancel) && stillUnsaved === true && isView(v) && after === 0,
    `place ${placed.status} ${placed.text}; ${before} element(s); asked ${asked}; after cancel ${brief(afterCancel)} unsaved ${stillUnsaved}; after discard ${brief(v)}, ${after} element(s)`);

  // N4 (FS1): Home -> Open on the native host: the page's own file dialog (a new model is unsaved:
  // asked first). Closed: back where it was, the model kept. Again, a bad file picked: the error, Back.
  const broken = join(tmp, "broken.bimble");
  writeFileSync(broken, "this is not a model either\n");
  v = await toHome(P);
  await P.click("[data-bb=home-open]");
  v = await P.until((x) => x.confirm || x.fileDialog || x.error !== null);
  const askedOpen = v.confirm;
  if (askedOpen) await P.click("[data-bb=shell-confirm-discard]");
  const dlg = await P.until((x) => x.fileDialog);
  await P.click("[data-bb=file-browser-close]");
  const closed = await P.until(isHome);
  const kept = await P.bb("b.state.value.file");
  check("N4 native Home -> Open: asked (unsaved), the page's file dialog; closed -> Home, the model kept",
    askedOpen && dlg.fileDialog && !dlg.opening && isHome(closed) && kept === "Untitled.bimble", `${brief(dlg)} then ${brief(closed)} file ${kept}`);
  await P.click("[data-bb=home-open]");
  v = await P.until((x) => x.confirm);
  await P.click("[data-bb=shell-confirm-discard]");
  await P.until((x) => x.fileDialog);
  await waitFor("broken.bimble listed", 5000, () => P.evalJs("!!document.querySelector('[data-bb=file-browser-entry][data-name=\"broken.bimble\"]')"));
  await P.click('[data-bb=file-browser-entry][data-name="broken.bimble"]');
  await P.click("[data-bb=file-browser-go]");
  v = await P.until((x) => x.error !== null);
  const err = v;
  await P.click("[data-bb=shell-error-back]");
  v = await P.until(isHome);
  check("N4 native Home -> Open a bad file in the dialog: the core's answer shown; Back -> Home", err.error !== null && /broken\.bimble|not|bimble/i.test(err.error) && err.errorBack && isHome(v),
    `${brief(err)} then ${brief(v)}`);

  // N5: an agent opens a file while Home shows.
  const model = join(tmp, "frame.json");
  copyFileSync(join(CCORE, "golden/frame.json"), model);
  const op = await host("action", "open", JSON.stringify({ path: model, discard: true }));
  v = await P.until(isView);
  check("N5 an agent opens a file while Home shows: the model view", op.status === 200 && isView(v), `${op.status} ${op.text} ${brief(v)}`);

  // N6: a bad file (the shell's open with a path, as FS1's dialog will send it).
  const bad = join(tmp, "bad.bimble");
  writeFileSync(bad, "this is not a model\n");
  const sent = await P.evalJs(`(async () => { try { const m = await import('/src/shell/send.ts'); m.send({ do: 'open', path: ${JSON.stringify(bad)} }); return 'sent'; } catch (e) { return String(e); } })()`);
  v = await P.until((x) => x.error !== null);
  const badErr = v;
  await P.click("[data-bb=shell-error-back]");
  v = await P.until(isView);
  check("N6 open a bad file: the error shown; Back -> the model view it came from", sent === "sent" && badErr.error !== null && isView(v) && (await P.bb("b.state.value.file")) === "frame.json",
    `${sent}; ${brief(badErr)} then ${brief(v)}`);

  // N7: model -> Home -> back to the model.
  v = await toHome(P);
  const onHome = isHome(v);
  await P.click("[data-bb=home-back]");
  v = await P.until(isView);
  check("N7 model -> Home -> Back to the model: the view", onHome && isView(v), brief(v));

  // N8: a page and the three ways back.
  const toPage = async () => { await P.click("[data-missing=schedule-list-in-navigator]"); return P.until((x) => x.page !== null); };
  const p1 = await toPage();
  await P.click("[data-bb=viewtab-page] .xx");
  const b1 = await P.until(isView);
  const p2 = await toPage();
  await P.key("Escape");
  const b2 = await P.until(isView);
  const p3 = await toPage();
  await toHome(P);
  await P.click("[data-bb=home-back]");
  const b3 = await P.until(isView);
  check("N8 a page (Quantities): its tab's close, Escape, Home then Back -> the view each time",
    p1.page !== null && isView(b1) && p2.page !== null && isView(b2) && p3.page !== null && isView(b3), [p1, b1, p2, b2, p3, b3].map(brief).join(" "));

  // N9: the last view tab closed.
  v = await P.shows();
  const tabsBefore = v.tabs;
  await P.click("[data-bb=viewtabs] .vtab:not([data-bb=viewtab-page]) .xx");
  v = await P.until(isHome);
  const tabNull = await P.bb("b.state.value.tab === null");
  const closedHome = v;
  await P.click("[data-bb=home-back]");
  v = await P.until((x) => isView(x) && x.tabs > 0);
  check("N9 the last tab closed: Home (the core has no tab); Back to the model -> a view tab again",
    tabsBefore === 1 && tabNull && isHome(closedHome) && closedHome.back && isView(v) && v.tabs === 1, `${tabsBefore} tabs, tab null ${tabNull}, ${brief(closedHome)} then ${brief(v)}`);

  // N10: an agent closes the model while a page shows.
  await toPage();
  const cl = await host("action", "close", JSON.stringify({ discard: true }));
  v = await P.until(isHome);
  check("N10 an agent closes the model while a page shows: Home, no way back to a model", cl.status === 200 && isHome(v) && !v.back, `${cl.status} ${brief(v)}`);

  // N11: reload with a live session.
  await host("action", "open", JSON.stringify({ path: model, discard: true }));
  await P.go(NATIVE);
  const r1 = await P.until(isView);
  await host("action", "close", JSON.stringify({ discard: true }));
  await P.go(NATIVE);
  const r2 = await P.until(isHome);
  check("N11 reload with a model open: the view; with nothing open: Home", isView(r1) && isHome(r2), `${brief(r1)} ${brief(r2)}`);

  // N12: Home's "change" link (Settings) is not a link to nowhere.
  const settingsOn = await P.bb("b.actions.value.some((a) => a.name === 'settings' && a.enabled)");
  const ch = await P.evalJs("(() => { const e = document.querySelector('[data-bb=home-settings]'); return e ? JSON.stringify({ dis: e.getAttribute('aria-disabled') === 'true' || e.disabled === true }) : 'null'; })()");
  const chv = JSON.parse(ch);
  // FAKE1 (requirement change): while the core does not serve "user" (query capabilities), the whole "You" card,
  // the change link with it, is the NotAvailable state; once it does, the link is disabled while Settings is off.
  const userCap = await P.bb("b.capabilities.value === null || b.capabilities.value.user === true");
  const youNa = await P.evalJs("!!document.querySelector('[data-bb=home] .you[data-bb=not-available]')");
  check("N12 Home's change link: disabled while Settings is not available (no \"You\" card: NotAvailable, while the core has no user)",
    userCap ? chv !== null && chv.dis === !settingsOn : chv === null && youNa, `${ch} settings ${settingsOn} user ${userCap} na ${youNa}`);
  check("native: no uncaught page error", P.exceptions.length === 0, P.exceptions.join(" | "));
  await P.close();
}

async function wasmPaths(cdp) {
  const P = await session(cdp, WASM, { files: true });
  await P.go(WASM);
  let v = await P.until(isView);
  check("W1 wasm boot: the model view", isView(v), brief(v));

  // W2: the picker cancelled; then a picker that never answers.
  v = await toHome(P);
  await P.click("[data-bb=home-open]");
  const ch1 = await waitFor("the file picker", 5000, () => P.takeChooser()).catch(() => null);
  await P.evalJs("(() => { for (const i of document.querySelectorAll('input[type=file]')) i.dispatchEvent(new Event('cancel')); return 1; })()");
  const c1 = await P.until(isHome);
  await P.click("[data-bb=home-open]");
  await waitFor("the file picker", 5000, () => P.takeChooser()).catch(() => null);
  const waiting = await P.shows();
  await P.click("[data-bb=shell-opening-cancel]");
  const c2 = await P.until(isHome);
  check("W2 wasm Home -> Open, picker cancelled: Home; a picker that never answers: Cancel -> Home",
    !!ch1 && isHome(c1) && waiting.opening && isHome(c2), `${brief(c1)} waiting ${brief(waiting)} ${brief(c2)}`);

  // W3: a bad file picked.
  const bad = join(tmp, "bad.bimble");
  writeFileSync(bad, "this is not a model\n");
  await P.click("[data-bb=home-open]");
  const ch3 = await waitFor("the file picker", 5000, () => P.takeChooser()).catch(() => null);
  if (ch3) await P.S("DOM.setFileInputFiles", { files: [bad], backendNodeId: ch3.backendNodeId });
  v = await P.until((x) => x.error !== null);
  const e3 = v;
  await P.click("[data-bb=shell-error-back]");
  v = await P.until(isHome);
  check("W3 wasm Open, a bad file: the error shown; Back -> Home", !!ch3 && e3.error !== null && isHome(v), `${brief(e3)} then ${brief(v)}`);

  // W4: a good file picked.
  const good = join(tmp, "picked.json");
  copyFileSync(join(CCORE, "golden/frame.json"), good);
  await P.click("[data-bb=home-open]");
  const ch4 = await waitFor("the file picker", 5000, () => P.takeChooser()).catch(() => null);
  if (ch4) await P.S("DOM.setFileInputFiles", { files: [good], backendNodeId: ch4.backendNodeId });
  v = await P.until(isView, 5000);
  check("W4 wasm Home -> Open, a model picked: its view", !!ch4 && isView(v) && (await P.bb("b.state.value.file")) === "picked.json", brief(v));
  check("wasm: no uncaught page error", P.exceptions.length === 0, P.exceptions.join(" | "));
  await P.close();

  // W5: the viewer-only build.
  const V = await session(cdp, VIEWONLY);
  await V.go(VIEWONLY);
  const v0 = await V.until(isView);
  await toHome(V);
  await V.click("[data-bb=home-new]");
  const e5 = await V.until((x) => x.error !== null);
  await V.click("[data-bb=shell-error-back]");
  const h5 = await V.until(isHome);
  await V.click("[data-bb=home-back]");
  const b5 = await V.until(isView);
  check("W5 view-only: boot -> view; Home -> New refused: shown; Back -> Home -> Back to the model -> view",
    isView(v0) && e5.error !== null && /view-only/.test(e5.error) && isHome(h5) && isView(b5), [v0, e5, h5, b5].map(brief).join(" "));
  check("view-only: no uncaught page error", V.exceptions.length === 0, V.exceptions.join(" | "));
  await V.close();
}

async function main() {
  if (!existsSync(join(UI, "node_modules"))) throw new Error(`no ${UI}/node_modules: run npm ci in web-ui/`);
  execFileSync("node", [join(UI, "scripts/copy-ccore.mjs")], { stdio: "inherit" });
  execFileSync("node", [join(UI, "scripts/copy-ccore.mjs"), "--view"], { stdio: "inherit" });
  const h = start("host", BIN, [String(PORT)], {
    // FS1: HOME is the test's folder (the file dialog lists it), never the user's.
    cwd: tmp, env: { ...process.env, HOME: tmp, BIMBLE_CONFIG_DIR: join(tmp, "config"), BIMBLE_TOKEN: TOKEN, BIMBLE_ORIGINS: `http://localhost:${VITE},http://127.0.0.1:${VITE}` },
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
  for (const [name, fn] of [["native", nativePaths], ["wasm", wasmPaths]]) {
    try { await fn(cdp); } catch (e) { check(`${name} paths ran to the end`, false, e.stack ?? String(e)); }
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
console.log("\npaths (FAIL = a dead end):");
for (const [name, ok] of table) console.log(`  ${ok ? "ok  " : "DEAD"} ${name.replace(/:.*/, "")}`);
console.log(`dead ends: ${table.filter((r) => !r[1]).length}`);
console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
