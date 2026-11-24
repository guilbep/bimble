// The real lab against the native headless twin (experiment 2, pass criterion: no component changed).
// node e2e/native-lab.mjs   (from web-ui/; needs target/debug/bimble, /usr/bin/chromium, node_modules)
// Starts the twin (BIMBLE_HEADLESS=1), Vite on 5173 and a headless Chromium over raw CDP, opens
// /?native=1&port=7882&token=labtest&select=first:IfcBeam, waits for data-bb-ready, then checks:
//   (a) no uncaught exception and no console error (sync-XHR deprecation and favicon 404 are listed, not failed);
//   (b) the twin's selection is non-empty (the change JSON from GET /change, over HTTP from Node);
//   (c) "IfcBeam" appears in the DOM (properties panel);
//   (d) GET /change?since=0 returns seq >= 1.
// Edit phase (the page also carries edit=move-first-beam: one real `move` verb, first IfcBeam, +1 mm in x):
//   (e1) that beam's placement origin x rose by 1 mm (+-1e-6), read from Node over HTTP;
//   (e2) query/history has undo.length >= 1;  (e3) GET /change says "unsaved": true;
// then undo() from the page (bimble.ts, over the sync XHR transport):
//   (u1) origin x is back;  (u2) history.undo is one shorter;  (u3) "unsaved" is false (reported if not).
// Screenshot: /tmp/native-lab.png. Exit 0 and PASS when all hold; 1 otherwise. Kills all it starts.
import { spawn, execFileSync } from "node:child_process";
import { existsSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const UI = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const ROOT = resolve(UI, "..");
const BIN = join(ROOT, "target/debug/bimble");
const PORT = 7882;
const TOKEN = "labtest";
const TWIN = `http://127.0.0.1:${PORT}`;
const PAGE = `http://localhost:5173/?native=1&port=${PORT}&token=${TOKEN}&select=first:IfcBeam&edit=move-first-beam`;
const SHOT = "/tmp/native-lab.png";
const tmp = mkdtempSync(join(tmpdir(), "bimble-native-lab-"));
const procs = [];
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

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
  for (const p of procs) {
    if (p.exitCode === null) {
      try {
        process.kill(-p.pid, "SIGTERM");
      } catch {}
    }
  }
  await sleep(300);
  for (const p of procs) {
    if (p.exitCode === null) {
      try {
        process.kill(-p.pid, "SIGKILL");
      } catch {}
    }
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
    await sleep(200);
  }
  throw new Error(`timeout waiting for ${what}${last ? `: ${last.message ?? last}` : ""}`);
}

async function cdpConnect(wsUrl) {
  const ws = new WebSocket(wsUrl);
  await new Promise((res, rej) => {
    ws.onopen = res;
    ws.onerror = () => rej(new Error("CDP connect failed"));
  });
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
  const send = (method, params = {}, sessionId) =>
    new Promise((resolve, reject) => {
      const m = ++id;
      pending.set(m, { resolve, reject });
      ws.send(JSON.stringify({ id: m, method, params, ...(sessionId ? { sessionId } : {}) }));
    });
  return { send, on: (fn) => listeners.push(fn), close: () => ws.close() };
}

async function main() {
  const failures = [];
  // 1. The model file.
  if (!existsSync(BIN)) execFileSync("cargo", ["build", "-q", "-p", "bimble-cli"], { cwd: ROOT, stdio: "inherit" });
  const file = join(tmp, "frame.bimble");
  execFileSync(BIN, ["demo", file], { stdio: "inherit" });

  // 2. The twin.
  const env = { ...process.env, BIMBLE_SERVE: String(PORT), BIMBLE_TOKEN: TOKEN, BIMBLE_HEADLESS: "1" };
  delete env.WAYLAND_DISPLAY;
  delete env.DISPLAY;
  const twin = start("twin", BIN, ["view", file], { env });
  await waitFor("twin 'listening'", 20000, () => twin.out.includes("listening") || (twin.exitCode !== null && Promise.reject(new Error(twin.out))));

  // Before the edit: the first IfcBeam and its origin x, from Node over HTTP.
  const q = async (name, args) => {
    const r = await fetch(`${TWIN}/query/${name}?t=${TOKEN}`, { method: "POST", body: JSON.stringify(args ?? {}) });
    const text = await r.text();
    if (!r.ok) throw new Error(`query ${name}: ${r.status} ${text}`);
    return JSON.parse(text);
  };
  const lastChange = async () => (await (await fetch(`${TWIN}/change?since=0&t=${TOKEN}`)).json()).change;
  const beams = await q("elements", { class: "IfcBeam" });
  const beamId = beams[0]?.id;
  if (!beamId) throw new Error(`no IfcBeam: ${JSON.stringify(beams).slice(0, 200)}`);
  const originX = async () => (await q("element", { id: beamId })).placement.origin[0];
  const x0 = await originX();
  const undo0 = (await q("history")).undo.length;
  console.log(`before edit: beam ${beamId} origin x ${x0}, history.undo ${undo0}, unsaved ${(await lastChange()).unsaved}`);

  // 3. Vite.
  const vite = start("vite", "npx", ["vite", "--port", "5173", "--strictPort"], { cwd: UI });
  await waitFor("Vite on 5173", 30000, async () => {
    if (vite.exitCode !== null) throw new Error(vite.out);
    return (await fetch("http://localhost:5173/")).ok;
  });

  // 4. Chromium over raw CDP.
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
  cdp.on((msg) => {
    if (msg.sessionId !== sessionId) return;
    if (msg.method === "Runtime.consoleAPICalled") {
      const text = msg.params.args.map((a) => a.value ?? a.description ?? a.type).join(" ");
      consoleLog.push({ level: msg.params.type, text });
    } else if (msg.method === "Runtime.exceptionThrown") {
      const d = msg.params.exceptionDetails;
      exceptions.push(d.exception?.description ?? d.text);
    } else if (msg.method === "Log.entryAdded") {
      consoleLog.push({ level: `log.${msg.params.entry.level}`, text: `${msg.params.entry.text} ${msg.params.entry.url ?? ""}`.trim() });
    }
  });
  await S("Runtime.enable");
  await S("Log.enable");
  await S("Page.enable");
  await S("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  const t0 = Date.now();
  await S("Page.navigate", { url: PAGE });
  const evalJs = async (expr) => (await S("Runtime.evaluate", { expression: expr, returnByValue: true })).result.value;

  // 5. data-bb-ready (deeplink.ts sets it on <html>).
  let ready = false;
  try {
    await waitFor("data-bb-ready", 30000, () => evalJs("document.documentElement.dataset.bbReady === '1'"));
    ready = true;
    console.log(`data-bb-ready after ${Date.now() - t0} ms`);
  } catch (e) {
    failures.push(e.message);
  }
  await sleep(500); // let the last change land in the DOM

  // 7. Screenshot (before the asserts, so a failure still gives a picture).
  const shot = await S("Page.captureScreenshot", { format: "png" });
  writeFileSync(SHOT, Buffer.from(shot.data, "base64"));

  // 6. Asserts.
  // Ignored, explicitly: the sync-XHR deprecation warning, and the dev server's favicon 404
  // (index.html has no icon; Chromium asks for /favicon.ico; not the lab's code).
  const IGNORED = [/Synchronous XMLHttpRequest on the main thread is deprecated/, /404 \(Not Found\) http:\/\/localhost:5173\/favicon\.ico$/];
  const ignored = consoleLog.filter((m) => IGNORED.some((r) => r.test(m.text)));
  const errors = consoleLog.filter((m) => /error/.test(m.level) && !IGNORED.some((r) => r.test(m.text)));
  console.log(`console messages (${consoleLog.length}):`);
  for (const m of consoleLog) console.log(`  [${m.level}] ${m.text}`);
  console.log(`ignored (sync-XHR deprecation, favicon 404): ${ignored.length}`);
  if (exceptions.length) failures.push(`(a) uncaught exceptions:\n    ${exceptions.join("\n    ")}`);
  if (errors.length) failures.push(`(a) console errors:\n    ${errors.map((m) => m.text).join("\n    ")}`);

  // The selection is in the twin's change JSON (calls.rs change_json), not in query("state").
  const ch = await (await fetch(`${TWIN}/change?since=0&t=${TOKEN}`)).json();
  const sel = ch.change?.selection ?? null;
  console.log(`(b) twin change.selection: ${JSON.stringify(sel)}`);
  if (!Array.isArray(sel) || sel.length === 0) failures.push(`(b) selection empty; /change: ${JSON.stringify(ch).slice(0, 400)}`);

  const hasBeam = ready && (await evalJs("document.body.innerText.includes('IfcBeam')"));
  console.log(`(c) "IfcBeam" in the page text: ${hasBeam}`);
  if (!hasBeam) failures.push(`(c) "IfcBeam" not in the DOM text`);

  console.log(`(d) /change?since=0 seq: ${ch.seq}`);
  if (!(ch.seq >= 1)) failures.push(`(d) seq ${ch.seq} < 1`);

  // Edit phase: the deep link ran `move` before data-bb-ready.
  const x1 = await originX();
  const h1 = (await q("history")).undo;
  const unsaved1 = (await lastChange()).unsaved;
  const e1 = Math.abs(x1 - x0 - 1) <= 1e-6;
  console.log(`(e1) origin x ${x0} -> ${x1} (delta ${x1 - x0}): ${e1 ? "ok" : "FAIL"}`);
  if (!e1) failures.push(`(e1) origin x ${x0} -> ${x1}, expected +1 mm`);
  console.log(`(e2) history.undo length ${h1.length} ${JSON.stringify(h1)}: ${h1.length >= 1 ? "ok" : "FAIL"}`);
  if (!(h1.length >= 1)) failures.push(`(e2) history.undo empty`);
  console.log(`(e3) /change unsaved: ${unsaved1}: ${unsaved1 === true ? "ok" : "FAIL"}`);
  if (unsaved1 !== true) failures.push(`(e3) unsaved ${unsaved1}, expected true`);

  // Undo from the page.
  const undoErr = (await S("Runtime.evaluate", {
    expression: "(async () => (await import('/src/bimble.ts')).undo())()",
    awaitPromise: true,
    returnByValue: true,
  })).result.value;
  console.log(`page undo() returned: ${JSON.stringify(undoErr)}`);
  if (undoErr) failures.push(`(u) page undo() error: ${undoErr}`);
  await sleep(300);
  const x2 = await originX();
  const h2 = (await q("history")).undo;
  const unsaved2 = (await lastChange()).unsaved;
  const u1 = x2 === x0;
  console.log(`(u1) origin x after undo ${x2} (before edit ${x0}): ${u1 ? "ok" : "FAIL"}`);
  if (!u1) failures.push(`(u1) origin x ${x2}, expected ${x0}`);
  const u2 = h2.length === h1.length - 1;
  console.log(`(u2) history.undo length ${h1.length} -> ${h2.length}: ${u2 ? "ok" : "FAIL"}`);
  if (!u2) failures.push(`(u2) history.undo ${h1.length} -> ${h2.length}, expected one shorter`);
  console.log(`(u3) /change unsaved after undo: ${unsaved2}${unsaved2 === false ? ": ok" : " (finding: the twin still reports unsaved)"}`);
  const errorsAfter = consoleLog.filter((m) => /error/.test(m.level) && !IGNORED.some((r) => r.test(m.text)));
  if (errorsAfter.length > errors.length) failures.push(`(a) console errors after the edit phase:\n    ${errorsAfter.slice(errors.length).map((m) => m.text).join("\n    ")}`);
  if (exceptions.length) failures.push(`(a) uncaught exceptions (edit phase):\n    ${exceptions.join("\n    ")}`);

  console.log(`screenshot: ${SHOT}`);
  cdp.close();
  if (failures.length) {
    console.log(`FAIL:\n  ${failures.join("\n  ")}`);
    console.log(`--- twin output ---\n${twin.out}\n--- vite output ---\n${vite.out}`);
    return 1;
  }
  console.log("PASS");
  return 0;
}

let code = 1;
try {
  code = await main();
} catch (e) {
  console.log(`FAIL: ${e.stack ?? e}`);
  for (const p of procs) console.log(`--- ${p.label} output ---\n${p.out}`);
} finally {
  await cleanup();
}
process.exit(code);
