// Dimensions and texts in the lab's plan view (ticket C4, ccore/src/annot.h, ADR 0068 rule 9):
// node ccore/test/lab_dims.mjs. Starts out/tcc/bimble-ccore (or $HOST_BIN) with a copy of
// golden/frame.json, Vite from web-ui/ and headless /usr/bin/chromium over raw CDP; opens
// /?native=1&gl=1&port=…&token=…, opens Plan Niveau 1 and checks, with real mouse events:
//   the dimension tool (bimble.ts startDimension): a click near the corner (-150,-150) of column
//     P1.00 and one near the corner (5850,-150) of column P1.10 snap and anchor on them, a third
//     click sets the line: one history step; the overlay draws the value "6000";
//   move the column P1.10 by 1000 in x (verb move): the overlay shows "7000"; undo: "6000";
//   a text (startText + a click): drawn by the overlay, one step;
//   a click on the dimension line selects it (not an element); Delete removes it (verb), one step;
//   a drag of the text moves it (move_annotation);
//   Esc stops the tool.
// No uncaught page error; no console error but the sync-XHR deprecation and the favicon 404.
// Prints "N passed, M failed"; exits non-zero on a failure. Kills all it starts.
import { spawn } from "node:child_process";
import { copyFileSync, existsSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { CHROMIUM, port, vitePort } from "./ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = resolve(CCORE, "../web-ui");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("lab_dims", 47413);
const VITE = vitePort("lab_dims", 5217);
const TOKEN = "labdims-token";
const PAGE = `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}`;
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-dims-"));
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
  const wsUrl = await waitFor("DevTools line", 60000, () => chrome.out.match(/DevTools listening on (ws:\/\/\S+)/)?.[1]);
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
  await waitFor("the first drawn frame", 30000, () => evalJs("window.__bbGl && window.__bbGl.firstFrameMs"));
  await waitFor("the model (data-bb-ready)", 15000, () => evalJs("(async () => { const b = await import('/src/bimble.ts'); return b.state.value.file === 'frame.bimble' && b.placing.value !== null; })()"));
  const B = (body) => evalJs(`(async () => { const b = await import('/src/bimble.ts'); ${body} })()`);
  const gl = async () => JSON.parse(await evalJs("JSON.stringify(window.__bbGl)"));
  const rev = () => B("return b.state.value.rev;");
  const undoCount = () => B("return b.query('history').undo.length;");
  const frames = () => evalJs("window.__bbGl.frames");
  const screen = async (x, y, z = 0) => {
    const p = await evalJs(`JSON.stringify(window.__bbGl.toScreen(${x}, ${y}, ${z}))`);
    const xy = JSON.parse(p);
    if (!xy) throw new Error(`(${x}, ${y}) is behind the camera`);
    const on = await evalJs(`document.elementFromPoint(${xy[0]}, ${xy[1]})?.id === 'bimble'`);
    if (!on) throw new Error(`(${x}, ${y}) -> ${xy} is not on the canvas`);
    return xy;
  };
  const move = async ([x, y]) => {
    const f0 = await frames(), n0 = (await gl()).toolCalls;
    await S("Input.dispatchMouseEvent", { type: "mouseMoved", x, y, button: "none", buttons: 0 });
    await waitFor("a tool answer drawn", 5000, async () => (await gl()).toolCalls > n0 && (await frames()) > f0);
  };
  const click = async ([x, y]) => {
    const n0 = (await gl()).toolCalls;
    await S("Input.dispatchMouseEvent", { type: "mousePressed", x, y, button: "left", buttons: 1, clickCount: 1 });
    await S("Input.dispatchMouseEvent", { type: "mouseReleased", x, y, button: "left", buttons: 0, clickCount: 1 });
    await waitFor("the click's tool answer", 5000, async () => (await gl()).toolCalls > n0);
  };
  const setPlacing = async (kind) => {
    const e = await B(`return b.setPlacing({ kind: '${kind}' });`);
    if (e) throw new Error(`setPlacing ${kind}: ${e}`);
    await waitFor(`placing ${kind}`, 5000, () => B(`return b.placing.value?.active === true && b.placing.value.kind === '${kind}';`));
  };
  /** Translucent ghost-blue pixels on the canvas (blue clearly above red and green). */
  const bluePx = () => evalJs(`(() => { const c = document.getElementById('bimble'), g = c.getContext('webgl2');
    const p = new Uint8Array(c.width * c.height * 4); g.readPixels(0, 0, c.width, c.height, g.RGBA, g.UNSIGNED_BYTE, p);
    let n = 0; for (let i = 0; i < p.length; i += 4) if (p[i + 2] - p[i] > 40 && p[i + 2] > p[i + 1]) n++; return n; })()`);
  const near = (a, b, tol) => Math.abs(a[0] - b[0]) <= tol && Math.abs(a[1] - b[1]) <= tol;

  /** Pixels that are not the background (247, 247, 245). */
  const shot = async (name) => { if (!process.env.SHOTS) return; const { data } = await S("Page.captureScreenshot", { format: "png" }); (await import("node:fs")).writeFileSync(join(process.env.SHOTS, `${name}.png`), Buffer.from(data, "base64")); console.log("shot", name, await evalJs("(() => { const c = document.getElementById('bimble'); return c.width + 'x' + c.height; })()")); };
  const modelPx = () => evalJs(`(() => { const c = document.getElementById('bimble'), g = c.getContext('webgl2');
    const p = new Uint8Array(c.width * c.height * 4); g.readPixels(0, 0, c.width, c.height, g.RGBA, g.UNSIGNED_BYTE, p);
    let n = 0; for (let i = 0; i < p.length; i += 4) {
      if (Math.abs(p[i] - 40) <= 2 && Math.abs(p[i + 1] - 110) <= 2 && Math.abs(p[i + 2] - 200) <= 2) continue; // the box's edges and handles
      if (Math.abs(p[i] - 247) > 3 || Math.abs(p[i + 1] - 247) > 3 || Math.abs(p[i + 2] - 245) > 3) n++;
    } return n; })()`);
  /** Keeps the canvas pixels as `name`; diffPx: pixels that differ from it now, the box's colour left out. */
  const keepPx = (name) => evalJs(`(() => { const c = document.getElementById('bimble'), g = c.getContext('webgl2');
    const p = new Uint8Array(c.width * c.height * 4); g.readPixels(0, 0, c.width, c.height, g.RGBA, g.UNSIGNED_BYTE, p); (window.__keep ??= {})['${name}'] = p; return p.length; })()`);
  const diffPx = (name) => evalJs(`(() => { const c = document.getElementById('bimble'), g = c.getContext('webgl2'), q = window.__keep['${name}'];
    const p = new Uint8Array(c.width * c.height * 4); g.readPixels(0, 0, c.width, c.height, g.RGBA, g.UNSIGNED_BYTE, p);
    if (q.length !== p.length) return -1;
    let n = 0; for (let i = 0; i < p.length; i += 4) {
      if (Math.abs(p[i] - 40) <= 2 && Math.abs(p[i + 1] - 110) <= 2 && Math.abs(p[i + 2] - 200) <= 2) continue;
      if (Math.abs(p[i] - q[i]) > 2 || Math.abs(p[i + 1] - q[i + 1]) > 2 || Math.abs(p[i + 2] - q[i + 2]) > 2) n++;
    } return n; })()`);
  /** The canvas pixel [r, g, b] under client (x, y). */
  const pixelAt = ([x, y]) => evalJs(`(() => { const c = document.getElementById('bimble'), g = c.getContext('webgl2'), r = c.getBoundingClientRect();
    const p = new Uint8Array(4); g.readPixels(Math.round(${x} - r.left), c.height - 1 - Math.round(${y} - r.top), 1, 1, g.RGBA, g.UNSIGNED_BYTE, p); return [p[0], p[1], p[2]]; })()`);
  const pickAt = ([x, y]) => evalJs(`window.__bbGl.pickAt(${x}, ${y})`);
  // The cut surface colour of each material family: to_srgb(to_linear(c) * 0.72), as the shader.
  const lin = (c) => (c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4);
  const srgb = (c) => (c <= 0.0031308 ? c * 12.92 : 1.055 * c ** (1 / 2.4) - 0.055);
  const FAMILIES = [[214, 214, 210], [150, 170, 196], [226, 196, 150], [206, 150, 128], [236, 220, 120], [176, 160, 132], [176, 214, 226], [238, 238, 236]];
  const CUTS = FAMILIES.map((f) => f.map((v) => Math.round(255 * srgb(lin(v / 255) * 0.72))));
  const cutPx = () => evalJs(`(() => { const c = document.getElementById('bimble'), g = c.getContext('webgl2'), cuts = ${JSON.stringify(CUTS)};
    const p = new Uint8Array(c.width * c.height * 4); g.readPixels(0, 0, c.width, c.height, g.RGBA, g.UNSIGNED_BYTE, p);
    let n = 0; for (let i = 0; i < p.length; i += 4) if (cuts.some((k) => Math.abs(p[i] - k[0]) <= 1 && Math.abs(p[i + 1] - k[1]) <= 1 && Math.abs(p[i + 2] - k[2]) <= 1)) n++; return n; })()`);
  // Settled: no new frame for 1200 ms (400 ms raced the redraw on a loaded machine) (at most 5000 ms). It waited for one more frame, and none ever came:
  // the frame of the change was drawn already, so each call waited the full 5000 ms (K3: 6 to 15 calls a suite).
  const settle = async () => {
    const until = Date.now() + 5000;
    let f = await frames(), since = Date.now();
    while (Date.now() - since < 1200 && Date.now() < until) {
      await sleep(50);
      const g = await frames();
      if (g !== f) (f = g), (since = Date.now());
    }
  };
  const vis = () => B("return b.query('visibility');");
  const sel = () => B("return b.state.value.selection;");
  const mouseClick = async ([x, y]) => {
    const p0 = (await gl()).picks;
    await S("Input.dispatchMouseEvent", { type: "mousePressed", x, y, button: "left", buttons: 1, clickCount: 1 });
    await S("Input.dispatchMouseEvent", { type: "mouseReleased", x, y, button: "left", buttons: 0, clickCount: 1 });
    await waitFor("the pick", 5000, async () => (await gl()).picks > p0);
    await sleep(150); // the change feed brings the selection back
  };

  const PLAN_N1 = "3w8zGYnP$Ay1lyvMZjDnXq";
  const C60 = "2KmXk9zn1FAPxjTPfLpwLP";
  const e0 = await B(`return b.openView('${PLAN_N1}');`);
  check("open Plan Niveau 1", e0 === null, String(e0));
  await waitFor("the plan camera", 5000, async () => (await gl()).camera?.mode === "plan");
  await settle();
  // Zoom about (3000, -300) until 12 px (the snap tolerance) is under 100 mm: corners then win over
  // the axis crossings at the column centres (snap.rs order).
  for (let k = 0; k < 20 && (await gl()).camera.half > 2400; k++) {
    const z = await screen(3000, -300, 0);
    await S("Input.dispatchMouseEvent", { type: "mouseWheel", x: z[0], y: z[1], deltaX: 0, deltaY: -150 });
    await settle();
  }
  { // then pan (3000, -300) to the canvas centre (a drag pans a plan)
    const from = await screen(3000, -300, 0);
    const to = await evalJs("(() => { const r = document.getElementById('bimble').getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; })()");
    await S("Input.dispatchMouseEvent", { type: "mousePressed", x: from[0], y: from[1], button: "left", buttons: 1, clickCount: 1 });
    for (let k = 1; k <= 10; k++) await S("Input.dispatchMouseEvent", { type: "mouseMoved", x: from[0] + ((to[0] - from[0]) * k) / 10, y: from[1] + ((to[1] - from[1]) * k) / 10, button: "left", buttons: 1 });
    await S("Input.dispatchMouseEvent", { type: "mouseReleased", x: to[0], y: to[1], button: "left", buttons: 0, clickCount: 1 });
    await settle();
  }
  const texts = async () => (await gl()).overlayTexts;
  const aCalls = async () => (await gl()).annotateCalls;
  const aMove = async ([x, y]) => {
    const n0 = await aCalls();
    await S("Input.dispatchMouseEvent", { type: "mouseMoved", x, y, button: "none", buttons: 0 });
    await waitFor("an annotate answer", 5000, async () => (await aCalls()) > n0);
  };
  const aClick = async ([x, y]) => {
    await aMove([x, y]);
    const n0 = await aCalls();
    await S("Input.dispatchMouseEvent", { type: "mousePressed", x, y, button: "left", buttons: 1, clickCount: 1 });
    await S("Input.dispatchMouseEvent", { type: "mouseReleased", x, y, button: "left", buttons: 0, clickCount: 1 });
    await waitFor("the click's annotate answer", 5000, async () => (await aCalls()) > n0);
  };
  const key = async (k, code, vk) => {
    await S("Input.dispatchKeyEvent", { type: "keyDown", key: k, code, windowsVirtualKeyCode: vk });
    await S("Input.dispatchKeyEvent", { type: "keyUp", key: k, code, windowsVirtualKeyCode: vk });
  };

  // ---- the dimension tool ----
  const h0 = await undoCount();
  const sd = await B("return b.startDimension();");
  check("startDimension in the plan -> null", sd === null, String(sd));
  await waitFor("the tool on", 3000, async () => (await gl()).annotate?.active === true);
  await aClick(await screen(-145, -152, 0));
  const st1 = (await gl()).annotate;
  check("click 1 snapped on the corner of P1.00, anchored", JSON.stringify(st1?.points) === "[[-150,-150]]" && st1.anchors[0]?.feature === "corner:0", JSON.stringify(st1));
  await aClick(await screen(5853, -146, 0));
  const st2 = (await gl()).annotate;
  check("click 2 on the corner of P1.10, anchored on it", st2?.points.length === 2 && st2.anchors[1]?.element === C60, JSON.stringify(st2));
  await aMove(await screen(3000, -900, 0));
  await settle();
  check("the preview of the line drawn (dashed): \"6000\"", (await gl()).annotate?.preview?.text === "6000" && (await texts()).includes("6000"), JSON.stringify(await texts()));
  await aClick(await screen(3000, -900, 0));
  const added = (await gl()).annotate?.added;
  check("click 3 adds the dimension, one history step", typeof added === "string" && (await undoCount()) === h0 + 1, String(added));
  await waitFor("the overlay drawn with it", 5000, async () => (await gl()).annotations?.items.some((a) => a.id === added));
  await settle();
  check("the overlay draws the value 6000", (await texts()).filter((t) => t === "6000").length === 1, JSON.stringify(await texts()));
  await key("Escape", "Escape", 27);
  await waitFor("the tool off", 3000, async () => (await gl()).annotate === null || (await gl()).annotate?.active === false);
  check("Esc stops the tool, nothing more added", (await undoCount()) === h0 + 1);
  await shot("dims");

  // ---- the dimension follows its column ----
  const mv = await B(`return b.runVerb('move', { ids: ['${C60}'], d: [1000, 0, 0] });`);
  check("move P1.10 by 1000 in x -> null", mv === null, String(mv));
  const follows = await waitFor("7000 drawn", 5000, async () => (await texts()).includes("7000")).catch(() => false);
  check("the overlay value follows: 7000", follows === true, JSON.stringify(await texts()));
  await B("return b.undo();");
  const back = await waitFor("6000 again", 5000, async () => (await texts()).includes("6000") && !(await texts()).includes("7000")).catch(() => false);
  check("undo: 6000 again", back === true, JSON.stringify(await texts()));

  // ---- a text ----
  const stx = await B("return b.startText('Poteau P1');");
  check("startText -> null", stx === null, String(stx));
  await waitFor("the text tool on", 3000, async () => (await gl()).annotate?.mode === "text");
  await aClick(await screen(2000, 600, 0));
  const tid = (await gl()).annotAdded;
  await waitFor("the text drawn", 5000, async () => (await texts()).includes("Poteau P1")).catch(() => {});
  check("a click adds the text, drawn by the overlay, one step", typeof tid === "string" && (await texts()).includes("Poteau P1") && (await undoCount()) === h0 + 2, JSON.stringify(await texts()));
  check("the text tool stops after one text", (await gl()).annotate === null || (await gl()).annotate.active === false);

  // ---- drag the text ----
  const tAt = JSON.parse(await evalJs(`JSON.stringify(window.__bbGl.annotationScreen('${tid}'))`));
  const t0 = (await gl()).annotations.items.find((a) => a.id === tid).at;
  await S("Input.dispatchMouseEvent", { type: "mousePressed", x: tAt[0], y: tAt[1], button: "left", buttons: 1, clickCount: 1 });
  for (let k = 1; k <= 5; k++) await S("Input.dispatchMouseEvent", { type: "mouseMoved", x: tAt[0] + 8 * k, y: tAt[1], button: "left", buttons: 1 });
  await S("Input.dispatchMouseEvent", { type: "mouseReleased", x: tAt[0] + 40, y: tAt[1], button: "left", buttons: 0, clickCount: 1 });
  const moved = await waitFor("the text moved", 5000, async () => { const a = (await gl()).annotations?.items.find((x) => x.id === tid); return a && a.at[0] > t0[0] + 10 ? a.at : null; }).catch(() => null);
  check("a drag moves the text to the right (move_annotation), one step", moved !== null && Math.abs(moved[1] - t0[1]) < 1e-9 && (await undoCount()) === h0 + 3, JSON.stringify({ t0, moved }));

  // ---- select the dimension, Delete ----
  const sel0 = await sel();
  const dAt = JSON.parse(await evalJs(`JSON.stringify(window.__bbGl.annotationScreen('${added}'))`));
  await S("Input.dispatchMouseEvent", { type: "mousePressed", x: dAt[0], y: dAt[1], button: "left", buttons: 1, clickCount: 1 });
  await S("Input.dispatchMouseEvent", { type: "mouseReleased", x: dAt[0], y: dAt[1], button: "left", buttons: 0, clickCount: 1 });
  await waitFor("the dimension selected", 3000, async () => (await gl()).selectedAnnotation === added).catch(() => {});
  check("a click on the dimension line selects it", (await gl()).selectedAnnotation === added, String((await gl()).selectedAnnotation));
  check("…and no element", JSON.stringify(await sel()) === JSON.stringify(sel0), JSON.stringify(await sel()));
  await key("Delete", "Delete", 46);
  const gone = await waitFor("the dimension gone", 5000, async () => !(await gl()).annotations?.items.some((a) => a.id === added) && !(await texts()).includes("6000")).catch(() => false);
  check("Delete removes it (delete_annotation), one step; the text stays", gone === true && (await undoCount()) === h0 + 4 && (await texts()).includes("Poteau P1"), JSON.stringify(await texts()));
  await B("return b.undo();");
  const again = await waitFor("undo: the dimension back", 5000, async () => (await texts()).includes("6000")).catch(() => false);
  check("undo the delete: drawn again", again === true);
  await shot("dims-end");

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
console.log(`lab_dims: ${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
