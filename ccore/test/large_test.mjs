// The large-model generator and benchmark, small (ticket T16): node ccore/test/large_test.mjs
// Generates bench/large_model.py at N = 300 into out/ (twice: same bytes), checks the GlobalIds
// (22 chars, IFC alphabet, first char 0-3, unique; storeys, materials, elements), starts
// out/tcc/bimble-ccore (or $HOST_BIN) on it (47375): it loads (the C loader validates), `query
// elements` has every row, GET /mesh has one row per element, a rotated wall and a polygon with
// voids are there, place + undo work. Then runs bench/large.mjs on that size (host part only,
// RUNS=1, BINS=tcc; ports 47372-47373) and checks its JSON. Prints "N passed, M failed"; exits
// non-zero on a failure. Kills all it starts.
import { execFileSync, spawn } from "node:child_process";
import { readFileSync, rmSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const OUT = join(CCORE, "out");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const N = 300;
const PORT = 47375;
const TOKEN = "large-test";
const A = join(OUT, `t16-large-${N}.bimble`), B = join(OUT, `t16-large-${N}-again.bimble`);
let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? `: ${detail}` : ""}`);
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let host = null;

try {
  const info = JSON.parse(execFileSync("python3", [join(CCORE, "bench/large_model.py"), String(N), A]).toString());
  execFileSync("python3", [join(CCORE, "bench/large_model.py"), String(N), B]);
  check("deterministic: two runs, same bytes", readFileSync(A).equals(readFileSync(B)));
  rmSync(B, { force: true });
  check(`about ${N} elements (within 1.5 %)`, Math.abs(info.elements - N) <= 0.015 * N, JSON.stringify(info));
  const doc = JSON.parse(readFileSync(A, "utf8"));
  const m = doc.model;
  const ids = [m.project_id, ...Object.keys(m.storeys), ...Object.keys(m.materials), ...Object.keys(m.elements)];
  check("GlobalIds valid (22 chars, IFC alphabet, first char 0-3)", ids.every((s) => /^[0-3][0-9A-Za-z_$]{21}$/.test(s)));
  check("GlobalIds unique", new Set(ids).size === ids.length, `${new Set(ids).size} / ${ids.length}`);
  const els = Object.values(m.elements);
  check("map keys = ids", Object.entries(m.elements).every(([k, e]) => k === e.id));
  check("every class: footing, column, beam, slab, wall, proxy",
    ["IfcFooting", "IfcColumn", "IfcBeam", "IfcSlab", "IfcWall", "IfcBuildingElementProxy"].every((c) => els.some((e) => e.class === c)));
  check("rotated walls (x axis 0.6, 0.8)", els.some((e) => e.class === "IfcWall" && e.placement.x_axis[0] === 0.6 && e.placement.x_axis[1] === 0.8));
  check("extruded polygons with two voids", els.some((e) => e.generator.kind === "extruded_polygon" && e.generator.voids.length === 2));

  host = spawn(BIN, [String(PORT), A], { stdio: ["ignore", "ignore", "pipe"], env: { ...process.env, BIMBLE_TOKEN: TOKEN } });
  host.log = "";
  host.stderr.on("data", (d) => (host.log += d));
  for (let i = 0; i < 1000 && !host.log.includes("listening") && host.exitCode === null; i++) await sleep(5);
  check("the C host loads and validates it (listening)", host.log.includes("listening"), host.log.trim());
  const base = `http://127.0.0.1:${PORT}`;
  const post = async (path, body = "") => {
    const r = await fetch(`${base}${path}?t=${TOKEN}`, { method: "POST", body, headers: { "Content-Type": "text/plain" } });
    return { status: r.status, body: await r.text() };
  };
  const rows = JSON.parse((await post("/query/elements", JSON.stringify({ limit: 10000 }))).body);
  check("query elements: every element", rows.length === els.length, `${rows.length} / ${els.length}`);
  const mesh = Buffer.from(await (await fetch(`${base}/mesh?t=${TOKEN}`)).arrayBuffer());
  const nE = mesh.readUInt32LE(12);
  check("GET /mesh: BMSH, one row per element", mesh.subarray(0, 4).toString() === "BMSH" && nE === els.length, `${nE}`);
  const storey = Object.values(m.storeys).sort((a, b) => a.elevation - b.elevation)[1].id;
  const pl = await post("/verb/place", JSON.stringify({ a: [3000.0, 2500.0], kind: "column", storey }));
  check("verb place -> 200", pl.status === 200, pl.body);
  const un = await post("/undo/-");
  const h = JSON.parse((await post("/query/history")).body);
  check("undo -> 200, nothing left to undo", un.status === 200 && h.undo.length === 0 && h.redo.length === 1, JSON.stringify(h));
  host.kill("SIGTERM");
  await sleep(100);

  const raw = join(OUT, "t16-large-test.json");
  let benchOk = true, out = "";
  try {
    out = execFileSync("node", [join(CCORE, "bench/large.mjs")], {
      env: { ...process.env, SIZES: String(N), BINS: "tcc", RUNS: "1", LAB: "0", RAW: raw }, timeout: 120000,
    }).toString();
  } catch (e) {
    benchOk = false;
    out = `${e.stdout ?? ""}${e.stderr ?? ""}`;
  }
  check("bench/large.mjs (host part, N=300, tcc) exits 0", benchOk, out.trim().split("\n").at(-1));
  if (benchOk) {
    const r = JSON.parse(readFileSync(raw, "utf8")).sizes[N].host.tcc;
    check("bench JSON: start, mesh, session numbers", r.start_ms.median > 0 && r.mesh.bytes === mesh.length && r.session.file_bytes > 0 && r.session.restart_ms > 0,
      JSON.stringify({ start: r.start_ms.median, mesh: r.mesh, session: r.session }));
  }
} catch (e) {
  check("no exception", false, e.stack ?? String(e));
} finally {
  if (host && host.exitCode === null) host.kill("SIGKILL");
}
console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
