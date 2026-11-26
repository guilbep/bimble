// 3D navigation of the lab's WebGL2 viewport (ticket NAV1, web-ui/src/gl/nav.ts, Rust
// crates/bimble-viewer/src/lib.rs wheel_pivot / orbit_pivot_at, bimble-render Camera zoom_at /
// orbit_about / on_target_plane): node ccore/test/lab_nav.mjs. Starts out/tcc/bimble-ccore (or
// $HOST_BIN) with a copy of golden/frame.json (frame.bimble, in a temp dir), Vite from web-ui/ and
// headless /usr/bin/chromium over raw CDP; opens /?native=1&gl=1&port=…&token=… and checks, with real
// mouse and wheel events:
//   wheel over a column's corner: the point hit stays under the cursor (2 px) while N notches zoom in,
//     the eye comes nearer to it by the zoom factor; the pivot is picked once for the N notches (it is
//     kept while only the wheel moves the camera) and again after a pointer move; N notches out at the
//     same place bring the camera back;
//   wheel over empty space: the pivot is on the plane through the orbit target facing the viewer, and
//     stays under the cursor;
//   zoom limit: the distance stops at 10 mm and the target stops there too;
//   orbit drag from the column: the point under the press keeps its screen position (2 px) during the
//     drag, a marker is drawn there, yaw and pitch change by 0.008 rad/px; no select;
//   orbit drag from empty space: about the orbit target (unchanged) with nothing selected; about the
//     selection's centre (kept on screen) with a column selected;
//   a double-click fits all (the target and distance of Fit all, the angles kept).
// No uncaught page error; no console error or warning but the sync-XHR deprecation and the favicon 404.
// Prints "N passed, M failed"; exits non-zero on a failure. Kills all it starts.
import { spawn } from "node:child_process";
import { copyFileSync, existsSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { port, vitePort } from "./ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = resolve(CCORE, "../web-ui");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("lab_nav");
const VITE = vitePort("lab_nav");
const TOKEN = "labnav-token";
const PAGE = `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}`;
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-nav-"));
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

const sub = (a, b) => a.map((v, k) => v - b[k]);
const len = (a) => Math.hypot(...a);
const dot = (a, b) => a.reduce((s, v, k) => s + v * b[k], 0);
const px = (a, b) => (a && b ? Math.hypot(a[0] - b[0], a[1] - b[1]) : Infinity);
const f2 = (p) => (p ? p.map((v) => v.toFixed(1)).join(", ") : String(p));

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
  const chrome = start("chromium", "/usr/bin/chromium", [
    "--headless=new", "--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--remote-debugging-port=0",
    `--user-data-dir=${join(tmp, "chromium")}`, "--no-first-run", "--no-default-browser-check", "about:blank",
  ]);
  const wsUrl = await waitFor("DevTools line", 60000, () => chrome.out.match(/DevTools listening on (ws:\/\/\S+)/)?.[1]);
  const cdp = await cdpConnect(wsUrl);
  const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank" });
  const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
  const S = (m, p) => cdp.send(m, p, sessionId);
  const consoleLog = [], exceptions = [], selects = [];
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
      const r = msg.params.request;
      if (r.method === "POST" && new URL(r.url).pathname.startsWith("/select")) selects.push(r.url);
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
  const J = async (expr) => JSON.parse(await evalJs(`JSON.stringify(${expr})`));
  await waitFor("the first drawn frame", 30000, () => evalJs("window.__bbGl && window.__bbGl.firstFrameMs"));
  await waitFor("the model", 15000, () => evalJs("(async () => { const b = await import('/src/bimble.ts'); return b.state.value.file === 'frame.bimble' && b.placing.value !== null; })()"));
  const B = (body) => evalJs(`(async () => { const b = await import('/src/bimble.ts'); ${body} })()`);
  const gl = async () => JSON.parse(await evalJs("JSON.stringify(window.__bbGl)"));
  const camera = async () => (await gl()).camera;
  const nav = async () => (await gl()).nav;
  const frames = () => evalJs("window.__bbGl.frames");
  const settle = async () => { const f0 = await frames(); await waitFor("a new frame", 3000, async () => (await frames()) > f0).catch(() => {}); await sleep(100); };
  const sel = () => B("return b.state.value.selection;");
  const toScreen = (p) => J(`window.__bbGl.toScreen(${p[0]}, ${p[1]}, ${p[2]})`);
  const pickAt = (p) => evalJs(`window.__bbGl.pickAt(${p[0]}, ${p[1]})`);
  const eyeW = () => J("window.__bbGl.navEye()");
  const hitEl = (x, y) => evalJs(`(() => { const e = document.elementFromPoint(${x}, ${y}); return e ? (e.id || e.dataset.bb || e.tagName) : null; })()`);
  const mouse = (type, x, y, extra = {}) => S("Input.dispatchMouseEvent", { type, x, y, button: type === "mouseMoved" ? "none" : "left",
    buttons: type === "mousePressed" ? 1 : 0, clickCount: 1, ...extra });
  const click = async ([x, y]) => { await mouse("mouseMoved", x, y); await mouse("mousePressed", x, y); await mouse("mouseReleased", x, y); };
  const wheel = async ([x, y], dy) => { await mouse("mouseWheel", x, y, { deltaX: 0, deltaY: dy }); await sleep(20); };
  /** A left drag from `at` by (dx, dy) in `steps`; after each step `each(k)` (the press still held). */
  const drag = async ([x, y], dx, dy, steps, each = async () => {}) => {
    await mouse("mouseMoved", x, y);
    await mouse("mousePressed", x, y);
    for (let k = 1; k <= steps; k++) {
      await mouse("mouseMoved", x + (dx * k) / steps, y + (dy * k) / steps, { button: "left", buttons: 1 });
      await each(k);
    }
    await mouse("mouseReleased", x + dx, y + dy);
    await settle();
  };
  // The overlay's pixel at a client point, read from a copy (no repeated readback of the overlay itself).
  const overlayPx = ([x, y]) => J(`(() => { const c = document.querySelector('[data-bb-overlay="annotations"]'), r = c.getBoundingClientRect(), k = c.width / r.width;
    const cp = document.createElement('canvas'); cp.width = c.width; cp.height = c.height;
    const g = cp.getContext('2d', { willReadFrequently: true }); g.drawImage(c, 0, 0);
    return Array.from(g.getImageData(Math.round((${x} - r.left) * k), Math.round((${y} - r.top) * k), 1, 1).data); })()`);

  await settle();
  const cam0 = await camera();
  const vp = JSON.parse(await evalJs("JSON.stringify(document.getElementById('bimble').getBoundingClientRect())"));
  check("first view: 3D, the navigation hooks there", cam0?.mode === "3d" && (await nav()) !== undefined, JSON.stringify(cam0));

  /** A column the camera sees and a cursor on it near one of its top corners: the corner nudged 3 px
   *  toward the box's middle on screen, where a click picks the column. */
  const columnCorner = async () => {
    const cols = await B("return b.query('elements', { class: 'IfcColumn' }).map((e) => e.id);");
    for (const id of cols) {
      const bx = await J(`window.__bbGl.elementBox(${JSON.stringify(id)})`);
      if (!bx) continue;
      const mid = await toScreen(bx.min.map((v, k) => (v + bx.max[k]) / 2));
      if (!mid) continue;
      for (const [i, j] of [[0, 0], [1, 0], [0, 1], [1, 1]]) {
        const corner = [i ? bx.max[0] : bx.min[0], j ? bx.max[1] : bx.min[1], bx.max[2]];
        const c = await toScreen(corner);
        if (!c) continue;
        const d = px(c, mid) || 1, at = [c[0] + (3 * (mid[0] - c[0])) / d, c[1] + (3 * (mid[1] - c[1])) / d];
        if (at[0] < vp.left + 20 || at[0] > vp.right - 150 || at[1] < vp.top + 20 || at[1] > vp.bottom - 20) continue;
        if ((await hitEl(...at)) === "bimble" && (await pickAt(at)) === id) return { id, box: bx, corner, at };
      }
    }
    throw new Error("no column corner the camera sees");
  };
  /** Empty space on the canvas: nothing picked there, the canvas under the pointer. */
  const emptySpot = async () => {
    for (const [fx, fy] of [[0.05, 0.1], [0.05, 0.9], [0.5, 0.06], [0.3, 0.95], [0.9, 0.9], [0.05, 0.5]]) {
      const at = [vp.left + fx * vp.width, vp.top + fy * vp.height];
      if ((await hitEl(...at)) === "bimble" && (await pickAt(at)) === null) return at;
    }
    throw new Error("no empty spot on the canvas");
  };

  // ---- the wheel over a column's corner ----
  const cc = await columnCorner();
  console.log(`column ${cc.id}, corner ${f2(cc.corner)}, cursor ${f2(cc.at)}`);
  const N = 5, nSel0 = selects.length;
  await mouse("mouseMoved", ...cc.at);
  const n0 = await nav(), c0 = await camera(), e0 = await eyeW();
  await wheel(cc.at, -100);
  const n1 = await nav(), p = n1.zoomPivot;
  const inBox = p && p.every((v, k) => v >= cc.box.min[k] - 2 && v <= cc.box.max[k] + 2);
  check("wheel over the column: the pivot is the point hit on it (one pick)", n1.zoomFrom === "hit" && inBox && n1.picks === n0.picks + 1,
    `pivot ${f2(p)} (${n1.zoomFrom}), box ${f2(cc.box.min)} .. ${f2(cc.box.max)}, picks ${n0.picks} -> ${n1.picks}`);
  const d0 = len(sub(e0, p));
  const drift = [];
  for (let k = 1; k < N; k++) { await wheel(cc.at, -100); drift.push(px(await toScreen(p), cc.at)); }
  await settle();
  const n2 = await nav(), c1 = await camera(), e1 = await eyeW();
  const sNow = await toScreen(p);
  check(`after ${N} notches in, the point is still under the cursor (2 px)`, px(sNow, cc.at) <= 2 && Math.max(...drift) <= 2,
    `at ${f2(sNow)}, cursor ${f2(cc.at)}, drift per notch ${drift.map((v) => v.toFixed(2)).join(" ")}`);
  const f = Math.exp(-0.1 * N);
  check("the eye came nearer the point by the zoom factor (e^-0.1 per notch)", Math.abs(len(sub(e1, p)) / d0 - f) < 1e-3 && Math.abs(c1.distance / c0.distance - f) < 1e-3,
    `eye to point ${d0.toFixed(0)} -> ${len(sub(e1, p)).toFixed(0)} mm (x${(len(sub(e1, p)) / d0).toFixed(4)}, expected x${f.toFixed(4)}), distance ${c0.distance.toFixed(0)} -> ${c1.distance.toFixed(0)}`);
  check(`the pivot kept for the ${N} notches (picked once)`, n2.picks === n0.picks + 1 && JSON.stringify(n2.zoomPivot) === JSON.stringify(p), `picks ${n0.picks} -> ${n2.picks}`);
  for (let k = 0; k < N; k++) await wheel(cc.at, 100);
  await settle();
  const n3 = await nav(), c2 = await camera();
  check(`${N} notches out at the same place: the camera back, no new pick`, n3.picks === n2.picks && Math.abs(c2.distance - c0.distance) < 1e-6 * c0.distance &&
    len(sub(c2.target, c0.target)) < 1e-3 * Math.max(1, c0.distance / 1000) && c2.yaw === c0.yaw && c2.pitch === c0.pitch,
    `distance ${c0.distance} -> ${c2.distance}, target ${f2(c0.target)} -> ${f2(c2.target)}, picks ${n2.picks} -> ${n3.picks}`);
  const moved = [cc.at[0] + 4, cc.at[1]];
  await mouse("mouseMoved", ...moved);
  await wheel(moved, -100);
  const n4 = await nav();
  check("a pointer move drops the pivot: the next notch picks again", n4.picks === n3.picks + 1 && n4.zoomFrom === "hit", `picks ${n3.picks} -> ${n4.picks}`);
  await wheel(moved, 100);
  check("no wheel selected anything", selects.length === nSel0 && JSON.stringify(await sel()) === "[]");

  // ---- the wheel over empty space: the plane through the orbit target ----
  const em = await emptySpot();
  await mouse("mouseMoved", ...em);
  const n5 = await nav();
  for (let k = 0; k < N; k++) await wheel(em, -100);
  await settle();
  const n6 = await nav(), c3 = await camera(), q = n6.zoomPivot, eq = await eyeW();
  // The target in world mm: the eye minus the view direction times the distance.
  const dir = [Math.cos(c3.pitch) * Math.cos(c3.yaw), Math.cos(c3.pitch) * Math.sin(c3.yaw), Math.sin(c3.pitch)];
  const tW = eq.map((v, k) => v - c3.distance * dir[k]);
  const sq = q && await toScreen(q);
  check("wheel over empty space: the pivot on the plane through the target, facing the viewer", n6.zoomFrom === "plane" && q && Math.abs(dot(sub(q, tW), dir)) < 1,
    `pivot ${f2(q)} (${n6.zoomFrom}), off the plane by ${q ? dot(sub(q, tW), dir).toFixed(3) : "?"} mm`);
  check(`after ${N} notches, that point still under the cursor (2 px), picked once`, px(sq, em) <= 2 && n6.picks === n5.picks + 1, `at ${f2(sq)}, cursor ${f2(em)}, picks ${n5.picks} -> ${n6.picks}`);
  for (let k = 0; k < N; k++) await wheel(em, 100);

  // ---- the zoom limit (over the same empty spot: the ray through it stays the same line) ----
  for (let k = 0; k < 20; k++) await wheel(em, -1000);
  const cz = await camera();
  await wheel(em, -1000);
  const cz2 = await camera();
  check("zoom limit: the distance stops at 10 mm, the target stops with it", cz.distance === 10 && cz2.distance === 10 && JSON.stringify(cz2.target) === JSON.stringify(cz.target),
    `distance ${cz.distance}, target ${f2(cz.target)} -> ${f2(cz2.target)}`);

  // ---- a double-click fits all ----
  const fitR = JSON.parse(await evalJs("JSON.stringify(document.querySelector('[data-bb=\"view-fit\"]')?.getBoundingClientRect() ?? null)"));
  const em2 = em;
  check("the spot is still empty after the zoom", (await pickAt(em2)) === null);
  const nf0 = (await nav()).fits, cpre = await camera();
  await mouse("mouseMoved", ...em2);
  await mouse("mousePressed", ...em2, { clickCount: 1 });
  await mouse("mouseReleased", ...em2, { clickCount: 1 });
  await mouse("mousePressed", ...em2, { clickCount: 2 });
  await mouse("mouseReleased", ...em2, { clickCount: 2 });
  await settle();
  const cdbl = await camera();
  let fitOk = fitR !== null;
  if (fitR) {
    await wheel(em2, -300);
    await click([fitR.left + fitR.width / 2, fitR.top + fitR.height / 2]);
    await settle();
    const cfa = await camera();
    fitOk = Math.abs(cfa.distance - cdbl.distance) < 1e-6 * cfa.distance && JSON.stringify(cfa.target) === JSON.stringify(cdbl.target);
  }
  check("a double-click fits all (as Fit all; the angles kept)", (await nav()).fits === nf0 + 1 && fitOk && cdbl.yaw === cpre.yaw && cdbl.pitch === cpre.pitch &&
    cdbl.distance > 1000 * cpre.distance && JSON.stringify(cdbl.target) === JSON.stringify(cam0.target),
    `distance ${cpre.distance} -> ${cdbl.distance}, target ${f2(cdbl.target)} (first view ${f2(cam0.target)})`);

  // ---- an orbit drag from the column ----
  const co = await columnCorner();
  const picks0 = (await gl()).picks, nSel1 = selects.length, o0 = await camera();
  let pivotO = null, from = null, worst = 0, marker = null;
  await drag(co.at, 60, 24, 6, async (k) => {
    const n = await nav();
    if (k === 1) { pivotO = n.orbitPivot; from = n.orbitFrom; }
    if (!pivotO) return;
    worst = Math.max(worst, px(await toScreen(pivotO), co.at));
    if (k === 6) { await settle(); marker = await overlayPx(co.at); }
  });
  const o1 = await camera(), nEnd = await nav();

  await sleep(500);
  check("orbit from the column: the pivot is the point under the press", from === "hit" && pivotO && pivotO.every((v, k) => v >= co.box.min[k] - 2 && v <= co.box.max[k] + 2),
    `pivot ${f2(pivotO)} (${from})`);
  check("during the drag the pivot keeps its screen position (2 px)", pivotO !== null && worst <= 2, `worst ${worst.toFixed(3)} px`);
  // The first step (10, 4) px is already past the click distance (4 px): the whole (60, 24) px is applied.
  check("yaw and pitch turned by the drag (0.008 rad/px), no select, no click pick", o1.yaw !== o0.yaw && o1.pitch !== o0.pitch &&
    Math.abs(o0.yaw - o1.yaw - 60 * 0.008) < 1e-6 && Math.abs(o1.pitch - o0.pitch - 24 * 0.008) < 1e-6 && (await gl()).picks === picks0 && selects.length === nSel1,
    `yaw ${o0.yaw.toFixed(4)} -> ${o1.yaw.toFixed(4)}, pitch ${o0.pitch.toFixed(4)} -> ${o1.pitch.toFixed(4)}`);
  check("a marker at the pivot while it turns (the selection fill, 31 78 140), gone after", marker && Math.abs(marker[0] - 31) < 12 && Math.abs(marker[1] - 78) < 12 &&
    Math.abs(marker[2] - 140) < 12 && marker[3] > 200 && nEnd.orbitPivot === null && (await overlayPx(co.at))[3] === 0,
    `pixel ${marker}, after the drag ${await overlayPx(co.at)}, pivot ${JSON.stringify(nEnd.orbitPivot)}`);

  // ---- an orbit drag from empty space, nothing selected: about the target ----
  const ee = await emptySpot();
  const t0 = await camera();
  let tWorst = 0, fromE = null, pivE;
  await drag(ee, 40, 15, 5, async (k) => {
    const n = await nav(), c = await camera();
    if (k === 1) { fromE = n.orbitFrom; pivE = n.orbitPivot; }
    tWorst = Math.max(tWorst, len(sub(c.target, t0.target)));
  });
  const t1 = await camera();
  check("orbit from empty space, nothing selected: about the orbit target (kept)", fromE === "target" && pivE === null && tWorst === 0 && t1.yaw !== t0.yaw,
    `from ${fromE}, target moved ${tWorst} mm, yaw ${t0.yaw.toFixed(4)} -> ${t1.yaw.toFixed(4)}`);

  // ---- an orbit drag from empty space with a column selected: about its centre ----
  const cs = await columnCorner();
  await click(cs.at);
  await waitFor("the column selected", 5000, async () => (await sel())[0] === cs.id).catch(() => {});
  await sleep(300); // the selection opens a panel: the canvas may resize
  await settle();
  check("a click on the column still selects it", JSON.stringify(await sel()) === JSON.stringify([cs.id]), JSON.stringify(await sel()));
  const centre = cs.box.min.map((v, k) => (v + cs.box.max[k]) / 2);
  const es = await emptySpot();
  const sc0 = await toScreen(centre);
  let cWorst = 0, fromS = null, pivS = null;
  await drag(es, -40, 12, 5, async (k) => {
    const n = await nav();
    if (k === 1) { fromS = n.orbitFrom; pivS = n.orbitPivot; }
    cWorst = Math.max(cWorst, px(await toScreen(centre), sc0));
  });
  check("orbit from empty space with a column selected: about the column's centre, kept on screen (2 px)",
    fromS === "selection" && pivS && len(sub(pivS, centre)) < 1 && cWorst <= 2 && JSON.stringify(await sel()) === JSON.stringify([cs.id]),
    `from ${fromS}, pivot ${f2(pivS)}, centre ${f2(centre)}, worst ${cWorst.toFixed(3)} px`);

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
console.log(`lab_nav: ${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
