// Draw only what can be seen (ticket VIS1, ADR 0078 amendment 7): LAB_GPU=vulkan CCORE_PORT_BASE=auto node ccore/test/lab_visible.mjs
// Six synthetic buildings (bench/buildings_synth.mjs: a shell around thousands of small inner elements a storey) linked
// into a new project on the native host, opened by tools/drive.mjs's Drive (Vite, ONE headless Chromium; LAB_GPU picks
// SwiftShader by default, the machine's GPU with vulkan). A sweep of 60 cameras (20 far: around the site at several
// pitches; 20 close: one building from 45 m; 20 inside a room), some reached by fast turns (moving frames at large steps,
// then the stop): at each, the still picture with "draw only what can be seen" against the same picture with every
// triangle drawn, and the ground truth (every shown element's faces drawn as ids offscreen and read back). Checks:
//   - never a missing element: every element with a pixel in the ground truth is drawn by the still frame (0 on every
//     camera); the picture with culling equals the one without (no pixel differs by more than 40 levels; at all: printed);
//   - culling is used (a pyramid of this camera) and leaves triangles out: far and close cameras submit fewer triangles;
//   - the moving frame's pyramid serves the first still frame after a stop (no frame without culling once settled);
//   - the setting (Settings, "Draw only what can be seen": on by default since VIS2) on culls, off leaves nothing out;
//   - the performance panel's Record gives the three numbers (drawn, left out, the triangles of what can be seen).
// VIS2: the buildings carry long members turned in every way (braces, tilted instanced beams, risers at 30 and 45
// degrees) and are placed 2,600 km from the project's origin (drive's --georef: georeferenced links in an empty project,
// where f32 steps are 256 mm), the class the page lost on a real project of links; the first still frame after fast turns
// is checked as the invariant that matters: it reuses the moving frame's pyramid unless something that makes it stale
// changed in between (a link's detail released, the links, the lists: then it is not used, never a stale one); links
// opened from their rows only (SIZE3's path) draw their copies as their own shapes.
// Fails on the code before VIS1: no bench.idCount, nothing left out; before VIS2: georeferenced links lose elements.
import { mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { Drive, GEOREF } from "../tools/drive.mjs";
import { LAB_GPU } from "./lib/chromium.mjs";
import { CCORE } from "./lib/lab.mjs";

let passed = 0, failed = 0;
const check = (name, ok, detail = "") => { if (ok) passed++; else failed++; console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? `: ${detail}` : ""}`); };
const root = mkdtempSync(join(process.env.VIS1_SCRATCH ?? tmpdir(), "ccore-lab-visible-"));
const d = new Drive({ links: 6, storeys: 2, georef: process.env.VIS2_GEOREF !== "0", gpu: LAB_GPU, size: "1280x800", dpr: 1, profile: false, quiet: true, scratchRoot: root, lock: process.env.LAB_LOCK === "1",
  hostBin: process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore") }); // (the gate builds the tcc host) // (LAB_LOCK=1: the shared measuring lock, flock -n)
d.dir = join(root, "out");
const bye = async (code) => { await d.close(); rmSync(root, { recursive: true, force: true }); process.exit(code); };
for (const sig of ["SIGINT", "SIGTERM", "SIGHUP"]) process.on(sig, () => bye(130));

// One camera: the picture without culling, then with it (frames until this camera's pyramid is used), the ground truth.
const CAMERA = String.raw`(async (setCam, fast) => {
  const g = window.__bbGl, b = g.bench, raf = () => new Promise((r) => requestAnimationFrame(r));
  const pix = () => b.read().rgba;
  g.noProxies = true; // (the culling's exactness: far cells' proxies at 1 px stand for elements the ground truth draws)
  setCam(b);
  if (fast) { for (let i = 0; i < 5; i++) { b.frame(0.3, true); await raf(); } for (let i = 0; i < 5; i++) { b.frame(-0.3, true); await raf(); } b.frame(0, true); for (let i = 0; i < 8; i++) await raf(); } // (the moving frame's pyramid: its fence answered within a few frames, more under load)
  let usedFirst = null, whyFirst = null;
  if (fast) { g.visible = true; b.frame(0, false); usedFirst = g.vis ? g.vis.used : null; whyFirst = g.vis ? g.vis.why ?? null : null; }
  g.visible = false; b.frame(0, false);
  const all = { tris: g.frameTris, px: pix(), outlines: g.moving ? g.moving.outlines : null };
  g.visible = true;
  let used = false;
  for (let i = 0; i < 6 && !used; i++) { await raf(); await raf(); b.frame(0, false); used = !!(g.vis && g.vis.used); }
  const vis = { tris: g.frameTris, px: pix(), hidden: g.vis ? g.vis.hiddenTris : 0 };
  let changed = 0, any = 0;
  for (let k = 0; k < all.px.length; k += 4) { const dd = Math.max(Math.abs(all.px[k] - vis.px[k]), Math.abs(all.px[k + 1] - vis.px[k + 1]), Math.abs(all.px[k + 2] - vis.px[k + 2])); if (dd > 40) changed++; if (dd > 0) any++; }
  const truth = b.idCount();
  b.frame(0, false);
  const missing = b.missing();
  delete g.noProxies;
  return { allOutlines: all.outlines, visOutlines: g.moving ? g.moving.outlines : null, used, usedFirst, whyFirst, allTris: all.tris, visTris: vis.tris, hidden: vis.hidden, changed, any, seen: truth.elements, seenTris: truth.tris, missing: missing ? missing.n : -1, ids: missing ? missing.first : [] };
})`;

async function main() {
  await d.open();
  await d.gesture(["wait ready", "wait idle"]);
  const W = 36000, D = 24000, H = 3200, STEP_X = W + 14000, STEP_Y = D + 16000;
  const [GX, GY, GZ] = process.env.VIS2_GEOREF !== "0" ? GEOREF : [0, 0, 0]; // VIS2 (VIS2_GEOREF=0: at the origin)
  const cams = [];
  for (let i = 0; i < 20; i++) cams.push({ kind: "far", fast: i % 5 === 4, js: `(b) => { b.snap(${(i * 2 * Math.PI) / 20}, ${[-0.2, 0, 0.25, 0.45][i % 4]}); }` });
  for (let i = 0; i < 20; i++) {
    const k = i % 6, cx = STEP_X * (k % 6) + W / 2, cy = STEP_Y * Math.floor(k / 6) + D / 2;
    cams.push({ kind: "close", fast: i % 5 === 2, js: `(b) => { b.look([${GX + cx}, ${GY + cy}, ${GZ + H}], 45000); b.frame(${(i * 2 * Math.PI) / 20}, false); }` });
  }
  for (let i = 0; i < 20; i++) {
    const k = i % 3, x = 3000 + 6000 * (i % 6), y = i % 2 ? 5000 : 19000;
    cams.push({ kind: "inside", fast: i % 5 === 1, js: `(b) => { b.look([${GX + STEP_X * k + x}, ${GY + y}, ${GZ + H + 1500}], 1500); b.frame(${(i * 2 * Math.PI) / 20}, false); }` });
  }
  // The exactness sweep needs the machine's GPU: SwiftShader rasterises a few pixels differently from the pyramid's test
  // (1 element, 9 pixels on one inside camera, 27 Nov), so on a CPU rasteriser it is a declared skip, not a pass.
  const sweep = LAB_GPU !== "software";
  if (!sweep) console.log("skipped: the 60-camera exactness sweep (it needs the machine's GPU: LAB_GPU=vulkan); the settings and the report are checked");
  let missing = 0, changed = 0, any = 0, usedN = 0, firstUsed = 0, fastN = 0, firstStale = 0;
  const sum = { far: [0, 0, 0], close: [0, 0, 0], inside: [0, 0, 0] };
  for (const [i, c] of (sweep ? cams : []).entries()) {
    const r = await d.ev(`${CAMERA}(${c.js}, ${c.fast})`);
    missing += Math.max(0, r.missing), changed += r.changed, any += r.any, usedN += r.used ? 1 : 0;
    if (c.fast) { fastN++, firstUsed += r.usedFirst ? 1 : 0; if (!r.usedFirst) { console.log(`  camera ${i} (${c.kind}, after fast turns): the first still frame without the moving frame's pyramid: ${r.whyFirst}`); if (/^(detail|links|lists|shown|box|size)(,(detail|links|lists|shown|box|size))*( \(pending\))?$/.test(r.whyFirst ?? "")) firstStale++; } }
    sum[c.kind][0] += r.allTris, sum[c.kind][1] += r.visTris, sum[c.kind][2] += r.seenTris;
    if (r.missing !== 0 || r.changed > 0) console.log(`  camera ${i} (${c.kind}${c.fast ? ", after fast turns" : ""}): missing ${r.missing} ${JSON.stringify(r.ids)}, pixels > 40: ${r.changed} (triangles all ${r.allTris} / culled ${r.visTris}; outlines ${r.allOutlines} / ${r.visOutlines})`);
  }
  if (sweep) check("60 cameras: every element seen in the ground truth is drawn (0 missing)", missing === 0, `${missing} missing`);
  if (sweep) check("60 cameras: the picture with culling equals the one without (0 pixels over 40 levels)", changed === 0, `${changed} over 40, ${any} at all`);
  if (sweep) check("culling used on at least 50 of 60 cameras (this camera's pyramid)", usedN >= 50, `${usedN}`);
  // VIS2: the invariant: the moving frame's pyramid is reused unless something that makes it stale changed between them (a
  // link's detail released, the links, the lists, what is shown: never a stale pyramid, and the sweep's 0 missing shows none
  // was); not yet back, or not reused with nothing changed, fails. (A link's detail coming back no longer makes it stale:
  // the cause of 11 of 12 on 27 Nov.)
  if (sweep) check("after fast turns, the first still frame reuses the moving frame's pyramid whenever nothing that makes it stale changed", firstUsed + firstStale === fastN, `${firstUsed} of ${fastN} reused, ${firstStale} stale (not used, the safe behaviour)`);
  if (sweep) for (const k of ["far", "close", "inside"]) console.log(`  ${k}: submitted ${sum[k][0].toLocaleString("en-US")} -> ${sum[k][1].toLocaleString("en-US")} triangles; the seen elements' ${sum[k][2].toLocaleString("en-US")}`);
  // the far view with its proxies (1 px, still) against its full detail: a picture within a stated bound (proxies are 3D2's,
  // not this ticket's; the culling's truth is the sweep above, without them)
  const prx = await d.ev(`(async () => { const g = window.__bbGl, b = g.bench, raf = () => new Promise((r) => requestAnimationFrame(r)); b.snap(0, 0);
    g.noProxies = true; for (let i = 0; i < 3; i++) { await raf(); b.frame(0, false); } const a = b.read().rgba;
    delete g.noProxies; for (let i = 0; i < 3; i++) { await raf(); b.frame(0, false); } const c = b.read().rgba; let n = 0, ink = 0;
    for (let k = 0; k < a.length; k += 4) { if (Math.max(Math.abs(a[k] - c[k]), Math.abs(a[k + 1] - c[k + 1]), Math.abs(a[k + 2] - c[k + 2])) > 40) n++; if (a[k] < 235) ink++; }
    return { changed: n, ink, cells: (g.linkProxies || []).reduce((t, x) => t + x.drawnCells, 0) }; })()`);
  check("the far view with its proxies differs from its full detail on at most 5 % of the drawn pixels (measured 2.3 % on 27 Nov: 3D2's 1 px proxies)", prx.changed <= 0.05 * Math.max(1, prx.ink), `${prx.changed} of ${prx.ink} drawn pixels over 40 levels, ${prx.cells} proxy cells`);
  if (sweep) check("far and close cameras submit fewer triangles with culling", sum.far[1] < 0.8 * sum.far[0] && sum.close[1] < 0.5 * sum.close[0], `far ${sum.far[1]}/${sum.far[0]}, close ${sum.close[1]}/${sum.close[0]}`);

  // the setting off from the Settings dialog (File › Settings): nothing left out; on again
  const toggle = () => d.ev(`(async () => { const raf = () => new Promise((r) => requestAnimationFrame(r)); if (!document.querySelector('[data-bb=settings-draw-visible]')) { document.querySelector('[data-bb=quickbar-file]').click(); await raf(); await raf(); document.querySelector('[data-bb=file-menu-settings]').click(); for (let i = 0; i < 20 && !document.querySelector('[data-bb=settings-draw-visible]'); i++) await raf(); }
    const c = document.querySelector('[data-bb=settings-draw-visible]'); if (!c) return null; c.click(); for (let i = 0; i < 10; i++) await raf(); return c.checked; })()`);
  // (frames until the state read is there: under load the new camera's pyramid comes back after more frames than 4, its
  // fence polled each frame; on, it must come within 120 frames; off, nothing may be left out from the first frame on)
  const r1q = `(async () => { const g = window.__bbGl, b = g.bench, raf = () => new Promise((r) => requestAnimationFrame(r)); delete g.visible; b.snap(0, 0);
    let i = 0; for (; i < 120; i++) { await raf(); b.frame(0, false); if (i >= 3 && (!g.vis || !g.vis.on || g.vis.used)) break; }
    return { used: g.vis ? g.vis.used : null, hidden: g.vis ? g.vis.hiddenTris : null, on: g.vis ? g.vis.on : null, why: g.vis ? g.vis.why ?? null : null, frames: i + 1 }; })()`;
  const rDef = await d.ev(r1q);
  check("VIS2: on by default: culling used, triangles left out", rDef.on === true && rDef.used === true && rDef.hidden > 0, JSON.stringify(rDef));
  const offNow = await toggle();
  check("Settings: Draw only what can be seen, on by default (VIS2), turned off", offNow === false, String(offNow));
  const r0 = await d.ev(r1q);
  check("setting off: nothing left out", r0.used === false && r0.hidden === 0 && r0.on === false, JSON.stringify(r0));
  const onNow = await toggle();
  check("Settings: turned on again", onNow === true, String(onNow));
  const r1 = await d.ev(r1q);
  check("setting on: culling used, triangles left out", r1.on === true && r1.used === true && r1.hidden > 0, JSON.stringify(r1));
  await d.ev(`(() => { document.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape", bubbles: true })); return 0; })()`);
  await d.gesture(["sleep 300"]);

  // the panel's numbers, behind Record
  const rep = await d.ev(`(async () => { const g = window.__bbGl, b = g.bench, raf = () => new Promise((r) => requestAnimationFrame(r)); g.visible = true; b.snap(0, 0); for (let i = 0; i < 40 && !(g.vis && g.vis.used && i >= 3); i++) { await raf(); await raf(); b.frame(0, false); } window.__bbPerf.record(); return window.__bbPerf.report(false, null); })()`);
  const seen = /drawn ([\d,]+) triangles; ([\d,]+) belong to the ([\d,]+) elements you can see/.exec(rep), hid = /left out as hidden behind the last picture: ([\d,]+) triangles/.exec(rep);
  check("the report: drawn, left out as hidden, and the triangles of what can be seen", !!seen && !!hid && Number(hid[1].replace(/,/g, "")) > 0, `${seen?.[0]} | ${hid?.[0]}`);
  // (last: it leaves the links released, drawn by their proxies)
  // VIS2: the links opened from their rows and boxes only (the open's decoded budget at 0: SIZE3's path on a large
  // project), their detail read again when wanted: the copies are drawn as their own shapes (their groups once kept the
  // offset 0 of shapes packed after them: every copy drew its link's first shape) and nothing seen is missing
  if (sweep) {
    const CAM = `(b) => { b.look([${GX + STEP_X + W / 2}, ${GY + D / 2}, ${GZ + H}], 45000); b.frame(0.7, false); }`;
    const shot = () => d.ev(`(async () => { const g = window.__bbGl, b = g.bench, raf = () => new Promise((r) => requestAnimationFrame(r)); g.noProxies = true; g.visible = false; (${CAM})(b); for (let i = 0; i < 4; i++) { await raf(); b.frame(0, false); } const px = Array.from(b.read().rgba); delete g.visible; delete g.noProxies; return px; })()`);
    const before = await shot();
    const f0 = await d.ev("window.__bbGl.meshFetches");
    await d.ev("(window.__bbGl.openKeepBytes = 0, delete window.__bbGl.openRows, 0)");
    await d.hcall("action", "manage_links", { op: "reload_all" });
    const t0 = Date.now();
    const ready = async () => { const g = await d.ev("({ f: window.__bbGl.meshFetches, kept: window.__bbGl.openKeptBytes, at: window.__bbGl.linksDetailedAt, meshAt: window.__bbGl.meshAt })"); return g.f > f0 && g.kept === 0 && typeof g.at === "number" && g.at > g.meshAt; };
    while (Date.now() - t0 < 120000 && !(await ready())) await new Promise((r) => setTimeout(r, 300));
    await d.gesture(["sleep 1500", "wait idle"]);
    await d.ev(`(async () => { const b = window.__bbGl.bench, raf = () => new Promise((r) => requestAnimationFrame(r)); (${CAM})(b); for (let i = 0; i < 30; i++) { await raf(); b.frame(0, false); } return 0; })()`); // (the links in view asked again)
    await d.gesture(["sleep 1500", "wait idle"]);
    const after = await shot();
    let diff = 0;
    for (let k = 0; k < before.length; k += 4) if (Math.max(Math.abs(before[k] - after[k]), Math.abs(before[k + 1] - after[k + 1]), Math.abs(before[k + 2] - after[k + 2])) > 40) diff++;
    const r = await d.ev(`${CAMERA}(${CAM}, false)`);
    check("VIS2: links opened from their rows only, their detail back: the same picture as opened whole, copies as their own shapes, 0 missing", diff === 0 && r.missing === 0 && r.changed === 0, `${diff} pixels differ from the open keeping them whole; missing ${r.missing}, culled picture ${r.changed} pixels off`);
    await d.ev("(delete window.__bbGl.openKeepBytes, 0)");
  }
  console.log(`lab_visible: ${passed} passed, ${failed} failed (LAB_GPU=${LAB_GPU}, ${d.gpu?.renderer})`);
  await bye(failed ? 1 : 0);
}
main().catch(async (e) => { console.error(e); await bye(1); });
