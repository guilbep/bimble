// The lab's WebGL2 viewport against the C host (ticket 11a): node ccore/test/lab_gl.mjs
// Starts out/tcc/bimble-ccore (or $HOST_BIN) on 47323 with CCORE_MODEL=golden/frame.json, Vite
// from web-ui/ on 5193 (needs web-ui/node_modules), headless /usr/bin/chromium over raw CDP; opens
// /?native=1&gl=1&port=47323&token=… and checks: the page draws a frame from GET /mesh; concrete
// grey as Rust shades a top face (207,207,204 ±6; fill 214,214,210, see below) covers > 5 % of the canvas and the background (247,247,245 ±2) is the
// rest's main colour; no uncaught page error, no console error but the listed ones; a click on a
// concrete pixel sends POST /select/- with one 22-character GlobalId (the stub host may answer
// 400: the request is counted, not the answer); a click on the background sends select([]).
// Prints the times and pixel stats, "N passed, M failed"; exits non-zero on a failure. Kills all
// it starts.
import { spawn } from "node:child_process";
import { existsSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { port, vitePort } from "./ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = resolve(CCORE, "../web-ui");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("lab_gl", 47323);
const VITE = vitePort("lab_gl", 5193);
const TOKEN = "labgl-token";
const PAGE = `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}`;
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-gl-"));
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
  const host = start("host", BIN, [String(PORT)], {
    env: { ...process.env, BIMBLE_TOKEN: TOKEN, CCORE_MODEL: join(CCORE, "golden/frame.json"),
      BIMBLE_ORIGINS: `http://localhost:${VITE},http://127.0.0.1:${VITE}` },
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
  const wsUrl = await waitFor("DevTools line", 15000, () => chrome.out.match(/DevTools listening on (ws:\/\/\S+)/)?.[1]);
  const cdp = await cdpConnect(wsUrl);
  const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank" });
  const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
  const S = (m, p) => cdp.send(m, p, sessionId);
  const consoleLog = [];
  const exceptions = [];
  const selects = []; // {requestId, postData}
  const failedResponses = []; // "status url", for the log
  cdp.on((msg) => {
    if (msg.sessionId !== sessionId) return;
    if (msg.method === "Runtime.consoleAPICalled") {
      consoleLog.push({ level: msg.params.type, text: msg.params.args.map((a) => a.value ?? a.description ?? a.type).join(" ") });
    } else if (msg.method === "Runtime.exceptionThrown") {
      const d = msg.params.exceptionDetails;
      exceptions.push(d.exception?.description ?? d.text);
    } else if (msg.method === "Log.entryAdded") {
      consoleLog.push({ level: `log.${msg.params.entry.level}`, text: `${msg.params.entry.text} ${msg.params.entry.url ?? ""}`.trim() });
    } else if (msg.method === "Network.responseReceived" && msg.params.response.status >= 400) {
      failedResponses.push(`${msg.params.response.status} ${msg.params.response.url}`);
    } else if (msg.method === "Network.requestWillBeSent") {
      const r = msg.params.request;
      if (r.method === "POST" && new URL(r.url).pathname === "/select/-") selects.push({ requestId: msg.params.requestId, postData: r.postData });
    }
  });
  await S("Runtime.enable");
  await S("Log.enable");
  await S("Page.enable");
  await S("Network.enable");
  await S("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  const t0 = Date.now();
  await S("Page.navigate", { url: PAGE });
  const evalJs = async (expr) => (await S("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true })).result.value;

  const firstMs = await waitFor("the first drawn frame (window.__bbGl.firstFrameMs)", 30000,
    () => evalJs("window.__bbGl && window.__bbGl.firstFrameMs"));
  const wallMs = Date.now() - t0;
  const gl = await evalJs("JSON.stringify(window.__bbGl)");
  console.log(`first drawn frame: ${firstMs.toFixed(1)} ms after navigation start (performance.now), ${wallMs} ms wall from Page.navigate`);
  console.log(`viewport stats: ${gl}`);
  const st = JSON.parse(gl);
  check("mesh loaded from the host", st.meshBytes > 0 && st.error === null && st.meshRev === 1, gl); // session rev after load (lib.rs:1679)
  await sleep(500);

  // Pixels: read the WebGL2 drawing buffer (preserveDrawingBuffer) of #bimble.
  const px = JSON.parse(await evalJs(`(() => {
    const c = document.getElementById('bimble');
    const g = c.getContext('webgl2');
    const w = c.width, h = c.height, p = new Uint8Array(w * h * 4);
    g.readPixels(0, 0, w, h, g.RGBA, g.UNSIGNED_BYTE, p);
    const near = (i, r, gg, b, t) => Math.abs(p[i] - r) <= t && Math.abs(p[i + 1] - gg) <= t && Math.abs(p[i + 2] - b) <= t;
    // A grey face pixel (shaded concrete): neither background nor edge, r = g, b just below.
    // Expected value: Rust shades in linear light (shader.wgsl face_shade: ambient 0.62 + 0.38 *
    // lambert; light = view dir + (-0.3,-0.2,0.9), lib.rs:2566). Default camera (yaw -55, pitch 28):
    // a top face gets 0.933, so fill 214 -> linear 0.672 * 0.933 -> sRGB 207.5. The fill colour
    // itself is never on screen; the ticket had it wrong (orchestrator error, corrected here).
    const face = (i) => !near(i, 247, 247, 245, 2) && p[i] >= 120 && p[i] === p[i + 1] && p[i + 2] <= p[i] && p[i] - p[i + 2] <= 8;
    let concrete = 0, bg = 0, dark = 0, other = 0, cx = -1, cy = -1, bx = -1, by = -1;
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
      const i = 4 * (y * w + x);
      if (near(i, 207, 207, 204, 6)) concrete++;
      // a click target: the centre of a 9x9 block of grey face pixels, nearest the canvas centre
      if (face(i) && x > 4 && y > 4 && x < w - 5 && y < h - 5 && (cx < 0 || Math.hypot(x - w / 2, y - h / 2) < Math.hypot(cx - w / 2, cy - h / 2))) {
        let all = true;
        for (let dy = -4; dy <= 4 && all; dy++) for (let dx = -4; dx <= 4 && all; dx++) all = face(4 * ((y + dy) * w + x + dx));
        if (all) cx = x, cy = y;
      }
      if (near(i, 207, 207, 204, 6)) {
        // counted above
      } else if (near(i, 247, 247, 245, 2)) {
        bg++;
        if (bx < 0 && x > 10 && y > 10) bx = x, by = y;
      } else if (p[i] < 80) dark++;
      else other++;
    }
    const corner = [p[0], p[1], p[2]];
    const hist = {};
    for (let i = 0; i < p.length; i += 4) { const k = p[i] + ',' + p[i + 1] + ',' + p[i + 2]; hist[k] = (hist[k] || 0) + 1; }
    const top = Object.entries(hist).sort((a, b) => b[1] - a[1]).slice(0, 8).map(([k, v]) => k + ' ' + (100 * v / (w * h)).toFixed(1) + '%').join('; ');
    // readPixels rows go bottom-up: flip y to CSS (top-left) coordinates.
    return JSON.stringify({ w, h, cw: c.clientWidth, ch: c.clientHeight, concrete, bg, dark, other, corner, top,
      click: cx < 0 ? null : [cx, h - 1 - cy], empty: bx < 0 ? null : [bx, h - 1 - by] });
  })()`));
  const n = px.w * px.h;
  const pct = (k) => ((100 * k) / n).toFixed(1);
  console.log(`canvas ${px.w}x${px.h} (css ${px.cw}x${px.ch}): concrete ${pct(px.concrete)} %, background ${pct(px.bg)} %, ` +
    `dark (edges) ${pct(px.dark)} %, other (shaded faces) ${pct(px.other)} %; corner pixel ${px.corner}`);
  console.log(`most frequent colours: ${px.top}`);
  check("concrete grey covers > 5 % of the canvas", px.concrete > 0.05 * n, `${pct(px.concrete)} %`);
  check("background colour elsewhere (the largest class, corner pixel)", px.bg > px.other && px.bg > px.dark &&
    px.corner.every((v, k) => Math.abs(v - [247, 247, 245][k]) <= 2), `${pct(px.bg)} %, corner ${px.corner}`);
  check("edges drawn (dark pixels)", px.dark > 0, `${px.dark} px`);

  const clickAt = async (cx, cy) => {
    const rect = JSON.parse(await evalJs("JSON.stringify(document.getElementById('bimble').getBoundingClientRect())"));
    const x = rect.left + (cx * px.cw) / px.w, y = rect.top + (cy * px.ch) / px.h;
    const onCanvas = await evalJs(`document.elementFromPoint(${x}, ${y})?.id === 'bimble'`);
    for (const type of ["mouseMoved", "mousePressed", "mouseReleased"])
      await S("Input.dispatchMouseEvent", { type, x, y, button: type === "mouseMoved" ? "none" : "left", buttons: type === "mousePressed" ? 1 : 0, clickCount: 1 });
    return onCanvas;
  };
  const bodyOf = async (s) => s.postData ?? (await S("Network.getRequestPostData", { requestId: s.requestId }).catch(() => ({}))).postData;
  check("a grey face pixel to click", px.click !== null, JSON.stringify(px.click));
  // Background first: a successful select opens the properties panel and resizes the canvas,
  // so a point measured before it may no longer be on the canvas.
  if (px.empty) {
    const before = selects.length;
    await clickAt(...px.empty);
    await waitFor("POST /select/-", 5000, () => selects.length > before).catch(() => {});
    const got = selects.slice(before);
    const body = got.length ? await bodyOf(got[0]) : undefined;
    check("click on the background -> POST /select/- with []", got.length === 1 && body === "[]", `${got.length} request(s), body ${body}`);
  }  if (px.click) {
    const before = selects.length;
    const onCanvas = await clickAt(...px.click);
    check("the click point is on the canvas", onCanvas);
    await waitFor("POST /select/-", 5000, () => selects.length > before).catch(() => {});
    const got = selects.slice(before);
    const body = got.length ? await bodyOf(got[0]) : undefined;
    let ids = null;
    try { ids = JSON.parse(body); } catch {}
    check("click on a grey face -> one POST /select/- with a 22-char GlobalId", got.length === 1 && Array.isArray(ids) && ids.length === 1 &&
      /^[0-3][0-9A-Za-z_$]{21}$/.test(ids[0]), `${got.length} request(s), body ${body}`);
  }

  // Orbit (left drag) and zoom (wheel) redraw a different picture and send no select.
  const sum = () => evalJs(`(() => { const c = document.getElementById('bimble'), g = c.getContext('webgl2');
    const p = new Uint8Array(c.width * c.height * 4); g.readPixels(0, 0, c.width, c.height, g.RGBA, g.UNSIGNED_BYTE, p);
    let s = 0; for (let i = 0; i < p.length; i += 7) s = (s * 31 + p[i]) >>> 0; return s; })()`);
  const rect = JSON.parse(await evalJs("JSON.stringify(document.getElementById('bimble').getBoundingClientRect())"));
  const mx = rect.left + rect.width / 2, my = rect.top + rect.height / 2;
  const nSel = selects.length, s0 = await sum(), f0 = await evalJs("window.__bbGl.frames");
  await S("Input.dispatchMouseEvent", { type: "mousePressed", x: mx, y: my, button: "left", buttons: 1, clickCount: 1 });
  for (let k = 1; k <= 5; k++) await S("Input.dispatchMouseEvent", { type: "mouseMoved", x: mx + 12 * k, y: my + 4 * k, button: "left", buttons: 1 });
  await S("Input.dispatchMouseEvent", { type: "mouseReleased", x: mx + 60, y: my + 20, button: "left", buttons: 0, clickCount: 1 });
  await sleep(300);
  const s1 = await sum();
  await S("Input.dispatchMouseEvent", { type: "mouseWheel", x: mx, y: my, deltaX: 0, deltaY: -300 });
  await sleep(300);
  const s2 = await sum(), f2 = await evalJs("window.__bbGl.frames");
  check("orbit and zoom redraw a different picture, no select", s1 !== s0 && s2 !== s1 && f2 > f0 && selects.length === nSel,
    `frames ${f0} -> ${f2}, selects ${nSel} -> ${selects.length}`);
  const hostSelects = (host.out.match(/select/g) ?? []).length;
  console.log(`select requests seen by CDP: ${selects.length}; host log lines: ${host.out.trim().split("\n").length} (${hostSelects} mention select)`);
  console.log(`host: ${host.out.trim().split("\n").filter((l) => /mesh/.test(l)).join(" | ")}`);

  // Ignored, explicitly: the sync-XHR deprecation (bimble.ts uses sync XHR by design), the dev
  // server's favicon 404, and the stub host's 400 on select (no model in the calls yet). Chrome
  // logs a failed sync XHR with the calling script's URL (bimble.ts), not the request's: the
  // check below makes sure every 400 the page got was a POST /select/-.
  const IGNORED = [
    /Synchronous XMLHttpRequest on the main thread is deprecated/,
    /404 \(Not Found\) http:\/\/localhost:\d+\/favicon\.ico$/,
    new RegExp(`400 \\(Bad Request\\) http://127\\.0\\.0\\.1:${PORT}/select/-\\?`),
    /^gl viewport: select:/,
    new RegExp(`400 \\(Bad Request\\) http://localhost:${VITE}/src/bimble\\.ts$`),
  ];
  const bad = failedResponses.filter((r) => !/^404 http:\/\/localhost:\d+\/favicon\.ico$/.test(r) &&
    !new RegExp(`^400 http://127\\.0\\.0\\.1:${PORT}/select/-\\?`).test(r));
  check("no failed response but favicon and the stub's select", bad.length === 0, bad.join(" | "));
  const errors = consoleLog.filter((m) => /error/.test(m.level) && !IGNORED.some((r) => r.test(m.text)));
  console.log(`console messages: ${consoleLog.length}; responses >= 400: ${failedResponses.join(" | ") || "none"}`);
  for (const m of consoleLog.filter((m) => !IGNORED.some((r) => r.test(m.text)))) console.log(`  [${m.level}] ${m.text}`);
  check("no uncaught page error", exceptions.length === 0, exceptions.join(" | "));
  check("no console error", errors.length === 0, errors.map((m) => m.text).join(" | "));
  // Selection colour (last: the lab then asks the stub for the element, a 400 the checks above
  // would count). The stub host cannot select (400), so set the page's selection as the
  // change feed would (bimble.ts `state`) and look for the selection pink (215,172,182), shaded.
  const pink = await evalJs(`(async () => {
    const b = await import('/src/bimble.ts');
    b.state.value = { ...b.state.value, selection: ['1khkLES9P8rPMAGC5wqOCk'] };
    await new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)));
    const c = document.getElementById('bimble'), g = c.getContext('webgl2');
    const p = new Uint8Array(c.width * c.height * 4);
    g.readPixels(0, 0, c.width, c.height, g.RGBA, g.UNSIGNED_BYTE, p);
    let n = 0;
    for (let i = 0; i < p.length; i += 4) if (p[i] - p[i + 1] > 20 && p[i + 2] > p[i + 1] && p[i] > 120) n++;
    b.state.value = { ...b.state.value, selection: [] };
    return n;
  })()`);
  check("a selected element is drawn in the selection colour", pink > 100, `${pink} pink px`);
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
