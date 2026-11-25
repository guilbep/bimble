// The section box, hide and isolate in the lab's WebGL2 viewport (ticket B3, ADR 0014, ADR 0068 rule 2):
// node ccore/test/lab_section.mjs. Starts out/tcc/bimble-ccore (or $HOST_BIN) with a copy of
// golden/frame.json (frame.bimble, in a temp dir), Vite from web-ui/ and headless /usr/bin/chromium
// over raw CDP; opens /?native=1&gl=1&port=…&token=… and, with real mouse events:
//   pick: a click on a column's top selects it (box off);
//   ribbon: the View tab's "Section box" button turns the box on (pressed; fitted to the selected
//     column), Box to selection with nothing selected fits everything: about the same pixels;
//   drag: the top handle dragged down to the column's mid-height (box_press / box_move /
//     box_release, the face follows in C) -> the box's top there, fewer model pixels, cut-surface
//     pixels (the material colour at the cut shade 0.72, unlit) appear; no history step, rev unchanged;
//   pick on the clipped part: the click on the column's former top no longer selects it;
//   off: the ribbon button again -> the pixels back;
//   hide: hide_now on the column -> one element not drawn, fewer pixels, a click on it does not
//     select it; isolate_now -> only it drawn; clear_temporary -> back.
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
const PORT = port("lab_section", 47395);
const VITE = vitePort("lab_section", 5211);
const TOKEN = "labsection-token";
const PAGE = `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}`;
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-section-"));
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
  const ribbonBox = async () => {
    const xy = await evalJs(`(async () => { const { t } = await import('/src/i18n/index.ts'); const label = t('ribbon.view.g2.t1');
      const el = [...document.querySelectorAll('.tool')].find((e) => e.textContent.trim() === label); if (!el) return null;
      const r = el.getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; })()`);
    if (!xy) throw new Error("no Section box button in the ribbon");
    await S("Input.dispatchMouseEvent", { type: "mousePressed", x: xy[0], y: xy[1], button: "left", buttons: 1, clickCount: 1 });
    await S("Input.dispatchMouseEvent", { type: "mouseReleased", x: xy[0], y: xy[1], button: "left", buttons: 0, clickCount: 1 });
  };
  const rev0 = await rev(), undo0 = await undoCount();

  // ---- a column, its top, a pick with the box off ----
  const cols = await B("return b.query('elements', { class: 'IfcColumn' }).map((e) => e.id);");
  // A column the camera sees: its point at 80 % height picks it (the box off).
  let col = null, colBox = null;
  for (const id of cols) {
    const e = await B(`return b.element('${id}');`);
    const xy = await screen(e.placement.origin[0], e.placement.origin[1], e.placement.origin[2] + 0.8 * (e.generator_params.height ?? 3000)).catch(() => null);
    if (xy && (await pickAt(xy)) === id) { col = id; break; }
  }
  if (!col) throw new Error("no column the camera sees");
  await B(`return b.select(['${col}']);`);
  await waitFor("the column selected", 5000, async () => (await sel())[0] === col);
  await ribbonBox();
  await waitFor("the box on", 5000, async () => (await vis()).section_box.on === true);
  let v = await vis();
  const pressed = await B("return b.actions.value.find((a) => a.name === 'section_box')?.pressed;");
  check("ribbon Section box: on, pressed, fitted to the selected column", v.section_box.on && pressed === true && v.section_box.box !== null, JSON.stringify(v.section_box));
  const o = v.section_box.origin_mm, cb = v.section_box.box;
  const cx = (cb.min[0] + cb.max[0]) / 2, cy = (cb.min[1] + cb.max[1]) / 2, ztop = cb.min[2] + 0.8 * (cb.max[2] - cb.min[2]), zmid = (cb.min[2] + cb.max[2]) / 2;
  const W = (x, y, z) => [x + o[0], y + o[1], z + o[2]]; // scene -> world mm
  await ribbonBox();
  await waitFor("the box off", 5000, async () => (await vis()).section_box.on === false);
  // Nothing selected for every measure (a selection opens the modifier panel: the canvas changes size).
  await B("return b.select([]);");
  await waitFor("nothing selected", 5000, async () => (await sel()).length === 0);
  await settle();
  await sleep(300);
  await settle();
  const topXY = await screen(...W(cx, cy, ztop)), midXY = await screen(...W(cx, cy, zmid));
  const px0 = await modelPx(), cut0 = await cutPx();
  await keepPx("off");
  await shot("0-off");
  await mouseClick(topXY);
  check("box off: a click on the column at 80 % height selects it", (await sel())[0] === col, JSON.stringify(await sel()));
  await B("return b.select([]);");
  await waitFor("nothing selected", 5000, async () => (await sel()).length === 0);
  await settle();

  // ---- on again, fitted to everything ----
  await ribbonBox();
  await waitFor("the box on", 5000, async () => (await vis()).section_box.on === true);
  await B("return b.runAction('box_to_selection');");
  await waitFor("the box around everything", 5000, async () => (await vis()).section_box.box.max[2] > cb.max[2] + 100);
  await settle();
  const px1 = await modelPx(), cut1 = await cutPx();
  const capXY = await screen(...W(cx, cy, zmid)), cap1 = await pixelAt(capXY);
  await shot("1-all");
  const changed1 = await diffPx("off");
  check("box around everything: the picture unchanged but the box drawn over it (< 1 % of the model's pixels differ)",
    changed1 >= 0 && changed1 < 0.01 * px0, `${changed1} px differ (box colour left out), model px ${px0} -> ${px1}`);

  // ---- drag the top handle down to the column's mid-height ----
  const all = (await vis()).section_box.box;
  const hx = (all.min[0] + all.max[0]) / 2, hy = (all.min[1] + all.max[1]) / 2;
  const handle = await screen(...W(hx, hy, all.max[2]));
  const to = await screen(...W(hx, hy, zmid));
  const c0 = (await gl()).boxCalls;
  await S("Input.dispatchMouseEvent", { type: "mousePressed", x: handle[0], y: handle[1], button: "left", buttons: 1, clickCount: 1 });
  let g = await gl();
  check("press on the top handle: the box tool holds face 5", g.boxFace === 5 && g.boxCalls === c0 + 1, `face ${g.boxFace}, ${g.boxCalls - c0} calls ${g.boxError ?? ""}`);
  for (let i = 1; i <= 8; i++) {
    const f0 = await frames();
    await S("Input.dispatchMouseEvent", { type: "mouseMoved", x: handle[0] + ((to[0] - handle[0]) * i) / 8, y: handle[1] + ((to[1] - handle[1]) * i) / 8, button: "left", buttons: 1 });
    await waitFor("a frame", 5000, async () => (await frames()) > f0);
  }
  g = await gl();
  const during = (await vis()).section_box.box;
  check("while dragging: the preview moves, the session's box does not",
    g.clip && Math.abs(g.clip.max[2] - zmid) < 60 && during.max[2] === all.max[2], `preview ${g.clip && g.clip.max[2]}, session ${during.max[2]}, want ${zmid}`);
  await S("Input.dispatchMouseEvent", { type: "mouseReleased", x: to[0], y: to[1], button: "left", buttons: 0, clickCount: 1 });
  await waitFor("the box released", 5000, async () => (await vis()).section_box.box.max[2] < all.max[2] - 100);
  await settle();
  v = await vis();
  check("release: the box's top at the column's mid-height (one session change)", Math.abs(v.section_box.box.max[2] - zmid) < 60, `${v.section_box.box.max[2]} vs ${zmid}`);
  check("the drag made no history step and kept the rev", (await rev()) === rev0 && (await undoCount()) === undo0, `rev ${rev0} -> ${await rev()}`);
  const px2 = await modelPx(), cut2 = await cutPx();
  await shot("2-cut");
  check("cut at mid-height: fewer model pixels", px2 < 0.9 * px1, `${px1} -> ${px2}`);
  const cap2 = await pixelAt(capXY), CUT = CUTS[0], same = (p, k) => p.every((x, i) => Math.abs(x - k[i]) <= 1);
  check("cut at mid-height: the column's cut face is filled in the cut colour (concrete at the cut shade 0.72, unlit)",
    same(cap2, CUT) && !same(cap1, CUT), `cut-face pixel ${JSON.stringify(cap1)} -> ${JSON.stringify(cap2)}, cut colour ${JSON.stringify(CUT)}`);
  console.log(`pixels: box off ${px0}, around everything ${px1}, cut at mid-height ${px2}; cut-colour px ${cut0} / ${cut1} / ${cut2}`);

  // ---- pick on the clipped part ----
  await mouseClick(topXY);
  check("a click where the column's upper part was: the column is not picked (the box removed it)", !(await sel()).includes(col), JSON.stringify(await sel()));
  await B("return b.select([]);");

  // ---- off from the ribbon ----
  await ribbonBox();
  await waitFor("the box off", 5000, async () => (await vis()).section_box.on === false);
  await settle();
  const px3 = await modelPx();
  check("ribbon Section box again: off, the pixels back", Math.abs(px3 - px0) < 0.01 * px0, `${px0} -> ${px3}`);

  // ---- hide and isolate ----
  await B(`return b.select(['${col}']);`);
  await waitFor("the column selected", 5000, async () => (await sel())[0] === col);
  check("hide_now -> no error", (await B("return b.runAction('hide_now');")) === null);
  await waitFor("one hidden for now", 5000, async () => (await vis()).hidden_now.count === 1);
  await settle();
  g = await gl();
  const px4 = await modelPx();
  check("hide_now: the column is not drawn (1 element left out)", g.hiddenElements === 1 && px4 < px0, `${g.hiddenElements} hidden, ${px0} -> ${px4} px`);
  await mouseClick(midXY);
  check("a click where the hidden column is does not select it", !(await sel()).includes(col), JSON.stringify(await sel()));
  await B(`return b.select(['${col}']);`);
  await waitFor("the column selected", 5000, async () => (await sel())[0] === col);
  await B("return b.runAction('isolate_now');");
  await waitFor("isolated for now", 5000, async () => (await vis()).isolated_now.count === 1);
  await settle();
  g = await gl();
  const px5 = await modelPx(), n = await B("return b.query('elements', { limit: 100000 }).length;");
  check("isolate_now: only the column drawn", g.hiddenElements === n - 1 && px5 < 0.25 * px0, `${g.hiddenElements} of ${n} hidden, ${px5} px`);
  await B("return b.runAction('clear_temporary');");
  await waitFor("nothing hidden", 5000, async () => (await vis()).isolated_now.count === 0);
  await settle();
  g = await gl();
  check("clear_temporary: everything drawn again", g.hiddenElements === 0 && Math.abs((await modelPx()) - px0) < 0.01 * px0, `${g.hiddenElements} hidden`);
  console.log(`box tool calls posted: ${toolPosts.length} (${[...new Set(toolPosts)].join(", ")})`);

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
console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
