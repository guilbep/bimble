// WebGPU draws linked models (ticket GPU2, ADR 0083 amended): CCORE_PORT_BASE=auto node ccore/test/lab_webgpu_links.mjs
// A native host (HOME, BIMBLE_CONFIG_DIR in a temp folder, BIMBLE_LINK_CACHE_MIN=0), six synthetic IFC made through
// it (bench/links_synth.mjs: 160 closed meshes of about 600 triangles each, no two alike), a new project, Link this
// folder (each cache with its baked proxies); five links in a row 25 m apart, the sixth 1.5 km behind them; link_0
// red, link_1 purple. Vite and ONE headless Chromium at 1280x800 with a WebGPU adapter (lib/chromium.mjs: SwiftShader's
// fallback adapter, accepted with ?gpuSoftware=1, by default; the machine's GPU under LAB_GPU=vulkan). Checks:
//   - pictures of WebGPU against WebGL2 at three fixed cameras, the renderer switched LIVE between them (the toggle:
//     perf.ts chooseRenderer), in the states: plain (the far link from its proxies), a link hidden, an element of a
//     link hidden, a class isolated, a link moved and another recoloured, the budget at 0 (the far link released:
//     drawn from its coarsest proxies), a section box around a linked element. Tolerance as lab_webgpu.mjs: per
//     picture at most 1 % of the pixels differ by more than 24 levels (the counts and the largest difference printed);
//   - WebGPU holds the links itself: every loaded link taken over, WebGL2's buffers for them emptied (the bytes each
//     renderer holds printed: "GPU bytes with one renderer's buffers freed");
//   - no draw lists rebuilt and no /mesh asked by a switch;
//   - a pick on a link under WebGPU (its triangles read back from WebGPU's buffers: the first ray starts the read,
//     the next finds it);
//   - the live switch back and forth 10 times leaks no GPU memory: each renderer's bytes back to the same value
//     within 1 %; the switch's time (the toggle to the end of the other renderer's first frame) printed.
// Fails before GPU2: with links loaded WebGPU stepped aside ("linked models are drawn by WebGL2 for now"): the
// WebGPU pictures could not be taken. Prints "N passed, M failed"; exits non-zero on a failure.
import { spawn } from "node:child_process";
import { mkdirSync, mkdtempSync, readFileSync, rmSync, existsSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { port, vitePort } from "./ports.mjs";
import { CHROMIUM, LAB_GPU, gpuFlags, webgpuFlags } from "./lib/chromium.mjs";
import { makeLinkFolder } from "../bench/links_synth.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = resolve(CCORE, "../web-ui");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("lab_webgpu_links");
const VITE = vitePort("lab_webgpu_links");
const TOKEN = "labwebgpulinks-token";
const SOFT = LAB_GPU === "software";
const PAGE = `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}&perf=1${SOFT ? "&gpuSoftware=1" : ""}`;
const tmp = mkdtempSync(join(tmpdir(), "gpu2-lab-webgpu-links-"));
const home = join(tmp, "home"), dir = join(home, "links");
const N = 6, PROXIES = 160, TRIS = 600, FAR_MM = 1500000, STEP_MM = 25000;
const TOL_LEVELS = 24, TOL_SHARE = 0.01, BG = [247, 247, 245];
const CAMS = [[0, 0], [0.6, -0.3], [2.0, 0.4]]; // (yaw turned, distance from 140 m times 1 + k)
const procs = [];
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let passed = 0, failed = 0;
const check = (name, ok, detail = "") => { if (ok) passed++; else failed++; console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? `: ${detail}` : ""}`); };
const loadNow = () => readFileSync("/proc/loadavg", "utf8").trim();

function start(name, cmd, args, opts) {
  const p = spawn(cmd, args, { stdio: ["ignore", "pipe", "pipe"], detached: true, ...opts });
  p.out = "";
  p.stdout.on("data", (d) => (p.out += d)), p.stderr.on("data", (d) => (p.out += d));
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
      const { resolve: ok, reject } = pending.get(msg.id);
      pending.delete(msg.id);
      if (msg.error) reject(new Error(`${msg.error.message} ${msg.error.data ?? ""}`));
      else ok(msg.result);
    } else if (msg.method) for (const fn of listeners) fn(msg);
  };
  const send = (method, params = {}, sessionId) => new Promise((ok, reject) => {
    const m = ++id;
    pending.set(m, { resolve: ok, reject });
    ws.send(JSON.stringify({ id: m, method, params, ...(sessionId ? { sessionId } : {}) }));
  });
  return { send, on: (fn) => listeners.push(fn), close: () => ws.close() };
}
const decode = (p) => ({ w: p.w, h: p.h, rgba: Buffer.from(p.b64, "base64") });
const inked = (p) => { let n = 0; for (let i = 0; i < p.rgba.length; i += 4) if (Math.abs(p.rgba[i] - BG[0]) > 6 || Math.abs(p.rgba[i + 1] - BG[1]) > 6 || Math.abs(p.rgba[i + 2] - BG[2]) > 6) n++; return n; };
function compare(a, b) {
  if (a.w !== b.w || a.h !== b.h) return { size: `${a.w}x${a.h} vs ${b.w}x${b.h}`, over: Infinity, any: Infinity, max: 255, share: 1 };
  let over = 0, any = 0, max = 0;
  for (let i = 0; i < a.rgba.length; i += 4) {
    const d = Math.max(Math.abs(a.rgba[i] - b.rgba[i]), Math.abs(a.rgba[i + 1] - b.rgba[i + 1]), Math.abs(a.rgba[i + 2] - b.rgba[i + 2]));
    if (d > 0) any++;
    if (d > TOL_LEVELS) over++;
    if (d > max) max = d;
  }
  return { over, any, max, share: over / (a.w * a.h) };
}

async function main() {
  console.log(`loadavg ${loadNow()}; LAB_GPU=${LAB_GPU}`);
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
  await makeLinkFolder({ hcall, dir, work: tmp, n: N, proxies: PROXIES, tris: TRIS, unique: true });
  check("a new project, the folder linked", (await hcall("action", "new")).status === 200 && (await hcall("action", "link_folder", { path: dir })).status === 200);
  await waitFor("the links ready", 120000, async () => { const r = (await q("links_panel")).models; return r.length === N && r.every((m) => m.state === "ready") ? r : null; });
  for (let k = 0; k < N; k++) {
    await hcall("action", "manage_links", { op: "unpin", name: `link_${k}.ifc` });
    const far = [2 * STEP_MM - Math.cos(-55 * Math.PI / 180) * FAR_MM, -Math.sin(-55 * Math.PI / 180) * FAR_MM, -Math.tan(28 * Math.PI / 180) * FAR_MM].map(Math.round);
    await hcall("action", "manage_links", { op: "place", name: `link_${k}.ifc`, move_mm: k < N - 1 ? [k * STEP_MM, 0, 0] : far });
  }
  await hcall("action", "manage_links", { op: "colour", name: "link_0.ifc", colour: 0 });
  await hcall("action", "manage_links", { op: "colour", name: "link_1.ifc", colour: 3 });
  const draw0 = await q("link_draw");
  const near = [0, 1, 2].map((a) => draw0.slice(0, N - 1).reduce((s, l) => s + l.origin[a], 0) / (N - 1));

  const vite = start("vite", "npx", ["vite", "--port", String(VITE), "--strictPort"], { cwd: UI });
  await waitFor(`Vite on ${VITE}`, 30000, async () => { if (vite.exitCode !== null) throw new Error(vite.out); return (await fetch(`http://localhost:${VITE}/`)).ok; });
  console.log(`memory available ${(Number(/MemAvailable:\s+(\d+)/.exec(readFileSync("/proc/meminfo", "utf8"))[1]) / 1048576).toFixed(1)} GB; starting Chromium`);
  const chrome = start("chromium", CHROMIUM, ["--headless=new", ...gpuFlags(), ...webgpuFlags(), "--remote-debugging-port=0",
    `--user-data-dir=${join(tmp, "chromium")}`, "--no-first-run", "--no-default-browser-check", "about:blank"]);
  const wsUrl = await waitFor("DevTools line", 60000, () => chrome.out.match(/DevTools listening on (ws:\/\/\S+)/)?.[1]);
  const cdp = await cdpConnect(wsUrl);
  const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank" });
  const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
  const S = (m, p) => cdp.send(m, p, sessionId);
  const exceptions = [], meshRequests = [], warnings = [];
  cdp.on((msg) => {
    if (msg.sessionId !== sessionId) return;
    if (msg.method === "Runtime.exceptionThrown") exceptions.push(msg.params.exceptionDetails.exception?.description ?? msg.params.exceptionDetails.text);
    else if (msg.method === "Runtime.consoleAPICalled" && /warn|error/.test(msg.params.type)) warnings.push(msg.params.args.map((a) => a.value ?? a.description).join(" "));
    else if (msg.method === "Network.requestWillBeSent" && /\/mesh\?/.test(msg.params.request.url)) meshRequests.push(msg.params.request.url);
  });
  await S("Runtime.enable"), await S("Page.enable"), await S("Network.enable");
  await S("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  await S("Page.navigate", { url: PAGE });
  const evalJs = async (expr) => {
    const r = await S("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
    return r.result.value;
  };
  const gl = async () => JSON.parse(await evalJs("JSON.stringify(window.__bbGl, (k, v) => k === 'frameLog' ? undefined : v)"));
  const R = () => evalJs("(() => { const r = window.__bbPerf.snapshot().renderer; return { using: r.using, reason: r.reason, progress: r.progress ?? null, switchMs: r.switchMs ?? null }; })()");
  const calm = () => evalJs(`new Promise((res) => { let last = performance.now(), calm = 0; const f = (t) => { calm = t - last < 25 ? calm + 1 : 0; last = t; if (calm >= 3) res(); else requestAnimationFrame(f); }; requestAnimationFrame(f); })`);
  /** The renderer switched live and settled (WebGPU: every loaded link taken over); the switch's time. */
  const use = async (to) => {
    await evalJs(`(async () => (await import('/src/perf.ts')).chooseRenderer(${JSON.stringify(to)}))()`);
    const want = to === "webgpu" ? "WebGPU" : "WebGL2";
    let seen = null;
    await waitFor(`${want} drawing`, 60000, async () => {
      const r = await R(), s = await evalJs("window.__bbGl.gpuSplit ?? null");
      seen = { ...r, split: s };
      return r.using === want && !r.progress && (to === "webgl2" || (s && s.taken === s.models)) ? r : null;
    }).catch((e) => { throw new Error(`${e.message}; last seen ${JSON.stringify(seen)}; warnings ${warnings.slice(-3).join(" | ")}`); });
    await evalJs("window.__bbGl.bench.frame(0, false)");
    await calm();
    return (await waitFor("the switch's time", 10000, async () => (await R()).switchMs).catch(() => null));
  };
  /** The picture at a camera (the near links' view turned by a yaw, at a distance), once the fill after the move ended. */
  const snap = async (gpu, [dy, k]) => {
    await evalJs(`(window.__bbGl.bench.look(${JSON.stringify(near)}, ${Math.round(140000 * (1 + k))}), 0)`);
    await calm();
    await evalJs(`(window.__bbGl.bench.frame(${dy}, false), 0)`);
    await waitFor("the picture complete", 20000, async () => { await calm(); await sleep(500); await calm(); return evalJs("(() => { const m = window.__bbGl.moving; return !!m && (!m.moving || (m.t === 0 && m.outlines === 0 && !m.proxied)); })()"); });
    return decode(await evalJs(`(async () => { let w, h, rgba;
    if (${gpu}) { await window.__bbGpu.done(); const p = await window.__bbGpu.pixels(); w = p.w, h = p.h, rgba = p.rgba; }
    else { const s = window.__bbGl.bench.read(); w = s.w, h = s.h; rgba = new Uint8Array(s.rgba.length); for (let y = 0; y < h; y++) rgba.set(s.rgba.subarray(4 * w * (h - 1 - y), 4 * w * (h - y)), 4 * w * y); }
    let bin = ""; for (let i = 0; i < rgba.length; i += 32768) bin += String.fromCharCode(...rgba.subarray(i, i + 32768));
    return { w, h, b64: btoa(bin) }; })()`));
  };
  const B = (body) => evalJs(`(async () => { const b = await import('/src/bimble.ts'); const l = await import('/src/shell/links.ts'); ${body}; return 0; })()`);

  await waitFor("the first drawn frame", 60000, () => evalJs("window.__bbGl && window.__bbGl.firstFrameMs"));
  await waitFor("six links drawn, their arrays dropped", 120000, async () => { const s = await gl(); return s.links.length === N && (s.partBytes ?? []).length === N + 1 && s.partBytes.slice(1).every((b) => b === 0) ? s : null; });
  await waitFor("their detail on the GPU", 60000, async () => { const s = await gl(); return (s.linkDetail ?? []).filter((x) => x.detail).length === N ? s : null; }).catch(() => null);
  const adapter = await evalJs("(async () => { const a = await navigator.gpu?.requestAdapter(); return a ? [a.info.vendor, a.info.architecture, a.info.isFallbackAdapter] : null; })()");
  console.log(`WebGPU adapter ${JSON.stringify(adapter)}`);
  // an element of link_2 and of link_4 under some pixel (WebGL2's pick)
  await use("webgl2");
  const find = (link) => evalJs(`(() => { const r = document.getElementById("bimble").getBoundingClientRect(), g = window.__bbGl;
    for (let y = r.top + 40; y < r.bottom - 40; y += 9) for (let x = r.left + 40; x < r.right - 40; x += 9) { const h = g.pickInfoAt(x, y); if (h && h.link === ${JSON.stringify(link)}) return { x, y, ...h }; } return null; })()`);
  await evalJs(`(window.__bbGl.bench.look(${JSON.stringify(near)}, 140000), window.__bbGl.bench.frame(0, false), 0)`);
  // (a link's detail may still be on its way: SIZE3 asks the links' meshes as they are needed; a pick finds it once it is in)
  const findSoon = (link) => waitFor(`an element of ${link} under a pixel`, 20000, async () => { await evalJs("(window.__bbGl.bench.frame(0, false), 0)"); return find(link); }).catch(() => null);
  const hit2 = await findSoon("link_2.ifc"), hit4 = await findSoon("link_4.ifc");
  check("elements of link_2 and link_4 under some pixels (WebGL2's pick)", !!hit2 && !!hit4, JSON.stringify([hit2?.id, hit4?.id]));

  const STATES = [
    { name: "plain", on: null, off: null },
    { name: "a link hidden", on: `await (await fetch('http://127.0.0.1:${PORT}/action/manage_links?t=${TOKEN}', { method: 'POST', body: JSON.stringify({ op: 'hide', name: 'link_1.ifc' }) })).text()`,
      off: `await (await fetch('http://127.0.0.1:${PORT}/action/manage_links?t=${TOKEN}', { method: 'POST', body: JSON.stringify({ op: 'show', name: 'link_1.ifc' }) })).text()` },
    { name: "an element of a link hidden", on: `b.viewRule("hide", { element: ${JSON.stringify(hit2?.id ?? "")} })`, off: "b.runAction('show_all')" },
    { name: "a class isolated", on: `b.viewRule("isolate", { class: "IfcColumn" })`, off: "b.runAction('show_all')" },
    { name: "a link moved, another recoloured", on: `l.modelSet("place", "link_0.ifc", { move_mm: [0, 8000, 0] }); l.modelSet("colour", "link_2.ifc", { colour: 2 })`,
      off: `l.modelSet("place", "link_0.ifc", { move_mm: [0, 0, 0] }); l.modelSet("colour", "link_2.ifc", { colour: null })` },
    { name: "the budget at 0 (the far link released: its proxies)", on: "window.__bbGl.gpuBudget = 0", off: "delete window.__bbGl.gpuBudget", settle: true },
    { name: "a section box around a linked element", on: `b.selectLinked("link_4.ifc", ${JSON.stringify(hit4?.id ?? "")}); b.runAction('section_box')`, off: "b.runAction('section_box'); b.select([])" },
  ];
  // VIS1 (ADR 0078 amendment 7): WebGL2 finds its 3D edges on screen; the WebGPU renderer draws lines (no offscreen target
  // yet): WebGL2's pictures here are its line pass (the bench hook lineEdges), like for like
  await evalJs("(window.__bbGl.lineEdges = true, 0)");
  const lists0 = (await gl()).listsBuilt, whole0 = meshRequests.filter((u) => !/proxies|[?&]link=/.test(u)).length;
  for (const st of STATES) {
    if (st.on) { await B(st.on); await sleep(900); }
    if (st.settle) { // the budget's releases: frames drawn and 2 s, on both renderers
      for (let i = 0; i < 8; i++) { await evalJs("window.__bbGl.bench.frame(0, false)"); await sleep(400); }
      const det = (await gl()).linkDetail?.map((x) => [x.name, x.detail]);
      console.log(`  budget 0: ${JSON.stringify(det)}`);
    }
    const pics = {};
    for (const to of ["webgl2", "webgpu"]) {
      await use(to);
      if (st.settle) for (let i = 0; i < 4; i++) { await evalJs("window.__bbGl.bench.frame(0, false)"); await sleep(300); }
      pics[to] = [];
      for (const c of CAMS) pics[to].push(await snap(to === "webgpu", c));
    }
    if (st.name === "plain") { // (WebGPU in use: the near links' view, a whole frame)
      await evalJs(`(window.__bbGl.bench.look(${JSON.stringify(near)}, 140000), 0)`), await calm(), await evalJs("(window.__bbGl.bench.frame(0, false), 0)"), await sleep(600), await calm();
    }
    const s = await gl();
    CAMS.forEach((c, i) => {
      const cmp = compare(pics.webgl2[i], pics.webgpu[i]);
      check(`${st.name} @${c.join(",")}: WebGPU = WebGL2 within ${TOL_LEVELS} levels on ${100 * (1 - TOL_SHARE)} % of the pixels`, cmp.share <= TOL_SHARE && inked(pics.webgl2[i]) > 1000,
        `${cmp.any} pixels differ at all, ${cmp.over} by more than ${TOL_LEVELS} (${(100 * cmp.share).toFixed(3)} %), max ${cmp.max}${cmp.size ? `, ${cmp.size}` : ""}; ${inked(pics.webgl2[i])} drawn pixels`);
    });
    if (st.name === "plain") {
      const far = s.linkProxies?.find((x) => x.name === `link_${N - 1}.ifc`);
      check("plain, WebGPU: the far link drawn from its proxies", far && far.drawnTris > 0, JSON.stringify(far));
      // (VIS2: at least the open model and the N - 1 near links: the far link, drawn from its proxies, gives its detail back
      // when its proxies arrive after the open (ADR 0078 decision 30), which SHELL1's longer bake made the usual order;
      // the check wanted all N + 1 loaded and failed from SHELL1's merge on, the page right)
      check("WebGPU holds every loaded link (the open model and the near links at least), WebGL2's buffers for them emptied", s.gpuSplit && s.gpuSplit.taken === s.gpuSplit.models && s.gpuSplit.models >= N && s.gpuSplit.webgl < 65536,
        `${JSON.stringify(s.gpuSplit)} (WebGPU ${(s.gpuSplit?.webgpu / 1e6).toFixed(2)} MB, WebGL2 left ${(s.gpuSplit?.webgl / 1e6).toFixed(2)} MB); proxies arrived late for ${s.linkProxiesLate ?? 0} links`);
    }
    if (st.off) { await B(st.off); await sleep(900); }
  }
  const s1 = await gl();
  check("no draw lists rebuilt and no whole /mesh asked by the switches and states", s1.listsBuilt === lists0 && meshRequests.filter((u) => !/proxies|[?&]link=/.test(u)).length === whole0,
    `lists ${lists0} -> ${s1.listsBuilt}, whole /mesh ${whole0} -> ${meshRequests.filter((u) => !/proxies|[?&]link=/.test(u)).length}`);

  // ---- a pick on a link under WebGPU: its triangles read back from WebGPU's buffers ----
  await use("webgpu");
  await evalJs(`(window.__bbGl.bench.look(${JSON.stringify(near)}, 140000), window.__bbGl.bench.frame(0, false), 0)`);
  const p0 = await evalJs(`window.__bbGl.pickInfoAt(${hit4?.x ?? 0}, ${hit4?.y ?? 0})?.id ?? null`);
  const p1 = await waitFor("the pick's read back", 5000, () => evalJs(`window.__bbGl.pickInfoAt(${hit4?.x ?? 0}, ${hit4?.y ?? 0})?.id ?? null`)).catch(() => null);
  check("a pick on a link under WebGPU: the element WebGL2 picked there (read back from WebGPU's buffers)", p1 === hit4?.id,
    `first ray ${p0}, then ${p1}; picks read back ${(await gl()).gpuPickReads}`);

  // ---- the live switch back and forth 10 times: no GPU memory kept ----
  const sizes = [], times = [];
  for (let i = 0; i < 10; i++) {
    for (const to of ["webgl2", "webgpu"]) {
      const ms = await use(to);
      times.push([to, ms]);
      const s = await gl();
      sizes.push({ to, gl: s.gpuBytes, split: s.gpuSplit ? s.gpuSplit.webgpu + s.gpuSplit.webgl : null, gpuOnly: s.gpuSplit?.webgpu ?? null, glLeft: s.gpuSplit?.webgl ?? null });
    }
  }
  const g2 = sizes.filter((x) => x.to === "webgl2").map((x) => x.gl), g4 = sizes.filter((x) => x.to === "webgpu").map((x) => x.split);
  const within = (xs) => xs.every((x) => Math.abs(x - xs[0]) <= 0.01 * xs[0]);
  console.log(`switch times (ms): ${times.map(([t, ms]) => `${t} ${ms?.toFixed?.(0)}`).join(", ")}`);
  console.log(`bytes: WebGL2 ${g2.map((x) => (x / 1e6).toFixed(2)).join(" ")} MB; WebGPU (both renderers' held) ${g4.map((x) => (x / 1e6).toFixed(2)).join(" ")} MB`);
  check("10 switches each way: WebGL2's bytes back to the same value within 1 %", g2.length === 10 && within(g2));
  check("10 switches each way: the bytes held under WebGPU (WebGPU's + WebGL2's left) the same within 1 %", g4.length === 10 && g4.every((x) => x !== null) && within(g4));
  check("under WebGPU, WebGL2 holds no model's buffers (its state texture alone)", sizes.filter((x) => x.to === "webgpu").every((x) => x.glLeft !== null && x.glLeft < 0.05 * x.gpuOnly),
    `${sizes.filter((x) => x.to === "webgpu").map((x) => (x.glLeft / 1e6).toFixed(2)).join(" ")} MB`);
  check("no uncaught page error", exceptions.length === 0, exceptions.join(" | "));
  const gpuWarn = warnings.filter((w) => /webgpu/i.test(w));
  check("no WebGPU warning (validation errors, a fall back)", gpuWarn.length === 0, gpuWarn.slice(0, 3).join(" | "));
}

try { await main(); } catch (e) { check("the run", false, e?.stack ?? String(e)); } finally { await cleanup(); }
console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
