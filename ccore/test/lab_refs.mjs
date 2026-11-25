// Reference plans in the lab's 2D view (ticket L1, session/links_ref.h): node ccore/test/lab_refs.mjs
// Starts out/tcc/bimble-ccore (or $HOST_BIN) with a copy of golden/frame.json, Vite from web-ui/ and
// headless chromium over raw CDP; opens /?native=1&gl=1&port=…&token=… and checks: the ground plan
// (the first floor plan) opens; D1's plan PDF of the frame (golden/drawing/pdf/frame-full-helvetica.pdf)
// linked at 1:100 through the client (bimble.ts linkPdf: action link_pdf {path, scale}) is one
// history step, a row of query links, and its lines are drawn on the overlay in the reference edge
// colour (52,110,180); undo removes it (no such pixel left, no row); linked again with offset_mm set
// so the PDF's drawing of column P1.00 (its lower left square) lies on the model's P1.00, the PDF's
// corner is drawn within one pixel of the column's corner (window.__bbGl.toScreen, the projection
// the model is drawn with); the 30,000-segment many.pdf: the overlay draw's time (printed). No
// uncaught page error. Prints "N passed, M failed"; exits non-zero on a failure. Kills all it starts.
import { spawn } from "node:child_process";
import { copyFileSync, existsSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { CHROMIUM, port, vitePort } from "./ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = resolve(CCORE, "../web-ui");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("lab_refs", 47418);
const VITE = vitePort("lab_refs", 5203);
const TOKEN = "labrefs-token";
const PAGE = `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}`;
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-refs-"));
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

async function main() {
  if (!existsSync(join(UI, "node_modules"))) throw new Error(`no ${UI}/node_modules: run npm ci in web-ui/`);
  const MODEL = join(tmp, "frame.json");
  copyFileSync(join(CCORE, "golden/frame.json"), MODEL);
  const PDF = join(CCORE, "golden/drawing/pdf/frame-full-helvetica.pdf");
  const MANY = join(CCORE, "golden/reference/fixtures/many.pdf");
  const host = start("host", BIN, [String(PORT)], {
    env: { ...process.env, BIMBLE_TOKEN: TOKEN, CCORE_MODEL: MODEL, BIMBLE_ORIGINS: `http://localhost:${VITE},http://127.0.0.1:${VITE}` },
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
  const exceptions = [];
  cdp.on((msg) => {
    if (msg.sessionId === sessionId && msg.method === "Runtime.exceptionThrown") {
      const d = msg.params.exceptionDetails;
      exceptions.push(d.exception?.description ?? d.text);
    }
  });
  await S("Runtime.enable");
  await S("Page.enable");
  await S("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  await S("Page.navigate", { url: PAGE });
  const evalJs = async (expr) => (await S("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true })).result.value;
  await waitFor("the first drawn frame", 30000, () => evalJs("window.__bbGl && window.__bbGl.firstFrameMs"));
  await sleep(500);
  const B = (body) => evalJs(`(async () => { const b = await import('/src/bimble.ts'); ${body} })()`);
  const raf2 = () => evalJs("new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)))");
  const settle = async () => { for (let i = 0; i < 4; i++) await raf2(); };
  /** Overlay pixels in the reference edge colour (52,110,180), within `tol`, optionally in a box. */
  const refPx = (box) => evalJs(`(() => { const o = document.querySelector('[data-bb-overlay]'); const c = o.getContext('2d');
    const r = document.getElementById('bimble').getBoundingClientRect(); const b = ${JSON.stringify(box ?? null)};
    const x0 = b ? Math.max(0, Math.floor(b[0] - r.left)) : 0, y0 = b ? Math.max(0, Math.floor(b[1] - r.top)) : 0;
    const w = b ? b[2] : o.width, h = b ? b[3] : o.height; const d = c.getImageData(x0, y0, w, h).data; let n = 0;
    for (let i = 0; i < d.length; i += 4) if (d[i + 3] > 100 && Math.abs(d[i] - 52) < 40 && Math.abs(d[i + 1] - 110) < 40 && Math.abs(d[i + 2] - 180) < 40) n++;
    return n; })()`);

  // ---- the ground plan ----
  const ov = await B("return b.openViews.value;");
  const plan1 = ov.views.find((v) => v.type === "plan" && v.look === "down");
  check("open_views: a floor plan", !!plan1, JSON.stringify(ov.views.map((v) => v.name)));
  const e0 = await B(`return b.openView('${plan1.id}');`);
  check("the ground plan opens", !e0, String(e0));
  await waitFor("the plan camera", 10000, async () => (await evalJs("window.__bbGl.camera && window.__bbGl.camera.mode")) === "plan").catch(() => false);
  await settle();
  check("no reference pixel before a link", (await refPx()) === 0);

  // ---- link the plan PDF at 1:100 (Rust's placement) ----
  const undo0 = await B("return b.query('history').undo.length;");
  const e1 = await B(`return b.linkPdf(${JSON.stringify(PDF)}, 100);`);
  check("linkPdf(frame-full-helvetica.pdf, 1:100) -> no error", e1 === null, String(e1));
  check("one history step", (await B("return b.query('history').undo.length;")) === undo0 + 1);
  const links = await B("return b.query('links');");
  check("query links: the PDF's row", Array.isArray(links) && links.some((l) => l.kind === "pdf" && l.name === "frame-full-helvetica.pdf" && l.visible), JSON.stringify(links));
  await waitFor("the reference drawn", 10000, async () => (await evalJs("window.__bbRefs && window.__bbRefs.segments")) > 0).catch(() => false);
  await settle();
  const px1 = await refPx();
  check("the PDF's lines are drawn (reference edge colour)", px1 > 500, `${px1} px`);
  const vr = await B("return b.viewReferences();");
  console.log(`view_references: ${vr?.references.length} reference(s), ${vr?.segments} segments, ${vr?.shown} shown`);

  // ---- undo removes it ----
  await B("return b.undo();");
  await waitFor("no reference after undo", 10000, async () => (await evalJs("window.__bbRefs.segments")) === 0).catch(() => false);
  await settle();
  const px2 = await refPx();
  check("undo: no reference pixel left", px2 === 0, `${px2} px`);
  check("undo: no row in query links", !(await B("return b.query('links');")).some((l) => l.kind === "pdf"));

  // ---- the PDF's corner on the column's corner ----
  // P1.00, 300 x 300 mm at (0, 0): its lower left corner is (-150, -150). In the PDF read at 1:100,
  // its drawing is the lowest-left square of about 300 mm (vr: placed by Rust's rule; minus that
  // reference's offset gives the PDF's own coordinates).
  /** The polylines of about 300 x 300 mm (the columns' outlines) as boxes, lowest-left first. */
  const squaresOf = (r) => r.polylines.map((p) => {
    const xs = p.filter((_, i) => i % 2 === 0), ys = p.filter((_, i) => i % 2 === 1);
    return { x: Math.min(...xs), y: Math.min(...ys), w: Math.max(...xs) - Math.min(...xs), h: Math.max(...ys) - Math.min(...ys) };
  }).filter((q) => Math.abs(q.w - 300) < 15 && Math.abs(q.h - 300) < 15).sort((a, b) => a.x + a.y - (b.x + b.y));
  check("the PDF draws the columns (squares of 300 mm)", squaresOf(vr.references[0]).length >= 4, `${squaresOf(vr.references[0]).length}`);
  const e2 = await B(`return b.linkPdf(${JSON.stringify(PDF)}, 100, { offset_mm: [0, 0] });`);
  check("linkPdf at offset (0, 0) -> no error", e2 === null, String(e2));
  const vr0 = await B("return b.viewReferences();");
  const sq0 = squaresOf(vr0.references[0])[0] ?? { x: 0, y: 0 };
  await B("return b.undo();");
  const off = [-150 - sq0.x, -150 - sq0.y];
  const e3 = await B(`return b.linkPdf(${JSON.stringify(PDF)}, 100, { offset_mm: ${JSON.stringify(off)} });`);
  check("linkPdf with offset_mm onto P1.00 -> no error", e3 === null, String(e3));
  await waitFor("drawn again", 10000, async () => (await evalJs("window.__bbRefs.segments")) > 0).catch(() => false);
  await settle();
  const corner = await evalJs("JSON.stringify(window.__bbGl.toScreen(-150, -150, 0))").then((t) => JSON.parse(t));
  const near = corner ? await refPx([corner[0] - 1, corner[1] - 1, 3, 3]) : 0;
  check("the PDF's corner of P1.00 is drawn within 1 px of the column's corner", near > 0, `${JSON.stringify(corner)}: ${near} px in 3 x 3`);
  const far = corner ? await refPx([corner[0] + 40, corner[1] - 60, 20, 20]) : -1;
  check("control: no reference pixel in a free spot beside it", far === 0, `${far} px`);

  // ---- 30,000 segments: the overlay draw's time ----
  const e4 = await B(`return b.linkPdf(${JSON.stringify(MANY)}, 100);`);
  check("linkPdf(many.pdf) -> no error", e4 === null, String(e4));
  await waitFor("many drawn", 10000, async () => (await evalJs("window.__bbRefs.segments")) >= 30000).catch(() => false);
  // The same layer's draw, timed in the page: a second layer over the same view_references,
  // drawn 10 times into a 1280 x 800 2D canvas with the viewport's projection (toScreen).
  const times = await evalJs(`(async () => {
    const m = await import('/src/gl/reference.ts'); const L = m.referenceLayer(); L.refresh('t', true);
    const c = document.createElement('canvas'); c.width = 1280; c.height = 800; const ctx = c.getContext('2d');
    const r = document.getElementById('bimble').getBoundingClientRect();
    const toLocal = (x, y) => { const p = window.__bbGl.toScreen(x, y, 0); return p ? [p[0] - r.left, p[1] - r.top] : null; };
    const out = []; for (let i = 0; i < 10; i++) { const t0 = performance.now(); L.draw(ctx, toLocal); ctx.getImageData(0, 0, 1, 1); out.push(performance.now() - t0); }
    return out; })()`);
  const segs = await evalJs("window.__bbRefs.segments");
  times.sort((a, b) => a - b);
  console.log(`reference draw with ${segs} segments: median ${times[5]?.toFixed(2)} ms, max ${times[9]?.toFixed(2)} ms (overlay stroke, 1280 x 800, headless swiftshader)`);
  check("30,000-segment reference drawn", segs >= 30000, `${segs}`);
  check("no uncaught page error", exceptions.length === 0, exceptions.join(" | "));
}

main().catch((e) => { check("run", false, String(e.stack ?? e)); }).finally(async () => {
  await cleanup();
  console.log(`${passed} passed, ${failed} failed`);
  process.exit(failed ? 1 : 0);
});
