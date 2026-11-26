// A view On paper, Export view and one PDF of several sheets in the lab (ticket PAPER1; ccore/session/sheet_svg.h
// `view_svg`, `sheets_pdf`; web-ui/src/shell/PaperView.vue, dialogs/Export.vue): node ccore/test/lab_paper.mjs.
// Starts out/tcc/bimble-ccore (or $HOST_BIN) with a copy of golden/drawing/model.json (its views and sheets S-101,
// S-102), copies both wasm builds into web-ui/public/ccore/ (scripts/copy-ccore.mjs, with and without --view; needs
// out/wasm/ccore.wasm and ccore-view.wasm) with the same model as public/ccore/paper1-model.json, starts Vite from
// web-ui/ and headless Chromium over raw CDP, and on three pages:
//   native  /?native=1&gl=1&port=…&token=…
//   wasm    /?ccore=1&gl=1&model=ccore/paper1-model.json
//   view    the same on the viewer-only wasm (&ccore_wasm=ccore/ccore-view.wasm)
// native and wasm: opens the view "Plan Rez-de-chaussée" (as the navigator does), clicks On paper in the view tabs:
// the paper picture shows (its SVG, decoded from the <image>, has the view's title), the model canvas is hidden,
// open_views says paper.on and the action is pressed; Model: the picture goes, the canvas shows again. Then File →
// Export view opens the Export dialog on "This view", PDF: the download is "plan-Plan Rez-de-chaussée-1-100.pdf"
// (Rust's name, lib.rs plan_panel), %PDF- … %%EOF, and pdftotext finds the view's title; SVG: the .svg download
// with the same title. Print and export with S-101 and S-102 checked and "One PDF": one download "S-101-S-102.pdf",
// pdftotext finds both numbers and pdfinfo (poppler) says 2 pages.
// view: no Model / On paper switch (capability paper_view false), File → Export view is greyed "not yet", and the
// core refuses view_svg and sheets_pdf ("this build is view-only").
// No uncaught page error; no console error or warning but the sync-XHR deprecation and the favicon 404.
// Prints "N passed, M failed"; exits non-zero on a failure. Kills all it starts. Fails before PAPER1: the switch
// was hidden (capability false), view_svg unknown, Export view greyed, "One PDF" disabled.
import { execFileSync, spawn } from "node:child_process";
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { CHROMIUM, port, vitePort } from "./ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = resolve(CCORE, "../web-ui");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("lab_paper", 47309);
const VITE = vitePort("lab_paper", 5130);
const TOKEN = "labpaper-token";
const MODEL_PUBLIC = join(UI, "public/ccore/paper1-model.json");
const WASM = `http://localhost:${VITE}/?ccore=1&gl=1&model=ccore/paper1-model.json`;
const PAGES = {
  native: `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}`,
  wasm: WASM,
  view: `${WASM}&ccore_wasm=ccore/ccore-view.wasm`,
};
const VIEW = "Plan Rez-de-chaussée";
const TITLE = "Plan Rez-de-chaussée, 1:100"; // the drawing's title (golden/drawing/view/01.svg)
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-paper-"));
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

function pdftotext(file) {
  try {
    return execFileSync("pdftotext", [file, "-"], { encoding: "utf8" });
  } catch (e) {
    return e.code === "ENOENT" ? null : `pdftotext failed: ${e.message}`;
  }
}


function pdfPages(file) {
  try {
    return Number(/Pages:\s+(\d+)/.exec(execFileSync("pdfinfo", [file], { encoding: "utf8" }))?.[1] ?? NaN);
  } catch (e) {
    return e.code === "ENOENT" ? null : NaN;
  }
}

async function onPage(cdp, mode) {
  const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank" });
  const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
  const S = (m, p) => cdp.send(m, p, sessionId);
  const consoleLog = [], exceptions = [];
  const downloads = new Map();
  cdp.on((msg) => {
    if (msg.method === "Browser.downloadWillBegin") downloads.set(msg.params.guid, { name: msg.params.suggestedFilename, state: "started" });
    else if (msg.method === "Browser.downloadProgress" && downloads.has(msg.params.guid)) downloads.get(msg.params.guid).state = msg.params.state;
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
  await S("Page.navigate", { url: PAGES[mode] });
  const evalJs = async (expr) => {
    const r = await S("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
    return r.result.value;
  };
  const B = "const b = await import('/src/bimble.ts');";
  await waitFor(`${mode}: the model with its views`, 30000, () =>
    evalJs(`(async () => { ${B} return b.state.value.file !== null && b.openViews.value.views.some((v) => v.name === ${JSON.stringify(VIEW)}); })()`));
  const viewId = await evalJs(`(async () => { ${B} return b.openViews.value.views.find((v) => v.name === ${JSON.stringify(VIEW)}).id; })()`);
  // As the navigator does (Navigator.vue: openView, then the shell shows the view).
  await evalJs(`(async () => { ${B} return b.openView(${JSON.stringify(viewId)}); })()`);
  await waitFor(`${mode}: the plan shown`, 5000, () => evalJs(`(async () => { ${B} return b.openViews.value.active === ${JSON.stringify(viewId)}; })()`));
  await sleep(300);
  const sw = () => evalJs("(() => { const s = document.querySelector('[data-bb=view-picture]'); return s ? [...s.children].map((c) => c.classList.contains('on')) : null; })()");
  const canvasHidden = () => evalJs("getComputedStyle(document.getElementById('bimble')).visibility === 'hidden'");

  if (mode === "view") {
    check("view: no Model / On paper switch (capability paper_view false)", (await sw()) === null);
    await evalJs("document.querySelector('[data-bb=quickbar-file]').click()");
    await waitFor("view: the File menu", 5000, () => evalJs("!!document.querySelector('[data-bb=file-menu]')"));
    const ev = await evalJs("(() => { const e = document.querySelector('[data-bb=file-menu-on_paper]'); return e ? { disabled: e.disabled, ny: e.hasAttribute('data-not-yet') } : null; })()");
    check("view: File → Export view greyed, marked not yet", ev?.disabled === true && ev?.ny === true, JSON.stringify(ev));
    await evalJs("(async () => { const d = await import('/src/dialogs/index.ts'); d.currentDialog.value = null; })()");
    const refusal = await evalJs(`(async () => { ${B} const out = [];
      try { b.viewSvg(${JSON.stringify(viewId)}, true); out.push('answered'); } catch (e) { out.push(e.message); }
      try { await b.sheetsPdf(b.sheets().map((s) => s.id)); out.push('answered'); } catch (e) { out.push(e.message); }
      return out; })()`);
    check("view: view_svg and sheets_pdf refused (view-only)", refusal.every((m) => m === "this build is view-only"), JSON.stringify(refusal));
  } else {
    // ---- the switch ----
    check(`${mode}: the switch is offered, Model on`, JSON.stringify(await sw()) === "[true,false]", JSON.stringify(await sw()));
    await evalJs("document.querySelector('[data-bb=view-picture]').children[1].click()");
    await waitFor(`${mode}: the paper picture`, 10000, () => evalJs("!!document.querySelector('[data-bb=paper-view-image]')"));
    const svg = await evalJs("decodeURIComponent(document.querySelector('[data-bb=paper-view-image]').getAttribute('href').replace(/^data:image\\/svg\\+xml;charset=utf-8,/, ''))");
    check(`${mode}: On paper: an SVG with the view's title "${TITLE}"`, svg.includes("<svg") && svg.includes(TITLE), svg.slice(0, 160));
    check(`${mode}: On paper: the model canvas hidden`, await canvasHidden());
    const st = await evalJs(`(async () => { ${B} return { on: b.openViews.value.paper.on, pressed: b.actions.value.find((a) => a.name === 'paper_view')?.pressed }; })()`);
    check(`${mode}: On paper: open_views paper.on, paper_view pressed`, st.on === true && st.pressed === true, JSON.stringify(st));
    check(`${mode}: On paper: the switch says so`, JSON.stringify(await sw()) === "[false,true]", JSON.stringify(await sw()));
    // Pan and zoom move the picture's view box, not the core.
    const vb0 = await evalJs("document.querySelector('[data-bb=paper-view] svg').getAttribute('viewBox')");
    await evalJs("document.querySelector('[data-bb=paper-view]').dispatchEvent(new WheelEvent('wheel', { deltaY: -200, clientX: 600, clientY: 400, bubbles: true, cancelable: true }))");
    await sleep(100);
    const vb1 = await evalJs("document.querySelector('[data-bb=paper-view] svg').getAttribute('viewBox')");
    check(`${mode}: the wheel zooms the paper`, vb0 !== vb1 && Number(vb1.split(' ')[2]) < Number(vb0.split(' ')[2]), `${vb0} -> ${vb1}`);
    await evalJs("document.querySelector('[data-bb=view-picture]').children[0].click()");
    await waitFor(`${mode}: back to the model`, 5000, () => evalJs("!document.querySelector('[data-bb=paper-view]')"));
    check(`${mode}: Model: the canvas shows again`, !(await canvasHidden()));

    // ---- File → Export view ----
    const download = async (tag, file, click) => {
      const dl = join(tmp, `dl-${mode}-${tag}`);
      mkdirSync(dl);
      downloads.clear();
      await cdp.send("Browser.setDownloadBehavior", { behavior: "allow", downloadPath: dl, eventsEnabled: true });
      await evalJs(click);
      const done = await waitFor(`${mode} ${tag}: the download`, 20000, async () => {
        const err = await evalJs("document.querySelector('[data-bb=export-failed]')?.textContent ?? ''");
        if (err) throw new Error(err);
        return [...downloads.values()].find((d) => d.state === "completed") ?? null;
      }).catch((e) => ({ error: e.message }));
      check(`${mode} ${tag}: a download named "${file}"`, done.name === file, JSON.stringify(done));
      const path = join(dl, file);
      return { path, bytes: existsSync(path) ? readFileSync(path) : Buffer.alloc(0) };
    };
    await evalJs("document.querySelector('[data-bb=quickbar-file]').click()");
    await waitFor(`${mode}: the File menu`, 5000, () => evalJs("!!document.querySelector('[data-bb=file-menu-on_paper]')"));
    check(`${mode}: File → Export view enabled`, await evalJs("!document.querySelector('[data-bb=file-menu-on_paper]').disabled"));
    await evalJs("document.querySelector('[data-bb=file-menu-on_paper]').click()");
    await waitFor(`${mode}: the Export dialog on This view`, 5000, () => evalJs("document.querySelector('[data-bb=export-what-view]')?.classList.contains('on') === true"));
    const sel = await evalJs("document.querySelector('[data-bb=export-view-select]')?.value");
    check(`${mode}: Export view: the active view chosen`, sel === viewId, sel);
    await evalJs("document.querySelector('[data-bb=export-pdf]').click()");
    const pdfName = `plan-${VIEW}-1-100.pdf`;
    const pdf = await download("view pdf", pdfName, "document.querySelector('[data-bb=export-go]').click()");
    check(`${mode} view pdf: %PDF- ... %%EOF, Helvetica`, pdf.bytes.subarray(0, 5).toString() === "%PDF-" && pdf.bytes.subarray(-5).toString() === "%%EOF" && pdf.bytes.includes("/BaseFont /Helvetica"));
    const text = pdf.bytes.length ? pdftotext(pdf.path) : "";
    check(`${mode} view pdf: pdftotext finds the view's title`, text !== null && text.replace(/\s+/g, " ").includes(TITLE),
      text === null ? "pdftotext is not installed (poppler)" : text.slice(0, 200).replace(/\s+/g, " "));
    await evalJs("document.querySelector('[data-bb=export-svg]').click()");
    const svgFile = await download("view svg", `plan-${VIEW}-1-100.svg`, "document.querySelector('[data-bb=export-go]').click()");
    check(`${mode} view svg: an SVG with the title`, svgFile.bytes.toString().includes("<svg") && svgFile.bytes.toString().includes(TITLE));
    await evalJs("(async () => { const d = await import('/src/dialogs/index.ts'); d.currentDialog.value = null; })()");
    await sleep(200);

    // ---- Print and export: S-101 and S-102 as one PDF ----
    await evalJs("document.querySelector('[data-bb=quickbar-export]').click()");
    await waitFor(`${mode}: the Export dialog on the sheets`, 5000, () => evalJs("!!document.querySelector('[data-bb=export-sheets]')"));
    await evalJs(`(() => { for (const l of document.querySelectorAll('[data-bb=export-sheets] label')) {
      const c = l.querySelector('input'); if (c.checked !== /S-10[12]/.test(l.textContent)) c.click(); } })()`);
    await evalJs("document.querySelector('[data-bb=export-pdf]').click()");
    await sleep(50);
    check(`${mode}: One PDF enabled for PDF`, await evalJs("!document.querySelector('[data-bb=export-one-pdf]').disabled && !document.querySelector('[data-bb=export-one-pdf]').hasAttribute('data-missing')"));
    await evalJs("document.querySelector('[data-bb=export-one-pdf]').click()");
    const one = await download("one pdf", "S-101-S-102.pdf", "document.querySelector('[data-bb=export-go]').click()");
    const t2 = one.bytes.length ? pdftotext(one.path) : "";
    check(`${mode} one pdf: pdftotext finds S-101 and S-102`, t2 !== null && t2.includes("S-101") && t2.includes("S-102"), t2 === null ? "pdftotext is not installed" : t2.slice(0, 120).replace(/\s+/g, " "));
    const pages = one.bytes.length ? pdfPages(one.path) : NaN;
    check(`${mode} one pdf: 2 pages`, pages === 2, pages === null ? "pdfinfo is not installed (poppler)" : String(pages));
    await evalJs("(async () => { const d = await import('/src/dialogs/index.ts'); d.currentDialog.value = null; })()");
  }

  const IGNORED = [
    /Synchronous XMLHttpRequest on the main thread is deprecated/,
    /404 \(Not Found\) http:\/\/localhost:\d+\/favicon\.ico$/,
  ];
  const errors = consoleLog.filter((m) => /error|warn/.test(m.level) && !IGNORED.some((r) => r.test(m.text)));
  check(`${mode}: no uncaught page error`, exceptions.length === 0, exceptions.join(" | "));
  check(`${mode}: no console error or warning`, errors.length === 0, errors.map((m) => m.text).join(" | "));
  await cdp.send("Target.closeTarget", { targetId });
}

async function main() {
  if (!existsSync(join(UI, "node_modules"))) throw new Error(`no ${UI}/node_modules: run npm ci in web-ui/`);
  execFileSync("node", [join(UI, "scripts/copy-ccore.mjs")], { stdio: "inherit" });
  execFileSync("node", [join(UI, "scripts/copy-ccore.mjs"), "--view"], { stdio: "inherit" });
  copyFileSync(join(CCORE, "golden/drawing/model.json"), MODEL_PUBLIC);
  const model = join(tmp, "frame.json");
  copyFileSync(join(CCORE, "golden/drawing/model.json"), model);
  const host = start("host", BIN, [String(PORT), model], {
    env: { ...process.env, BIMBLE_TOKEN: TOKEN, BIMBLE_FONTS: "", XDG_CONFIG_HOME: join(tmp, "config"), BIMBLE_ORIGINS: `http://localhost:${VITE},http://127.0.0.1:${VITE}` },
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
  for (const mode of ["native", "wasm", "view"]) {
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
console.log(`lab_paper: ${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
