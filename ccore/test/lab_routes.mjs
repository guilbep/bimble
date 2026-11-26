// Nothing fake in the preview (ticket FAKE1): node ccore/test/lab_routes.mjs
// Visits every route of the lab the index files export (web-ui/src/pages, dialogs, panels: their
// registries, read in the page), every ribbon tab, the File menu and the quick bar, and asks of
// each what the user sees:
//   - a page, dialog or panel shows data (text and no empty table) or NotAvailable
//     (`data-bb="not-available"`), never an empty table or an empty panel;
//   - every visible, enabled control maps to an action the core enables now: the ribbon tools
//     (shell/ribbon-tabs.ts RIBBON), the File menu entries, the quick bar, the shell's CONTROLS
//     (shell/machine.ts) that run an action; a tool the core cannot run is drawn "not yet"
//     (`data-not-yet`) with a tooltip, and a click on it changes nothing and shows the tooltip;
//   - a ribbon tab whose every tool is "not yet" carries the "preview" mark;
//   - a file open read only: the header says "Read only" (not "Saved"), no Save is enabled;
//   - no console error, no uncaught exception, no 4xx/5xx answer during the walk (GET /mesh with no
//     model too: no request at all, TEST1); closing the model asks no /mesh and draws nothing.
// Runs on: the native host (/?native=1) with no model, an editable model (golden/frame.json) and a
// read-only IFC (golden/ifc/frame.ifc); the C core's wasm (/?ccore=1, the model it boots with)
// and the viewer-only wasm (&ccore_wasm=ccore/ccore-view.wasm). Needs web-ui/node_modules,
// out/tcc/bimble-ccore (or $HOST_BIN), the wasm builds (build.sh wasm, wasm-view) and Chromium.
// Prints the route table (route, state per walk), then "N passed, M failed"; exits non-zero on a failure.
import { spawn, spawnSync, execFileSync } from "node:child_process";
import { copyFileSync, existsSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { CHROMIUM, port, vitePort } from "./ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = resolve(CCORE, "../web-ui");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("lab_routes");
const VITE = vitePort("lab_routes");
const TOKEN = "labroutes-token";
const NATIVE = `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}`;
const WASM = `http://localhost:${VITE}/?ccore=1&gl=1`;
const VIEWONLY = `${WASM}&ccore_wasm=ccore/ccore-view.wasm`;
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-routes-"));
const procs = [];
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail && !ok ? `: ${detail}` : ""}`);
};
const routes = new Map(); // route -> {walk: state}

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

async function host(op, name, body = "") {
  const r = await fetch(`http://127.0.0.1:${PORT}/${op}/${name}?t=${TOKEN}`, { method: "POST", headers: { "Content-Type": "text/plain" }, body });
  return { status: r.status, text: await r.text() };
}

/** A browser tab on `url` that records console errors, exceptions and 4xx/5xx answers. */
async function session(cdp) {
  const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank" });
  const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
  const S = (m, p) => cdp.send(m, p, sessionId);
  const errors = [];
  const meshReqs = []; // TEST1: every GET /mesh the page sends (its URL)
  cdp.on((msg) => {
    if (msg.sessionId !== sessionId) return;
    const p = msg.params;
    if (msg.method === "Network.requestWillBeSent" && /^\/mesh$/.test(new URL(p.request.url).pathname)) meshReqs.push(p.request.url);
    if (msg.method === "Runtime.exceptionThrown") errors.push(`exception: ${p.exceptionDetails.exception?.description ?? p.exceptionDetails.text}`);
    if (msg.method === "Runtime.consoleAPICalled" && p.type === "error") errors.push(`console.error: ${p.args.map((a) => a.value ?? a.description).join(" ")}`);
    // The browser's own favicon request (the lab has none) is not the page's.
    const fav = (u) => /\/favicon\.ico$/.test(u ?? "");
    if (msg.method === "Network.responseReceived" && p.response.status >= 400 && !fav(p.response.url)) errors.push(`HTTP ${p.response.status} ${p.response.url}`);
    if (msg.method === "Log.entryAdded" && p.entry.level === "error" && !fav(p.entry.url)) errors.push(`log: ${p.entry.text} ${p.entry.url ?? ""}`);
  });
  await S("Runtime.enable");
  await S("Page.enable");
  await S("Log.enable");
  await S("Network.enable");
  await S("Emulation.setDeviceMetricsOverride", { width: 1600, height: 1000, deviceScaleFactor: 1, mobile: false });
  const evalJs = async (expr) => {
    const r = await S("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
    return r.result.value;
  };
  // The page's own modules (Vite serves them by path): the registries, the shell, the client.
  const mods = `const b = await import('/src/bimble.ts'), sh = await import('/src/shell/send.ts'), m = await import('/src/shell/machine.ts'),
    pg = await import('/src/pages/index.ts'), dl = await import('/src/dialogs/index.ts'), pn = await import('/src/panels/index.ts'),
    tb = await import('/src/shell/tabs.ts'), rb = await import('/src/shell/ribbon-tabs.ts'), cap = await import('/src/shell/capability.ts');`;
  const js = (body) => evalJs(`(async () => { ${mods} ${body} })()`);
  const go = async (u) => {
    await S("Page.navigate", { url: u });
    await waitFor("the page's actions", 30000, () => js("return b.actions.value.length > 0"));
    await sleep(500);
  };
  const click = async (sel) => {
    const r = JSON.parse(await evalJs(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) return 'null';
      const b = e.getBoundingClientRect(); return b.width && b.height ? JSON.stringify({ x: b.x + b.width / 2, y: b.y + b.height / 2 }) : 'null'; })()`));
    if (!r) return false;
    for (const type of ["mousePressed", "mouseReleased"]) await S("Input.dispatchMouseEvent", { type, x: r.x, y: r.y, button: "left", buttons: type === "mousePressed" ? 1 : 0, clickCount: 1 });
    await sleep(150);
    return true;
  };
  return { S, evalJs, js, go, click, errors, meshReqs, seen: 0, close: () => cdp.send("Target.closeTarget", { targetId }) };
}

// What a screen shows, from the DOM: NotAvailable, or data (text, no empty table), or empty.
const SCREEN = (sel) => `(() => {
  const el = document.querySelector(${JSON.stringify(sel)});
  if (!el) return JSON.stringify({ state: 'absent' });
  // The whole screen is NotAvailable (its root, or the one child of a dialog frame); else parts of it may be.
  const na = el.querySelector('[data-bb=not-available]');
  if (na && (na.parentElement === el || (na.parentElement.parentElement === el && na.parentElement.children.length === 1))) return JSON.stringify({ state: 'not-available' });
  const empty = [...el.querySelectorAll('table')].filter((t) => !t.querySelector('tbody tr, tr td')).length;
  const text = (el.innerText ?? '').replace(/\\s+/g, ' ').trim();
  return JSON.stringify({ state: empty ? 'empty-table' : text.length < 8 ? 'empty' : na ? 'data, parts not-available' : 'data', text: text.slice(0, 80), empty });
})()`;

/** One walk: every page, dialog, panel, ribbon tab; the File menu, the quick bar, the shell controls. */
async function walk(P, label, { model }) {
  const set = (route, st) => { if (!routes.has(route)) routes.set(route, {}); routes.get(route)[label] = st; };
  const before = P.seen; // since the last walk's check: the file's opening counts too
  const ids = JSON.parse(await P.js("return JSON.stringify({ pages: Object.keys(pg.pages), dialogs: Object.keys(dl.dialogs), panels: Object.keys(pn.panels), tabs: tb.TABS.map((t) => t.id) })"));
  // Pages: through the shell (a model is needed for any page but Home).
  for (const id of ids.pages) {
    if (id === "home") {
      await P.js("sh.send({ do: 'home' }); return 1");
    } else {
      if (!model) { set(`page ${id}`, "no model: not reachable"); continue; }
      const ok = await P.js(`return sh.send({ do: 'showPage', page: ${JSON.stringify(id)} })`);
      if (!ok) { set(`page ${id}`, "refused"); check(`${label}: page ${id} opens`, false); continue; }
    }
    await sleep(350);
    const s = JSON.parse(await P.evalJs(SCREEN(id === "home" ? "[data-bb=home]" : "[data-bb=page]")));
    set(`page ${id}`, s.state);
    check(`${label}: page ${id} shows data or NotAvailable`, s.state.startsWith("data") || s.state === "not-available", JSON.stringify(s));
    // The left panel beside a page: never empty.
    if (id !== "home") {
      const side = JSON.parse(await P.evalJs(SCREEN("[data-bb=properties]")));
      check(`${label}: page ${id}: the left panel is not empty`, side.state !== "empty" && side.state !== "empty-table", JSON.stringify(side));
    }
    if (id === "home") {
      // Home's user line and recent files: data where served, else NotAvailable.
      const h = JSON.parse(await P.js(`const c = b.capabilities.value; return JSON.stringify({ user: c === null || c.user === true, recent: c === null || c.recent_files === true,
        youNa: !!document.querySelector('[data-bb=home] .you[data-bb=not-available], [data-bb=home] [data-bb=not-available].you'),
        recentNa: [...document.querySelectorAll('[data-bb=home] .right [data-bb=not-available]')].length > 0,
        you: !!document.querySelector('[data-bb=home-you]'), grid: !!document.querySelector('[data-bb=home-recent]') })`));
      check(`${label}: Home's user line: NotAvailable unless the core serves "user"`, h.user ? h.you : h.youNa && !h.you, JSON.stringify(h));
      check(`${label}: Home's recent files: NotAvailable unless the core serves "recent_files"`, h.recent ? h.grid : h.recentNa && !h.grid, JSON.stringify(h));
    }
  }
  if (model) await P.js("sh.send({ do: 'showView' }); return 1");
  await sleep(300);
  // Dialogs and panels: opened directly (as their buttons do).
  for (const id of ids.dialogs) {
    await P.js(`dl.currentDialog.value = ${JSON.stringify(id)}; return 1`);
    await sleep(300);
    const s = JSON.parse(await P.evalJs(SCREEN(".bb-backdrop")));
    set(`dialog ${id}`, s.state);
    check(`${label}: dialog ${id} shows data or NotAvailable`, s.state.startsWith("data") || s.state === "not-available", JSON.stringify(s));
    if (id === "file-menu") {
      // Enabled entries run an enabled action (or open a dialog); "not yet" entries say so.
      const fm = JSON.parse(await P.js(`const en = new Set(b.actions.value.filter((a) => a.enabled).map((a) => a.name));
        return JSON.stringify([...document.querySelectorAll('[data-bb=file-menu] button[data-bb^=file-menu-]')].map((e) => ({ id: e.dataset.bb.slice(10),
          enabled: !e.disabled, ny: e.hasAttribute('data-not-yet'), title: e.title, core: en.has(e.dataset.bb.slice(10)) })))`));
      // save_as (FS1): a hidden action of the core (calls.c HIDDEN_ACTIONS, not in `query actions`),
      // enabled by the page with an editable model: checked as that.
      const editableNow = await P.js("return b.state.value.file !== null && b.state.value.editable !== false;");
      // close (FIXUI): hidden too (calls.c HIDDEN_ACTIONS), enabled by the core whenever a file is loaded.
      const fileNow = await P.js("return b.state.value.file !== null;");
      const bad = fm.filter((e) => e.enabled && !e.core && !["export"].includes(e.id) && !(e.id === "save_as" && editableNow) && !(e.id === "close" && fileNow));
      check(`${label}: File menu: every enabled entry's action is enabled in the core`, bad.length === 0, JSON.stringify(bad));
      const nyBad = fm.filter((e) => e.ny && (e.enabled || !/./.test(e.title)));
      check(`${label}: File menu: "not yet" entries are greyed with a tooltip`, nyBad.length === 0, JSON.stringify(nyBad));
      const want = JSON.parse(await P.js("return JSON.stringify(['settings', 'about', 'on_paper'].filter((a) => cap.notYetAction(a)))"));
      check(`${label}: File menu: ${want.join(", ") || "none"} marked not yet`, want.every((a) => fm.find((e) => e.id === a)?.ny), JSON.stringify(fm.filter((e) => e.ny).map((e) => e.id)));
    }
    await P.js("dl.currentDialog.value = null; return 1");
  }
  for (const id of ids.panels) {
    await P.js(`pn.currentPanel.value = ${JSON.stringify(id)}; return 1`);
    await sleep(300);
    const s = JSON.parse(await P.evalJs(SCREEN("[data-bb=right-panel]")));
    set(`panel ${id}`, model ? s.state : `${s.state} (no model)`);
    if (model) check(`${label}: panel ${id} shows data or NotAvailable`, s.state.startsWith("data") || s.state === "not-available", JSON.stringify(s));
    await P.js("pn.currentPanel.value = null; return 1");
  }
  if (!model) {
    // TEST1 (BUG2): GET /mesh with no model counts like any other answer (it was exempt: the WebGL
    // viewport asked every second and the host answered 404). The page asks nothing with no model.
    const errs = P.errors.slice(before);
    check(`${label}: no console error, no 4xx/5xx since the page opened, nor during the walk`, errs.length === 0, errs.join(" | "));
    check(`${label}: no GET /mesh with no model open`, P.meshReqs.length === 0, P.meshReqs.join(" | "));
    P.seen = P.errors.length;
    return;
  }
  const seen = [...ids.pages.map((i) => `page ${i}`), ...ids.dialogs.map((i) => `dialog ${i}`), ...ids.panels.map((i) => `panel ${i}`)].filter((r) => routes.get(r)?.[label] !== undefined);
  check(`${label}: routes visited = routes the index files export (${ids.pages.length} pages, ${ids.dialogs.length} dialogs, ${ids.panels.length} panels)`,
    seen.length === ids.pages.length + ids.dialogs.length + ids.panels.length, `${seen.length}`);
  // The ribbon, tab by tab.
  for (const tab of ids.tabs) {
    await P.js(`tb.activeTab.value = ${JSON.stringify(tab)}; return 1`);
    await sleep(250);
    const r = JSON.parse(await P.js(`const en = new Set(b.actions.value.filter((a) => a.enabled).map((a) => a.name));
      const out = [];
      rb.RIBBON[${JSON.stringify(tab)}].forEach((g, gi) => g.tools.forEach((tl, ti) => {
        const e = document.querySelector('[data-bb=ribbon-tool-' + (gi + 1) + '-' + (ti + 1) + ']');
        out.push({ key: tl.key, action: tl.action, screen: tl.page ?? tl.panel ?? tl.dialog ?? null, shown: !!e,
          ny: !!e && e.hasAttribute('data-not-yet'), off: !!e && e.getAttribute('aria-disabled') === 'true', title: e?.title ?? '',
          want: cap.toolNotYet(tl), core: tl.action ? en.has(tl.action) : null, bb: 'ribbon-tool-' + (gi + 1) + '-' + (ti + 1) });
      }));
      const mark = !!document.querySelector('[data-bb=tab-preview-${tab}]');
      return JSON.stringify({ tools: out, mark, boxes: [...document.querySelectorAll('.ribbon .rbox')].map((x) => ({ ny: x.hasAttribute('data-not-yet'), title: x.title })) })`));
    const live = r.tools.filter((x) => x.shown && !x.ny && !x.off);
    const bad = live.filter((x) => !x.screen && !x.core);
    check(`${label}: ribbon ${tab}: every enabled tool runs an action the core enables`, bad.length === 0, JSON.stringify(bad));
    const fake = r.tools.filter((x) => x.shown && !x.ny && !x.screen && x.action === null);
    check(`${label}: ribbon ${tab}: no tool without action or screen looks normal`, fake.length === 0, JSON.stringify(fake.map((x) => x.key)));
    const nyWrong = r.tools.filter((x) => x.shown && x.ny !== x.want);
    check(`${label}: ribbon ${tab}: "not yet" drawn as capability.ts says`, nyWrong.length === 0, JSON.stringify(nyWrong));
    check(`${label}: ribbon ${tab}: "not yet" tools have a tooltip`, r.tools.filter((x) => x.ny && !x.title).length === 0);
    check(`${label}: ribbon ${tab}: "not yet" boxes have a tooltip`, r.boxes.every((x) => x.ny && x.title));
    const allNy = r.tools.every((x) => x.ny || !x.shown);
    check(`${label}: ribbon ${tab}: the "preview" mark ${allNy ? "shown" : "absent"} (every tool not yet: ${allNy})`, r.mark === allNy);
    set(`ribbon ${tab}`, `${r.tools.length - r.tools.filter((x) => x.ny).length} real, ${r.tools.filter((x) => x.ny).length} not yet${r.mark ? ", preview" : ""}`);
    // Each "not yet" tool: a click changes nothing and shows its tooltip.
    for (const x of r.tools.filter((t) => t.ny)) {
      const s0 = await P.js("return JSON.stringify([sh.here.value, dl.currentDialog.value, pn.currentPanel.value, b.state.value.rev])");
      await P.click(`[data-bb=${x.bb}]`);
      const tip = await P.evalJs(`!!document.querySelector('[data-bb=${x.bb}] [data-bb=not-yet-tip]')`);
      const s1 = await P.js("return JSON.stringify([sh.here.value, dl.currentDialog.value, pn.currentPanel.value, b.state.value.rev])");
      check(`${label}: ribbon ${tab} ${x.key}: "not yet" click: nothing changes, the tooltip shows`, tip && s0 === s1, `${tip} ${s0} -> ${s1}`);
    }
  }
  await P.js("tb.activeTab.value = 'view'; return 1");
  // The quick bar and the shell's controls: enabled means the core enables the action.
  const q = JSON.parse(await P.js(`const en = new Set(b.actions.value.filter((a) => a.enabled).map((a) => a.name));
    const ACT = { new: 'new', open: 'open', import: 'import_ifc' }, PAGE = { 'project-info': 'project', history: 'history' };
    const core = m.coreOf ? null : null;
    const c = { file: b.state.value.file, unsaved: b.state.value.unsaved, tab: b.state.value.tab !== null };
    const ctl = m.CONTROLS.filter((x) => x.shown(sh.here.value, c)).map((x) => {
      const e = document.querySelector('[data-bb=' + x.id + ']');
      const act = x.event.do === 'showPage' ? PAGE[x.event.page] : ACT[x.event.do];
      return { id: x.id, act: act ?? null, enabled: !!e && !e.disabled && e.getAttribute('aria-disabled') !== 'true', core: act ? en.has(act) : null };
    });
    const qb = ['save', 'open', 'undo', 'redo', 'sync', 'primary'].map((k) => { const e = document.querySelector('[data-bb=quickbar-' + k + ']');
      const act = { sync: 'synchronize', primary: 'save' }[k] ?? k; return { id: 'quickbar-' + k, act, enabled: !!e && !e.disabled, core: en.has(act) }; });
    const chip = document.querySelector('[data-bb=state-chip]');
    return JSON.stringify({ ctl, qb, chip: chip?.textContent ?? null, ro: !!chip?.hasAttribute('data-read-only'), editable: b.state.value.editable })`));
  const badCtl = [...q.ctl, ...q.qb].filter((x) => x.enabled && x.act && !x.core);
  check(`${label}: quick bar and shell controls: enabled only where the core enables the action`, badCtl.length === 0, JSON.stringify(badCtl));
  if (q.editable === false) {
    check(`${label}: read only: the header says read only, not "Saved", and no Save is enabled`,
      q.ro && !q.qb.some((x) => x.act === "save" && x.enabled), JSON.stringify(q));
  }
  check(`${label}: no console error, no 4xx/5xx since the file opened, nor during the walk`, P.errors.length === before, P.errors.slice(before).join(" | "));
  P.seen = P.errors.length;
}

async function main() {
  if (!existsSync(join(UI, "node_modules"))) throw new Error(`no ${UI}/node_modules: run npm ci in web-ui/`);
  execFileSync("node", [join(UI, "scripts/copy-ccore.mjs")], { stdio: "inherit" });
  execFileSync("node", [join(UI, "scripts/copy-ccore.mjs"), "--view"], { stdio: "inherit" });
  copyFileSync(join(CCORE, "golden/frame.json"), join(tmp, "frame.json"));
  copyFileSync(join(CCORE, "golden/ifc/frame.ifc"), join(tmp, "frame.ifc"));
  const h = start("host", BIN, [String(PORT)], {
    cwd: tmp, env: { ...process.env, BIMBLE_TOKEN: TOKEN, BIMBLE_ORIGINS: `http://localhost:${VITE},http://127.0.0.1:${VITE}` },
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
  const run = async (label, fn) => { try { await fn(); } catch (e) { check(`${label}: the walk ran to the end`, false, e.stack ?? String(e)); } };

  // Native host: no model, an editable model, a read-only IFC.
  await run("native", async () => {
    const P = await session(cdp);
    await P.go(NATIVE);
    const caps = JSON.parse(await P.js("return JSON.stringify(b.capabilities.value)"));
    check("native: the page read the core's capabilities", caps !== null && caps.quantities === true && caps.lca === false, JSON.stringify(caps));
    await walk(P, "native, no model", { model: false });
    const o1 = await host("action", "open", JSON.stringify({ path: join(tmp, "frame.json"), discard: true }));
    check("native: open frame.json", o1.status === 200, o1.text);
    await waitFor("the model in the page", 10000, () => P.js("return b.state.value.file === 'frame.json'"));
    await P.js("sh.send({ do: 'showView' }); return 1");
    await sleep(500);
    await walk(P, "native, editable", { model: true });
    const o2 = await host("action", "open", JSON.stringify({ path: join(tmp, "frame.ifc"), discard: true }));
    check("native: open frame.ifc read only", o2.status === 200, o2.text);
    await waitFor("the IFC in the page", 10000, () => P.js("return b.state.value.file === 'frame.ifc' && b.state.value.editable === false"));
    await P.js("sh.send({ do: 'showView' }); return 1");
    await sleep(500);
    await walk(P, "native, read-only IFC", { model: true });
    // FIXUI 1: the Navigator's Objects of the read-only IFC (it said 0), then an element clicked in
    // its tree: the Properties panel shows its name, IFC class and GlobalId (it showed only "Read-only
    // file", and each click answered 400 /query/element). Fails on the old code: Objects 0, no tree.
    {
      const e0 = P.errors.length;
      await P.js("sh.send({ do: 'showView' }); return 1");
      await sleep(300);
      const objN = await P.evalJs("document.querySelector('[data-bb=nav-objects] .n')?.textContent?.trim() ?? null");
      check("native, read-only IFC: the Navigator's Objects count is the scene's 72", objN === "72", String(objN));
      await P.click("[data-bb=nav-objects]");
      await sleep(200);
      await P.click("[data-bb=nav-storey]");
      await sleep(200);
      await P.click("[data-bb=nav-class]");
      await sleep(200);
      const want = JSON.parse(await P.evalJs(`JSON.stringify((() => { const e = document.querySelector('[data-bb=nav-element]');
        return e ? { id: e.dataset.id, name: e.textContent.trim() } : null; })())`));
      const clicked = want ? await P.click("[data-bb=nav-element]") : false;
      await sleep(500);
      const got = JSON.parse(await P.evalJs(`JSON.stringify((() => { const p = document.querySelector('[data-bb=properties]');
        if (!p) return null;
        return { text: p.innerText.replace(/\\s+/g, ' '), name: p.querySelector('#el-name')?.value ?? null,
          sel: null }; })())`));
      const sel = await P.js("return b.state.value.selection.join(',')");
      check("native, read-only IFC: a click in the Objects tree selects the element", clicked && want && sel === want.id, `${JSON.stringify(want)} selection ${sel}`);
      const cls = await P.js(`return b.query('scene_element', { id: ${JSON.stringify(want?.id ?? "")} }).class`).catch(() => null);
      check("native, read-only IFC: Properties show the element's name, IFC class and GlobalId",
        !!got && !!want && got.name === want.name && want.name.length > 0 && /^Ifc[A-Za-z]+$/.test(cls ?? "") && got.text.includes(cls) && got.text.includes(want.id),
        `${JSON.stringify(got)} want ${JSON.stringify(want)} class ${cls}`);
      check("native, read-only IFC: Properties show storey, material, family, volume (Rust's rows)",
        !!got && /Volume [0-9]+\.[0-9]{3} m³/.test(got.text) && /Family|Famille/.test(got.text), JSON.stringify(got));
      check("native, read-only IFC: no 4xx/5xx nor console error during the selection", P.errors.length === e0, P.errors.slice(e0).join(" | "));
      await P.js("b.select([]); return 1");
      P.seen = P.errors.length;
    }
    // TEST1 (BUG2): close the model: the page drops the picture, asks no /mesh, sees no 4xx. 2.5 s
    // is more than two turns of the old error path (a 404, then a retry a second later).
    const e0 = P.errors.length, m0 = P.meshReqs.length;
    const cl = await host("action", "close");
    check("native: close", cl.status === 200, cl.text);
    await waitFor("no model in the page", 10000, () => P.js("return b.state.value.file === null"));
    await sleep(2500);
    const tris = await P.evalJs("window.__bbGl?.drawnTris ?? -1");
    check("native, closed: no GET /mesh after the close, no 4xx/5xx, nothing drawn",
      P.meshReqs.length === m0 && P.errors.length === e0 && tris === 0,
      `mesh ${P.meshReqs.slice(m0).join(" | ")}; errors ${P.errors.slice(e0).join(" | ")}; triangles drawn ${tris}`);
    P.seen = P.errors.length;
    await P.close();
  });
  // The C core's wasm and the viewer-only wasm: the model each boots with.
  for (const [label, url] of [["wasm", WASM], ["view-only", VIEWONLY]]) {
    await run(label, async () => {
      const P = await session(cdp);
      await P.go(url);
      await waitFor("a model", 20000, () => P.js("return b.state.value.file !== null"));
      await P.js("sh.send({ do: 'showView' }); return 1");
      await sleep(500);
      const caps = JSON.parse(await P.js("return JSON.stringify(b.capabilities.value)"));
      check(`${label}: the page read the core's capabilities`, caps !== null && (label === "view-only" ? caps.quantities === false : caps.quantities === true), JSON.stringify(caps));
      await walk(P, label, { model: true });
      await P.close();
    });
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
const walks = [...new Set([...routes.values()].flatMap((r) => Object.keys(r)))];
console.log(`\nroutes visited: ${routes.size}`);
console.log(`| route | ${walks.join(" | ")} |`);
console.log(`|---|${walks.map(() => "---").join("|")}|`);
for (const [r, st] of routes) console.log(`| ${r} | ${walks.map((w) => st[w] ?? "").join(" | ")} |`);
console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
