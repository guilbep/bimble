// Schedules in the C session (ticket Q2, session/schedules.h): node ccore/test/schedules_test.mjs
// Starts out/tcc/bimble-ccore (or $HOST_BIN) on a copy of golden/frame.json and checks, over HTTP,
// the viewer's calls (crates/bimble-viewer/src/calls.rs:399-490) against Rust's text:
//   query schedule_list: golden/schedule/session/list-frame.json (the ten built-in ones, struct
//     field order), list-changed.json after two set_schedule verbs;
//   query schedule_rows {name}: every built-in schedule of frame.json and frame2.json equals
//     golden/schedule/<frame>/<slug>.json (Q1's table_json, what the call answers), the changed
//     "Concrete columns" equals session/rows-changed-concrete-columns.json;
//   query schedule_csv {name}: {"csv": golden/schedule/<frame>/<slug>.csv}, and the whole answer
//     for Slabs equals session/csv-frame-slabs.json (serde_json's escaping);
//   errors: an unknown name "no schedule with this name" (also without a model: the name is
//     looked up first), a known one without a model "no model open", no name "missing field";
//   an IFC opened as it is (read-only): the built-in list, "All elements" lists its elements.
// On the code before Q2 every one of these calls answered [] or "query … is not in the C core".
// Prints "N passed, M failed"; exits non-zero on a failure. Kills what it starts.
import { spawn } from "node:child_process";
import { copyFileSync, mkdtempSync, readdirSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { port } from "./ports.mjs";
import { hostEnv } from "./lib/host_env.mjs"; // FIXSES: never the caller's BIMBLE_CONFIG_DIR

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const GOLDEN = join(CCORE, "golden");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("schedules_test");
const TOKEN = "schedulestest-token";
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
const q = (name, args) => call("query", name, args === undefined ? "" : JSON.stringify(args));
const usedSession = new Set(); // golden/schedule/session/ files compared (all must be)
const golden = (rel) => {
  if (rel.startsWith("schedule/session/")) usedSession.add(rel.slice(17));
  return readFileSync(join(GOLDEN, rel), "utf8");
};
const line = (rel) => golden(rel).replace(/\n$/, ""); // the goldens end with "\n"; the answers do not
const slug = (name) => name.toLowerCase().replaceAll(" ", "-");
const diff = (got, want) => {
  let i = 0;
  while (i < got.length && got[i] === want[i]) i++;
  return `first difference at ${i}:\n  got  …${got.slice(Math.max(0, i - 40), i + 60)}\n  want …${want.slice(Math.max(0, i - 40), i + 60)}`;
};
const same = (name, got, want) => check(name, got === want, got === want ? "" : diff(got, want));

const tmp = mkdtempSync(join(tmpdir(), "ccore-schedules-"));
const model = join(tmp, "frame.json");
copyFileSync(join(GOLDEN, "frame.json"), model);
copyFileSync(join(GOLDEN, "frame2.json"), join(tmp, "frame2.json"));
copyFileSync(join(GOLDEN, "ifc/fixtures/triangulated.ifc"), join(tmp, "triangulated.ifc"));
const host = spawn(BIN, [String(PORT), model], { env: hostEnv({ BIMBLE_TOKEN: TOKEN }), stdio: ["ignore", "ignore", "pipe"], detached: true });
let log = "";
host.stderr.on("data", (d) => (log += d));

/** Every effective schedule of the open frame: rows and CSV equal Rust's tables. */
async function tables(frame) {
  const list = JSON.parse((await q("schedule_list")).text);
  check(`${frame}: ten built-in schedules`, list.length === 10, String(list.length));
  for (const d of list) {
    const s = slug(d.name);
    const r = await q("schedule_rows", { name: d.name });
    check(`${frame} ${s}: rows 200`, r.status === 200, r.text);
    same(`${frame} ${s}: schedule_rows = golden/schedule/${frame}/${s}.json`, r.text, line(`schedule/${frame}/${s}.json`));
    const c = await q("schedule_csv", { name: d.name });
    check(`${frame} ${s}: csv 200`, c.status === 200, c.text);
    let csv = null;
    try { csv = JSON.parse(c.text).csv; } catch { /* checked below */ }
    same(`${frame} ${s}: schedule_csv.csv = golden/schedule/${frame}/${s}.csv`, csv ?? c.text, golden(`schedule/${frame}/${s}.csv`));
    check(`${frame} ${s}: one key, "csv"`, c.text.startsWith('{"csv":"') && c.text.endsWith('"}'), c.text.slice(0, 40));
  }
}

try {
  for (let i = 0; i < 400 && !log.includes("listening"); i++) await sleep(10);
  if (!log.includes("listening")) throw new Error(`host did not start: ${log}`);

  // ---- frame.json: the list, every table ----
  const l0 = await q("schedule_list");
  same("schedule_list = session/list-frame.json", l0.text, line("schedule/session/list-frame.json"));
  await tables("frame");
  same("schedule_csv Slabs = session/csv-frame-slabs.json (the whole answer)", (await q("schedule_csv", { name: "Slabs" })).text, line("schedule/session/csv-frame-slabs.json"));
  // The kept table follows the name: rows of another schedule at the same rev are its own.
  const a1 = await q("schedule_rows", { name: "Slabs" }), a2 = await q("schedule_rows", { name: "Footings" });
  check("same rev, another name: another table", a1.text !== a2.text && a2.text === line("schedule/frame/footings.json"));

  // ---- errors ----
  const e1 = await q("schedule_rows", { name: "Nope" });
  check("unknown name: 400 no schedule with this name", e1.status === 400 && e1.text === "no schedule with this name", `${e1.status} ${e1.text}`);
  const e2 = await q("schedule_rows", { name: "slabs" });
  check("the name is exact (calls.rs d.name == a.name): slabs is unknown", e2.status === 400 && e2.text === "no schedule with this name", `${e2.status} ${e2.text}`);
  const e3 = await q("schedule_csv", {});
  check("no name: 400 missing field `name`", e3.status === 400 && e3.text.startsWith("missing field `name`"), `${e3.status} ${e3.text}`);

  // ---- set_schedule: the stored list, the changed table ----
  const v1 = await call("verb", "set_schedule", JSON.stringify({ name: "Concrete columns", group_by_level: false, columns: ["level", "name", "material", "volume", "mass"] }));
  const v2 = await call("verb", "set_schedule", JSON.stringify({ name: "Steel beams", family: "timber" }));
  check("two set_schedule verbs", v1.status === 200 && v2.status === 200, `${v1.status} ${v1.text} ${v2.status} ${v2.text}`);
  same("schedule_list after = session/list-changed.json", (await q("schedule_list")).text, line("schedule/session/list-changed.json"));
  same("schedule_rows Concrete columns after = session/rows-changed-concrete-columns.json",
    (await q("schedule_rows", { name: "Concrete columns" })).text, line("schedule/session/rows-changed-concrete-columns.json"));
  await call("undo");
  await call("undo");
  same("undo twice: the table is the built-in one again", (await q("schedule_rows", { name: "Concrete columns" })).text, line("schedule/frame/concrete-columns.json"));

  // ---- frame2.json ----
  const o2 = await call("action", "open", JSON.stringify({ path: join(tmp, "frame2.json"), discard: true }));
  check("open frame2.json", o2.status === 200, o2.text);
  await tables("frame2");

  // ---- an IFC opened as it is (read-only): the built-in ones, the scene's elements ----
  const o3 = await call("action", "open", JSON.stringify({ path: join(tmp, "triangulated.ifc"), discard: true }));
  check("open an IFC read-only", o3.status === 200, o3.text);
  same("IFC: schedule_list is the built-in list", (await q("schedule_list")).text, line("schedule/session/list-frame.json"));
  const ri = await q("schedule_rows", { name: "All elements" });
  let ti = null;
  try { ti = JSON.parse(ri.text); } catch { /* below */ }
  const ids = ti ? ti.groups.flatMap((g) => g.ids) : [];
  check("IFC: All elements lists its elements (count > 0, one id per row)", ri.status === 200 && ti && ti.count > 0 && ids.length === ti.count && ids.every((x) => typeof x === "string" && x.length === 22), ri.text.slice(0, 200));

  // ---- nothing open ----
  await call("action", "close", JSON.stringify({ discard: true }));
  same("no model: schedule_list is the built-in list", (await q("schedule_list")).text, line("schedule/session/list-frame.json"));
  const e4 = await q("schedule_rows", { name: "Slabs" });
  check("no model: 400 no model open", e4.status === 400 && e4.text === "no model open", `${e4.status} ${e4.text}`);
  const e5 = await q("schedule_csv", { name: "Nope" });
  check("no model, unknown name: the name first", e5.status === 400 && e5.text === "no schedule with this name", `${e5.status} ${e5.text}`);
  const onDisk = readdirSync(join(GOLDEN, "schedule/session")).sort();
  check(`every file of golden/schedule/session/ compared (${onDisk.length} on disk)`, onDisk.every((f) => usedSession.has(f)), onDisk.filter((f) => !usedSession.has(f)).join(" "));
} catch (e) {
  check("no exception", false, e.stack ?? String(e));
} finally {
  try { process.kill(-host.pid, "SIGKILL"); } catch {}
  rmSync(tmp, { recursive: true, force: true });
}
console.log(`schedules_test: ${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
