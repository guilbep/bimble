// The native app `bimble` driven by its scripted driver (ticket NUI1, ADR 0084): node ccore/test/native_app_test.mjs
// after `ccore/build.sh native-app` (out/app/bimble; NATIVE_BIN another binary, e.g. the ASan one).
//   1. the fixture (golden/frame.json) opens offscreen: the picture is not blank and close to the committed
//      reference (results/native/fixture.png, within a share of pixels: another GPU rasterises edges a
//      little differently); a click on the top slab picks its GlobalId; hide, show all, isolate; the
//      section box on; Measure between two corners of that slab along X: 18 300 mm by hand (its outline
//      -150 .. 18 150), or 18 301.3 mm when the core snaps one end to the corner 220 mm below (the slab's
//      thickness: sqrt(18300² + 220²)), hence 18 300 +- 1.5; FR / EN; the Performance report names the renderer;
//   2. six links (the six synthetic IFC files of test/fixtures, cached: BIMBLE_LINK_CACHE_MIN=0) linked
//      as a folder: the five with geometry drawn in detail (cyclic_map.ifc has none), a click in a link;
//   3. every scripted input answered within 200 ms (ADR 0074; test/lib/input_time.mjs's rule: the app's
//      own time, event -> its UI and core work done, strict; event -> the GPU's fence strict on a real GPU,
//      reported and failing only above 1 s on a software rasteriser, lavapipe or llvmpipe);
//   4. malformed files (a truncated JSON, a garbage .bimbleview, a truncated IFC; a truncated IFC as a
//      link) end in the core's error, never a crash (an exit code, no signal).
// Without the binary, SDL3 or a Vulkan device: "skipped: <why>" (ci/run.sh full says so); with
// CCORE_NATIVE_REQUIRED=1 (ci/run.sh native, the CI job ccore-native) that is a failure.
// NATIVE_WRITE_REF=1 writes the reference picture instead of comparing.
import { execFileSync, spawnSync } from "node:child_process";
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { readPng, inkShare, diffShare } from "./lib/png.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const BIN = process.env.NATIVE_BIN ?? join(CCORE, "out/app/bimble");
const REQUIRED = process.env.CCORE_NATIVE_REQUIRED === "1";
const REF = join(CCORE, "results/native/fixture.png");
let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? `: ${detail}` : ""}`);
};
const skip = (why) => {
  if (REQUIRED) { console.log(`FAIL native app: ${why} (CCORE_NATIVE_REQUIRED=1)`); process.exit(1); }
  console.log(`skipped: ${why}`);
  process.exit(0);
};

if (!existsSync(BIN)) skip(`${BIN} not built (ccore/build.sh native-app: SDL3 >= 3.4, FreeType, glslc)`);
const tmp = mkdtempSync(join(process.env.NATIVE_TMP ?? tmpdir(), "nui1-test-"));
const conf = join(tmp, "conf");
const env = { ...process.env, BIMBLE_CONFIG_DIR: conf, BIMBLE_CACHE_DIR: join(tmp, "cache"), LANG: "fr_CH.UTF-8" };

/** Runs bimble offscreen with a script; returns { code, signal, err, out }. */
function run(name, script, file, extra = [], envx = {}) {
  const out = join(tmp, name);
  mkdirSync(out, { recursive: true });
  const sp = join(tmp, `${name}.txt`);
  writeFileSync(sp, script.trim() + "\n");
  const args = ["--offscreen", "--size", "1280x720", "--scale", "1", "--script", sp, "--out", out, ...extra];
  if (file) args.push(file);
  const r = spawnSync(BIN, args, { env: { ...env, ...envx }, encoding: "utf8", timeout: 180000 });
  return { code: r.status, signal: r.signal, err: (r.stderr ?? "") + (r.error ? String(r.error) : ""), out };
}
const lines = (err) => err.split("\n").filter((l) => l.startsWith("drive: "));
const expectations = (name, r) => {
  for (const l of lines(r.err)) if (/^drive: (ok|FAIL) /.test(l)) check(`${name}: ${l.slice(7).replace(/^(ok|FAIL) /, "")}`, l.startsWith("drive: ok"));
};
function inputs(name, r, software) {
  let rows = [];
  try { rows = readFileSync(join(r.out, "inputs.csv"), "utf8").trim().split("\n").slice(1).map((l) => l.split(",")); } catch {}
  const app = rows.map((x) => Number(x[2])), fr = rows.map((x) => Number(x[3]));
  const maxApp = Math.max(0, ...app), maxFrame = Math.max(0, ...fr), med = fr.sort((a, b) => a - b)[fr.length >> 1] ?? 0;
  check(`${name}: ${rows.length} inputs, event -> the app's work done within 200 ms (max ${maxApp.toFixed(1)} ms)`, rows.length > 0 && maxApp <= 200);
  const limit = software ? 1000 : 200;
  check(`${name}: event -> the frame's GPU fence within ${limit} ms (median ${med.toFixed(1)}, max ${maxFrame.toFixed(1)} ms${software ? ", software rasteriser: 200 ms reported, not asserted" : ""})`, maxFrame <= limit);
}

try {
  // ---- 0. does it start at all here (a GPU device)? ----
  const fixture = join(tmp, "frame.json");
  copyFileSync(join(CCORE, "golden/frame.json"), fixture);
  const probe = spawnSync(BIN, ["--offscreen", "--quit-after", "1", "--config-dir", conf, fixture], { env, encoding: "utf8", timeout: 60000 });
  if (probe.status === 3) skip(`no GPU device: ${(probe.stderr ?? "").split("\n")[0]}`);
  check("bimble starts offscreen on the fixture and exits 0", probe.status === 0, `${probe.status} ${probe.signal ?? ""} ${(probe.stderr ?? "").trim().split("\n").slice(-2).join(" | ")}`);

  // ---- 1. the fixture ----
  const TOP = "1khkLES9P8rPMAGC5wqOCk"; // slab D2, Niveau 2 (3000) + 2780; outline -150..18150 x -150..10150
  const r1 = run("fixture", `
wait complete
wait 200
shot fixture.png
click_world 9000 5000 5780
frames 3
expect_selected ${TOP}
shot picked.png
action hide_now
frames 2
expect_hidden 1
action show_all
frames 2
expect_hidden 0
click_world 9000 5000 5780
frames 3
action isolate_now
frames 2
expect_hidden -1
action show_all
frames 2
action section_box
frames 3
shot section-box.png
action section_box
action measure
frames 2
click_world -100 -100 5780
frames 2
click_world 18100 -100 5780
frames 3
expect_measure 18300 1.5
shot measure.png
key escape
lang en
frames 2
expect_text filemenu.label File
lang fr
frames 2
expect_text filemenu.label Fichier
expect_report renderer: native (SDL GPU / Vulkan)
report report.txt
quit
`, fixture);
  check("fixture: exits 0, no signal", r1.code === 0 && !r1.signal, `${r1.code} ${r1.signal ?? ""} ${r1.err.trim().split("\n").slice(-3).join(" | ")}`);
  expectations("fixture", r1);
  const report = existsSync(join(r1.out, "report.txt")) ? readFileSync(join(r1.out, "report.txt"), "utf8") : "";
  const adapter = (/^adapter: (.*)$/m.exec(report) ?? [])[1] ?? "?";
  const software = /llvmpipe|lavapipe|swiftshader|software/i.test(adapter);
  console.log(`adapter: ${adapter}${software ? " (software rasteriser)" : ""}`);
  check("fixture: the report has the renderer line", report.includes("renderer: native (SDL GPU / Vulkan)"));
  let img = null;
  try { img = readPng(join(r1.out, "fixture.png")); } catch (e) { check("fixture: the shot is a PNG", false, String(e)); }
  if (img) {
    const ink = inkShare(img, Math.floor(img.width * 0.3), Math.floor(img.height * 0.35), Math.floor(img.width * 0.95), Math.floor(img.height * 0.9));
    check(`fixture: the 3D view is not blank (${(ink * 100).toFixed(1)} % off its ground, over 2 %)`, ink > 0.02);
    if (process.env.NATIVE_WRITE_REF === "1") {
      mkdirSync(dirname(REF), { recursive: true });
      copyFileSync(join(r1.out, "fixture.png"), REF);
      console.log(`wrote ${REF}`);
    } else if (existsSync(REF)) {
      const share = diffShare(img, readPng(REF), 40);
      check(`fixture: close to the reference results/native/fixture.png (${(share * 100).toFixed(2)} % of pixels differ, at most 6 %)`, share <= 0.06);
    } else check("fixture: the reference results/native/fixture.png exists", false);
  }
  inputs("fixture", r1, software);

  // ---- 2. six links, cached (the lazy path: decoded by the loader, never by the session) ----
  const lk = join(tmp, "links");
  mkdirSync(lk);
  const IFC = ["booleans", "chain18", "curves", "cyclic_map", "mirror", "tilt"];
  for (const f of IFC) copyFileSync(join(CCORE, "test/fixtures", `${f}.ifc`), join(lk, `${f}.ifc`));
  const r2 = run("links6", `
open :new
wait 300
link_folder ${lk}
wait complete
frames 3
expect_parts 5
shot links6.png
drag 0.3 0.5 0.6 0.45 800
wheel 0.5 0.5 2
wheel 0.5 0.5 -1
rdrag 0.5 0.5 0.55 0.55 300
click 0.5 0.5
frames 3
key f
frames 2
report report.txt
quit
`, null, [], { BIMBLE_LINK_CACHE_MIN: "0" });
  check("links6: exits 0, no signal", r2.code === 0 && !r2.signal, `${r2.code} ${r2.signal ?? ""} ${r2.err.trim().split("\n").slice(-3).join(" | ")}`);
  expectations("links6", r2);
  const rep2 = existsSync(join(r2.out, "report.txt")) ? readFileSync(join(r2.out, "report.txt"), "utf8") : "";
  check("links6: the report lists 6 links, the 5 with elements in detail", /links \(6\):/.test(rep2) && (rep2.match(/detail loaded/g) ?? []).length === 5 && /0 elements, 0 triangles/.test(rep2), rep2.split("\n").filter((l) => /link/.test(l)).join(" | "));
  // a pick in a link: the centre of the view after the fit lands on one of them (or on nothing)
  const r3 = run("linkpick", `
open :new
wait 300
link_folder ${lk}
wait complete
action top
frames 3
click 0.5 0.5
frames 3
shot linkpick.png
quit
`, null, [], { BIMBLE_LINK_CACHE_MIN: "0" });
  check("linkpick: exits 0", r3.code === 0 && !r3.signal, `${r3.code} ${r3.signal ?? ""}`);
  inputs("links6", r2, software);

  // ---- 3b. NUI2: a tiling compositor's sizes (the window resized right after it maps, then smaller than the last
  // frame, then tiny), the target a frame after the window as on Wayland: no SDL assertion (SDL's own checks: the
  // viewport and scissor within the target), exit 0; the session log written (BIMBLE_LOG_DIR), --last-log names it;
  // Copy writes the report to a file too ----
  const logs = join(tmp, "sessions");
  const r5 = run("resize", `
wait complete
frames 25
shot resized.png
copy_report
frames 2
quit
`, fixture, ["--resize-script", "1280x720,1600x900,640x400,373x198,1920x1080,800x600,1280x720"], { BIMBLE_LOG_DIR: logs });
  check("resize: exits 0, no signal", r5.code === 0 && !r5.signal, `${r5.code} ${r5.signal ?? ""} ${r5.err.trim().split("\n").slice(-2).join(" | ")}`);
  check("resize: no SDL assertion (viewport and scissor within the target at every size)", !/SDL assertion/.test(r5.err), (r5.err.match(/SDL assertion.*/g) ?? []).slice(0, 2).join(" | "));
  expectations("resize", r5);
  const last = spawnSync(BIN, ["--last-log"], { env: { ...env, BIMBLE_LOG_DIR: logs }, encoding: "utf8" });
  const sdir = (last.stdout ?? "").trim();
  const slog = sdir && existsSync(join(sdir, "session.log")) ? readFileSync(join(sdir, "session.log"), "utf8") : "";
  check("session log: --last-log names the folder, session.log has the GPU, the sizes and the open", last.status === 0 && /GPU /.test(slog) && /sizes: swapchain 373x198/.test(slog) && /opened .*frame\.json/.test(slog),
    `${sdir} ${slog.split("\n").length} lines`);
  check("session log: frames.csv, report.txt and the copied report's file", existsSync(join(sdir, "frames.csv")) && existsSync(join(sdir, "report.txt")) && /drive: copy_report: .*report-.*\.txt/.test(r5.err),
    (r5.err.match(/drive: copy_report: .*/) ?? [""])[0].slice(0, 200));
  // the same sizes with no session log (the offscreen default) and with a folder it cannot write: no crash, no assertion
  // (a fault of the logging path at exit, found by ASan before 25d96e7, is what these guard)
  const ro = join(tmp, "readonly");
  mkdirSync(ro), execFileSync("chmod", ["0500", ro]);
  for (const [name, envx] of [["resize-nolog", {}], ["resize-rolog", { BIMBLE_LOG_DIR: join(ro, "sessions") }]]) {
    const rr = run(name, "wait complete\nframes 25\nreport report.txt\nquit\n", fixture, ["--resize-script", "1280x720,1600x900,640x400,373x198,1920x1080,800x600,1280x720"], envx);
    check(`${name}: exits 0, no signal, no SDL assertion`, rr.code === 0 && !rr.signal && !/SDL assertion/.test(rr.err), `${rr.code} ${rr.signal ?? ""} ${rr.err.trim().split("\n").slice(-2).join(" | ")}`);
  }
  execFileSync("chmod", ["0700", ro]);
  const small = spawnSync(BIN, ["--offscreen", "--size", "373x198", "--quit-after", "2", "--config-dir", conf, fixture], { env, encoding: "utf8", timeout: 60000 });
  check("--size 373x198 works (a window smaller than the panels)", small.status === 0, `${small.status} ${(small.stderr ?? "").trim().split("\n").slice(-1)}`);

  // ---- 3c. NUI2: the pointer tools at scale 1 and 2, after resizes, the view offset by the panels: the section box's
  // handles dragged (found where they are drawn: the owner could not size the box; every press was refused), Box to
  // selection, a pick, Measure, the view cube ----
  const pointer = `
wait complete
frames 3
action section_box
frames 5
expect_box on
box_handle 5 0 -60 400
frames 5
expect_box_moved 5 100
box_handle 1 80 0 400
frames 5
expect_box_moved 1 100
action section_box
frames 3
click_world 9000 5000 5780
frames 3
expect_selected ${TOP}
action box_to_selection
frames 3
expect_box on
key escape
frames 2
action measure
frames 2
click_world -100 -100 5780
frames 2
click_world 18100 -100 5780
frames 3
expect_measure 18300 1.5
key escape
frames 2
click_cube
frames 3
expect_view_changed
quit
`;
  for (const [name, extra] of [["pointer-s1", ["--scale", "1"]], ["pointer-s2", ["--scale", "2", "--size", "900x560"]], ["pointer-resized", ["--resize-script", "1280x720,1600x900,1100x700"]]]) {
    const rp = run(name, pointer, fixture, extra);
    check(`${name}: exits 0, no signal`, rp.code === 0 && !rp.signal, `${rp.code} ${rp.signal ?? ""} ${rp.err.trim().split("\n").slice(-2).join(" | ")}`);
    expectations(name, rp);
  }
  const rpl = run("pointer-links", `
open :new
wait 300
link_folder ${join(tmp, "links")}
wait complete
frames 3
action fit
frames 3
action section_box
frames 5
expect_box on
box_handle 5 0 -60 400
frames 5
expect_box_moved 5 50
quit
`, null, ["--scale", "2", "--size", "900x560"], { BIMBLE_LINK_CACHE_MIN: "0" });
  check("pointer-links: exits 0", rpl.code === 0 && !rpl.signal, `${rpl.code} ${rpl.signal ?? ""}`);
  expectations("pointer-links", rpl);

  // ---- 3c'. NUI2: Ctrl-click grows the selection (three elements), Hide hides the three ----
  const rms = run("ctrl-select", `
wait complete
frames 3
click_world 9000 5000 5780
frames 3
expect_nselected 1
ctrl_click_world 18150 10150 0
frames 3
expect_nselected 2
ctrl_click_world 18150 -150 0
frames 3
expect_nselected 3
action hide_now
frames 3
expect_hidden 3
quit
`, fixture);
  check("ctrl-select: exits 0", rms.code === 0 && !rms.signal, `${rms.code} ${rms.signal ?? ""}`);
  expectations("ctrl-select", rms);

  // ---- 3d. NUI2: the tile path (renderer_path=tiles) with occlusion, its truth test on (--truth: every 3D frame drawn
  // without, then with occlusion, the id and depth buffers compared: an element whose pixel shows something farther is
  // missing): the open model by tiles (a pick, hide), six links by tiles (a pick in a link, orbit, zoom); 0 missing ----
  const rt = run("tiles-fixture", `
wait complete
frames 3
click_world 9000 5000 5780
frames 3
expect_selected ${TOP}
action hide_now
frames 3
expect_hidden 1
action show_all
drag 0.3 0.5 0.6 0.45 600
wheel 0.5 0.5 2
frames 3
report report.txt
quit
`, fixture, ["--truth", "--set", "renderer_path=tiles"]);
  check("tiles-fixture: exits 0, no signal", rt.code === 0 && !rt.signal, `${rt.code} ${rt.signal ?? ""} ${rt.err.trim().split("\n").slice(-2).join(" | ")}`);
  expectations("tiles-fixture", rt);
  const truthOf = (r) => (/\| truth test: frames, with ids missing, ids missing \| (\d+) \/ (\d+) \/ (\d+) \|/.exec(existsSync(join(r.out, "summary.md")) ? readFileSync(join(r.out, "summary.md"), "utf8") : "") ?? []).slice(1).map(Number);
  const [tf, tmf, tmi] = truthOf(rt);
  check(`tiles-fixture: the truth test ran on every 3D frame and found nothing missing (${tf} frames, ${tmi} missing)`, tf > 5 && tmf === 0 && tmi === 0);
  const rtl = run("tiles-links", `
open :new
wait 300
link_folder ${join(tmp, "links")}
wait complete
frames 3
expect_parts 5
drag 0.3 0.5 0.6 0.45 800
wheel 0.5 0.5 2
wheel 0.5 0.5 -1
frames 3
report report.txt
quit
`, null, ["--truth", "--set", "renderer_path=tiles"], { BIMBLE_LINK_CACHE_MIN: "0" });
  check("tiles-links: exits 0, no signal", rtl.code === 0 && !rtl.signal, `${rtl.code} ${rtl.signal ?? ""} ${rtl.err.trim().split("\n").slice(-2).join(" | ")}`);
  expectations("tiles-links", rtl);
  const [lf, lmf, lmi] = truthOf(rtl);
  const rep3 = existsSync(join(rtl.out, "report.txt")) ? readFileSync(join(rtl.out, "report.txt"), "utf8") : "";
  check(`tiles-links: drawn by tiles, the truth test found nothing missing (${lf} frames, ${lmi} missing)`, /detail loaded by tiles/.test(rep3) && lf > 5 && lmf === 0 && lmi === 0);

  // ---- 3f. NUI3: the desktop: the window's title (the file's name), a touchpad's two-finger scroll pans, Ctrl + scroll and
  // a pinch zoom, the section box by numbers (its top at 4.5 m), the command palette (Ctrl+K), a file dropped on the window
  // opened (the title follows); the session log has the drop and the box ----
  const dlogs = join(tmp, "desk-sessions");
  const rd = run("desktop", `
wait complete
frames 3
expect_title frame.json
swheel 0.5 0.5 0.4 0.6
frames 3
expect_cam panned
swheel 0.5 0.5 0 0.5 ctrl
frames 3
expect_cam zoomed
pinch 1.3
frames 3
expect_cam zoomed
action section_box
frames 5
expect_box on
box_numbers -1 -1 -1 19 11 4.5
frames 5
expect_box_top 4.5
key k ctrl
frames 3
shot palette.png
key escape
frames 2
drop ${join(CCORE, "test/fixtures/tilt.ifc")}
wait complete
frames 5
expect_title tilt.ifc
quit
`, fixture, [], { BIMBLE_LOG_DIR: dlogs });
  check("desktop: exits 0, no signal", rd.code === 0 && !rd.signal, `${rd.code} ${rd.signal ?? ""} ${rd.err.trim().split("\n").slice(-2).join(" | ")}`);
  expectations("desktop", rd);
  const dl = existsSync(dlogs) ? readdirSync(dlogs).map((d) => join(dlogs, d, "session.log")).filter(existsSync).map((f) => readFileSync(f, "utf8")).join("\n") : "";
  check("desktop: the session log has the drop and the box by numbers", /drop: .*tilt\.ifc: opened/.test(dl) && /section box by numbers: .* -> 200/.test(dl), (dl.match(/(drop|section box by numbers).*/g) ?? []).join(" | ").slice(0, 300));

  // ---- 4. malformed files: the core's error, never a crash ----
  const bad = join(tmp, "bad");
  mkdirSync(bad);
  const text = readFileSync(join(CCORE, "golden/frame.json"), "utf8");
  writeFileSync(join(bad, "truncated.json"), text.slice(0, text.length >> 1));
  writeFileSync(join(bad, "garbage.bimbleview"), Buffer.from("BIMBLEVW\x01\x00\x00\x00garbage, not a view file".padEnd(4096, "\xff"), "latin1"));
  const ifc = readFileSync(join(CCORE, "test/fixtures/booleans.ifc"), "utf8");
  writeFileSync(join(bad, "truncated.ifc"), ifc.slice(0, ifc.length >> 1));
  for (const f of ["truncated.json", "garbage.bimbleview", "truncated.ifc"]) {
    const r = spawnSync(BIN, ["--offscreen", "--quit-after", "3", "--config-dir", conf, join(bad, f)], { env, encoding: "utf8", timeout: 60000 });
    const said = (r.stderr ?? "").split("\n").find((l) => l.includes(f)) ?? "";
    // the IFC reader keeps what it could read of a truncated file (opened, exit 0); the others are refused
    const lenient = f.endsWith(".ifc");
    check(`malformed ${f}: no crash (exit ${r.status}, signal ${r.signal ?? "none"})${lenient ? ", read as far as it goes" : ", the core's error said"}`,
      !r.signal && r.status !== null && r.status <= 1 && (lenient || said.length > 0), said.slice(0, 160));
  }
  const bl = join(tmp, "badlink");
  mkdirSync(bl);
  copyFileSync(join(bad, "truncated.ifc"), join(bl, "truncated.ifc"));
  copyFileSync(join(CCORE, "test/fixtures/tilt.ifc"), join(bl, "tilt.ifc"));
  const r4 = run("badlink", `
open :new
wait 300
link_folder ${bl}
wait complete
frames 3
report report.txt
quit
`, null, [], { BIMBLE_LINK_CACHE_MIN: "0" });
  check("a truncated IFC as a link: no crash, the other link drawn", !r4.signal && r4.code === 0 && /detail loaded/.test(existsSync(join(r4.out, "report.txt")) ? readFileSync(join(r4.out, "report.txt"), "utf8") : ""),
    `${r4.code} ${r4.signal ?? ""} ${r4.err.trim().split("\n").slice(-2).join(" | ")}`);
} catch (e) {
  check("no exception", false, e?.stack ?? String(e));
} finally {
  if (process.env.NATIVE_KEEP !== "1") rmSync(tmp, { recursive: true, force: true });
  else console.log(`kept ${tmp}`);
}
console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
