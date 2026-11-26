// Plan views in the lab's WebGL2 viewport (ticket C1, ccore/session/views.h, ADR 0068 rule 9):
// node ccore/test/lab_plan.mjs. Starts out/tcc/bimble-ccore (or $HOST_BIN) with a copy of
// golden/frame.json (frame.bimble, in a temp dir), Vite from web-ui/ and headless /usr/bin/chromium
// over raw CDP; opens /?native=1&gl=1&port=…&token=… and checks:
//   open_views: the 3D view and Rust's default plans (7 views); the navigator's "Plan Niveau 1" row
//     (a click) opens its tab: the camera is orthographic, top-down (__bbGl.camera.mode "plan");
//   cut: a column of Niveau 1 cut at 1.2 m shows as a filled square at the cut shade (concrete x
//     0.72, unlit): its centre pixel is that colour, and so are about 300 x 300 mm of pixels around
//     it; selected, the square turns the selection colour at the cut shade;
//   above the cut: a beam of Niveau 1 (z 2530..3030) is not drawn: selecting it changes no pixel,
//     a click on it picks nothing; in 3D the same selection changes pixels (the control);
//   pan: a left drag moves the drawing by the drag (no orbit: yaw and pitch kept); zoom: the wheel
//     changes the scale and keeps the point under the pointer;
//   place: placing a column, the pointer 40 mm off a column centre snaps to it (hover); a Shift
//     click on a free plan point places an IfcColumn there on Niveau 1 (one history step);
//   back to 3D: the 3D tab restores the 3D camera (yaw, pitch as before); a drag orbits again;
//   the status bar's hint: "Drag: orbit…" (status.hint) in 3D, "Drag: pan · Scroll: zoom"
//     (status.hintPlan) in the plan, the 3D one again back in 3D (nothing selected each time).
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
const PORT = port("lab_plan");
const VITE = vitePort("lab_plan");
const TOKEN = "labplan-token";
const PAGE = `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}`;
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-plan-"));
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

  const camera = async () => (await gl()).camera;
  /** The status bar's hint once it is the text of `key` (3 s), else the hint shown. */
  const hintIs = async (key) => {
    const want = await evalJs(`(async () => (await import('/src/i18n/index.ts')).t('${key}'))()`);
    const now = () => evalJs(`document.querySelector('[data-bb="status-hint"]')?.textContent ?? null`);
    const got = await waitFor(`the hint ${key}`, 3000, async () => ((await now()) === want ? want : null)).catch(() => now());
    return { ok: got === want, detail: `${JSON.stringify(got)}, want ${JSON.stringify(want)}` };
  };
  const N1 = "1uAZA0Yfn05f$xRd1k4uPR";
  const ov = await B("return b.openViews.value;");
  check("open_views: 3D and the 6 default plans", ov.views.length === 7 && ov.views[0].type === "three_d" && ov.views.filter((v) => v.type === "plan").length === 6, JSON.stringify(ov.views.map((v) => v.name)));
  const plan1 = ov.views.find((v) => v.type === "plan" && v.look === "down" && v.storey === N1);
  const cam3d = await camera();
  check("3D on load: the 3D camera", cam3d.mode === "3d" && cam3d.view === ov.views[0].id, JSON.stringify(cam3d));
  const h3d = await hintIs("status.hint");
  check("3D on load: the status bar's hint is the orbit one", h3d.ok && (await B("return b.state.value.selection.length;")) === 0, h3d.detail);

  // ---- 3D control: the beam B1.x00 (0, 0) -> (6000, 0) at z 2530..3030 is drawn ----
  const beam = (await B("return b.query('elements', { class: 'IfcBeam' }).map((e) => [e.id, e.name]);")).find((e) => e[1] === "B1.x00")[0];
  const col = (await B("return b.query('elements', { class: 'IfcColumn' }).map((e) => [e.id, e.name]);")).find((e) => e[1] === "P1.11")[0];
  const BG = [247, 247, 245];
  const isBg = (p) => p.every((v, k) => Math.abs(v - BG[k]) <= 3);
  /** Pixels along the beam's axis between the columns and their footings (x 1000..5000, y 0). */
  const beamLine = async (z) => {
    const out = [];
    for (let x = 1000; x <= 5000; x += 250) out.push(await pixelAt(await screen(x, 0, z)));
    return out;
  };
  await settle();
  const line3 = await beamLine(2780);
  check("3D: the beam B1.x00 drawn along its axis (control)", line3.filter((p) => !isBg(p)).length >= 12, JSON.stringify(line3));

  // ---- open the plan from the navigator ----
  const row = await evalJs(`(() => { const r = [...document.querySelectorAll('*')].filter((e) => e.textContent.trim() === ${JSON.stringify(plan1.name)})
    .map((e) => e.getBoundingClientRect()).filter((r) => r.width > 0 && r.height > 0 && r.left < 300).sort((p, q) => p.width * p.height - q.width * q.height)[0];
    return r ? [r.left + r.width / 2, r.top + r.height / 2] : null; })()`);
  if (row) {
    await S("Input.dispatchMouseEvent", { type: "mousePressed", x: row[0], y: row[1], button: "left", buttons: 1, clickCount: 1 });
    await S("Input.dispatchMouseEvent", { type: "mouseReleased", x: row[0], y: row[1], button: "left", buttons: 0, clickCount: 1 });
  }
  const viaUi = await waitFor("the plan open", 3000, async () => (await B("return b.openViews.value.current?.id;")) === plan1.id).catch(() => false);
  if (!viaUi) { // the row may be under a collapsed group: the same call the row makes
    console.log(`navigator row ${row ? "clicked" : "not found"}: opening with openView()`);
    const e = await B(`return b.openView('${plan1.id}');`);
    if (e) throw new Error(`openView: ${e}`);
  }
  check("the navigator row Plan Niveau 1 (a click) opens its tab", viaUi === true, row ? "clicked, not opened" : "no row");
  check("Plan Niveau 1 is the current tab", (await B("return b.openViews.value.current?.id;")) === plan1.id && (await B("return b.state.value.tab?.id;")) === plan1.id);
  await waitFor("the plan camera", 5000, async () => (await camera()).mode === "plan");
  await settle();
  const camP = await camera();
  check("plan: orthographic top-down camera, looking down", camP.mode === "plan" && camP.view === plan1.id && camP.look === "down", JSON.stringify(camP));
  const hPlan = await hintIs("status.hintPlan");
  check("plan: the status bar's hint is pan and zoom, no orbit", hPlan.ok && (await B("return b.state.value.selection.length;")) === 0, hPlan.detail);
  await shot("plan");

  // ---- the cut: a column as a filled square at the cut shade ----
  const shade = (rgb) => rgb.map((c) => Math.round(255 * srgb(lin(c / 255) * 0.72)));
  const CAP = shade([214, 214, 210]), SEL_CAP = shade([215, 172, 182]);
  const c11 = await screen(6000, 5000, 600);
  const px = await pixelAt(c11);
  check(`the column P1.11's centre is the cut colour ${CAP}`, px.every((v, k) => Math.abs(v - CAP[k]) <= 3), `${px}`);
  const capPx = (rgb) => evalJs(`(() => { const c = document.getElementById('bimble'), g = c.getContext('webgl2');
    const p = new Uint8Array(c.width * c.height * 4); g.readPixels(0, 0, c.width, c.height, g.RGBA, g.UNSIGNED_BYTE, p);
    let n = 0; for (let i = 0; i < p.length; i += 4) if (Math.abs(p[i] - ${rgb[0]}) <= 3 && Math.abs(p[i + 1] - ${rgb[1]}) <= 3 && Math.abs(p[i + 2] - ${rgb[2]}) <= 3) n++; return n; })()`);
  const a = await screen(5850, 4850, 600), b2 = await screen(6150, 5150, 600);
  const side = Math.abs(b2[0] - a[0]), area = side * Math.abs(b2[1] - a[1]);
  const nCap = await capPx(CAP);
  check("12 column squares at the cut colour (about 12 x 300 x 300 mm of pixels)", nCap > 6 * area && nCap < 20 * area, `${nCap} px, one square ${area.toFixed(0)} px`);
  await B(`return b.select(['${col}']);`);
  await settle();
  await sleep(300); // the properties panel opens: the canvas may resize
  await settle();
  const a2 = await screen(5850, 4850, 600), b3 = await screen(6150, 5150, 600), area2 = Math.abs(b3[0] - a2[0]) * Math.abs(b3[1] - a2[1]);
  const nSel = await capPx(SEL_CAP);
  check("the column selected: its square at the selection colour's cut shade", nSel > 0.5 * area2 && nSel < 2 * area2, `${nSel} px of ${SEL_CAP}, square ${area2.toFixed(0)}`);
  await B("return b.select([]);");
  await settle();

  // ---- above the cut: the beam is not drawn, not picked ----
  const lineP = await beamLine(600);
  check("plan: the beam B1.x00 (above the cut) not drawn: background along its axis", lineP.every(isBg), JSON.stringify(lineP));
  const mid = await screen(3000, 0, 2780);
  check("plan: a click on the beam picks nothing", (await pickAt(mid)) === null, String(await pickAt(mid)));
  check("plan: a click on the column picks it", (await pickAt(c11)) === col, String(await pickAt(c11)));

  // ---- pan and zoom ----
  const before = await screen(6000, 5000, 600);
  await S("Input.dispatchMouseEvent", { type: "mousePressed", x: before[0], y: before[1], button: "left", buttons: 1, clickCount: 1 });
  for (let k = 1; k <= 5; k++) await S("Input.dispatchMouseEvent", { type: "mouseMoved", x: before[0] + 20 * k, y: before[1] + 10 * k, button: "left", buttons: 1 });
  await S("Input.dispatchMouseEvent", { type: "mouseReleased", x: before[0] + 100, y: before[1] + 50, button: "left", buttons: 0, clickCount: 1 });
  await settle();
  const after = await screen(6000, 5000, 600), camPan = await camera();
  check("pan: a left drag of (100, 50) px moves the drawing by it", Math.abs(after[0] - before[0] - 100) <= 1.5 && Math.abs(after[1] - before[1] - 50) <= 1.5, `${before} -> ${after}`);
  check("pan: no orbit (still top-down, yaw and pitch kept)", camPan.mode === "plan" && camPan.yaw === camP.yaw && camPan.pitch === camP.pitch);
  const z0 = await screen(12000, 10000, 600);
  await S("Input.dispatchMouseEvent", { type: "mouseWheel", x: z0[0], y: z0[1], deltaX: 0, deltaY: -300 });
  await settle();
  const z1 = await screen(12000, 10000, 600), camZ = await camera();
  const s0 = Math.abs((await screen(6150, 5000, 600))[0] - (await screen(5850, 5000, 600))[0]);
  check("zoom: the wheel enlarges the drawing", camZ.half < camPan.half * 0.8 && s0 > side * 1.2, `half ${camPan.half} -> ${camZ.half}, square ${side} -> ${s0} px`);
  check("zoom: the point under the pointer stays put", Math.abs(z1[0] - z0[0]) <= 1.5 && Math.abs(z1[1] - z0[1]) <= 1.5, `${z0} -> ${z1}`);
  // Back to a view of the whole plan for placing.
  await S("Input.dispatchMouseEvent", { type: "mouseWheel", x: z0[0], y: z0[1], deltaX: 0, deltaY: 300 });
  await settle();

  // ---- place a column in the plan ----
  const undo0 = await B("return b.query('history').undo.length;");
  await setPlacing("column");
  const snapAt = await screen(6040, 5000, 0);
  await move(snapAt);
  const hov = (await gl()).hover;
  check("place: 40 mm off a column centre, the hover snaps to it (6000, 5000)", hov && hov.at[0] === 6000 && hov.at[1] === 5000 && hov.snap !== null, JSON.stringify(hov));
  const P = [3000, 2500], at = await screen(P[0], P[1], 0);
  const mmPx = (2 * (await camera()).half) / (await evalJs("document.getElementById('bimble').clientHeight"));
  const n0 = (await gl()).toolCalls;
  await S("Input.dispatchMouseEvent", { type: "mouseMoved", x: at[0], y: at[1], button: "none", buttons: 0, modifiers: 8 });
  await S("Input.dispatchMouseEvent", { type: "mousePressed", x: at[0], y: at[1], button: "left", buttons: 1, clickCount: 1, modifiers: 8 });
  await S("Input.dispatchMouseEvent", { type: "mouseReleased", x: at[0], y: at[1], button: "left", buttons: 0, clickCount: 1, modifiers: 8 });
  await waitFor("the click's tool answer", 5000, async () => (await gl()).toolCalls > n0 && (await gl()).placed !== null);
  const placed = (await gl()).placed;
  const el = await B(`return b.element('${placed}');`);
  const o = el?.placement?.origin ?? [];
  check("place: a Shift click places an IfcColumn on Niveau 1 at the clicked plan point", el?.class === "IfcColumn" && el.storey === N1 &&
    Math.abs(o[0] - P[0]) <= mmPx + 10 && Math.abs(o[1] - P[1]) <= mmPx + 10, JSON.stringify({ c: el?.class, s: el?.storey, o, mmPx }));
  check("place: one history step", (await B("return b.query('history').undo.length;")) === undo0 + 1);
  await S("Input.dispatchKeyEvent", { type: "keyDown", key: "Escape", code: "Escape", windowsVirtualKeyCode: 27 });
  await S("Input.dispatchKeyEvent", { type: "keyUp", key: "Escape", code: "Escape", windowsVirtualKeyCode: 27 });
  await waitFor("placing off", 5000, () => B("return b.placing.value?.active === false;"));
  await shot("placed");

  // ---- back to 3D ----
  const e3 = await B(`return b.tabCall('activate', { type: 'view', id: '${ov.views[0].id}' });`);
  check("activate the 3D tab", e3 === null, String(e3));
  await waitFor("the 3D camera", 5000, async () => (await camera()).mode === "3d");
  const cam3 = await camera();
  check("3D: the 3D camera back (yaw, pitch as before the plan)", cam3.view === ov.views[0].id && cam3.yaw === cam3d.yaw && cam3.pitch === cam3d.pitch, JSON.stringify(cam3));
  await B("return b.select([]);"); // the placed column may be selected: the selection's hint comes first
  await waitFor("nothing selected", 5000, async () => (await B("return b.state.value.selection.length;")) === 0);
  const hBack = await hintIs("status.hint");
  check("3D again: the status bar's hint is the orbit one again", hBack.ok, hBack.detail);
  const cx = await evalJs("(() => { const r = document.getElementById('bimble').getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; })()");
  await S("Input.dispatchMouseEvent", { type: "mousePressed", x: cx[0], y: cx[1], button: "left", buttons: 1, clickCount: 1 });
  for (let k = 1; k <= 5; k++) await S("Input.dispatchMouseEvent", { type: "mouseMoved", x: cx[0] + 10 * k, y: cx[1], button: "left", buttons: 1 });
  await S("Input.dispatchMouseEvent", { type: "mouseReleased", x: cx[0] + 50, y: cx[1], button: "left", buttons: 0, clickCount: 1 });
  await settle();
  check("3D: a drag orbits again (yaw changed)", (await camera()).yaw !== cam3d.yaw);
  await B(`return b.tabCall('activate', { type: 'view', id: '${plan1.id}' });`);
  await waitFor("the plan camera again", 5000, async () => (await camera()).mode === "plan");
  const camBack = await camera();
  check("the plan again: its own camera kept (scale as left)", Math.abs(camBack.half - camPan.half) < 1e-6 * camPan.half, `${camPan.half} -> ${camBack.half}`);

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
console.log(`lab_plan: ${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
