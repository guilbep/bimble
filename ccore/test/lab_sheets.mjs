// Sheets in the lab (ticket SH1, ccore/session/sheets.h, web-ui/src/pages/Sheets.vue):
// node ccore/test/lab_sheets.mjs. Starts out/tcc/bimble-ccore (or $HOST_BIN) with a copy of
// golden/frame.json, Vite from web-ui/ and headless /usr/bin/chromium over raw CDP; opens
// /?native=1&gl=1&port=…&token=… and checks:
//   a new sheet (add_sheet) opened by its sheet tab (tab open, type sheet) shows the Sheets page on
//     it: the paper white at its proportion (A1 landscape, 841 x 594), no placed view;
//   "place view" in the side panel with Plan Niveau 1 (sheet_place at the paper's centre): one
//     outline on the paper with the view's name, one history step; the side panel names the placed
//     view selected and its scale 1:100;
//   a drag of the outline with real mouse events (sheet_move): sheet_layout gives the new at_mm,
//     the drag's distance in paper mm; one history step;
//   undo twice: the outline back at the centre, then gone;
//   the Rust verb goldens still match: `verbs` == golden/verbs_list.json, `describe add_sheet` ==
//     golden/verbs_schema/add_sheet.json.
// No uncaught page error; no console error or warning but the sync-XHR deprecation and the favicon 404.
// Prints "N passed, M failed"; exits non-zero on a failure. Kills all it starts.
import { spawn } from "node:child_process";
import { copyFileSync, existsSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { CHROMIUM, port, vitePort } from "./ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = resolve(CCORE, "../web-ui");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("lab_sheets", 47427);
const VITE = vitePort("lab_sheets", 5221);
const TOKEN = "labsheets-token";
const PAGE = `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}`;
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-sheets-"));
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

async function main() {
  if (!existsSync(join(UI, "node_modules"))) throw new Error(`no ${UI}/node_modules: run npm ci in web-ui/`);
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
  const wsUrl = await waitFor("DevTools line", 15000, () => chrome.out.match(/DevTools listening on (ws:\/\/\S+)/)?.[1]);
  const cdp = await cdpConnect(wsUrl);
  const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank" });
  const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
  const S = (m, p) => cdp.send(m, p, sessionId);
  const consoleLog = [], exceptions = [], toolPosts = [];
  cdp.on((msg) => {
    if (msg.sessionId !== sessionId) return;
    if (msg.method === "Runtime.consoleAPICalled") {
      consoleLog.push({ level: msg.params.type, text: msg.params.args.map((a) => a.value ?? a.description ?? a.type).join(" ") });
    } else if (msg.method === "Runtime.exceptionThrown") {
      const d = msg.params.exceptionDetails;
      exceptions.push(d.exception?.description ?? d.text);
    } else if (msg.method === "Log.entryAdded") {
      consoleLog.push({ level: `log.${msg.params.entry.level}`, text: `${msg.params.entry.text} ${msg.params.entry.url ?? ""}`.trim() });
    } else if (msg.method === "Network.requestWillBeSent") {
      const r = msg.params.request, path = new URL(r.url).pathname;
      if (r.method === "POST" && path.startsWith("/tool/box_")) toolPosts.push(path);
    }
  });
  for (const d of ["Runtime", "Log", "Page", "Network"]) await S(`${d}.enable`);
  await S("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  await S("Page.navigate", { url: PAGE });
  const evalJs = async (expr) => {
    const r = await S("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
    return r.result.value;
  };
  await waitFor("the model (data-bb-ready)", 30000, () => evalJs("(async () => { const b = await import('/src/bimble.ts'); return b.state.value.file === 'frame.bimble' && b.placing.value !== null; })()"));
  const B = (body) => evalJs(`(async () => { const b = await import('/src/bimble.ts'); ${body} })()`);
  const undoCount = () => B("return b.query('history').undo.length;");
  /** SHOTS=<dir>: a screenshot of the page there (for the eye; nothing checked). */
  const shot = async (name) => { if (!process.env.SHOTS) return; const { data } = await S("Page.captureScreenshot", { format: "png" }); (await import("node:fs")).writeFileSync(join(process.env.SHOTS, `${name}.png`), Buffer.from(data, "base64")); };
  const PLAN_N1 = "3w8zGYnP$Ay1lyvMZjDnXq";
  const h0 = await undoCount();

  // ---- a new sheet, opened by its tab ----
  const add = await B("return b.runVerb('add_sheet', { number: 'S-201', name: 'Lab' });");
  check("add_sheet -> null", add === null, String(add));
  const sid = await B("return b.sheets().find((s) => s.number === 'S-201')?.id ?? null;");
  check("the sheet is listed", typeof sid === "string", String(sid));
  const tab = await B(`return b.tabCall('open', { type: 'sheet', id: '${sid}' });`);
  check("tab open, type sheet -> null", tab === null, String(tab));
  const onPage = await waitFor("the Sheets page", 5000, () => evalJs("!!document.querySelector('[data-bb=sheet-paper-rect]')")).catch(() => false);
  check("the sheet tab opens the Sheets page, the paper drawn", onPage === true);
  const title = await evalJs("document.querySelector('[data-bb=sheet-props-title]')?.textContent ?? ''");
  check("…on that sheet", title.includes("S-201"), title);
  const paper = JSON.parse(await evalJs("(() => { const r = document.querySelector('[data-bb=sheet-paper-rect]'); return JSON.stringify([+r.getAttribute('width'), +r.getAttribute('height'), getComputedStyle(r).fill]); })()"));
  check("A1 landscape: 841 x 594, white", paper[0] === 841 && paper[1] === 594 && /255, 255, 255|#fff/i.test(paper[2]), JSON.stringify(paper));
  const box = await evalJs("(() => { const r = document.querySelector('[data-bb=sheet-paper-rect]').getBoundingClientRect(); return [r.width, r.height]; })()");
  check("the paper on screen at its proportion", Math.abs(box[0] / box[1] - 841 / 594) < 0.01, JSON.stringify(box));
  check("no placed view yet", (await evalJs("document.querySelectorAll('[data-bb=sheet-placed]').length")) === 0);

  // ---- place the ground plan from the side panel ----
  await evalJs(`(() => { const s = document.getElementById('sheet-place-view'); s.value = '${PLAN_N1}'; s.dispatchEvent(new Event('change')); })()`);
  await waitFor("the place button on", 3000, () => evalJs("!document.querySelector('[data-bb=sheet-place]').disabled"));
  await evalJs("document.querySelector('[data-bb=sheet-place]').click()");
  const one = await waitFor("one outline", 5000, () => evalJs("document.querySelectorAll('[data-bb=sheet-placed]').length === 1")).catch(() => false);
  check("place view: one outline on the paper", one === true, await evalJs("document.querySelector('[data-bb=sheet-side-error]')?.textContent ?? ''"));
  const lay0 = await B(`return b.sheetLayout('${sid}');`);
  check("sheet_layout: one placed view at the paper's centre", lay0.placed.length === 1 && lay0.placed[0].at_mm[0] === 420.5 && lay0.placed[0].at_mm[1] === 297, JSON.stringify(lay0));
  check("…its box an estimate, the plan at 1:100", lay0.placed[0]?.box === "estimate" && lay0.placed[0].scale === 100 && lay0.placed[0].box_mm[2] > lay0.placed[0].box_mm[0], JSON.stringify(lay0.placed[0]));
  const label = await evalJs("document.querySelector('[data-bb=sheet-placed-name]')?.textContent ?? ''");
  check("the outline shows the view's name", label === lay0.placed[0]?.view_name && label === "Plan Niveau 1", label);
  check("one history step", (await undoCount()) === h0 + 2);
  const selName = await evalJs("document.querySelector('[data-bb=sheet-selected-view]')?.textContent ?? ''");
  const selScale = await evalJs("document.querySelector('[data-bb=sheet-selected-scale]')?.textContent ?? ''");
  check("the side panel: the placed view selected, its scale", selName.includes("Niveau 1") && selScale === "1:100", `${selName} ${selScale}`);

  await shot("sheet-placed");

  // ---- drag it ----
  const r = await evalJs("(() => { const g = document.querySelector('[data-bb=sheet-placed] rect').getBoundingClientRect(); const s = document.querySelector('[data-bb=sheet] svg'); const m = s.getScreenCTM(); return [g.left + g.width / 2, g.top + g.height / 2, m.a]; })()");
  const [x0, y0, pxPerMm] = r, DX = 120, DY = 45;
  await S("Input.dispatchMouseEvent", { type: "mousePressed", x: x0, y: y0, button: "left", buttons: 1, clickCount: 1 });
  for (let k = 1; k <= 10; k++) await S("Input.dispatchMouseEvent", { type: "mouseMoved", x: x0 + (DX * k) / 10, y: y0 + (DY * k) / 10, button: "left", buttons: 1 });
  await S("Input.dispatchMouseEvent", { type: "mouseReleased", x: x0 + DX, y: y0 + DY, button: "left", buttons: 0, clickCount: 1 });
  const lay1 = await waitFor("the move applied", 5000, async () => { const l = await B(`return b.sheetLayout('${sid}');`); return l.placed[0].at_mm[0] !== 420.5 ? l : null; }).catch(() => null);
  const want = [420.5 + DX / pxPerMm, 297 + DY / pxPerMm];
  const got = lay1?.placed[0].at_mm;
  check("drag: sheet_layout gives the new at_mm (the drag in paper mm)", got && Math.abs(got[0] - want[0]) < 0.6 && Math.abs(got[1] - want[1]) < 0.6, JSON.stringify({ got, want, pxPerMm }));
  check("the move is one history step", (await undoCount()) === h0 + 3);
  const moved = await evalJs("(() => { const g = document.querySelector('[data-bb=sheet-placed] rect').getBoundingClientRect(); return [g.left + g.width / 2, g.top + g.height / 2]; })()");
  check("the outline drawn where it was dropped", Math.abs(moved[0] - (x0 + DX)) < 2 && Math.abs(moved[1] - (y0 + DY)) < 2, JSON.stringify({ moved, to: [x0 + DX, y0 + DY] }));

  await shot("sheet-moved");

  // ---- undo twice ----
  await B("return b.undo();");
  const back = await waitFor("undo the move", 5000, async () => { const l = await B(`return b.sheetLayout('${sid}');`); return l.placed[0]?.at_mm[0] === 420.5 && l.placed[0].at_mm[1] === 297; }).catch(() => false);
  check("undo: the view back at the centre", back === true);
  await B("return b.undo();");
  const gone = await waitFor("undo the placement", 5000, () => evalJs("document.querySelectorAll('[data-bb=sheet-placed]').length === 0")).catch(() => false);
  check("undo again: the outline gone", gone === true && (await B(`return b.sheetLayout('${sid}').placed.length;`)) === 0);

  // ---- the Rust verb goldens still match ----
  const verbs = await B("return b.verbs();");
  check("verbs == golden/verbs_list.json", JSON.stringify(verbs) === JSON.stringify(JSON.parse(readFileSync(join(CCORE, "golden/verbs_list.json"), "utf8"))));
  const desc = await B("return b.describe('add_sheet');");
  check("describe add_sheet == golden/verbs_schema/add_sheet.json", JSON.stringify(desc) === JSON.stringify(JSON.parse(readFileSync(join(CCORE, "golden/verbs_schema/add_sheet.json"), "utf8"))));

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
console.log(`lab_sheets: ${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
