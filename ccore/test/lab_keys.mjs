// The keyboard shortcuts of the lab (ticket KEY1, web-ui/src/shell/keys.ts): node ccore/test/lab_keys.mjs
// Starts out/tcc/bimble-ccore (or $HOST_BIN) on a copy of golden/frame.json, copies the wasm build
// (scripts/copy-ccore.mjs), Vite from web-ui/ and headless /usr/bin/chromium over raw CDP. Twice, on
// the native host (/?native=1&gl=1) and on ccore.wasm (/?ccore=1&gl=1), with real CDP key events:
//   The table: its rows are Rust's 16 shortcuts (golden/parity/shortcuts.json: the same keys, the
//     same action), every action row's label is the `shortcut` the core lists for that action.
//   Each row where it applies: E, H, O turn their switch (`actions` pressed); T looks down (pitch
//     1.5); F fits again after a wheel zoom; B turns the section box; M the measure tool; Delete
//     removes the selected column (one step), Ctrl Z brings it back, Ctrl Y and Ctrl Shift Z redo;
//     Ctrl S saves (state unsaved false) and the browser's default is prevented; Ctrl K opens the
//     palette (even from a field, as Rust's palette_key); Ctrl N starts a new model; Esc's layers in
//     order: the dialog, the measure tool, the selection, the page (back to the view).
//   And where it must not: E, Delete, Ctrl Z, T, M, Esc (on the tool) typed in a text field do
//     nothing; E with the File menu open does nothing; Ctrl Z with nothing to undo is not handled
//     (its default not prevented); O, T, B in a plan do nothing (lib.rs:3002 `!flat`); P stays inert
//     (the C core never enables paper_view: listed, not run); Ctrl K with a dialog open opens nothing.
//   Every row fired at least once but the inert ones (P), counted by window.__bbKeys.
//   Labels: the ribbon's Edges tooltip "… (E)", the File menu's New "Ctrl N", Save "Ctrl S", Open
//     none (Rust has no Ctrl O), the palette's Edges row "E", the command field "Ctrl K".
//   The plan annotation toggles: enabled, pressed, each off and on (the page's actions follow).
//   The status bar's pointer coordinates (glviewport.ts, lib.rs:2826-2864 and coords.rs): in the
//     column's plan, zoomed in to under 1 mm a pixel, the pointer over the column's corner reads
//     "x … y … · … m" within 1 mm of it (no georeference); one `coordinates` query for one move,
//     none while still; with LV95 (EPSG:2056, E 2 600 000, N 1 200 000) "E … N … · … m (LV95)", the
//     corner's survey point to the cm shown.
// Before KEY1 the lab had no handler for 13 of the 16 rows (parity.md): the E, Delete-of-elements,
// Ctrl S, Ctrl Z checks fail on the old code, window.__bbKeys does not exist, and the status bar's
// readout stayed empty on the C core (query pointer answers null).
// No uncaught page error; no console error or warning but the sync-XHR deprecation and the favicon
// 404. Prints "N passed, M failed"; exits non-zero on a failure. Kills all it starts.
import { spawn, execFileSync } from "node:child_process";
import { copyFileSync, existsSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { CHROMIUM, port, vitePort } from "./ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = resolve(CCORE, "../web-ui");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("lab_keys");
const VITE = vitePort("lab_keys");
const TOKEN = "labkeys-token";
const PAGES = {
  native: `http://localhost:${VITE}/?native=1&gl=1&port=${PORT}&token=${TOKEN}`,
  wasm: `http://localhost:${VITE}/?ccore=1&gl=1`,
};
const RUST_KEYS = JSON.parse(readFileSync(join(CCORE, "golden/parity/shortcuts.json"), "utf8"));
const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-keys-"));
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

// CDP key events: key, code, virtual key code; modifiers 2 = Ctrl, 8 = Shift.
const KEYDEF = {
  Escape: ["Escape", "Escape", 27], Delete: ["Delete", "Delete", 46],
  ...Object.fromEntries("ABCDEFGHIJKLMNOPQRSTUVWXYZ".split("").map((c) => [c, [c.toLowerCase(), `Key${c}`, c.charCodeAt(0)]])),
};

async function keys(mode, S, evalJs) {
  const B = (body) => evalJs(`(async () => { const b = await import('/src/bimble.ts'); ${body} })()`);
  const raf2 = () => evalJs("new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)))");
  /** Presses keys like "Ctrl Shift Z", "E", "Escape" (a real CDP key event; a letter also types its text). */
  const press = async (combo) => {
    const parts = combo.split(" "), name = parts.at(-1);
    const mods = (parts.includes("Ctrl") ? 2 : 0) | (parts.includes("Shift") ? 8 : 0);
    const [key0, code, vk] = KEYDEF[name];
    const key = mods & 8 && key0.length === 1 ? key0.toUpperCase() : key0;
    const text = key.length === 1 && !(mods & 2) ? key : undefined;
    await S("Input.dispatchKeyEvent", { type: text ? "keyDown" : "rawKeyDown", key, code, windowsVirtualKeyCode: vk, modifiers: mods, ...(text ? { text } : {}) });
    await S("Input.dispatchKeyEvent", { type: "keyUp", key, code, windowsVirtualKeyCode: vk, modifiers: mods });
    await raf2();
  };
  const pressed = (name) => B(`return b.query('actions').find((a) => a.name === ${JSON.stringify(name)}).pressed;`);
  const handled = () => evalJs("JSON.stringify(window.__bbKeys?.stats.handled ?? null)").then(JSON.parse);
  const cam = () => evalJs("JSON.stringify(window.__bbGl.camera)").then(JSON.parse);
  const blur = () => evalJs("(document.activeElement && document.activeElement !== document.body && document.activeElement.blur(), document.querySelector('[data-bb=key1-field]')?.remove(), true)");
  /** A text field focused (the page's own fields are on pages and panels; this one is in the view). */
  const field = () => evalJs(`(() => { const i = document.createElement('input'); i.dataset.bb = 'key1-field'; i.style.cssText = 'position:fixed;left:4px;bottom:40px;z-index:50';
    document.body.appendChild(i); i.focus(); return document.activeElement === i; })()`);
  const fieldText = () => evalJs("document.querySelector('[data-bb=key1-field]')?.value ?? null");
  /** The last keydown's default prevented, as a listener after keys.ts's sees it (bubble phase). */
  const lastPrevented = () => evalJs("window.__k1.at(-1)?.prevented ?? null");
  const dialog = () => evalJs("(() => { const d = document.querySelector('.bb-backdrop > *'); return d ? (d.dataset.bb ?? d.querySelector('[data-bb]')?.dataset.bb ?? d.className) : null; })()");
  const undoN = async () => (await B("return b.query('history');")).undo.length;
  const elementIds = async () => (await B("return b.query('elements');")).map((e) => e.id);
  const shell = () => evalJs("document.querySelector('[data-bb-shell]')?.dataset.bbShell ?? null");

  await S("Page.navigate", { url: PAGES[mode] });
  await waitFor(`${mode}: the first frame`, 30000, () => evalJs("document.documentElement.dataset.bbReady === '1' && window.__bbGl && window.__bbGl.firstFrameMs && window.__bbGl.meshRev !== null && !!window.__bbKeys"));
  await evalJs("window.__k1 = []; window.addEventListener('keydown', (e) => window.__k1.push({ key: e.key, prevented: e.defaultPrevented })); true");
  await waitFor(`${mode}: the actions`, 5000, () => B("return b.actions.value.length > 0;"));
  await blur();

  // ---- the table equals Rust's ----
  const table = await evalJs("JSON.stringify(window.__bbKeys.KEYS)").then(JSON.parse);
  const rust = RUST_KEYS.map((r) => `${r.keys}=${r.action ?? (r.keys === "Ctrl Shift Z" ? "redo" : r.keys === "Delete" ? "delete" : r.keys === "Escape" ? "escape" : r.keys === "P" ? "paper_view" : "?")}`);
  const ours = table.map((r) => `${r.keys}=${r.action ?? r.effect}`);
  check(`${mode}: the key table is Rust's 16 shortcuts, the same actions`, JSON.stringify([...ours].sort()) === JSON.stringify([...rust].map((s) => s.replace("Ctrl K=palette", "Ctrl K=palette")).sort()),
    `\n  lab  ${ours.join(", ")}\n  Rust ${rust.join(", ")}`);
  const acts = await B("return b.query('actions');");
  const labelMismatch = acts.filter((a) => a.shortcut && !table.some((r) => r.keys === a.shortcut && r.action === a.name)).map((a) => `${a.name}: ${a.shortcut}`);
  const firstOf = (name) => table.find((r) => r.action === name)?.keys ?? null;
  // Rust's Action::keys names no key for PaperView, handle_keys binds P to it (lib.rs:2988): the one exception.
  const shownMismatch = acts.filter((a) => (a.shortcut ?? null) !== firstOf(a.name) && !(a.name === "paper_view" && a.shortcut === null)).map((a) => `${a.name}: core ${a.shortcut} table ${firstOf(a.name)}`);
  check(`${mode}: every shortcut the core lists (Action::keys) is the table's first row for that action`, labelMismatch.length === 0 && shownMismatch.length === 0, [...labelMismatch, ...shownMismatch].join("; "));

  // ---- the switches: E, H, O (3D), B (3D), M ----
  for (const [k, name] of [["E", "edges"], ["H", "hatching"], ["O", "orthographic"], ["B", "section_box"], ["M", "measure"]]) {
    const p0 = await pressed(name);
    await press(k);
    const p1 = await pressed(name);
    await press(k);
    const p2 = await pressed(name);
    check(`${mode}: ${k} turns ${name} (${p0} -> ${p1} -> ${p2})`, p1 === !p0 && p2 === p0);
  }
  // Not while typing: E typed in a field turns nothing and types its letter.
  {
    const p0 = await pressed("edges");
    await field();
    await press("E");
    check(`${mode}: E typed in a text field: edges kept, the letter typed`, (await pressed("edges")) === p0 && (await fieldText()) === "e", `${await fieldText()}`);
    await blur();
  }
  // Not with a dialog open: E with the File menu open turns nothing.
  {
    const p0 = await pressed("edges");
    await evalJs("document.querySelector('[data-bb=quickbar-file]').click(), true");
    await waitFor(`${mode}: the File menu`, 3000, async () => (await dialog()) === "file-menu");
    await press("E");
    check(`${mode}: E with the File menu open: edges kept`, (await pressed("edges")) === p0);
    // The File menu's shortcuts: the key table's (New Ctrl N, Save Ctrl S, Open none: Rust has no Ctrl O).
    const kbd = await evalJs("Object.fromEntries([...document.querySelectorAll('[data-bb^=file-menu-]')].filter((b) => b.querySelector('[data-bb=file-menu-kbd]')).map((b) => [b.dataset.bb, b.querySelector('[data-bb=file-menu-kbd]').textContent.trim()]))");
    check(`${mode}: File menu labels: New "Ctrl N", Save "Ctrl S", Open none`, kbd["file-menu-new"] === "Ctrl N" && kbd["file-menu-save"] === "Ctrl S" && kbd["file-menu-open"] === "", JSON.stringify(kbd));
    // Ctrl K with a dialog open opens nothing (Rust: palette only when none is open).
    await press("Ctrl K");
    check(`${mode}: Ctrl K with the File menu open: still the File menu`, (await dialog()) === "file-menu", await dialog());
    // Esc layer 1: the dialog closes.
    await press("Escape");
    check(`${mode}: Esc closes the dialog first`, (await dialog()) === null, await dialog());
  }
  // Labels: the ribbon's Edges tooltip, the command field.
  {
    const tip = await evalJs("[...document.querySelectorAll('.tool')].map((t) => t.getAttribute('title')).filter(Boolean)");
    check(`${mode}: ribbon tooltips name their key (one ends with "(E)" on the View tab or "(M)")`, tip.some((s) => /\((E|M|F|B)\)$/.test(s)), JSON.stringify(tip));
    check(`${mode}: the command field shows "Ctrl K"`, (await evalJs("document.querySelector('[data-bb=command-field-kbd]')?.textContent.trim()")) === "Ctrl K");
  }
  // T: Top (pitch 1.5); F: fit after a wheel zoom.
  {
    await press("T");
    await waitFor(`${mode}: top`, 3000, async () => Math.abs((await cam()).pitch - 1.5) < 1e-6).catch(() => {});
    check(`${mode}: T looks down (pitch 1.5)`, Math.abs((await cam()).pitch - 1.5) < 1e-6, JSON.stringify(await cam()));
    await press("F");
    const d0 = (await cam()).distance;
    const r = await evalJs("JSON.stringify(document.getElementById('bimble').getBoundingClientRect())").then(JSON.parse);
    await S("Input.dispatchMouseEvent", { type: "mouseWheel", x: r.left + r.width / 2, y: r.top + r.height / 2, deltaX: 0, deltaY: -400 });
    await waitFor(`${mode}: zoomed`, 3000, async () => Math.abs((await cam()).distance - d0) > 1e-3 * d0).catch(() => {});
    const dz = (await cam()).distance;
    await press("F");
    const df = (await cam()).distance;
    check(`${mode}: F fits again after a wheel zoom (${d0.toFixed(0)} -> ${dz.toFixed(0)} -> ${df.toFixed(0)})`, Math.abs(dz - d0) > 1e-3 * d0 && Math.abs(df - d0) < 1e-6 * d0);
  }
  // M then Esc: Esc's tool layer (the measure tool off); typing: Esc in a field keeps the tool.
  {
    await press("M");
    await field();
    await press("Escape");
    const kept = await pressed("measure");
    await blur();
    await press("Escape");
    check(`${mode}: Esc in a field keeps the measure tool; then Esc stops it`, kept === true && (await pressed("measure")) === false);
  }
  // Delete: the selected column goes (one step); Ctrl Z, Ctrl Y, Ctrl Shift Z.
  {
    const ids0 = await elementIds();
    const col = (await B("return b.query('elements');")).find((e) => e.class === "IfcColumn")?.id;
    const h0 = await undoN();
    // Ctrl Z with nothing to undo (a fresh model): not handled, its default left alone.
    const hk0 = (await handled())["Ctrl Z"] ?? 0;
    await press("Ctrl Z");
    // FIXUI 7: the fresh model has nothing to undo, or the check tests nothing (it passed on h0 > 0).
    check(`${mode}: Ctrl Z with nothing to undo: not handled, default not prevented`, h0 === 0 && ((await handled())["Ctrl Z"] ?? 0) === hk0 && (await lastPrevented()) === false, `${h0} steps`);
    await B(`return b.select([${JSON.stringify(col)}]);`);
    await waitFor(`${mode}: selected`, 3000, async () => (await B("return b.state.value.selection;")).length === 1);
    await field();
    await press("Delete");
    check(`${mode}: Delete in a text field: the column kept`, (await elementIds()).includes(col));
    await blur();
    await press("Delete");
    await waitFor(`${mode}: deleted`, 3000, async () => !(await elementIds()).includes(col)).catch(() => {});
    const ids1 = await elementIds();
    check(`${mode}: Delete removes the selected column, one step`, !ids1.includes(col) && ids1.length === ids0.length - 1 && (await undoN()) === h0 + 1, `${ids0.length} -> ${ids1.length}, steps ${h0} -> ${await undoN()}`);
    check(`${mode}: the model is unsaved`, (await B("return b.state.value.unsaved;")) === true);
    await field();
    await press("Z");
    await press("Ctrl Z");
    check(`${mode}: Ctrl Z in a text field: no undo`, !(await elementIds()).includes(col));
    await blur();
    await press("Ctrl Z");
    await waitFor(`${mode}: undone`, 3000, async () => (await elementIds()).includes(col)).catch(() => {});
    check(`${mode}: Ctrl Z brings the column back`, (await elementIds()).includes(col) && (await lastPrevented()) === true);
    await press("Ctrl Y");
    await waitFor(`${mode}: redone`, 3000, async () => !(await elementIds()).includes(col)).catch(() => {});
    check(`${mode}: Ctrl Y deletes it again`, !(await elementIds()).includes(col));
    await press("Ctrl Z");
    await waitFor(`${mode}: undone 2`, 3000, async () => (await elementIds()).includes(col)).catch(() => {});
    await press("Ctrl Shift Z");
    await waitFor(`${mode}: redone 2`, 3000, async () => !(await elementIds()).includes(col)).catch(() => {});
    check(`${mode}: Ctrl Shift Z redoes too`, !(await elementIds()).includes(col));
    await press("Ctrl Z");
    await waitFor(`${mode}: undone 3`, 3000, async () => (await elementIds()).includes(col)).catch(() => {});
    // Esc's selection layer: the column selected, Esc lets it go.
    await B(`return b.select([${JSON.stringify(col)}]);`);
    await waitFor(`${mode}: selected again`, 3000, async () => (await B("return b.state.value.selection;")).length === 1);
    await press("Escape");
    await waitFor(`${mode}: nothing selected`, 3000, async () => (await B("return b.state.value.selection;")).length === 0).catch(() => {});
    check(`${mode}: Esc clears the selection`, (await B("return b.state.value.selection;")).length === 0);
  }
  // Ctrl S: saved, the browser's default prevented (the column deleted again: something to save).
  {
    await B(`return b.select([${JSON.stringify((await B("return b.query('elements');")).find((e) => e.class === "IfcColumn")?.id)}]);`);
    await press("Delete");
    check(`${mode}: unsaved before Ctrl S`, (await B("return b.state.value.unsaved;")) === true);
    await press("Ctrl S");
    await waitFor(`${mode}: saved`, 5000, async () => (await B("return b.query('state').unsaved;")) === false).catch(() => {});
    check(`${mode}: Ctrl S saves (unsaved false), the browser's Save dialog prevented`, (await B("return b.query('state').unsaved;")) === false && (await lastPrevented()) === true);
    await press("Ctrl S");
    check(`${mode}: Ctrl S with nothing to save: still no browser dialog (prevented)`, (await lastPrevented()) === true);
  }
  // Ctrl K: the palette, also from a text field (Rust's palette_key comes first).
  {
    await field();
    await press("Ctrl K");
    await waitFor(`${mode}: the palette`, 3000, async () => (await dialog()) === "palette").catch(() => {});
    check(`${mode}: Ctrl K from a text field opens the palette`, (await dialog()) === "palette", await dialog());
    const row = await evalJs("[...document.querySelectorAll('[data-bb=palette-item]')].map((r) => [r.querySelector('.lab')?.textContent, r.querySelector('.kbd')?.textContent]).filter((x) => x[1] === 'E').length");
    check(`${mode}: the palette shows E on one row (Edges)`, row === 1, String(row));
    await press("Escape");
    await blur();
  }
  // 2D: O, T, B do nothing in a plan; P inert (paper_view never enabled by the C core).
  {
    const plan = (await B("return b.openViews.value;")).views.find((v) => v.type === "plan");
    await B(`return b.openView(${JSON.stringify(plan.id)});`);
    await waitFor(`${mode}: the plan`, 5000, async () => (await cam())?.mode === "plan");
    const o0 = await pressed("orthographic"), b0 = await pressed("section_box"), c0 = await cam();
    const paper0 = (await B("return b.openViews.value;")).paper.on;
    const hp0 = (await handled()).P ?? 0;
    await press("O"); await press("T"); await press("B");
    const c1 = await cam();
    check(`${mode}: in a plan O, T, B do nothing`, (await pressed("orthographic")) === o0 && (await pressed("section_box")) === b0 && c1.pitch === c0.pitch && c1.yaw === c0.yaw);
    // FIXUI 7: paper_view is enabled in a plan (PAPER1): P puts the plan on paper, P again back
    // (the check said "P inert" and passed whenever paper_view was enabled).
    const pe = (await B("return b.query('actions');")).find((a) => a.name === "paper_view")?.enabled;
    await press("P");
    await waitFor(`${mode}: P on paper`, 3000, async () => (await B("return b.openViews.value;")).paper.on === !paper0).catch(() => null);
    const paper1 = (await B("return b.openViews.value;")).paper.on, hp1 = (await handled()).P ?? 0;
    await press("P");
    await waitFor(`${mode}: P back`, 3000, async () => (await B("return b.openViews.value;")).paper.on === paper0).catch(() => null);
    const paper2 = (await B("return b.openViews.value;")).paper.on;
    check(`${mode}: in a plan P switches the view on paper and back (paper_view enabled: ${pe})`,
      pe === true && paper1 === !paper0 && hp1 === hp0 + 1 && paper2 === paper0, `paper ${paper0} -> ${paper1} -> ${paper2}, P handled ${hp0} -> ${hp1}`);
    // E still works in a plan.
    const e0 = await pressed("edges");
    await press("E");
    check(`${mode}: E in a plan turns edges`, (await pressed("edges")) === !e0);
    await press("E");
    const three = (await B("return b.openViews.value;")).views.find((v) => v.type === "three_d");
    await B(`return b.openView(${JSON.stringify(three.id)});`);
    await waitFor(`${mode}: 3D again`, 5000, async () => (await cam())?.mode === "3d");
  }
  // Esc's page layer: a page, Esc back to the view.
  {
    await evalJs("(async () => { const s = await import('/src/shell/send.ts'); s.send({ do: 'showPage', page: 'quantities' }); })()");
    await waitFor(`${mode}: the page`, 3000, async () => (await shell()) === "model.page");
    await press("E"); // a view key on a page: nothing
    await press("Escape");
    await waitFor(`${mode}: the view`, 3000, async () => (await shell()) === "model.view").catch(() => {});
    check(`${mode}: Esc on a page goes back to the view`, (await shell()) === "model.view", await shell());
  }
  // Every row fired once at least, but the inert ones.
  {
    const h = await handled();
    const inert = table.filter((r) => r.action === "paper_view").map((r) => r.keys);
    const missing = table.filter((r) => !inert.includes(r.keys) && r.keys !== "Ctrl N" && !h[r.keys]).map((r) => r.keys);
    check(`${mode}: every row ran but Ctrl N (last) and the inert ${inert.join(", ")} (${Object.keys(h).length} rows)`, missing.length === 0, `not run: ${missing.join(", ")}`);
  }
  // ---- the status bar's pointer coordinates (lib.rs:2826-2864, coords.rs text) ----
  {
    const readout = () => evalJs("document.querySelector('[data-bb=status-coords]')?.textContent.trim() ?? ''");
    const num = (t) => Number(t.replace(/['\u2019]/g, ""));
    const col = (await B("return b.query('elements');")).find((e) => e.class === "IfcColumn");
    const el = await B(`return b.element(${JSON.stringify(col.id)});`);
    const o = el.placement.origin, w = el.generator_params.width ?? el.generator_params.size ?? 300, d = el.generator_params.depth ?? w;
    const plan = (await B("return b.openViews.value;")).views.find((v) => v.type === "plan" && v.look === "down" && v.storey === el.storey) ??
      (await B("return b.openViews.value;")).views.find((v) => v.type === "plan");
    await B(`return b.openView(${JSON.stringify(plan.id)});`);
    await waitFor(`${mode}: the plan`, 5000, async () => (await cam())?.mode === "plan");
    await press("F");
    // A corner of the column (its box's min x, min y: the placement origin is its centre), 2 mm inside.
    const corner = [o[0] - w / 2 + 2, o[1] - d / 2 + 2];
    const at = async () => JSON.parse(await evalJs(`JSON.stringify(window.__bbGl.toScreen(${corner[0]}, ${corner[1]}, ${o[2] + 1000}))`));
    // Zoomed in about the corner until a pixel is under 1 mm (the wheel zooms about the pointer).
    for (let i = 0; i < 40 && (await cam()).half * 2 / 800 > 0.8; i++) {
      const p = await at();
      await S("Input.dispatchMouseEvent", { type: "mouseWheel", x: p[0], y: p[1], deltaX: 0, deltaY: -500 });
      await raf2();
    }
    const mmPx = (await cam()).half * 2 / 800;
    const q0 = await evalJs("window.__bbGl.pointerQueries");
    const p = await at();
    await S("Input.dispatchMouseEvent", { type: "mouseMoved", x: p[0], y: p[1], button: "none" });
    await waitFor(`${mode}: the readout`, 3000, async () => (await readout()) !== "").catch(() => {});
    const t0 = await readout();
    const m = /^x (\S+) y (\S+) · (\S+) m$/.exec(t0);
    const got = m ? [num(m[1]) * 1000, num(m[2]) * 1000] : null;
    check(`${mode}: the status bar over a column corner (no georeference): "x … y … · … m" within 1 mm (${mmPx.toFixed(2)} mm a pixel)`,
      got && Math.abs(got[0] - corner[0]) <= 1 + mmPx / 2 && Math.abs(got[1] - corner[1]) <= 1 + mmPx / 2, `${t0} vs ${corner}`);
    // Still pointer: no more query; moved: one more at most a frame.
    for (let i = 0; i < 5; i++) await raf2();
    const q1 = await evalJs("window.__bbGl.pointerQueries");
    check(`${mode}: the pointer still: no query (${q1 - q0} for the one move)`, q1 - q0 === 1, `${q1 - q0}`);
    // With a georeference (LV95, as G2's): "E … N … · … m (LV95)", east and north offset by the corner.
    await B("return b.georefSet({ crs: 'EPSG:2056', east_m: 2600000, north_m: 1200000, height_m: 432.1, rotation_deg: 0 });");
    await S("Input.dispatchMouseEvent", { type: "mouseMoved", x: p[0] + 1, y: p[1], button: "none" });
    await S("Input.dispatchMouseEvent", { type: "mouseMoved", x: p[0], y: p[1], button: "none" });
    await waitFor(`${mode}: the survey readout`, 3000, async () => /\(LV95\)$/.test(await readout())).catch(() => {});
    const t1 = await readout();
    console.log(`${mode}: readout over the corner (${corner}): "${t0}"; with LV95: "${t1}"`);
    const g = /^E (\S+) N (\S+) · (\S+) m \(LV95\)$/.exec(t1);
    const gv = g ? [num(g[1]), num(g[2])] : null;
    check(`${mode}: with a georeference: "E … N … · … m (LV95)", the corner's survey point to the cm shown`,
      gv && Math.abs(gv[0] - (2600000 + corner[0] / 1000)) <= 0.006 + mmPx / 2000 && Math.abs(gv[1] - (1200000 + corner[1] / 1000)) <= 0.006 + mmPx / 2000, `${t1} vs ${corner}`);
    await B("return b.undo();"); // the georeference off again
    await S("Input.dispatchMouseEvent", { type: "mouseMoved", x: 5, y: 5, button: "none" });
    const three = (await B("return b.openViews.value;")).views.find((v) => v.type === "three_d");
    await B(`return b.openView(${JSON.stringify(three.id)});`);
    await waitFor(`${mode}: 3D again (coords)`, 5000, async () => (await cam())?.mode === "3d");
  }
  // The plan annotation toggles (Annotate › On plans; C: session/toggles.h, toggles_test.c draws them):
  // enabled with a model, pressed by default, each turns off and on, the page's actions follow.
  {
    const names = ["section_labels", "openings", "section_marks", "slab_levels"];
    const st = await B(`return b.query('actions').filter((a) => ${JSON.stringify(names)}.includes(a.name)).map((a) => [a.enabled, a.pressed]);`);
    check(`${mode}: the four plan annotation toggles enabled and pressed`, JSON.stringify(st) === JSON.stringify(names.map(() => [true, true])), JSON.stringify(st));
    const off = [];
    for (const n of names) {
      await B(`return b.runAction(${JSON.stringify(n)});`);
      off.push(await B(`return b.actions.value.find((a) => a.name === ${JSON.stringify(n)}).pressed;`));
      await B(`return b.runAction(${JSON.stringify(n)});`);
    }
    check(`${mode}: each toggle turns off (the page's actions follow) and on again`, off.every((x) => x === false) &&
      (await B(`return b.actions.value.filter((a) => ${JSON.stringify(names)}.includes(a.name)).every((a) => a.pressed);`)), JSON.stringify(off));
  }
  // Ctrl N: a new model (the model is saved: no question); not from a field.
  {
    const f0 = await B("return b.state.value.file;");
    await field();
    await press("Ctrl N");
    await raf2();
    check(`${mode}: Ctrl N in a text field: the same file`, (await B("return b.state.value.file;")) === f0);
    await blur();
    await press("Ctrl N");
    // The model has unsaved changes here (Rust's dirty flag stays set after an undo: ACT1), so the
    // shell asks first (UX1's confirmClose): answer Discard, as a user would.
    await waitFor(`${mode}: a new model`, 8000, async () => {
      await evalJs("document.querySelector('[data-bb=shell-confirm-discard]')?.click()");
      return (await B("return b.state.value.file;")) !== f0;
    }).catch(() => {});
    check(`${mode}: Ctrl N starts a new model, the browser's default prevented`, (await B("return b.state.value.file;")) !== f0 && (await lastPrevented()) === true, `${f0} -> ${await B("return b.state.value.file;")}`);
  }
}

async function main() {
  if (!existsSync(join(UI, "node_modules"))) throw new Error(`no ${UI}/node_modules: run npm ci in web-ui/`);
  execFileSync("node", [join(UI, "scripts/copy-ccore.mjs")], { stdio: "ignore" });
  const model = join(tmp, "frame.bimble");
  copyFileSync(join(CCORE, "golden/frame.json"), model);
  const h = start("host", BIN, [String(PORT), model], {
    env: { ...process.env, BIMBLE_TOKEN: TOKEN, BIMBLE_ORIGINS: `http://localhost:${VITE},http://127.0.0.1:${VITE}` },
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
  // The wasm Save downloads the file (ccore.ts): into the temp dir, no prompt.
  await S("Browser.setDownloadBehavior", { behavior: "allow", downloadPath: tmp }).catch(() => {});
  await S("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  const evalJs = async (expr) => {
    const r = await S("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
    return r.result.value;
  };
  for (const mode of ["native", "wasm"]) await keys(mode, S, evalJs);

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
