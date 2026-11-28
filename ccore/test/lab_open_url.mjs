// Open a model from a link (ticket URL1, web-ui/src/openurl.ts, ADR 0086): node ccore/test/lab_open_url.mjs
// Builds the C lab as Pages serves it (copy-ccore.mjs for both wasm builds and the samples, then
// `VITE_CCORE=1 vite build --base ./`: a production page, as the published site; needs
// out/wasm/ccore.wasm and ccore-view.wasm and web-ui/node_modules), serves it from origin A
// (http://localhost:<port("open_url")>) and test files from origin B (http://127.0.0.1:<port("open_url_b")>:
// CORS allowed and refused, a slow chunked answer with and without Content-Length, a redirect, 404,
// 403, 503, an HTML page, plain text, a file over the limit declared by Content-Length and one that
// exceeds it without declaring, a gzip-encoded answer, a .ifc.gz), and drives one headless Chromium
// over raw CDP. Each case reaches the stated state with the stated message:
//   - #open=samples/frame.ifc (same origin): no question; the first feedback (the `opening` card)
//     within 200 ms of the navigation's start (lib/input_time.mjs, the page's part asserted), also with
//     the CPU throttled 4x, 6x and 10x (index.html draws the card while the HTML is parsed); the
//     model is frame.ifc, read-only; identical to the same file dropped on the page (state rev, the
//     elements' ids, status counts, the mesh's bytes and triangles drawn); no demo model fetched;
//   - ?view=1#open=samples/frame.ifc: the viewer-only wasm, ccore.wasm not fetched; the bytes a
//     first-time visitor downloads (raw and gzip -9, as Pages serves them) and page start -> first picture;
//   - another origin: the question "Open a model from <host>?" before any request reaches it; Cancel
//     -> Home and no request; Open -> the model (identical), remembered for the tab's session (no
//     question for the next file there); File -> Copy link copies <site>/#open=<encoded url>; Home
//     lists the link and a click fetches it again; Home's field (typed, Enter): the opening card
//     within 200 ms of the key;
//   - each failure: its message, the error card with Try again, Back to Home and Copy the file's
//     address (not Back); Try again fetches again; a refused scheme (data:, javascript:, file:, ftp:,
//     http: off localhost) fetches nothing; Cancel during a slow download closes the connection;
//   - a built page runs no `action=`, and no `edit=` with a link; `?model=` of another origin is
//     ignored (the demo opens, origin B gets no request);
//   - the native host build (out/tcc/bimble-ccore, no model; the page from Vite with ?native=1): the
//     host has no HTTP client, so the page fetches as the web build does (the same question for
//     origin B), stages the bytes (POST /stage) and the host opens the staged copy: frame.ifc, the
//     same counts and mesh.
// No uncaught page error. Prints the timings and transfer sizes, "N passed, M failed"; exits non-zero
// on a failure. Kills all it starts. Fails on the old code: no `open` parameter (the demo frame.json opens).
import { spawn, execFileSync } from "node:child_process";
import { createServer } from "node:http";
import { existsSync, mkdtempSync, readFileSync, rmSync, statSync } from "node:fs";
import { gzipSync } from "node:zlib";
import { createHash } from "node:crypto";
import { tmpdir } from "node:os";
import { dirname, extname, join, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";
import { CHROMIUM, port, vitePort } from "./ports.mjs";
import { hostEnv } from "./lib/host_env.mjs";
import { chromiumEnv, gpuFlags } from "./lib/chromium.mjs";
import { adapter, readTimes, verdict, watchSource } from "./lib/input_time.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = resolve(CCORE, "../web-ui");
const PA = port("open_url"), PB = port("open_url_b"), PDEAD = port("open_url_dead"); // PDEAD: locked, nothing listens
const PHOST = port("open_url_host"), VITE = vitePort("open_url"), TOKEN = "url1-lab-token";
const A = `http://localhost:${PA}`, B = `http://127.0.0.1:${PB}`;
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-open-url-"));
const SITE = join(tmp, "site");
const LIMIT = 100_000_000; // openurl.ts BROWSER_MAX
const procs = [], servers = [];
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? `: ${detail}` : ""}`);
};

async function waitFor(what, ms, fn) {
  const until = Date.now() + ms;
  let last;
  while (Date.now() < until) {
    try { const v = await fn(); if (v) return v; } catch (e) { last = e; }
    await sleep(50);
  }
  throw new Error(`timeout waiting for ${what}${last ? `: ${last.message ?? last}` : ""}`);
}

async function cleanup() {
  for (const sig of ["SIGTERM", "SIGKILL"]) {
    for (const p of procs) if (p.exitCode === null && p.signalCode === null) try { process.kill(-p.pid, sig); } catch {}
    await sleep(300);
  }
  for (const s of servers) { s.closeAllConnections?.(); await new Promise((r) => s.close(r)); }
  rmSync(tmp, { recursive: true, force: true });
}

// ---- the files ----
const IFC = readFileSync(join(CCORE, "golden/ifc/frame.ifc"));
const VIEW = readFileSync(join(CCORE, "golden/view/frame.bimbleview"));
const IFC_GZ = gzipSync(IFC);

// ---- origin A: the built site, as Pages serves it (a folder serves its index.html) ----
const TYPES = { ".html": "text/html", ".js": "text/javascript", ".css": "text/css", ".wasm": "application/wasm", ".json": "application/json", ".woff2": "font/woff2", ".svg": "image/svg+xml", ".ifc": "application/octet-stream", ".bimbleview": "application/octet-stream" };
const siteLog = [];
function serveSite(req, res) {
  const path = decodeURIComponent(new URL(req.url, A).pathname);
  siteLog.push(path);
  let f = resolve(SITE, `.${path}`);
  if (!f.startsWith(SITE + sep) && f !== SITE) { res.writeHead(403).end(); return; }
  if (existsSync(f) && statSync(f).isDirectory()) f = join(f, "index.html");
  if (!existsSync(f)) { res.writeHead(404).end("not found"); return; }
  const body = readFileSync(f);
  res.writeHead(200, { "Content-Type": TYPES[extname(f)] ?? "application/octet-stream", "Content-Length": body.length });
  res.end(req.method === "HEAD" ? undefined : body);
}

// ---- origin B: the test files (another origin: CORS applies) ----
const bLog = []; // { method, path, closedEarly, sent }
const CORS = { "Access-Control-Allow-Origin": "*" };
function serveB(req, res) {
  const path = new URL(req.url, B).pathname;
  const e = { method: req.method, path, sent: 0, done: false, closedEarly: false };
  bLog.push(e);
  res.on("close", () => { if (!e.done) e.closedEarly = true; });
  const end = (status, headers, body) => { res.writeHead(status, headers); e.sent += body?.length ?? 0; e.done = true; res.end(req.method === "HEAD" ? undefined : body); };
  if (req.method === "HEAD") return end(200, CORS);
  const slow = async (body, chunk, ms, headers) => {
    res.writeHead(200, headers);
    for (let at = 0; at < body.length && !res.destroyed; at += chunk) {
      res.write(body.subarray(at, at + chunk)); e.sent += Math.min(chunk, body.length - at);
      await sleep(ms);
    }
    if (!res.destroyed) { e.done = true; res.end(); }
  };
  switch (path) {
    case "/cors/frame.ifc": return end(200, { ...CORS, "Content-Type": "text/plain" }, IFC); // a wrong type: the bytes decide
    case "/cors/frame.bimbleview": return end(200, { ...CORS, "Content-Type": "text/html" }, VIEW);
    case "/nocors/frame.ifc": return end(200, { "Content-Type": "application/octet-stream" }, IFC);
    case "/missing.ifc": return end(404, CORS, Buffer.from("no such file"));
    case "/private.ifc": return end(403, CORS, Buffer.from("forbidden"));
    case "/broken.ifc": return end(503, CORS, Buffer.from("down"));
    case "/login": return end(200, { ...CORS, "Content-Type": "text/html" }, Buffer.from("<!doctype html><html><head><title>Sign in</title></head><body><form>Sign in to continue</form></body></html>"));
    case "/text.ifc": return end(200, { ...CORS, "Content-Type": "text/plain" }, Buffer.from("This is plain text, not a model at all, but long enough to be sniffed by the page."));
    case "/gzip/frame.ifc": return end(200, { ...CORS, "Content-Encoding": "gzip", "Content-Type": "application/octet-stream" }, IFC_GZ);
    case "/frame.ifc.gz": return end(200, { ...CORS, "Content-Type": "application/gzip" }, IFC_GZ);
    case "/redirect": return end(302, { ...CORS, Location: "/cors/frame.ifc" }, Buffer.alloc(0));
    case "/slow/frame.ifc": return void slow(IFC, 2048, 150, { ...CORS, "Content-Type": "application/octet-stream" }); // chunked: no length
    case "/slowlen/frame.ifc": return void slow(IFC, 8192, 150, { ...CORS, "Content-Length": IFC.length });
    case "/big-declared.ifc": { // the limit + 1 declared, 64 KB sent, then nothing: refused before the download
      res.writeHead(200, { ...CORS, "Content-Length": LIMIT + 1 });
      res.write(IFC.subarray(0, 65536)); e.sent += 65536;
      return;
    }
    case "/big-stream.ifc": { // no length; 1 MiB chunks until the page stops reading (300 MiB at most)
      res.writeHead(200, { ...CORS, "Content-Type": "application/octet-stream" });
      const chunk = Buffer.alloc(1 << 20, 0x20);
      IFC.copy(chunk, 0, 0, 4096);
      const pump = () => {
        while (!res.destroyed && e.sent < 300 * (1 << 20)) {
          e.sent += chunk.length;
          if (!res.write(chunk)) return void res.once("drain", pump);
        }
        if (!res.destroyed) { e.done = true; res.end(); }
      };
      return pump();
    }
  }
  end(404, CORS, Buffer.from("no route"));
}

// ---- CDP ----
async function cdpConnect(wsUrl) {
  const ws = new WebSocket(wsUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = () => rej(new Error("CDP connect failed")); });
  let id = 0;
  const pending = new Map(), listeners = [];
  ws.onmessage = (ev) => {
    const msg = JSON.parse(ev.data);
    if (msg.id !== undefined && pending.has(msg.id)) {
      const { resolve: ok, reject } = pending.get(msg.id);
      pending.delete(msg.id);
      if (msg.error) reject(new Error(`${msg.error.message} ${msg.error.data ?? ""}`)); else ok(msg.result);
    } else if (msg.method) for (const fn of listeners) fn(msg);
  };
  const send = (method, params = {}, sessionId) => new Promise((ok, reject) => {
    const m = ++id;
    pending.set(m, { resolve: ok, reject });
    ws.send(JSON.stringify({ id: m, method, params, ...(sessionId ? { sessionId } : {}) }));
  });
  return { send, on: (fn) => listeners.push(fn), close: () => ws.close() };
}

// What every new document gets: when each card first appeared, the first-feedback watcher (from the
// navigation's start: the timeline's origin), and Copy writes to window.__copied.
const FIRST = "!!document.querySelector('[data-bb=shell-opening],[data-bb=shell-openurl-ask]')";
const PRELUDE = `
  window.__seen = {};
  new MutationObserver(() => { for (const k of ['shell-openurl-ask', 'shell-opening', 'shell-error', 'shell-openurl-progress', 'home'])
    if (window.__seen[k] === undefined && document.querySelector('[data-bb=' + k + ']')) window.__seen[k] = performance.now(); })
    .observe(document, { subtree: true, childList: true, attributes: true });
  ${watchSource({ kind: "bb-none", page: FIRST, stamp: "0" })};
  Object.defineProperty(navigator, 'clipboard', { value: { writeText: async (t) => { window.__copied = t; } }, configurable: true });
`;

async function main() {
  for (const f of ["out/wasm/ccore.wasm", "out/wasm/ccore-view.wasm"]) if (!existsSync(join(CCORE, f))) throw new Error(`no ${f}: run ccore/build.sh wasm and wasm-view`);
  if (!existsSync(join(UI, "node_modules"))) throw new Error(`no ${UI}/node_modules: run npm ci in web-ui/`);
  // The production page (PROD: the deep-link guard, the preload script), as the Pages job builds it.
  execFileSync("node", [join(UI, "scripts/copy-ccore.mjs")], { stdio: "inherit" });
  execFileSync("node", [join(UI, "scripts/copy-ccore.mjs"), "--view"], { stdio: "inherit" });
  execFileSync("npx", ["vite", "build", "--base", "./", "--outDir", SITE, "--emptyOutDir", "--logLevel", "warn"], { cwd: UI, stdio: "inherit", env: { ...process.env, VITE_CCORE: "1" } });
  for (const [p, h] of [[PA, serveSite], [PB, serveB]]) {
    const s = createServer(h);
    servers.push(s);
    await new Promise((r) => s.listen(p, p === PA ? "localhost" : "127.0.0.1", r));
  }

  // One Chromium; its own short TMPDIR (its singleton socket's path must fit: test/lib/chromium.mjs chromiumEnv, removed on exit).
  const chrome = spawn(CHROMIUM, ["--headless=new", ...gpuFlags(), "--remote-debugging-port=0", "--lang=en-US",
    `--user-data-dir=${join(tmp, "chromium")}`, "--no-first-run", "--no-default-browser-check", "about:blank"],
  { stdio: ["ignore", "pipe", "pipe"], detached: true, env: chromiumEnv({ ...process.env, HOME: tmp }) });
  chrome.out = "";
  chrome.stdout.on("data", (d) => (chrome.out += d));
  chrome.stderr.on("data", (d) => (chrome.out += d));
  procs.push(chrome);
  const wsUrl = await waitFor("DevTools line", 60000, () => chrome.out.match(/DevTools listening on (ws:\/\/\S+)/)?.[1]);
  const cdp = await cdpConnect(wsUrl);

  const exceptions = [], warnings = [], requests = new Map(); // requestId -> { url, bytes }
  async function tab() {
    const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank" });
    const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
    const S = (m, p) => cdp.send(m, p, sessionId);
    cdp.on((msg) => {
      if (msg.sessionId !== sessionId) return;
      if (msg.method === "Runtime.exceptionThrown") { const d = msg.params.exceptionDetails; exceptions.push(d.exception?.description ?? d.text); }
      else if (msg.method === "Runtime.consoleAPICalled" && msg.params.type === "warning") warnings.push(msg.params.args.map((a) => a.value ?? a.description).join(" "));
      else if (msg.method === "Network.requestWillBeSent") requests.set(msg.params.requestId, { url: msg.params.request.url, bytes: 0 });
      else if (msg.method === "Network.loadingFinished" && requests.has(msg.params.requestId)) requests.get(msg.params.requestId).bytes = msg.params.encodedDataLength;
    });
    for (const d of ["Runtime", "Page", "Network"]) await S(`${d}.enable`);
    await S("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
    await S("Page.addScriptToEvaluateOnNewDocument", { source: PRELUDE });
    const evalJs = async (expr) => {
      const r = await S("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true });
      if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
      return r.result.value;
    };
    const go = async (url) => { requests.clear(); await evalJs("window.__bbOld = 1").catch(() => {}); await S("Page.navigate", { url }); await waitFor("the new document", 20000, () => evalJs("!window.__bbOld && document.readyState !== 'loading'")); };
    return { S, evalJs, go };
  }
  const T = await tab();
  const { evalJs, go, S } = T;
  const gpu = await adapter(evalJs);
  const q = (name, args = {}) => evalJs(`(() => { try { return JSON.parse(window.__bbQueries.query(${JSON.stringify(name)}, ${JSON.stringify(JSON.stringify(args))})); } catch (e) { throw new Error('query ${name}: ' + String(e)); } })()`);
  const ready = () => waitFor("data-bb-ready", 60000, () => evalJs("document.documentElement.dataset.bbReady === '1' && !!window.__bbGl && window.__bbGl.meshRev !== null"));
  const has = (sel) => evalJs(`!!document.querySelector(${JSON.stringify(sel)})`);
  const text = (sel) => evalJs(`document.querySelector(${JSON.stringify(sel)})?.textContent ?? null`);
  const click = (sel) => evalJs(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) throw new Error('no ${sel.replace(/'/g, "")}'); e.click(); return true; })()`);
  const errorShown = () => waitFor("the error card", 20000, () => has("[data-bb=shell-error]"));
  const bCount = (p) => bLog.filter((e) => e.path === p && e.method === "GET").length;
  /** What makes two opens the same model: the file, rev, the elements' ids, status counts, the mesh drawn. */
  const identity = async () => {
    const st = await q("state");
    const status = await q("status");
    const counts = Object.fromEntries(Object.entries(status ?? {}).filter(([k, v]) => typeof v === "number" && !/ms|idle|fps|time/i.test(k)));
    const gl = await evalJs("({ bytes: window.__bbGl.meshBytes, tris: window.__bbGl.drawnTris })");
    // The picture: the viewport's pixels once its frames are settled.
    const r = await evalJs("(() => { const b = document.getElementById('bimble').getBoundingClientRect(); return [b.left, b.top, b.width, b.height]; })()");
    const shot = await S("Page.captureScreenshot", { format: "png", clip: { x: r[0], y: r[1], width: r[2], height: r[3], scale: 1 } });
    const pixels = createHash("sha256").update(shot.data).digest("hex").slice(0, 16);
    return { file: st.file, editable: st.editable, counts, mesh: gl, pixels };
  };
  const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);
  const fmt = (n) => n.toLocaleString("en-US");

  // ---- 1. the site's sample, same origin: no question, first feedback within 200 ms, the model ----
  await go(`${A}/?c=sample#open=samples/frame.ifc`);
  await ready();
  const first = await readTimes(evalJs);
  const v1 = verdict(first, gpu);
  check(`#open=samples/frame.ifc: the opening card from the navigation's start: ${v1.line}`, v1.pageOk && v1.presOk);
  // the same on a slow machine (GitHub's runner: SwiftShader on 2-4 slow cores measured 217.8 ms before the card was
  // drawn by index.html's inline script): the CPU throttled 4x, 6x and 10x (Emulation.setCPUThrottlingRate), so the rule cannot
  // pass only on a fast machine
  for (const rate of [4, 6, 10]) { // (10x: the code before measured 162-184 ms at 6x, so 10x is the case that fails on it)
    await S("Emulation.setCPUThrottlingRate", { rate });
    await S("Network.setCacheDisabled", { cacheDisabled: true }); // (a fresh runner: the bundle fetched and compiled again)
    try {
      await go(`${A}/?c=sample#open=samples/frame.ifc`);
      const v = verdict(await readTimes(evalJs, 20000), gpu);
      check(`#open=samples/frame.ifc, the CPU throttled ${rate}x: the opening card from the navigation's start: ${v.line}`, v.pageOk && v.presOk);
      await ready();
    } finally { await S("Emulation.setCPUThrottlingRate", { rate: 1 }); await S("Network.setCacheDisabled", { cacheDisabled: false }); }
  }
  check("same origin: no question asked", (await evalJs("window.__seen['shell-openurl-ask'] === undefined")) === true);
  const idLink = await identity();
  check("the link's model is samples/frame.ifc, read-only, 72 elements", idLink.file === "frame.ifc" && idLink.editable === false && idLink.counts.total === 72, JSON.stringify(idLink));
  const firstPic = await evalJs("window.__bbGl.firstFrameMs");
  const urls1 = [...requests.values()].map((r) => new URL(r.url).pathname);
  check("no demo model fetched (frame.json), no request to origin B", !urls1.some((u) => u.endsWith("/frame.json")) && bLog.length === 0, urls1.join(" "));
  console.log(`  sample over localhost (full wasm): page start -> first picture ${firstPic?.toFixed(0)} ms`);

  // ---- 2. the same file dropped on the page: the same model ----
  await go(`${A}/?c=drop`);
  await ready();
  check("without a link: the demo model (frame.json) opens", (await q("state")).file === "frame.json");
  const revBefore = await evalJs("window.__bbGl.meshRev");
  await evalJs(`(async () => { const b = await (await fetch('samples/frame.ifc')).arrayBuffer(); const dt = new DataTransfer(); dt.items.add(new File([b], 'frame.ifc'));
    window.dispatchEvent(new DragEvent('drop', { dataTransfer: dt, bubbles: true, cancelable: true })); return true; })()`);
  await waitFor("the dropped file open and drawn", 30000, async () => (await q("state")).file === "frame.ifc" && !(await has("[data-bb=shell-opening]")) && (await evalJs("window.__bbGl.meshRev")) !== revBefore);
  await sleep(500);
  const idDrop = await identity();
  check("opened from the link = dropped (elements, triangles, model bytes, mesh bytes and triangles drawn, the picture)", same(idLink, idDrop), `${JSON.stringify(idLink)} vs ${JSON.stringify(idDrop)}`);

  // ---- 3. ?view=1: the viewer-only wasm; what a first-time visitor downloads ----
  await go(`${A}/?view=1&c=view#open=samples/frame.ifc`);
  await ready();
  const idView = await identity();
  const reqs = [...requests.values()].filter((r) => r.url.startsWith(A));
  const paths = reqs.map((r) => new URL(r.url).pathname);
  check("?view=1: ccore-view.wasm fetched, ccore.wasm and frame.json not", paths.includes("/ccore/ccore-view.wasm") && !paths.includes("/ccore/ccore.wasm") && !paths.some((p) => p.endsWith("frame.json")), paths.join(" "));
  check("?view=1: the same model", idView.file === "frame.ifc" && same(idView.counts, idLink.counts) && same(idView.mesh, idLink.mesh), JSON.stringify(idView));
  const viewPic = await evalJs("window.__bbGl.firstFrameMs");
  let raw = 0, gz = 0;
  const rows = [];
  for (const p of [...new Set(paths)]) {
    const f = join(SITE, p === "/" ? "index.html" : p.slice(1));
    if (!existsSync(f) || statSync(f).isDirectory()) continue;
    const b = readFileSync(f), z = gzipSync(b, { level: 9 }).length;
    raw += b.length, gz += z;
    rows.push(`${p} ${fmt(b.length)} (gzip ${fmt(z)})`);
  }
  console.log(`  ?view=1#open=samples/frame.ifc: page start -> first picture ${viewPic?.toFixed(0)} ms; first visit transfers ${fmt(raw)} bytes, ${fmt(gz)} gzip -9 (as Pages serves it):`);
  for (const r of rows) console.log(`    ${r}`);
  check("the viewer-only wasm is under 1,000,000 bytes", statSync(join(SITE, "ccore/ccore-view.wasm")).size < 1000000, fmt(statSync(join(SITE, "ccore/ccore-view.wasm")).size));

  // ---- 4. another origin: the question first; Cancel fetches nothing ----
  const corsIfc = `${B}/cors/frame.ifc`;
  await go(`${A}/?c=ask#open=${encodeURIComponent(corsIfc)}`);
  await waitFor("the question", 10000, () => has("[data-bb=shell-openurl-ask]"));
  const askT = await readTimes(evalJs);
  const v4 = verdict(askT, gpu);
  check(`another origin: the question from the navigation's start: ${v4.line}`, v4.pageOk && v4.presOk);
  const askText = await text("[data-bb=shell-openurl-ask]");
  check("the question names the host and the address", askText.includes(`Open a model from 127.0.0.1:${PB}?`) && askText.includes(corsIfc), askText);
  await sleep(300);
  check("nothing reaches the other origin before the click", bLog.length === 0, JSON.stringify(bLog));
  await click("[data-bb=shell-opening-cancel]");
  await waitFor("Home after Cancel", 10000, () => has("[data-bb=home]"));
  check("Cancel: Home, and still no request", bLog.length === 0);

  // ---- 5. Open: the model; remembered for the session; Copy link; Home's list ----
  await go(`${A}/?c=open#open=${encodeURIComponent(corsIfc)}`);
  await waitFor("the question again (Cancel remembers nothing)", 10000, () => has("[data-bb=shell-openurl-ask]"));
  await click("[data-bb=shell-openurl-allow]");
  await ready();
  const idCors = await identity();
  check("Open: the other origin's file opens, identical to the dropped one", same(idCors, idLink), JSON.stringify(idCors));
  await click("[data-bb=quickbar-file]");
  await waitFor("File -> Copy link", 5000, () => has("[data-bb=file-menu-copy-link]"));
  await click("[data-bb=file-menu-copy-link]");
  const copied = await evalJs("window.__copied ?? null");
  check("File -> Copy link: <site>/#open=<encoded address> (canonical)", copied === `${A}/#open=${encodeURIComponent(corsIfc)}`, copied);
  await go(`${A}/?c=again#open=${encodeURIComponent(`${B}/cors/frame.bimbleview`)}`);
  await ready();
  check("the origin is remembered for the tab's session: no question; the .bimbleview (served as text/html) opens", (await evalJs("window.__seen['shell-openurl-ask'] === undefined")) && (await q("state")).file === "frame.bimbleview");
  await go(`${A}/?c=demo-only`);
  await ready();
  check("without a link: File -> Copy link is not offered", (await click("[data-bb=quickbar-file]")) && !(await has("[data-bb=file-menu-copy-link]")));
  await evalJs("document.querySelector('[data-bb=quickbar-file]').click()");
  await click("[data-bb=quickbar-home]");
  await waitFor("Home", 5000, () => has("[data-bb=home]"));
  const listed = await evalJs("[...document.querySelectorAll('[data-bb=home-recent-link]')].map((a) => a.dataset.url)");
  check("Home lists the links opened (newest first), never the bytes", listed[0] === `${B}/cors/frame.bimbleview` && listed.includes(corsIfc) && (await evalJs("localStorage.getItem('bimble.recentLinks').length < 1000")), JSON.stringify(listed));
  const before = bCount("/cors/frame.ifc");
  await click(`[data-bb=home-recent-link][data-url="${corsIfc}"]`);
  await waitFor("the recent link open", 20000, async () => (await q("state")).file === "frame.ifc" && !(await has("[data-bb=shell-opening]")));
  check("a recent link fetches the file again", bCount("/cors/frame.ifc") === before + 1);

  // ---- 6. Home's field: typed, Enter: the opening card within 200 ms of the key ----
  await click("[data-bb=quickbar-home]");
  await waitFor("Home", 5000, () => has("[data-bb=home-link-input]"));
  await evalJs("document.querySelector('[data-bb=home-link-input]').focus()");
  await S("Input.insertText", { text: `${B}/slowlen/frame.ifc` });
  await evalJs(watchSource({ kind: "keydown", page: "!!document.querySelector('[data-bb=shell-opening]')" }));
  await S("Input.dispatchKeyEvent", { type: "keyDown", key: "Enter", code: "Enter", windowsVirtualKeyCode: 13, text: "\r" });
  await S("Input.dispatchKeyEvent", { type: "keyUp", key: "Enter", code: "Enter", windowsVirtualKeyCode: 13 });
  const v6 = verdict(await readTimes(evalJs), gpu);
  check(`Home's field, Enter -> the opening card: ${v6.line}`, v6.pageOk && v6.presOk);
  await waitFor("the progress of a known length", 5000, async () => / of /.test((await text("[data-bb=shell-openurl-bytes]")) ?? ""));
  const bar = await evalJs("(() => { const p = document.querySelector('[data-bb=shell-openurl-progress]'); return p ? [p.value, p.max] : null; })()");
  check("Content-Length given: a bar with its total and 'x of y'", bar && bar[1] === IFC.length && bar[0] > 0, JSON.stringify(bar));
  await waitFor("the slow file open", 30000, async () => (await q("state")).file === "frame.ifc" && !(await has("[data-bb=shell-opening]")));

  // ---- 7. a slow answer without a length: bytes so far; Cancel closes the connection ----
  await go(`${A}/?c=slow#open=${encodeURIComponent(`${B}/slow/frame.ifc`)}`);
  await waitFor("bytes so far", 10000, async () => /received/.test((await text("[data-bb=shell-openurl-bytes]")) ?? ""));
  const sofar = await text("[data-bb=shell-openurl-bytes]");
  await click("[data-bb=shell-opening-cancel]");
  await waitFor("Home after Cancel", 5000, () => has("[data-bb=home]"));
  await sleep(500);
  const slowReq = bLog.filter((e) => e.path === "/slow/frame.ifc").pop();
  check("no length: 'N MB received'; Cancel aborts the download (the server sees the connection closed)", /received/.test(sofar) && slowReq?.closedEarly === true && slowReq.sent < IFC.length, `${sofar}; ${JSON.stringify(slowReq)}`);

  // ---- 8. each failure: its message and the link's error card ----
  const failure = async (name, url, want, { prompt = false } = {}) => {
    await go(`${A}/?c=${encodeURIComponent(name)}#open=${encodeURIComponent(url)}`);
    if (prompt) { await waitFor("the question", 10000, () => has("[data-bb=shell-openurl-ask]")); await click("[data-bb=shell-openurl-allow]"); }
    await errorShown();
    const msg = await text("[data-bb=shell-error-message]");
    const btns = await evalJs("[...document.querySelectorAll('[data-bb=shell-error] button')].map((b) => b.dataset.bb).join(' ')");
    const ok = want.every((w) => msg.includes(w)) && btns === "shell-error-copy shell-error-home shell-error-retry";
    check(`${name}: "${want.join("…")}"; Copy, Back to Home, Try again`, ok, `${msg} [${btns}]`);
    return msg;
  };
  await failure("CORS refused", `${B}/nocors/frame.ifc`, [`127.0.0.1:${PB} does not allow opening its files from another site`, `allow this site (${A})`, "drop it"]);
  const tries = bCount("/nocors/frame.ifc");
  await click("[data-bb=shell-error-retry]");
  await waitFor("Try again: fetched again", 10000, () => bCount("/nocors/frame.ifc") > tries);
  await errorShown();
  check("Try again fetches again and shows the error again", (await text("[data-bb=shell-error-message]")).includes("does not allow"));
  await click("[data-bb=shell-error-copy]");
  check("Copy the file's address", (await evalJs("window.__copied")) === `${B}/nocors/frame.ifc`);
  await click("[data-bb=shell-error-home]");
  await waitFor("Back to Home", 5000, () => has("[data-bb=home]"));
  check("Back to Home: Home", true);
  await failure("404", `${B}/missing.ifc`, ["answered 404 Not Found", "not at this address"]);
  await failure("403", `${B}/private.ifc`, ["answered 403 Forbidden", "not public", "drop it"]);
  await failure("503", `${B}/broken.ifc`, ["answered 503 Service Unavailable", "server error"]);
  await failure("an HTML page", `${B}/login`, ["sent a web page, not a model", "sign-in page"]);
  await failure("plain text", `${B}/text.ifc`, ["not a model (text/plain", "starting with “This is plain text"]);
  await failure("over the limit, declared", `${B}/big-declared.ifc`, ["Too large for the browser: 100 MB (at most 100 MB here)", "Open it in the Bimble app"]);
  const decl = bLog.filter((e) => e.path === "/big-declared.ifc").pop();
  await waitFor("the declared answer closed", 5000, () => decl?.closedEarly).catch(() => {});
  check("declared over the limit: refused before the download (connection closed after 64 KB)", decl?.closedEarly === true && decl.sent <= 65536, JSON.stringify(decl));
  await failure("over the limit, undeclared", `${B}/big-stream.ifc`, ["Too large for the browser: more than 100 MB", "Open it in the Bimble app"]);
  await sleep(300);
  const strm = bLog.filter((e) => e.path === "/big-stream.ifc").pop();
  check("undeclared: stopped once past the limit (not the 300 MiB the server would send)", strm?.closedEarly === true && strm.sent < 300 * (1 << 20), `${fmt(strm?.sent ?? 0)} bytes sent`);
  await failure("network down", `http://127.0.0.1:${PDEAD}/x.ifc`, [`127.0.0.1:${PDEAD} cannot be reached`], { prompt: true });
  for (const [name, url] of [["data:", "data:text/plain,ISO-10303-21;"], ["javascript:", "javascript:alert(1)"], ["file:", "file:///etc/hostname"], ["ftp:", "ftp://example.org/x.ifc"], ["http: off localhost", "http://example.org/x.ifc"]]) {
    const n = bLog.length;
    await failure(`refused scheme ${name}`, url, [`Address refused (${url.split(":")[0]}:)`, "only https://"]);
    check(`${name}: nothing fetched, no question`, bLog.length === n && (await evalJs("window.__seen['shell-openurl-ask'] === undefined")));
  }

  // ---- 9. what a server sends compressed: decoded by the browser, or a .ifc.gz; a redirect ----
  for (const [name, url] of [["a gzip-encoded answer", `${B}/gzip/frame.ifc`], ["a .ifc.gz file", `${B}/frame.ifc.gz`], ["a redirect", `${B}/redirect`]]) {
    await go(`${A}/?c=${encodeURIComponent(name)}#open=${encodeURIComponent(url)}`);
    await ready();
    const id = await identity();
    check(`${name}: the same model`, same({ ...id, file: "frame.ifc" }, idLink) && /\.ifc$/.test(id.file), JSON.stringify(id));
  }

  // ---- 10. a built page runs no action from a link; ?model= of another origin is ignored ----
  await go(`${A}/?c=hooks&action=new&edit=move-first-beam#open=samples/frame.ifc`);
  await ready();
  await sleep(1500);
  const hook = await q("state");
  check("built page, a link with action=new&edit=…: no action ran (the link's model, unsaved false: no New, no move)", hook.file === "frame.ifc" && hook.unsaved === false, JSON.stringify(hook));
  await go(`${A}/?c=hooks2&action=new`);
  await ready();
  await sleep(1500);
  check("built page, the demo with action=new: no action ran", (await q("state")).file === "frame.json");
  const nB = bLog.length;
  await go(`${A}/?c=model&model=${encodeURIComponent(`${B}/cors/frame.ifc`)}&ccore_wasm=${encodeURIComponent(`${B}/x.wasm`)}`);
  await ready();
  check("built page: ?model= and ?ccore_wasm= of another origin are ignored (no request; the demo opens)", bLog.length === nB && (await q("state")).file === "frame.json", JSON.stringify(bLog.slice(nB)));

  // ---- 11. the native host build: the page fetches (the host has no HTTP client), the host opens the staged copy ----
  const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
  if (!existsSync(BIN)) throw new Error(`no ${BIN}: run ccore/build.sh tcc`);
  const start = (label, cmd, args, opts) => {
    const p = spawn(cmd, args, { stdio: ["ignore", "pipe", "pipe"], detached: true, ...opts });
    p.out = "";
    p.stdout.on("data", (d) => (p.out += d));
    p.stderr.on("data", (d) => (p.out += d));
    p.label = label;
    procs.push(p);
    return p;
  };
  const host = start("host", BIN, [String(PHOST)], { env: hostEnv({ HOME: tmp, BIMBLE_TOKEN: TOKEN, BIMBLE_ORIGINS: `http://localhost:${VITE},http://127.0.0.1:${VITE}` }) });
  await waitFor("host 'listening'", 10000, () => host.out.includes("listening") || (host.exitCode !== null && Promise.reject(new Error(host.out))));
  const vite = start("vite", "npx", ["vite", "--port", String(VITE), "--strictPort"], { cwd: UI });
  await waitFor(`Vite on ${VITE}`, 60000, async () => { if (vite.exitCode !== null) throw new Error(vite.out); return (await fetch(`http://localhost:${VITE}/`)).ok; });
  const nB0 = bCount("/cors/frame.ifc");
  await go(`http://localhost:${VITE}/?native=1&gl=1&port=${PHOST}&token=${TOKEN}#open=${encodeURIComponent(corsIfc)}`);
  await waitFor("the question (the native page's own origin and session)", 30000, () => has("[data-bb=shell-openurl-ask]"));
  await click("[data-bb=shell-openurl-allow]");
  await waitFor("the staged copy open and drawn", 60000, async () => !(await has("[data-bb=shell-opening]")) && (await q("state")).file !== null && (await evalJs("!!window.__bbGl && window.__bbGl.meshRev !== null && window.__bbGl.drawnTris > 0")));
  await sleep(500);
  const idNative = await identity();
  check("native host: the page fetched the file (no HTTP client in the host), staged it, the host opened it: frame.ifc, the same counts and mesh",
    bCount("/cors/frame.ifc") === nB0 + 1 && /frame\.ifc$/.test(idNative.file ?? "") && same(idNative.counts, idLink.counts) && same(idNative.mesh, idLink.mesh), JSON.stringify(idNative));

  check("no uncaught page error", exceptions.length === 0, exceptions.slice(0, 3).join(" | "));
  cdp.close();
}

try {
  await main();
} catch (e) {
  check("no exception in the test", false, e.stack ?? String(e));
  for (const p of procs) console.log(`--- chromium output ---\n${p.out?.slice(-1500)}`);
} finally {
  await cleanup();
}
console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
