// The 3D of a folder of links (ticket LNK3D, ADR 0078 amendment 3): CCORE_PORT_BASE=auto node ccore/test/lab_links_3d.mjs
// A native host (HOME, BIMBLE_CONFIG_DIR in a temp folder, BIMBLE_LINK_CACHE_MIN=0), six synthetic IFC made
// through it (bench/links_synth.mjs: bench/mesh_heavy_model.py's model, 160 closed meshes of about 600
// triangles each, no two alike: no instancing, GlobalIds per link, exported as IFC4), a new project, Link this folder: each cache holds
// its baked proxies (section HLOD; the report says its size and time). Five links in a row 25 m apart,
// the sixth 1.5 km behind them as the home camera looks; link_0 red, link_1 purple (colour overrides). Vite and ONE headless Chromium at
// 1280x800 over raw CDP on /?native=1&gl=1. Checks:
//   - the first picture is drawn from the links' proxies before their mesh arrives;
//   - the camera on the five near links: the far link is drawn from its proxies (its triangles a frame far
//     under its detail);
//   - hide a link (its eye in the Links panel), an element (view_rule), a class: the frame after the input
//     shows it gone and NO /mesh request was made, the draw lists not rebuilt; showing the link again gives
//     the same pixels as before (0 differing);
//   - move a pinned-off link by an offset: its pixels move, no /mesh request; a colour override: its
//     pixels in that colour, no /mesh request;
//   - a note on a linked element is drawn in 3D and follows the link's move; a click on its label selects it;
//   - each input's feedback within 200 ms (ADR 0074), the load printed; loaded (1-minute load over 4): that
//     input measured once more after the load falls under 4. The link's eye is clicked for real and timed by
//     test/lib/input_time.mjs (the page's time asserted at 200 ms; the presented frame by its rule); the other
//     inputs are the page's calls (view_rule, the Links panel's place and colour), timed from the call to the frame.
// Fails on the code before LNK3D: every hide or move fetched /mesh again (M1LINKS, LINKF), no proxies of
// links, no notes in 3D.
import { spawn } from "node:child_process";
import { mkdirSync, mkdtempSync, readFileSync, rmSync, statSync, existsSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { CHROMIUM, port, vitePort } from "./ports.mjs";
import { makeLinkFolder } from "../bench/links_synth.mjs";
import { adapter, readTimes, verdict, watchSource } from "./lib/input_time.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = process.env.LNK3D_UI ? resolve(process.env.LNK3D_UI) : resolve(CCORE, "../web-ui"); // (LNK3D_UI, HOST_BIN: the code before, to see it fail)
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("lab_links_3d");
const VITE = vitePort("lab_links_3d");
const TOKEN = "lablinks3d-token";
const PAGE = `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}`;
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-links3d-"));
const home = join(tmp, "home"), dir = join(home, "links");
const N = 6, PROXIES = 160, TRIS = 600, FAR_MM = 1500000, STEP_MM = 25000;
const procs = [];
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? `: ${detail}` : ""}`);
};
const loadNow = () => readFileSync("/proc/loadavg", "utf8").trim();
const load1 = () => Number(loadNow().split(" ")[0]);

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
// the temporary folder (six IFC of 16 MB, their caches, Chromium's profile) goes on a signal too
for (const sig of ["SIGINT", "SIGTERM", "SIGHUP"]) process.on(sig, () => { cleanup().finally(() => process.exit(130)); });
process.on("exit", () => {
  for (const p of procs) if (p.exitCode === null && p.signalCode === null) try { process.kill(-p.pid, "SIGKILL"); } catch {}
  try { rmSync(tmp, { recursive: true, force: true }); } catch {}
});
async function waitFor(what, ms, fn) {
  const until = Date.now() + ms;
  let last;
  while (Date.now() < until) {
    try { const v = await fn(); if (v) return v; } catch (e) { last = e; }
    await sleep(100);
  }
  throw new Error(`timeout waiting for ${what}${last ? `: ${last.message ?? last}` : ""}`);
}
async function cdpConnect(wsUrl) {
  const ws = new WebSocket(wsUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = () => rej(new Error("CDP connect failed")); });
  let id = 0;
  const pending = new Map(), listeners = [];
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

// In the page: pixels of a link colour (the palette's red, purple, green, shaded), their count and centroid.
const PIX = String.raw`((kind) => { const r = window.__bbGl.bench.read(), p = r.rgba; let n = 0, sx = 0, sy = 0;
  for (let i = 0; i < p.length; i += 4) { const R = p[i], G = p[i + 1], B = p[i + 2];
    const on = kind === "red" ? R - G > 60 && R - B > 60 : kind === "purple" ? B - G > 40 && R - G > 15 : G - B > 50 && G - R > 15;
    if (on) { n++; const k = i / 4; sx += k % r.w; sy += Math.floor(k / r.w); } }
  return { n, x: n ? sx / n : 0, y: n ? sy / n : 0 }; })`;

async function main() {
  if (!existsSync(join(UI, "node_modules"))) throw new Error(`no ${UI}/node_modules: run npm ci in web-ui/`);
  console.log(`loadavg ${loadNow()}`);
  mkdirSync(dir, { recursive: true });
  const host = start("host", BIN, [String(PORT)], {
    env: { ...process.env, BIMBLE_TOKEN: TOKEN, HOME: home, BIMBLE_CONFIG_DIR: join(tmp, "config"), BIMBLE_LINK_CACHE_MIN: "0",
      BIMBLE_CACHE_DIR: join(tmp, "cache"), BIMBLE_ORIGINS: `http://localhost:${VITE},http://127.0.0.1:${VITE}` },
  });
  await waitFor("host 'listening'", 5000, () => host.out.includes("listening") || (host.exitCode !== null && Promise.reject(new Error(host.out))));
  const hcall = async (op, name, body = {}) => {
    const r = await fetch(`http://127.0.0.1:${PORT}/${op}/${name}?t=${TOKEN}`, { method: "POST", body: JSON.stringify(body), headers: { "Content-Type": "text/plain" } });
    return { status: r.status, text: await r.text() };
  };
  const q = async (name, body = {}) => { const r = await hcall("query", name, body); if (r.status !== 200) throw new Error(`${name}: ${r.text}`); return JSON.parse(r.text); };
  const rows = async () => (await q("links_panel")).models;

  // ---- the links: made, linked, converted (each cache with its proxies) ----
  const t0 = Date.now();
  const files = await makeLinkFolder({ hcall, dir, work: tmp, n: N, proxies: PROXIES, tris: TRIS, unique: true });
  console.log(`made ${files.length} IFC in ${Date.now() - t0} ms, ${(statSync(files[0]).size / 1e6).toFixed(1)} MB each`);
  check("a new project", (await hcall("action", "new")).status === 200);
  check("link the folder", (await hcall("action", "link_folder", { path: dir })).status === 200);
  await waitFor("the links ready", 120000, async () => { const r = await rows(); return r.length === N && r.every((m) => m.state === "ready") ? r : null; });
  const logs = files.map((f) => readFileSync(`${f}.log.txt`, "utf8"));
  const prx = logs.map((t) => /proxies \(HLOD, baked for the 3D view\): (\d+) cells, (\d+) triangles, (\d+) bytes, built in (\d+) ms/.exec(t));
  const detail = logs.map((t) => Number(/\ntriangles: (\d+)/.exec(t)?.[1] ?? 0));
  const cacheBytes = files.map((f) => statSync(`${f}.bimbleview`).size);
  console.log(`caches: ${cacheBytes.map((b) => (b / 1e6).toFixed(2)).join(", ")} MB; proxies: ${prx.map((m) => m ? `${m[1]} cells ${m[2]} tris ${(Number(m[3]) / 1e3).toFixed(0)} kB ${m[4]} ms` : "none").join("; ")}; detail ${detail.join(", ")} triangles`);
  check("each cache holds its baked proxies (the report says cells, triangles, bytes, time)", prx.every((m) => m && Number(m[1]) > 0 && Number(m[2]) > 0 && Number(m[2]) < detail[0] / 2));
  // placed: five in a row, the sixth far; link_0 red, link_1 blue
  for (let k = 0; k < N; k++) {
    await hcall("action", "manage_links", { op: "unpin", name: `link_${k}.ifc` });
    // the far one behind the near ones as the home camera looks (yaw -55 degrees: the eye toward (0.57, -0.82))
    // and as far below as it looks down (pitch 28 degrees): on the view's axis
    const far = [2 * STEP_MM - Math.cos(-55 * Math.PI / 180) * FAR_MM, -Math.sin(-55 * Math.PI / 180) * FAR_MM, -Math.tan(28 * Math.PI / 180) * FAR_MM].map(Math.round);
    const r = await hcall("action", "manage_links", { op: "place", name: `link_${k}.ifc`, move_mm: k < N - 1 ? [k * STEP_MM, 0, 0] : far });
    if (r.status !== 200) throw new Error(`place: ${r.text}`);
  }
  await hcall("action", "manage_links", { op: "colour", name: "link_0.ifc", colour: 0 });
  await hcall("action", "manage_links", { op: "colour", name: "link_1.ifc", colour: 3 });
  const draw0 = await q("link_draw");
  const near = [0, 1, 2].map((a) => draw0.slice(0, N - 1).reduce((s, l) => s + l.origin[a], 0) / (N - 1));

  // ---- the page ----
  const vite = start("vite", "npx", ["vite", "--port", String(VITE), "--strictPort"], { cwd: UI });
  await waitFor(`Vite on ${VITE}`, 30000, async () => { if (vite.exitCode !== null) throw new Error(vite.out); return (await fetch(`http://localhost:${VITE}/`)).ok; });
  console.log(`memory available ${(Number(/MemAvailable:\s+(\d+)/.exec(readFileSync("/proc/meminfo", "utf8"))[1]) / 1048576).toFixed(1)} GB; starting Chromium`);
  const chrome = start("chromium", CHROMIUM, ["--headless=new", "--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--remote-debugging-port=0",
    `--user-data-dir=${join(tmp, "chromium")}`, "--no-first-run", "--no-default-browser-check", "about:blank"]);
  const wsUrl = await waitFor("DevTools line", 60000, () => chrome.out.match(/DevTools listening on (ws:\/\/\S+)/)?.[1]);
  const cdp = await cdpConnect(wsUrl);
  const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank" });
  const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
  const S = (m, p) => cdp.send(m, p, sessionId);
  const consoleLog = [], exceptions = [], meshRequests = [];
  cdp.on((msg) => {
    if (msg.sessionId !== sessionId) return;
    if (msg.method === "Runtime.consoleAPICalled") consoleLog.push({ level: msg.params.type, text: msg.params.args.map((a) => a.value ?? a.description ?? a.type).join(" ") });
    else if (msg.method === "Runtime.exceptionThrown") exceptions.push(msg.params.exceptionDetails.exception?.description ?? msg.params.exceptionDetails.text);
    else if (msg.method === "Network.requestWillBeSent") { // the network log: every /mesh asked
      const u = msg.params.request.url;
      if (/\/mesh\?/.test(u)) meshRequests.push(u.replace(/t=[^&]*&?/, ""));
    }
  });
  await S("Runtime.enable");
  await S("Page.enable");
  await S("Network.enable");
  await S("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  await S("Page.navigate", { url: PAGE });
  const evalJs = async (expr) => {
    const r = await S("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
    return r.result.value;
  };
  const gl = async () => JSON.parse(await evalJs("JSON.stringify(window.__bbGl, (k, v) => k === 'frameLog' ? undefined : v)"));
  const calm = () => evalJs(`new Promise((res) => { let last = performance.now(), calm = 0; const f = (t) => { calm = t - last < 25 ? calm + 1 : 0; last = t; if (calm >= 3) res(); else requestAnimationFrame(f); }; requestAnimationFrame(f); })`);
  const pix = (kind) => evalJs(`${PIX}(${JSON.stringify(kind)})`);
  // an input timed from its start to the end of the first frame after it that meets `cond` (ADR 0074)
  const timedOnce = (startJs, cond) => evalJs(`new Promise((res) => { const g = window.__bbGl, f0 = g.frames, t0 = performance.now(); Promise.resolve((${startJs})());
    const tick = () => { if (g.frames > f0 && (${cond})) res(performance.now() - t0); else if (performance.now() - t0 > 8000) res(-1); else requestAnimationFrame(tick); }; requestAnimationFrame(tick); })`);
  const timings = [];
  const timed = async (what, startJs, cond, undoJs = null) => {
    await calm();
    let ms = await timedOnce(startJs, cond), l = loadNow();
    if ((ms < 0 || ms >= 200) && load1() > 4 && undoJs) { // loaded: once more under load 4
      await evalJs(`(${undoJs})()`);
      await waitFor("the load under 4", 120000, () => load1() < 4).catch(() => null);
      await calm();
      console.log(`  ${what}: ${ms.toFixed(1)} ms at load ${l}; measured again`);
      ms = await timedOnce(startJs, cond), l = loadNow();
    }
    timings.push([what, ms, l]);
    return ms;
  };

  await waitFor("the first drawn frame", 60000, () => evalJs("window.__bbGl && window.__bbGl.firstFrameMs"));
  const st0 = await waitFor("six links drawn", 60000, async () => { const s = await gl(); return s.links.length === N && s.linkTris > 0 && (s.linkProxies ?? []).length === N ? s : null; });
  check("the first picture is drawn from the links' proxies, before their mesh arrives", st0.proxyPreview?.links === N && st0.proxyPreview.at <= st0.meshAt,
    `proxies ${st0.proxyPreview?.at?.toFixed(0)} ms (${st0.proxyPreview?.tris} triangles), mesh ${st0.meshAt?.toFixed(0)} ms, links' proxies asked in ${st0.linkProxiesMs?.toFixed(0)} ms`);
  await evalJs(`window.__bbGl.bench.look(${JSON.stringify(near)}, 140000)`);
  await calm();
  await sleep(400);
  await calm();
  const stillMs = await evalJs("window.__bbGl.bench.frame(0, false)"); // a whole still frame, its numbers
  const st1 = await gl();
  const far = st1.linkProxies.find((x) => x.name === `link_${N - 1}.ifc`);
  check("the camera on the near links: the far link drawn from its proxies (its triangles far under its detail)", far && far.drawnTris > 0 && far.drawnTris < detail[N - 1] / 4,
    `${far?.drawnTris} of ${detail[N - 1]} triangles; still frame ${stillMs.toFixed(0)} ms, ${st1.frameTris} triangles, ${st1.frameDraws} draws, GPU ${(st1.gpuBytes / 1e6).toFixed(1)} MB`);
  const fetches0 = st1.meshFetches, lists0 = st1.listsBuilt, req0 = meshRequests.filter((u) => !/proxies/.test(u)).length;
  const noRefetch = async (what) => {
    const s = await gl(), req = meshRequests.filter((u) => !/proxies/.test(u)).length;
    check(`${what}: no /mesh request, the draw lists not rebuilt`, s.meshFetches === fetches0 && s.listsBuilt === lists0 && req === req0,
      `fetches ${fetches0} -> ${s.meshFetches}, lists ${lists0} -> ${s.listsBuilt}, /mesh requests ${req0} -> ${req}`);
  };
  const red0 = await pix("red"), pur0 = await pix("purple");
  check("link_0 drawn red and link_1 purple (their colour overrides)", red0.n > 200 && pur0.n > 200, `red ${red0.n} px, purple ${pur0.n} px`);
  const before = await evalJs("Array.from(window.__bbGl.bench.read().rgba)");

  // ---- hide a link: its eye in the Links panel ----
  await evalJs("(async () => (await import('/src/bimble.ts')).runAction('manage_links'))()");
  await waitFor("the Links panel rows", 10000, () => evalJs(`!!document.querySelector('[data-bb=link-row][data-name="link_1.ifc"] [data-bb=link-eye]')`));
  // the eye clicked for real (CDP input), timed by test/lib/input_time.mjs (ADR 0074, FIRST1): the page's time
  // (the row and the viewport's frame without the link) asserted at 200 ms, the presented frame as its rule says
  const gpu = await adapter(evalJs);
  const eyeClick = async (visible) => {
    const rect = `(() => { const r = document.querySelector('[data-bb=link-row][data-name="link_1.ifc"] [data-bb=link-eye]').getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; })()`;
    // (the panel may hold it below the fold: scrolled into view, every scroll put back after the click)
    await evalJs(`(() => { const e = document.querySelector('[data-bb=link-row][data-name="link_1.ifc"] [data-bb=link-eye]'); window.__scrolls = [];
      for (let p = e.parentElement; p; p = p.parentElement) window.__scrolls.push([p, p.scrollTop, p.scrollLeft]); e.scrollIntoView({ block: "center" }); })()`);
    await calm();
    let at = await evalJs(rect); // where it stays (the panel laid out)
    for (let i = 0; i < 20; i++) { await sleep(100); const b = await evalJs(rect); if (b[0] === at[0] && b[1] === at[1]) break; at = b; }
    const top = await evalJs(`!!document.elementFromPoint(${at[0]}, ${at[1]})?.closest('[data-bb=link-eye]')`);
    if (!top) throw new Error(`the eye of link_1 is covered at ${at}`);
    const cond = `document.querySelector('[data-bb=link-row][data-name="link_1.ifc"]')?.dataset.visible === '${visible ? 1 : 0}' && window.__bbGl.linkDraw[1].visible === ${visible}`;
    await evalJs(watchSource({ kind: "pointerdown", page: cond, presented: cond }));
    await S("Input.dispatchMouseEvent", { type: "mouseMoved", x: at[0], y: at[1], button: "none", buttons: 0 });
    await S("Input.dispatchMouseEvent", { type: "mousePressed", x: at[0], y: at[1], button: "left", buttons: 1, clickCount: 1 });
    await S("Input.dispatchMouseEvent", { type: "mouseReleased", x: at[0], y: at[1], button: "left", buttons: 0, clickCount: 1 });
    const tm = await readTimes(evalJs);
    await evalJs("(() => { for (const [p, t, l] of window.__scrolls ?? []) p.scrollTop = t, p.scrollLeft = l; })()");
    return verdict(tm, gpu);
  };
  const hideV = await eyeClick(false);
  check(`hide a link (its eye clicked): ${hideV.line}`, hideV.ok);
  const pur1 = await pix("purple");
  check("hide a link: the frame after the click shows it gone", hideV.pageOk && pur1.n === 0, `purple ${pur0.n} -> ${pur1.n} px`);
  await noRefetch("hide a link");
  const showV = await eyeClick(true);
  check(`show the link again (its eye clicked): ${showV.line}`, showV.ok);
  const showMs = showV.pageOk ? 0 : -1;
  await calm();
  const after = await evalJs("Array.from(window.__bbGl.bench.read().rgba)");
  let diff = 0;
  for (let i = 0; i < before.length; i += 4) if (Math.max(Math.abs(before[i] - after[i]), Math.abs(before[i + 1] - after[i + 1]), Math.abs(before[i + 2] - after[i + 2])) > 40) diff++;
  check("shown again: the same picture as before the hide (0 pixels differ by more than 40 levels)", showMs >= 0 && diff === 0, `${diff} pixels`);
  await noRefetch("show the link");

  // ---- hide an element of a link (the Navigator's rule) ----
  const hit = await evalJs(`(() => { const r = document.getElementById("bimble").getBoundingClientRect(), g = window.__bbGl;
    for (let y = r.top + 40; y < r.bottom - 40; y += 9) for (let x = r.left + 40; x < r.right - 40; x += 9) { const h = g.pickInfoAt(x, y); if (h && h.link === "link_2.ifc") return { x, y, ...h }; } return null; })()`);
  check("an element of link_2 under some pixel", hit !== null, JSON.stringify(hit));
  if (hit) {
    const hideEl = `() => import('/src/bimble.ts').then((b) => b.viewRule("hide", { element: ${JSON.stringify(hit.id)} }))`;
    const ms = await timed("hide an element of a link", hideEl, `window.__bbGl.pickInfoAt(${hit.x}, ${hit.y})?.id !== ${JSON.stringify(hit.id)}`);
    check("hide an element of a link: the frame after it shows it gone (not picked there)", ms >= 0, `${ms.toFixed(1)} ms`);
    await noRefetch("hide an element");
    await evalJs("(async () => (await import('/src/bimble.ts')).runAction('show_all'))()");
    await waitFor("shown again", 5000, () => evalJs(`window.__bbGl.pickInfoAt(${hit.x}, ${hit.y})?.id === ${JSON.stringify(hit.id)}`));
  }

  // ---- hide a class (every link's proxies, IfcBuildingElementProxy) ----
  await calm();
  const tris0 = (await gl()).drawnTris;
  const hideCls = `() => import('/src/bimble.ts').then((b) => b.viewRule("hide", { class: "IfcBuildingElementProxy" }))`;
  const clsMs = await timed("hide a class", hideCls, `window.__bbGl.drawnTris < ${tris0} / 3`);
  const tris1 = (await gl()).drawnTris;
  check("hide a class: the frame after it draws without them", clsMs >= 0 && tris1 < tris0 / 3, `${clsMs.toFixed(1)} ms, triangles shown ${tris0} -> ${tris1}`);
  await noRefetch("hide a class");
  await evalJs("(async () => (await import('/src/bimble.ts')).runAction('show_all'))()");
  await waitFor("the class shown again", 5000, async () => (await gl()).drawnTris === tris0);

  // ---- move a pinned-off link by an offset; a colour override ----
  const move = (dy) => `() => import('/src/shell/links.ts').then((l) => l.modelSet("place", "link_0.ifc", { move_mm: [0, ${dy}, 0] }))`;
  const moveMs = await timed("move a link by an offset", move(8000), "window.__bbGl.linkDraw[0].xf.t[1] - window.__bbGl.linkDraw[1].xf.t[1] > 7999", move(0));
  await calm();
  const red1 = await pix("red");
  check("move a link: its pixels move", moveMs >= 0 && red1.n > 0 && Math.hypot(red1.x - red0.x, red1.y - red0.y) > 10,
    `${moveMs.toFixed(1)} ms, red centroid (${red0.x.toFixed(0)}, ${red0.y.toFixed(0)}) -> (${red1.x.toFixed(0)}, ${red1.y.toFixed(0)})`);
  await noRefetch("move a link");
  const colourMs = await timed("a link's colour", `() => import('/src/shell/links.ts').then((l) => l.modelSet("colour", "link_2.ifc", { colour: 2 }))`,
    "window.__bbGl.linkDraw[2].colour === 2");
  const green = await pix("green");
  check("a colour override: the link in that colour", colourMs >= 0 && green.n > 200, `${colourMs.toFixed(1)} ms, green ${green.n} px`);
  await noRefetch("a colour override");

  // ---- a note on a linked element, in 3D, following its link ----
  const els = await q("scene_elements", { link: "link_0.ifc", storey: "Niveau 1", class: "IfcColumn", limit: 1 }).catch(() => []);
  const tree = els.length ? null : await q("scene_tree", { link: "link_0.ifc" });
  const el = els[0]?.id ?? (await q("scene_elements", { link: "link_0.ifc", storey: tree.storeys[0].name, limit: 1 }))[0]?.id;
  const nr = await hcall("action", "manage_links", { op: "note_add", name: "link_0.ifc", element: el, text: "LNK3D note" });
  check("a note on a linked element", nr.status === 200, nr.text);
  const n0 = await waitFor("the note drawn", 10000, async () => { const s = await gl(); return s.notes?.[0]?.drawn ? s.notes[0] : null; });
  check("the note drawn in 3D (leader and label on the overlay)", n0.drawn && n0.text === "LNK3D note", `at (${n0.x.toFixed(0)}, ${n0.y.toFixed(0)})`);
  const red2 = await pix("red");
  await timed("move the link with the note", move(16000), "window.__bbGl.linkDraw[0].xf.t[1] - window.__bbGl.linkDraw[1].xf.t[1] > 15999", move(8000));
  await calm();
  const n1 = (await gl()).notes[0], red3 = await pix("red");
  const dn = [n1.x - n0.x, n1.y - n0.y], dr = [red3.x - red2.x, -(red3.y - red2.y)]; // read() is bottom row first
  check("the note follows its link's move (no core round trip)", n1.drawn && Math.hypot(...dn) > 10 && Math.hypot(dn[0] - dr[0], dn[1] - dr[1]) < 0.5 * Math.hypot(...dn) + 15,
    `note moved (${dn[0].toFixed(0)}, ${dn[1].toFixed(0)}) px, the link's red (${dr[0].toFixed(0)}, ${dr[1].toFixed(0)}) px`);
  await noRefetch("move the link with the note");
  const lab = [n1.x + 24, n1.y - 26];
  await calm();
  await S("Input.dispatchMouseEvent", { type: "mouseMoved", x: lab[0], y: lab[1], button: "none", buttons: 0 });
  await S("Input.dispatchMouseEvent", { type: "mousePressed", x: lab[0], y: lab[1], button: "left", buttons: 1, clickCount: 1 });
  await S("Input.dispatchMouseEvent", { type: "mouseReleased", x: lab[0], y: lab[1], button: "left", buttons: 0, clickCount: 1 });
  const sel = await waitFor("the note selected", 5000, async () => { const s = await gl(); return s.noteSelected === 0 ? s : null; }).catch(() => null);
  const linked = await waitFor("its element selected", 5000, () => evalJs("(async () => (await import('/src/bimble.ts')).state.value.linked)()")).catch(() => null);
  check("a click on the note's label selects it (and its element)", sel !== null && linked?.id === el && linked?.link === "link_0.ifc",
    `note ${sel?.noteSelected}, linked ${JSON.stringify(linked)}, clicked at (${lab[0].toFixed(0)}, ${lab[1].toFixed(0)})`);

  // ---- the GPU memory budget across links (D) ----
  await calm();
  const gpu0 = (await gl()).gpuBytes, before3 = await evalJs("Array.from(window.__bbGl.bench.read().rgba)");
  await hcall("action", "manage_links", { op: "hide", name: "link_3.ifc" });
  await waitFor("link_3 hidden in the page", 5000, async () => (await gl()).linkDraw?.[3]?.visible === false);
  await evalJs(`window.__bbGl.gpuBudget = ${gpu0 - 1000}`);
  const rel = await waitFor("a link's detail released", 5000, async () => { const s = await gl(); return s.linkDetail?.find((x) => x.name === "link_3.ifc")?.detail === false ? s : null; }).catch(() => null);
  const panel = await waitFor("the Links panel says it", 5000, () => evalJs(`document.querySelector('[data-bb=link-row][data-name="link_3.ifc"] [data-bb=link-detail]')?.dataset.detail === '0' ? document.querySelector('[data-bb=link-row][data-name="link_3.ifc"] [data-bb=link-detail]').textContent : null`)).catch(() => null);
  check("the budget set low: the hidden link's detail leaves the GPU; the Links panel says \"proxies only\"", rel !== null && rel.gpuBytes < gpu0 && panel === "proxies only",
    `GPU ${(gpu0 / 1e6).toFixed(2)} -> ${((rel?.gpuBytes ?? 0) / 1e6).toFixed(2)} MB, panel ${JSON.stringify(panel)}, ${JSON.stringify(rel?.linkDetail?.map((x) => [x.name, x.detail]))}`);
  const showBack = `() => import('/src/shell/links.ts').then((l) => l.modelOp("show", "link_3.ifc"))`;
  const backMs = await timed("show a link whose detail was released", showBack, `window.__bbGl.linkDetail?.find((x) => x.name === "link_3.ifc")?.detail === true && window.__bbGl.linkDraw[3].visible`);
  await calm();
  const after3 = await evalJs("Array.from(window.__bbGl.bench.read().rgba)"), st3 = await gl();
  let d3 = 0;
  for (let i = 0; i < before3.length; i += 4) if (Math.max(Math.abs(before3[i] - after3[i]), Math.abs(before3[i + 1] - after3[i + 1]), Math.abs(before3[i + 2] - after3[i + 2])) > 40) d3++;
  check("shown again: its detail back on the GPU (from the page's copy), the same picture as before the hide", backMs >= 0 && d3 === 0,
    `${backMs.toFixed(1)} ms (its upload ${st3.linkReloadMs?.toFixed(1)} ms), ${d3} pixels differ; ${JSON.stringify(st3.linkDetail?.map((x) => [x.name, x.detail, x.loads]))}`);
  await noRefetch("the budget's releases and uploads");
  const sel4 = await evalJs(`(() => { const r = document.getElementById("bimble").getBoundingClientRect(), g = window.__bbGl;
    for (let y = r.top + 40; y < r.bottom - 40; y += 9) for (let x = r.left + 40; x < r.right - 40; x += 9) { const h = g.pickInfoAt(x, y); if (h && h.link === "link_4.ifc") return h; } return null; })()`);
  if (sel4) await evalJs(`import('/src/bimble.ts').then((b) => b.selectLinked("link_4.ifc", ${JSON.stringify(sel4.id)}))`);
  await waitFor("link_4's element selected", 5000, () => evalJs("(async () => (await import('/src/bimble.ts')).state.value.linked?.link === 'link_4.ifc')()"));
  // the camera close on link_0: link_4 (100 m away) out of view, then not needed after 2 s, but selected
  await evalJs(`window.__bbGl.bench.look(${JSON.stringify(draw0[0].origin)}, 20000)`);
  await calm();
  await sleep(2600);
  await evalJs("window.__bbGl.gpuBudget = 0");
  await sleep(600);
  await calm();
  const st4 = await gl();
  const det = Object.fromEntries((st4.linkDetail ?? []).map((x) => [x.name, x.detail]));
  check("a budget of 0, link_4 out of view for 2 s: its detail kept (its element selected); links out of view released, the one on screen kept",
    det["link_4.ifc"] === true && det["link_0.ifc"] === true && det["link_3.ifc"] === false && det["link_2.ifc"] === false, JSON.stringify(det));
  await evalJs(`window.__bbGl.bench.look(${JSON.stringify(near)}, 140000)`);
  await evalJs("delete window.__bbGl.gpuBudget");

  // ---- the numbers ----
  const st2 = await gl();
  console.log(`GPU ${(st2.gpuBytes / 1e6).toFixed(1)} MB (links' proxies ${(st2.linkProxies.reduce((a, x) => a + x.bytes, 0) / 1e6).toFixed(2)} MB); state upload: ${JSON.stringify(st2.stateUpload)}, visibility in the page ${st2.visApplyMs?.toFixed(1)} ms`);
  for (const [what, ms, l] of timings) {
    check(`${what}: feedback within 200 ms (ADR 0074)`, ms >= 0 && ms < 200, `${ms.toFixed(1)} ms, load ${l}`);
  }
  const errors = consoleLog.filter((m) => /error/.test(m.level) && !/favicon|Synchronous XMLHttpRequest/.test(m.text));
  check("no uncaught page error", exceptions.length === 0, exceptions.join(" | "));
  check("no console error", errors.length === 0, errors.map((m) => m.text).join(" | "));
  console.log(`/mesh requests: ${meshRequests.length} (${meshRequests.map((u) => u.replace(/^.*\/mesh\?/, "")).join(", ")})`);
  console.log(`loadavg ${loadNow()}`);
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
