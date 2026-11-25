// Visibility and the section box in the C session (ticket B3): node ccore/test/visibility_test.mjs
// Starts out/tcc/bimble-ccore (or $HOST_BIN) on a copy of golden/frame.json and checks, over HTTP:
//   hide_now, isolate_now, unhide, clear_temporary, hide_in_view, isolate_in_view,
//   show_all_in_view, reveal_hidden: `query visibility`, the change feed's "view" signature, the
//     enabled and pressed actions, the selection cleared (kept while revealing);
//   section_box, box_to_selection: on/off, the box fitted around what is visible or the selected
//     element (bounds of GET /mesh plus ClipBox::around's margin, f32), printed as serde prints f32;
//   the box tool (op tool): box_set, box_press/box_move/box_release/box_cancel (a face follows the
//     pointer ray, 10 mm minimum; the session's box changes only on release);
//   session state only: rev and history never change; a new model resets it all.
// The Rust twin is not available here: the expected shapes are written from
// crates/bimble-viewer/src/calls.rs:442-460 (query visibility), :134-167 (change_json "view"),
// ribbon.rs:555-643 (enabled, pressed), visibility.rs (hide_or_isolate), lib.rs:2156-2190 and
// bimble-render ClipBox::around (the box), section.rs (the drag, MIN_SIZE).
// Prints "N passed, M failed"; exits non-zero on a failure. Kills what it starts.
import { spawn } from "node:child_process";
import { copyFileSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { port } from "./ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("visibility_test", 47394);
const TOKEN = "vistest-token";
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
  return r.text;
};
const change = async () => (await (await fetch(`${base}/change?since=0&t=${TOKEN}`)).json()).change;
const act = (name) => call("action", name);

// ---- what Rust would print ----
/** serde_json's f32: the shortest digits that read back to the same f32, "x.0" for an integer. */
function f32Text(f) {
  if (f === 0) return "0.0";
  for (let p = 1; p <= 9; p++) {
    const x = Number(f.toPrecision(p));
    if (Math.fround(x) === f) return Number.isInteger(x) ? `${x}.0` : String(x);
  }
  throw new Error(`no f32 text for ${f}`);
}
const f64Text = (x) => (Number.isInteger(x) ? `${x}.0` : String(x));
const capped = (ids) => `{"count":${ids.length},"ids":${JSON.stringify(ids.slice(0, 50))}}`;
/** calls.rs:442-460, keys sorted (json!, preserve_order off). */
function visText({ hiddenNow = [], isolatedNow = [], hiddenInView = [], isolatedInView = [], on = false, box = null, origin }) {
  const b = box ? `{"max":[${box.max.map(f32Text)}],"min":[${box.min.map(f32Text)}],"yaw":0.0}` : "null";
  return `{"hidden_in_view":${capped(hiddenInView)},"hidden_now":${capped(hiddenNow)},"isolated_in_view":${capped(isolatedInView)},` +
    `"isolated_now":${capped(isolatedNow)},"section_box":{"box":${b},"on":${on},"origin_mm":[${origin.map(f64Text)}]}}`;
}
/** ClipBox::around over f32 boxes (bimble-render lib.rs:148-167). */
function around(boxes) {
  const f = Math.fround, lo = [Infinity, Infinity, Infinity], hi = [-Infinity, -Infinity, -Infinity];
  for (const b of boxes) for (let k = 0; k < 3; k++) lo[k] = Math.min(lo[k], b[k]), hi[k] = Math.max(hi[k], b[3 + k]);
  const m = [0, 1, 2].map((k) => f(1 + f(f(hi[k] - lo[k]) * f(1e-5))));
  return { min: [0, 1, 2].map((k) => f(lo[k] - m[k])), max: [0, 1, 2].map((k) => f(hi[k] + m[k])) };
}
/** Per element of GET /mesh (BMSH, session/mesh.c): its id and its f32 box. */
async function meshBoxes() {
  const ab = await (await fetch(`${base}/mesh?t=${TOKEN}`)).arrayBuffer();
  const dv = new DataView(ab), u = (o) => dv.getUint32(o, true);
  const nE = u(12), nV = u(16), bytes = new Uint8Array(ab);
  const origin = [dv.getFloat64(28, true), dv.getFloat64(36, true), dv.getFloat64(44, true)];
  const pos = new Float32Array(ab, 52 + 48 * nE, 3 * nV), out = new Map();
  for (let e = 0, o = 52; e < nE; e++, o += 48) {
    const id = String.fromCharCode(...bytes.subarray(o, o + 22)), v0 = u(o + 24), nv = u(o + 28);
    const b = [Infinity, Infinity, Infinity, -Infinity, -Infinity, -Infinity];
    for (let v = v0; v < v0 + nv; v++)
      for (let k = 0; k < 3; k++) b[k] = Math.min(b[k], pos[3 * v + k]), b[3 + k] = Math.max(b[3 + k], pos[3 * v + k]);
    out.set(id, b);
  }
  return { origin, boxes: out };
}

const tmp = mkdtempSync(join(tmpdir(), "ccore-vis-"));
const model = join(tmp, "frame.bimble");
copyFileSync(join(CCORE, "golden/frame.json"), model);
const host = spawn(BIN, [String(PORT), model], { env: { ...process.env, BIMBLE_TOKEN: TOKEN }, stdio: ["ignore", "ignore", "pipe"], detached: true });
let log = "";
host.stderr.on("data", (d) => (log += d));
try {
  for (let i = 0; i < 400 && !log.includes("listening"); i++) await sleep(10);
  if (!log.includes("listening")) throw new Error(`host did not start: ${log}`);
  const { origin, boxes } = await meshBoxes();
  const ids = [...boxes.keys()].sort();
  const [a, b, c, d] = ids;
  const rev0 = (await change()).rev, hist0 = await q("history");
  const view = async () => JSON.stringify((await change()).view);
  const sel = async () => JSON.stringify((await change()).selection);
  const enabled = async (name) => JSON.parse(await q("actions")).find((x) => x.name === name).enabled;
  const pressed = async (name) => JSON.parse(await q("actions")).find((x) => x.name === name).pressed;
  const vis = (o) => visText({ origin, ...o });
  const same = async (what, want) => {
    const got = await q("visibility");
    check(`${what}: query visibility`, got === want, `\n  got  ${got}\n  want ${want}`);
  };

  // ---- nothing hidden, no box ----
  await same("loaded", vis({}));
  check("loaded: view signature [0,0,0,0,0,0,0,1]", (await view()) === "[0,0,0,0,0,0,0,1]", await view());
  for (const n of ["hide_now", "isolate_now", "hide_in_view", "isolate_in_view", "unhide", "clear_temporary", "show_all_in_view", "box_to_selection"])
    check(`no selection, nothing hidden: ${n} disabled`, !(await enabled(n)));
  check("section_box and reveal_hidden enabled with a model", (await enabled("section_box")) && (await enabled("reveal_hidden")));
  const off = await act("hide_now");
  check("hide_now without a selection -> 400 not enabled", off.status === 400 && off.text === 'action "hide_now" is not enabled now', off.text);

  // ---- for now: hide, hide more, unhide, isolate, clear ----
  await call("select", "-", JSON.stringify([b, a]));
  check("selected: the hide actions enabled", (await enabled("hide_now")) && (await enabled("isolate_in_view")) && !(await enabled("unhide")));
  check("hide_now -> 200", (await act("hide_now")).status === 200);
  await same("hide_now a, b", vis({ hiddenNow: [a, b] }));
  check("hide_now: view [0,2,0,0,0,0,0,1]", (await view()) === "[0,2,0,0,0,0,0,1]", await view());
  check("hide_now: the selection is cleared (visibility.rs:106)", (await sel()) === "[]", await sel());
  check("hide_now: clear_temporary enabled", await enabled("clear_temporary"));
  await call("select", "-", JSON.stringify([c]));
  await act("hide_now");
  await same("hide_now c: hiding more adds", vis({ hiddenNow: [a, b, c] }));
  await call("select", "-", JSON.stringify([c]));
  check("a hidden element selected: unhide enabled", await enabled("unhide"));
  await act("unhide");
  await same("unhide c", vis({ hiddenNow: [a, b] }));
  await call("select", "-", JSON.stringify([d]));
  await act("isolate_now");
  await same("isolate_now d replaces", vis({ isolatedNow: [d] }));
  check("isolate_now: view [0,1,1,0,0,0,0,1]", (await view()) === "[0,1,1,0,0,0,0,1]", await view());
  await act("clear_temporary");
  await same("clear_temporary", vis({}));
  check("clear_temporary: view back to [0,0,0,0,0,0,0,1]", (await view()) === "[0,0,0,0,0,0,0,1]", await view());

  // ---- in the view ----
  await call("select", "-", JSON.stringify([a]));
  await act("hide_in_view");
  await same("hide_in_view a", vis({ hiddenInView: [a] }));
  check("hide_in_view: view [0,0,0,1,0,1,0,1], show_all_in_view enabled", (await view()) === "[0,0,0,1,0,1,0,1]" && (await enabled("show_all_in_view")), await view());
  await call("select", "-", JSON.stringify([b]));
  await act("isolate_in_view");
  await same("isolate_in_view b clears the hidden", vis({ isolatedInView: [b] }));
  check("isolate_in_view: view [0,0,0,0,1,1,0,1]", (await view()) === "[0,0,0,0,1,1,0,1]", await view());
  await call("select", "-", JSON.stringify([c]));
  await act("unhide");
  await same("unhide c joins the isolation", vis({ isolatedInView: [b, c] }));
  await act("show_all_in_view");
  await same("show_all_in_view", vis({}));
  check("show_all_in_view: disabled again", !(await enabled("show_all_in_view")));

  // ---- reveal hidden: the selection stays ----
  await act("reveal_hidden");
  check("reveal_hidden pressed", await pressed("reveal_hidden"));
  await call("select", "-", JSON.stringify([a]));
  await act("hide_now");
  check("revealing: hide_now keeps the selection", (await sel()) === JSON.stringify([a]), await sel());
  const sets = JSON.parse(await q("visibility_sets"));
  check("query visibility_sets: the whole sets", JSON.stringify(sets) === JSON.stringify({ hidden_in_view: [], isolated_in_view: [], now: { ids: [a], isolate: false }, reveal: true }), JSON.stringify(sets));
  await act("reveal_hidden");
  await act("clear_temporary");
  check("reveal_hidden off", !(await pressed("reveal_hidden")));

  // ---- the section box ----
  await call("select", "-", "[]");
  check("section_box -> 200", (await act("section_box")).status === 200);
  const all = around([...boxes.values()]);
  await same("section_box on: fitted around everything visible", vis({ on: true, box: all }));
  check("section_box: view [1,0,0,0,0,0,0,1], pressed, box_to_selection enabled",
    (await view()) === "[1,0,0,0,0,0,0,1]" && (await pressed("section_box")) && (await enabled("box_to_selection")), await view());
  await call("select", "-", JSON.stringify([d]));
  await act("box_to_selection");
  const one = around([boxes.get(d)]);
  await same("box_to_selection: around the selected element", vis({ on: true, box: one }));
  await act("section_box");
  await same("section_box off: the box is kept", vis({ on: false, box: one }));
  await act("section_box");
  await same("section_box on again: not refitted", vis({ on: true, box: one }));
  // Hidden elements are left out of the fit: d isolated (the selection then cleared), fit to what shows.
  await act("clear_temporary");
  await call("select", "-", JSON.stringify([d]));
  await act("isolate_now");
  check("box_to_selection with nothing selected -> 200", (await act("box_to_selection")).status === 200);
  await same("box_to_selection: around the visible elements only (d isolated)", vis({ on: true, box: one, isolatedNow: [d] }));

  // ---- the box tool ----
  const set = await call("tool", "box_set", JSON.stringify({ min: [0, 0, 0], max: [8000, 6000, 3000], on: true }));
  const boxSet = { min: [0, 0, 0], max: [8000, 6000, 3000] };
  check("tool box_set -> 200, its state", set.status === 200 && set.text === '{"active":false,"box":{"max":[8000.0,6000.0,3000.0],"min":[0.0,0.0,0.0],"yaw":0.0},"face":null,"on":true}', set.text);
  await same("box_set", vis({ on: true, box: boxSet, isolatedNow: [d] }));
  await act("clear_temporary");
  const bad = await call("tool", "box_set", JSON.stringify({ min: [0, 0, 0], max: [5, 6000, 3000] }));
  check("box_set thinner than 10 mm -> 400", bad.status === 400 && /at least 10 mm/.test(bad.text), bad.text);
  // A ray from up and beside the box towards the top face's centre (4000, 3000, 3000).
  const top = [4000, 3000, 3000], eye = [4000 - 9000, 3000, 3000 + 6000];
  const ray = (p, from = eye) => { const dd = p.map((v, k) => v - from[k]), l = Math.hypot(...dd); return [...from, ...dd.map((v) => v / l)]; };
  const press = await call("tool", "box_press", JSON.stringify({ ray: ray(top), ray_tol: ray([4000, 3000 + 150, 3000]) }));
  const ps = JSON.parse(press.text);
  check("box_press on the top handle: face 5 held", press.status === 200 && ps.active === true && ps.face === 5, press.text);
  const mv = JSON.parse((await call("tool", "box_move", JSON.stringify({ ray: ray([4000, 3000, 2500]) }))).text);
  check("box_move 500 mm down: the preview's top at 2500", Math.abs(mv.box.max[2] - 2500) < 0.01 && mv.box.min[2] === 0, JSON.stringify(mv.box));
  await same("box_move: the session's box is unchanged", vis({ on: true, box: boxSet }));
  const low = JSON.parse((await call("tool", "box_move", JSON.stringify({ ray: ray([4000, 3000, -4000]) }))).text);
  check("box_move below the bottom: 10 mm thick at least (section.rs MIN_SIZE)", low.box.max[2] === 10, JSON.stringify(low.box));
  const rel = JSON.parse((await call("tool", "box_release", JSON.stringify({ ray: ray([4000, 3000, 1500]) }))).text);
  check("box_release: the top at 1500, the drag over", rel.active === false && rel.face === null && Math.abs(rel.box.max[2] - 1500) < 0.01, JSON.stringify(rel));
  const committed = JSON.parse(await q("visibility")).section_box.box;
  check("box_release: the session's box is the released one", JSON.stringify(committed) === JSON.stringify(rel.box), JSON.stringify(committed));
  const miss = JSON.parse((await call("tool", "box_press", JSON.stringify({ ray: ray([20000, 20000, 0]), ray_tol: ray([20000, 20150, 0]) }))).text);
  check("box_press away from every handle: inactive (the page orbits)", miss.active === false && miss.face === null, JSON.stringify(miss));
  await call("tool", "box_press", JSON.stringify({ ray: ray([4000, 3000, 1500]), ray_tol: ray([4000, 3150, 1500]) }));
  await call("tool", "box_move", JSON.stringify({ ray: ray([4000, 3000, 900]) }));
  const cancel = JSON.parse((await call("tool", "box_cancel", "")).text);
  check("box_cancel: the box as it was", cancel.active === false && JSON.stringify(cancel.box) === JSON.stringify(committed), JSON.stringify(cancel));
  const unk = await call("tool", "box_nope", "{}");
  check("tool box_nope -> 400 (unknown tool call)", unk.status === 400 && /unknown tool call/.test(unk.text), unk.text);
  await act("section_box");
  const offPress = JSON.parse((await call("tool", "box_press", JSON.stringify({ ray: ray(top) }))).text);
  check("box off: box_press grabs nothing", offPress.active === false && offPress.on === false, JSON.stringify(offPress));

  // ---- session state only ----
  check("rev unchanged by every visibility call", (await change()).rev === rev0, `${rev0} -> ${(await change()).rev}`);
  check("no history step", (await q("history")) === hist0, await q("history"));

  // ---- a new model: all reset ----
  await call("select", "-", JSON.stringify([a]));
  await act("hide_in_view");
  await act("section_box");
  check("before new: hidden and on", (await view()).startsWith("[1,0,0,1,"), await view());
  await act("new");
  const vNew = JSON.parse(await q("visibility"));
  check("new model: nothing hidden, no box (lib.rs:1657-1676)",
    vNew.hidden_in_view.count === 0 && vNew.section_box.on === false && vNew.section_box.box === null, JSON.stringify(vNew));
  check("new model: view [0,0,0,0,0,0,0,1]", (await view()) === "[0,0,0,0,0,0,0,1]", await view());
  const closed = await call("action", "close", JSON.stringify({ discard: true }));
  check("close (discarding the new model) -> 200", closed.status === 200, closed.text);
  const none = await call("tool", "box_press", JSON.stringify({ ray: ray(top) }));
  check("no model: box tool -> 400 no model open", none.status === 400 && none.text === "no model open", none.text);
  check("no model: query visibility origin null", JSON.parse(await q("visibility")).section_box.origin_mm === null);
} catch (e) {
  check("no exception", false, e.stack ?? String(e));
} finally {
  try { process.kill(-host.pid, "SIGKILL"); } catch {}
  rmSync(tmp, { recursive: true, force: true });
}
console.log(`visibility_test: ${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
