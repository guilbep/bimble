// The native app at national coordinates (ticket NUI3; the orchestrator's finding on the page, VIS2): node
// ccore/test/native_geo_test.mjs after `ccore/build.sh native-app` (NATIVE_BIN another binary).
// A georeferenced project: links placed 2,600 km from 0 (LV95's east and north in mm), one link in a local frame
// beside them. f32 steps by distance / 2^24: 155 mm there. The renderer draws relative to a scene origin (f64); that
// origin must sit where the triangles are. It used to be the first link ready: a small link at 0, ready first, put
// every other link at 155 mm steps (this test fails on that code). Checks:
//   1. the same synthetic building (a slab 18,300 mm long, 14 columns, two beams 15.6 m, a 12 m beam turned 30 degrees) linked
//      alone at 0 and, at 2,600 km, beside a small link at 0: the same picture (at most 0.2 % of pixels differ);
//   2. Measure on the building at 0 and far: 18,300 mm to 1.5 mm (far too: MEAS2 measures each link in its own frame);
//   3. the session log names the scene origin and the far link (f32 steps reported).
// Without the binary, SDL3 or a Vulkan device: "skipped: <why>"; CCORE_NATIVE_REQUIRED=1 makes that a failure.
import { spawnSync } from "node:child_process";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync, copyFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { readPng, inkShare, diffShare } from "./lib/png.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const BIN = process.env.NATIVE_BIN ?? join(CCORE, "out/app/bimble");
const REQUIRED = process.env.CCORE_NATIVE_REQUIRED === "1";
let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? `: ${detail}` : ""}`);
};
const skip = (why) => {
  if (REQUIRED) { console.log(`FAIL native geo: ${why} (CCORE_NATIVE_REQUIRED=1)`); process.exit(1); }
  console.log(`skipped: ${why}`);
  process.exit(0);
};
if (!existsSync(BIN)) skip(`${BIN} not built (ccore/build.sh native-app)`);
const tmp = mkdtempSync(join(process.env.NATIVE_TMP ?? tmpdir(), "nui3-geo-"));
const env = { ...process.env, BIMBLE_CONFIG_DIR: join(tmp, "conf"), BIMBLE_CACHE_DIR: join(tmp, "cache"), BIMBLE_LINK_CACHE_MIN: "0", LANG: "C" };

/** A synthetic IFC (mm): the building with its placements offset by (ox, oy, oz). */
function geoIfc(ox, oy, oz) {
  const L = ["ISO-10303-21;", "HEADER;", "FILE_DESCRIPTION(('x'),'2;1');", "FILE_NAME('x','t',(''),(''),'','','');", "FILE_SCHEMA(('IFC4'));", "ENDSEC;", "DATA;",
    "#1=IFCSIUNIT(*,.LENGTHUNIT.,.MILLI.,.METRE.);", "#2=IFCUNITASSIGNMENT((#1));", "#3=IFCCARTESIANPOINT((0.,0.,0.));", "#4=IFCAXIS2PLACEMENT3D(#3,$,$);",
    "#5=IFCGEOMETRICREPRESENTATIONCONTEXT($,'Model',3,1.E-05,#4,$);", "#6=IFCPROJECT('0YvctVUKr0kugbFTf53O9L',$,'p',$,$,$,$,(#5),#2);",
    "#10=IFCCARTESIANPOINT((0.,0.));", "#11=IFCAXIS2PLACEMENT2D(#10,$);", "#12=IFCDIRECTION((0.,0.,1.));"];
  let id = 100, n = 0;
  const gid = () => `2Geo${String(n++).padStart(4, "0")}VUKr0kugbFTf53O9`.slice(0, 22);
  const el = (x, y, z, w, d, h, ang = 0) => { // a w x d box, h high, its centre at (x, y) on z, turned by ang degrees
    const c = Math.cos((ang * Math.PI) / 180), s = Math.sin((ang * Math.PI) / 180);
    const p = id++, dx = id++, ap = id++, lp = id++, pr = id++, ex = id++, sr = id++, ps = id++, e = id++;
    L.push(`#${p}=IFCCARTESIANPOINT((${(ox + x).toFixed(1)},${(oy + y).toFixed(1)},${(oz + z).toFixed(1)}));`, `#${dx}=IFCDIRECTION((${c.toFixed(6)},${s.toFixed(6)},0.));`,
      `#${ap}=IFCAXIS2PLACEMENT3D(#${p},#12,#${dx});`, `#${lp}=IFCLOCALPLACEMENT($,#${ap});`, `#${pr}=IFCRECTANGLEPROFILEDEF(.AREA.,$,#11,${w}.,${d}.);`,
      `#${ex}=IFCEXTRUDEDAREASOLID(#${pr},#4,#12,${h}.);`, `#${sr}=IFCSHAPEREPRESENTATION(#5,'Body','SweptSolid',(#${ex}));`, `#${ps}=IFCPRODUCTDEFINITIONSHAPE($,$,(#${sr}));`,
      `#${e}=IFCBUILDINGELEMENTPROXY('${gid()}',$,'e${n}',$,$,#${lp},#${ps},$,$);`);
  };
  el(9150, 1000, 0, 18300, 2000, 200); // the slab: 18,300 mm along X
  for (let i = 0; i < 7; i++) for (let j = 0; j < 2; j++) el(1500 + i * 2500, 600 + j * 800, 200, 300, 300, 3000); // the corners left free (the measure)
  for (let j = 0; j < 2; j++) el(9150, 600 + j * 800, 3200, 15600, 300, 400);
  el(9000, 1000, 3600, 12000, 200, 300, 30); // the 12 m beam turned 30 degrees
  L.push("ENDSEC;", "END-ISO-10303-21;");
  return L.join("\n") + "\n";
}

function run(name, script, envx = {}) {
  const out = join(tmp, name);
  mkdirSync(out, { recursive: true });
  const sp = join(tmp, `${name}.txt`);
  writeFileSync(sp, script.trim() + "\n");
  const r = spawnSync(BIN, ["--offscreen", "--size", "1280x720", "--scale", "1", "--script", sp, "--out", out], { env: { ...env, ...envx }, encoding: "utf8", timeout: 180000 });
  return { code: r.status, signal: r.signal, err: (r.stderr ?? "") + (r.error ? String(r.error) : ""), out };
}

try {
  const probe = spawnSync(BIN, ["--offscreen", "--quit-after", "1", "--config-dir", join(tmp, "conf")], { env, encoding: "utf8", timeout: 60000 });
  if (probe.status === 3) skip(`no GPU device: ${(probe.stderr ?? "").split("\n")[0]}`);
  const FAR = [2600000000, 1200000000, 470000];
  const g0 = join(tmp, "geo0"), g1 = join(tmp, "geo1");
  mkdirSync(g0), mkdirSync(g1);
  writeFileSync(join(g0, "b-building.ifc"), geoIfc(0, 0, 0));
  writeFileSync(join(g1, "b-building.ifc"), geoIfc(...FAR));
  copyFileSync(join(CCORE, "test/fixtures/tilt.ifc"), join(g1, "a-local.ifc")); // a small link in its own frame, first by name
  const script = (dir, off) => `
open :new
wait 300
link_folder ${dir}
wait complete
eye a-local.ifc off
eye a-local off
frames 3
look_box 0.5 -2.5 4 0.5 0.5 0.2
frames 5
shot geo.png
action measure
frames 2
click_world ${off[0] + 40} ${off[1] + 40} ${off[2] + 200}
frames 2
click_world ${off[0] + 18260} ${off[1] + 40} ${off[2] + 200}
frames 3
expect_measure 18300 1.5
quit
`;
  const logs = join(tmp, "sessions");
  const r0 = run("geo-at-0", script(g0, [0, 0, 0]));
  const r1 = run("geo-far", script(g1, FAR), { BIMBLE_LOG_DIR: logs });
  for (const [nm, r] of [["geo-at-0", r0], ["geo-far", r1]]) {
    check(`${nm}: exits 0, no signal`, r.code === 0 && !r.signal, `${r.code} ${r.signal ?? ""} ${r.err.trim().split("\n").slice(-2).join(" | ")}`);
    for (const l of r.err.split("\n").filter((x) => /^drive: (ok|FAIL) /.test(x))) {
      const what = `${nm}: ${l.slice(7).replace(/^(ok|FAIL) /, "")}`;
      check(what, l.startsWith("drive: ok"));
    }
  }
  let share = 1, detail = "";
  try {
    const a = readPng(join(r0.out, "geo.png")), b = readPng(join(r1.out, "geo.png"));
    share = diffShare(a, b), detail = `${(share * 100).toFixed(3)} % of pixels differ; ink ${(inkShare(a) * 100).toFixed(1)} %`;
    check("geo: the picture at 0 is not blank", inkShare(a) > 0.02, detail);
  } catch (e) { detail = String(e); }
  check(`geo: the building at 2,600 km beside a local link draws as at 0 (${detail}; at most 0.2 %)`, share <= 0.002);
  const sl = existsSync(logs) ? readdirSync(logs).map((d) => join(logs, d, "session.log")).filter(existsSync).map((f) => readFileSync(f, "utf8")).join("\n") : "";
  check("geo: the session log names the scene origin and the far link's f32 steps", /the scene origin/.test(sl) && /km from the scene origin: its f32 positions step by/.test(sl),
    (sl.match(/.*scene origin.*/g) ?? []).slice(0, 3).join(" | "));
} finally {
  if (!process.env.NATIVE_KEEP) rmSync(tmp, { recursive: true, force: true });
  else console.log(`kept ${tmp}`);
}
console.log(`native geo: ${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
