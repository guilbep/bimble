// The C lab as GitHub Pages serves it (ticket PG1): node ccore/test/lab_pages.mjs
// Runs `npm run build:ccore -- --outDir <tmp>/site/c --emptyOutDir` in web-ui/ (both wasm builds,
// copy-ccore.mjs twice, `VITE_CCORE=1 vite build --base ./`; needs web-ui/node_modules), puts a
// stand-in for the Rust lab at site/index.html, serves site/ with a static Node server (as Pages:
// /c redirects to /c/, a folder serves its index.html) and drives headless /usr/bin/chromium over
// raw CDP with real mouse and key events:
//   /c/ (no parameter): the page boots on ccore.wasm (its size), the address gets no `ccore=`, a
//     frame is drawn (concrete grey > 5 %), the status bar's preview line names the commit;
//     place: the Concrete tab, the ribbon's Column, a click on a free spot -> one IfcColumn (in the
//     saved text), the picture changed; Escape, the quick bar's Undo -> the column is gone from the
//     saved text and the picture is the one before the place.
//   /c/?view=1&edit=move-first-beam: the page boots on ccore-view.wasm (its size), a frame is
//     drawn, the deep link's `move` verb is answered "this build is view-only", the mesh stays rev 1.
// Every request of the pages stays under /c/ (relative URLs only; the browser's own /favicon.ico
// aside), no failed response but that favicon, no page error, no console error.
// Prints "N passed, M failed"; exits non-zero on a failure. Kills all it starts.
import { spawn, execFileSync } from "node:child_process";
import { createServer } from "node:http";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, extname, join, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";
import { CHROMIUM, port } from "./ports.mjs";
import { loops, settled } from "./lib/frames.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = resolve(CCORE, "../web-ui");
const PORT = port("lab_pages");
const ORIGIN = `http://localhost:${PORT}`;
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-pages-"));
const SITE = join(tmp, "site");
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

let server = null;
async function cleanup() {
  for (const sig of ["SIGTERM", "SIGKILL"]) {
    for (const p of procs) if (p.exitCode === null && p.signalCode === null) try { process.kill(-p.pid, sig); } catch {}
    await sleep(300);
  }
  if (server) await new Promise((r) => server.close(r));
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

// A static server as GitHub Pages: files of site/, a folder's index.html, /c -> 301 /c/. Logs paths.
const TYPES = { ".html": "text/html", ".js": "text/javascript", ".css": "text/css", ".wasm": "application/wasm",
  ".json": "application/json", ".woff2": "font/woff2", ".svg": "image/svg+xml", ".png": "image/png", ".ico": "image/x-icon" };
const served = []; // [status, path]
function serve() {
  server = createServer((req, res) => {
    const path = decodeURIComponent(new URL(req.url, ORIGIN).pathname);
    let file = resolve(SITE, "." + path);
    const reply = (status, body = "", headers = {}) => { served.push([status, path]); res.writeHead(status, headers); res.end(body); };
    if (file !== SITE && !file.startsWith(SITE + sep)) return reply(404);
    if (existsSync(file) && statSync(file).isDirectory()) {
      if (!path.endsWith("/")) return reply(301, "", { Location: `${path}/` });
      file = join(file, "index.html");
    }
    if (!existsSync(file)) return reply(404, "not found");
    reply(200, readFileSync(file), { "Content-Type": TYPES[extname(file)] ?? "application/octet-stream", "Cache-Control": "no-store" });
  });
  return new Promise((r) => server.listen(PORT, "127.0.0.1", r));
}

async function main() {
  if (!existsSync(join(UI, "node_modules"))) throw new Error(`no ${UI}/node_modules: run npm ci in web-ui/`);
  const rev = execFileSync("git", ["rev-parse", "--short", "HEAD"], { cwd: UI }).toString().trim();
  const t0 = Date.now();
  let built = true;
  try {
    execFileSync("npm", ["run", "build:ccore", "--", "--outDir", join(SITE, "c"), "--emptyOutDir"], { cwd: UI, stdio: ["ignore", "pipe", "pipe"], env: { ...process.env, VITE_CCORE_REV: "" } });
  } catch (e) {
    built = false;
    console.log(`${e.stdout ?? ""}${e.stderr ?? ""}`.slice(-3000));
  }
  console.log(`build:ccore: ${Date.now() - t0} ms`);
  const C = join(SITE, "c");
  const need = ["index.html", "ccore/ccore.wasm", "ccore/ccore-view.wasm", "ccore/frame.json"];
  check("npm run build:ccore builds site/c/ (index.html, ccore.wasm, ccore-view.wasm, frame.json)", built && need.every((f) => existsSync(join(C, f))),
    need.filter((f) => !existsSync(join(C, f))).join(", "));
  if (!built) return;
  const html = readFileSync(join(C, "index.html"), "utf8");
  const abs = [...html.matchAll(/(?:src|href)="(\/[^"]*)"/g)].map((m) => m[1]);
  check("index.html refers to its files by relative URLs", abs.length === 0, abs.join(" "));
  const wasmFull = statSync(join(C, "ccore/ccore.wasm")).size, wasmView = statSync(join(C, "ccore/ccore-view.wasm")).size;
  console.log(`site/c/: ccore.wasm ${wasmFull} bytes, ccore-view.wasm ${wasmView} bytes`);
  // The Rust lab at the site root: a stand-in, so a request that leaves /c/ is answered and seen.
  mkdirSync(SITE, { recursive: true });
  writeFileSync(join(SITE, "index.html"), "<!doctype html><title>root</title>");
  await serve();

  const chrome = start("chromium", CHROMIUM, [
    "--headless=new", "--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--remote-debugging-port=0",
    `--user-data-dir=${join(tmp, "chromium")}`, "--no-first-run", "--no-default-browser-check", "about:blank",
  ]);
  const wsUrl = await waitFor("DevTools line", 60000, () => chrome.out.match(/DevTools listening on (ws:\/\/\S+)/)?.[1]);
  const cdp = await cdpConnect(wsUrl);
  const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank" });
  const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
  const S = (m, p) => cdp.send(m, p, sessionId);
  const consoleLog = [], exceptions = [], failedResponses = [], requests = [];
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
      requests.push(msg.params.request.url);
    } else if (msg.method === "Network.responseReceived") {
      const r = msg.params.response;
      if (r.status >= 400) failedResponses.push(`${r.status} ${r.url}`);
    }
  });
  for (const d of ["Runtime", "Log", "Page", "Network"]) await S(`${d}.enable`);
  await S("Network.setCacheDisabled", { cacheDisabled: true });
  await S("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  const evalJs = async (expr) => {
    const r = await S("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
    return r.result.value;
  };
  const gl = async () => JSON.parse(await evalJs("JSON.stringify(window.__bbGl ?? null)"));
  const hash = `(() => { const c = document.getElementById('bimble'), g = c.getContext('webgl2');
    const p = new Uint8Array(c.width * c.height * 4); g.readPixels(0, 0, c.width, c.height, g.RGBA, g.UNSIGNED_BYTE, p);
    let s = 0; for (let i = 0; i < p.length; i += 3) s = (s * 31 + p[i]) >>> 0; return s; })()`;
  const concrete = `(() => { const c = document.getElementById('bimble'), g = c.getContext('webgl2');
    const w = c.width, h = c.height, p = new Uint8Array(w * h * 4); g.readPixels(0, 0, w, h, g.RGBA, g.UNSIGNED_BYTE, p);
    let n = 0; for (let i = 0; i < p.length; i += 4) if (Math.abs(p[i] - 207) <= 6 && Math.abs(p[i + 1] - 207) <= 6 && Math.abs(p[i + 2] - 204) <= 6) n++;
    return n / (w * h); })()`;
  // Center of an element found by a selector (and an optional text test), in CSS pixels.
  const centre = (sel, text = null) => evalJs(`(() => {
    const el = [...document.querySelectorAll(${JSON.stringify(sel)})].find((e) => ${text ? `${text}.test(e.textContent.trim())` : "true"});
    if (!el) return null; const r = el.getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; })()`);
  const mouse = async (type, [x, y]) => S("Input.dispatchMouseEvent",
    { type, x, y, button: type === "mouseMoved" ? "none" : "left", buttons: type === "mousePressed" ? 1 : 0, clickCount: 1 });
  const click = async (xy) => { await mouse("mouseMoved", xy); await mouse("mousePressed", xy); await mouse("mouseReleased", xy); };
  const savedText = () => evalJs("(() => { try { return window.ccoreSave(); } catch (e) { return 'error: ' + e; } })()");
  const count = (text, s) => text.split(s).length - 1;
  const boot = async (what) => {
    await waitFor(`${what}: the first drawn frame`, 30000, () => evalJs("window.__bbGl && window.__bbGl.firstFrameMs"));
    return waitFor(`${what}: data-bb-ready`, 20000, () => evalJs("document.documentElement.dataset.bbReady === '1'")).catch(() => false);
  };

  // ---- /c/: the full build, no parameter ----
  await S("Page.navigate", { url: `${ORIGIN}/c` });
  const ready = await boot("/c/");
  check("/c redirects to /c/ and the page boots (data-bb-ready)", ready === true && (await evalJs("location.pathname")) === "/c/");
  const cc = JSON.parse(await evalJs("JSON.stringify(window.__bbCcore ?? null)"));
  check("the page runs ccore.wasm (its size) on frame.json, the address has no ccore=",
    cc?.wasmBytes === wasmFull && cc?.model === "frame.json" && !(await evalJs("location.search")).includes("ccore"), `${JSON.stringify(cc)} ${await evalJs("location.search")}`);
  const g0 = await gl();
  await settled(evalJs, "the first picture", await loops(evalJs), { rev: "mesh" }); // TEST1: was sleep(300)
  const share = await evalJs(concrete);
  check("a frame is drawn: the mesh (rev 1) and concrete grey > 5 % of the canvas", g0?.meshBytes > 0 && g0.meshRev === 1 && share > 0.05,
    `${g0?.meshBytes} bytes, rev ${g0?.meshRev}, ${(100 * share).toFixed(1)} %`);
  const line = await evalJs("document.querySelector('[data-bb=status-preview]')?.textContent ?? ''");
  check("the status bar's preview line names the commit and the uncut openings", line.startsWith(`Preview, C core ${rev}`) && line.includes("uncut"), line);

  // Place: the Concrete tab, the ribbon's Column (Poteau), one click on a free spot.
  const tab = await centre("[data-bb=tabsbar] a.tab", /^(Béton|Concrete)$/);
  if (tab) await click(tab);
  await sleep(500);
  await mouse("mouseMoved", [5, 5]);
  await settled(evalJs, "the picture before the place", await loops(evalJs), { rev: "mesh" }); // TEST1: was sleep(300)
  const h0 = await evalJs(hash);
  const tool = await centre("[data-bb=ribbon] [data-bb=ribbon-tool-1-1]");
  if (tool) await click(tool);
  const placingOn = await waitFor("the placing chip", 5000, () => evalJs("!!document.querySelector('[data-bb=status-chip].c-n')")).catch(() => false);
  check("the Concrete tab, then the ribbon's Column: placing on (the status chip)", !!tab && !!tool && placingOn === true, `tab ${tab} tool ${tool}`);
  const at = JSON.parse(await evalJs("JSON.stringify(window.__bbGl.toScreen ? window.__bbGl.toScreen(3000, 2530, 0) : null)"));
  let placed = null;
  if (at) {
    const n0 = (await gl()).toolCalls;
    await mouse("mouseMoved", at);
    await waitFor("the hover's tool answer", 5000, async () => (await gl()).toolCalls > n0).catch(() => null);
    await mouse("mousePressed", at);
    await mouse("mouseReleased", at);
    placed = await waitFor("the placed column drawn", 5000, async () => { const g = await gl(); return g.placed && g.meshRev === 2 ? g.placed : null; }).catch(() => null);
  }
  await S("Input.dispatchKeyEvent", { type: "keyDown", key: "Escape", code: "Escape", windowsVirtualKeyCode: 27 });
  await S("Input.dispatchKeyEvent", { type: "keyUp", key: "Escape", code: "Escape", windowsVirtualKeyCode: 27 });
  await mouse("mouseMoved", [5, 5]);
  const placingOff = await waitFor("placing off", 5000, () => evalJs("!document.querySelector('[data-bb=status-chip].c-n')")).catch(() => false);
  await settled(evalJs, "the picture of the place", await loops(evalJs), { rev: "mesh" }).catch(() => null); // TEST1: was sleep(300); then one read
  const h1 = await evalJs(hash);
  const t1 = await savedText();
  check("place: a click on a free spot -> one IfcColumn in the saved text, the mesh rev 2, the picture changed",
    !!placed && t1.includes(placed) && h1 !== h0, `at ${JSON.stringify(at)}, placed ${placed}, hash ${h0} -> ${h1}`);
  check("Escape: placing off", placingOff === true);

  // Undo: the quick bar's button.
  const undoAt = await centre("[data-bb=quickbar-undo]");
  const undoOn = await evalJs("document.querySelector('[data-bb=quickbar-undo]')?.disabled === false");
  if (undoAt) await click(undoAt);
  await mouse("mouseMoved", [5, 5]);
  const g2 = await waitFor("the mesh after undo", 5000, async () => { const g = await gl(); return g.meshRev === 3 ? g : null; }).catch(() => null);
  await settled(evalJs, "the picture of the undo", await loops(evalJs), { rev: "mesh" }).catch(() => null); // TEST1: was sleep(300); then one read
  const h2 = await evalJs(hash);
  const t2 = await savedText();
  check("undo (quick bar): the column leaves the saved text, the picture is the one before the place",
    undoOn && g2 !== null && !t2.startsWith("error") && placed && !t2.includes(placed) && count(t1, '"IfcColumn"') === count(t2, '"IfcColumn"') + 1 && h2 === h0,
    `button enabled ${undoOn}, rev ${g2?.meshRev}, IfcColumn ${count(t1, '"IfcColumn"')} -> ${count(t2, '"IfcColumn"')}, hash ${h2} (before the place ${h0})`);

  // ---- /c/?view=1: the viewer-only build; a verb is refused ----
  const warn0 = consoleLog.length;
  await S("Page.navigate", { url: `${ORIGIN}/c/?view=1&edit=move-first-beam` });
  await waitFor("the new page", 10000, async () => (await evalJs("location.search")).includes("view=1"));
  const vready = await boot("/c/?view=1");
  const vc = JSON.parse(await evalJs("JSON.stringify(window.__bbCcore ?? null)"));
  const vg = await gl();
  await settled(evalJs, "the view-only build's first picture", await loops(evalJs), { rev: "mesh" }); // TEST1: was sleep(300)
  const vshare = await evalJs(concrete);
  check("/c/?view=1 boots on ccore-view.wasm (its size) and draws a frame",
    vready === true && vc?.wasmBytes === wasmView && vg?.meshBytes > 0 && vshare > 0.05, `${JSON.stringify(vc)}, ${vg?.meshBytes} bytes, ${(100 * vshare).toFixed(1)} %`);
  const refused = consoleLog.slice(warn0).filter((m) => /this build is view-only/.test(m.text));
  check("the deep link's move verb is answered \"this build is view-only\", the mesh stays rev 1",
    refused.length >= 1 && (await gl()).meshRev === 1, refused.map((m) => `[${m.level}] ${m.text}`).join(" | ") || "no answer logged");

  // ---- everything ----
  const isFavicon = (u) => u === `${ORIGIN}/favicon.ico`;
  const outside = requests.filter((u) => !u.startsWith(`${ORIGIN}/c/`) && u !== `${ORIGIN}/c` && !/^(data|blob):/.test(u) && !isFavicon(u));
  const outsideServed = served.filter(([, p]) => !p.startsWith("/c/") && p !== "/c" && p !== "/favicon.ico");
  console.log(`requests: ${requests.length} by the page, ${served.length} served (${served.filter(([s]) => s === 200).length} files)`);
  check("no request leaves /c/ (the browser's /favicon.ico aside)", outside.length === 0 && outsideServed.length === 0,
    [...outside, ...outsideServed.map(([s, p]) => `${s} ${p}`)].join(" | "));
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
