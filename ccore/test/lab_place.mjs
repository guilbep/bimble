// The C host answers the lab from a real model (ticket 9b): node ccore/test/lab_place.mjs
// 1. Starts out/tcc/bimble-ccore (or $HOST_BIN) on 47333 with a copy of golden/frame.json named
//    frame.bimble (out/t9b-lab_place/), and runs web-ui/e2e/native-place.mjs against it, unchanged.
// 2. Starts a second host on 47334 (another copy), Vite from web-ui/ on 5195 (needs
//    web-ui/node_modules: `npm ci` there) and headless /usr/bin/chromium over raw CDP; opens
//    /?native=1&port=47334&token=… and checks: data-bb-ready is set (modelReady resolved), the
//    navigator lists the 3 storeys, selecting an element through the client shows its properties
//    (the element query's answer); ticket E1: renaming it in the properties panel goes through the
//    verb `rename` and a project field through `set_project_field` (the lab POSTs no /apply),
//    two undos restore both; no uncaught page error.
// Prints "N passed, M failed"; exits non-zero on a failure. Kills all it starts.
import { spawn } from "node:child_process";
import { copyFileSync, existsSync, mkdirSync, rmSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { RUN_TAG, port, vitePort } from "./ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = resolve(CCORE, "../web-ui");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("lab_place", 47333);
const LAB_PORT = port("lab_place_lab", 47334);
const VITE = vitePort("lab_place", 5195);
const TOKEN = "labplace-token";
const TMP = join(CCORE, `out/t9b-lab_place${RUN_TAG}`);
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
  p.done = new Promise((r) => p.on("exit", (code) => r(code)));
  procs.push(p);
  return p;
}

async function cleanup() {
  for (const sig of ["SIGTERM", "SIGKILL"]) {
    for (const p of procs) if (p.exitCode === null && p.signalCode === null) try { process.kill(-p.pid, sig); } catch {}
    await sleep(300);
  }
  rmSync(TMP, { recursive: true, force: true });
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

function startHost(port, dir) {
  mkdirSync(dir, { recursive: true });
  const model = join(dir, "frame.bimble"); // native-place.mjs waits for state.file "frame.bimble"
  copyFileSync(join(CCORE, "golden/frame.json"), model);
  const h = start(`host ${port}`, BIN, [String(port), model], {
    env: { ...process.env, BIMBLE_TOKEN: TOKEN, BIMBLE_ORIGINS: `http://localhost:${VITE},http://127.0.0.1:${VITE}` },
  });
  return waitFor(`host ${port} 'listening'`, 5000, () => h.out.includes("listening") || (h.exitCode !== null && Promise.reject(new Error(h.out))));
}

async function nativePlace() {
  await startHost(PORT, join(TMP, "a"));
  const e2e = start("native-place", "node", [join(UI, "e2e/native-place.mjs"), "--endpoint", `http://127.0.0.1:${PORT}`, "--token", TOKEN]);
  const code = await e2e.done;
  console.log(e2e.out.trim().split("\n").map((l) => `  | ${l}`).join("\n"));
  check("web-ui/e2e/native-place.mjs passes against the C host", code === 0 && /^PASS$/m.test(e2e.out), `exit ${code}`);
}

async function lab() {
  if (!existsSync(join(UI, "node_modules"))) throw new Error(`no ${UI}/node_modules: run npm ci in web-ui/`);
  await startHost(LAB_PORT, join(TMP, "b"));
  const vite = start("vite", "npx", ["vite", "--port", String(VITE), "--strictPort"], { cwd: UI });
  await waitFor(`Vite on ${VITE}`, 30000, async () => {
    if (vite.exitCode !== null) throw new Error(vite.out);
    return (await fetch(`http://localhost:${VITE}/`)).ok;
  });
  const chrome = start("chromium", "/usr/bin/chromium", [
    "--headless=new", "--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--remote-debugging-port=0",
    `--user-data-dir=${join(TMP, "chromium")}`, "--no-first-run", "--no-default-browser-check", "about:blank",
  ]);
  const wsUrl = await waitFor("DevTools line", 15000, () => chrome.out.match(/DevTools listening on (ws:\/\/\S+)/)?.[1]);
  const cdp = await cdpConnect(wsUrl);
  const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank" });
  const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
  const S = (m, p) => cdp.send(m, p, sessionId);
  const exceptions = [];
  cdp.on((msg) => {
    if (msg.sessionId === sessionId && msg.method === "Runtime.exceptionThrown") {
      const d = msg.params.exceptionDetails;
      exceptions.push(d.exception?.description ?? d.text);
    }
  });
  await S("Runtime.enable");
  await S("Page.enable");
  await S("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  const t0 = Date.now();
  await S("Page.navigate", { url: `http://localhost:${VITE}/?native=1&port=${LAB_PORT}&token=${TOKEN}` });
  const evalJs = async (expr) => (await S("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true })).result.value;
  const bb = (expr) => evalJs(`(async () => { const b = await import('/src/bimble.ts'); return ${expr}; })()`);

  // modelReady (bimble.ts:648: file set and actions loaded), then deeplink.ts:82 sets data-bb-ready.
  let ready = false;
  try {
    await waitFor("data-bb-ready", 20000, () => evalJs("document.documentElement.dataset.bbReady === '1'"));
    ready = true;
    console.log(`data-bb-ready after ${Date.now() - t0} ms`);
  } catch (e) {
    console.log(e.message);
  }
  check("data-bb-ready is set (modelReady resolved)", ready);
  check("page state: frame.bimble, editable", await bb("b.state.value.file === 'frame.bimble'"), await bb("JSON.stringify(b.state.value)"));

  // The navigator's object tree: open "Objects", then one row per storey.
  const names = await bb("JSON.stringify(b.storeys.value.map((s) => s.name))");
  check("page holds 3 storeys", JSON.parse(names).length === 3, names);
  await evalJs(`(() => {
    const nav = document.querySelector('[data-bb=navigator]');
    const a = [...nav.querySelectorAll('a[aria-expanded]')].find((x) => x.querySelector('.n')?.textContent.trim() === '72');
    a?.click();
    return !!a;
  })()`);
  const rows = await waitFor("storey rows", 5000, async () => {
    // The rows after the "Objects" row (the 3D view row above it has the same indent).
    const r = JSON.parse(await evalJs(`(() => {
      const all = [...document.querySelectorAll('[data-bb=navigator] a.ti')];
      const at = all.findIndex((x) => x.querySelector('.n')?.textContent.trim() === '72');
      return JSON.stringify(all.slice(at + 1).filter((a) => a.style.paddingLeft === '22px').map((a) => a.querySelector('span')?.textContent.trim()));
    })()`));
    return r.length ? r : null;
  });
  check("navigator lists the 3 storeys", JSON.stringify(rows) === names, JSON.stringify(rows));

  // Select an element through the client: the properties panel shows the element query's answer.
  const id = await bb("b.query('elements', { class: 'IfcBeam', limit: 1 })[0].id");
  const detail = JSON.parse(await bb(`JSON.stringify(b.element(${JSON.stringify(id)}))`));
  const err = await bb(`b.select([${JSON.stringify(id)}])`);
  check("select through the client", err === null, String(err));
  const props = await waitFor("properties of the selection", 5000, async () => {
    const t = await evalJs("document.querySelector('[data-bb=properties]')?.innerText ?? ''");
    return t.includes(id) && t.includes(detail.name) ? t : null;
  }).catch((e) => e.message);
  check("properties show the selected element (GlobalId and name)", props.includes(id) && props.includes(detail.name), props.replace(/\s+/g, " ").slice(0, 200));
  check("page selection = [id]", await bb(`JSON.stringify(b.state.value.selection) === ${JSON.stringify(JSON.stringify([id]))}`));

  // Ticket E1 (ADR 0068 rule 1): the properties panel's name field renames through the verb
  // `rename`, the project page's fields through `set_project_field`; no raw `apply` from the lab.
  const posts = [];
  cdp.on((msg) => {
    if (msg.sessionId === sessionId && msg.method === "Network.requestWillBeSent" && msg.params.request.method === "POST") {
      const u = new URL(msg.params.request.url);
      if (u.port === String(LAB_PORT)) posts.push(u.pathname);
    }
  });
  await S("Network.enable");
  const newName = "B-renamed by the lab";
  const typed = await evalJs(`(() => {
    const i = document.querySelector('[data-bb=properties] #el-name');
    if (!i) return false;
    i.value = ${JSON.stringify(newName)};
    i.dispatchEvent(new Event('input', { bubbles: true }));
    i.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }));
    return true;
  })()`);
  check("properties panel: the name field takes the new name", typed === true);
  const renamed = await waitFor("the element's new name", 5000, async () => (await bb(`b.element(${JSON.stringify(id)}).name`)) === newName).catch(() => false);
  check("rename through the properties panel -> the element's name", renamed, await bb(`b.element(${JSON.stringify(id)}).name`));
  const renameLabel = await bb("b.query('history', {}).undo[0]");
  check("rename is one undo step", typeof renameLabel === "string" && renameLabel.startsWith("rename "), String(renameLabel));
  const pfErr = await bb("b.setProject({ ...(b.project.value?.info ?? {}), project_number: 'P-42' })");
  const pnum = await waitFor("project_number", 5000, async () => (await bb("b.project.value?.info?.project_number ?? null")) === "P-42").catch(() => false);
  check("project field through the client (setProject) -> set", pfErr === null && pnum, String(pfErr));
  await bb("b.undo()");
  await bb("b.undo()");
  const back = await waitFor("undo of the rename", 5000, async () => (await bb(`b.element(${JSON.stringify(id)}).name`)) === detail.name).catch(() => false);
  check("two undos: name and project as before", back && (await bb("b.project.value?.info?.project_number ?? null")) === null, await bb(`b.element(${JSON.stringify(id)}).name`));
  console.log(`  lab POSTs during the edits: ${[...new Set(posts)].join(", ")}`);
  check("the lab sent verb/rename and verb/set_project_field", posts.includes("/verb/rename") && posts.includes("/verb/set_project_field"), [...new Set(posts)].join(", "));
  check("the lab sent no raw apply", !posts.some((p) => p.startsWith("/apply")), [...new Set(posts)].join(", "));
  check("no uncaught page error", exceptions.length === 0, exceptions.join(" | "));
  cdp.close();
}

try {
  rmSync(TMP, { recursive: true, force: true });
  await nativePlace();
  await lab();
} catch (e) {
  check("no exception in the test", false, e.stack ?? String(e));
  for (const p of procs) console.log(`--- ${p.label} output ---\n${p.out.slice(-2000)}`);
} finally {
  await cleanup();
}
console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
