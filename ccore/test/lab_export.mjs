// The PDF export in the lab (ticket D7, ccore/session/sheet_svg.h `sheet_pdf`, web-ui/src/dialogs/Export.vue):
// node ccore/test/lab_export.mjs. Starts out/tcc/bimble-ccore (or $HOST_BIN) with a copy of
// golden/drawing/model.json (its sheets), copies the wasm build and the fonts into web-ui/public/ccore/
// (scripts/copy-ccore.mjs; needs out/wasm/ccore.wasm) with the same model as public/ccore/d7-model.json,
// starts Vite from web-ui/ and headless /usr/bin/chromium over raw CDP, and on two pages:
//   native  /?native=1&gl=1&port=…&token=…   (the host reads the fonts from the repository)
//   wasm    /?ccore=1&gl=1&model=ccore/d7-model.json   (the page fetches ccore/fonts/ on the first export)
// opens Print and export (the quick bar), keeps only sheet S-101 checked, picks PDF (no "missing" mark
// on it any more) and clicks Export; the download (CDP Browser.setDownloadBehavior) is
// "S-101 Plan and section.pdf" (Rust's "<number> <name>.pdf"), starts with %PDF- and ends with %%EOF,
// embeds Roboto (/FontFile2: the fonts reached the core), and pdftotext (else: fail, the tool is
// named) finds the sheet number and its name in it. The wasm page fetches each font file once.
// No uncaught page error; no console error or warning but the sync-XHR deprecation and the favicon 404.
// Prints "N passed, M failed"; exits non-zero on a failure. Kills all it starts.
import { execFileSync, spawn } from "node:child_process";
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { CHROMIUM, port, vitePort } from "./ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = resolve(CCORE, "../web-ui");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("lab_export", 47306);
const VITE = vitePort("lab_export", 5196);
const TOKEN = "labexport-token";
const MODEL_PUBLIC = join(UI, "public/ccore/d7-model.json");
const PAGES = {
  native: `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}`,
  wasm: `http://localhost:${VITE}/?ccore=1&gl=1&model=ccore/d7-model.json`,
};
const FILE = "S-101 Plan and section.pdf";
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-export-"));
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

/** One page: open it, export S-101 as PDF from the dialog, check the download. */
async function exportOn(cdp, mode) {
  const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank" });
  const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
  const S = (m, p) => cdp.send(m, p, sessionId);
  const consoleLog = [], exceptions = [], fontGets = [];
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
    } else if (msg.method === "Network.requestWillBeSent") {
      const path = new URL(msg.params.request.url).pathname;
      if (path.includes("/fonts/")) fontGets.push(path);
    }
  });
  const dl = join(tmp, `dl-${mode}`);
  mkdirSync(dl);
  await cdp.send("Browser.setDownloadBehavior", { behavior: "allow", downloadPath: dl, eventsEnabled: true });
  for (const d of ["Runtime", "Log", "Page", "Network"]) await S(`${d}.enable`);
  await S("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  await S("Page.navigate", { url: PAGES[mode] });
  const evalJs = async (expr) => {
    const r = await S("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
    return r.result.value;
  };
  await waitFor(`${mode}: the model with its sheets`, 30000, () =>
    evalJs("(async () => { const b = await import('/src/bimble.ts'); return b.state.value.file !== null && b.sheets().some((s) => s.number === 'S-101'); })()"));

  // ---- Print and export: S-101 only, PDF ----
  await evalJs("document.querySelector('[data-bb=quickbar-export]').click()");
  await waitFor(`${mode}: the Export dialog`, 5000, () => evalJs("!!document.querySelector('[data-bb=export-sheets]')"));
  await evalJs(`(() => { for (const l of document.querySelectorAll('[data-bb=export-sheets] label')) {
    const c = l.querySelector('input'); if (c.checked !== l.textContent.includes('S-101')) c.click(); } })()`);
  await sleep(100);
  const goText = await evalJs("document.querySelector('[data-bb=export-go]').textContent");
  check(`${mode}: one sheet checked`, /\b1\b/.test(goText), goText);
  const pdfBtn = await evalJs("(() => { const b = document.querySelector('[data-bb=export-pdf]'); return b ? [b.disabled, b.hasAttribute('data-missing')] : null; })()");
  check(`${mode}: the PDF choice is there, enabled, not marked missing`, pdfBtn && pdfBtn[0] === false && pdfBtn[1] === false, JSON.stringify(pdfBtn));
  await evalJs("document.querySelector('[data-bb=export-pdf]').click()");
  check(`${mode}: PDF chosen`, await evalJs("document.querySelector('[data-bb=export-pdf]').classList.contains('on')"));
  const t0 = performance.now();
  await evalJs("document.querySelector('[data-bb=export-go]').click()");
  const done = await waitFor(`${mode}: the download`, 20000, async () => {
    const err = await evalJs("document.querySelector('[data-bb=export-failed]')?.textContent ?? ''");
    if (err) throw new Error(err);
    return [...downloads.values()].find((d) => d.state === "completed") ?? null;
  }).catch((e) => ({ error: e.message }));
  console.log(`${mode}: click -> download complete in ${(performance.now() - t0).toFixed(0)} ms`);
  check(`${mode}: a download, named "${FILE}" (Rust's "<number> <name>.pdf")`, done.name === FILE, JSON.stringify(done));
  const file = join(dl, FILE);
  const bytes = existsSync(file) ? readFileSync(file) : Buffer.alloc(0);
  console.log(`${mode}: ${FILE}: ${bytes.length} bytes (files: ${readdirSync(dl).join(", ")})`);
  check(`${mode}: %PDF- ... %%EOF`, bytes.subarray(0, 5).toString() === "%PDF-" && bytes.subarray(-5).toString() === "%%EOF");
  check(`${mode}: Roboto embedded (the fonts reached the core)`, bytes.includes("/FontFile2") && bytes.includes("/BaseFont /Roboto"));
  const text = bytes.length ? pdftotext(file) : "";
  check(`${mode}: pdftotext finds the sheet number and name`, text !== null && text.includes("S-101") && text.includes("Plan and section"),
    text === null ? "pdftotext is not installed (poppler)" : text.slice(0, 200).replace(/\s+/g, " "));
  if (mode === "wasm") {
    const once = ["/ccore/fonts/Roboto-Regular.ttf", "/ccore/fonts/Roboto-Bold.ttf"].every((f) => fontGets.filter((g) => g === f).length === 1);
    check("wasm: each font file fetched once, on the export", once && fontGets.length === 2, fontGets.join(" "));
  } else check("native: the page fetches no font (the host reads them)", fontGets.length === 0, fontGets.join(" "));

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
  check("copy-ccore.mjs puts the fonts in public/ccore/fonts/", ["Roboto-Regular.ttf", "Roboto-Bold.ttf", "LICENSE-Roboto"].every((f) => existsSync(join(UI, "public/ccore/fonts", f))));
  copyFileSync(join(CCORE, "golden/drawing/model.json"), MODEL_PUBLIC);
  const model = join(tmp, "frame.json");
  copyFileSync(join(CCORE, "golden/drawing/model.json"), model);
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
  await exportOn(cdp, "native");
  await exportOn(cdp, "wasm");
  check("native host: no Helvetica fallback note", !host.out.includes("set in Helvetica"), host.out.slice(-300));
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
console.log(`lab_export: ${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
