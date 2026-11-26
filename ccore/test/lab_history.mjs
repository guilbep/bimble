// The History page writes the history (ticket H2a): node ccore/test/lab_history.mjs
// Starts out/tcc/bimble-ccore (or $HOST_BIN) on a copy of golden/frame.json with no history folder, its
// settings in a temp folder (BIMBLE_CONFIG_DIR, HOME and XDG_CONFIG_HOME there too: never the user's own),
// copies the wasm build (scripts/copy-ccore.mjs), Vite from web-ui/ and headless /usr/bin/chromium over
// raw CDP, on the native host (/?native=1&gl=1):
//   History page, no name in the settings: Create version is disabled and the card links to Settings
//     (the link opens the Settings dialog); a name and e-mail set: the author shown is "name <e-mail>";
//   the first version (a message, Create version): it is first in the list with that author, the
//     folder frame.bimble-history exists next to the file, the message field is empty again;
//   an edit: "1 elements" pending; Create version: the version first, pending back to 0;
//   New variant "idea": the branch select shows idea; an edit there: the select is disabled (Rust's
//     Switch: commit first); Create version on idea; the select back to main: the model is main's
//     (the project field typed on idea is gone, pending 0, the version list is main's);
//   nothing pending: Create version disabled (Rust's Commit needs changes); a bad branch name: Rust's
//     text inline under the form (the one 400 the page logs).
// On the code before H2a the Create version button is disabled for ever (data-missing) and the
// first check of the flow fails.
// No uncaught page error; no console error or warning but the sync-XHR deprecation, the favicon 404 and
// the one 400 of the refused branch name.
// Prints "N passed, M failed"; exits non-zero on a failure. Kills all it starts.
import { spawn, execFileSync } from "node:child_process";
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { CHROMIUM, port, vitePort } from "./ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = resolve(CCORE, "../web-ui");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("lab_history", 47304);
const VITE = vitePort("lab_history", 5133);
const TOKEN = "labhistory-token";
const PAGE = `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}`;
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-history-"));
const CONF = join(tmp, "conf");
const procs = [];
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail && !ok ? `: ${detail}` : ""}`);
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


async function lab(S, evalJs) {
  const B = (body) => evalJs(`(async () => { const b = await import('/src/bimble.ts'); ${body} })()`);
  const raf2 = () => evalJs("new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)))");
  const exists = (sel) => evalJs(`!!document.querySelector(${JSON.stringify(sel)})`);
  const text = (sel) => evalJs(`document.querySelector(${JSON.stringify(sel)})?.textContent.trim() ?? null`);
  const disabled = (sel) => evalJs(`document.querySelector(${JSON.stringify(sel)})?.disabled ?? null`);
  const click = async (sel) => { await evalJs(`document.querySelector(${JSON.stringify(sel)}).click()`); await raf2(); };
  const field = async (sel, value) => {
    await evalJs(`(() => { const i = document.querySelector(${JSON.stringify(sel)}); i.value = ${JSON.stringify(value)}; i.dispatchEvent(new Event('input')); i.dispatchEvent(new Event('change')); return true; })()`);
    await raf2();
  };
  const pick = async (sel, value) => {
    await evalJs(`(() => { const s = document.querySelector(${JSON.stringify(sel)}); s.value = ${JSON.stringify(value)}; s.dispatchEvent(new Event('change')); return true; })()`);
    await raf2();
  };
  const versions = () => evalJs("JSON.stringify([...document.querySelectorAll('[data-bb=history-versions] .ver')].map((v) => [v.querySelector('.msg').textContent.trim(), v.querySelector('.who').textContent.trim()]))").then(JSON.parse);
  const pending = () => text("[data-bb=history-pending]");
  const project = () => B("return JSON.stringify(b.query('project'));");
  const edit = async (value) => {
    const err = await B(`return b.runVerb('set_project_field', { label: 'Phase', value: ${JSON.stringify(value)} });`);
    await waitFor(`the edit ${value} in pending`, 5000, async () => (await pending()) !== "0 elements");
    return err;
  };
  await S("Page.navigate", { url: PAGE });
  await waitFor("the model loaded", 30000, () => B("await b.ready; return b.state.value.file !== null && b.actions.value.length > 0;"));
  await B("const s = await import('/src/shell/send.ts'); return s.send({ do: 'showPage', page: 'history' });");
  await waitFor("the History page", 5000, () => exists("[data-bb=history] [data-bb=history-commit]"));
  check("history_write capability on the native host", (await B("return b.capabilities.value?.history_write;")) === true);
  // No name: Settings first.
  await field("[data-bb=history-message]", "first version");
  check("no name: Create version disabled", (await disabled("[data-bb=history-commit]")) === true);
  check("no name: the card links to Settings", await exists("[data-bb=history-author-settings]"), await text("[data-bb=history-author]"));
  await click("[data-bb=history-author-settings]");
  check("the link opens the Settings dialog", await exists("[data-bb=settings] [data-bb=settings-name]"));
  await field("[data-bb=settings-name]", "Ada Lovelace");
  await field("[data-bb=settings-email]", "ada@example.org");
  await click("[data-bb=settings-close]");
  await waitFor("the author shown", 3000, async () => ((await text("[data-bb=history-author]")) ?? "").includes("Ada Lovelace <ada@example.org>"));
  check("the author from the settings", ((await text("[data-bb=history-author]")) ?? "").includes("Ada Lovelace <ada@example.org>"));
  // The first version: starts the history.
  check("the first version: Create version enabled", (await disabled("[data-bb=history-commit]")) === false);
  await click("[data-bb=history-commit]");
  await waitFor("the first version listed", 5000, async () => (await versions()).length === 1);
  const v1 = await versions();
  check("the first version first, with the author", v1[0]?.[0] === "first version" && v1[0]?.[1].startsWith("Ada Lovelace <ada@example.org> · "), JSON.stringify(v1));
  check("the history folder next to the file", existsSync(join(tmp, "frame.bimble-history/format")));
  check("the message field empty again", (await evalJs("document.querySelector('[data-bb=history-message]').value")) === "");
  check("nothing pending after the version", (await pending()) === "0 elements", await pending());
  // An edit, a version.
  await edit("51");
  check("an edit: 1 element pending", (await pending()) === "1 elements", await pending());
  await field("[data-bb=history-message]", "phase 51");
  await click("[data-bb=history-commit]");
  await waitFor("the second version listed", 5000, async () => (await versions()).length === 2);
  const v2 = await versions();
  check("the new version first", v2[0]?.[0] === "phase 51" && v2[1]?.[0] === "first version", JSON.stringify(v2));
  check("pending back to 0", (await pending()) === "0 elements", await pending());
  check("the model saved with the version", (await B("return b.state.value.unsaved;")) === false);
  // Nothing to commit: Rust's text inline.
  await field("[data-bb=history-message]", "again");
  const stuck = await disabled("[data-bb=history-commit]");
  check("nothing pending: Create version disabled (Rust's Commit needs changes)", stuck === true, String(stuck));
  // New variant.
  await click("[data-bb=history-new-variant]");
  await field("[data-bb=history-branch-name]", "bad name");
  await click("[data-bb=history-branch-create]");
  check("a bad branch name: Rust's text inline", ((await text("[data-bb=history-branch-error]")) ?? "").startsWith('invalid branch name "bad name"'), await text("[data-bb=history-branch-error]"));
  await field("[data-bb=history-branch-name]", "idea");
  await click("[data-bb=history-branch-create]");
  await waitFor("on idea", 5000, async () => (await evalJs("document.querySelector('[data-bb=history-switch]').value")) === "idea");
  check("New variant: on idea", (await evalJs("document.querySelector('[data-bb=history-switch]').value")) === "idea");
  check("the select lists idea and main", (await evalJs("[...document.querySelectorAll('[data-bb=history-switch] option')].map((o) => o.value).join('|')")) === "idea|main");
  await edit("idea");
  check("changes not in a version: Switch disabled", (await disabled("[data-bb=history-switch]")) === true);
  await field("[data-bb=history-message]", "on idea");
  await click("[data-bb=history-commit]");
  await waitFor("the version on idea", 5000, async () => (await versions())[0]?.[0] === "on idea");
  check("Switch enabled again", (await disabled("[data-bb=history-switch]")) === false);
  check("the project field is idea's", (await project()).includes('"idea"'), await project());
  // Switch back to main.
  await pick("[data-bb=history-switch]", "main");
  await waitFor("back on main", 5000, async () => (await versions())[0]?.[0] === "phase 51");
  const v3 = await versions();
  check("switch: main's versions", v3.map((v) => v[0]).join("|") === "phase 51|first version", JSON.stringify(v3));
  check("switch: the model is main's (the field typed on idea is gone)", (await project()).includes('"51"') && !(await project()).includes('"idea"'), await project());
  check("switch: nothing pending, saved", (await pending()) === "0 elements" && (await B("return b.state.value.unsaved;")) === false, await pending());
  check("switch: no error shown", !(await exists("[data-bb=history-commit-error]")) && !(await exists("[data-bb=history-branch-error]")));
}

async function main() {
  if (!existsSync(join(UI, "node_modules"))) throw new Error(`no ${UI}/node_modules: run npm ci in web-ui/`);
  execFileSync("node", [join(UI, "scripts/copy-ccore.mjs")], { stdio: "ignore" });
  mkdirSync(CONF, { recursive: true });
  const model = join(tmp, "frame.bimble");
  copyFileSync(join(CCORE, "golden/frame.json"), model); // no history folder: the first version starts one
  const h = start("host", BIN, [String(PORT), model], {
    env: { ...process.env, BIMBLE_TOKEN: TOKEN, BIMBLE_ORIGINS: `http://localhost:${VITE},http://127.0.0.1:${VITE}`,
      BIMBLE_CONFIG_DIR: CONF, HOME: tmp, XDG_CONFIG_HOME: join(tmp, "xdg") },
  });
  await waitFor("host 'listening'", 5000, () => h.out.includes("listening") || (h.exitCode !== null && Promise.reject(new Error(h.out))));
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
  await cdp.send("Browser.grantPermissions", { origin: `http://localhost:${VITE}`, permissions: ["clipboardReadWrite", "clipboardSanitizedWrite"] }).catch(() => {});
  const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank" });
  const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
  const S = (m, p) => cdp.send(m, p, sessionId);
  const consoleLog = [], exceptions = [];
  cdp.on((msg) => {
    if (msg.sessionId !== sessionId) return;
    if (msg.method === "Runtime.consoleAPICalled") {
      consoleLog.push({ level: msg.params.type, text: msg.params.args.map((a) => a.value ?? a.description ?? a.type).join(" ") });
    } else if (msg.method === "Runtime.exceptionThrown") {
      const d = msg.params.exceptionDetails;
      exceptions.push(d.exception?.description ?? d.text);
    } else if (msg.method === "Log.entryAdded") {
      consoleLog.push({ level: `log.${msg.params.entry.level}`, text: `${msg.params.entry.text} ${msg.params.entry.url ?? ""}`.trim() });
    }
  });
  for (const d of ["Runtime", "Log", "Page"]) await S(`${d}.enable`);
  await S("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  const evalJs = async (expr) => {
    const r = await S("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true, userGesture: true });
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
    return r.result.value;
  };
  await lab(S, evalJs);

  const IGNORED = [
    /Synchronous XMLHttpRequest on the main thread is deprecated/,
    /404 \(Not Found\) http:\/\/localhost:\d+\/favicon\.ico$/,
    /^Canvas2D: Multiple readback operations using getImageData/,
  ];
  const refused = consoleLog.filter((m) => /status of 400 \(Bad Request\)/.test(m.text));
  check("one 400 logged: the refused branch name", refused.length === 1, refused.map((m) => m.text).join(" | "));
  const errors = consoleLog.filter((m) => /error|warn/.test(m.level) && !IGNORED.some((r) => r.test(m.text)) && !refused.includes(m));
  check("no uncaught page error", exceptions.length === 0, exceptions.join(" | "));
  check("no console error or warning", errors.length === 0, errors.map((m) => m.text).join(" | "));
  cdp.close();
}

try {
  await main();
} catch (e) {
  check("no exception in the test", false, e.stack ?? String(e));
  for (const p of procs) console.log(`--- ${p.label} output ---\n${p.out.slice(-2000)}`);
} finally {
  await cleanup();
  console.log(`${passed} passed, ${failed} failed`);
  process.exit(failed ? 1 : 0);
}
