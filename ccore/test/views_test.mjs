// Views and tabs in the C session (ticket C1, session/views.h): node ccore/test/views_test.mjs
// Starts out/tcc/bimble-ccore (or $HOST_BIN) on a copy of golden/frame.json and checks, over HTTP:
//   query open_views: the 3D view, then a floor plan per storey by elevation, then a ceiling plan
//     per storey (view.rs default_views): ids, names, order, ranges (a storey lower than the cut
//     height gets its cut at half its height), auto_name, active, tabs, current, paper; the plan id
//     of Niveau 1 and the 3D id are Rust's (golden/verbs_all.txt, made by ccore_golden.rs with
//     GlobalId::derived);
//   query views: the model's stored views only (bimble-api query::views): [] on load, then the one
//     set_view stored, which also replaces the default in open_views (view.rs effective);
//   op tab: open a plan (its tab added, current, the placing storey follows, paper offered),
//     activate an open tab, activate a tab not open / open an unknown view / open a non-view ->
//     Rust's errors, close the current tab (the last one left is shown), close all (Home: no tab);
//     the change JSON's "tab" and "view"[7] (open_tabs.len()) follow;
//   query view_clip (a C addition): null in 3D; for the plan, the model's bounds cut to the view
//     range (depth to cut), its cut height;
//   the place tool in a plan: a ray straight down (an orthographic camera) places a column at the
//     clicked plan point on the plan's storey, one history step.
// Expected texts written from crates/bimble-viewer/src/calls.rs:205-232, :362-397,
// ribbon.rs:1443-1530, viewing.rs:17-38, :354-410, crates/bimble-core/src/view.rs:296-635.
// Prints "N passed, M failed"; exits non-zero on a failure. Kills what it starts.
import { spawn } from "node:child_process";
import { copyFileSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { port } from "./ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("views_test", 47398);
const TOKEN = "viewstest-token";
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
const tab = (op, t) => call("tab", "-", JSON.stringify({ op, tab: t }));
const vtab = (id) => ({ type: "view", id });

// Rust's ids (golden/verbs_all.txt: set_view on them, from ccore_golden.rs plan_view()).
const V3D = "1v7Vtm2FfafmYPKIQEXnlY";
const PLAN_N1 = "3w8zGYnP$Ay1lyvMZjDnXq";
const ST = { fond: "04Aobkc8v6$94cGl_9HNVX", n1: "1uAZA0Yfn05f$xRd1k4uPR", n2: "1jLno3kWj4Hfl6pUGBDd7E" };

const f = (x) => (Number.isInteger(x) ? `${x}.0` : String(x));
const plane = (level, off) => `{"level":"${level}","offset_mm":${f(off)}}`;
/** A ViewSummary + auto_name, keys sorted (calls.rs:368-385). */
function planText(id, look, storeyId, storeyName, cut, { name, scale = 100, auto = true, range } = {}) {
  const nm = name ?? `${look === "down" ? "Plan" : "Ceiling"} ${storeyName}`;
  const r = range ?? (look === "down"
    ? `{"bottom":${plane("associated", 0)},"cut":${plane("associated", cut)},"depth":${plane("associated", 0)},"top":${plane("above", 0)}}`
    : `{"bottom":${plane("associated", 0)},"cut":${plane("associated", cut)},"depth":${plane("above", 0)},"top":${plane("above", 0)}}`);
  const an = auto ? `{"kind":"${look === "down" ? "plan" : "ceiling"}","storey":"${storeyName}"}` : "null";
  return `{"annotations":0,"auto_name":${an},"crop":null,"id":"${id}","look":"${look}","name":"${nm}","range":${r},` +
    `"references":0,"scale":${scale},"storey":"${storeyId}","true_north":false,"type":"plan"}`;
}
const v3dText = `{"annotations":0,"auto_name":{"kind":"3d","storey":null},"crop":null,"id":"${V3D}","name":"3D","references":0,"scale":100,"true_north":false,"type":"three_d"}`;

const tmp = mkdtempSync(join(tmpdir(), "ccore-views-"));
const model = join(tmp, "frame.bimble");
copyFileSync(join(CCORE, "golden/frame.json"), model);
const host = spawn(BIN, [String(PORT), model], { env: { ...process.env, BIMBLE_TOKEN: TOKEN }, stdio: ["ignore", "ignore", "pipe"], detached: true });
let log = "";
host.stderr.on("data", (d) => (log += d));
try {
  for (let i = 0; i < 400 && !log.includes("listening"); i++) await sleep(10);
  if (!log.includes("listening")) throw new Error(`host did not start: ${log}`);

  // ---- the views on load ----
  const ov0 = JSON.parse(await q("open_views"));
  const ids = ov0.views.map((v) => v.id);
  check("7 views: 3D, 3 plans, 3 ceilings", ov0.views.length === 7, String(ov0.views.length));
  check("names in Rust's order (storeys by elevation, plans then ceilings)",
    JSON.stringify(ov0.views.map((v) => v.name)) === JSON.stringify(["3D", "Plan Fondations", "Plan Niveau 1", "Plan Niveau 2", "Ceiling Fondations", "Ceiling Niveau 1", "Ceiling Niveau 2"]),
    JSON.stringify(ov0.views.map((v) => v.name)));
  check("3D id = Rust's derived(\"bimble view 3d\")", ids[0] === V3D, ids[0]);
  check("Plan Niveau 1 id = Rust's derived(\"bimble view plan down <storey>\")", ids[2] === PLAN_N1, ids[2]);
  check("ids unique", new Set(ids).size === ids.length);
  const want = [v3dText,
    planText(ids[1], "down", ST.fond, "Fondations", 400), // 800 mm to Niveau 1: cut at half
    planText(ids[2], "down", ST.n1, "Niveau 1", 1200),
    planText(ids[3], "down", ST.n2, "Niveau 2", 1200), // the top storey: no level above
    planText(ids[4], "up", ST.fond, "Fondations", 400),
    planText(ids[5], "up", ST.n1, "Niveau 1", 2300),
    planText(ids[6], "up", ST.n2, "Niveau 2", 2300)];
  const wantOv0 = `{"active":"${V3D}","current":{"id":"${V3D}","type":"view"},"paper":{"offered":false,"on":false},` +
    `"tabs":[{"id":"${V3D}","type":"view"}],"views":[${want.join(",")}]}`;
  const ovText0 = await q("open_views");
  check("open_views on load: Rust's text", ovText0 === wantOv0, `\n  got  ${ovText0}\n  want ${wantOv0}`);
  check("query views on load: [] (stored views only)", (await q("views")) === "[]", await q("views"));
  const c0 = await change();
  check("change: tab is the 3D view, one tab", JSON.stringify(c0.tab) === `{"id":"${V3D}","type":"view"}` && c0.view[7] === 1, JSON.stringify(c0));
  const vc3 = JSON.parse(await q("view_clip"));
  check("view_clip in 3D: no clip", vc3 && vc3.type === "three_d" && vc3.clip === null && vc3.look === null, JSON.stringify(vc3));

  // ---- open a plan ----
  const st0 = JSON.parse(await q("placing")).storey;
  const op = await tab("open", vtab(ids[3])); // Plan Niveau 2
  check("tab open Plan Niveau 2 -> 200", op.status === 200, op.text);
  const ov1 = JSON.parse(await q("open_views"));
  check("open: active and current the plan, tabs [3D, plan], paper offered",
    ov1.active === ids[3] && ov1.current?.id === ids[3] && JSON.stringify(ov1.tabs.map((t) => t.id)) === JSON.stringify([V3D, ids[3]]) && ov1.paper.offered === true && ov1.paper.on === false,
    JSON.stringify({ a: ov1.active, c: ov1.current, t: ov1.tabs, p: ov1.paper }));
  const c1 = await change();
  check("change: tab the plan, 2 tabs, on_paper 0", c1.tab?.id === ids[3] && c1.view[7] === 2 && c1.view[6] === 0, JSON.stringify(c1));
  check("the placing storey follows the plan (viewing.rs:395-402)", JSON.parse(await q("placing")).storey === ST.n2 && st0 !== ST.n2, `${st0} -> ${JSON.parse(await q("placing")).storey}`);
  const vc = JSON.parse(await q("view_clip"));
  const origin = JSON.parse(await q("visibility")).section_box.origin_mm;
  check("view_clip: a plan looking down, cut 4200 (3000 + 1200), its storey", vc.type === "plan" && vc.look === "down" && vc.cut === 4200 && vc.elevation === 3000 && vc.storey === ST.n2 && vc.id === ids[3], JSON.stringify(vc));
  check("view_clip: z from the storey (depth) to the cut, scene coordinates",
    vc.clip && vc.clip.min[2] === Math.fround(3000 - origin[2]) && vc.clip.max[2] === Math.fround(4200 - origin[2]), JSON.stringify({ vc, origin }));
  check("view_clip: x, y the model's bounds", vc.clip && vc.clip.min[0] < vc.clip.max[0] && vc.clip.min[1] < vc.clip.max[1], JSON.stringify(vc.clip));

  // ---- activate, errors ----
  check("activate the 3D tab -> 200", (await tab("activate", vtab(V3D))).status === 200);
  check("current 3D, the plan's tab kept", JSON.parse(await q("open_views")).current.id === V3D && JSON.parse(await q("open_views")).tabs.length === 2);
  const na = await tab("activate", vtab(ids[1]));
  check("activate a view whose tab is not open -> 400 no such open tab", na.status === 400 && na.text === "no such open tab", na.text);
  const nv = await tab("open", vtab("0000000000000000000000"));
  check("open an unknown view -> 400 no such view", nv.status === 400 && nv.text === "no such view", nv.text);
  const np = await tab("open", { type: "project" });
  check("open a non-view -> 400 takes a view", np.status === 400 && np.text === 'op "open" takes a view', np.text);
  check("activate the plan tab -> 200", (await tab("activate", vtab(ids[3]))).status === 200);

  // ---- the place tool in the plan: an orthographic ray straight down ----
  const sp = await call("set_placing", "-", JSON.stringify({ kind: "column" }));
  check("set_placing column -> 200", sp.status === 200, sp.text);
  const hist0 = JSON.parse(await q("history")).undo.length;
  const P = [2470, 1530]; // world plan mm, away from the snap targets
  const ray = (x, y) => [x - origin[0], y - origin[1], 100000 - origin[2], 0, 0, -1];
  const tol = (x, y) => ray(x + 150, y);
  const clk = await call("tool", "click", JSON.stringify({ ray: ray(...P), ray_tol: tol(...P), free: true }));
  const ts = JSON.parse(clk.text);
  check("tool click in the plan -> a column placed", clk.status === 200 && typeof ts.placed === "string", clk.text.slice(0, 300));
  if (typeof ts.placed === "string") {
    const el = JSON.parse(await q("element", { id: ts.placed }));
    check("the column: IfcColumn on Niveau 2 at the clicked plan point", el.class === "IfcColumn" && el.storey === ST.n2 &&
      el.placement.origin[0] === P[0] && el.placement.origin[1] === P[1], JSON.stringify({ c: el.class, s: el.storey, o: el.placement.origin }));
  }
  check("one history step", JSON.parse(await q("history")).undo.length === hist0 + 1);
  await call("tool", "cancel");

  // ---- stored over the default ----
  const sv = await call("verb", "set_view", JSON.stringify({ id: ids[3], scale: 50 }));
  check("set_view scale 50 on Plan Niveau 2 -> 200", sv.status === 200, sv.text);
  const views = JSON.parse(await q("views"));
  check("query views: the stored plan only", views.length === 1 && views[0].id === ids[3] && views[0].scale === 50 && !("auto_name" in views[0]), JSON.stringify(views));
  const ov2 = JSON.parse(await q("open_views"));
  check("open_views: the stored view in the default's place, 7 views, auto_name kept (same name)",
    ov2.views.length === 7 && ov2.views[3].id === ids[3] && ov2.views[3].scale === 50 && ov2.views[3].auto_name?.kind === "plan", JSON.stringify(ov2.views[3]));
  const want3 = planText(ids[3], "down", ST.n2, "Niveau 2", 1200, { scale: 50 });
  const ovText2 = await q("open_views");
  check("open_views: the stored view's text", ovText2.includes(`,${want3},`), `\n  got  ${ovText2}\n  want ${want3}`);

  // ---- close ----
  check("close the current plan tab -> 200", (await tab("close", vtab(ids[3]))).status === 200);
  const ov3 = JSON.parse(await q("open_views"));
  check("closed: the last tab left (3D) is current", ov3.current?.id === V3D && ov3.active === V3D && ov3.tabs.length === 1, JSON.stringify({ c: ov3.current, t: ov3.tabs }));
  check("closed: change tab 3D, 1 tab", (await change()).tab?.id === V3D && (await change()).view[7] === 1);
  check("close the 3D tab -> 200", (await tab("close", vtab(V3D))).status === 200);
  const ov4 = JSON.parse(await q("open_views"));
  const c4 = await change();
  check("all closed: Home (current null, no tab)", ov4.current === null && ov4.tabs.length === 0 && c4.tab === null && c4.view[7] === 0, JSON.stringify({ ov4c: ov4.current, t: ov4.tabs, c4: c4.tab }));
  check("Home: view_clip null", (await q("view_clip")) === "null", await q("view_clip"));
  const re = await tab("activate", vtab(V3D));
  check("Home: activate 3D -> 400 no such open tab", re.status === 400 && re.text === "no such open tab", re.text);
  check("Home: open a ceiling plan -> 200, current", (await tab("open", vtab(ids[5]))).status === 200 && JSON.parse(await q("open_views")).current?.id === ids[5]);
  const vcu = JSON.parse(await q("view_clip"));
  check("ceiling plan: look up, z from the cut (2300) up to the level above (3000)",
    vcu.look === "up" && vcu.cut === 2300 && vcu.clip && vcu.clip.min[2] === Math.fround(2300 - origin[2]) && vcu.clip.max[2] === Math.fround(3000 - origin[2]), JSON.stringify(vcu));

  // ---- a new model: the 3D view only, current ----
  await call("action", "new", JSON.stringify({ discard: true }));
  const ovn = JSON.parse(await q("open_views"));
  check("new model: 3D + plan + ceiling of Level 0, 3D current", ovn.views.length === 3 && ovn.current?.id === V3D && ovn.tabs.length === 1 && ovn.views[1].name === "Plan Level 0", JSON.stringify(ovn.views.map((v) => v.name)));
  await call("action", "close", JSON.stringify({ discard: true }));
  const ovc = await q("open_views");
  check("no model: Rust's empty open_views", ovc === '{"active":null,"current":null,"paper":{"offered":false,"on":false},"tabs":[],"views":[]}', ovc);
} catch (e) {
  check("no exception", false, e.stack ?? String(e));
} finally {
  try { process.kill(-host.pid, "SIGKILL"); } catch {}
  rmSync(tmp, { recursive: true, force: true });
}
console.log(`views_test: ${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
