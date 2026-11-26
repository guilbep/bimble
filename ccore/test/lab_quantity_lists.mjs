// The Quantities tab in the lab, every control of it (ticket QTY1, ADR 0080):
//   CCORE_PORT_BASE=auto node ccore/test/lab_quantity_lists.mjs
// Starts out/tcc/bimble-ccore (or $HOST_BIN) on a copy of golden/frame.json (72 elements), with HOME
// and BIMBLE_CONFIG_DIR in temporary folders, Vite from web-ui/ and one headless Chromium over raw
// CDP at 1280 x 800, opens /?native=1&gl=1&tab=quantities and clicks each tool (real mouse events):
//   New quantities: the dialog; "Poteaux", columns ticked -> Create: one step, the Quantities page on
//     it, listed in the navigator's Quantities.
//   Columns: the dialog on Poteaux; level, class, material off, count, mass, gross volume on, count
//     two places earlier (past global_id, then mass) -> Apply: name, volume, count, mass, gross_volume; totals by hand:
//     24 columns (12 a level x 2 levels), each 300 x 300 x 2780 mm = 0.2502 m3, 24 x 0.2502 =
//     6.0048 ("6.005", net = gross: no opening), mass 24 x 0.2502 x 2500 = 15012 kg ("15012").
//   Export CSV: the page's file (anchor + Blob) has the table's header line, every row of the table
//     and its total line.
//   Undo: the default columns back; redo: the chosen ones.
//   By material: one click makes the "By material" list once and shows it: C30/37 line 72 elements,
//     443.42 m2 (12 footings 1.44 + 2 x (12 x 0.09 + 9 x 1.5 + 8 x 1.25 + 188.49)), 126.264 m3,
//     315661 kg (126.2644 x 2500); a second click adds no step.
//   Save, then open the file again: Poteaux keeps its five columns, By material is there.
//   Assign CFC, CFC catalogue, Filter and group... : Bar schedule and the two CFC tools say "not
//     available yet" (their tooltip, capability cfc / rebar false).
// Each click's own feedback (the dialog, the page, the table changed, the tooltip) is timed from the
// pointerdown's timeStamp to the next animation frame after it: under 200 ms each (ADR 0074),
// /proc/loadavg printed. On the code before QTY1 the New quantities, Columns and By material tools are
// drawn "not yet" (action null): the first checks fail. Prints "N passed, M failed".
import { spawn, execFileSync } from "node:child_process";
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { CHROMIUM, port, vitePort } from "./ports.mjs";
import { hostEnv } from "./lib/host_env.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = resolve(CCORE, "../web-ui");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("lab_quantity_lists");
const VITE = vitePort("lab_quantity_lists");
const TOKEN = "labqtylists-token";
const PAGE = `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}&tab=quantities`;
const SHOTS = process.env.QTY1_SHOTS ?? null;
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-qtylists-"));
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

async function main() {
  if (!existsSync(join(UI, "node_modules"))) throw new Error(`no ${UI}/node_modules: run npm ci in web-ui/`);
  console.log(`loadavg ${readFileSync("/proc/loadavg", "utf8").trim()}`);
  execFileSync("node", [join(UI, "scripts/copy-ccore.mjs")], { stdio: "ignore" });
  const model = join(tmp, "frame.bimble");
  copyFileSync(join(CCORE, "golden/frame.json"), model);
  mkdirSync(join(tmp, "home"));
  const host = start("host", BIN, [String(PORT), model], {
    env: hostEnv({ HOME: join(tmp, "home"), BIMBLE_TOKEN: TOKEN, BIMBLE_ORIGINS: `http://localhost:${VITE},http://127.0.0.1:${VITE}` }),
  });
  await waitFor("host 'listening'", 5000, () => host.out.includes("listening") || (host.exitCode !== null && Promise.reject(new Error(host.out))));
  const vite = start("vite", "npx", ["vite", "--port", String(VITE), "--strictPort"], { cwd: UI });
  await waitFor(`Vite on ${VITE}`, 30000, async () => {
    if (vite.exitCode !== null) throw new Error(vite.out);
    return (await fetch(`http://localhost:${VITE}/`)).ok;
  });
  const chrome = start("chromium", CHROMIUM, [
    "--headless=new", "--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--remote-debugging-port=0", "--lang=en-US",
    `--user-data-dir=${join(tmp, "chromium")}`, "--no-first-run", "--no-default-browser-check", "about:blank",
  ]);
  const wsUrl = await waitFor("DevTools line", 60000, () => chrome.out.match(/DevTools listening on (ws:\/\/\S+)/)?.[1]);
  const cdp = await cdpConnect(wsUrl);
  const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank" });
  const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
  const S = (m, p) => cdp.send(m, p, sessionId);
  const consoleLog = [], exceptions = [];
  cdp.on((msg) => {
    if (msg.sessionId !== sessionId) return;
    if (msg.method === "Runtime.consoleAPICalled") consoleLog.push({ level: msg.params.type, text: msg.params.args.map((a) => a.value ?? a.description ?? a.type).join(" ") });
    else if (msg.method === "Runtime.exceptionThrown") exceptions.push(msg.params.exceptionDetails.exception?.description ?? msg.params.exceptionDetails.text);
    else if (msg.method === "Log.entryAdded") consoleLog.push({ level: `log.${msg.params.entry.level}`, text: `${msg.params.entry.text} ${msg.params.entry.url ?? ""}`.trim() });
  });
  for (const d of ["Runtime", "Log", "Page"]) await S(`${d}.enable`);
  await S("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  const evalJs = async (expr) => {
    const r = await S("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
    return r.result.value;
  };
  const B = (body) => evalJs(`(async () => { const b = await import('/src/bimble.ts'); ${body} })()`);
  const exists = (sel) => evalJs(`!!document.querySelector(${JSON.stringify(sel)})`);
  const shot = async (name) => {
    if (!SHOTS) return;
    await sleep(300);
    const { data } = await S("Page.captureScreenshot", { format: "png" });
    writeFileSync(join(SHOTS, `${name}.png`), Buffer.from(data, "base64"));
  };
  const times = [];
  /** A real click (mouse events at the element's centre); ms from its pointerdown's timeStamp to the
   *  animation frame after `cond` holds (null after 5 s). */
  const click = async (name, sel, cond) => {
    const box = await evalJs(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) return null; e.scrollIntoView({ block: 'nearest' }); const r = e.getBoundingClientRect(); return [r.x + r.width / 2, r.y + r.height / 2]; })()`);
    if (!box) { check(`${name}: the control is there (${sel})`, false); return null; }
    await evalJs(`(() => { window.__qT = null; document.addEventListener('pointerdown', (e) => { window.__qT = e.timeStamp; }, { capture: true, once: true }); return true; })()`);
    for (const type of ["mousePressed", "mouseReleased"]) await S("Input.dispatchMouseEvent", { type, x: box[0], y: box[1], button: "left", buttons: type === "mousePressed" ? 1 : 0, clickCount: 1 });
    const ms = await evalJs(`(async () => {
      const b = await import('/src/bimble.ts');
      const raf = () => new Promise((r) => requestAnimationFrame(r));
      const t0 = performance.now();
      while (performance.now() - t0 < 5000) {
        let ok = false; try { ok = await (async () => (${cond}))(); } catch { ok = false; }
        if (ok && window.__qT !== null) { await raf(); return performance.now() - window.__qT; }
        await raf();
      }
      return null; })()`);
    if (typeof ms === "number") times.push([name, ms]);
    return ms;
  };
  const fast = (ms) => typeof ms === "number" && ms < 200;
  const tool = (g, m) => `[data-bb=ribbon] [data-bb=ribbon-tool-${g}-${m}]`;
  const lists = () => B("return b.scheduleList();");
  const table = () => evalJs(`(() => { const t = document.querySelector('[data-bb=quantities-table]'); if (!t) return null;
    const cells = (tr) => [...tr.querySelectorAll('td')].map((td) => td.textContent.trim());
    return { title: document.querySelector('[data-bb=quantities-title]')?.textContent.trim(), head: [...t.querySelectorAll('thead th')].map((th) => th.textContent.trim()),
      groups: [...t.querySelectorAll('tbody tr.g')].map(cells), rows: [...t.querySelectorAll('tbody tr:not(.g)')].map(cells), total: [...t.querySelectorAll('tfoot td')].map((td) => td.textContent.trim()) }; })()`);

  await S("Page.navigate", { url: PAGE });
  await waitFor("data-bb-ready", 30000, () => evalJs("document.documentElement.dataset.bbReady === '1'"));
  await waitFor("the Quantities ribbon", 10000, () => exists(tool(1, 1)));
  await sleep(500);
  const all = await evalJs("[...document.querySelectorAll('[data-bb=ribbon] .tool')].map((e) => [e.getAttribute('data-bb'), e.hasAttribute('data-not-yet'), e.getAttribute('aria-disabled')])");
  check("the tab has 8 tools", all.length === 8, JSON.stringify(all));
  const notYet = all.filter((x) => x[1]).map((x) => x[0]).sort();
  check("not yet: Assign CFC, CFC catalogue, Bar schedule only (capabilities cfc, rebar)", JSON.stringify(notYet) === JSON.stringify(["ribbon-tool-2-1", "ribbon-tool-2-2", "ribbon-tool-3-1"]), JSON.stringify(notYet));
  check("no tool drawn off (every other tool works on an editable model)", all.every((x) => x[1] || x[2] !== "true"), JSON.stringify(all));
  await shot("qty1-ribbon-1280");
  for (const [g, m] of [[2, 1], [2, 2], [3, 1]]) {
    const ms = await click(`not yet ${g}-${m}`, tool(g, m), "!!document.querySelector('[data-bb=not-yet-tip]')");
    const tip = await evalJs("document.querySelector('[data-bb=not-yet-tip]')?.textContent ?? ''");
    check(`tool ${g}-${m}: its tooltip says not available yet, within 200 ms (${ms?.toFixed(1)} ms)`, fast(ms) && /not available|pas encore/i.test(tip), tip);
    await sleep(2700);
  }

  // ---- New quantities ----
  const n0 = (await lists()).length;
  let ms = await click("New quantities", tool(1, 1), "!!document.querySelector('[data-bb=new-quantities-dialog]')");
  check(`New quantities: the dialog within 200 ms (${ms?.toFixed(1)} ms)`, fast(ms));
  await evalJs(`(() => { const e = document.querySelector('[data-bb=new-quantities-name]'); e.value = 'Poteaux'; e.dispatchEvent(new Event('input', { bubbles: true })); return true; })()`);
  await click("tick columns", "[data-bb=new-quantities-columns]", "document.querySelector('[data-bb=new-quantities-columns]').checked");
  await shot("qty1-new-quantities-1280");
  const undo0 = await B("return b.query('history').undo?.length ?? b.query('history').length ?? null;").catch(() => null);
  ms = await click("Create", "[data-bb=new-quantities-create]", "document.querySelector('[data-bb=quantities-title]')?.textContent.trim() === 'Poteaux' && !!document.querySelector('[data-bb=quantities-table] tfoot')");
  const L1 = await lists();
  const pot = L1.find((d) => d.name === "Poteaux");
  check(`Create: the page on Poteaux within 200 ms (${ms?.toFixed(1)} ms)`, fast(ms));
  check("Create: one list more, IfcColumn, Rust's new_schedule columns", L1.length === n0 + 1 && JSON.stringify(pot?.classes) === '["IfcColumn"]' && pot?.fields.join() === "level,name,class,material,volume", JSON.stringify(pot));
  const undo1 = await B("return b.query('history').undo?.length ?? b.query('history').length ?? null;").catch(() => null);
  check("Create: one history step", undo0 === null || undo1 === undo0 + 1, `${undo0} -> ${undo1}`);
  await evalJs("document.querySelector('[data-bb=nav-schedules]')?.getAttribute('aria-expanded') === 'true' || document.querySelector('[data-bb=nav-schedules]')?.click(), true");
  await sleep(200);
  check("the navigator's Quantities lists Poteaux", await exists("[data-bb=nav-schedule][data-name=Poteaux]"));

  // ---- Columns ----
  ms = await click("Columns", tool(1, 2), "!!document.querySelector('[data-bb=quantity-columns-dialog]')");
  check(`Columns: the dialog within 200 ms (${ms?.toFixed(1)} ms), on Poteaux`, fast(ms) && /Poteaux/.test(await evalJs("document.querySelector('[data-bb=quantity-columns-title]')?.textContent ?? ''")));
  for (const c of ["level", "class", "material", "count", "mass", "gross_volume"]) await click(`column ${c}`, `[data-bb=quantity-column-${c}]`, "true");
  for (let i = 0; i < 2; i++) await click("count earlier", "[data-bb=quantity-column-up-count]", "true"); // past global_id (unticked), then mass
  await shot("qty1-columns-1280");
  ms = await click("Apply", "[data-bb=quantity-columns-apply]", "document.querySelectorAll('[data-bb=quantities-table] thead th').length === 5");
  check(`Apply: the table's 5 columns within 200 ms (${ms?.toFixed(1)} ms)`, fast(ms));
  const pot2 = (await lists()).find((d) => d.name === "Poteaux");
  check("Columns: name, volume, count, mass, gross_volume", pot2?.fields.join() === "name,volume,count,mass,gross_volume", JSON.stringify(pot2?.fields));
  let T = await table();
  // hand: 24 columns, 6.0048 m3 net = gross, 15012 kg (see the header)
  check("totals by hand: Total (24) | 6.005 | 24 | 15012 | 6.005", JSON.stringify(T?.total.slice(1)) === JSON.stringify(["6.005", "24", "15012", "6.005"]), JSON.stringify(T?.total));
  check("24 rows (12 a level), 2 storey lines with 3.002 m3 each", T?.rows.length === 24 && T.groups.length === 2 && T.groups.every((g) => g[1] === "3.002"), JSON.stringify(T?.groups));
  await shot("qty1-poteaux-page-1280");

  // ---- CSV = the table ----
  await evalJs(`(() => { window.__qcsv = null; const click = HTMLAnchorElement.prototype.click, mk = URL.createObjectURL, blobs = new Map();
    URL.createObjectURL = (b) => { const u = mk.call(URL, b); blobs.set(u, b); return u; };
    HTMLAnchorElement.prototype.click = function () { const b = blobs.get(this.href); if (b) { b.text().then((t) => { window.__qcsv = t; }); return; } return click.call(this); }; return true; })()`);
  await click("Export CSV", "[data-bb=quantities-export]", "true");
  const csv = await waitFor("the CSV", 5000, () => evalJs("window.__qcsv"));
  const lines = csv.trimEnd().split("\n");
  check("CSV: the table's header line", lines[0] === T.head.join(","), `${lines[0]} vs ${T.head.join(",")}`);
  check("CSV: every row of the table", T.rows.every((r) => lines.includes(r.join(","))), lines.slice(1, 3).join(" | "));
  check("CSV: the total line's values", lines.at(-1).split(",").slice(1).join() === T.total.slice(1).join(), lines.at(-1));

  // ---- Undo, redo ----
  await B("b.undo(); return true;");
  await waitFor("undo", 3000, async () => (await lists()).find((d) => d.name === "Poteaux")?.fields.join() === "level,name,class,material,volume");
  check("undo: Poteaux's columns back to the default", true);
  const redoErr = await B("return b.redo();");
  if (redoErr) console.log(`redo: ${redoErr}`);
  await waitFor("redo", 3000, async () => (await lists()).find((d) => d.name === "Poteaux")?.fields.join() === "name,volume,count,mass,gross_volume");
  check("redo: the chosen columns again", true);

  // ---- By material ----
  ms = await click("By material", tool(3, 2), "document.querySelector('[data-bb=quantities-title]')?.textContent.trim() === 'By material' && document.querySelectorAll('[data-bb=quantities-table] tbody tr.g').length >= 4");
  check(`By material: the page on its list within 200 ms (${ms?.toFixed(1)} ms)`, fast(ms));
  T = await table();
  const mat = T?.groups[0] ?? [];
  // hand (see the header): 72 elements, 443.42 m2, 126.264 m3, 315661 kg
  check("By material: the C30/37 line 72 | 443.42 | 126.264 | 315661", /C30\/37/.test(mat[0] ?? "") && JSON.stringify(mat.slice(-4)) === JSON.stringify(["72", "443.42", "126.264", "315661"]), JSON.stringify(T?.groups));
  check("By material: a line per storey within it (Fondations 12, Niveau 1 30, Niveau 2 30), no element row", T?.groups.length === 4 && T.rows.length === 0 &&
    JSON.stringify(T.groups.slice(1).map((g) => g.at(-4))) === JSON.stringify(["12", "30", "30"]), JSON.stringify(T?.groups));
  await shot("qty1-by-material-1280");
  const nBy = (await lists()).length;
  await click("By material again", tool(3, 2), "true");
  await click("By material once more", tool(3, 2), "document.querySelector('[data-bb=quantities-title]')?.textContent.trim() === 'By material'");
  check("By material again: no second list", (await lists()).length === nBy);

  // ---- Save and open again ----
  await B("return b.runAction('save');");
  await sleep(300);
  await evalJs(`(async () => { const f = await import('/src/files.ts'); return f.openPath(${JSON.stringify(model)}); })()`);
  await sleep(1500);
  const L2 = await lists();
  check("save, open again: Poteaux keeps its five columns, By material is there", L2.find((d) => d.name === "Poteaux")?.fields.join() === "name,volume,count,mass,gross_volume" &&
    L2.some((d) => d.name === "By material" && d.group_by_material === true && d.summary === true), JSON.stringify(L2.map((d) => d.name)));

  const v = times.map((x) => x[1]).sort((a, b) => a - b);
  for (const [n, t] of times) console.log(`time ${n} ${t.toFixed(1)} ms`);
  if (v.length) console.log(`time: ${v.length} clicks, median ${v[Math.floor(v.length / 2)].toFixed(1)} ms, max ${v.at(-1).toFixed(1)} ms; loadavg ${readFileSync("/proc/loadavg", "utf8").trim()}`);
  const IGNORED = [/Synchronous XMLHttpRequest on the main thread is deprecated/, /404 \(Not Found\) http:\/\/localhost:\d+\/favicon\.ico$/];
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
}
console.log(`lab_quantity_lists: ${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
