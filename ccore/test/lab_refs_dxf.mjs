// DXF reference plans in the lab's 2D view (ticket L2, session/links_ref.h + links_dxf.h):
// node ccore/test/lab_refs_dxf.mjs. Starts out/tcc/bimble-ccore (or $HOST_BIN) with a copy of
// golden/frame.json, Vite from web-ui/ and headless chromium over raw CDP, as lab_refs.mjs (L1).
// In the ground plan: golden/dxf/fixtures/r12.dxf linked through the client (bimble.ts linkDxf:
// action link_dxf {path, offset_mm}) with its (0, 0) on column P1.00's lower left corner
// (-150, -150) is one history step and a row of query links; its lines are drawn in the reference
// edge colour, its (0, 0) within one pixel of the corner (window.__bbGl.toScreen); then
// manage_links by id (bimble.ts manageReference): hide (no pixel, row not visible), show,
// remove (no pixel, no row), undo (back). No uncaught page error. Prints "N passed, M failed".
import { spawn } from "node:child_process";
import { copyFileSync, existsSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { CHROMIUM, port, vitePort } from "./ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = resolve(CCORE, "../web-ui");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("lab_refs_dxf", 47438);
const VITE = vitePort("lab_refs_dxf", 5223);
const TOKEN = "labrefsdxf-token";
const PAGE = `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}`;
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-refs-dxf-"));
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
  const DXF = join(CCORE, "golden/dxf/fixtures/r12.dxf");
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
  check("open_views: a floor plan", !!plan1);
  const e0 = await B(`return b.openView('${plan1.id}');`);
  check("the ground plan opens", !e0, String(e0));
  await waitFor("the plan camera", 10000, async () => (await evalJs("window.__bbGl.camera && window.__bbGl.camera.mode")) === "plan").catch(() => false);
  await settle();
  check("no reference pixel before a link", (await refPx()) === 0);

  // ---- link r12.dxf with its (0, 0) on P1.00's lower left corner (-150, -150) ----
  const undo0 = await B("return b.query('history').undo.length;");
  const e1 = await B(`return b.linkDxf(${JSON.stringify(DXF)}, { offset_mm: [-150, -150], rotation_deg: 0 });`);
  check("linkDxf(r12.dxf) -> no error", e1 === null, String(e1));
  check("one history step", (await B("return b.query('history').undo.length;")) === undo0 + 1);
  const links = await B("return b.query('links');");
  const row = Array.isArray(links) ? links.find((l) => l.kind === "dxf" && l.name === "r12.dxf") : null;
  check("query links: the DXF's row, visible", !!row && row.visible, JSON.stringify(links));
  await waitFor("the reference drawn", 10000, async () => (await evalJs("window.__bbRefs && window.__bbRefs.segments")) > 0).catch(() => false);
  await settle();
  const px1 = await refPx();
  check("the DXF's lines are drawn (reference edge colour)", px1 > 200, `${px1} px`);
  const corner = await evalJs("JSON.stringify(window.__bbGl.toScreen(-150, -150, 0))").then((t) => JSON.parse(t));
  const near = corner ? await refPx([corner[0] - 1, corner[1] - 1, 3, 3]) : 0;
  check("the DXF's (0, 0) is drawn within 1 px of P1.00's corner", near > 0, `${JSON.stringify(corner)}: ${near} px in 3 x 3`);
  const far = corner ? await refPx([corner[0] - 80, corner[1] - 80, 20, 20]) : -1;
  check("control: no reference pixel in a free spot beside it", far === 0, `${far} px`);

  // ---- hide, show, remove, undo (manage_links by id) ----
  const id = row?.id ?? "";
  const eh = await B(`return b.manageReference(${JSON.stringify(id)}, { op: "hide" });`);
  check("hide -> no error", eh === null, String(eh));
  await waitFor("hidden", 10000, async () => (await evalJs("window.__bbRefs.segments")) === 0).catch(() => false);
  await settle();
  const pxh = await refPx();
  check("hidden: no reference pixel", pxh === 0, `${pxh} px`);
  check("hidden: its row says so", (await B("return b.query('links');")).some((l) => l.id === id && !l.visible));
  const es = await B(`return b.manageReference(${JSON.stringify(id)}, { op: "show" });`);
  check("show -> no error", es === null, String(es));
  await waitFor("shown", 10000, async () => (await evalJs("window.__bbRefs.segments")) > 0).catch(() => false);
  await settle();
  check("shown again: drawn", (await refPx()) > 200);
  const er = await B(`return b.manageReference(${JSON.stringify(id)}, { op: "remove" });`);
  check("remove -> no error", er === null, String(er));
  await waitFor("removed", 10000, async () => (await evalJs("window.__bbRefs.segments")) === 0).catch(() => false);
  await settle();
  check("removed: no reference pixel", (await refPx()) === 0);
  check("removed: no row", !(await B("return b.query('links');")).some((l) => l.id === id));
  await B("return b.undo();");
  await waitFor("back after undo", 10000, async () => (await evalJs("window.__bbRefs.segments")) > 0).catch(() => false);
  await settle();
  check("undo the remove: its row and its lines are back", (await B("return b.query('links');")).some((l) => l.id === id) && (await refPx()) > 200);
  check("no uncaught page error", exceptions.length === 0, exceptions.join(" | "));
}

main().catch((e) => { check("run", false, String(e.stack ?? e)); }).finally(async () => {
  await cleanup();
  console.log(`${passed} passed, ${failed} failed`);
  process.exit(failed ? 1 : 0);
});
