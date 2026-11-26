// Linked models in the lab's WebGL2 viewport (ticket B5, session/links.h): node ccore/test/lab_links.mjs
// Starts out/tcc/bimble-ccore (or $HOST_BIN) with CCORE_MODEL=golden/frame.json, Vite from web-ui/
// (needs web-ui/node_modules) and headless /usr/bin/chromium over raw CDP; opens
// /?native=1&gl=1&port=…&token=… and checks: before any link, no linked element drawn; the client
// links golden/ifc/fixtures/mapped_item.ifc (bimble.ts linkModel: action link_model {path}) and the
// viewport refetches /mesh (the LNKS trailer), draws the link (more pixels in Rust's reference
// tint, style.rs 190,214,238 shaded, and its edges in 52,110,180); a click on a linked element sends
// POST /select/- {link, ids}, the change feed's `linked` names it, the properties panel shows its
// name and GlobalId; editing its name is refused (an error shown, nothing changed: rev, the
// linked element's name); the panel says "Linked model, read-only" with the link's name above the
// fields, and the name field is disabled; hiding the link (manage_links hide) removes its pixels; no
// uncaught page error, no console error but the listed ones. Every pixel read waits for a frame drawn
// after the change it tests (`drawnAfter`, lab_section's `settle`). Prints pixel counts,
// "N passed, M failed"; exits non-zero on a failure. Kills all it starts.
import { spawn } from "node:child_process";
import { copyFileSync, existsSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { CHROMIUM, port, vitePort } from "./ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = resolve(CCORE, "../web-ui");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("lab_links");
const VITE = vitePort("lab_links");
const TOKEN = "lablinks-token";
const PAGE = `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}`;
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-links-"));
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

async function main() {
  if (!existsSync(join(UI, "node_modules"))) throw new Error(`no ${UI}/node_modules: run npm ci in web-ui/`);
  const LINK = join(tmp, "mapped_item.ifc");
  copyFileSync(join(CCORE, "golden/ifc/fixtures/mapped_item.ifc"), LINK);
  const host = start("host", BIN, [String(PORT)], {
    env: { ...process.env, BIMBLE_TOKEN: TOKEN, CCORE_MODEL: join(CCORE, "golden/frame.json"),
      BIMBLE_ORIGINS: `http://localhost:${VITE},http://127.0.0.1:${VITE}` },
  });
  await waitFor("host 'listening'", 5000, () => host.out.includes("listening") || (host.exitCode !== null && Promise.reject(new Error(host.out))));
  const hq = async (name, args = {}) => {
    const r = await fetch(`http://127.0.0.1:${PORT}/query/${name}?t=${TOKEN}`, { method: "POST", body: JSON.stringify(args) });
    return { status: r.status, text: await r.text() };
  };
  const hchange = async () => (await (await fetch(`http://127.0.0.1:${PORT}/change?since=0&t=${TOKEN}`)).json()).change;

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
  const consoleLog = [];
  const exceptions = [];
  const selects = [];
  const failedResponses = [];
  cdp.on((msg) => {
    if (msg.sessionId !== sessionId) return;
    if (msg.method === "Runtime.consoleAPICalled") {
      consoleLog.push({ level: msg.params.type, text: msg.params.args.map((a) => a.value ?? a.description ?? a.type).join(" ") });
    } else if (msg.method === "Runtime.exceptionThrown") {
      const d = msg.params.exceptionDetails;
      exceptions.push(d.exception?.description ?? d.text);
    } else if (msg.method === "Log.entryAdded") {
      consoleLog.push({ level: `log.${msg.params.entry.level}`, text: `${msg.params.entry.text} ${msg.params.entry.url ?? ""}`.trim() });
    } else if (msg.method === "Network.responseReceived" && msg.params.response.status >= 400) {
      failedResponses.push(`${msg.params.response.status} ${msg.params.response.url}`);
    } else if (msg.method === "Network.requestWillBeSent") {
      const r = msg.params.request;
      if (r.method === "POST" && new URL(r.url).pathname === "/select/-") selects.push({ requestId: msg.params.requestId, postData: r.postData });
    }
  });
  await S("Runtime.enable");
  await S("Log.enable");
  await S("Page.enable");
  await S("Network.enable");
  await S("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  await S("Page.navigate", { url: PAGE });
  const evalJs = async (expr) => (await S("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true })).result.value;
  await waitFor("the first drawn frame", 30000, () => evalJs("window.__bbGl && window.__bbGl.firstFrameMs"));
  await sleep(500);
  const frames = () => evalJs("window.__bbGl.frames");
  /** The canvas shows a change: `cond` holds (the change reached the page), a frame was counted since
   *  `f0` (the count taken before the change) and the viewport's loop turned after `cond` was seen
   *  (two animation frames: its draw of the change is in that turn or before). False on a timeout. */
  const drawnAfter = async (f0, what, cond) => {
    try {
      await waitFor(what, 10000, cond);
      await evalJs("new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)))");
      await waitFor(`a frame drawn after: ${what}`, 5000, async () => (await frames()) > f0);
      return true;
    } catch (e) {
      console.log(`not drawn: ${e.message}`);
      return false;
    }
  };

  // Pixels: the link's faces (reference blue, shaded: b > g > r, b - r >= 25, not the steel or
  // glass fills of the open model: those are counted before the link) and its edges (52,110,180).
  const pixels = () => evalJs(`(async () => {
    await new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)));
    const c = document.getElementById('bimble'), g = c.getContext('webgl2');
    const w = c.width, h = c.height, p = new Uint8Array(w * h * 4);
    g.readPixels(0, 0, w, h, g.RGBA, g.UNSIGNED_BYTE, p);
    let tint = 0, edge = 0;
    for (let i = 0; i < p.length; i += 4) {
      const r = p[i], gg = p[i + 1], b = p[i + 2];
      if (b > gg && gg > r && b - r >= 25 && r >= 140) tint++;
      if (Math.abs(r - 52) <= 30 && Math.abs(gg - 110) <= 30 && Math.abs(b - 180) <= 30) edge++;
    }
    return { tint, edge, w, h };
  })()`);
  const p0 = await pixels();
  const st0 = JSON.parse(await evalJs("JSON.stringify(window.__bbGl)"));
  console.log(`before the link: tint ${p0.tint} px, link edges ${p0.edge} px; mesh ${st0.meshBytes} bytes, links ${JSON.stringify(st0.links)}`);
  check("before: no link in the mesh", st0.links.length === 0 && st0.linkTris === 0, JSON.stringify(st0.links));

  // Link through the client (bimble.ts linkModel -> action link_model {path}).
  const fLink = await frames();
  const err = await evalJs(`(async () => (await import('/src/bimble.ts')).linkModel(${JSON.stringify(LINK)}))()`);
  check("linkModel(path) through the client -> no error", err === null, String(err));
  const st1 = await waitFor("the viewport's mesh with the link", 10000, async () => {
    const s = JSON.parse(await evalJs("JSON.stringify(window.__bbGl)"));
    return s.links.length === 1 && s.linkTris > 0 ? s : null;
  });
  await drawnAfter(fLink, "the link drawn", async () => true);
  console.log(`after the link: mesh ${st1.meshBytes} bytes, links ${JSON.stringify(st1.links)}, link triangles drawn ${st1.linkTris}`);
  check("the viewport refetched /mesh with the link", st1.meshBytes > st0.meshBytes && st1.links[0].name === "mapped_item.ifc" && st1.links[0].elements === 5,
    JSON.stringify(st1.links));
  const p1 = await pixels();
  console.log(`after the link: tint ${p1.tint} px, link edges ${p1.edge} px (canvas ${p1.w}x${p1.h})`);
  check("more pixels in the link tint", p1.tint > p0.tint + 200, `${p0.tint} -> ${p1.tint}`);
  check("the link's edges in the reference edge colour", p1.edge > p0.edge + 50, `${p0.edge} -> ${p1.edge}`);

  // A linked element under the pointer: a grid search through the pick hook, nearest the centre.
  const target = JSON.parse(await evalJs(`(() => {
    const c = document.getElementById('bimble'), r = c.getBoundingClientRect();
    let best = null;
    for (let y = r.top + 8; y < r.bottom - 8; y += 12) for (let x = r.left + 8; x < r.right - 8; x += 12) {
      const k = window.__bbGl.pickInfoAt(x, y);
      if (!k || k.link === null) continue;
      const ok = [[-4, 0], [4, 0], [0, -4], [0, 4]].every(([dx, dy]) => { const q = window.__bbGl.pickInfoAt(x + dx, y + dy); return q && q.id === k.id && q.link === k.link; });
      const d = Math.hypot(x - (r.left + r.right) / 2, y - (r.top + r.bottom) / 2);
      if (ok && (!best || d < best.d)) best = { x, y, d, ...k };
    }
    return JSON.stringify(best);
  })()`));
  check("a linked element under some pixel (pick)", target !== null && target.link === "mapped_item.ifc", JSON.stringify(target));
  if (target) {
    const before = selects.length, fClick = await frames();
    for (const type of ["mouseMoved", "mousePressed", "mouseReleased"])
      await S("Input.dispatchMouseEvent", { type, x: target.x, y: target.y, button: type === "mouseMoved" ? "none" : "left", buttons: type === "mousePressed" ? 1 : 0, clickCount: 1 });
    await waitFor("POST /select/-", 5000, () => selects.length > before).catch(() => {});
    const got = selects.slice(before);
    const body = got.length ? got[0].postData ?? (await S("Network.getRequestPostData", { requestId: got[0].requestId }).catch(() => ({}))).postData : undefined;
    check("click on the linked element -> POST /select/- {link, ids}", got.length === 1 && body === JSON.stringify({ link: "mapped_item.ifc", ids: [target.id] }), `${got.length} request(s), body ${body}`);
    const c = await hchange();
    check("the host: change linked = the element, selection its id", c.linked?.id === target.id && c.linked?.link === "mapped_item.ifc" &&
      JSON.stringify(c.selection) === JSON.stringify([target.id]), JSON.stringify({ linked: c.linked, selection: c.selection }));
    const el = JSON.parse((await hq("element", { id: target.id, link: "mapped_item.ifc" })).text);
    const shown = await waitFor("the properties panel with the linked element", 5000, () => evalJs(
      `document.body.innerText.includes(${JSON.stringify(target.id)}) && document.body.innerText.includes(${JSON.stringify(el.name)}) ? document.getElementById('el-name')?.value ?? '' : null`)).catch(() => null);
    console.log(`properties: name field ${JSON.stringify(shown)}, element ${JSON.stringify(el)}`);
    check("the properties panel shows the linked element (its GlobalId and name)", shown === el.name, String(shown));
    // The selection colour: read once the page has the selection and a frame is drawn after it.
    const selDrawn = await drawnAfter(fClick, "the linked element selected in the page", () => evalJs(`(async () => {
      const b = await import('/src/bimble.ts'); const l = b.state.value.linked;
      return l?.id === ${JSON.stringify(target.id)} && l.link === 'mapped_item.ifc'; })()`));
    const pink = await evalJs(`(() => { const c = document.getElementById('bimble'), g = c.getContext('webgl2');
      const p = new Uint8Array(c.width * c.height * 4); g.readPixels(0, 0, c.width, c.height, g.RGBA, g.UNSIGNED_BYTE, p);
      let n = 0; for (let i = 0; i < p.length; i += 4) if (p[i] - p[i + 1] > 20 && p[i + 2] > p[i + 1] && p[i] > 120) n++; return n; })()`);
    check("the selected linked element is drawn in the selection colour", selDrawn && pink > 50, `${pink} px${selDrawn ? "" : ", no frame drawn after the selection"}`);
    // Read-only: the line above the fields names the link; the name field is disabled.
    const ro = JSON.parse(await evalJs(`(async () => {
      const { t } = await import('/src/i18n/index.ts');
      const line = document.querySelector('[data-bb="props-readonly"]'), name = document.getElementById('el-name');
      return JSON.stringify({ line: line?.textContent.trim() ?? null, want: t('props.linkedReadOnly', { name: 'mapped_item' }),
        above: !!(line && name && (line.compareDocumentPosition(name) & Node.DOCUMENT_POSITION_FOLLOWING)), disabled: name?.disabled ?? null });
    })()`));
    console.log(`read-only line ${JSON.stringify(ro.line)}, name field disabled ${ro.disabled}`);
    check("the read-only line names the link, above the fields", ro.line === ro.want && ro.above, JSON.stringify(ro));
    check("the linked element's name field is disabled", ro.disabled === true, String(ro.disabled));
    // Cannot edit: force a new name into the (disabled) field, Enter: nothing changes.
    const rev0 = c.rev;
    const edit = await evalJs(`(async () => {
      const i = document.getElementById('el-name');
      if (!i) return 'no name field';
      i.value = 'EDITED-BY-LAB'; i.dispatchEvent(new Event('input'));
      i.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }));
      await new Promise((r) => setTimeout(r, 300));
      const a = [...document.querySelectorAll('[role=alert]')].map((e) => e.textContent).join(' | ');
      return a;
    })()`);
    const el2 = JSON.parse((await hq("element", { id: target.id, link: "mapped_item.ifc" })).text);
    const c2 = await hchange();
    console.log(`edit attempt: alert ${JSON.stringify(edit)}`);
    check("nothing changed: rev, the linked element's name, still selected", c2.rev === rev0 && el2.name === el.name && c2.linked?.id === target.id,
      `rev ${rev0} -> ${c2.rev}, name ${el2.name}`);
  }

  // Hide the link (Manage links): its pixels go.
  const fHide = await frames();
  const herr = await evalJs(`(async () => (await import('/src/bimble.ts')).manageLink('hide', 'mapped_item.ifc'))()`);
  const hidden = await drawnAfter(fHide, "the link hidden in the viewport", async () => {
    const s = JSON.parse(await evalJs("JSON.stringify(window.__bbGl)"));
    return s.links.length === 1 && s.links[0].visible === false && s.linkTris === 0;
  });
  const p2 = await pixels();
  console.log(`after hide: tint ${p2.tint} px, link edges ${p2.edge} px`);
  check("hide: no error, the link's pixels gone", hidden && herr === null && p2.tint <= p0.tint + 50 && p2.edge <= p0.edge + 10, `${herr} tint ${p2.tint}, edges ${p2.edge}`);
  const fShow = await frames();
  const serr = await evalJs(`(async () => (await import('/src/bimble.ts')).manageLink('show', 'mapped_item.ifc'))()`);
  const shownAgain = await drawnAfter(fShow, "the link shown again", async () => JSON.parse(await evalJs("JSON.stringify(window.__bbGl)")).linkTris > 0);
  const p3 = await pixels();
  check("show: the link drawn again", shownAgain && serr === null && p3.tint > p0.tint + 200, `${serr} tint ${p3.tint}`);

  const IGNORED = [
    /Synchronous XMLHttpRequest on the main thread is deprecated/,
    /404 \(Not Found\) http:\/\/localhost:\d+\/favicon\.ico$/,
  ];
  const bad = failedResponses.filter((r) => !/^404 http:\/\/localhost:\d+\/favicon\.ico$/.test(r));
  check("no failed response but favicon", bad.length === 0, bad.join(" | "));
  const errors = consoleLog.filter((m) => /error/.test(m.level) && !IGNORED.some((r) => r.test(m.text)));
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
