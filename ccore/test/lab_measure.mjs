// The measure tool in the lab (ticket T1, ccore/session/measure.h, web-ui/src/gl/measure.ts):
// node ccore/test/lab_measure.mjs. Starts out/tcc/bimble-ccore (or $HOST_BIN) with a copy of
// golden/frame.json, Vite from web-ui/ and headless /usr/bin/chromium over raw CDP; opens
// /?native=1&gl=1&port=…&token=… and, with real mouse events:
//   3D: action measure (pressed), the pointer near the foot corner (-150, -150, 0) of column P1.00
//     snaps to it (a corner), a click; near the foot corner (5850, -150, 0) of P1.10, a click: one
//     measurement, "6 000 mm" (thin space) drawn by the overlay, `measurements` has 1 row
//     ("1. 6 000 mm"), rev and the history unchanged; clear: 0 rows;
//   Plan Niveau 1: the same bay between the two corners, seen from above: 6 000 mm, 1 row;
//   Esc drops the first point, a second Esc leaves the tool (not pressed); the rev never changed.
// No uncaught page error; no console error but the sync-XHR deprecation and the favicon 404.
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
const PORT = port("lab_measure", 47428);
const VITE = vitePort("lab_measure", 5192);
const TOKEN = "labmeasure-token";
const PAGE = `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}`;
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-measure-"));
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
  const chrome = start("chromium", "/usr/bin/chromium", [
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
      if (r.method === "POST" && path.startsWith("/tool/measure_")) toolPosts.push(path);
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
  const settle = async () => { const f0 = await frames(); await waitFor("a new frame", 5000, async () => (await frames()) > f0).catch(() => {}); await sleep(100); };
  const shot = async (name) => { if (!process.env.SHOTS) return; const { data } = await S("Page.captureScreenshot", { format: "png" }); (await import("node:fs")).writeFileSync(join(process.env.SHOTS, `${name}.png`), Buffer.from(data, "base64")); };
  const PLAN_N1 = "3w8zGYnP$Ay1lyvMZjDnXq";
  const THIN = "\u2009";
  const rev0 = await rev(), undo0 = await undoCount();
  const mCalls = async () => (await gl()).measureCalls;
  const texts = async () => (await gl()).overlayTexts;
  const mMove = async ([x, y]) => {
    const n0 = await mCalls();
    await S("Input.dispatchMouseEvent", { type: "mouseMoved", x, y, button: "none", buttons: 0 });
    await waitFor("a measure answer", 5000, async () => (await mCalls()) > n0);
  };
  const mClick = async ([x, y]) => {
    await mMove([x, y]);
    const n0 = await mCalls();
    await S("Input.dispatchMouseEvent", { type: "mousePressed", x, y, button: "left", buttons: 1, clickCount: 1 });
    await S("Input.dispatchMouseEvent", { type: "mouseReleased", x, y, button: "left", buttons: 0, clickCount: 1 });
    await waitFor("the click's measure answer", 5000, async () => (await mCalls()) > n0);
  };
  const key = async (k, code, vk) => {
    await S("Input.dispatchKeyEvent", { type: "keyDown", key: k, code, windowsVirtualKeyCode: vk });
    await S("Input.dispatchKeyEvent", { type: "keyUp", key: k, code, windowsVirtualKeyCode: vk });
  };
  const rows = () => B("return b.measurements();");
  const pressed = () => B("return b.actions.value.find((a) => a.name === 'measure')?.pressed === true;");
  const at = (h) => (h ? h.at.map((v) => Math.round(v * 1000) / 1000) : null);
  const label = `6${THIN}000 mm`;

  // ---- 3D ----
  const e1 = await B("return b.runAction('measure');");
  check("action measure -> null", e1 === null, String(e1));
  await waitFor("the tool on", 3000, async () => (await gl()).measure?.active === true);
  check("the action is pressed", await pressed());
  check("no measurement yet", (await rows()).length === 0);
  await mMove(await screen(-148, -148, 0));
  const h1 = (await gl()).measure?.hover;
  check("3D: the pointer near P1.00's foot snaps to its corner", h1?.snap === "corner" && JSON.stringify(at(h1)) === "[-150,-150,0]", JSON.stringify(h1));
  await mClick(await screen(-148, -148, 0));
  check("3D: click 1 keeps the first point", JSON.stringify((await gl()).measure?.first) === "[-150,-150,0]", JSON.stringify((await gl()).measure?.first));
  await mMove(await screen(5852, -148, 0));
  await settle();
  check("3D: the rubber line's length is drawn", (await texts()).includes(label), JSON.stringify(await texts()));
  await mClick(await screen(5852, -148, 0));
  const st3 = (await gl()).measure;
  check("3D: click 2 makes one measurement of 6000 mm", st3?.items.length === 1 && st3.items[0].value_mm === 6000 && st3.items[0].text === label && st3.first === null,
    JSON.stringify(st3?.items));
  await settle();
  check("3D: the overlay draws \"6 000 mm\"", (await texts()).filter((t) => t === label).length === 1, JSON.stringify(await texts()));
  const r3 = await rows();
  check("3D: measurements has 1 row, \"1. 6 000 mm\"", r3.length === 1 && r3[0].label === `1. ${label}` && r3[0].value_mm === 6000, JSON.stringify(r3));
  check("3D: rev and the history unchanged", (await rev()) === rev0 && (await undoCount()) === undo0);
  check("3D: the tool calls went to the host (POST /tool/measure_*)", toolPosts.some((p) => p.endsWith("measure_click")), toolPosts.slice(-3).join(" "));
  await shot("measure-3d");
  const ec = await B("return b.clearMeasurements();");
  await waitFor("cleared", 3000, async () => (await rows()).length === 0).catch(() => {});
  check("clear: 0 rows", ec === null && (await rows()).length === 0);

  // ---- a plan ----
  const e0 = await B(`return b.openView('${PLAN_N1}');`);
  check("open Plan Niveau 1", e0 === null, String(e0));
  await waitFor("the plan camera", 5000, async () => (await gl()).camera?.mode === "plan");
  await settle();
  for (let k = 0; k < 20 && (await gl()).camera.half > 2400; k++) { // zoom about the bay
    const z = await screen(3000, -300, 0);
    await S("Input.dispatchMouseEvent", { type: "mouseWheel", x: z[0], y: z[1], deltaX: 0, deltaY: -150 });
    await settle();
  }
  { // then pan (3000, -300) to the canvas centre (a drag pans a plan, also while measuring)
    const from = await screen(3000, -300, 0);
    const to = await evalJs("(() => { const r = document.getElementById('bimble').getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; })()");
    await S("Input.dispatchMouseEvent", { type: "mousePressed", x: from[0], y: from[1], button: "left", buttons: 1, clickCount: 1 });
    for (let k = 1; k <= 10; k++) await S("Input.dispatchMouseEvent", { type: "mouseMoved", x: from[0] + ((to[0] - from[0]) * k) / 10, y: from[1] + ((to[1] - from[1]) * k) / 10, button: "left", buttons: 1 });
    await S("Input.dispatchMouseEvent", { type: "mouseReleased", x: to[0], y: to[1], button: "left", buttons: 0, clickCount: 1 });
    await settle();
  }
  check("the pan made no measurement", (await gl()).measure?.first === null && (await rows()).length === 0);
  check("the tool stays on in the plan", (await gl()).measure?.active === true);
  await mMove(await screen(-148, -148, 0));
  const hp = (await gl()).measure?.hover;
  check("plan: the pointer at P1.00's corner snaps to it", hp?.snap === "corner" && JSON.stringify(at(hp).slice(0, 2)) === "[-150,-150]", JSON.stringify(hp));
  await mClick(await screen(-148, -148, 0));
  await mClick(await screen(5852, -148, 0));
  const sp = (await gl()).measure;
  check("plan: one measurement of 6000 mm", sp?.items.length === 1 && sp.items[0].value_mm === 6000 && sp.items[0].text === label, JSON.stringify(sp?.items));
  await settle();
  check("plan: the overlay draws \"6 000 mm\"", (await texts()).includes(label), JSON.stringify(await texts()));
  const rp = await rows();
  check("plan: measurements has 1 row", rp.length === 1 && rp[0].label === `1. ${label}`, JSON.stringify(rp));
  await shot("measure-plan");

  // ---- Esc ----
  await mClick(await screen(-148, -148, 0));
  check("a first point again", (await gl()).measure?.first !== null);
  await key("Escape", "Escape", 27);
  await waitFor("the first point dropped", 3000, async () => (await gl()).measure?.first === null).catch(() => {});
  check("Esc drops the first point, the tool stays on", (await gl()).measure?.first === null && (await gl()).measure?.active === true);
  await key("Escape", "Escape", 27);
  await waitFor("the tool off", 3000, async () => (await gl()).measure?.active === false).catch(() => {});
  check("a second Esc leaves the tool, not pressed", (await gl()).measure?.active === false && !(await pressed()));
  check("the measurement stays", (await rows()).length === 1);
  check("rev and the history never changed", (await rev()) === rev0 && (await undoCount()) === undo0, `${await rev()} vs ${rev0}`);

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
console.log(`lab_measure: ${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
