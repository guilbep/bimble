// The lab fully in the browser on the C core's wasm module (ticket T13b): node ccore/test/lab_wasm.mjs
// No host process: copies out/wasm/ccore.wasm and golden/frame.json into web-ui/public/ccore/
// (scripts/copy-ccore.mjs), starts Vite from web-ui/ on 5201 (needs web-ui/node_modules) and
// headless /usr/bin/chromium over raw CDP, opens /?ccore=1&gl=1 and checks: the page boots on the
// module (data-bb-ready), the navigator lists the 3 storeys, the canvas shows shaded concrete
// (207,207,204 ±6) on > 5 %, a click on a grey face selects one element (page state.selection) and
// the properties panel shows it, a place through the client adds an element and the mesh redraws
// (pixel hash changes), ccoreSave() returns the saved text, undo restores the picture; no request
// to a host, no page error, no console error. Prints the
// wasm transfer size, navigation -> first frame and place -> pixels times, "N passed, M failed";
// exits non-zero on a failure. Kills all it starts.
import { spawn, execFileSync } from "node:child_process";
import { existsSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import { gzipSync } from "node:zlib";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = resolve(CCORE, "../web-ui");
const VITE = 5201;
const PAGE = `http://localhost:${VITE}/?ccore=1&gl=1`;
const STOREY = "1uAZA0Yfn05f$xRd1k4uPR";
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-wasm-"));
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
  execFileSync("node", [join(UI, "scripts/copy-ccore.mjs")], { stdio: "inherit" });
  const wasmFile = readFileSync(join(UI, "public/ccore/ccore.wasm"));

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
  const failedResponses = [];
  const requests = new Map(); // requestId -> url
  let wasmNet = null; // {encoded, encoding}
  let hostRequests = 0; // anything to 127.0.0.1 (a native host): must stay 0
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
      requests.set(msg.params.requestId, msg.params.request.url);
      if (/^https?:\/\/127\.0\.0\.1:/.test(msg.params.request.url)) hostRequests++;
    } else if (msg.method === "Network.responseReceived") {
      const r = msg.params.response;
      if (r.status >= 400) failedResponses.push(`${r.status} ${r.url}`);
      if (/\/ccore\/ccore\.wasm$/.test(r.url)) wasmNet = { id: msg.params.requestId, encoding: r.headers["content-encoding"] ?? r.headers["Content-Encoding"] ?? "none", encoded: null };
    } else if (msg.method === "Network.loadingFinished" && wasmNet && msg.params.requestId === wasmNet.id) {
      wasmNet.encoded = msg.params.encodedDataLength;
    }
  });
  await S("Runtime.enable");
  await S("Log.enable");
  await S("Page.enable");
  await S("Network.enable");
  await S("Network.setCacheDisabled", { cacheDisabled: true });
  await S("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  const t0 = Date.now();
  await S("Page.navigate", { url: PAGE });
  const evalJs = async (expr) => (await S("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true })).result.value;
  const bb = (expr) => evalJs(`(async () => { const b = await import('/src/bimble.ts'); return ${expr}; })()`);

  const firstMs = await waitFor("the first drawn frame (window.__bbGl.firstFrameMs)", 30000,
    () => evalJs("window.__bbGl && window.__bbGl.firstFrameMs"));
  const wallMs = Date.now() - t0;
  const ready = await waitFor("data-bb-ready", 20000, () => evalJs("document.documentElement.dataset.bbReady === '1'")).catch(() => false);
  check("data-bb-ready is set (the page booted on the module)", ready === true);
  const cc = JSON.parse(await evalJs("JSON.stringify(window.__bbCcore ?? null)"));
  const gl0 = JSON.parse(await evalJs("JSON.stringify(window.__bbGl)"));
  console.log(`module: ${JSON.stringify(cc)}`);
  console.log(`first drawn frame: ${firstMs.toFixed(1)} ms after navigation start (performance.now), ${wallMs} ms wall from Page.navigate`);
  console.log(`viewport stats: ${JSON.stringify(gl0)}`);
  const gz = gzipSync(wasmFile, { level: 9 }).length;
  console.log(`ccore.wasm: ${wasmFile.length} bytes, transferred ${wasmNet?.encoded ?? "?"} bytes (content-encoding ${wasmNet?.encoding ?? "?"}); gzip -9 would be ${gz} bytes`);
  check("ccore.wasm fetched from the page", wasmNet !== null && cc !== null && cc.wasmBytes === wasmFile.length, JSON.stringify(wasmNet));
  check("mesh loaded from the module", gl0.meshBytes > 0 && gl0.error === null && gl0.meshRev === 1, JSON.stringify(gl0));
  const st = JSON.parse(await bb("JSON.stringify(b.state.value)"));
  check("page state: frame.json, editable, rev 1", st.file === "frame.json" && st.rev === 1, JSON.stringify(st));

  // The navigator's object tree: open "Objects" (72 elements), then one row per storey.
  const names = await bb("JSON.stringify(b.storeys.value.map((s) => s.name))");
  check("page holds 3 storeys", JSON.parse(names).length === 3, names);
  await evalJs(`(() => {
    const nav = document.querySelector('[data-bb=navigator]');
    const a = nav && [...nav.querySelectorAll('a[aria-expanded]')].find((x) => x.querySelector('.n')?.textContent.trim() === '72');
    a?.click();
    return !!a;
  })()`);
  const rows = await waitFor("storey rows", 5000, async () => {
    const r = JSON.parse(await evalJs(`(() => {
      const all = [...document.querySelectorAll('[data-bb=navigator] a.ti')];
      const at = all.findIndex((x) => x.querySelector('.n')?.textContent.trim() === '72');
      return JSON.stringify(at < 0 ? [] : all.slice(at + 1).filter((a) => a.style.paddingLeft === '22px').map((a) => a.querySelector('span')?.textContent.trim()));
    })()`));
    return r.length ? r : null;
  }).catch(() => []);
  check("navigator lists the 3 storeys", JSON.stringify(rows) === names, JSON.stringify(rows));
  await sleep(300);

  // Pixels (preserveDrawingBuffer): concrete share and a click target, as lab_gl.mjs.
  const px = JSON.parse(await evalJs(`(() => {
    const c = document.getElementById('bimble');
    const g = c.getContext('webgl2');
    const w = c.width, h = c.height, p = new Uint8Array(w * h * 4);
    g.readPixels(0, 0, w, h, g.RGBA, g.UNSIGNED_BYTE, p);
    const near = (i, r, gg, b, t) => Math.abs(p[i] - r) <= t && Math.abs(p[i + 1] - gg) <= t && Math.abs(p[i + 2] - b) <= t;
    const face = (i) => !near(i, 247, 247, 245, 2) && p[i] >= 120 && p[i] === p[i + 1] && p[i + 2] <= p[i] && p[i] - p[i + 2] <= 8;
    let concrete = 0, cx = -1, cy = -1;
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
      const i = 4 * (y * w + x);
      if (near(i, 207, 207, 204, 6)) concrete++;
      if (face(i) && x > 4 && y > 4 && x < w - 5 && y < h - 5 && (cx < 0 || Math.hypot(x - w / 2, y - h / 2) < Math.hypot(cx - w / 2, cy - h / 2))) {
        let all = true;
        for (let dy = -4; dy <= 4 && all; dy++) for (let dx = -4; dx <= 4 && all; dx++) all = face(4 * ((y + dy) * w + x + dx));
        if (all) cx = x, cy = y;
      }
    }
    return JSON.stringify({ w, h, cw: c.clientWidth, ch: c.clientHeight, concrete, click: cx < 0 ? null : [cx, h - 1 - cy] });
  })()`));
  const n = px.w * px.h;
  console.log(`canvas ${px.w}x${px.h}: concrete ${((100 * px.concrete) / n).toFixed(1)} %`);
  check("concrete grey covers > 5 % of the canvas", px.concrete > 0.05 * n, `${((100 * px.concrete) / n).toFixed(1)} %`);

  // A click on a grey face: the page's selection gets its id, the properties panel shows it.
  check("a grey face pixel to click", px.click !== null, JSON.stringify(px.click));
  if (px.click) {
    const rect = JSON.parse(await evalJs("JSON.stringify(document.getElementById('bimble').getBoundingClientRect())"));
    const x = rect.left + (px.click[0] * px.cw) / px.w, y = rect.top + (px.click[1] * px.ch) / px.h;
    for (const type of ["mouseMoved", "mousePressed", "mouseReleased"])
      await S("Input.dispatchMouseEvent", { type, x, y, button: type === "mouseMoved" ? "none" : "left", buttons: type === "mousePressed" ? 1 : 0, clickCount: 1 });
    const sel = await waitFor("page selection", 5000, async () => {
      const s = JSON.parse(await bb("JSON.stringify(b.state.value.selection)"));
      return s.length ? s : null;
    }).catch(() => []);
    check("click on a grey face -> page state.selection = [one GlobalId]", sel.length === 1 && /^[0-3][0-9A-Za-z_$]{21}$/.test(sel[0]), JSON.stringify(sel));
    if (sel.length === 1) {
      const detail = JSON.parse(await bb(`JSON.stringify(b.element(${JSON.stringify(sel[0])}))`));
      const props = await waitFor("properties of the selection", 5000, async () => {
        const t = await evalJs("document.querySelector('[data-bb=properties]')?.innerText ?? ''");
        return t.includes(sel[0]) && t.includes(detail.name) ? t : null;
      }).catch((e) => e.message);
      check("properties panel shows the clicked element (GlobalId and name)", props.includes(sel[0]) && props.includes(detail.name),
        props.replace(/\s+/g, " ").slice(0, 200));
    }
  }

  // Place through the client: one more element, the mesh of the new rev drawn, the picture changes;
  // undo: the old rev's picture again. Times: the call to the first frame drawn with the new mesh.
  await bb("b.select([])");
  await sleep(300);
  const hash = `(() => { const c = document.getElementById('bimble'), g = c.getContext('webgl2');
    const p = new Uint8Array(c.width * c.height * 4); g.readPixels(0, 0, c.width, c.height, g.RGBA, g.UNSIGNED_BYTE, p);
    let s = 0; for (let i = 0; i < p.length; i += 3) s = (s * 31 + p[i]) >>> 0; return s; })()`;
  // Runs `call` in the page, then waits for a frame drawn with the mesh of the new rev; ms.
  const timed = (call) => bb(`(async () => {
    const t = performance.now();
    const err = ${call};
    if (err) return 'error: ' + err;
    const rev = b.state.value.rev;
    for (let k = 0; k < 600; k++) {
      await new Promise((r) => requestAnimationFrame(r));
      if (window.__bbGl.meshRev === b.state.value.rev && b.state.value.rev !== rev - 1) {
        const f = window.__bbGl.frames;
        await new Promise((r) => requestAnimationFrame(r));
        if (window.__bbGl.frames >= f) return performance.now() - t;
      }
    }
    return 'timeout';
  })()`);
  const h0 = await evalJs(hash), count0 = await bb("b.query('elements', {}).length"), rev0 = await bb("b.state.value.rev");
  const placeMs = await timed(`b.runVerb('place', { a: [0.0, -2000.0], b: [6000.0, -2000.0], kind: 'wall', storey: ${JSON.stringify(STOREY)} })`);
  await sleep(200);
  const h1 = await evalJs(hash), count1 = await bb("b.query('elements', {}).length"), rev1 = await bb("b.state.value.rev");
  const gl1 = JSON.parse(await evalJs("JSON.stringify(window.__bbGl)"));
  console.log(`place (wall): ${typeof placeMs === "number" ? placeMs.toFixed(1) + " ms from the call to the frame with the new mesh" : placeMs}; ` +
    `elements ${count0} -> ${count1}, rev ${rev0} -> ${rev1}, mesh ${gl1.meshBytes} bytes, built+read in ${gl1.meshFetchMs.toFixed(2)} ms`);
  check("place through the client adds one element (rev + 1)", count1 === count0 + 1 && rev1 === rev0 + 1, `${count0} -> ${count1}, rev ${rev0} -> ${rev1}`);
  check("the mesh of the new rev is drawn and the picture changed", typeof placeMs === "number" && gl1.meshRev === rev1 && h1 !== h0, `${placeMs}, hash ${h0} -> ${h1}`);
  // Save: the module keeps the text (no files); ccoreSave() hands it to the page.
  const saved = JSON.parse(await evalJs("(() => { try { const t = window.ccoreSave(); return JSON.stringify({ n: t.length, wall: t.includes('\"IfcWall\"') }); } catch (e) { return JSON.stringify({ e: String(e) }); } })()"));
  check("ccoreSave() returns the saved text with the placed wall, state saved", saved.n > 1000 && saved.wall && (await bb("b.state.value.unsaved")) === false, JSON.stringify(saved));
  const undoMs = await timed("b.undo()");
  await sleep(200);
  const h2 = await evalJs(hash), count2 = await bb("b.query('elements', {}).length");
  console.log(`undo: ${typeof undoMs === "number" ? undoMs.toFixed(1) + " ms to the frame" : undoMs}`);
  check("undo restores the element count and the picture", count2 === count0 && h2 === h0, `${count2} elements, hash ${h2} (before the place ${h0})`);

  check("no request to a host (127.0.0.1)", hostRequests === 0, String(hostRequests));
  const bad = failedResponses.filter((r) => !/^404 http:\/\/localhost:\d+\/favicon\.ico$/.test(r));
  check("no failed response but the favicon", bad.length === 0, bad.join(" | "));
  const IGNORED = [/404 \(Not Found\) http:\/\/localhost:\d+\/favicon\.ico$/];
  const errors = consoleLog.filter((m) => /error/.test(m.level) && !IGNORED.some((r) => r.test(m.text)));
  console.log(`console messages: ${consoleLog.length}`);
  for (const m of consoleLog.filter((m) => !IGNORED.some((r) => r.test(m.text)))) console.log(`  [${m.level}] ${m.text}`);
  check("no uncaught page error", exceptions.length === 0, exceptions.join(" | "));
  check("no console error", errors.length === 0, errors.map((m) => m.text).join(" | "));
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
