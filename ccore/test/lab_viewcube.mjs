// The view cube of the lab's WebGL2 viewport (ticket VC1, web-ui/src/viewcube.ts, Rust
// crates/bimble-viewer/src/viewcube.rs): node ccore/test/lab_viewcube.mjs. Starts out/tcc/bimble-ccore
// (or $HOST_BIN) with a copy of golden/frame.json (frame.bimble, in a temp dir), Vite from web-ui/
// and headless /usr/bin/chromium over raw CDP; opens /?native=1&gl=1&port=…&token=… and checks, with
// real mouse events:
//   corner: the cube's canvas is 120 x 120 px, 8 px from the viewport's top-right corner; its pixels
//     show the faces (grey 214), the edge and corner bands (196), the compass ring (90) and the red N;
//     the rest of its square is transparent; it is what the pointer hits there;
//   hover: the target under the pointer is filled with ink (the control accent, ADR 0049);
//   pick: a click on a column selects it; a click on the cube never selects (no pick, no select
//     request, the selection kept);
//   clicks: Top looks straight down, north up (yaw -90°, pitch 1.57) in 300 ms (the page's frame
//     log: every frame on the way strictly between, one way; the arrival at 300 ms or later), the
//     path animated (a turn on a clock slowed 10 x draws in-between angles: KEY1, robust under load)
//     and the cube's middle is then TOP; the front-top edge (45° down), Front (level, the cube's
//     middle FRONT), a corner (isometric: yaw -45°, pitch 35.26°);
//   drags: on the cube, orbit (0.008 rad/px: yaw by the drag, no select); on the compass ring (the S),
//     a turn about the vertical (pitch kept); the wheel over the cube zooms the view;
//   Home: back to yaw -55°, pitch 28° and fit all (the target and distance of the first view);
//     Fit all: the distance of the fit, the angles kept;
//   plan: in a plan tab the cube and Home are off (the pointer there reaches the 3D canvas; a click
//     there turns nothing), Fit all stays and fits the plan; back to 3D the cube is back.
// No uncaught page error; no console error or warning but the sync-XHR deprecation and the favicon 404.
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
const PORT = port("lab_viewcube");
const VITE = vitePort("lab_viewcube");
const TOKEN = "labcube-token";
const PAGE = `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}`;
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-viewcube-"));
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

const TAU = 2 * Math.PI, DEG = Math.PI / 180;
/** Angles equal modulo a full turn. */
const angEq = (a, b, tol = 2e-3) => Math.abs((((a - b) % TAU) + 1.5 * TAU) % TAU - TAU / 2) <= tol;
const fmt = (c) => `yaw ${(c.yaw / DEG).toFixed(2)}°, pitch ${(c.pitch / DEG).toFixed(2)}° (${c.pitch.toFixed(4)})`;

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
  await waitFor("the first drawn frame", 30000, () => evalJs("window.__bbGl && window.__bbGl.firstFrameMs"));
  await waitFor("the model", 15000, () => evalJs("(async () => { const b = await import('/src/bimble.ts'); return b.state.value.file === 'frame.bimble' && b.placing.value !== null; })()"));
  const B = (body) => evalJs(`(async () => { const b = await import('/src/bimble.ts'); ${body} })()`);
  const gl = async () => JSON.parse(await evalJs("JSON.stringify(window.__bbGl)"));
  const camera = async () => (await gl()).camera;
  const frames = () => evalJs("window.__bbGl.frames");
  // Settled: no new frame for 1200 ms (400 ms raced the redraw on a loaded machine) (at most 3000 ms). It waited for one more frame, and none ever came:
  // the frame of the change was drawn already, so each call waited the full 3000 ms (K3: 6 to 15 calls a suite).
  const settle = async () => {
    const until = Date.now() + 3000;
    let f = await frames(), since = Date.now();
    while (Date.now() - since < 1200 && Date.now() < until) {
      await sleep(50);
      const g = await frames();
      if (g !== f) (f = g), (since = Date.now());
    }
  };
  const sel = () => B("return b.state.value.selection;");
  // SHOTS=<dir>: a screenshot of the cube's corner at a few steps (for a look, not checked).
  const shot = async (name) => {
    if (!process.env.SHOTS) return;
    const r = await rectOf("#bimble");
    const { data } = await S("Page.captureScreenshot", { format: "png", clip: { x: r.right - 200, y: r.top, width: 200, height: 200, scale: 2 } });
    (await import("node:fs")).writeFileSync(join(process.env.SHOTS, `cube-${name}.png`), Buffer.from(data, "base64"));
  };
  const rectOf = async (q) => JSON.parse(await evalJs(`JSON.stringify(document.querySelector(${JSON.stringify(q)})?.getBoundingClientRect() ?? null)`));
  const hitEl = (x, y) => evalJs(`(() => { const e = document.elementFromPoint(${x}, ${y}); return e ? (e.id || e.dataset.bb || e.tagName) : null; })()`);
  const cellAt = async (d) => JSON.parse(await evalJs(`JSON.stringify(window.__bbGl.cubeCellAt(${JSON.stringify(d)}))`));
  const cubeHitAt = async ([x, y]) => JSON.parse(await evalJs(`JSON.stringify(window.__bbGl.cubeHitAt(${x}, ${y}))`));
  const mouse = (type, x, y, extra = {}) => S("Input.dispatchMouseEvent", { type, x, y, button: type === "mouseMoved" ? "none" : "left",
    buttons: type === "mousePressed" ? 1 : 0, clickCount: 1, ...extra });
  const click = async ([x, y]) => { await mouse("mouseMoved", x, y); await mouse("mousePressed", x, y); await mouse("mouseReleased", x, y); };
  const drag = async ([x, y], dx, dy, steps = 5) => {
    await mouse("mouseMoved", x, y);
    await mouse("mousePressed", x, y);
    for (let k = 1; k <= steps; k++) await mouse("mouseMoved", x + (dx * k) / steps, y + (dy * k) / steps, { button: "left", buttons: 1 });
    await mouse("mouseReleased", x + dx, y + dy);
    await settle();
  };
  /** Clicks `at` and waits for the turn's end. `log`: the page's own record of the turn, one entry
   *  per frame drawn (glviewport.ts turnLog: the time since the turn started and the angles), read
   *  in the page, not polled over CDP (KEY1: polling saw 0 in-between angles under load).
   *  `slow`: the turn's clock runs that much slower (window.__bbGl.clockScale, a test hook). */
  const clickTurn = async (at, slow = 1) => {
    await evalJs(`window.__bbGl.clockScale = ${slow}; true`);
    const c0 = await camera(), n0 = await evalJs("window.__bbGl.turnLog?.n ?? 0");
    await click(at);
    await waitFor("a turn", 3000, () => evalJs(`(window.__bbGl.turnLog?.n ?? 0) > ${n0}`)).catch(() => {});
    // The end: the log's last frame arrived (the camera's own state is written by the next frame).
    await waitFor("the turn's end", 5000 / slow, () => evalJs(`window.__bbGl.turnLog.n > ${n0} && window.__bbGl.turnLog.frames.at(-1)?.done === true`)).catch(() => {});
    const end = await waitFor("the camera at rest", 3000, async () => { const c = await camera(); return c.turning ? null : c; });
    const log = await evalJs("JSON.parse(JSON.stringify(window.__bbGl.turnLog ?? null))");
    await evalJs("window.__bbGl.clockScale = 1; true");
    await settle();
    return { c0, end, log };
  };
  /** The turn's frames: every frame's pitch moves one way (from the start toward the end), the
   *  frames drawn before the arrival strictly between start and end, the arrival exact and last. */
  const turnPath = (t, coord) => {
    const f = t.log?.frames ?? [], from = t.c0[coord], to = t.end[coord], sgn = Math.sign(to - from);
    const mid = f.filter((x) => !x.done);
    const oneWay = f.every((x, i) => i === 0 || sgn * (x[coord] - f[i - 1][coord]) >= -1e-9) && sgn * (f[0]?.[coord] - from) >= -1e-9;
    const between = mid.filter((x) => sgn * (x[coord] - from) > 0 && sgn * (to - x[coord]) > 0);
    const arrived = f.length > 0 && f.at(-1).done && Math.abs(f.at(-1)[coord] - to) < 1e-9 && f.filter((x) => x.done).length === 1;
    return { n: f.length, mid: mid.length, between: between.length, oneWay, arrived,
      lastMidMs: mid.length ? Math.max(...mid.map((x) => x.ms)) : null, arriveMs: f.at(-1)?.ms ?? null };
  };
  /** The cube's own pixels (its 2D canvas): counts by colour, the corner's alpha, the pixel at a client point. */
  const cubePx = (at = null) => evalJs(`(() => {
    // A copy read once (a fresh canvas each time: no repeated readback of the cube's own canvas).
    const c = document.querySelector('[data-bb="viewcube"]'), r = c.getBoundingClientRect(), cp = document.createElement('canvas');
    cp.width = c.width; cp.height = c.height;
    const x = cp.getContext('2d', { willReadFrequently: true });
    x.drawImage(c, 0, 0);
    const p = x.getImageData(0, 0, c.width, c.height).data, k = c.width / r.width;
    const near = (i, v, t = 1) => Math.abs(p[i] - v[0]) <= t && Math.abs(p[i + 1] - v[1]) <= t && Math.abs(p[i + 2] - v[2]) <= t && p[i + 3] === 255;
    let face = 0, band = 0, ring = 0, red = 0, opaque = 0;
    for (let i = 0; i < p.length; i += 4) {
      if (p[i + 3] > 0) opaque++;
      if (near(i, [214, 214, 214])) face++;
      else if (near(i, [196, 196, 196])) band++;
      else if (near(i, [90, 90, 90])) ring++;
      else if (p[i] > 200 && p[i + 1] < 140 && p[i + 2] < 140 && p[i + 3] > 200) red++;
    }
    const at = ${JSON.stringify(at)};
    let px = null;
    if (at) { const i = 4 * (Math.round((at[1] - r.top) * k) * c.width + Math.round((at[0] - r.left) * k)); px = [p[i], p[i + 1], p[i + 2], p[i + 3]]; }
    return { w: c.width, h: c.height, n: c.width * c.height, face, band, ring, red, opaque, corner: p[3], px };
  })()`);

  await settle();
  const cam0 = await camera();
  const nav0 = (await gl()).viewCube;
  check("first view: yaw -55°, pitch 28° (project north 0), the cube shown", angEq(cam0.yaw, -55 * DEG, 1e-6) && Math.abs(cam0.pitch - 28 * DEG) < 1e-6 && nav0?.shown === true,
    `${fmt(cam0)}, ${JSON.stringify(nav0)}`);

  // ---- the cube in its corner, its pixels ----
  const vp = await rectOf("#bimble"), cr = await rectOf('[data-bb="viewcube"]');
  check("the cube: 120 x 120 px, 8 px from the viewport's top-right corner", cr && Math.abs(cr.width - 120) < 0.5 && Math.abs(cr.height - 120) < 0.5 &&
    Math.abs(vp.right - cr.right - 8) <= 1 && Math.abs(cr.top - vp.top - 8) <= 1, `viewport ${JSON.stringify(vp)}, cube ${JSON.stringify(cr)}`);
  const px0 = await cubePx();
  console.log(`cube pixels: ${JSON.stringify(px0)}`);
  check("the cube's pixels: faces (214), bands (196), the ring (90)", px0.face > 600 && px0.band > 600 && px0.ring > 150, JSON.stringify(px0));
  check("the rest of the cube's square is transparent (the model shows through)", px0.corner === 0 && px0.opaque < 0.6 * px0.n, `${px0.opaque} of ${px0.n} opaque`);
  const centre = [cr.left + 60, cr.top + 60 - 120 * 0.08];
  check("the pointer over the cube hits the cube, not the 3D canvas", (await hitEl(...centre)) === "viewcube", String(await hitEl(...centre)));

  // ---- hover ----
  await shot("0-first");
  const top0 = await cellAt([0, 0, 1]);
  await mouse("mouseMoved", ...top0);
  await sleep(100);
  await shot("1-hover-top");
  const hov = (await gl()).viewCube.hover, hpx = (await cubePx(top0)).px;
  // The face's label is drawn over its middle (grey 60, as Rust): ink with a little label.
  check("hover: the Top target filled with ink (20, 20, 20)", JSON.stringify(hov) === "[0,0,1]" && hpx && hpx.slice(0, 3).every((v) => v <= 40),
    `hover ${JSON.stringify(hov)}, pixel ${hpx}`);

  // ---- a click on a column selects it ----
  const cols = await B("return b.query('elements', { class: 'IfcColumn' }).map((e) => e.id);");
  let col = null, colXY = null;
  for (const id of cols) {
    const e = await B(`return b.element('${id}');`);
    const xy = JSON.parse(await evalJs(`JSON.stringify(window.__bbGl.toScreen(${e.placement.origin[0]}, ${e.placement.origin[1]}, ${e.placement.origin[2] + 0.5 * (e.generator_params.height ?? 3000)}))`));
    if (xy && (await hitEl(...xy)) === "bimble" && (await evalJs(`window.__bbGl.pickAt(${xy[0]}, ${xy[1]})`)) === id) { col = id; colXY = xy; break; }
  }
  if (!col) throw new Error("no column the camera sees");
  const pk0 = (await gl()).picks;
  await click(colXY);
  await waitFor("the column selected", 5000, async () => (await sel())[0] === col).catch(() => {});
  check("pick: a click on a column selects it", (await gl()).picks === pk0 + 1 && JSON.stringify(await sel()) === JSON.stringify([col]), JSON.stringify(await sel()));
  await sleep(300); // the selection opens a panel: the canvas may resize
  await settle();
  // A pause after the selection (K3: the Top turn started 0.4 to 1 s after it drew a single frame
  // in its 300 ms). Since KEY1 the path is proven by the page's frame log and a slowed turn, not by
  // this pause; it stays for the panel's resize.
  await sleep(2600);

  // ---- clicks on the cube ----
  const picks1 = (await gl()).picks, selects1 = selects.length;
  const top = await clickTurn(await cellAt([0, 0, 1]));
  console.log(`Top: ${fmt(top.end)}`);
  check("Top: straight down, north up (yaw -90°, pitch 1.57)", angEq(top.end.yaw, -Math.PI / 2) && Math.abs(top.end.pitch - 1.57) < 1e-3, fmt(top.end));
  // The turn as drawn, from the page's own frame log (KEY1: headless SwiftShader under load may draw
  // a single frame in 300 ms; then the arrival is the only frame, and the slowed turn below shows the path).
  const tp = turnPath(top, "pitch");
  console.log(`Top: ${tp.n} frames drawn, ${tp.mid} before the arrival (last at ${tp.lastMidMs?.toFixed(0)} ms), arrival at ${tp.arriveMs?.toFixed(0)} ms`);
  check("Top: the turn takes 300 ms (frames before the arrival under 300 ms, the arrival at 300 ms or later)",
    tp.arrived && tp.arriveMs >= 300 && (tp.lastMidMs === null || tp.lastMidMs < 300), JSON.stringify(tp));
  check("Top: every frame drawn on the way is strictly between start and end, one way", tp.oneWay && tp.between === tp.mid, JSON.stringify(tp));
  const cr2 = await rectOf('[data-bb="viewcube"]'), mid = [cr2.left + 60, cr2.top + 60 - 120 * 0.08];
  await shot("2-top");
  const pxTop = await cubePx();
  check("Top: the whole compass in sight, its N in red", pxTop.red > 5 && pxTop.ring > 300, JSON.stringify(pxTop));
  check("Top: the cube's middle is now TOP", JSON.stringify(await cubeHitAt(mid)) === "[0,0,1]", JSON.stringify(await cubeHitAt(mid)));
  check("a click on the cube never selects: no pick, no select request, the column still selected",
    (await gl()).picks === picks1 && selects.length === selects1 && JSON.stringify(await sel()) === JSON.stringify([col]),
    `picks ${picks1} -> ${(await gl()).picks}, selects ${selects1} -> ${selects.length}, selection ${JSON.stringify(await sel())}`);

  // A slowed clock (10 x: 3 s): many frames even on a loaded machine; the path is animated.
  const edge = await clickTurn(await cellAt([0, -1, 1]), 0.1);
  const ep = turnPath(edge, "pitch");
  console.log(`edge (slowed clock): ${ep.n} frames drawn, ${ep.between} in between, arrival at ${ep.arriveMs?.toFixed(0)} ms`);
  check("the turn is animated: a slowed turn (clock / 10) draws in-between angles, one way, then arrives at 3000 ms",
    ep.between >= 1 && ep.between === ep.mid && ep.oneWay && ep.arrived && ep.arriveMs >= 3000 && (ep.lastMidMs === null || ep.lastMidMs < 3000), JSON.stringify(ep));
  check("the front-top edge: 45° down from the front (yaw -90°, pitch 45°)", angEq(edge.end.yaw, -Math.PI / 2) && Math.abs(edge.end.pitch - Math.PI / 4) < 1e-3, fmt(edge.end));
  const front = await clickTurn(await cellAt([0, -1, 0]));
  check("Front: level, from the south (yaw -90°, pitch 0)", angEq(front.end.yaw, -Math.PI / 2) && Math.abs(front.end.pitch) < 1e-3, fmt(front.end));
  check("Front: the cube's middle is now FRONT", JSON.stringify(await cubeHitAt(mid)) === "[0,-1,0]", JSON.stringify(await cubeHitAt(mid)));
  const corner = await clickTurn(await cellAt([1, -1, 1]));
  check("a corner: isometric from the south-east (yaw -45°, pitch 35.26°)", angEq(corner.end.yaw, -Math.PI / 4) && Math.abs(corner.end.pitch - Math.asin(1 / Math.sqrt(3))) < 1e-3, fmt(corner.end));
  await shot("3-corner");
  const st = (await gl()).viewCube;
  check("four clicks on the cube, none selected anything", st.looks === 4 && JSON.stringify(st.lastLook) === "[1,-1,1]" && (await gl()).picks === picks1 &&
    selects.length === selects1 && JSON.stringify(await sel()) === JSON.stringify([col]), JSON.stringify(st));

  // ---- drags ----
  const d0 = await camera(), o0 = (await gl()).viewCube.orbits;
  // Short drags that stay on the cube (half size 23 px): beside it, the drag turns (viewcube.rs: where
  // the pointer is decides). The first 3 px are under the click distance; then 12 px are applied.
  await drag(mid, 15, 0);
  const d1 = await camera();
  check("a drag on the cube orbits (yaw by 15 px x 0.008, pitch kept), no select", Math.abs(d0.yaw - d1.yaw - 0.12) < 0.005 && Math.abs(d1.pitch - d0.pitch) < 1e-9 &&
    (await gl()).viewCube.orbits > o0 && (await gl()).picks === picks1 && selects.length === selects1, `${fmt(d0)} -> ${fmt(d1)}`);
  await drag(mid, 0, -15);
  const d2 = await camera();
  check("a vertical drag on the cube tilts (pitch by -15 px x 0.008)", Math.abs(d2.pitch - (d1.pitch - 0.12)) < 0.005 && Math.abs(d2.yaw - d1.yaw) < 1e-9 &&
    (await gl()).viewCube.turns === 0, `${fmt(d1)} -> ${fmt(d2)}`);
  const s = JSON.parse(await evalJs("JSON.stringify(window.__bbGl.cubeLetterAt('S'))"));
  check("the compass S is beside the cube (no target under it)", s !== null && (await cubeHitAt(s)) === null && (await hitEl(...s)) === "viewcube", JSON.stringify(s));
  const t0 = (await gl()).viewCube.turns;
  await drag(s, 40, 0, 8);
  const d3 = await camera();
  check("a drag on the compass ring turns about the vertical (40 px x 0.01, pitch kept)", (await gl()).viewCube.turns > t0 && Math.abs(d2.yaw - d3.yaw - 0.4) < 0.005 && Math.abs(d3.pitch - d2.pitch) < 1e-9,
    `${fmt(d2)} -> ${fmt(d3)}, turns ${t0} -> ${(await gl()).viewCube.turns}`);
  const z0 = (await camera()).distance;
  await mouse("mouseWheel", mid[0], mid[1], { deltaX: 0, deltaY: -300 });
  await settle();
  check("the wheel over the cube zooms the view", (await camera()).distance < 0.9 * z0, `${z0} -> ${(await camera()).distance}`);
  check("no drag or wheel on the cube selected anything", (await gl()).picks === picks1 && selects.length === selects1 && JSON.stringify(await sel()) === JSON.stringify([col]));

  // ---- Home and Fit all ----
  const home = await rectOf('[data-bb="view-home"]'), fitR = await rectOf('[data-bb="view-fit"]');
  check("Home and Fit all under the cube", home && fitR && home.top >= cr2.bottom && fitR.top >= home.bottom && Math.abs(home.left + home.width / 2 - (cr2.left + 60)) < 1,
    `${JSON.stringify(home)} ${JSON.stringify(fitR)}`);
  const h = await clickTurn([home.left + home.width / 2, home.top + home.height / 2]);
  // The first view was fitted on a canvas without the selection's panel: compare the target only.
  check("Home: yaw -55°, pitch 28°, the model fitted (the first view's target)", angEq(h.end.yaw, -55 * DEG) && Math.abs(h.end.pitch - 28 * DEG) < 1e-3 &&
    h.end.target.every((v, k) => Math.abs(v - cam0.target[k]) < 1e-3), `${fmt(h.end)}, target ${h.end.target} vs ${cam0.target}`);
  const fitD = h.end.distance;
  await mouse("mouseWheel", mid[0], mid[1], { deltaX: 0, deltaY: 400 });
  await drag(mid, 30, 0);
  const pre = await camera();
  await click([fitR.left + fitR.width / 2, fitR.top + fitR.height / 2]);
  await settle();
  const fa = await camera();
  check("Fit all: the fitted distance again, the angles kept", Math.abs(fa.distance - fitD) < 1e-6 * fitD && fa.yaw === pre.yaw && fa.pitch === pre.pitch && pre.distance !== fitD,
    `${pre.distance} -> ${fa.distance} (fit ${fitD})`);
  check("Home and Fit all selected nothing", (await gl()).picks === picks1 && selects.length === selects1);

  // ---- a plan: no cube, no Home ----
  const ov = await B("return b.openViews.value;");
  const plan1 = ov.views.find((v) => v.type === "plan" && v.look === "down");
  const e1 = await B(`return b.openView('${plan1.id}');`);
  if (e1) throw new Error(`openView: ${e1}`);
  await waitFor("the plan camera", 5000, async () => (await camera()).mode === "plan");
  await settle();
  const cp = await camera(), vcP = (await gl()).viewCube, crP = await rectOf('[data-bb="viewcube"]');
  check("plan: the cube is off (not shown, no size)", vcP.shown === false && crP.width === 0 && crP.height === 0, JSON.stringify({ vcP, crP }));
  const homeP = await rectOf('[data-bb="view-home"]'), fitP = await rectOf('[data-bb="view-fit"]');
  check("plan: Home off, Fit all on", homeP.width === 0 && fitP.width > 0, `${JSON.stringify(homeP)} ${JSON.stringify(fitP)}`);
  const vpP = await rectOf("#bimble"), where = [vpP.right - 8 - 60, vpP.top + 8 + 60 + 20]; // under where the cube was, below Fit all
  check("plan: the pointer where the cube was reaches the 3D canvas", (await hitEl(...where)) === "bimble", String(await hitEl(...where)));
  await click(where);
  await sleep(300);
  const cp2 = await camera();
  check("plan: a click there turns nothing (still the plan, yaw and pitch kept)", cp2.mode === "plan" && cp2.yaw === cp.yaw && cp2.pitch === cp.pitch && !cp2.turning, fmt(cp2));
  await mouse("mouseWheel", vpP.left + vpP.width / 2, vpP.top + vpP.height / 2, { deltaX: 0, deltaY: -300 });
  await settle();
  const zp = await camera();
  const fitP2 = await rectOf('[data-bb="view-fit"]');
  await click([fitP2.left + fitP2.width / 2, fitP2.top + fitP2.height / 2]);
  await settle();
  const fp = await camera();
  check("plan: Fit all fits the plan again", zp.half < 0.9 * cp.half && Math.abs(fp.half - cp2.half) < 1e-6 * cp2.half, `half ${cp.half} -> ${zp.half} -> ${fp.half}`);

  // ---- back to 3D ----
  const e3 = await B(`return b.tabCall('activate', { type: 'view', id: '${ov.views[0].id}' });`);
  if (e3) throw new Error(`activate 3D: ${e3}`);
  await waitFor("the 3D camera", 5000, async () => (await camera()).mode === "3d");
  await settle();
  await shot("4-plan-back-3d");
  const vc3 = (await gl()).viewCube, cr3 = await rectOf('[data-bb="viewcube"]');
  check("3D again: the cube is back", vc3.shown === true && Math.abs(cr3.width - 120) < 0.5, JSON.stringify(cr3));

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
console.log(`lab_viewcube: ${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
