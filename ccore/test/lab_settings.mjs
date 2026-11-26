// Settings and About in the lab (ticket SET1): node ccore/test/lab_settings.mjs
// Starts out/tcc/bimble-ccore (or $HOST_BIN) on a copy of golden/frame.json with its settings folder in a
// temp folder (BIMBLE_CONFIG_DIR, and HOME / XDG_CONFIG_HOME there too: never the user's own), copies
// the wasm build (scripts/copy-ccore.mjs), Vite from web-ui/ and headless /usr/bin/chromium over raw CDP.
// Twice, on the native host (/?native=1&gl=1: the settings file) and on ccore.wasm (/?ccore=1&gl=1: the
// browser's localStorage `bimble.settings`, settings.rs:246):
//   File › Settings is enabled and opens the dialog (DEAD1: a font choice not installed, Ubuntu Light,
//     is disabled with where its files go); a name typed there is the header disc's trigram
//     and Home's greeting, and is in the file (native: settings.json in Rust's form; wasm: the stored
//     text); the theme "dark" turns the app's ground to Rust's dark panel colour (#1E1E1E); the text
//     size 14 pt makes the root font 18.67 px; after a reload the name, the disc, the theme and the
//     size are still there; the dark theme's text passes 4.5:1 on its ground on Home and in the
//     dialog, at 1280 px and at 360 px wide; the readout's system "wgs84" is the `coordinates`
//     query's system (a georeferenced model reads latitude and longitude, not LV95) and back.
//   File › About shows the version and the core's build; "Copy for a bug report" (a real click) puts
//     the text on the clipboard, or shows it selected when the clipboard is refused.
// On the code before SET1 the File menu's Settings and About are disabled (`settings`, `about` never
// enabled): the first check of each mode fails, and state.user stays null.
// No uncaught page error; no console error or warning but the sync-XHR deprecation and the favicon 404.
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
const PORT = port("lab_settings");
const VITE = vitePort("lab_settings");
const TOKEN = "labsettings-token";
const PAGES = {
  native: `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}`,
  wasm: `http://localhost:${VITE}/?ccore=1&gl=1`,
};
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-settings-"));
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

// In the page: the text elements under `sel` whose colour is under 4.5:1 on the first opaque ground
// behind them (WCAG AA, normal text), as "text: ratio".
const CONTRAST_JS = (sel) => `(() => {
  const rgb = (c) => (c.match(/[\\d.]+/g) || []).map(Number);
  const lin = (v) => { v /= 255; return v <= 0.04045 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4; };
  const lum = ([r, g, b]) => 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b);
  const ground = (el) => { for (let e = el; e; e = e.parentElement) { const c = rgb(getComputedStyle(e).backgroundColor); if (c.length === 3 || c[3] > 0.9) return c; } return rgb(getComputedStyle(document.body).backgroundColor); };
  const out = []; let n = 0;
  for (const root of document.querySelectorAll(${JSON.stringify(sel)})) for (const el of root.querySelectorAll('*')) {
    if (![...el.childNodes].some((c) => c.nodeType === 3 && c.textContent.trim())) continue;
    const cs = getComputedStyle(el), r = el.getBoundingClientRect();
    if (cs.visibility === 'hidden' || cs.display === 'none' || !r.width || Number(cs.opacity) < 0.9 || el.closest('[disabled],[aria-disabled]')) continue;
    n++;
    const [a, b] = [lum(rgb(cs.color)), lum(ground(el))].sort((x, y) => y - x), k = (a + 0.05) / (b + 0.05);
    if (k < 4.5) out.push(el.textContent.trim().slice(0, 30) + ': ' + k.toFixed(2));
  }
  return JSON.stringify({ n, out });
})()`;

async function lab(mode, S, evalJs) {
  const B = (body) => evalJs(`(async () => { const b = await import('/src/bimble.ts'); ${body} })()`);
  const raf2 = () => evalJs("new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)))");
  const exists = (sel) => evalJs(`!!document.querySelector(${JSON.stringify(sel)})`);
  const text = (sel) => evalJs(`document.querySelector(${JSON.stringify(sel)})?.textContent.trim() ?? null`);
  const click = async (sel) => { await evalJs(`document.querySelector(${JSON.stringify(sel)}).click()`); await raf2(); };
  /** A real mouse click on the element's centre (a user gesture: the clipboard needs one). */
  const realClick = async (sel) => {
    const r = JSON.parse(await evalJs(`JSON.stringify(document.querySelector(${JSON.stringify(sel)}).getBoundingClientRect())`));
    const x = r.x + r.width / 2, y = r.y + r.height / 2;
    await S("Input.dispatchMouseEvent", { type: "mousePressed", x, y, button: "left", clickCount: 1 });
    await S("Input.dispatchMouseEvent", { type: "mouseReleased", x, y, button: "left", clickCount: 1 });
    await raf2();
  };
  const field = async (sel, value) => {
    await evalJs(`(() => { const i = document.querySelector(${JSON.stringify(sel)}); i.value = ${JSON.stringify(value)}; i.dispatchEvent(new Event('input')); i.dispatchEvent(new Event('change')); return true; })()`);
    await raf2();
  };
  const appBg = () => evalJs("getComputedStyle(document.querySelector('.bb-app')).backgroundColor");
  const rootFont = () => evalJs("getComputedStyle(document.documentElement).fontSize");
  const load = async () => {
    await S("Page.navigate", { url: PAGES[mode] });
    await waitFor(`${mode}: the model loaded`, 30000, () => B("await b.ready; return b.state.value.file !== null && b.actions.value.length > 0;"));
    await raf2();
  };
  const stored = async () => (mode === "native"
    ? (existsSync(join(CONF, "settings.json")) ? readFileSync(join(CONF, "settings.json"), "utf8") : "")
    : (await evalJs("localStorage.getItem('bimble.settings') ?? ''")));

  await load();
  const bg0 = await appBg(), f0 = await rootFont();
  // File › Settings
  await click("[data-bb=quickbar-file]");
  const off = await evalJs("document.querySelector('[data-bb=file-menu-settings]')?.disabled ?? null");
  check(`${mode}: File › Settings enabled`, off === false, String(off));
  await click("[data-bb=file-menu-settings]");
  check(`${mode}: the Settings dialog opens`, await exists("[data-bb=settings] [data-bb=settings-name]"), await text("[data-bb=settings]") ?? "none");
  // DEAD1: a font choice this computer lacks (Ubuntu Light: no Ubuntu face installed, measured as the page does) is
  // disabled with where its files go; the lab's own font ("roboto": the interface's own, shipped) stays on
  const fontUi = await evalJs(`(() => { const u = document.querySelector('[data-bb=settings-font-ubuntu]'), r = document.querySelector('[data-bb=settings-font-roboto]');
    const c = document.createElement('canvas').getContext('2d'), t = 'mmmmmmmmmmlli1WQ@#ąé'; let has = false;
    for (const f of ['Ubuntu', 'Ubuntu Light']) for (const b of ['monospace', 'serif']) { c.font = '72px ' + b; const w = c.measureText(t).width; c.font = "72px '" + f + "', " + b; if (c.measureText(t).width !== w) has = true; }
    return u && r ? { has, uOff: u.disabled, title: u.closest('label')?.title ?? '', note: document.querySelector('[data-bb=settings-font-missing-ubuntu]')?.textContent ?? null, rOff: r.disabled } : null; })()`);
  check(`${mode}: Settings: Ubuntu Light ${fontUi?.has ? "installed: on" : "not installed: off, with where its files go"}; the interface's own font on (DEAD1)`,
    !!fontUi && fontUi.rOff === false && (fontUi.has ? fontUi.uOff === false : fontUi.uOff === true && /fonts/.test(fontUi.title) && !!fontUi.note), JSON.stringify(fontUi));
  // The name
  await field("[data-bb=settings-name]", "Ada Lovelace");
  await field("[data-bb=settings-email]", "ada@example.org");
  const user = await B("return b.state.value.user;");
  check(`${mode}: state.user from the name (calls.rs:253)`, user?.name === "Ada Lovelace" && user?.trigram === "AL" && user?.email === "ada@example.org", JSON.stringify(user));
  check(`${mode}: the header disc shows AL`, (await text("[data-bb=quickbar-user]")) === "AL", await text("[data-bb=quickbar-user]"));
  const file1 = await stored();
  check(`${mode}: stored in Rust's form (${mode === "native" ? "settings.json" : "localStorage bimble.settings"})`,
    file1.startsWith('{"coordinates":"survey","gpu_cull":false,"memory_share":0.8,') && file1.includes('"user_email":"ada@example.org","user_name":"Ada Lovelace"}'), file1);
  // The theme
  await evalJs("(() => { const s = document.querySelector('[data-bb=settings-theme]'); s.value = 'dark'; s.dispatchEvent(new Event('change')); return true; })()");
  await raf2();
  const bg1 = await appBg();
  check(`${mode}: theme dark: the ground is Rust's dark panel (rgb(30, 30, 30)), was ${bg0}`, bg0 === "rgb(245, 242, 235)" && bg1 === "rgb(30, 30, 30)", bg1);
  check(`${mode}: data-theme="dark" on the root`, (await evalJs("document.documentElement.dataset.theme")) === "dark");
  // The text size
  await field("[data-bb=settings-text-size]", "14");
  const f1 = await rootFont();
  check(`${mode}: text size 14 pt: the root font 18.67 px (was ${f0})`, f0 === "16px" && f1 === "18.67px", f1);
  // Contrast of the dark theme, the dialog open (1280 px)
  const c1 = JSON.parse(await evalJs(CONTRAST_JS("[data-bb=settings]")));
  check(`${mode}: dark: the dialog's ${c1.n} texts pass 4.5:1 at 1280 px`, c1.n > 5 && c1.out.length === 0, c1.out.join(" | "));
  // The readout's system
  await realClick("[data-bb=settings-coord-wgs84]");
  check(`${mode}: the readout's system is wgs84`, (await B("return b.readoutSystem();")) === "wgs84");
  await B("return b.georefSet({ crs: 'EPSG:2056', east_m: 2600000, north_m: 1200000 });");
  const tw = await B("return b.coordinates([1000, 2000, 0])?.text ?? null;");
  const ts = await B("return b.coordinates([1000, 2000, 0], 'survey')?.text ?? null;");
  check(`${mode}: coordinates() asks in the setting's system: "${tw}" (survey: "${ts}")`, !!tw && !!ts && tw !== ts && /LV95/.test(ts) && !/LV95/.test(tw), `${tw} / ${ts}`);
  await B("return b.undo();");
  await realClick("[data-bb=settings-coord-survey]");
  check(`${mode}: back to survey`, (await B("return b.readoutSystem();")) === "survey");
  // 3D2 (ADR 0078): HLOD proxies (moving_proxies) turned off and on from the dialog; GPU culling said not available
  const storedHas = async (t) => { for (let i = 0; i < 100; i++) { const v = await stored(); if (v.includes(t)) return v; await raf2(); } return await stored(); };
  await realClick("[data-bb=settings-moving-proxies]");
  const pOff = await storedHas('"moving_proxies":false');
  await realClick("[data-bb=settings-moving-proxies]");
  const pOn = await storedHas('"moving_proxies":true');
  check(`${mode}: Far parts simplified: off then on (moving_proxies stored false, then true)`, pOff.includes('"moving_proxies":false') && pOn.includes('"moving_proxies":true'), `${pOff} / ${pOn}`);
  check(`${mode}: GPU culling of copies: not available yet, with the reason`, /WebGL2/.test((await text("[data-bb=settings-gpu-cull]")) ?? ""), (await text("[data-bb=settings-gpu-cull]")) ?? "none");
  if (mode === "wasm") { // GPU1 (ADR 0083): the 3D renderer, stored; GPU culling offered with WebGPU (the native file stays Rust's eleven fields below)
    const pickRenderer = (r) => evalJs(`(() => { const s = document.querySelector('[data-bb=settings-renderer]'); s.value = ${JSON.stringify(r)}; s.dispatchEvent(new Event('change')); return s.value; })()`);
    check(`${mode}: 3D renderer: automatic by default`, (await evalJs("document.querySelector('[data-bb=settings-renderer]')?.value ?? null")) === "auto");
    await pickRenderer("webgpu");
    const rOn = await storedHas('"renderer":"webgpu"');
    await raf2();
    const cullOn = await evalJs("!!document.querySelector('[data-bb=settings-gpu-cull-on]') && !document.querySelector('[data-bb=settings-gpu-cull]')");
    await pickRenderer("auto");
    const rOff = await storedHas('"renderer":"auto"');
    check(`${mode}: 3D renderer: WebGPU stored, GPU culling of copies then offered, back to automatic`, rOn.includes('"renderer":"webgpu"') && cullOn && rOff.includes('"renderer":"auto"'), `${cullOn} ${rOff}`);
  }
  await click("[data-bb=settings-close]");

  // A reload: everything still there
  await load();
  check(`${mode}: after a reload: the disc AL`, (await text("[data-bb=quickbar-user]")) === "AL", await text("[data-bb=quickbar-user]"));
  check(`${mode}: after a reload: dark and 14 pt`, (await appBg()) === "rgb(30, 30, 30)" && (await rootFont()) === "18.67px", `${await appBg()} ${await rootFont()}`);
  await B("const s = await import('/src/shell/send.ts'); return s.send({ do: 'home' });");
  await raf2();
  check(`${mode}: after a reload: Home greets Ada Lovelace`, ((await text("[data-bb=home-title]")) ?? "").includes("Ada Lovelace"), await text("[data-bb=home-title]"));
  check(`${mode}: Home's user card: the name and e-mail`, ((await text("[data-bb=home-you]")) ?? "").includes("Ada Lovelace") && (await text("[data-bb=home-email]")) === "ada@example.org", await text("[data-bb=home-you]"));
  const ch = JSON.parse(await evalJs(CONTRAST_JS("[data-bb=home]")));
  check(`${mode}: dark: Home's ${ch.n} texts pass 4.5:1 at 1280 px`, ch.n > 5 && ch.out.length === 0, ch.out.join(" | "));
  // Home's "change" opens Settings
  await click("[data-bb=home-settings]");
  check(`${mode}: Home's "change" opens Settings`, await exists("[data-bb=settings]"));
  // 360 px: the dialog and the small frame
  await S("Emulation.setDeviceMetricsOverride", { width: 360, height: 740, deviceScaleFactor: 1, mobile: true });
  await sleep(300);
  const c3 = JSON.parse(await evalJs(CONTRAST_JS("[data-bb=settings], .bb-app")));
  check(`${mode}: dark: ${c3.n} texts pass 4.5:1 at 360 px`, c3.n > 5 && c3.out.length === 0, c3.out.join(" | "));
  const wide = await evalJs("document.querySelector('[data-bb=settings]').getBoundingClientRect().width");
  check(`${mode}: at 360 px the dialog fits (${wide} px)`, wide <= 360 - 16, String(wide));
  await S("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  await sleep(300);
  // Back to the defaults (the next mode starts clean on its own storage anyway)
  await evalJs("(() => { const s = document.querySelector('[data-bb=settings-theme]'); s.value = 'grey'; s.dispatchEvent(new Event('change')); return true; })()");
  await field("[data-bb=settings-text-size]", "12");
  check(`${mode}: grey and 12 pt: the design's look again`, (await appBg()) === "rgb(245, 242, 235)" && (await rootFont()) === "16px", `${await appBg()} ${await rootFont()}`);
  await click("[data-bb=settings-close]");

  // About
  await click("[data-bb=quickbar-file]");
  check(`${mode}: File › About enabled`, (await evalJs("document.querySelector('[data-bb=file-menu-about]')?.disabled ?? null")) === false);
  await click("[data-bb=file-menu-about]");
  const ver = await text("[data-bb=about-version]"), sv = await B("return b.state.value.version;");
  check(`${mode}: About shows the version (${ver}) and the core's build (${await text("[data-bb=about-build]")})`, !!ver && ver === sv && /\d/.test((await text("[data-bb=about-build]")) ?? ""), `${ver} vs ${sv}`);
  check(`${mode}: About names the graphics (${await text("[data-bb=about-graphics]")})`, ((await text("[data-bb=about-graphics]")) ?? "").length > 0);
  await realClick("[data-bb=about-copy]");
  await sleep(200);
  const copied = await exists("[data-bb=about-copied]");
  const clip = copied ? await evalJs("navigator.clipboard.readText().catch((e) => 'refused: ' + e)") : await evalJs("document.querySelector('[data-bb=about-text]')?.value ?? ''");
  const sel = copied ? true : await evalJs("(() => { const a = document.querySelector('[data-bb=about-text]'); return !!a && document.activeElement === a && a.selectionEnd - a.selectionStart === a.value.length; })()");
  console.log(`${mode}: copy for a bug report: ${copied ? "the clipboard" : "the selected text"}: ${JSON.stringify(clip)}`);
  check(`${mode}: "Copy for a bug report": "Bimble <version>\\nCore: …\\nGraphics: …\\nSystem: …"`, sel && clip.startsWith(`Bimble ${sv}\nCore: `) && clip.includes("\nGraphics: ") && clip.includes("\nSystem: "), clip);
  await click("[data-bb=about-close]");
}

async function main() {
  if (!existsSync(join(UI, "node_modules"))) throw new Error(`no ${UI}/node_modules: run npm ci in web-ui/`);
  execFileSync("node", [join(UI, "scripts/copy-ccore.mjs")], { stdio: "ignore" });
  mkdirSync(CONF, { recursive: true });
  const model = join(tmp, "frame.bimble");
  copyFileSync(join(CCORE, "golden/frame.json"), model);
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
  for (const mode of ["native", "wasm"]) await lab(mode, S, evalJs);
  // The native file: Rust's form, read back by the C core's own reader at the next start.
  const f = readFileSync(join(CONF, "settings.json"), "utf8");
  check("native: settings.json in Rust's form (sorted keys, compact, the eleven fields)", /^\{"coordinates":"survey","gpu_cull":false,"memory_share":0\.8,"moving_fps":30,"moving_proxies":true,"recent":\[[^\]]*\],"text_size":12\.0,"theme":"grey","ui_font":"roboto","user_email":"ada@example\.org","user_name":"Ada Lovelace"\}$/.test(f), f);

  const IGNORED = [
    /Synchronous XMLHttpRequest on the main thread is deprecated/,
    /404 \(Not Found\) http:\/\/localhost:\d+\/favicon\.ico$/,
    /^Canvas2D: Multiple readback operations using getImageData/,
  ];
  const errors = consoleLog.filter((m) => /error|warn/.test(m.level) && !IGNORED.some((r) => r.test(m.text)));
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
