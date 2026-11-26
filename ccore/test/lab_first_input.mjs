// The first input of a session answers within 200 ms (ticket FIRST1, ADR 0074):
//   CCORE_PORT_BASE=auto node ccore/test/lab_first_input.mjs        (FIRST1_BUILDS=native,wasm)
// Vite from web-ui/ and, per case, a fresh headless Chromium (its own profile: the case's page is the
// session's first; SwiftShader, 1280 x 800, raw CDP; one at a time) and a fresh page: native = out/tcc/bimble-ccore (or $HOST_BIN) started anew on a copy of
// golden/frame.json (72 elements; HOME and BIMBLE_CONFIG_DIR in temporary folders), page
// /?native=1&gl=1&tab=quantities; wasm = the C core's module in the page, /?ccore=1&gl=1&tab=quantities.
// Each case waits for the page's data-bb-ready, then 0 or 400 ms, then gives the page ONE input, its
// first one, with real input events (CDP Input.dispatch*):
//   tool  a click on a ribbon tool the core does not serve (Quantities > Assign CFC): its own
//         feedback is the "not available yet" tooltip;
//   move  a pointer move over the middle of the viewport (the model is there): the status bar's
//         pointer coordinates;
//   key   Ctrl K: the palette is shown;
//   nav   a click on the navigator's first plan view (the first plan shown in the session): the plan
//         is the active view and the viewport drew a frame after the click.
// Response (ADR 0074): from the input event's timeStamp (pointerdown / pointermove / keydown) to the
// first animation frame whose callbacks see the feedback (a watcher runs in every frame from before
// the input); each case strictly under 200 ms. /proc/loadavg printed at the start and the end, and
// the frames' times around the input when a case fails.
// Before FIRST1 data-bb-ready was set before the page's first frame was on screen: headless
// Chromium's GPU process (SwiftShader) takes 200 to 900 ms per frame for the first frames of a
// session (it compiles the pipelines of each new kind of drawing on first use), the page's frames
// queue behind them, and the first input after ready waited up to 710 ms for its frame: 8 of the
// 16 cases failed (native key 202 ms, move no feedback in 5 s, the plan at 400 ms 201 ms; wasm at
// 0 ms 392 to 489 ms; load 1.7-2.6).
// Prints "N passed, M failed"; exits non-zero on a failure. Kills all it starts.
import { spawn, execFileSync } from "node:child_process";
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { CHROMIUM, port, vitePort } from "./ports.mjs";
import { hostEnv } from "./lib/host_env.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = resolve(CCORE, "../web-ui");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("lab_first_input");
const VITE = vitePort("lab_first_input");
const TOKEN = "labfirstinput-token";
const BUILDS = (process.env.FIRST1_BUILDS ?? "native,wasm").split(",");
const DELAYS = [0, 400];
const INPUTS = ["tool", "move", "key", "nav"];
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-firstinput-"));
const procs = [];
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail && !ok ? `: ${detail}` : ""}`);
};
const load = () => readFileSync("/proc/loadavg", "utf8").trim();
function start(name, cmd, args, opts) {
  const p = spawn(cmd, args, { stdio: ["ignore", "pipe", "pipe"], detached: true, ...opts });
  p.out = "";
  p.stdout.on("data", (d) => (p.out += d));
  p.stderr.on("data", (d) => (p.out += d));
  p.label = name;
  procs.push(p);
  return p;
}
async function stop(p) {
  for (const sig of ["SIGTERM", "SIGKILL"]) {
    if (p.exitCode !== null || p.signalCode !== null) return;
    try { process.kill(-p.pid, sig); } catch {}
    await sleep(200);
  }
}
async function cleanup() {
  for (const p of procs) await stop(p);
  rmSync(tmp, { recursive: true, force: true });
}
async function waitFor(what, ms, fn) {
  const until = Date.now() + ms;
  let last;
  while (Date.now() < until) {
    try { const v = await fn(); if (v) return v; } catch (e) { last = e; }
    await sleep(50);
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

/** The feedback of each input, a page expression (true once it is there; read in an animation frame). */
const FEEDBACK = {
  tool: "!!document.querySelector('[data-bb=not-yet-tip]')",
  move: "(document.querySelector('[data-bb=status-coords]')?.textContent.trim() ?? '') !== ''",
  key: "!!document.querySelector('[data-bb=palette]')",
  nav: "window.__f1B.openViews.value.active === window.__f1View && (window.__bbGl?.frames ?? 0) > window.__f1Frames",
};
/** The input event whose timeStamp starts the clock. */
const KIND = { tool: "pointerdown", move: "pointermove", key: "keydown", nav: "pointerdown" };
/** Where the input goes (a page expression: the element whose middle is clicked or moved over). */
const TARGET = {
  tool: "document.querySelector('[data-bb=ribbon] [data-bb=ribbon-tool-2-1]')",
  move: "document.getElementById('bimble')",
  key: "document.body",
  nav: "document.querySelector(`[data-bb=navigator] a[data-view='${window.__f1View}']`)",
};

/** A fresh headless Chromium (its own profile): the session's first page is the case's page. */
async function browser(label) {
  const chrome = start("chromium", CHROMIUM, [
    "--headless=new", "--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--remote-debugging-port=0", "--lang=en-US",
    `--user-data-dir=${join(tmp, `chromium-${label}`)}`, "--no-first-run", "--no-default-browser-check", "about:blank",
  ]);
  const wsUrl = await waitFor("DevTools line", 60000, () => chrome.out.match(/DevTools listening on (ws:\/\/\S+)/)?.[1]);
  return { chrome, cdp: await cdpConnect(wsUrl), profile: join(tmp, `chromium-${label}`) };
}

async function oneCase(build, delay, input) {
  const name = `${build} ${input} at ${delay} ms`;
  let host = null;
  if (build === "native") {
    const model = join(tmp, `frame-${build}-${input}-${delay}.bimble`);
    copyFileSync(join(CCORE, "golden/frame.json"), model);
    const home = join(tmp, `home-${input}-${delay}`);
    mkdirSync(home);
    host = start("host", BIN, [String(PORT), model], {
      env: hostEnv({ HOME: home, BIMBLE_TOKEN: TOKEN, BIMBLE_ORIGINS: `http://localhost:${VITE},http://127.0.0.1:${VITE}` }),
    });
    await waitFor("host 'listening'", 5000, () => host.out.includes("listening") || (host.exitCode !== null && Promise.reject(new Error(host.out))));
  }
  const page = build === "native"
    ? `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}&tab=quantities`
    : `http://localhost:${VITE}/?ccore=1&gl=1&tab=quantities`;
  const { chrome, cdp, profile } = await browser(`${build}-${input}-${delay}`);
  const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank" });
  const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
  const S = (m, p) => cdp.send(m, p, sessionId);
  const exceptions = [];
  cdp.on((msg) => { if (msg.sessionId === sessionId && msg.method === "Runtime.exceptionThrown") exceptions.push(msg.params.exceptionDetails.exception?.description ?? msg.params.exceptionDetails.text); });
  try {
    for (const d of ["Runtime", "Page"]) await S(`${d}.enable`);
    await S("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
    const evalJs = async (expr) => {
      const r = await S("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true });
      if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
      return r.result.value;
    };
    // Every input the page gets, its timeStamp: the first one is ours (no input before it).
    await S("Page.addScriptToEvaluateOnNewDocument", { source: `window.__f1In = [];
      for (const t of ['pointerdown', 'pointermove', 'keydown']) addEventListener(t, (e) => window.__f1In.push([t, e.timeStamp]), true);` });
    await S("Page.navigate", { url: page });
    // Ready, then (in the same page task) the case's setup, the watcher armed, then the delay:
    // measured in the page, no CDP round trip between ready and the input for 0 ms. The watcher runs
    // in every animation frame from before the input: the first frame whose callbacks see the
    // feedback ends the clock (ADR 0074; a check started after the input could miss that frame and
    // time a later one).
    const armed = await evalJs(`new Promise((done) => { const t0 = performance.now();
      const go = async () => {
        const at = performance.now();
        const b = window.__f1B = await import('/src/bimble.ts');
        const v = (b.openViews.value.views ?? []).find((x) => x.type === 'plan' && x.look !== 'up');
        window.__f1View = v?.id ?? null; window.__f1Frames = window.__bbGl?.frames ?? 0;
        const el = ${TARGET[input]};
        if (!el) return done({ error: 'no control (${input})', at });
        const r = el.getBoundingClientRect();
        window.__f1Hit = null; window.__f1Log = [];
        const loop = (ft) => {
          if (window.__f1Log.length < 400) window.__f1Log.push(ft);
          const e = window.__f1In.find((x) => x[0] === ${JSON.stringify(KIND[input])});
          if (e) {
            let ok = false; try { ok = ${FEEDBACK[input]}; } catch { ok = false; }
            if (ok) { window.__f1Hit = { ms: performance.now() - e[1], after: e[1] - at }; return; }
            if (performance.now() - e[1] > 5000) { window.__f1Hit = { ms: null, after: e[1] - at }; return; }
          }
          requestAnimationFrame(loop);
        };
        requestAnimationFrame(loop);
        const wait = () => performance.now() - at >= ${delay} ? done({ at, before: window.__f1In.length, xy: [r.x + r.width / 2, r.y + r.height / 2] }) : setTimeout(wait, 2);
        wait();
      };
      const c = () => document.documentElement?.dataset.bbReady === '1' ? go() : performance.now() - t0 > 60000 ? done({ error: 'no data-bb-ready in 60 s' }) : setTimeout(c, 2);
      c(); })`);
    if (armed.error) { check(`${name}: ${armed.error}`, false); return; }
    const [x, y] = armed.xy;
    if (input === "tool" || input === "nav") {
      for (const type of ["mousePressed", "mouseReleased"]) await S("Input.dispatchMouseEvent", { type, x, y, button: "left", buttons: type === "mousePressed" ? 1 : 0, clickCount: 1 });
    } else if (input === "move") {
      await S("Input.dispatchMouseEvent", { type: "mouseMoved", x, y, button: "none", buttons: 0 });
    } else {
      await S("Input.dispatchKeyEvent", { type: "rawKeyDown", key: "k", code: "KeyK", windowsVirtualKeyCode: 75, modifiers: 2 });
      await S("Input.dispatchKeyEvent", { type: "keyUp", key: "k", code: "KeyK", windowsVirtualKeyCode: 75, modifiers: 2 });
    }
    const r = await waitFor("the feedback", 8000, () => evalJs("window.__f1Hit")).catch(() => null);
    const ms = r?.ms;
    // The frames around the input (their times from the input event), printed when the check fails.
    const frames = await evalJs(`(() => { const e = window.__f1In.find((x) => x[0] === ${JSON.stringify(KIND[input])}); return e ? window.__f1Log.map((t) => Math.round(t - e[1])).filter((t) => t > -200) : []; })()`).catch(() => []);
    check(`${name}: its feedback within 200 ms (${typeof ms === "number" ? ms.toFixed(1) : "none"} ms; the input ${r ? r.after.toFixed(0) : "-"} ms after ready, ready ${armed.at.toFixed(0)} ms after navigation, no input before it: ${armed.before === 0})`,
      typeof ms === "number" && ms < 200 && armed.before === 0, `frames at ${frames.join(" ")} ms from the input`);
    if (exceptions.length) check(`${name}: no page exception`, false, exceptions.join(" | "));
  } finally {
    cdp.close();
    await stop(chrome);
    rmSync(profile, { recursive: true, force: true }); // 16 profiles would fill a small /tmp
    if (host) await stop(host);
  }
}

async function main() {
  if (!existsSync(join(UI, "node_modules"))) throw new Error(`no ${UI}/node_modules: run npm ci in web-ui/`);
  console.log(`loadavg ${load()}`);
  if (BUILDS.includes("wasm")) execFileSync("node", [join(UI, "scripts/copy-ccore.mjs")], { stdio: "ignore" });
  const vite = start("vite", "npx", ["vite", "--port", String(VITE), "--strictPort"], { cwd: UI });
  await waitFor(`Vite on ${VITE}`, 30000, async () => {
    if (vite.exitCode !== null) throw new Error(vite.out);
    return (await fetch(`http://localhost:${VITE}/`)).ok;
  });
  // The lab through Vite once, in a browser of its own, so no case times Vite's first transform of the modules.
  {
    const { chrome, cdp, profile } = await browser("warm");
    await cdp.send("Target.createTarget", { url: `http://localhost:${VITE}/?ccore=1&gl=1` });
    await sleep(6000);
    cdp.close();
    await stop(chrome);
    rmSync(profile, { recursive: true, force: true });
  }
  for (const build of BUILDS) for (const delay of DELAYS) for (const input of INPUTS) {
    try { await oneCase(build, delay, input); } catch (e) { check(`${build} ${input} at ${delay} ms`, false, e.message ?? String(e)); }
  }
  console.log(`loadavg ${load()}`);
}

let code = 0;
try {
  await main();
} catch (e) {
  console.log(`FAIL ${e.message ?? e}`);
  failed++;
} finally {
  await cleanup();
  console.log(`${passed} passed, ${failed} failed`);
  code = failed ? 1 : 0;
}
process.exit(code);
