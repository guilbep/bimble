// The place tool in the lab's WebGL2 viewport (ticket B4, ADR 0068 rule 2): node ccore/test/lab_tool.mjs
// Starts out/tcc/bimble-ccore (or $HOST_BIN) with a copy of golden/frame.json (frame.bimble, in a
// temp dir), Vite from web-ui/ (needs web-ui/node_modules) and headless /usr/bin/chromium over raw
// CDP; opens /?native=1&gl=1&port=…&token=… and, with real mouse and key events on the canvas:
//   column: set_placing column; the pointer 40 mm off a column centre -> the tool's hover snaps to the
//     axis crossing (6000, 5000), a ghost is drawn (ghost triangles, translucent blue pixels); on a
//     free spot -> the hover follows (rounded to 10 mm); a click there -> one new IfcColumn at the
//     hover point on the placing storey, rev + 1, one history step ("add P…").
//   beam: two clicks -> nothing after the first (one point in `placing`), a ghost while the pointer
//     moves to the second, one IfcBeam after it, one history step for the gesture.
//   Esc: a wall's first click, then Escape -> placing off, no point, no element, no history step.
//   undo: one undo takes the beam back (not in `elements` of class IfcBeam).
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
const PORT = port("lab_tool", 47324);
const VITE = vitePort("lab_tool", 5209);
const TOKEN = "labtool-token";
const PAGE = `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}`;
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-tool-"));
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
      if (r.method === "POST" && path.startsWith("/tool/")) toolPosts.push(path);
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

  // ---- column ----
  const blue0 = await bluePx();
  await setPlacing("column");
  await move(await screen(6037, 4958));
  let g = await gl();
  check("column: 40 mm off a column centre, the hover snaps to the axis crossing (6000, 5000)",
    g.hover && g.hover.snap === "axis crossing" && g.hover.at[0] === 6000 && g.hover.at[1] === 5000 && g.hover.z === 0, JSON.stringify(g.hover));
  check("column: a ghost (triangles from the C core)", g.ghostTris === 12, `${g.ghostTris} triangles`);
  await move(await screen(3000, 2530));
  g = await gl();
  const blue1 = await bluePx();
  check("the ghost is drawn translucent blue (on a free spot)", blue1 > blue0 + 50, `${blue0} -> ${blue1} blue px`);
  check("column: on a free spot the hover follows the pointer (10 mm rounded)",
    g.hover && g.hover.snap === null && near(g.hover.at, [3000, 2530], 40) && g.hover.at.every((v) => v % 10 === 0), JSON.stringify(g.hover));
  const at = g.hover.at;
  const r0 = await rev(), u0 = await undoCount();
  await click(await screen(3000, 2530));
  await waitFor("rev + 1 after the column", 5000, async () => (await rev()) === r0 + 1);
  g = await gl();
  const col = g.placed ? await B(`return b.element('${g.placed}');`) : null;
  check("column: one click adds one IfcColumn at the hover point on the placing storey",
    col && col.class === "IfcColumn" && col.placement.origin[0] === at[0] && col.placement.origin[1] === at[1] && col.storey_name === "Niveau 1",
    JSON.stringify(col && { class: col.class, origin: col.placement.origin, storey: col.storey_name }));
  const hist = await B("return b.query('history').undo;");
  check("column: one history step, labelled as the verb's", hist.length === u0 + 1 && /^add P/.test(hist[0]), JSON.stringify(hist));

  // ---- beam: two clicks, one step ----
  await setPlacing("beam");
  const r1 = await rev(), u1 = await undoCount();
  await move(await screen(1000, 7500));
  g = await gl();
  check("beam: no point yet, no ghost", g.ghostTris === 0 && g.hover !== null, `${g.ghostTris} triangles`);
  await click(await screen(1000, 7500));
  await waitFor("one point in placing", 5000, () => B("return b.placing.value?.points.length === 1;"));
  check("beam: the first click adds nothing", (await rev()) === r1 && (await undoCount()) === u1);
  await move(await screen(9000, 7500));
  g = await gl();
  check("beam: a ghost follows the pointer to the second point", g.ghostTris === 12, `${g.ghostTris} triangles`);
  await click(await screen(9000, 7500));
  await waitFor("rev + 1 after the beam", 5000, async () => (await rev()) === r1 + 1);
  g = await gl();
  const beam = g.placed ? await B(`return b.element('${g.placed}');`) : null;
  check("beam: the second click adds one IfcBeam",
    beam && beam.class === "IfcBeam" && near(beam.placement.origin, [1000, 7500], 40), JSON.stringify(beam && { class: beam.class, origin: beam.placement.origin }));
  check("beam: one history step for the gesture", (await undoCount()) === u1 + 1, `${u1} -> ${await undoCount()}`);
  check("still placing a beam, points cleared", await B("return b.placing.value?.active === true && b.placing.value.points.length === 0;"));

  // ---- Esc ----
  await setPlacing("wall");
  const r2 = await rev(), u2 = await undoCount();
  await move(await screen(1000, 8500));
  await click(await screen(1000, 8500));
  await waitFor("one wall point", 5000, () => B("return b.placing.value?.points.length === 1;"));
  await S("Input.dispatchKeyEvent", { type: "keyDown", key: "Escape", code: "Escape", windowsVirtualKeyCode: 27 });
  await S("Input.dispatchKeyEvent", { type: "keyUp", key: "Escape", code: "Escape", windowsVirtualKeyCode: 27 });
  await waitFor("placing off", 5000, () => B("return b.placing.value?.active === false;"));
  g = await gl();
  check("Esc: placing off, no point, no element, no step, the ghost gone",
    (await rev()) === r2 && (await undoCount()) === u2 && g.ghostTris === 0 && (await B("return b.placing.value.points.length;")) === 0,
    `rev ${r2} -> ${await rev()}, ghost ${g.ghostTris}`);
  const blue2 = await bluePx();
  check("no ghost pixels after Esc", blue2 <= blue0 + 10, `${blue2} blue px`);

  // ---- undo ----
  await B("return b.undo();");
  await waitFor("rev + 1 after undo", 5000, async () => (await rev()) === r2 + 1);
  const beams = await B("return b.query('elements', { class: 'IfcBeam' }).map((e) => e.id);");
  check("one undo takes the beam back (the frame's 34 beams left)", (await undoCount()) === u2 - 1 && beam && beams.length === 34 && !beams.includes(beam.id), `${beams.length} beams`);
  console.log(`tool calls posted: ${toolPosts.length} (${[...new Set(toolPosts)].join(", ")})`);

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
