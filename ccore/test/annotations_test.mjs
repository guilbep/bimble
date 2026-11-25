// Dimensions and texts in the C session (ticket C4, src/annot.h): node ccore/test/annotations_test.mjs
// Starts out/tcc/bimble-ccore (or $HOST_BIN) on a copy of golden/frame.json and checks, over HTTP:
//   the verbs add_dimension, add_text, move_annotation, delete_annotation (C additions): one
//     history step each, a default plan stored on its first annotation (add_view), errors;
//   query view_annotations: the resolved geometry (plan mm), the value in mm (DimUnit::Mm) or
//     cm (DimUnit::Cm), anchors that follow their element: move the column (verb move) -> the
//     value changes; set_generator (a wider column) -> the corner moves; undo -> back;
//   the annotate tool (op tool "annotate", then pointer/click/cancel with orthographic rays as
//     the plan camera sends them): 2 clicks snapped to two column corners, a third for the
//     offset -> one dimension anchored on both corners; Esc cancels; the text mode;
//   save and reopen (.bimble v1 and v2): the annotations come back with their anchors.
// Prints "N passed, M failed"; exits non-zero on a failure. Kills what it starts.
import { spawn } from "node:child_process";
import { copyFileSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { port } from "./ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("annotations_test", 47412);
const TOKEN = "annotest-token";
const base = `http://127.0.0.1:${PORT}`;
let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail && !ok ? `: ${detail}` : ""}`);
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const call = async (op, name = "-", body = "") => {
  const r = await fetch(`${base}/${op}/${encodeURIComponent(name)}?t=${TOKEN}`, { method: "POST", body, headers: { "Content-Type": "text/plain" } });
  return { status: r.status, text: await r.text() };
};
const q = async (name, args) => {
  const r = await call("query", name, args === undefined ? "" : JSON.stringify(args));
  if (r.status !== 200) throw new Error(`query ${name}: ${r.status} ${r.text}`);
  return JSON.parse(r.text);
};
const verb = (name, args) => call("verb", name, JSON.stringify(args));
const undoN = async () => (await q("history")).undo.length;

const PLAN_N1 = "3w8zGYnP$Ay1lyvMZjDnXq"; // Plan Niveau 1 (Rust's derived id, views_test.mjs)
const C00 = "2P97ZHH3v3XutfaphHA_Tu", C60 = "2KmXk9zn1FAPxjTPfLpwLP"; // columns of Niveau 1 at (0,0), (6000,0), 300 x 300

const tmp = mkdtempSync(join(tmpdir(), "ccore-annot-"));
const model = join(tmp, "frame.bimble");
copyFileSync(join(CCORE, "golden/frame.json"), model);
const host = spawn(BIN, [String(PORT), model], { env: { ...process.env, BIMBLE_TOKEN: TOKEN }, stdio: ["ignore", "ignore", "pipe"], detached: true });
let log = "";
host.stderr.on("data", (d) => (log += d));
try {
  for (let i = 0; i < 400 && !log.includes("listening"); i++) await sleep(10);
  if (!log.includes("listening")) throw new Error(`host did not start: ${log}`);

  // ---- verbs ----
  const h0 = await undoN();
  const verbs = (await q("verbs")).map((v) => v.name);
  check("query verbs stays Rust's 14 (the C additions are not listed)", verbs.length === 14 && !verbs.includes("add_dimension"), JSON.stringify(verbs));
  const ds = await call("query", "describe", JSON.stringify({ name: "add_dimension" }));
  check("describe add_dimension -> its schema", ds.status === 200 && JSON.parse(ds.text).title === "AddDimensionArgs", ds.text.slice(0, 200));
  const corner = (id, k) => ({ element: id, feature: `corner:${k}` });
  // corner 0 = (-150, -150) of each column: 6000 apart; the line 800 mm below (to the right of a->b).
  let r = await verb("add_dimension", { view: PLAN_N1, a: corner(C00, 0), b: corner(C60, 0), offset: -800 });
  check("add_dimension on two column corners -> 200", r.status === 200, r.text);
  check("one history step", (await undoN()) === h0 + 1);
  const sv = await q("views");
  check("the default plan is stored with it (add_view)", sv.length === 1 && sv[0].id === PLAN_N1 && sv[0].annotations === 1, JSON.stringify(sv));
  let va = await q("view_annotations", { view: PLAN_N1 });
  let d = va.items[0];
  check("view_annotations: one dimension, 6000 mm, text \"6000\"", va.items.length === 1 && d.type === "dimension" && d.value_mm === 6000 && d.text === "6000", JSON.stringify(va));
  check("its ends at the corners, line 800 below, dir +x", JSON.stringify([d.a, d.b, d.qa, d.qb, d.dir]) === JSON.stringify([[-150, -150], [5850, -150], [-150, -950], [5850, -950], [1, 0]]), JSON.stringify(d));
  check("anchors follow their element", d.anchors.every((x) => x.follows) && d.anchors[0].element === C00 && d.anchors[1].feature === "corner:0", JSON.stringify(d.anchors));
  check("unit cm -> \"600\" (DimUnit::Cm)", (await q("view_annotations", { view: PLAN_N1, unit: "cm" })).items[0].text === "600");
  check("unit m -> \"6.00\"", (await q("view_annotations", { view: PLAN_N1, unit: "m" })).items[0].text === "6.00");
  const dimId = d.id;

  // ---- anchors follow ----
  r = await verb("move", { ids: [C60], d: [1000, 0, 0] });
  check("move the column 1000 in x -> 200", r.status === 200, r.text);
  d = (await q("view_annotations", { view: PLAN_N1 })).items[0];
  check("the dimension follows: 7000", d.value_mm === 7000 && d.text === "7000" && d.b[0] === 6850, JSON.stringify(d));
  check("undo the move -> 6000 again", (await call("undo")).status === 200 && (await q("view_annotations", { view: PLAN_N1 })).items[0].value_mm === 6000);
  r = await verb("set_generator", { id: C60, params: { width: 500 } });
  check("set_generator width 500 -> 200", r.status === 200, r.text);
  d = (await q("view_annotations", { view: PLAN_N1 })).items[0];
  check("a wider column: its corner 0 at x 5750 -> 5900", d.value_mm === 5900 && d.b[0] === 5750, JSON.stringify(d));
  await call("undo");

  // ---- text, move, delete ----
  r = await verb("add_text", { view: PLAN_N1, at: [1000, 2000], text: "Poteau" });
  check("add_text -> 200, one step", r.status === 200 && (await undoN()) === h0 + 2, r.text);
  va = await q("view_annotations", { view: PLAN_N1 });
  const t = va.items[1];
  check("the text: at, size 3.5 (Rust's default), angle 0", t.type === "text" && t.text === "Poteau" && t.size_mm === 3.5 && JSON.stringify(t.at) === "[1000,2000]" && t.angle_deg === 0, JSON.stringify(t));
  r = await verb("move_annotation", { view: PLAN_N1, id: t.id, by: [100, -50] });
  check("move_annotation of the text -> at + by", r.status === 200 && JSON.stringify((await q("view_annotations", { view: PLAN_N1 })).items[1].at) === "[1100,1950]", r.text);
  r = await verb("move_annotation", { view: PLAN_N1, id: dimId, by: [0, -200] });
  d = (await q("view_annotations", { view: PLAN_N1 })).items[0];
  check("move_annotation of the dimension, 200 down -> offset -1000, line at y -1150", r.status === 200 && d.offset_mm === -1000 && d.qa[1] === -1150, JSON.stringify(d));
  r = await verb("move_annotation", { view: PLAN_N1, id: dimId, offset: 500 });
  check("move_annotation offset 500 -> line above", r.status === 200 && (await q("view_annotations", { view: PLAN_N1 })).items[0].qa[1] === 350, r.text);
  r = await verb("delete_annotation", { view: PLAN_N1, id: t.id });
  check("delete_annotation the text -> 200, one item left", r.status === 200 && (await q("view_annotations", { view: PLAN_N1 })).items.length === 1, r.text);
  const hd = await undoN();
  await call("undo");
  check("undo the delete -> the text is back", (await q("view_annotations", { view: PLAN_N1 })).items.length === 2 && (await undoN()) === hd - 1);
  await call("redo");

  // ---- errors ----
  const e1 = await verb("add_dimension", { view: PLAN_N1, a: [0, 0], b: [0, 0], offset: 0 });
  check("two equal points -> 400", e1.status === 400 && /two points apart/.test(e1.text), e1.text);
  const e2 = await verb("add_dimension", { view: PLAN_N1, a: corner(C00, 9), b: [0, 0], offset: 0 });
  check("a column has no corner:9 -> 400", e2.status === 400 && /no feature "corner:9"/.test(e2.text), e2.text);
  const e3 = await verb("add_text", { view: "1v7Vtm2FfafmYPKIQEXnlY", at: [0, 0], text: "x" });
  check("a text in the 3D view -> 400", e3.status === 400 && /go in 2D views/.test(e3.text), e3.text);
  const e4 = await verb("delete_annotation", { view: PLAN_N1, id: "0000000000000000000000" });
  check("delete an unknown id -> 400", e4.status === 400 && /no dimension or text/.test(e4.text), e4.text);
  const e5 = await verb("add_dimension", { view: PLAN_N1, a: { feature: "origin" }, b: [0, 0], offset: 0 });
  check("a feature without its element -> 400", e5.status === 400 && /needs its element/.test(e5.text), e5.text);

  // ---- the annotate tool, orthographic rays as the plan camera ----
  check("annotate in the 3D view -> 400", (await call("tool", "annotate", '{"mode":"dimension"}')).status === 400);
  check("open Plan Niveau 1", (await call("tab", "-", JSON.stringify({ op: "open", tab: { type: "view", id: PLAN_N1 } }))).status === 200);
  const origin = (await q("visibility")).section_box.origin_mm;
  const ray = (x, y) => [x - origin[0], y - origin[1], 100000 - origin[2], 0, 0, -1];
  const ptr = (x, y) => JSON.stringify({ ray: ray(x, y), ray_tol: ray(x + 150, y) });
  let st = JSON.parse((await call("tool", "annotate", '{"mode":"dimension"}')).text);
  check("annotate dimension -> active, its view", st.active && st.mode === "dimension" && st.view === PLAN_N1, JSON.stringify(st));
  check("query placing: not placing", (await q("placing")).active !== true);
  st = JSON.parse((await call("tool", "pointer", ptr(-140, -160))).text);
  check("hover near a corner: snapped to (-150,-150), its anchor", JSON.stringify(st.hover?.at) === "[-150,-150]" && st.hover.snap === "corner" && st.anchor?.element === C00 && st.anchor.feature === "corner:0", JSON.stringify(st));
  const h1 = await undoN();
  st = JSON.parse((await call("tool", "click", ptr(-140, -160))).text);
  check("click 1 -> one point", st.points.length === 1 && st.anchors.length === 1, JSON.stringify(st));
  st = JSON.parse((await call("tool", "pointer", ptr(5860, -140))).text);
  check("preview a -> pointer: 6000", st.preview?.value_mm === 6000 && st.preview.text === "6000", JSON.stringify(st.preview));
  st = JSON.parse((await call("tool", "click", ptr(5860, -140))).text);
  check("click 2 -> two points, both anchored", st.points.length === 2 && st.anchors[1]?.element === C60, JSON.stringify(st));
  st = JSON.parse((await call("tool", "pointer", ptr(3000, -1500))).text);
  check("preview of the third click: offset -1350 (rounded to 10 mm by the pointer)", st.preview?.offset_mm === -1350 && st.preview.value_mm === 6000, JSON.stringify(st.preview));
  check("no history step yet", (await undoN()) === h1);
  st = JSON.parse((await call("tool", "click", ptr(3000, -1500))).text);
  check("click 3 -> a dimension added, one history step", typeof st.added === "string" && (await undoN()) === h1 + 1 && st.points.length === 0 && st.active, JSON.stringify(st));
  va = await q("view_annotations");
  const nd = va.items.find((x) => x.id === st.added);
  check("view_annotations (the active view): the new one, anchored on both corners, 6000", nd && nd.value_mm === 6000 && nd.anchors[0].feature === "corner:0" && nd.anchors[1].element === C60 && va.view === PLAN_N1, JSON.stringify(nd));
  await call("tool", "click", ptr(0, 5000));
  st = JSON.parse((await call("tool", "cancel")).text);
  check("Esc -> the tool stops, the point goes, nothing added", !st.active && st.points.length === 0 && (await undoN()) === h1 + 1, JSON.stringify(st));
  st = JSON.parse((await call("tool", "annotate", '{"mode":"text","text":"Axe A"}')).text);
  check("annotate text -> active", st.active && st.mode === "text" && st.text === "Axe A", JSON.stringify(st));
  st = JSON.parse((await call("tool", "click", JSON.stringify({ ray: ray(2003, 3007), ray_tol: ray(2153, 3007), free: true }))).text);
  const tx = (await q("view_annotations")).items.find((x) => x.id === st.added);
  check("a click adds the text at the point (rounded to 10 mm), the tool stops", tx && tx.text === "Axe A" && JSON.stringify(tx.at) === "[2000,3010]" && !st.active, JSON.stringify({ st, tx }));
  check("undo -> the text goes", (await call("undo")).status === 200 && !(await q("view_annotations")).items.some((x) => x.id === st.added));
  await call("redo");

  // ---- save and reopen, v1 and v2 ----
  const before = JSON.stringify((await q("view_annotations", { view: PLAN_N1 })).items);
  for (const v of [1, 2]) {
    const to = join(tmp, v === 2 ? "saved-v2.bimble" : "saved-v1.json"); // save_as: .bimble writes v2, .json v1
    const sa = await call("action", "save_as", JSON.stringify({ path: to }));
    if (sa.status !== 200) { check(`save v${v}`, false, sa.text); continue; }
    const head = readFileSync(to).subarray(0, 8).toString("latin1");
    if (v === 1) check("v1 file: the anchor's feature in the JSON", readFileSync(to, "utf8").includes('"feature": "corner:0"'));
    const op = await call("action", "open", JSON.stringify({ path: to, discard: true }));
    check(`reopen v${v} -> 200`, op.status === 200, op.text);
    const after = JSON.stringify((await q("view_annotations", { view: PLAN_N1 })).items);
    check(`v${v} round trip: the same annotations (${head.slice(0, 1) === "{" ? "JSON" : "binary"})`, after === before, `\n  before ${before}\n  after  ${after}`);
  }
} catch (e) {
  check("no exception", false, e.stack ?? String(e));
} finally {
  try { process.kill(-host.pid, "SIGKILL"); } catch {}
  rmSync(tmp, { recursive: true, force: true });
}
console.log(`annotations_test: ${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
