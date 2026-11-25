// The viewer-only wasm (ticket W1, ADR 0069 rule 3): node ccore/test/wasm_view_run.mjs
// Instantiates out/wasm/ccore.wasm (build.sh wasm, the editor) and out/wasm/ccore-view.wasm
// (build.sh wasm-view) side by side and makes the same calls on both:
//   query    the handle API's model queries (golden/query.txt, golden/frame2/query.txt) and the
//            session's read queries on every golden file: byte for byte, status included
//   mesh     mesh (local and world) of frame and frame2, host_mesh (world and local) of every file
//   session  every golden file opened through host_open (.json editable; .ifc and .bimbleview
//            read-only): select, query element, the visibility actions, the section box tool, plan
//            tabs and their clip, change_json: byte for byte
//   refused  the view build answers "this build is view-only" (400) to verb, apply, undo, redo,
//            set_placing, the place tool, the edit and file-writing actions, and the handle API's
//            open_ifc, apply, verb, undo, redo, save, export_ifc; the editor build does not
//   time     a synthetic 3072-element model (made with the editor's verbs) as .json and IFC4: opened
//            and meshed on both, byte for byte, and timed (the view build is -Os, the editor -O2)
// Prints per-suite "N passed, M failed", both sizes; exits non-zero on a failure. No dependencies.
import { readFileSync, readdirSync } from "node:fs";
import { gzipSync } from "node:zlib";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const G = join(CCORE, "golden");
const FULL = process.env.CCORE_WASM ?? join(CCORE, "out/wasm/ccore.wasm");
const VIEW = process.env.CCORE_VIEW_WASM ?? join(CCORE, "out/wasm/ccore-view.wasm");
const VIEW_ONLY = "this build is view-only"; // session/view_only.h CC_VIEW_ONLY_TEXT
const read = (p) => readFileSync(join(G, p), "utf8");
const enc = new TextEncoder(), dec = new TextDecoder("utf-8");

async function load(path) {
  const bytes = readFileSync(path);
  const { instance } = await WebAssembly.instantiate(bytes, {});
  const X = instance.exports;
  const res = X.cc_alloc(12);
  const mem = () => new Uint8Array(X.memory.buffer);
  const put = (data) => {
    const b = typeof data === "string" ? enc.encode(data) : data;
    const p = X.cc_alloc(b.length || 1);
    mem().set(b, p);
    return [p, b.length];
  };
  /** f(...rest, ...ptr,len pairs, res) -> {status, bytes, text, value} */
  const call = (f, args, ...rest) => {
    const ins = args.map(put);
    const status = f(...rest, ...ins.flat(), res);
    for (const [p] of ins) X.cc_free(p);
    const dv = new DataView(X.memory.buffer);
    const ptr = dv.getUint32(res, true), len = dv.getUint32(res + 4, true), value = dv.getUint32(res + 8, true);
    const out = mem().slice(ptr, ptr + len);
    X.cc_free(ptr);
    return { status, bytes: out, get text() { return dec.decode(out); }, value };
  };
  return { X, call, size: bytes.length, gzip: gzipSync(bytes, { level: 9 }).length };
}

const F = await load(FULL), V = await load(VIEW);

// ---- checks ----
const suites = [];
let cur;
function suite(name) {
  cur = { name, passed: 0, failed: 0, shown: 0 };
  suites.push(cur);
}
function check(what, ok, detail = "") {
  if (ok) return void cur.passed++;
  cur.failed++;
  if (cur.shown++ < 20) console.log(`FAIL ${cur.name}: ${what}${detail ? `\n  ${detail}` : ""}`);
}
const end = () => console.log(`${cur.name}: ${cur.passed} passed, ${cur.failed} failed`);
const hex = (b) => Buffer.from(b).toString("hex");
/** The same answer from both modules: status, value and bytes. */
function same(what, f, v) {
  const ok = f.status === v.status && f.value === v.value && Buffer.compare(Buffer.from(f.bytes), Buffer.from(v.bytes)) === 0;
  check(what, ok, ok ? "" : `editor ${f.status} ${f.value} ${f.bytes.length} B ${JSON.stringify(dec.decode(f.bytes.slice(0, 160)))}\n  view   ${v.status} ${v.value} ${v.bytes.length} B ${JSON.stringify(dec.decode(v.bytes.slice(0, 160)))}`);
  return ok;
}
const refused = (what, r) => check(`${what} -> 400 "${VIEW_ONLY}"`, r.status === 400 && r.text === VIEW_ONLY, `${r.status} ${r.text.slice(0, 120)}`);

// ---- query: the handle API ----
suite("query");
for (const [model, golden] of [["frame.json", "query.txt"], ["frame2.json", "frame2/query.txt"]]) {
  const text = read(model);
  const hf = F.call(F.X.open_text, [text]), hv = V.call(V.X.open_text, [text]);
  same(`open_text ${model}`, hf, hv);
  for (const line of read(golden).split("\n")) {
    const m = line.match(/^>>> query (\S+) (.*)$/);
    if (m) same(`${model}: query ${m[1]} ${m[2]}`, F.call(F.X.query, [m[1], m[2]], hf.value), V.call(V.X.query, [m[1], m[2]], hv.value));
  }
  for (const q of ["storeys", "materials", "project", "views", "sheets", "schedules", "ids"])
    same(`${model}: query ${q}`, F.call(F.X.query, [q, ""], hf.value), V.call(V.X.query, [q, ""], hv.value));
  same(`${model}: history`, F.call(F.X.history, [], hf.value), V.call(V.X.history, [], hv.value));
  // ---- mesh: the handle API ----
  for (const flags of [0, 1]) same(`${model}: mesh flags ${flags}`, F.call(F.X.mesh, [], hf.value, flags), V.call(V.X.mesh, [], hv.value, flags));
  // The verb table is the editor's: the view build lists none.
  const vl = V.call(V.X.query, ["verbs", ""], hv.value);
  check(`${model}: view query verbs -> 200 []`, vl.status === 200 && vl.text === "[]", vl.text.slice(0, 80));
  check(`${model}: editor query verbs lists verbs`, JSON.parse(F.call(F.X.query, ["verbs", ""], hf.value).text).length > 0);
  F.X.close(hf.value), V.X.close(hv.value);
}
end();

// ---- session: every golden file through host_open, the same calls on both ----
suite("session");
const files = [
  ["frame.json", read("frame.json")], ["frame2.json", read("frame2.json")],
  ["frame.ifc", read("ifc/frame.ifc")], ["frame2.ifc", read("ifc/frame2.ifc")],
  ["frame.ifc2x3.ifc", read("ifc/frame.ifc2x3.ifc")],
  ...readdirSync(join(G, "ifc/fixtures")).filter((f) => f.endsWith(".ifc")).sort().map((f) => [f, read(`ifc/fixtures/${f}`)]),
  ...["frame.bimbleview", "frame2.bimbleview"].map((f) => [f, readFileSync(join(G, "view", f))]),
];
const READ_QUERIES = ["state", "actions", "storeys", "materials", "project", "status", "open_views", "visibility",
  "visibility_sets", "placing", "views", "sheets", "schedules", "links", "measurements", "recent_actions",
  "recent_files", "standards", "collab", "history", "view_clip", "view_annotations"];
let sessionCalls = 0;
for (const [i, [name, content]] of files.entries()) {
  const bytes = typeof content === "string" ? enc.encode(content) : content;
  // host_open flag 1: discard what is open (the module keeps one session).
  same(`${name}: host_open`, F.call(F.X.host_open, [name, bytes], 1), V.call(V.X.host_open, [name, bytes], 1));
  const both = (op, nm, body) => {
    sessionCalls++;
    const f = F.call(F.X.call, [op, nm, body]), v = V.call(V.X.call, [op, nm, body]);
    same(`${name}: ${op} ${nm} ${body.slice(0, 80)}`, f, v);
    return f;
  };
  const fmesh = () => {
    for (const flags of [0, 1]) same(`${name}: host_mesh flags ${flags}`, F.call(F.X.host_mesh, [], flags), V.call(V.X.host_mesh, [], flags));
    same(`${name}: change_json`, F.call(F.X.change_json, []), V.call(V.X.change_json, []));
  };
  for (const q of READ_QUERIES) both("query", q, "{}");
  both("query", "status", "null");
  fmesh();
  both("query", "elements", JSON.stringify({ limit: 100000 }));
  // The scene's ids (read-only files have no `elements`): BMSH rows of 48 bytes at 52, the GlobalId
  // first (session/mesh.c).
  const mb = F.call(F.X.host_mesh, [], 0).bytes, mdv = new DataView(mb.buffer, mb.byteOffset, mb.byteLength);
  const ids = [];
  for (let k = 0; mb.length >= 52 && k < mdv.getUint32(12, true); k++) ids.push(dec.decode(mb.subarray(52 + 48 * k, 74 + 48 * k)));
  check(`${name}: has elements`, ids.length > 0, String(ids.length));
  for (const id of ids.slice(0, 5)) both("query", "element", JSON.stringify({ id }));
  if (ids.length) {
    both("select", "-", JSON.stringify([ids[0]]));
    both("query", "state", "{}");
    both("select", "-", JSON.stringify(ids.slice(0, 3)));
    both("query", "element", JSON.stringify({ id: ids[1] ?? ids[0] }));
  }
  // Visibility (session/visibility.h) and the section box tool.
  both("action", "hide_now", "");
  both("query", "visibility", "{}");
  fmesh();
  both("action", "unhide", "");
  both("select", "-", JSON.stringify(ids.slice(0, 2)));
  for (const a of ["isolate_in_view", "box_to_selection", "section_box", "show_all_in_view", "reveal_hidden", "hide_in_view", "clear_temporary", "isolate_now"])
    both("action", a, ""), both("query", "visibility", "{}");
  both("tool", "box_set", JSON.stringify({ min: [0, 0, 0], max: [8000.5, 6000.25, 3000.1], on: true }));
  both("tool", "box_press", JSON.stringify({ ray: [-5000, 3000, 9000, 0.8320502943378437, 0, -0.5547001962252291], ray_tol: [-5000, 3150, 9000, 0.8320502943378437, 0, -0.5547001962252291] }));
  both("tool", "box_move", JSON.stringify({ ray: [-5000, 3000, 9000, 0.8, 0, -0.6] }));
  both("query", "visibility", "{}");
  both("tool", "box_release", "");
  both("tool", "box_cancel", "");
  fmesh();
  both("action", "section_box", "");
  both("action", "show_all_in_view", "");
  // Plans: every view's tab, its clip and annotations (session/views.h).
  const ov = both("query", "open_views", "{}");
  const views = ov.status === 200 ? JSON.parse(ov.text).views : [];
  for (const vw of Array.isArray(views) ? views : []) {
    both("tab", "-", JSON.stringify({ op: "open", tab: { type: "view", id: vw.id } }));
    both("query", "open_views", "{}");
    both("query", "view_clip", "{}");
    both("query", "view_annotations", JSON.stringify({ id: vw.id }));
    fmesh();
  }
  for (const vw of Array.isArray(views) ? views : []) both("tab", "-", JSON.stringify({ op: "close", tab: { type: "view", id: vw.id } }));
  both("select", "-", "[]");
  both("query", "state", "{}");
  fmesh();
  if (i === 0) check("frame.json has plan views", Array.isArray(views) && views.length > 1, JSON.stringify(views).slice(0, 120));
}
check("session calls made", sessionCalls > 500, String(sessionCalls));
end();

// ---- refused: the edits, saves and exports ----
suite("refused");
{
  const text = read("frame.json");
  for (const M of [F, V]) M.call(M.X.host_open, ["frame.json", enc.encode(text)], 1);
  const firstId = JSON.parse(F.call(F.X.call, ["query", "elements", "{}"]).text)[0].id;
  const edits = [
    ["verb", "place", JSON.stringify({ kind: "column", a: [0, 0] })], ["verb", "rename", JSON.stringify({ id: firstId, name: "x" })],
    ["apply", "-", "[]"], ["undo", "-", ""], ["redo", "-", ""],
    ["set_placing", "-", JSON.stringify({ kind: "column" })], ["tool", "pointer", JSON.stringify({ ray: [0, 0, 1000, 0, 0, -1] })],
    ["tool", "click", "{}"], ["tool", "cancel", ""], ["tool", "annotate", JSON.stringify({ mode: "dimension" })],
    ...["new", "import_ifc", "convert_open", "save", "save_as", "export_ifc4", "export_ifc2x3", "share_for_viewing",
      "undo", "redo", "place_column", "place_beam", "place_wall", "place_slab"].map((a) => ["action", a, ""]),
  ];
  for (const [op, nm, body] of edits) {
    refused(`view: ${op} ${nm}`, V.call(V.X.call, [op, nm, body]));
    const f = F.call(F.X.call, [op, nm, body]);
    check(`editor: ${op} ${nm} is not refused as view-only`, f.text !== VIEW_ONLY, f.text);
  }
  const fr = F.call(F.X.call, ["verb", "rename", JSON.stringify({ id: firstId, name: "x" })]);
  check("editor: verb rename -> 200", fr.status === 200, `${fr.status} ${fr.text}`);
  same("frame.json after the refused calls: query state", F.call(F.X.call, ["query", "state", "{}"]), V.call(V.X.call, ["query", "state", "{}"]));
  // The handle API.
  const hv = V.call(V.X.open_text, [text]).value;
  refused("view: open_ifc", V.call(V.X.open_ifc, [read("ifc/frame.ifc")]));
  refused("view: apply", V.call(V.X.apply, ["[]"], hv));
  refused("view: verb", V.call(V.X.verb, ["rename", JSON.stringify({ id: firstId, name: "x" })], hv));
  refused("view: undo", V.call(V.X.undo, [], hv));
  refused("view: redo", V.call(V.X.redo, [], hv));
  refused("view: save", V.call(V.X.save, [], hv));
  refused("view: export_ifc", V.call(V.X.export_ifc, ["f.ifc", "2026-01-01T00:00:00"], hv, 0));
  const hf = F.call(F.X.open_text, [text]).value;
  check("editor: open_ifc -> 200", F.call(F.X.open_ifc, [read("ifc/frame.ifc")]).status === 200);
  check("editor: save -> 200", F.call(F.X.save, [], hf).status === 200);
  V.X.close(hv), F.X.close(hf);
}
end();

// ---- time: the view build is -Os (build.sh VIEW_OPT), the editor -O2 ----
// A synthetic model made with the editor: frame.json plus 1500 columns and 1500 beams (verb place),
// saved as .json and exported as IFC4. Each opened (host_open) and meshed (host_mesh) on both
// modules: the answers byte for byte, and the mean time of 5 rounds after one warm-up.
suite("time");
{
  const F = await load(FULL), V = await load(VIEW); // fresh sessions: the same revisions on both
  const h = F.call(F.X.open_text, [read("frame.json")]).value;
  const N1 = "1uAZA0Yfn05f$xRd1k4uPR";
  let placed = 0;
  for (let i = 0; i < 50; i++)
    for (let j = 0; j < 30; j++) {
      const x = 20000 + 3000 * i, y = 3000 * j;
      placed += F.call(F.X.verb, ["place", JSON.stringify({ a: [x, y], kind: "column", storey: N1 })], h).status === 200;
      placed += F.call(F.X.verb, ["place", JSON.stringify({ a: [x, y], b: [x + 3000, y], kind: "beam", storey: N1 })], h).status === 200;
    }
  check("3000 elements placed", placed === 3000, String(placed));
  const big = [["big.json", F.call(F.X.save, [], h).bytes], ["big.ifc", F.call(F.X.export_ifc, ["big.ifc", "2026-01-01T00:00:00"], h, 0).bytes]];
  F.X.close(h);
  for (const [name, bytes] of big) {
    same(`${name}: host_open`, F.call(F.X.host_open, [name, bytes], 1), V.call(V.X.host_open, [name, bytes], 1));
    same(`${name}: host_mesh`, F.call(F.X.host_mesh, [], 0), V.call(V.X.host_mesh, [], 0));
    const time = (M) => {
      const t0 = performance.now();
      for (let k = 0; k < 5; k++) M.call(M.X.host_open, [name, bytes], 1), M.call(M.X.host_mesh, [], 0);
      return (performance.now() - t0) / 5;
    };
    time(F), time(V);
    const tf = time(F), tv = time(V);
    console.log(`open + host_mesh of ${name} (${bytes.length} B), mean of 5: editor (-O2) ${tf.toFixed(1)} ms, view (-Os) ${tv.toFixed(1)} ms`);
  }
}
end();

console.log(`ccore.wasm ${F.size} bytes (gzip -9 ${F.gzip}), ccore-view.wasm ${V.size} bytes (gzip -9 ${V.gzip})`);
const p = suites.reduce((a, s) => a + s.passed, 0), f = suites.reduce((a, s) => a + s.failed, 0);
console.log(`${p} passed, ${f} failed`);
process.exit(f ? 1 : 0);
