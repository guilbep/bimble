// Plan views of an IFC opened read-only, in the lab's WebGL2 viewport (ticket S1,
// ccore/session/views.h): node ccore/test/lab_ifc_plan.mjs. Starts out/tcc/bimble-ccore (or
// $HOST_BIN) with no model, Vite from web-ui/ and headless /usr/bin/chromium over raw CDP; opens
// /?native=1&gl=1&port=…&token=…, then File -> Open (the action "open": read-only) of
//   golden/ifc/frame.ifc (three storeys): open_views offers the 3D view and Rust's 6 default plans
//     (golden/ifc/views/frame.txt); each plan opens as a tab with the plan camera; the pixels
//     drawn change with the storey (Fondations, Niveau 1, Niveau 2 all differ, and differ from
//     3D); in Plan Niveau 1 a click on the column P1.11 picks it (its GlobalId from
//     golden/ifc/read/frame/scene.txt) and selects it; a beam above the cut picks nothing;
//   golden/ifc/views/fixtures/storeys_grid.ifc (a storey without a GlobalId, keyed by its name):
//     Plan First draws the column C-first and picks it, Plan Ground does not draw it (a click
//     there picks nothing) but draws C-ground;
//   golden/ifc/fixtures/same_level.ifc (eight storeys at one level): 17 views; Plan Level A draws
//     the column C1 and a click picks it.
// No uncaught page error; no console error but the sync-XHR deprecation, the favicon 404 and the
// 400s of the model queries (storeys, materials, project, element) a read-only scene refuses.
// Prints "N passed, M failed"; exits non-zero on a failure. Kills all it starts.
import { spawn } from "node:child_process";
import { copyFileSync, existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { CHROMIUM, port, vitePort } from "./ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = resolve(CCORE, "../web-ui");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("lab_ifc_plan", 47446);
const VITE = vitePort("lab_ifc_plan", 5210);
const TOKEN = "labifcplan-token";
const PAGE = `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}`;
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-ifc-plan-"));
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
  const G = join(CCORE, "golden");
  const files = { frame: join(tmp, "frame.ifc"), grid: join(tmp, "storeys_grid.ifc"), same: join(tmp, "same_level.ifc") };
  copyFileSync(join(G, "ifc/frame.ifc"), files.frame);
  copyFileSync(join(G, "ifc/views/fixtures/storeys_grid.ifc"), files.grid);
  copyFileSync(join(G, "ifc/fixtures/same_level.ifc"), files.same);
  const host = start("host", BIN, [String(PORT)], {
    env: { ...process.env, BIMBLE_TOKEN: TOKEN, BIMBLE_ORIGINS: `http://localhost:${VITE},http://127.0.0.1:${VITE}` },
  });
  await waitFor("host 'listening'", 5000, () => host.out.includes("listening") || (host.exitCode !== null && Promise.reject(new Error(host.out))));
  const hostCall = async (op, name, body) => {
    const r = await fetch(`http://127.0.0.1:${PORT}/${op}/${encodeURIComponent(name)}?t=${TOKEN}`, { method: "POST", body, headers: { "Content-Type": "text/plain" } });
    return { status: r.status, text: await r.text() };
  };
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
  const consoleLog = [], exceptions = [], failed = [], requests = new Map();
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
    } else if (msg.method === "Network.responseReceived" && msg.params.response.status >= 400) {
      failed.push(`${msg.params.response.status} ${msg.params.response.url}`);
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
  await waitFor("the first drawn frame", 30000, () => evalJs("window.__bbGl && window.__bbGl.firstFrameMs !== undefined"));
  const B = (body) => evalJs(`(async () => { const b = await import('/src/bimble.ts'); ${body} })()`);
  const gl = async () => JSON.parse(await evalJs("JSON.stringify(window.__bbGl)"));
  const frames = () => evalJs("window.__bbGl.frames");
  const camera = async () => (await gl()).camera;
  const screen = async (x, y, z = 0) => {
    const xy = JSON.parse(await evalJs(`JSON.stringify(window.__bbGl.toScreen(${x}, ${y}, ${z}))`));
    if (!xy) throw new Error(`(${x}, ${y}) is behind the camera`);
    return xy;
  };
  const settle = async () => {
    const until = Date.now() + 5000;
    let f = await frames(), since = Date.now();
    while (Date.now() - since < 1200 && Date.now() < until) {
      await sleep(50);
      const g = await frames();
      if (g !== f) (f = g), (since = Date.now());
    }
  };
  /** Pixels that are not the background (247, 247, 245). */
  const modelPx = () => evalJs(`(() => { const c = document.getElementById('bimble'), g = c.getContext('webgl2');
    const p = new Uint8Array(c.width * c.height * 4); g.readPixels(0, 0, c.width, c.height, g.RGBA, g.UNSIGNED_BYTE, p);
    let n = 0; for (let i = 0; i < p.length; i += 4) if (Math.abs(p[i] - 247) > 3 || Math.abs(p[i + 1] - 247) > 3 || Math.abs(p[i + 2] - 245) > 3) n++; return n; })()`);
  const keepPx = (name) => evalJs(`(() => { const c = document.getElementById('bimble'), g = c.getContext('webgl2');
    const p = new Uint8Array(c.width * c.height * 4); g.readPixels(0, 0, c.width, c.height, g.RGBA, g.UNSIGNED_BYTE, p); (window.__keep ??= {})['${name}'] = p; return p.length; })()`);
  const diffPx = (name) => evalJs(`(() => { const c = document.getElementById('bimble'), g = c.getContext('webgl2'), q = window.__keep['${name}'];
    const p = new Uint8Array(c.width * c.height * 4); g.readPixels(0, 0, c.width, c.height, g.RGBA, g.UNSIGNED_BYTE, p);
    if (q.length !== p.length) return -1;
    let n = 0; for (let i = 0; i < p.length; i += 4) if (Math.abs(p[i] - q[i]) > 2 || Math.abs(p[i + 1] - q[i + 1]) > 2 || Math.abs(p[i + 2] - q[i + 2]) > 2) n++; return n; })()`);
  const pickAt = ([x, y]) => evalJs(`window.__bbGl.pickAt(${x}, ${y})`);
  const mouseClick = async ([x, y]) => {
    const p0 = (await gl()).picks;
    await S("Input.dispatchMouseEvent", { type: "mousePressed", x, y, button: "left", buttons: 1, clickCount: 1 });
    await S("Input.dispatchMouseEvent", { type: "mouseReleased", x, y, button: "left", buttons: 0, clickCount: 1 });
    await waitFor("the pick", 5000, async () => (await gl()).picks > p0);
    await sleep(150);
  };
  const openFile = async (f) => {
    const name = f.split("/").pop();
    const r = await hostCall("action", "open", JSON.stringify({ path: f, discard: true }));
    if (r.status !== 200) throw new Error(`open ${name}: ${r.status} ${r.text}`);
    await waitFor(`${name} on the page`, 15000, () => B(`return b.state.value.file === '${name}' && b.openViews.value.views.length > 0;`));
    await waitFor("the 3D camera", 5000, async () => (await camera()).mode === "3d");
    await settle();
    return B("return b.openViews.value;");
  };
  /** Opens the view named `name` as the navigator does (openView), waits for its plan camera. */
  const openPlan = async (ov, name) => {
    const v = ov.views.find((w) => w.name === name);
    if (!v) throw new Error(`no view ${name}`);
    const e = await B(`return b.openView('${v.id}');`);
    if (e) throw new Error(`openView ${name}: ${e}`);
    await waitFor(`${name} current`, 5000, async () => (await B("return b.openViews.value.current?.id;")) === v.id);
    await waitFor(`${name}: the plan camera`, 5000, async () => { const c = await camera(); return c.mode === "plan" && c.view === v.id; });
    await settle();
    return v;
  };

  // ---- frame.ifc, three storeys ----
  const ov = await openFile(files.frame);
  check("frame.ifc: opened read-only", (await B("return b.query('state').editable;")) === false);
  const want = readFileSync(join(G, "ifc/views/frame.txt"), "utf8").split("\n").find((l) => l.startsWith("views ")).slice(6);
  check("frame.ifc: the page's open_views = Rust's 7 views (golden/ifc/views/frame.txt)",
    JSON.stringify(ov.views) === JSON.stringify(JSON.parse(want)), JSON.stringify(ov.views.map((v) => v.name)));
  const px3d = await modelPx();
  const counts = {};
  for (const n of ["Plan Fondations", "Plan Niveau 1", "Plan Niveau 2"]) {
    await openPlan(ov, n);
    counts[n] = await modelPx();
    await keepPx(n);
    if (process.env.SHOTS) { const { data } = await S("Page.captureScreenshot", { format: "png" }); writeFileSync(join(process.env.SHOTS, `${n}.png`), Buffer.from(data, "base64")); }
  }
  check("frame.ifc: each plan draws something, not what 3D draws", Object.values(counts).every((c) => c > 1000 && c !== px3d), JSON.stringify({ ...counts, px3d }));
  // Niveau 2 is shown: it differs from the other two kept pictures.
  const d2f = await diffPx("Plan Fondations"), d21 = await diffPx("Plan Niveau 1");
  check("frame.ifc: the pixels drawn change with the storey (Niveau 2 vs Fondations, vs Niveau 1)", d2f > 1000 && d21 > 1000, `${d2f}, ${d21}`);
  const sceneTxt = readFileSync(join(G, "ifc/read/frame/scene.txt"), "utf8");
  const idOf = (nm) => new RegExp(`^\\d+ (\\S+) \\S+ .* name="${nm.replace(".", "\\.")}"`, "m").exec(sceneTxt)[1];
  const p111 = idOf("P1.11"), beam = idOf("B1.x00");
  await openPlan(ov, "Plan Niveau 1");
  check("frame.ifc: Plan Niveau 1 again draws the same pixels", (await diffPx("Plan Niveau 1")) < 50, String(await diffPx("Plan Niveau 1")));
  const d1f = await diffPx("Plan Fondations");
  check("frame.ifc: Plan Niveau 1 (columns cut over the footings) differs from Plan Fondations (footings cut)", d1f > 500, String(d1f));
  const c11 = await screen(6000, 5000, 600);
  check("frame.ifc Plan Niveau 1: a click on the column P1.11 picks it", (await pickAt(c11)) === p111, `${await pickAt(c11)}, want ${p111}`);
  check("frame.ifc Plan Niveau 1: a click on the beam B1.x00 (above the cut) picks nothing", (await pickAt(await screen(3000, 0, 2780))) === null, beam);
  await mouseClick(c11);
  check("frame.ifc Plan Niveau 1: the click selects P1.11", JSON.stringify(await B("return b.state.value.selection;")) === JSON.stringify([p111]),
    JSON.stringify(await B("return b.state.value.selection;")));
  await B("return b.select([]);");

  // ---- storeys_grid.ifc: a storey keyed by its name ----
  const ovg = await openFile(files.grid);
  await openPlan(ovg, "Plan First");
  const cFirst = await screen(4000, 0, 3600), cGround = await screen(0, 0, 600);
  check("storeys_grid Plan First (no GlobalId): the column C-first picked", (await pickAt(cFirst)) === "2T2cS1Storeys000000C10", String(await pickAt(cFirst)));
  check("storeys_grid Plan First: C-ground (below its range) picks nothing", (await pickAt(await screen(0, 0, 3600))) === null, String(await pickAt(await screen(0, 0, 3600))));
  const pxFirst = await modelPx();
  await openPlan(ovg, "Plan Ground");
  check("storeys_grid Plan Ground: C-ground picked, C-first not", (await pickAt(cGround)) === "2T2cS1Storeys000000C00" &&
    (await pickAt(await screen(4000, 0, 600))) === null, `${await pickAt(cGround)} ${await pickAt(await screen(4000, 0, 600))}`);
  const pxGround = await modelPx();
  check("storeys_grid: the 1200 mm column of First draws more pixels than the 600 mm one of Ground", pxFirst > 2 * pxGround && pxGround > 0, `${pxFirst} vs ${pxGround}`);

  // ---- same_level.ifc: eight storeys at one level ----
  const ovs = await openFile(files.same);
  check("same_level.ifc: 17 views (3D, 8 plans, 8 ceilings)", ovs.views.length === 17, String(ovs.views.length));
  await openPlan(ovs, "Plan Level A");
  check("same_level Plan Level A: the column C1 drawn", (await modelPx()) > 100, String(await modelPx()));
  const c1 = await screen(1000, 1000, 600);
  check("same_level Plan Level A: a click on C1 picks it", (await pickAt(c1)) === "2T2cSameLvl00000000010", String(await pickAt(c1)));

  // Ignored, explicitly: the sync-XHR deprecation, the favicon 404, and Chrome's line for a failed
  // sync XHR (logged with the calling script's URL, bimble.ts): the page asks the model queries
  // (storeys, materials, project, element) of a read-only scene, which answer 400 "this file is
  // read only" as calls.rs `model(v)?`. The check below makes sure every failed response is one.
  const IGNORED = [
    /Synchronous XMLHttpRequest on the main thread is deprecated/,
    /404 \(Not Found\) http:\/\/localhost:\d+\/favicon\.ico$/,
    new RegExp(`400 \\(Bad Request\\) http://localhost:${VITE}/src/bimble\\.ts$`),
  ];
  const errors = consoleLog.filter((m) => /error|warn/.test(m.level) && !IGNORED.some((r) => r.test(m.text)));
  check("no uncaught page error", exceptions.length === 0, exceptions.join(" | "));
  check("no console error or warning", errors.length === 0, errors.map((m) => m.text).join(" | "));
  const bad = failed.filter((r) => !/^404 http:\/\/localhost:\d+\/favicon\.ico$/.test(r) &&
    !new RegExp(`^400 http://127\\.0\\.0\\.1:${PORT}/query/(storeys|materials|project|element)\\?`).test(r));
  check("no failed response but the favicon and the model queries of a read-only scene", bad.length === 0, bad.join(" | "));
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
console.log(`lab_ifc_plan: ${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
