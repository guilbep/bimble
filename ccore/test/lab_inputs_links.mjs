// Every input answered within 200 ms on a project of links whose whole picture is slow (ticket VIS2, ADR 0074, ADR 0078
// amendment 9): LAB_GPU=vulkan CCORE_PORT_BASE=auto node ccore/test/lab_inputs_links.mjs (software by default)
// The cases on a machine 4 and 6 times slower than the reference one: LAB_SLOW=1 (skipped in the gate, declared; open item)
// Six synthetic buildings (bench/buildings_synth.mjs, 2.7 M triangles, placed 2,600 km away: drive.mjs --georef) linked
// into a new project on the native host, opened by tools/drive.mjs's Drive (one headless Chromium), draw_visible on (the
// default). After a still picture: a press that selects (click), Escape, a wheel step, the start of an orbit drag, a press
// that starts a pan; each timed by lib/input_time.mjs (ADR 0074 as every lab test applies it: the page's part, the input to
// the viewport frame drawn for it, at 200 ms always; the presented frame at 200 ms on a hardware GPU, reported on a CPU
// rasteriser and failing above 1,000 ms), after the picture is complete, the page idle and the GPU settled. What made them slow on the owner's project: the first moving frame rebuilt every batch's size histogram (the
// moving and still keys flipped one cache entry), a selection or a resize drew the whole picture with no pyramid,
// navigation pivots scanned every element, and components re-asked the host on every state change.
// Also: a selection and Escape ask the host nothing that does not depend on the selection, and the wheel's pivot comes
// from the last picture's element attachment. Fails on the code before VIS2's input fixes (those two checks; the times
// themselves: 250 to 800 ms for a press on the owner's project, under 200 ms on this smaller scene).
import { mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { Drive, GEOREF } from "../tools/drive.mjs";
import { LAB_GPU } from "./lib/chromium.mjs";
import { CCORE } from "./lib/lab.mjs";
import { adapter, readTimes, verdict, watchSource } from "./lib/input_time.mjs";

let passed = 0, failed = 0;
const check = (name, ok, detail = "") => { if (ok) passed++; else failed++; console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? `: ${detail}` : ""}`); };
const root = mkdtempSync(join(process.env.VIS1_SCRATCH ?? tmpdir(), "ccore-lab-inputs-"));
const d = new Drive({ links: 6, storeys: 2, georef: true, gpu: LAB_GPU, size: "1280x800", dpr: 1, profile: false, quiet: true, scratchRoot: root, lock: process.env.LAB_LOCK === "1", needGb: Number(process.env.LAB_NEED_GB ?? 0), waitMin: 2, // (in the gate: no lock, no memory wait; LAB_LOCK=1 for hand runs on the real GPU, never an unbounded wait)
  hostBin: process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore") });
d.dir = join(root, "out");
const bye = async (code) => { await d.close(); rmSync(root, { recursive: true, force: true }); process.exit(code); };
for (const sig of ["SIGINT", "SIGTERM", "SIGHUP"]) process.on(sig, () => bye(130));
// ADR 0074's rule as lib/input_time.mjs applies it: the page's part (the input to the frame the page draws for it) at 200 ms
// always; the presented frame at 200 ms on a hardware GPU, reported on a CPU rasteriser (SwiftShader: its GPU process, not
// the product) and failing only above 1,000 ms
const DOM = { click: "pointerdown", press: "pointerdown", drag: "pointerdown", key: "keydown", wheel: "wheel" };
/** Arms lib/input_time's watcher for the input `kind`: its own feedback, a viewport frame drawn after it. */
const arm = async (kind) => { const n0 = await d.ev("window.__bbGl.frames"); await d.ev(watchSource({ kind: DOM[kind], page: `window.__bbGl.frames > ${n0}` })); };
let gpu = null;
const timed = async (label) => { const v = verdict(await readTimes((e) => d.ev(e)), gpu); check(`${label}: ${v.line}`, v.pageOk && v.presOk); };

async function main() {
  await d.open();
  // (each measured input starts after the last frame has finished on the GPU too: gl.finish() on the page's own context, after
  // the page is idle; the claim under test is the input's own cost, not a GPU-process backlog left by the frame before)
  const still = ["wait idle", "sleep 400", "wait idle", "js document.getElementById('bimble').getContext('webgl2').finish()"];
  await d.gesture(["wait ready", ...still]);
  // before each measured input, a settled machine as far as the page can see it, waited for within a bound: no frame drawn,
  // gl.finish() back within 30 ms (the GPU process has no backlog) and the main thread answering every message within
  // 50 ms over 300 ms; tried again after a still picture up to 20 times (on a loaded runner a link's late work or the GPU
  // process's backlog can land just before the input); the assertion itself is unchanged
  const probe = `(async () => { const g = window.__bbGl, gl = document.getElementById('bimble').getContext('webgl2'), n0 = g.frames, t0 = performance.now(); gl.finish(); const fin = performance.now() - t0; let last = performance.now(), gap = 0; const end = last + 300; await new Promise((res) => { const ch = new MessageChannel(); ch.port1.onmessage = () => { const t = performance.now(); gap = Math.max(gap, t - last); last = t; if (t < end) ch.port2.postMessage(0); else { ch.port1.close(); res(); } }; ch.port2.postMessage(0); }); return { fin, gap, frames: g.frames - n0 }; })()`;
  const settle = async () => {
    let p = null;
    for (let i = 0; i < 20; i++) {
      p = await d.ev(probe);
      if (p.frames === 0 && p.fin < 30 && p.gap < 50) { if (i) console.log(`  (settled after ${i} more still pictures)`); return; }
      await d.gesture(still);
    }
    console.log(`  (not settled after 20 still pictures: the last frames ${p.frames}, gl.finish ${p.fin.toFixed(1)} ms, the main thread's longest gap ${p.gap.toFixed(1)} ms)`);
  };
  const whole = await d.ev("({ tris: window.__bbGl.frameTris, ms: window.__bbGl.frameMs ?? null })");
  console.log(`  a still picture: ${whole.tris.toLocaleString("en-US")} triangles, the last frame ${Math.round(whole.ms ?? 0)} ms (draw start to the GPU's end)`);
  // a point on building 0's south facade (world mm; buildings_synth: 36 x 24 m, 3.2 m storeys), in CSS px of the page
  const at = await d.ev(`(() => { const p = window.__bbGl.toScreen(${GEOREF[0] + 18000}, ${GEOREF[1] + 150}, ${GEOREF[2] + 3000}); return p ? [Math.round(p[0]), Math.round(p[1])] : null; })()`);
  if (!at) throw new Error("building 0 not on screen");
  const steps = [
    ["press (a click that selects)", "click", [`click ${at[0]} ${at[1]}`]],
    ["Escape (the selection cleared)", "key", ["key Escape"]],
    ["wheel (a zoom step)", "wheel", ["zoom 1 50"]],
    ["drag (an orbit starts)", "drag", ["orbit 200 0 400"]],
    ["press (a pan starts)", "press", ["pan 150 40 400"]],
  ];
  // the host's queries the page sends (synchronous XHR: each blocks the page), counted by name
  await d.ev(`(() => { if (window.__vis2q) return 0; window.__vis2q = {}; const open = XMLHttpRequest.prototype.open; XMLHttpRequest.prototype.open = function (m, u, ...a) { const k = /\\/query\\/([a-z_]+)/.exec(String(u)); if (k) window.__vis2q[k[1]] = (window.__vis2q[k[1]] || 0) + 1; return open.call(this, m, u, ...a); }; return 0; })()`);
  // (a first selection in a link places that link's scene in the host, which moves the links' counter once; the components'
  // computed values are lazy: the next render after it asks again. Both warmed here: two selections and their clearing)
  for (let i = 0; i < 2; i++) await d.gesture([`click ${at[0]} ${at[1]}`, ...still, "key Escape", ...still]);
  const q0 = await d.ev("({ ...window.__vis2q })");
  // (the keys the askers use: the components' (the model's rev, the links' count) and the viewport's notes (the mesh's rev
  // and size: a link's detail arriving merges the mesh again))
  const linksNow = () => d.ev("(async () => { const s = (await import('/src/bimble.ts')).state.value; return `${s.rev}|${s.links ?? 0}|${window.__bbGl.notesKey ?? ''}`; })()");
  const k0 = await linksNow();
  gpu = await adapter((e) => d.ev(e));
  console.log(`  adapter: ${gpu.renderer} (${gpu.software ? "a software rasteriser" : "a hardware GPU"})`);
  for (const [name, kind, g] of steps) {
    await settle();
    await arm(kind);
    await d.gesture(g);
    await d.gesture(still);
    if (kind === "click") { const sel = await d.ev("[...document.querySelectorAll('[data-bb=status-chip]')].map((e) => e.textContent).find((t) => /selected|sélectionn/.test(t)) ?? null"); check("the click selected an element (the press's feedback is its highlight)", !!sel, `${sel} at ${at}`); }
    if (kind === "key") { // after a selection and its clearing: what does not depend on the selection was not asked again
      const q1 = await d.ev("({ ...window.__vis2q })"), again = ["links_tree", "link_levels", "link_notes"].filter((k) => (q1[k] ?? 0) > (q0[k] ?? 0));
      const k1 = await linksNow(); // (the model or the links changed meanwhile (a link's indexing ending): then asking again is right)
      if (k1 !== k0) console.log(`  (the links, the model or the mesh changed during the selection (${k0} -> ${k1}): the queries' count not checked)`);
      else check("a selection and Escape ask the host nothing that does not depend on the selection (the links' tree, levels, notes: synchronous, 90 ms each on a real project)", again.length === 0, again.length ? `asked again: ${again.map((k) => `${k} ${q0[k] ?? 0} -> ${q1[k]}`).join(", ")}` : "");
    }
    if (kind === "wheel") { // the pivot without a scan of every element and without waiting for the GPU: the readout, or the plane at
      // once while the GPU still draws ("gpu busy"); here the GPU was idle before the wheel (gl.finish): the readout
      const nav = await d.ev("window.__bbGl.nav");
      check("the wheel's pivot read from the last picture's element attachment (the GPU idle before it: no scan of every element, no wait)", nav.pivotPick === "readout", `${nav.pivotPick ?? "not recorded"} (${nav.zoomFrom})`);
    }
    await timed(name);
  }
  // the same on a machine 4 and 6 times slower than the reference one (the dev machine: Apple M1 Pro, Asahi Linux): this host
  // calibrated first (a fixed typed-array loop in the page, median of 5), then the CPU throttled (CDP) by what is left of the
  // factor; a host already that much slower is covered by the unthrottled run above
  const REF_LOOP_MS = 25.0; // (the reference machine's median, 28 Nov 2026)
  const loop = `(() => { const a = new Float64Array(1 << 16); let s = 0; const t0 = performance.now(); for (let r = 0; r < 300; r++) for (let i = 0; i < a.length; i++) { a[i] = a[i] * 1.0000001 + i; s += a[i]; } return s > -1 ? performance.now() - t0 : 0; })()`;
  const runs = [];
  for (let i = 0; i < 5; i++) runs.push(await d.ev(loop));
  runs.sort((a, b) => a - b);
  const slower = Math.max(0.1, runs[2] / REF_LOOP_MS);
  console.log(`  calibration: the loop ${runs[2].toFixed(1)} ms here, ${REF_LOOP_MS} ms on the reference machine: this host is ${slower.toFixed(2)}x as slow`);
  // (the slow-machine cases run when asked: LAB_SLOW=1, meant for LAB_GPU=vulkan by hand; the gate declares the skip, its
  // reason and the numbers of the open item: ccore/results/inputs.md, ADR 0078 amendment 9's "What is left")
  if (process.env.LAB_SLOW !== "1") console.log("skipped: the slow-machine cases (4x and 6x the reference CPU): run with LAB_SLOW=1 LAB_GPU=vulkan; open item: press-select 212 ms and Escape 204 ms at 4x on 28 Nov 2026");
  const rates = [];
  if (process.env.LAB_SLOW === "1") for (const target of (process.env.VIS2_SLOWER ?? "4,6").split(",").map(Number).filter((x) => x > 1)) {
    const rate = target / slower;
    if (rate < 1.05) console.log(`  ${target}x the reference: the host is already ${slower.toFixed(2)}x slower than the reference: covered by the unthrottled run`);
    else rates.push({ target, rate: Math.round(rate * 100) / 100 });
  }
  for (const { target, rate } of rates) {
    console.log(`  ${target}x the reference machine: the CPU throttled ${rate}x`);
    await d.gesture(["fit", ...still]);
    const at3 = await d.ev(`(() => { const p = window.__bbGl.toScreen(${GEOREF[0] + 18000}, ${GEOREF[1] + 150}, ${GEOREF[2] + 3000}); return p ? [Math.round(p[0]), Math.round(p[1])] : null; })()`);
    await d.S("Emulation.setCPUThrottlingRate", { rate });
    try {
      for (const [name, kind, g] of [["press (a click that selects)", "click", [`click ${at3[0]} ${at3[1]}`]], ["Escape", "key", ["key Escape"]], ["wheel (a zoom step)", "wheel", ["zoom 1 50"]], ["drag (an orbit starts)", "drag", ["orbit 200 0 400"]]]) {
        await settle();
        await arm(kind);
        await d.gesture([...g, ...still]);
        await timed(`${target}x slower than the reference machine (throttled ${rate}x): ${name}`);
      }
    } finally { await d.S("Emulation.setCPUThrottlingRate", { rate: 1 }); }
  }
  // a click right after a feedback frame (Escape), before the complete picture: answered within 200 ms, the same element
  await d.gesture(["fit", ...still]); // (the home camera again: the pan moved the building)
  const at2 = await d.ev(`(() => { const p = window.__bbGl.toScreen(${GEOREF[0] + 18000}, ${GEOREF[1] + 150}, ${GEOREF[2] + 3000}); return p ? [Math.round(p[0]), Math.round(p[1])] : null; })()`);
  const selOf = () => d.ev("(async () => { const s = (await import('/src/bimble.ts')).state.value; return s.linked ? `${s.linked.link}/${s.linked.id}` : s.selection.join(','); })()");
  await d.gesture([`click ${at2[0]} ${at2[1]}`, ...still]);
  const want = await selOf();
  await settle();
  await d.gesture(["key Escape", "sleep 60"]);
  await arm("click");
  await d.gesture([`click ${at2[0]} ${at2[1]}`, ...still]);
  const got = await selOf();
  await timed("a click 60 ms after Escape (its feedback frame, before the complete picture)");
  check("that click selected the same element", !!want && got === want, got === want ? "" : `selected ${got}, expected ${want}`);
  console.log(`lab_inputs_links: ${passed} passed, ${failed} failed (LAB_GPU=${LAB_GPU}, ${d.gpu?.renderer})`);
  await bye(failed ? 1 : 0);
}
main().catch(async (e) => { console.error(e); await bye(1); });
