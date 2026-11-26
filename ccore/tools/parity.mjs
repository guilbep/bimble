// Runtime parity of the Rust egui app (the reference) and the C core + the Vue lab (ticket PAR2).
// Both cores answer the same calls over the same transport (crates/bimble-viewer/src/serve.rs):
// `POST /<op>/<name>?t=<token>`. This tool runs the same scenarios on both and asks both the same
// questions: every action (enabled, pressed), the state, every query name either dispatcher knows
// (status + the JSON shape of the answer), which ops and tools each one knows.
//
//   node ccore/tools/parity.mjs --rust    the Rust goldens: runs the headless native twin
//                                         (`bimble view`, BIMBLE_SERVE + BIMBLE_HEADLESS=1, ADR 0066)
//                                         and writes golden/parity/ (per scenario: actions.json,
//                                         state.json, queries.txt, steps.txt; ops.txt, tools.txt;
//                                         settings.json, shortcuts.json, ribbon.json from the Rust
//                                         sources; meta.json). Needs a built `bimble`: $RUST_BIN, else
//                                         $CARGO_TARGET_DIR/debug/bimble, else target/debug/bimble.
//                                         Rust is not on main (ADR 0072): run it in a worktree of the
//                                         branch oracle/rust, then copy golden/parity/ back.
//   node ccore/tools/parity.mjs           runs the C host (out/tcc/bimble-ccore or $HOST_BIN) on the
//                                         same scenarios (answers in out/parity/c/), parses web-ui/src,
//                                         prints the counts
//   node ccore/tools/parity.mjs --write   the same, and writes results/parity.md (never edit it by hand)
//   node ccore/tools/parity.mjs --check   the same as no flag, for CI: exits 1 only when the C host
//                                         crashes, drops a connection, answers 5xx, or answers a query
//                                         or tool call with malformed JSON. Missing features never fail.
// The Rust twin reads no user settings: XDG_CONFIG_HOME is a scratch folder (no recent files, no name).
// Fixtures are synthetic (golden/frame.json, golden/sheets/model.json, golden/ifc/frame.ifc), copied to
// out/parity/work/ first: a scenario never writes to golden/. Port: test/ports.mjs "parity".
import { spawn, execFileSync } from "node:child_process";
import { copyFileSync, existsSync, mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import http from "node:http";
import { dirname, join, relative, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { port } from "../test/ports.mjs";

// 3D1 (ADR 0078): Rust's 3D speed features and the C lab's state; update a row when a ticket ports one.
const THREED_SPEED = [
  { feature: "Box tree for picks", rust: "bimble-geom bvh.rs, pick_hit_bvh", ported: false, note: "the page tests each element's box before its triangles (linear); MEAS1's measure tool likewise in the core; frames.md: a pick answers well under 200 ms, so no tree yet" },
  { feature: "Copies as instances", rust: "bimble-geom dedup.rs, instanced draws", ported: true, note: "dedup in ccore/src/scene.c; BMSH version 3 (ccore/src/scene_bmsh.h) sends each shape once; glviewport.ts draws shapes of 4 copies or more instanced" },
  { feature: "Frustum culling", rust: "bimble-render clusters, DrawOptions::cull", ported: true, note: "web-ui/src/gl/cull.ts: spatial batches of at most 32,768 triangles culled as wholes" },
  { feature: "Moving budget", rust: "next_moving_px, moving_fps, DrawOptions::min_radius", ported: true, note: "ported with a change (ADR 0078): a triangle budget from moving_fps, largest on screen first, unfinished batches as box outlines, completed in slices after the stop (Rust's px threshold needs HLOD proxies)" },
  { feature: "HLOD proxies", rust: "bimble-geom hlod.rs, bimble-render proxies.rs", ported: false, note: "next ticket (ADR 0078): an octree of simplified stand-ins drawn under a pixel of error" },
  { feature: "GPU occlusion", rust: "gpu_occlusion.rs, gpu_occlusion_poll", ported: false, note: "WebGL2 occlusion queries answer a frame late; Rust ran it on native backends only (ADR 0036 #1)" },
];

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const ROOT = resolve(CCORE, "..");
const GOLD = join(CCORE, "golden/parity");
const OUT = join(CCORE, "out/parity");
const WORK = join(OUT, "work");
const REPORT = join(CCORE, "results/parity.md");
const UI = join(ROOT, "web-ui/src");
const PORT = port("parity", 47440);
const TOKEN = "parity-token";
const argv = process.argv.slice(2);
const MODE = argv.includes("--rust") ? "rust" : argv.includes("--check") ? "check" : argv.includes("--write") ? "write" : "print";
const rel = (p) => relative(ROOT, p);
const read = (p) => readFileSync(p, "utf8");
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ---------------------------------------------------------------- transport and hosts

const agent = new http.Agent({ keepAlive: false });
/** One call: {status, body}; status 0 when the connection fails (the host died or dropped it). */
function call(op, name, body = "") {
  return new Promise((res) => {
    const r = http.request({ host: "127.0.0.1", port: PORT, method: "POST", path: `/${op}/${encodeURIComponent(name)}?t=${TOKEN}`, agent,
      headers: { "Content-Type": "text/plain" }, timeout: 20000 }, (rs) => {
      let b = "";
      rs.setEncoding("utf8");
      rs.on("data", (d) => (b += d));
      rs.on("end", () => res({ status: rs.statusCode, body: b }));
      rs.on("error", (e) => res({ status: 0, body: String(e) }));
    });
    r.on("timeout", () => r.destroy(new Error("timeout")));
    r.on("error", (e) => res({ status: 0, body: String(e.message ?? e) }));
    r.end(body);
  });
}

function rustBin() {
  if (process.env.RUST_BIN) return process.env.RUST_BIN;
  const t = process.env.CARGO_TARGET_DIR ? resolve(process.env.CARGO_TARGET_DIR) : join(ROOT, "target");
  return join(t, "debug/bimble");
}
const HOST_BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");

/** Starts one host; resolves {proc, log(), exited, dead()} once it prints "listening". */
async function startHost(side, model) {
  let cmd, args, env = { ...process.env, BIMBLE_TOKEN: TOKEN };
  if (side === "rust") {
    cmd = rustBin();
    args = model ? ["view", model] : ["view"];
    delete env.DISPLAY;
    delete env.WAYLAND_DISPLAY;
    const xdg = join(WORK, "xdg");
    rmSync(xdg, { recursive: true, force: true });
    mkdirSync(xdg, { recursive: true });
    Object.assign(env, { BIMBLE_SERVE: String(PORT), BIMBLE_HEADLESS: "1", XDG_CONFIG_HOME: xdg });
  } else {
    cmd = HOST_BIN;
    args = model ? [String(PORT), model] : [String(PORT)];
  }
  if (!existsSync(cmd)) throw new Error(`parity: ${cmd} not found (${side === "rust" ? "cargo build -p bimble-cli on the branch oracle/rust, or RUST_BIN" : "ccore/build.sh tcc, or HOST_BIN"})`);
  const proc = spawn(cmd, args, { env, stdio: ["ignore", "ignore", "pipe"] });
  let log = "", code = null;
  proc.stderr.on("data", (d) => (log += d));
  const exited = new Promise((r) => proc.on("exit", (c, sig) => { code = sig ?? c; r(code); }));
  for (let i = 0; i < 3000 && !log.includes("listening") && code === null; i++) await sleep(10);
  if (!log.includes("listening")) {
    proc.kill("SIGKILL");
    throw new Error(`parity: the ${side} host did not start: ${log.trim().slice(0, 400)}`);
  }
  return { proc, exited, log: () => log, dead: () => code !== null, code: () => code };
}

async function stopHost(h) {
  if (!h.dead()) h.proc.kill("SIGTERM");
  const t = setTimeout(() => h.proc.kill("SIGKILL"), 5000);
  await h.exited;
  clearTimeout(t);
}

// ---------------------------------------------------------------- scenarios

const FIX = { frame: "frame.json", sheets: "sheets/model.json", ifc: "ifc/frame.ifc" };
function workCopy(key) {
  const src = join(CCORE, "golden", FIX[key]);
  mkdirSync(join(WORK, key), { recursive: true });
  const dst = join(WORK, key, FIX[key].split("/").pop());
  copyFileSync(src, dst);
  return dst;
}

const isPlan = (w) => w?.type === "plan" || w?.kind === "plan" || w?.kind?.type === "plan";
const parse = (t) => { try { return JSON.parse(t); } catch { return undefined; } };
const clean = (s) => String(s).split(WORK).join("<work>").split(ROOT).join("<repo>");

/** A step: runs on a host, returns [label, {status, body}]. */
const step = {
  action: (name, body = "") => async () => [`action ${name}${body ? " " + body : ""}`, await call("action", name, body)],
  undo: () => async () => ["undo", await call("undo", "-")],
  plan: () => async () => {
    const ov = parse((await call("query", "open_views")).body);
    const v = ov?.views?.find(isPlan);
    if (!v) return ["tab open <first plan>", { status: -1, body: "no plan view in open_views" }];
    const b = JSON.stringify({ op: "open", tab: { type: "view", id: v.id } });
    return [`tab ${b}`, await call("tab", "-", b)];
  },
  select: () => async () => {
    const els = parse((await call("query", "elements", JSON.stringify({ class: "IfcColumn" }))).body)
      ?? parse((await call("query", "elements")).body);
    const c = Array.isArray(els) ? els.find((e) => e.class === "IfcColumn") ?? els[0] : null;
    if (!c) return ["select <first IfcColumn>", { status: -1, body: "no element" }];
    return [`select ["${c.id}"]`, await call("select", "-", JSON.stringify([c.id]))];
  },
  /** A new sheet, shown: `action new_sheet` (an undoable "add the sheet", then the sheet is shown), on both
   * sides (the C core since FAKE1). */
  sheet: () => async () => ["action new_sheet", await call("action", "new_sheet")],
  move: () => async () => {
    const els = parse((await call("query", "elements")).body);
    const c = Array.isArray(els) ? els.find((e) => e.class === "IfcColumn") : null;
    if (!c) return ["verb move", { status: -1, body: "no column" }];
    const b = JSON.stringify({ ids: [c.id], d: [100, 0, 0] });
    return [`verb move ${b}`, await call("verb", "move", b)];
  },
};

// [name, fixture, how it opens ("argv", or "open" = action open {path} on the C host), steps]
const SCENARIOS = [
  ["empty", null, []],
  ["new", null, [step.action("new")]],
  ["frame", "frame", []],
  ["frame_plan", "frame", [step.plan()]],
  ["frame_selected", "frame", [step.select()]],
  ["frame_selected_plan", "frame", [step.plan(), step.select()]],
  ["frame_sheet", "sheets", [step.sheet()]],
  ["ifc_readonly", "ifc", []],
  ["placing", "frame", [step.action("place_column")]],
  ["measuring", "frame", [step.action("measure")]],
  ["section_box_on", "frame", [step.action("section_box")]],
  ["dirty", "frame", [step.move()]],
  ["after_undo", "frame", [step.move(), step.undo()]],
];

// ---------------------------------------------------------------- the Rust sources (names, ribbon, settings, keys)

const RS = join(ROOT, "crates/bimble-viewer/src");
const snake = (s) => s.replace(/([a-z0-9])([A-Z])/g, "$1_$2").toLowerCase();
/** The text of `fn <name>` in `src` (to its closing brace at the same indent) and its first line number. */
function fnText(src, name) {
  const lines = src.split("\n");
  const i = lines.findIndex((l) => new RegExp(`\\bfn ${name}\\b`).test(l));
  if (i < 0) return { text: "", line: 0 };
  const ind = /^\s*/.exec(lines[i])[0];
  let j = i + 1;
  while (j < lines.length && lines[j] !== `${ind}}`) j++;
  return { text: lines.slice(i, j + 1).join("\n"), line: i + 1 };
}
const lineOf = (src, from, idx) => src.slice(0, idx).split("\n").length + from - 1;

/** Action enum names in a code fragment → action names (Place(k) and SheetTool(t) included). */
function actionNames(frag) {
  const out = [];
  const re = /k\(edit::Kind::(\w+)\)|Action::Place\(edit::Kind::(\w+)\)|Action::SheetTool\(layout::Tool::(\w+)\)|Action::(\w+)/g;
  for (let m; (m = re.exec(frag));) {
    if (m[1] || m[2]) out.push("place_" + (m[1] || m[2]).toLowerCase());
    else if (m[3]) out.push("sheet_tool_" + m[3].toLowerCase());
    else out.push(snake(m[4]));
  }
  return out;
}

/** Where each action sits in the egui ribbon: {name: [{tab, group, line}]}, File menu and quick access too. */
function rustRibbon() {
  const src = read(join(RS, "ribbon.rs"));
  const where = {};
  const add = (n, tab, group, line) => (where[n] ??= []).push({ tab, group, line: `ribbon.rs:${line}` });
  const panel = fnText(src, "ribbon_panel");
  const tabRe = /Tab::(\w+) => \{/g;
  const tabs = [];
  for (let m; (m = tabRe.exec(panel.text));) tabs.push([m.index, m[1]]);
  const grpRe = /self\.group\(\s*ui,\s*"([^"]+)",\s*&\[([\s\S]*?)\],\s*&mut run/g;
  for (let m; (m = grpRe.exec(panel.text));) {
    const tab = tabs.filter(([i]) => i < m.index).at(-1)?.[1] ?? "?";
    for (const n of actionNames(m[2])) add(n, tab, m[1], lineOf(panel.text, panel.line, m.index));
  }
  const fm = fnText(src, "file_menu");
  const fmGroups = /let groups: &\[&\[Action\]\] = &\[([\s\S]*?)\];\n/.exec(fm.text);
  if (fmGroups) for (const n of actionNames(fmGroups[1])) add(n, "File", "File menu", lineOf(fm.text, fm.line, fmGroups.index));
  const hist = /ui\.button\(Action::(\w+)\.label\(\)\)/.exec(fm.text);
  if (hist) add(snake(hist[1]), "File", "File menu", lineOf(fm.text, fm.line, hist.index));
  const tr = fnText(src, "title_row");
  for (const m of tr.text.matchAll(/Action::(\w+)/g)) {
    const n = snake(m[1]);
    if (!(where[n] ?? []).some((w) => w.group === "Title row")) add(n, "Quick access", "Title row", lineOf(tr.text, tr.line, m.index));
  }
  return { where, tabs: ["File", "Quick access", ...[...src.matchAll(/^\s+Tab::(\w+),$/gm)].map((m) => m[1]).filter((t, i, a) => a.indexOf(t) === i)] };
}

/** settings.rs: every field of `Settings` with its type, default (as written) and doc. */
function rustSettings() {
  const src = read(join(RS, "settings.rs"));
  const theme = read(join(RS, "ui_theme.rs"));
  const st = /pub struct Settings \{([\s\S]*?)\n\}/.exec(src);
  const def = /impl Default for Settings \{[\s\S]*?Settings \{([\s\S]*?)\n\s*\}\n\s*\}/.exec(src);
  const defaults = {};
  for (const m of def[1].matchAll(/^\s*(\w+): (.+),$/gm)) defaults[m[1]] = m[2];
  const themes = [...theme.matchAll(/^\s*id: "([^"]+)",/gm)].map((m) => m[1]);
  const fonts = [...theme.matchAll(/UiFont::(\w+) => "([a-z][^"]*)",/g)].filter((m) => m[2] === m[2].toLowerCase()).map((m) => m[2]);
  const textSize = /pub const TEXT_SIZE: f32 = ([\d.]+);/.exec(theme)?.[1];
  const resolve = (e) => e === "crate::ui_theme::THEMES[0].id.to_owned()" ? `"${themes[0]}"`
    : e === "crate::ui_theme::UiFont::Roboto.id().to_owned()" ? '"roboto"'
    : e === "crate::ui_theme::TEXT_SIZE" ? textSize
    : e === "String::new()" ? '""' : e === "Vec::new()" ? "[]" : e.replace(/\.into\(\)$/, "");
  const fields = [];
  let doc = [];
  const base = lineOf(src, 1, st.index);
  st[1].split("\n").forEach((l, i) => {
    const d = /^\s*\/\/\/ ?(.*)$/.exec(l);
    if (d) return void doc.push(d[1]);
    const f = /^\s*pub (\w+): (.+),$/.exec(l);
    if (f) {
      fields.push({ field: f[1], type: f[2], default: resolve(defaults[f[1]] ?? "?"), source: `settings.rs:${base + i}`, doc: doc.join(" ") || null });
      doc = [];
    }
  });
  return { fields, themes, fonts };
}

/** The keyboard: Action::keys() and handle_keys / palette_key (lib.rs). */
function rustShortcuts() {
  const rib = read(join(RS, "ribbon.rs"));
  const lib = read(join(RS, "lib.rs"));
  const keysFn = fnText(rib, "keys");
  const byAction = {};
  for (const m of keysFn.text.matchAll(/Action::(\w+) => "([^"]+)"/g)) byAction[m[2]] = snake(m[1]);
  const hk = fnText(lib, "handle_keys");
  const rows = [];
  const seen = new Set();
  const push = (keys, how, idx, text, line0) => {
    if (seen.has(keys)) return;
    seen.add(keys);
    rows.push({ keys, action: byAction[keys] ?? null, effect: EFFECT[keys] ?? null, how, source: `lib.rs:${lineOf(text, line0, idx)}` });
  };
  const EFFECT = { // the keys handle_keys runs without a ribbon action (read from lib.rs; checked below)
    "Delete": "delete the selection (editable model)", "Escape": "stop placing, then measuring, then clear the selection",
    "P": "paper_view: On paper / model in a plan or section", "Ctrl Shift Z": "redo", "Ctrl K": "open the command palette",
  };
  const mod = (m) => m === "COMMAND" ? "Ctrl " : m === "SHIFT" ? "Shift " : m === "NONE" ? "" : m + " ";
  for (const m of hk.text.matchAll(/consume_key\(([\w:| ]+), egui::Key::(\w+)\)/g)) {
    let mods = m[1].trim();
    if (mods === "cmd") mods = "COMMAND";
    if (mods === "shift") mods = "COMMAND | SHIFT";
    const ks = mods.split("|").map((s) => mod(s.trim().replace("egui::Modifiers::", ""))).join("");
    push(ks + m[2], "consume_key", m.index, hk.text, hk.line);
  }
  for (const m of hk.text.matchAll(/key_pressed\(egui::Key::(\w+)\)/g)) push(m[1], "key_pressed", m.index, hk.text, hk.line);
  const pk = /fn palette_key[\s\S]*?consume_key\(egui::Modifiers::COMMAND, egui::Key::(\w+)\)/.exec(lib);
  if (pk) push("Ctrl " + pk[1], "palette_key", pk.index, lib, 1);
  for (const r of rows) if (!r.action && byAction[r.keys.replace("Escape", "Esc")]) r.action = byAction[r.keys];
  return rows;
}

/** The names the Rust dispatchers know: ops (calls.rs dispatch_op + the twin's view ops), queries. */
function rustNames() {
  const calls = read(join(RS, "calls.rs"));
  const twin = read(join(RS, "twin.rs"));
  const disp = read(join(ROOT, "crates/bimble-api/src/dispatch.rs"));
  const ops = [...fnText(calls, "dispatch_op").text.matchAll(/"(\w+)" => Op::/g)].map((m) => m[1]);
  const viewOps = [...fnText(twin, "view_op").text.matchAll(/^\s+"(\w+)" =>/gm)].map((m) => m[1]);
  const cq = fnText(calls, "call_query").text;
  const queries = [...cq.matchAll(/^\s+"(\w+)"(?: \| "\w+")* =>/gm)].map((m) => m[1]);
  const q = /pub fn query\([\s\S]*?\n\}/.exec(disp)[0];
  for (const m of q.matchAll(/"(\w+)"/g)) if (/^[a-z_]+$/.test(m[1]) && !queries.includes(m[1]) && !/known/.test(m[1])) queries.push(m[1]);
  return { ops, viewOps, queries: [...new Set(queries)] };
}

/** The names the C dispatchers know (session/calls.c and what it calls). */
const noComments = (t) => t.replace(/\/\*[\s\S]*?\*\//g, "").replace(/\/\/.*$/gm, "");
function cNames() {
  const c = noComments(read(join(CCORE, "session/calls.c")));
  const ops = /static const char \*const OPS\[\] = \{([\s\S]*?)\};/.exec(c)[1].match(/"(\w+)"/g).map((s) => s.slice(1, -1));
  const cq = /static int call_query\([\s\S]*?\n\}/.exec(c)[0];
  const queries = [];
  for (const m of cq.matchAll(/!strcmp\(name, "(\w+)"\)|"(\w+)"(?=[,}])/g)) {
    const n = m[1] || m[2];
    if (n && /^[a-z_]+$/.test(n) && !queries.includes(n)) queries.push(n);
  }
  const ver = read(join(CCORE, "session/versions.c")).match(/strcmp\(name, "(\w+)"\)/g).map((s) => /"(\w+)"/.exec(s)[1]);
  const qc = /static const char \*const NAMES\[\] = \{([\s\S]*?)\};/.exec(read(join(CCORE, "src/query.c")))[1].match(/"(\w+)"/g).map((s) => s.slice(1, -1));
  for (const n of [...ver, ...qc]) if (!queries.includes(n)) queries.push(n);
  const meas = /NAMES\[\] = \{([^}]*)\}/.exec(read(join(CCORE, "session/measure.c")))[1].match(/"(\w+)"/g).map((s) => s.slice(1, -1));
  const vis = /TOOL_NAMES\[\] = \{([^}]*)\}/.exec(read(join(CCORE, "session/visibility.c")))[1].match(/"(\w+)"/g).map((s) => s.slice(1, -1));
  const ct = /static int call_tool\([\s\S]*?\n\}/.exec(c)[0];
  const own = [...ct.matchAll(/!strcmp\(name, "(\w+)"\)/g)].map((m) => m[1]);
  return { ops, queries, tools: [...new Set([...meas, ...vis, ...own])] };
}

// ---------------------------------------------------------------- one side's dump

/** The JSON shape of a value: keys and value types, arrays as [shape of the first item]*count. */
function shape(v, depth = 0) {
  if (v === null) return "null";
  if (Array.isArray(v)) return v.length ? `[${depth > 3 ? "…" : shape(v[0], depth + 1)}]*${v.length}` : "[]";
  if (typeof v === "object") {
    if (depth > 3) return "{…}";
    return "{" + Object.keys(v).map((k) => `${k}:${shape(v[k], depth + 1)}`).join(",") + "}";
  }
  return typeof v === "number" ? "num" : typeof v === "string" ? "str" : typeof v === "boolean" ? "bool" : typeof v;
}

/** One answer as a line: `200 <shape>` or `400 "<text, cut>"`; malformed JSON is marked. */
function answerLine(r, json = true) {
  if (r.status === 200) {
    if (!json) return `200 ${r.body === "" ? '""' : JSON.stringify(clean(r.body).slice(0, 60))}`;
    const v = parse(r.body);
    return v === undefined ? `200 MALFORMED ${JSON.stringify(clean(r.body).slice(0, 80))}` : `200 ${shape(v)}`;
  }
  const t = clean(r.body).replace(/\s+/g, " ");
  return `${r.status} ${JSON.stringify(t.length > 100 ? t.slice(0, 100) + "…" : t)}`;
}

/** Typical arguments for a query, from the side's own answers (same fixture: same ids). */
async function queryArgs(names) {
  const els = parse((await call("query", "elements")).body);
  const sheets = parse((await call("query", "sheets")).body);
  const ov = parse((await call("query", "open_views")).body);
  const sched = parse((await call("query", "schedule_list")).body);
  const schedules = parse((await call("query", "schedules")).body);
  const el = Array.isArray(els) && els[0]?.id;
  const sheet = Array.isArray(sheets) && sheets[0]?.id;
  const view = ov?.views?.find(isPlan)?.id ?? ov?.views?.[0]?.id;
  const sname = (Array.isArray(sched) && sched[0]?.name) || (Array.isArray(schedules) && schedules[0]?.name) || "Columns";
  const A = {
    element: el ? { id: el } : { id: "0000000000000000000000" },
    describe: { name: "place" },
    palette_match: { q: "sec", items: ["Section box", "Fit all", "Section labels on plans"] },
    schedule_rows: { name: sname }, schedule_csv: { name: sname },
    sheet_svg: sheet ? { id: sheet } : { id: "0000000000000000000000" },
    sheet_layout: sheet ? { id: sheet } : { id: "0000000000000000000000" },
    view_annotations: view ? { view } : {}, view_references: view ? { view } : {},
  };
  return Object.fromEntries(names.map((n) => [n, A[n] ? JSON.stringify(A[n]) : ""]));
}

/** Runs every scenario on one side; returns {scenarios: {name: {actions, state, queries, steps}}, ops, tools, problems}. */
async function runSide(side, names) {
  rmSync(WORK, { recursive: true, force: true });
  mkdirSync(WORK, { recursive: true });
  const res = { scenarios: {}, ops: {}, tools: {}, fileActions: {}, problems: [] };
  for (const [sc, fix, steps] of SCENARIOS) {
    const t0 = performance.now();
    const model = fix ? workCopy(fix) : null;
    const viaOpen = side === "c" && fix === "ifc"; // the C host imports an .ifc given on its command line: open it read-only instead
    const log = [];
    const problem = (what) => res.problems.push(`${side} ${sc}: ${what}`);
    let h;
    try {
      h = await startHost(side, viaOpen ? null : model);
    } catch (e) {
      problem(String(e.message ?? e));
      continue;
    }
    try {
      if (viaOpen) {
        const b = JSON.stringify({ path: model });
        log.push([`action open ${clean(b)}`, await call("action", "open", b)]);
      }
      if (model) { // the twin loads in the background: wait for the file in `state`
        for (let i = 0; i < 600 && !h.dead(); i++) {
          const st = parse((await call("query", "state")).body);
          if (st?.file) break;
          await sleep(50);
        }
      }
      for (const s of steps) log.push(await s(log));
      if (h.dead()) throw new Error("gone"); // reported below (finally): the host exited
      const actions = parse((await call("query", "actions")).body) ?? [];
      const state = parse((await call("query", "state")).body) ?? null;
      const args = await queryArgs(names);
      const queries = {};
      for (const n of names) {
        const r = await call("query", n, args[n]);
        if (r.status === 0 || r.status >= 500) problem(`query ${n}: ${r.status} ${r.body.slice(0, 120)}`);
        else if (r.status === 200 && parse(r.body) === undefined) problem(`query ${n}: malformed JSON ${JSON.stringify(r.body.slice(0, 120))}`);
        queries[n] = { args: args[n], line: answerLine(r), body: r.status === 200 ? parse(r.body) : clean(r.body) };
      }
      res.scenarios[sc] = { actions, state, queries, steps: log.map(([l, r]) => `${clean(l)} -> ${answerLine(r, false)}`) };
      if (sc === "frame") { // probe ops and tools once, after the dump (the probes may change things)
        // The file actions without a body, as the lab's desktop buttons send them. Rust opens a file
        // dialog there (rfd), which a headless twin must not: asked on the C side only.
        if (side === "c") for (const a of ["open", "import_ifc", "link_model", "link_dxf", "link_pdf"]) res.fileActions[a] = answerLine(await call("action", a), false);
        for (const op of names.ops) res.ops[op] = answerLine(await call(op, "-", op === "tool" ? "{}" : ""), false);
        for (const t of names.tools) {
          const r = await call("tool", t, "{}");
          if (r.status === 0 || r.status >= 500) problem(`tool ${t}: ${r.status}`);
          else if (r.status === 200 && r.body !== "" && parse(r.body) === undefined) problem(`tool ${t}: malformed JSON`);
          res.tools[t] = answerLine(r, side === "c" && r.status === 200);
        }
      }
      for (const [l, r] of log) if (r.status === 0 || r.status >= 500) problem(`${l}: ${r.status} ${r.body.slice(0, 120)}`);
    } catch (e) {
      if (e.message !== "gone") problem(`the tool failed: ${e.stack ?? e}`);
    } finally {
      const died = h.dead();
      await stopHost(h);
      if (died) problem(`the host exited during the scenario (code ${h.code()}): ${h.log().trim().slice(-300)}`);
    }
    process.stderr.write(`parity ${side} ${sc}: ${(performance.now() - t0).toFixed(0)} ms\n`);
  }
  return res;
}

// ---------------------------------------------------------------- files

const pretty = (v) => JSON.stringify(v, null, 2) + "\n";
function writeSide(dir, res, names, rib = null) {
  rmSync(dir, { recursive: true, force: true });
  mkdirSync(dir, { recursive: true });
  for (const [sc, d] of Object.entries(res.scenarios)) {
    const s = join(dir, sc);
    mkdirSync(s, { recursive: true });
    // Rust's: with where the egui ribbon puts each action (ribbon.rs ribbon_panel, file_menu, title_row).
    const acts = rib ? d.actions.map((a) => ({ ...a, ribbon: (rib.where[a.name] ?? []).map((w) => `${w.tab} › ${w.group}`) })) : d.actions;
    writeFileSync(join(s, "actions.json"), "[\n" + acts.map((a) => "  " + JSON.stringify(a)).join(",\n") + "\n]\n"); // one action a line
    writeFileSync(join(s, "state.json"), pretty(d.state));
    // GlobalIds in the arguments are masked: a new sheet's id is random (the answers are shapes already).
    const mask = (t) => t.replace(/"[0-9A-Za-z_$]{22}"/g, '"<id>"');
    writeFileSync(join(s, "queries.txt"), names.queries.map((n) => `${n}${d.queries[n].args ? " " + mask(d.queries[n].args) : ""} -> ${d.queries[n].line}`).join("\n") + "\n");
    writeFileSync(join(s, "steps.txt"), (d.steps.length ? d.steps.join("\n") : "(none)") + "\n");
  }
  writeFileSync(join(dir, "ops.txt"), Object.entries(res.ops).map(([k, v]) => `${k} -> ${v}`).join("\n") + "\n");
  writeFileSync(join(dir, "tools.txt"), (Object.keys(res.tools).length ? Object.entries(res.tools).map(([k, v]) => `${k} -> ${v}`) : ["(no tool op)"]).join("\n") + "\n");
}

/** Reads a side's files back: {scenarios: {name: {actions, state, queries: {name: line}}}, ops, tools}. */
function readSide(dir) {
  const res = { scenarios: {}, ops: {}, tools: {} };
  const kv = (f) => Object.fromEntries(read(f).trim().split("\n").filter((l) => l.includes(" -> ")).map((l) => {
    const i = l.indexOf(" -> ");
    return [l.slice(0, i).split(" ")[0], l.slice(i + 4)];
  }));
  for (const [sc] of SCENARIOS) {
    const s = join(dir, sc);
    if (!existsSync(s)) continue;
    res.scenarios[sc] = { actions: JSON.parse(read(join(s, "actions.json"))), state: JSON.parse(read(join(s, "state.json"))),
      queries: kv(join(s, "queries.txt")), steps: read(join(s, "steps.txt")).trim().split("\n") };
  }
  res.ops = kv(join(dir, "ops.txt"));
  res.tools = existsSync(join(dir, "tools.txt")) ? kv(join(dir, "tools.txt")) : {};
  return res;
}

// ---------------------------------------------------------------- the Vue sources

function walk(d, out = []) {
  for (const e of readdirSync(d, { withFileTypes: true })) {
    const p = join(d, e.name);
    if (e.isDirectory()) walk(p, out);
    else if (/\.(vue|ts)$/.test(e.name) && !/^i18n\//.test(relative(UI, p))) out.push(p);
  }
  return out;
}

/** Where the Vue lab reaches actions, queries, settings, keys; its ribbon, pages, panels, dialogs. */
function vueScan(actionNames, queryNames, settings) {
  const files = walk(UI);
  const at = (f, i) => `${relative(ROOT, f)}:${i + 1}`;
  const actions = {}; // name -> [{at, how}]
  const queries = {};
  const unresolved = [];
  const addA = (n, where, how) => (actions[n] ??= []).push({ at: where, how });
  // The ribbon definition (shell/ribbon-tabs.ts) and its English labels.
  const ribF = join(UI, "shell/ribbon-tabs.ts");
  const ribLines = read(ribF).split("\n");
  const en = {};
  const i18n = read(join(UI, "i18n/parts/ribbon.ts"));
  const enBlock = i18n.slice(i18n.indexOf("export const en"));
  for (const m of enBlock.matchAll(/"(ribbon\.[\w.]+)": "([^"]*)"/g)) en[m[1]] = m[2];
  const ribbon = []; // {tab, group, groupLabel, key, label, action, page, panel, at}
  let tab = null, group = null;
  ribLines.forEach((l, i) => {
    const t = /^ {2}(\w+): \[/.exec(l);
    if (t) tab = t[1];
    const g = /\{ key: "(ribbon\.\w+\.g\d+)", tools: \[/.exec(l);
    if (g) group = g[1];
    const tool = /\{ key: "(ribbon\.[\w.]+)", tag: "\w+", icon: ".*", action: (null|"([a-z_0-9]+)")(?:, page: "([\w-]+)")?(?:, panel: "([\w-]+)")? \}/.exec(l);
    if (tool) {
      const row = { tab, group, groupLabel: en[group] ?? group, key: tool[1], label: en[tool[1]] ?? tool[1], action: tool[3] ?? null, page: tool[4] ?? null, panel: tool[5] ?? null, at: at(ribF, i) };
      ribbon.push(row);
      if (row.action) addA(row.action, row.at, row.page ? `ribbon ${tab} › ${row.groupLabel} › ${row.label} (opens page ${row.page})` : row.panel ? `ribbon ${tab} › ${row.groupLabel} › ${row.label} (opens panel ${row.panel})` : `ribbon ${tab} › ${row.groupLabel} › ${row.label}`);
    } else if (/\{ key: "ribbon\.[\w.]+\.t\d+"/.test(l)) unresolved.push(`${at(ribF, i)}: a ribbon tool line the parser did not read`);
  });
  const routes = {}; // screenRoutes in App.vue: action -> page/panel
  const app = read(join(UI, "App.vue"));
  for (const m of app.matchAll(/^\s+(\w+): (?:openPage\("([\w-]+)"\)|\(\) => \{ currentPanel\.value = "([\w-]+)"; \})/gm)) routes[m[1]] = m[2] ? `page ${m[2]}` : `panel ${m[3]}`;
  const keys = []; // key handlers: {key, ctrl, at, text}
  const table = []; // KEY1: the rows of web-ui/src/shell/keys.ts KEYS: {keys, action, effect, context, at}
  const shown = []; // shortcut texts shown (kbd) without being a handler
  const settingsHits = {};
  let palette = null; // the palette lists every action of query("actions")
  for (const f of files) {
    if (f === ribF) continue;
    const lines = read(f).split("\n");
    const keyTable = /\/shell\/keys\.ts$/.test(f);
    lines.forEach((l, i) => {
      // KEY1: a row of the key table (shell/keys.ts): the shortcut and what it runs.
      const kr = keyTable && /^\s*\{ keys: "([^"]+)", key: "[^"]+",.*? action: (null|"([a-z_0-9]+)")(?:, effect: "(\w+)")?, context: "(\w+)"/.exec(l);
      if (kr) {
        table.push({ keys: kr[1], action: kr[3] ?? null, effect: kr[4] ?? null, context: kr[5], at: at(f, i) });
        if (kr[3]) addA(kr[3], at(f, i), `shortcut ${kr[1]}`);
        return;
      }
      // Action names as string literals in an action context.
      // Not the transports (bimble.ts, ccore.ts): they pass names on, a control of the UI calls them.
      const transport = /\/(bimble|ccore)\.ts$/.test(f);
      if (!transport && (/action|runAction|\bact\(|\bon\(|enabled\(|pressed|ModifierPanel|name ===/.test(l) || /Modifier/.test(f))) {
        for (const m of l.matchAll(/["'`]([a-z][a-z0-9_]*)["'`]/g)) {
          if (actionNames.has(m[1]) && /(action|runAction|act|on|enabled|mod|tool)\b|\(["'`]/.test(l)) {
            const how = /runAction\(/.test(l) ? "runAction" : /action: "/.test(l) ? "menu entry" : /enabled\(|\bon\(/.test(l) ? "enable check" : "reference";
            addA(m[1], at(f, i), how);
          }
        }
        if (/runAction\((?!["'])/.test(l) && !/function runAction/.test(l)) unresolved.push(`${at(f, i)}: runAction with a computed name: ${l.trim().slice(0, 100)}`);
      }
      for (const m of l.matchAll(/(?:@click="|= )(?:error = )?(undo|redo)\(\)/g)) addA(m[1], at(f, i), "op " + m[1]);
      for (const m of l.matchAll(/enabled\(`([a-z_]+)\$\{/g)) addA(`${m[1]}*`, at(f, i), "enable check (template)");
      // Queries.
      for (const m of l.matchAll(/(?:\bquery|tryQuery)(?:<[^()]*?>)?\(\s*"([a-z_]+)"/g)) (queries[m[1]] ??= []).push(at(f, i));
      for (const m of l.matchAll(/call\("query", "([a-z_]+)"/g)) (queries[m[1]] ??= []).push(at(f, i));
      // Keys.
      for (const m of l.matchAll(/key(?:\.toLowerCase\(\))? *(?:===|!==) *"([^"]+)"/g)) keys.push({ key: m[1], ctrl: /ctrlKey|metaKey/.test(l), at: at(f, i), neg: /!==/.test(m[0]), code: l.trim().slice(0, 90) });
      if (/go: \(\) => runAction\(a\.name\)/.test(l)) palette = at(f, i);
      if (/by\[e\.key\]/.test(l)) unresolved.push(`${at(f, i)}: keys from a table (by[e.key])`);
      const kbd = /kbd: "([^"]+)", action: "([a-z_]+)"|kbd: "([^"]+)", dialog: "([\w-]+)"/.exec(l);
      if (kbd) shown.push({ text: kbd[1] ?? kbd[3], target: kbd[2] ? `action ${kbd[2]}` : `dialog ${kbd[4]}`, at: at(f, i) });
      for (const s of settings) if (new RegExp(`\\b${s.field}\\b`).test(l)) (settingsHits[s.field] ??= []).push(at(f, i));
    });
  }
  const list = (d) => readdirSync(join(UI, d)).filter((f) => f.endsWith(".vue")).map((f) => f.replace(/\.vue$/, "").replace(/([a-z0-9])([A-Z])/g, "$1-$2").toLowerCase());
  return { actions, queries, ribbon, routes, keys, table, shown, settingsHits, unresolved, palette,
    pages: list("pages").filter((p) => !p.endsWith("-side")), panels: list("panels"), dialogs: list("dialogs"), files: files.length };
}

// ---------------------------------------------------------------- comparison and report

const SCN = SCENARIOS.map((s) => s[0]);
/** A shape line with the keys of every object sorted: "200 {b:num,a:str}" and "200 {a:str,b:num}" compare equal. */
function normShape(line) {
  if (!line?.startsWith("200 ")) return line;
  const t = line.slice(4);
  let i = 0;
  const val = () => {
    if (t.startsWith("{…}", i)) return (i += 3), "{…}";
    if (t[i] === "{") {
      i++;
      const kv = [];
      while (t[i] !== "}" && i < t.length) {
        const c = t.indexOf(":", i);
        const k = t.slice(i, c);
        i = c + 1;
        kv.push(`${k}:${val()}`);
        if (t[i] === ",") i++;
      }
      i++;
      return "{" + kv.sort().join(",") + "}";
    }
    if (t[i] === "[") {
      if (t[i + 1] === "]") return (i += 2), "[]";
      i++;
      const inner = t.startsWith("…", i) ? (i++, "…") : val();
      i++; // ]
      const n = /^\*\d+/.exec(t.slice(i))?.[0] ?? "";
      i += n.length;
      return `[${inner}]${n}`;
    }
    const m = /^[^,}\]]+/.exec(t.slice(i))[0];
    i += m.length;
    return m;
  };
  try { return "200 " + val(); } catch { return line; }
}
function compare(R, C, rib, vue, known) {
  const rNames = R.scenarios.empty.actions.map((a) => a.name);
  const cNamesL = C.scenarios.empty?.actions.map((a) => a.name) ?? [];
  const actions = [];
  for (const n of [...new Set([...rNames, ...cNamesL])]) {
    const row = { name: n, rust: rNames.includes(n), c: cNamesL.includes(n), enabled: [], pressed: [], meta: [] };
    const r0 = R.scenarios.empty.actions.find((a) => a.name === n), c0 = C.scenarios.empty?.actions.find((a) => a.name === n);
    row.label = (r0 ?? c0).label;
    row.tab = r0?.tab ?? c0?.tab;
    for (const k of ["label", "tab", "shortcut"]) if (r0 && c0 && r0[k] !== c0[k]) row.meta.push(`${k}: Rust ${JSON.stringify(r0[k])}, C ${JSON.stringify(c0[k])}`);
    for (const sc of SCN) {
      const ra = R.scenarios[sc]?.actions.find((a) => a.name === n), ca = C.scenarios[sc]?.actions.find((a) => a.name === n);
      if (!ra || !ca) continue;
      if (ra.enabled !== ca.enabled) row.enabled.push({ sc, r: ra.enabled, c: ca.enabled });
      if (ra.pressed !== ca.pressed) row.pressed.push({ sc, r: ra.pressed, c: ca.pressed });
    }
    row.cEver = SCN.some((sc) => C.scenarios[sc]?.actions.find((a) => a.name === n)?.enabled);
    row.rEver = SCN.some((sc) => R.scenarios[sc]?.actions.find((a) => a.name === n)?.enabled);
    row.status = !row.c ? "missing in C" : !row.rust ? "C-only" : row.enabled.length ? "enabled differs" : row.pressed.length ? "pressed differs" : "same";
    row.where = rib.where[n] ?? [];
    row.vue = vue.actions[n] ?? [];
    if (vue.routes[n]) row.vueRoute = vue.routes[n];
    actions.push(row);
  }
  const qNames = [...new Set([...Object.keys(R.scenarios.empty.queries), ...Object.keys(C.scenarios.empty?.queries ?? {})])];
  const queries = [];
  for (const n of qNames) {
    const row = { name: n, diffs: [] };
    const rl = SCN.map((sc) => R.scenarios[sc]?.queries[n]);
    // ISS72: `actions` compared on Rust's rows; the C-only rows are the actions table's "C-only".
    const cl = SCN.map((sc) => n === "actions" && C.scenarios[sc]?.queries[n]?.startsWith("200 ")
      ? answerLine({ status: 200, body: JSON.stringify(C.scenarios[sc].actions.filter((a) => rNames.includes(a.name))) }) : C.scenarios[sc]?.queries[n]);
    // TEST1: C knows a query when the grep of its dispatchers names it OR when the C host answered it in
    // some scenario with anything but "unknown query" (asked, not grepped: Q2's schedule_* are
    // dispatched from session/schedules.c, which the grep of calls.c does not see). Rust's names stay
    // its dispatcher's (golden/parity/names.json): its fallthrough answers a name it does not know
    // with "this file is read only" when no model is open, so an answer there proves nothing.
    row.rust = known.rust.includes(n);
    row.c = known.c.includes(n) || cl.some((l) => typeof l === "string" && !/unknown query/.test(l));
    row.keyOrder = [];
    SCN.forEach((sc, i) => {
      if (rl[i] === cl[i]) return;
      if (rl[i] && cl[i] && normShape(rl[i]) === normShape(cl[i])) return void row.keyOrder.push(sc);
      row.diffs.push({ sc, r: rl[i] ?? "(not asked)", c: cl[i] ?? "(not asked)" });
    });
    // A stub: "not in the C core", or one constant C answer where Rust answers 200 with something else.
    const cSet = new Set(cl.filter(Boolean)), cConst = cSet.size === 1 ? [...cSet][0] : null;
    const cut = cl.some((l) => /is not in the C core/.test(l ?? ""));
    const stub = cut || (cConst !== null && rl.some((l) => l?.startsWith("200 ") && l !== cConst));
    row.status = !row.c && row.rust ? "missing in C" : !row.rust && row.c ? "C-only" : !row.rust && !row.c ? "neither" : !row.diffs.length ? "same shape"
      : stub ? "stub" : "differs";
    row.vue = vue.queries[n] ?? [];
    queries.push(row);
  }
  return { actions, queries };
}

const RANK = { "missing in C": 0, "enabled differs": 1, "pressed differs": 2, stub: 1, differs: 2, "C-only": 4, "same": 5, "same shape": 5, neither: 6 };
const esc = (s) => String(s).replace(/\|/g, "\\|").replace(/\n/g, " ");
const ev = (list, n = 4) => list.slice(0, n).map((d) => `${d.sc}: R ${d.r} / C ${d.c}`).join("; ") + (list.length > n ? `; +${list.length - n} more` : "");
const vueWhere = (row) => {
  const strong = row.vue.filter((v) => !/enable check|reference/.test(v.how));
  const parts = (strong.length ? strong : row.vue).filter((v, i, a) => a.findIndex((w) => w.at === v.at) === i).slice(0, 3).map((v) => `${v.at} (${v.how})`);
  if (row.vueRoute) parts.push(`runAction routes to ${row.vueRoute} (App.vue)`);
  return parts.length ? parts.join("<br>") : "**no place in the new UI**";
};

function report(R, C, rib, settings, shortcuts, vue, cmp, meta) {
  const L = [];
  const head = git("rev-parse", "--short", "HEAD");
  const cnt = (rows, st) => rows.filter((r) => r.status === st).length;
  const A = cmp.actions, Q = cmp.queries;
  L.push("# Runtime parity: Rust egui app vs C core + Vue lab (ticket PAR2)", "");
  L.push("Generated by `node ccore/tools/parity.mjs --write`. Do not edit by hand.", "");
  // FIXDOC: what the counts below support, and what they do not (an audit read more into them).
  L.push("**What this report supports.** The C core serves the same action names as the Rust app; for each action, its `enabled` and `pressed` flags are equal in the scenarios listed below (synthetic fixtures, mainly `golden/frame.json`, 72 elements); a query's answer has the same JSON key names and value types (its shape); the Rust shortcuts have a row in the lab's key table.", "");
  L.push(`**What it does not support.** That an action does the same thing on both sides (only its flags are compared); that a query returns the same values (only its shape is compared); that a shortcut reaches the page (in a browser tab the browser keeps Ctrl N); parity with the v0.19.0 release: the committed Rust answers were dumped from a development build that reports \`${meta.rust_version}\` (\`ccore/golden/parity/meta.json\`).`, "");
  L.push(`- C core and Vue lab measured at commit \`${head}\` (the working tree at that commit). Run the tool again after a change of the actions, queries or the lab.`);
  L.push(`- Rust reference: \`${meta.rust_version}\` (a development build before v0.19.0, not the release binary), goldens \`ccore/golden/parity/\` made by \`parity.mjs --rust\` at commit \`${meta.commit}\` (the headless native twin, ADR 0066: the real \`Viewer::call\`, no window).`);
  L.push(`- Scenarios (${SCN.length}): ${SCN.map((s) => `\`${s}\``).join(", ")}. How each one is reached, per side: \`steps.txt\` (Rust: \`golden/parity/<scenario>/\`, C: \`out/parity/c/<scenario>/\`).`);
  L.push(`- Vue sources parsed: ${vue.files} files under \`web-ui/src\` (grep-level; see "Not resolved" at the end).`);
  L.push("- A query counts as known in C when the C host answered it in some scenario (anything but \"unknown query\"), or its dispatchers name it (ticket TEST1: asked, not grepped).");
  L.push("- Gap (TEST1): no scenario hides anything, so `unhide`, `clear_temporary` and `show_all_in_view` are never enabled on either side and their enabled state is not compared. A scenario that hides an element (Rust goldens: on the branch `oracle/rust`, `parity.mjs --rust`) is still to add.", "");
  L.push("## C crashes and malformed answers", "");
  L.push(C.problems.length ? C.problems.map((p) => `- **${esc(p)}**`).join("\n") : "None: the C host stayed up in every scenario, and every query and tool answer that said 200 was valid JSON.", "");
  L.push("## Summary", "");
  L.push("| What | Count |", "|---|---|");
  L.push(`| Actions (Rust ${A.filter((a) => a.rust).length}, C ${A.filter((a) => a.c).length}) | same ${cnt(A, "same")} · enabled state differs ${cnt(A, "enabled differs")} · pressed differs (enabled same) ${cnt(A, "pressed differs")} · missing in C ${cnt(A, "missing in C")} · C-only ${cnt(A, "C-only")} |`);
  L.push(`| Actions the C core never enables (any scenario) but Rust does | ${A.filter((a) => a.c && a.rust && a.rEver && !a.cEver).length} |`);
  L.push(`| Queries (Rust ${Q.filter((q) => q.rust).length}, C ${Q.filter((q) => q.c).length}) | same shape ${cnt(Q, "same shape")} · differs ${cnt(Q, "differs")} · stub ${cnt(Q, "stub")} · missing in C ${cnt(Q, "missing in C")} · C-only ${cnt(Q, "C-only")} |`);
  const opsR = Object.keys(R.ops).filter((o) => !/unknown op/.test(R.ops[o])), opsC = Object.keys(C.ops).filter((o) => !/unknown op/.test(C.ops[o]));
  L.push(`| Ops (Rust ${opsR.length}, C ${opsC.length}) | both ${opsR.filter((o) => opsC.includes(o)).length} · Rust-only ${opsR.filter((o) => !opsC.includes(o)).join(", ") || "none"} · C-only ${opsC.filter((o) => !opsR.includes(o)).join(", ") || "none"} |`);
  const setReach = settings.fields.filter((s) => vue.settingsHits[s.field]?.length).length;
  const setDialog = existsSync(join(UI, "dialogs/Settings.vue")) ? new Set([...read(join(UI, "dialogs/Settings.vue")).matchAll(/data-setting="(\w+)"/g)].map((m) => m[1])).size : 0; // SET1
  L.push(`| Settings (Rust ${settings.fields.length} fields, ${settings.themes.length} themes) | named somewhere in web-ui/src ${setReach} · not named ${settings.fields.length - setReach} · in the lab's Settings dialog ${setDialog} · C \`settings\` action enabled in ${A.find((a) => a.name === "settings")?.cEver ? "some" : "no"} scenario |`);
  const keyRows = shortcutRows(shortcuts, vue);
  const labOnlyKeys = (vue.table ?? []).filter((t) => !shortcuts.some((s) => s.keys === t.keys));
  L.push(`| Shortcuts (Rust ${shortcuts.length}) | in the lab's key table ${keyRows.filter((k) => k.handled).length} · of them inert in a browser tab ${keyRows.filter((k) => k.handled && inertKey(k, A)).length} (${keyRows.filter((k) => k.handled && inertKey(k, A)).map((k) => k.keys).join(", ") || "none"}) · shown only ${keyRows.filter((k) => !k.handled && k.shown).length} · absent ${keyRows.filter((k) => !k.handled && !k.shown).length} · runs another action than Rust's ${keyRows.filter((k) => k.differs).length} · lab-only ${labOnlyKeys.length} |`);
  const reach = A.filter((a) => a.rust);
  const noPlace = reach.filter((a) => !a.vue.length && !a.vueRoute);
  L.push(`| Rust actions with a place in the Vue UI besides the palette | ${reach.length - noPlace.length} of ${reach.length} (no place: ${noPlace.map((a) => `\`${a.name}\``).join(", ") || "none"}). The palette (${vue.palette ?? "not found"}) lists every action of \`actions\`. |`);
  const fake = fakeUi(A, Q, vue);
  L.push(`| Fake UI (Vue controls whose action C never enables, or whose query is a stub / missing) | ${fake.length} |`);
  const nullTools = vue.ribbon.filter((t) => !t.action && !t.page && !t.panel);
  L.push(`| Vue ribbon tools with no action and no page (drawn disabled, "no Rust call yet") | ${nullTools.length} of ${vue.ribbon.length} |`, "");

  L.push("## Top differences", "");
  const top = [];
  for (const a of A.filter((a) => a.rust && a.c && a.rEver && !a.cEver)) top.push(`- action \`${a.name}\` (${a.tab}): Rust enables it (${ev(a.enabled, 2)}), the C core never does.`);
  for (const a of A.filter((a) => a.status === "enabled differs" && a.cEver)) top.push(`- action \`${a.name}\`: enabled differs in ${a.enabled.length} scenario(s): ${ev(a.enabled, 3)}.`);
  for (const a of A.filter((a) => a.status === "pressed differs")) top.push(`- action \`${a.name}\`: pressed differs: ${ev(a.pressed, 3)}.`);
  for (const q of Q.filter((q) => q.status === "stub" || q.status === "missing in C")) top.push(`- query \`${q.name}\`: ${q.status} (${esc(ev(q.diffs, 1))}).`);
  L.push(top.length ? top.join("\n") : "None.", "");

  L.push("## Actions by Rust ribbon tab", "");
  L.push("Rust's order: the File menu, quick access, then the ribbon tabs. Differences first. Evidence: `scenario: R <Rust> / C <C>`. Vue: where the lab reaches it.", "");
  const tabOf = (a) => a.where[0]?.tab ?? (a.tab === "File" ? "File" : a.tab === "Quick access" ? "Quick access" : `${a.tab} (palette only)`);
  const order = [...rib.tabs, ...new Set(A.map(tabOf))].filter((t, i, a) => a.indexOf(t) === i);
  for (const t of order) {
    const rows = A.filter((a) => a.rust && tabOf(a) === t).sort((a, b) => RANK[a.status] - RANK[b.status]);
    if (!rows.length) continue;
    L.push(`### ${t}`, "", "| Action | Group | Status | Evidence | Vue |", "|---|---|---|---|---|");
    for (const a of rows) {
      const groups = [...new Set(a.where.map((w) => w.group))].join(", ") || "palette only";
      const evid = [a.enabled.length ? `enabled: ${ev(a.enabled)}` : "", a.pressed.length ? `pressed: ${ev(a.pressed)}` : "", ...a.meta,
        !a.c ? "not in C's `actions`" : "", a.c && a.rEver && !a.cEver ? "**C never enables it**" : ""].filter(Boolean).join("<br>");
      L.push(`| \`${a.name}\` ${esc(a.label)} | ${esc(groups)} | ${a.status} | ${esc(evid) || "—"} | ${vueWhere(a)} |`);
    }
    L.push("");
  }
  const cOnlyA = A.filter((a) => !a.rust);
  if (cOnlyA.length) L.push("C-only actions (listed by C's `actions`, not by Rust's): " + cOnlyA.map((a) => `\`${a.name}\``).join(", "), "");

  L.push("## Queries", "");
  L.push("Every query name either dispatcher knows, asked in every scenario with no or typical arguments. `stub`: the C answer is the same constant in every scenario (or \"not in the C core\") where Rust's is not. Shapes: `{key:type}`, arrays `[first item]*count`.", "");
  L.push("Key order is not a difference here (JSON objects); where only the order differs, the last column says so: Rust writes a struct's keys in field order, the C core sorts them.", "");
  L.push("| Query | Status | Scenarios that differ | First difference (R / C) | Vue | Key order |", "|---|---|---|---|---|---|");
  for (const q of [...Q].sort((a, b) => RANK[a.status] - RANK[b.status] || a.name.localeCompare(b.name))) {
    const d = q.diffs[0];
    const sc = q.diffs.length === SCN.length ? "all" : q.diffs.length ? q.diffs.map((x) => x.sc).join(", ") : "—";
    L.push(`| \`${q.name}\` | ${q.status} | ${sc} | ${d ? `${d.sc}: R \`${esc(d.r.slice(0, 140))}\`<br>C \`${esc(d.c.slice(0, 140))}\`` : "—"} | ${q.vue.slice(0, 2).join("<br>") || "not asked by the lab"} | ${q.keyOrder.length ? `differs (${q.keyOrder.length} scenarios)` : "—"} |`);
  }
  L.push("");
  L.push("Limits: `pointer` is null on both sides here (the headless twin has no hover). `recent_files` on Rust lists the file the scenario opened (the twin reads no user settings).", "");
  L.push("## Ops and tools", "", "| Op | Rust (probe `/<op>/-`, empty body) | C |", "|---|---|---|");
  for (const o of [...new Set([...Object.keys(R.ops), ...Object.keys(C.ops)])]) L.push(`| \`${o}\` | ${esc(R.ops[o] ?? "(not probed)")} | ${esc(C.ops[o] ?? "(not probed)")} |`);
  L.push("", "`orbit`, `pan`, `zoom` (and `GET /frame`) are the Rust twin's view ops (twin.rs `view_op`), not `Viewer::call` ops: the lab moves its own WebGL camera on the C core.");
  L.push("", `Tools (op \`tool\`, a C addition): ${Object.entries(C.tools).map(([k, v]) => `\`${k}\` ${esc(v.split(" ")[0])}`).join(", ")}. Rust has no \`tool\` op: placing, measuring and the section box are driven by egui pointer events there.`, "");

  L.push("File actions with no body (scenario `frame`), as a desktop button sends them. Rust shows a file dialog (not asked: the headless twin has no window); the C core answers:", "");
  for (const [a, v] of Object.entries(C.fileActions ?? {})) L.push(`- \`${a}\`: ${esc(v)}`);
  L.push("");
  // ---- 3D1 (ADR 0078): the 3D speed features of the Rust app, ported or not (THREED_SPEED below) ----
  L.push("## 3D speed features of the Rust app", "");
  L.push("What Rust v0.19.0's renderer did to draw large models fast (ADR 0034-0036) and whether the C lab's viewport does it (ticket 3D1, ADR 0078; numbers in `ccore/results/frames.md`). The list is `THREED_SPEED` in this tool.", "");
  L.push("| Feature | Rust | C lab | Where / why not |", "|---|---|---|---|");
  for (const f of THREED_SPEED) L.push(`| ${f.feature} | \`${f.rust}\` | ${f.ported ? "ported" : "**not ported**"} | ${esc(f.note)} |`);
  L.push("");
  L.push("## Settings (Rust `settings.rs`)", "");
  // SET1: the lab's Settings dialog (its `data-setting` fields) and the C core's `query settings`
  // (a field it lists under `needs` does not apply in this build).
  const cSet = C.scenarios.empty?.queries.settings ?? "";
  const cServes = cSet.startsWith("200 ");
  const needs = new Set((/needs:\{([^}]*)\}/.exec(cSet)?.[1] ?? "").split(",").map((x) => x.split(":")[0]).filter(Boolean));
  const dlgF = join(UI, "dialogs/Settings.vue");
  const dialog = existsSync(dlgF) ? new Set([...read(dlgF).matchAll(/data-setting="(\w+)"/g)].map((m) => m[1])) : new Set();
  const cOn = A.find((a) => a.name === "settings")?.cEver;
  L.push(`Rust opens them with the \`settings\` action (File menu, Manage › Settings, Home's "change"). ${cOn ? "The C core enables `settings` (ribbon.rs:591); the lab opens its Settings dialog (web-ui/src/dialogs/Settings.vue)." : "The C core never enables `settings`, so no Settings dialog opens in the lab."} C \`query settings\`: ${cServes ? "answers (ccore/session/settings.h; the same file as Rust)" : "**not served**"}. Themes: ${settings.themes.map((t) => `\`${t}\``).join(", ")}.`, "");
  L.push("| Field | Type | Default | C core (`query settings`) | Lab's Settings dialog | Vue: where the word appears (grep; may be another meaning) |", "|---|---|---|---|---|---|");
  for (const s of settings.fields) {
    const c = !cServes ? "**not served**" : needs.has(s.field) ? "stored, does not apply here" : "stored, applies";
    const d = dialog.has(s.field) ? "shown" : s.field === "recent" ? "Home's list (FS1)" : needs.has(s.field) ? "kept, not shown" : "**not shown**";
    L.push(`| \`${s.field}\` | \`${esc(s.type)}\` | \`${esc(s.default)}\` | ${c} | ${d} | ${(vue.settingsHits[s.field] ?? []).slice(0, 2).join("<br>") || "**no place in the new UI**"} |`);
  }
  L.push("");
  L.push("## Keyboard shortcuts (Rust `lib.rs` handle_keys, palette_key; `Action::keys`)", "");
  L.push(`The lab's key table is \`web-ui/src/shell/keys.ts\` (ticket KEY1): one row per shortcut, one listener; the ribbon tooltips, the File menu and the palette rows show the label of the same row. A row runs only when its action is enabled (\`actions\`): an action the C core never enables leaves its key inert (now: ${keyRows.filter((k) => k.action && A.find((a) => a.name === k.action)?.c && !A.find((a) => a.name === k.action)?.cEver).map((k) => `${k.keys} \`${k.action}\``).join(", ") || "none"}). A key the browser keeps never reaches the page: Ctrl N (a new window) in Chromium, Firefox and Safari; the row exists for a window the lab owns.`, "");
  L.push("| Keys | Rust action / effect | Rust source | Lab handler | Shown in the lab |", "|---|---|---|---|---|");
  for (const k of keyRows) L.push(`| ${k.keys} | ${k.action ? `\`${k.action}\`` : ""}${k.effect ? ` ${esc(k.effect)}` : ""} | ${k.source} | ${k.handled ? k.handled : "**none**"}${k.differs ? ` **runs another action: ${esc(k.differs)}**` : ""} | ${k.shown || "—"} |`);
  L.push("", `Lab-only rows (not in Rust): ${labOnlyKeys.map((t) => `${t.keys} (${t.at})`).join(", ") || "none"}.`, "");

  L.push("## Fake UI", "", "Vue controls that look live but cannot work on the C core today: the action is never enabled by the C core in any scenario, or the query is a stub or missing.", "");
  L.push(fake.length ? fake.map((f) => `- ${f}`).join("\n") : "None.", "");
  const idle = A.filter((a) => a.rust && a.c && !a.rEver && !a.cEver);
  L.push(`Not exercised (neither side enables it in any scenario; a later scenario should): ${idle.map((a) => `\`${a.name}\``).join(", ") || "none"}.`, "");
  L.push("Ribbon tools with no action and no page (drawn disabled on purpose, `action: null`): " + nullTools.map((t) => `${t.tab} › ${esc(t.label)}`).join(", ") + ".", "");

  L.push("## Lab-only (the Vue UI has it, Rust does not)", "");
  const rustActions = new Set(A.filter((a) => a.rust).map((a) => a.name));
  const vueOnlyActions = Object.keys(vue.actions).filter((n) => !rustActions.has(n) && !n.endsWith("*"));
  L.push(`- Action ids the lab calls that Rust does not list: ${vueOnlyActions.map((n) => `\`${n}\` (${vue.actions[n][0].at})`).join(", ") || "none"}.`);
  const pageAction = new Map(vue.ribbon.filter((t) => t.page || t.panel).map((t) => [t.page ?? t.panel, t.action]));
  for (const [a, r] of Object.entries(vue.routes)) pageAction.set(r.split(" ")[1], a);
  const labPages = vue.pages.filter((p) => !pageAction.get(p));
  L.push(`- Pages that no Rust action opens (some show Rust data, e.g. \`sheets\`): ${labPages.map((p) => `\`${p}\``).join(", ") || "none"} (of ${vue.pages.length} pages).`);
  L.push(`- Pages a Rust action opens: ${vue.pages.filter((p) => pageAction.get(p)).map((p) => `\`${p}\` ← \`${pageAction.get(p)}\``).join(", ")}.`);
  L.push(`- Panels: ${vue.panels.map((p) => `\`${p}\`${pageAction.get(p) ? ` ← \`${pageAction.get(p)}\`` : " (no Rust action)"}`).join(", ")}. Dialogs: ${vue.dialogs.map((d) => `\`${d}\``).join(", ")}.`);
  L.push(`- C-only queries: ${Q.filter((q) => q.status === "C-only").map((q) => `\`${q.name}\``).join(", ") || "none"}. C-only ops: ${opsC.filter((o) => !opsR.includes(o)).map((o) => `\`${o}\``).join(", ") || "none"}.`);
  const tabsR = rib.tabs, tabsV = [...new Set(vue.ribbon.map((t) => t.tab))];
  L.push(`- Ribbon tabs: Rust ${tabsR.slice(2).join(", ")}; Vue ${tabsV.join(", ")}.`);
  L.push(`- The FR/EN language switch (web-ui/src/i18n): Rust's labels are English only.`, "");
  L.push("## Not resolved by the parser", "");
  L.push("Calls with a computed action name. Where the names are literals in the same file (the File menu entries, Home's `act('…')`, the ribbon data, ModifierPanel's and SmallSheet's buttons) they are counted above; a `?action=` deep link (deeplink.ts) and the palette can run any name.", "");
  L.push(vue.unresolved.length ? vue.unresolved.map((u) => `- ${esc(u)}`).join("\n") : "Nothing.", "");
  return L.join("\n");
}

// FIXDOC: a key row that cannot run in a browser tab: the browser keeps the key (Ctrl N opens a
// window: no page receives it), or the C core never enables the row's action.
const BROWSER_KEEPS = new Set(["Ctrl N"]);
function inertKey(k, A) {
  if (BROWSER_KEEPS.has(k.keys)) return true;
  const a = k.action && A.find((x) => x.name === k.action);
  return !!(a && a.c && !a.cEver);
}

function shortcutRows(shortcuts, vue) {
  const norm = (k) => k.toLowerCase();
  return shortcuts.map((s) => {
    const parts = s.keys.split(" ");
    const key = parts.at(-1), ctrl = parts.includes("Ctrl");
    const h = vue.keys.filter((k) => !k.neg && norm(k.key) === norm(key === "Escape" ? "Escape" : key) && k.ctrl === ctrl);
    const shown = vue.shown.filter((x) => norm(x.text.replace("+", " ")) === norm(s.keys));
    // KEY1: the key table's row of these keys (exact spelling): handled; another action than Rust's: differs.
    const t = (vue.table ?? []).find((r) => r.keys === s.keys);
    const want = s.action ?? (s.keys === "Ctrl Shift Z" ? "redo" : null);
    const differs = t && want && (t.action ?? t.effect) !== want ? `${t.action ?? t.effect} for ${want}` : null;
    const handled = t ? [`${t.at} \`${t.action ?? t.effect}\` (${t.context})`] : h.slice(0, 2).map((x) => `${x.at} \`${esc(x.code)}\``);
    if (t?.action || t?.effect === "palette") shown.push({ text: s.keys, at: t.action ? "the key table's label (ribbon tooltip, File menu, palette)" : "the key table's label (the command field)" });
    return { ...s, handled: handled.join("<br>"), differs, shown: shown.map((x) => `${x.text} (${x.at})`).join("<br>") };
  });
}

function fakeUi(A, Q, vue) {
  const out = [];
  for (const a of A) {
    if (!a.c || a.cEver || !a.rEver) continue;
    const places = (vue.actions[a.name] ?? []).filter((v) => !/\(opens (page|panel)/.test(v.how));
    if (places.length) out.push(`action \`${a.name}\` (never enabled by C): ${places.slice(0, 3).map((p) => `${p.at} (${p.how})`).join(", ")}`);
  }
  for (const q of Q) if ((q.status === "stub" || q.status === "missing in C") && q.vue.length) out.push(`query \`${q.name}\` (${q.status} in C): ${q.vue.slice(0, 3).join(", ")}`);
  return out;
}

function git(...a) {
  try { return execFileSync("git", a, { cwd: ROOT, encoding: "utf8" }).trim(); } catch { return "?"; }
}

// ---------------------------------------------------------------- main

// Without the Rust tree (main, ADR 0072): --rust stops with where it lives; the other modes take the
// Rust names from the committed golden/parity/names.json (written by --rust from the same sources).
if (!existsSync(RS)) {
  if (MODE === "rust") {
    console.error("parity --rust: no Rust sources here (crates/ is not on main, ADR 0072). Run it in a worktree of the branch oracle/rust (git worktree add ../bimble-rust oracle/rust), then copy ccore/golden/parity/ back.");
    process.exit(2);
  }
  if (!existsSync(join(GOLD, "names.json"))) {
    console.error("parity: no golden/parity/names.json and no Rust sources: run parity --rust on the branch oracle/rust");
    process.exit(2);
  }
}
const rnG = existsSync(RS) ? null : JSON.parse(read(join(GOLD, "names.json")));
const rn = rnG ? { ops: rnG.ops, viewOps: rnG.view_ops, queries: rnG.queries } : rustNames(), cn = cNames();
const names = { queries: [...new Set([...rn.queries, ...cn.queries])], ops: [...new Set([...rn.ops, ...cn.ops, ...rn.viewOps.filter((o) => o !== "frame")])], tools: cn.tools };
const qNames = Object.assign([...names.queries], { ops: names.ops, tools: names.tools });

if (MODE === "rust") {
  const res = await runSide("rust", qNames);
  const rib = rustRibbon();
  writeSide(GOLD, res, names, rib);
  writeFileSync(join(GOLD, "ribbon.json"), pretty(rib));
  const st = rustSettings();
  writeFileSync(join(GOLD, "settings.json"), pretty(st));
  writeFileSync(join(GOLD, "shortcuts.json"), pretty(rustShortcuts()));
  const rv = parse(read(join(GOLD, "empty/state.json")))?.version;
  writeFileSync(join(GOLD, "names.json"), pretty({ ops: rn.ops, view_ops: rn.viewOps, queries: rn.queries }));
  writeFileSync(join(GOLD, "meta.json"), pretty({ commit: git("rev-parse", "--short", "HEAD"), rust_version: `bimble ${rv}`, scenarios: SCN }));
  if (res.problems.length) console.log("Rust problems:\n" + res.problems.join("\n"));
  console.log(`parity --rust: ${Object.keys(res.scenarios).length} scenarios written to ${rel(GOLD)}`);
  process.exit(0);
}

if (!existsSync(join(GOLD, "meta.json"))) {
  console.error("parity: no Rust goldens (ccore/golden/parity/): run node ccore/tools/parity.mjs --rust first");
  process.exit(2);
}
const C = await runSide("c", qNames);
writeSide(join(OUT, "c"), C, names);
const R = readSide(GOLD);
const Cr = readSide(join(OUT, "c"));
Cr.problems = C.problems;
Cr.fileActions = C.fileActions;
const rib = JSON.parse(read(join(GOLD, "ribbon.json")));
const settings = JSON.parse(read(join(GOLD, "settings.json")));
const shortcuts = JSON.parse(read(join(GOLD, "shortcuts.json")));
const meta = JSON.parse(read(join(GOLD, "meta.json")));
const allActions = new Set([...R.scenarios.empty.actions.map((a) => a.name), ...(Cr.scenarios.empty?.actions ?? []).map((a) => a.name), "new_view", "save_as", "close"]);
const vue = vueScan(allActions, names.queries, settings.fields);
const rNamesG = JSON.parse(read(join(GOLD, "names.json")));
const cmp = compare(R, Cr, rib, vue, { rust: rNamesG.queries, c: cn.queries });
const A = cmp.actions, Q = cmp.queries;
const c = (rows, st) => rows.filter((r) => r.status === st).length;
console.log(`actions: same ${c(A, "same")}, enabled differs ${c(A, "enabled differs")}, pressed differs ${c(A, "pressed differs")}, missing in C ${c(A, "missing in C")}, C-only ${c(A, "C-only")}`);
console.log(`queries: same shape ${c(Q, "same shape")}, differs ${c(Q, "differs")}, stub ${c(Q, "stub")}, missing in C ${c(Q, "missing in C")}, C-only ${c(Q, "C-only")}`);
console.log(`fake UI: ${fakeUi(A, Q, vue).length}; C problems (crash, 5xx, malformed JSON): ${C.problems.length}`);
for (const p of C.problems) console.log(`C PROBLEM: ${p}`);
if (MODE === "write") {
  writeFileSync(REPORT, report(R, Cr, rib, settings, shortcuts, vue, cmp, meta));
  console.log(`wrote ${rel(REPORT)}`);
}
process.exit(MODE === "check" && C.problems.length ? 1 : 0);
