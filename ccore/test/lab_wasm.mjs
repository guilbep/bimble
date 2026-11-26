// The lab fully in the browser on the C core's wasm module (ticket T13b): node ccore/test/lab_wasm.mjs
// No host process: copies out/wasm/ccore.wasm and golden/frame.json into web-ui/public/ccore/
// (scripts/copy-ccore.mjs), starts Vite from web-ui/ on <port> (needs web-ui/node_modules) and
// headless /usr/bin/chromium over raw CDP, opens /?ccore=1&gl=1 and checks: the page boots on the
// module (data-bb-ready), the navigator lists the 3 storeys, the canvas shows shaded concrete
// (207,207,204 ±6) on > 5 %, a click on a grey face selects one element (page state.selection) and
// the properties panel shows it, a place through the client adds an element and the mesh redraws
// (pixel hash changes), ccoreSave() returns the saved text, undo restores the picture; then the
// file actions (ticket A5): ccoreOpenFile(Blob of golden/bin/frame2.bimble) -> 2 storeys and a new
// mesh, ccoreExportIfc and ccoreSaveBytes, ribbon `new` -> 1 storey, ribbon `open` -> the page asks (unsaved; UX1), Discard -> the picker
// (intercepted: Page.fileChooserOpened) filled through DOM.setFileInputFiles (a copy of
// frame.json) -> 3 storeys; ticket A5c: ccoreOpenFile(Blob of golden/view/frame2.bimbleview,
// Rust's gzip) -> read-only (state editable false), the viewport draws its mesh (pixel hash
// changes) and the Concrete tab's ribbon shows Place: Column disabled (aria-disabled); no request
// to a host, no page error, no console error. Ticket BUG1: each picture is read once the viewport
// drew the state the action left (__bbGl.loops, __bbGl.drawn: no fixed sleep), and with the page's
// frames held for 300 ms after a deselect, a place, undo, redo, undo and a wall's ghost then Escape,
// the pixels 300 ms later are the old picture and the settled ones the state's; no frame is drawn at
// a buffer size other than the canvas's layout size (the properties panel changes it). Prints the
// wasm transfer size, navigation -> first frame and place -> pixels times, "N passed, M failed";
// exits non-zero on a failure. Kills all it starts.
import { spawn, execFileSync } from "node:child_process";
import { copyFileSync, existsSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import { gzipSync } from "node:zlib";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { CHROMIUM, vitePort } from "./ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = resolve(CCORE, "../web-ui");
const VITE = Number(process.env.LAB_WASM_VITE ?? vitePort("lab_wasm")); // LAB_WASM_VITE: a fixed port of your choice
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

  const chrome = start("chromium", CHROMIUM, [
    "--headless=new", "--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--remote-debugging-port=0",
    `--user-data-dir=${join(tmp, "chromium")}`, "--no-first-run", "--no-default-browser-check", "about:blank",
  ]);
  const wsUrl = await waitFor("DevTools line", 60000, () => chrome.out.match(/DevTools listening on (ws:\/\/\S+)/)?.[1]);
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

  // BUG1: the picture of the state an action leaves: a frame loop of the viewport that began after the
  // action (__bbGl.loops > l0), the state's rev drawn (__bbGl.drawn), at the canvas's layout size. A
  // fixed 300 ms sleep raced it: under load no frame came for > 300 ms after the deselect, and the
  // reference picture was the old one (the slab still selected, the canvas still 356 px high).
  const loops = () => evalJs("window.__bbGl.loops ?? -1");
  const settled = (what, l0) => waitFor(what, 10000, () => evalJs(`(async () => { const b = await import('/src/bimble.ts');
    const c = document.getElementById('bimble'), g = window.__bbGl, d = g.drawn, dpr = window.devicePixelRatio || 1;
    return g.loops > ${l0} && !!d && d.rev === b.state.value.rev && d.w === c.width && d.h === c.height
      && c.width === Math.max(1, Math.round(c.clientWidth * dpr)) && c.height === Math.max(1, Math.round(c.clientHeight * dpr)); })()`));

  // Place through the client: one more element, the mesh of the new rev drawn, the picture changes;
  // undo: the old rev's picture again. Times: the call to the first frame drawn with the new mesh.
  await bb("b.select([])");
  await settled("the picture without the selection", await loops());
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
  await settled("the picture of the place", await loops());
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
  await settled("the picture of the undo", await loops()).catch(() => null); // BUG1: then one read, no retry
  const h2 = await evalJs(hash), count2 = await bb("b.query('elements', {}).length");
  console.log(`undo: ${typeof undoMs === "number" ? undoMs.toFixed(1) + " ms to the frame" : undoMs}`);
  check("undo restores the element count and the picture", count2 === count0 && h2 === h0, `${count2} elements, hash ${h2} (before the place ${h0})`);

  // BUG1, deterministic. Each clear of #bimble records the buffer's size against the canvas's layout
  // size. The selection opens and closes the properties panel (the canvas changes height). With frames
  // on time, the frame after a layout change runs before the resize observer (HTML: rAF callbacks, then
  // layout, then observers): no frame may be drawn at the old size. Then each action runs with the
  // page's frames held (requestAnimationFrame queued: a loaded machine that draws no frame for 300 ms,
  // forced): 300 ms after it the pixels are not the state's (the race the old fixed sleep lost, every
  // time); the frames let go, the settled picture is the state's: deselect, place, undo, redo, a place
  // tool's ghost then Escape.
  await evalJs(`(() => { const B = window.__bug1 = { hold: false, held: [], stale: [], clears: 0 }, raf = window.requestAnimationFrame.bind(window);
    window.requestAnimationFrame = (cb) => B.hold ? (B.held.push(cb), 0) : raf(cb);
    B.release = () => { B.hold = false; for (const cb of B.held.splice(0)) raf(cb); };
    const p = WebGL2RenderingContext.prototype, clear = p.clear;
    p.clear = function (m) { const c = this.canvas; if (c && c.id === 'bimble') { const dpr = window.devicePixelRatio || 1;
      const w = Math.max(1, Math.round(c.clientWidth * dpr)), h = Math.max(1, Math.round(c.clientHeight * dpr)); B.clears++;
      if (c.width !== w || c.height !== h) B.stale.push(c.width + 'x' + c.height + ' drawn, ' + w + 'x' + h + ' laid out'); }
      return clear.call(this, m); };
    return 1; })()`);
  const slab = await bb("b.query('elements', { limit: 500 }).find((e) => e.class === 'IfcSlab')?.id ?? null");
  await bb(`b.select([${JSON.stringify(slab)}])`);
  await settled("the slab selected", await loops());
  const hSel = await evalJs(hash), hgtSel = await evalJs("document.getElementById('bimble').clientHeight");
  await bb("b.select([])");
  await settled("the deselect", await loops());
  const onTime = JSON.parse(await evalJs("JSON.stringify(window.__bug1)"));
  check("BUG1: frames on time: select then deselect (the canvas's height changes twice) -> no frame drawn at a buffer size other than the layout's",
    onTime.clears >= 2 && onTime.stale.length === 0, `${onTime.clears} clears, stale: ${onTime.stale.slice(0, 3).join("; ")}`);
  // `act` (an expression, or an async function) with the frames held: the pixels 300 ms later, then the settled ones.
  // The hold waits for one loop: the frame it asked before the hold runs; the next one is held.
  const held = async (what, act) => {
    const lh = await evalJs("(window.__bug1.hold = true, window.__bbGl.loops)");
    await waitFor("the frame loop held", 5000, async () => (await loops()) > lh); // its next frame was asked before the hold
    const r = typeof act === "string" ? await bb(act) : await act();
    await sleep(300);
    const slept = await evalJs(hash), l0 = await loops();
    await evalJs("window.__bug1.release()");
    await settled(`${what} (frames held)`, l0);
    return { r, slept, now: await evalJs(hash) };
  };
  await bb(`b.select([${JSON.stringify(slab)}])`);
  await settled("the slab selected", await loops());
  const desel = await held("the deselect", "b.select([])");
  const hgt = await evalJs("document.getElementById('bimble').clientHeight");
  console.log(`BUG1: canvas ${hgtSel} px high with the slab selected, ${hgt} px without; 300 ms after the deselect (frames held) the pixels are ` +
    `${desel.slept === hSel ? "the selected picture" : desel.slept === 0 ? "cleared (resized, not drawn yet)" : desel.slept}`);
  check("BUG1: frames held: deselect -> 300 ms later not the state's picture, settled the picture before",
    slab !== null && hSel !== h0 && desel.slept !== h0 && desel.now === h0, `slab ${slab}, selected ${hSel}, slept ${desel.slept}, settled ${desel.now}, before ${h0}`);
  const place = await held("the place", `b.runVerb('place', { a: [0.0, -2000.0], b: [6000.0, -2000.0], kind: 'wall', storey: ${JSON.stringify(STOREY)} })`);
  const undo = await held("the undo", "b.undo()");
  const redo = await held("the redo", "b.redo()");
  const undo2 = await held("the second undo", "b.undo()");
  const seq = [place, undo, redo, undo2], want = [h1, h0, h1, h0];
  check("BUG1: frames held: place, undo, redo, undo -> 300 ms later the previous picture each time; settled the place's, the before, the place's, the before",
    place.r === null && seq.every((x, k) => x.slept === want[(k + 3) % 4] && x.now === want[k]),
    seq.map((x, k) => `${["place", "undo", "redo", "undo"][k]} slept ${x.slept} settled ${x.now}`).join(", ") + ` (place ${h1}, before ${h0})`);
  // A wall's first point (the hover cross, then the ghost to the pointer), then Escape: the picture before.
  const crect = JSON.parse(await evalJs("JSON.stringify(document.getElementById('bimble').getBoundingClientRect())"));
  const at = (fx, fy) => ({ x: crect.left + fx * crect.width, y: crect.top + fy * crect.height });
  const revG = await bb("b.state.value.rev"), serr = await bb("b.setPlacing({ kind: 'wall' })");
  await waitFor("placing a wall", 5000, () => bb("b.placing.value?.active === true && b.placing.value.kind === 'wall'"));
  const p1 = at(0.4, 0.6), p2 = at(0.6, 0.6);
  await S("Input.dispatchMouseEvent", { type: "mouseMoved", ...p1, button: "none", buttons: 0 });
  for (const type of ["mousePressed", "mouseReleased"]) await S("Input.dispatchMouseEvent", { type, ...p1, button: "left", buttons: type === "mousePressed" ? 1 : 0, clickCount: 1 });
  await waitFor("one wall point", 5000, () => bb("b.placing.value?.points.length === 1"));
  await S("Input.dispatchMouseEvent", { type: "mouseMoved", ...p2, button: "none", buttons: 0 });
  await waitFor("the ghost", 5000, () => evalJs("window.__bbGl.ghostTris > 0"));
  await settled("the ghost", await loops());
  const hG = await evalJs(hash);
  const esc = await held("Escape", async () => {
    for (const type of ["keyDown", "keyUp"]) await S("Input.dispatchKeyEvent", { type, key: "Escape", code: "Escape", windowsVirtualKeyCode: 27 });
    return waitFor("placing off", 5000, () => bb("b.placing.value?.active === false"));
  });
  const revE = await bb("b.state.value.rev");
  check("BUG1: frames held: a wall's first point and its ghost, then Escape -> 300 ms later the ghost; settled the picture before",
    serr === null && hG !== h0 && esc.slept === hG && esc.now === h0 && revE === revG, `${serr} ghost ${hG} slept ${esc.slept} settled ${esc.now} before ${h0} rev ${revG} -> ${revE}`);
  const bug1 = JSON.parse(await evalJs("(() => { window.__bug1.release(); return JSON.stringify(window.__bug1); })()"));
  check("BUG1: frames held: no frame drawn at a buffer size other than the layout's", bug1.clears > onTime.clears && bug1.stale.length === 0,
    `${bug1.clears} clears, stale: ${bug1.stale.slice(0, 3).join("; ")}`);

  // Files (ticket A5): ccoreOpenFile with a Blob (golden/bin/frame2.bimble, v2: 2 storeys), the
  // storeys and the mesh change; Export IFC4 and Save bytes; the ribbon's `new` (1 storey); the
  // ribbon's `open`: the file picker it opens, filled through CDP DOM.setFileInputFiles (frame.json).
  const ccx = (expr) => evalJs(`(async () => { const c = await import('/src/ccore.ts'); const b = await import('/src/bimble.ts'); return ${expr}; })()`);
  const f2 = readFileSync(join(CCORE, "golden/bin/frame2.bimble")).toString("base64");
  const revBefore = await bb("b.state.value.rev");
  const opened = await ccx(`(async () => {
    const bytes = Uint8Array.from(atob(${JSON.stringify(f2)}), (ch) => ch.charCodeAt(0));
    try { return await c.ccoreOpenFile(new File([bytes], 'frame2.bimble'), true); } catch (e) { return String(e); }
  })()`);
  const st2 = await waitFor("storeys of frame2.bimble", 5000, async () => {
    const s = JSON.parse(await bb("JSON.stringify({ file: b.state.value.file, rev: b.state.value.rev, storeys: b.storeys.value.map((s) => s.name) })"));
    return s.storeys.length === 2 ? s : null;
  }).catch(() => null);
  check("ccoreOpenFile(frame2.bimble, v2) -> the page holds its 2 storeys", opened === true && st2?.file === "frame2.bimble", `${opened} ${JSON.stringify(st2)}`);
  const meshOk = await waitFor("the mesh of the opened file", 5000, async () => {
    const g = JSON.parse(await evalJs("JSON.stringify(window.__bbGl)"));
    return g.meshRev === st2?.rev && g.meshRev > revBefore ? g : null;
  }).catch(() => null);
  check("the viewport draws the opened file's mesh (a new rev)", meshOk !== null, `rev ${revBefore} -> ${st2?.rev}`);
  const exp = JSON.parse(await ccx("JSON.stringify((() => { const e = c.ccoreExportIfc('ifc4'); return { name: e.name, n: e.text.length, cols: e.text.includes('IFCCOLUMN') }; })())"));
  check("ccoreExportIfc('ifc4') -> frame2.ifc text", exp.name === "frame2.ifc" && exp.n > 1000 && exp.cols, JSON.stringify(exp));
  const sb = JSON.parse(await ccx("JSON.stringify((() => { const s = c.ccoreSaveBytes(); return { name: s.name, magic: new TextDecoder().decode(s.bytes.subarray(0, 6)), n: s.bytes.length }; })())"));
  check("ccoreSaveBytes() -> v2 bytes named frame2.bimble", sb.name === "frame2.bimble" && sb.magic === "bimble" && sb.n > 500, JSON.stringify(sb));
  const nerr = await bb("b.runAction('new')");
  const st3 = await waitFor("the new model's storey", 5000, async () => {
    const s = JSON.parse(await bb("JSON.stringify({ file: b.state.value.file, storeys: b.storeys.value.map((s) => s.name) })"));
    return s.storeys.length === 1 ? s : null;
  }).catch(() => null);
  check("ribbon action new -> Untitled.bimble with 'Level 0'", nerr === null && st3?.file === "Untitled.bimble" && st3.storeys[0] === "Level 0", `${nerr} ${JSON.stringify(st3)}`);
  // The picker as a click opens it (a user gesture), intercepted: CDP hands over its <input>.
  let chooser = null;
  cdp.on((msg) => { if (msg.sessionId === sessionId && msg.method === "Page.fileChooserOpened") chooser = msg.params; });
  await S("Page.setInterceptFileChooserDialog", { enabled: true });
  const oerr = (await S("Runtime.evaluate", { expression: "(async () => { const b = await import('/src/bimble.ts'); return b.runAction('open'); })()",
    returnByValue: true, awaitPromise: true, userGesture: true })).result.value;
  // UX1: the new model is unsaved, so open asks first in the page (shell/machine.ts confirmClose):
  // a real click on Discard, which opens the picker.
  const discard = await waitFor("the unsaved-changes question", 5000, async () => JSON.parse(await evalJs(`(() => {
    const e = document.querySelector('[data-bb=shell-confirm-discard]'); if (!e) return 'null';
    const b = e.getBoundingClientRect(); return JSON.stringify({ x: b.x + b.width / 2, y: b.y + b.height / 2 }); })()`))).catch(() => null);
  if (discard) for (const type of ["mousePressed", "mouseReleased"]) await S("Input.dispatchMouseEvent", { type, x: discard.x, y: discard.y, button: "left", buttons: type === "mousePressed" ? 1 : 0, clickCount: 1 });
  const nodeId = (await waitFor("Page.fileChooserOpened", 5000, () => chooser).catch(() => null))?.backendNodeId ?? 0;
  const pick = join(tmp, "picked.json");
  copyFileSync(join(CCORE, "golden/frame.json"), pick);
  if (nodeId) await S("DOM.setFileInputFiles", { files: [pick], backendNodeId: nodeId });
  const st4 = await waitFor("storeys of the picked file", 5000, async () => {
    const s = JSON.parse(await bb("JSON.stringify({ file: b.state.value.file, storeys: b.storeys.value.map((s) => s.name) })"));
    return s.storeys.length === 3 ? s : null;
  }).catch(() => null);
  check("ribbon action open: asked (unsaved), the file picker, then the picked frame.json's 3 storeys", oerr === null && !!discard && !!nodeId && st4?.file === "picked.json", `${oerr} node ${nodeId} ${JSON.stringify(st4)}`);

  // A .bimbleview (ticket A5c): read-only, drawn, edit actions off in the ribbon.
  const ribbonCol = `(async () => { const t = await import('/src/shell/tabs.ts'); t.activeTab.value = 'concrete';
    for (let k = 0; k < 10; k++) await new Promise((r) => requestAnimationFrame(r));
    const el = document.querySelector('[data-bb=ribbon] [data-bb=ribbon-tool-1-1]');
    return el ? String(el.getAttribute('aria-disabled')) : 'none'; })()`;
  const colBefore = await evalJs(ribbonCol);
  await sleep(300);
  const hv0 = await evalJs(hash), revV0 = await bb("b.state.value.rev");
  const fv = readFileSync(join(CCORE, "golden/view/frame2.bimbleview")).toString("base64");
  const vOpened = await ccx(`(async () => {
    const bytes = Uint8Array.from(atob(${JSON.stringify(fv)}), (ch) => ch.charCodeAt(0));
    try { return await c.ccoreOpenFile(new File([bytes], 'frame2.bimbleview'), true); } catch (e) { return String(e); }
  })()`);
  const stv = await waitFor("the view file open", 5000, async () => {
    const s = JSON.parse(await bb("JSON.stringify({ file: b.state.value.file, rev: b.state.value.rev })"));
    const g = JSON.parse(await evalJs("JSON.stringify(window.__bbGl)"));
    return s.file === "frame2.bimbleview" && g.meshRev === s.rev && s.rev > revV0 ? s : null;
  }).catch(() => null);
  await sleep(300);
  const hv1 = await evalJs(hash);
  const vState = JSON.parse(await bb("JSON.stringify(b.query('state', {}))"));
  check("ccoreOpenFile(frame2.bimbleview) -> read-only, its mesh drawn (the picture changed)",
    vOpened === true && stv !== null && vState.editable === false && hv1 !== hv0, `${vOpened} ${JSON.stringify(stv)} ${JSON.stringify(vState)} hash ${hv0} -> ${hv1}`);
  const colAfter = await evalJs(ribbonCol);
  check("the ribbon: Place: Column enabled on frame.json, disabled (aria-disabled) on the view file", colBefore === "null" && colAfter === "true", `${colBefore} -> ${colAfter}`);

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
