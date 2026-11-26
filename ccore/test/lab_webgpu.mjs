// The WebGPU renderer of the 3D view (ticket GPU1, ADR 0083, web-ui/src/gpu/):
//   CCORE_PORT_BASE=auto node ccore/test/lab_webgpu.mjs
// The native host, the lab, one headless Chromium at a time.
// 1. The fallback, always: Chromium as every lab test starts it (SwiftShader WebGL2) exposes
//    navigator.gpu with no adapter (ADR 0020's case). Asked for WebGPU (?gpu=webgpu), the view
//    stays on WebGL2 and draws; the performance panel and the copied report say why.
// 2. When this machine has a software WebGPU adapter (Chromium --enable-unsafe-webgpu
//    --enable-features=Vulkan --use-vulkan=swiftshader): without ?gpuSoftware=1 the fallback adapter is refused
//    (WebGL2 draws, the reason said); with it the WebGPU renderer draws, and its pictures are
//    compared pixel by pixel with WebGL2's on fixed cameras of golden/frame.json (72 elements)
//    and the mesh-heavy synthetic model (bench/mesh_heavy_model.py 700 1000): plain, a selection,
//    an element hidden, a section box, and with GPU culling of copies on (?gpuCull=1). Tolerance:
//    per picture at most 1 % of the pixels differ by more than 24 levels in a channel (the two APIs
//    rasterise 1-px lines and the depth bias of coplanar faces a little differently; no anti-
//    aliasing in either); the count and the largest difference are printed for each. A moving
//    frame (the budget) on WebGPU is not empty: at least 20 % of the still picture's drawn pixels
//    (frames.mjs's rule). When no adapter runs here, part 2 prints "SKIP WebGPU parity: <reason>"
//    (ci/run.sh adds it to the suite's line) and only part 1 counts.
// Prints "N passed, M failed"; exits non-zero on a failure.
import { execFileSync } from "node:child_process";
import { existsSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { CCORE, lab, sleep, waitFor } from "./lib/lab.mjs";

let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? `: ${detail}` : ""}`);
};
// Dawn's Vulkan backend on SwiftShader: --use-webgpu-adapter=swiftshader alone gives an adapter whose
// canvas cannot present here ("A valid external Instance reference no longer exists", Chromium 153).
const WEBGPU_FLAGS = ["--enable-unsafe-webgpu", "--enable-features=Vulkan", "--use-vulkan=swiftshader", "--disable-vulkan-surface"];
const BG = [247, 247, 245];
const TOL_LEVELS = 24, TOL_SHARE = 0.01;
const CAMS = [[0, 0], [0.8, -0.2], [2.4, 0.3]];

const settled = (L, using) => waitFor(`the renderer settled (${using})`, 30000, () => L.evalJs(`(() => { const r = window.__bbPerf?.snapshot().renderer;
  return r && r.reason !== "starting…" && (${JSON.stringify(using)} === "any" || r.using === ${JSON.stringify(using)}) ? { using: r.using, reason: r.reason, adapter: r.adapter } : null; })()`));

/** The picture at a camera (fitted, yaw/pitch offsets) as RGBA top row first, base64; drawn twice (the
 *  second a still frame on both renderers: the first may be a moving one for WebGPU). */
const snap = (L, gpu, [dy, dp]) => L.evalJs(`(async () => { const g = window.__bbGl; g.bench.snap(${dy}, ${dp}); let s = g.bench.snap(${dy}, ${dp});
  let w = s.w, h = s.h, rgba;
  if (${gpu}) { await window.__bbGpu.done(); const p = await window.__bbGpu.pixels(); w = p.w, h = p.h, rgba = p.rgba; }
  else { rgba = new Uint8Array(s.rgba.length); for (let y = 0; y < h; y++) rgba.set(s.rgba.subarray(4 * w * (h - 1 - y), 4 * w * (h - y)), 4 * w * y); }
  let bin = ""; for (let i = 0; i < rgba.length; i += 32768) bin += String.fromCharCode(...rgba.subarray(i, i + 32768));
  return { w, h, b64: btoa(bin) }; })()`);
const decode = (p) => ({ w: p.w, h: p.h, rgba: Buffer.from(p.b64, "base64") });
const inked = (p) => { let n = 0; for (let i = 0; i < p.rgba.length; i += 4) if (Math.abs(p.rgba[i] - BG[0]) > 6 || Math.abs(p.rgba[i + 1] - BG[1]) > 6 || Math.abs(p.rgba[i + 2] - BG[2]) > 6) n++; return n; };
function compare(a, b) {
  if (a.w !== b.w || a.h !== b.h) return { size: `${a.w}x${a.h} vs ${b.w}x${b.h}`, over: Infinity, any: Infinity, max: 255, share: 1 };
  let over = 0, any = 0, max = 0;
  for (let i = 0; i < a.rgba.length; i += 4) {
    const d = Math.max(Math.abs(a.rgba[i] - b.rgba[i]), Math.abs(a.rgba[i + 1] - b.rgba[i + 1]), Math.abs(a.rgba[i + 2] - b.rgba[i + 2]));
    if (d > 0) any++;
    if (d > TOL_LEVELS) over++;
    if (d > max) max = d;
  }
  return { over, any, max, share: over / (a.w * a.h) };
}

const B = (L, body) => L.evalJs(`(async () => { const b = await import('/src/bimble.ts'); ${body} })()`);
/** The states compared: on/off steps through the C core's own actions (the same on both pages). */
const STATES = (id) => [
  { name: "plain", on: null, off: null },
  { name: "selected", on: `b.select(['${id}'])`, off: "b.select([])" },
  { name: "hidden", on: `b.select(['${id}']); b.runAction('hide_now'); b.select([])`, off: "b.runAction('clear_temporary')" },
  { name: "section box", on: `b.select(['${id}']); b.runAction('box_to_selection'); b.select([])`, off: "b.runAction('section_box')" },
];

async function pictures(L, q, gpu, id, states) {
  await L.navigate(q);
  const r = await settled(L, "any");
  if (r.using !== (gpu ? "WebGPU" : "WebGL2")) throw new Error(`${q}: ${r.using} draws: ${r.reason}`);
  await waitFor("proxies asked", 120000, () => L.evalJs("window.__bbGl.proxiesPending !== true"));
  await sleep(500);
  const out = new Map();
  for (const st of states) {
    if (st.on) { await B(L, `${st.on}; return 0;`); await sleep(900); }
    for (const c of CAMS) out.set(`${st.name} @${c.join(",")}`, decode(await snap(L, gpu, c)));
    if (st.off) { await B(L, `${st.off}; return 0;`); await sleep(900); }
  }
  return { out, r };
}

async function model(name, file, frameTest) {
  const L = await lab({ name: "lab_webgpu", model: file, query: "gpu=webgl2", chromiumFlags: WEBGPU_FLAGS });
  try {
    const r0 = await settled(L, "WebGL2");
    check(`${name}: ?gpu=webgl2 draws with WebGL2`, r0.using === "WebGL2");
    // the element under the canvas's centre (the same id on both pages)
    const id = await L.evalJs("(() => { const c = document.getElementById('bimble').getBoundingClientRect(); return window.__bbGl.pickAt(c.left + c.width / 2, c.top + c.height / 2); })()");
    check(`${name}: an element under the centre for the states`, typeof id === "string", String(id));
    const states = STATES(id);
    const gl = await pictures(L, "gpu=webgl2", false, id, states);
    const gp = await pictures(L, "gpu=webgpu&gpuSoftware=1", true, id, states);
    const info = await L.evalJs("({ scene: window.__bbGpu?.scene() ?? null, err: window.__bbGl.error })");
    console.log(`${name}: WebGPU scene ${JSON.stringify(info.scene)}; adapter ${JSON.stringify(gp.r.adapter)}`);
    check(`${name}: ?gpu=webgpu&gpuSoftware=1 draws with WebGPU`, gp.r.using === "WebGPU", JSON.stringify(gp.r));
    for (const [k, a] of gl.out) {
      const c = compare(a, gp.out.get(k));
      check(`${name}: ${k}: WebGPU = WebGL2 within ${TOL_LEVELS} levels on ${100 * (1 - TOL_SHARE)} % of the pixels`, c.share <= TOL_SHARE,
        `${c.any} pixels differ at all, ${c.over} by more than ${TOL_LEVELS} (${(100 * c.share).toFixed(3)} %), max ${c.max}${c.size ? `, ${c.size}` : ""}; ${inked(a)} drawn pixels`);
    }
    // GPU culling of copies (the compute pass, an indirect draw per group)
    const gc = await pictures(L, "gpu=webgpu&gpuSoftware=1&gpuCull=1", true, id, [states[0]]);
    const cs = await L.evalJs("({ cull: window.__bbPerf.snapshot().renderer.gpuCull, scene: window.__bbGpu?.scene() ?? null, draws: window.__bbGl.frameDraws })");
    console.log(`${name}: GPU culling: ${JSON.stringify(cs)}`);
    check(`${name}: ?gpuCull=1 turns GPU culling on`, cs.cull === true);
    for (const [k, a] of gc.out) {
      const c = compare(gl.out.get(k), a);
      check(`${name}: GPU culling, ${k}: = WebGL2 within the tolerance`, c.share <= TOL_SHARE, `${c.any} differ, ${c.over} over ${TOL_LEVELS}, max ${c.max}`);
    }
    if (frameTest) { // a moving frame is never empty: the budget's frame against the still picture
      const still = decode(await snap(L, true, [0, 0]));
      const mv = await L.evalJs(`(async () => { const g = window.__bbGl; g.bench.frame(0.05, true); await window.__bbGpu.done(); const p = await window.__bbGpu.pixels();
        let n = 0; for (let i = 0; i < p.rgba.length; i += 4) if (Math.abs(p.rgba[i] - 247) > 6 || Math.abs(p.rgba[i + 1] - 247) > 6 || Math.abs(p.rgba[i + 2] - 245) > 6) n++;
        return { n, moving: g.moving }; })()`);
      const s = inked(still);
      console.log(`${name}: moving frame ${JSON.stringify(mv.moving)}; drawn pixels ${mv.n} of the still picture's ${s}`);
      check(`${name}: a WebGPU moving frame is a moving one (the budget)`, mv.moving?.moving === true, JSON.stringify(mv.moving));
      check(`${name}: a WebGPU moving frame is not empty (>= 20 % of the still picture's pixels)`, mv.n >= 0.2 * s, `${mv.n} / ${s}`);
    }
    check(`${name}: no uncaught page error`, L.errors.length === 0, L.errors.join(" | "));
    const warn = L.console.filter((l) => /webgpu/i.test(l));
    if (warn.length) console.log(`${name}: console: ${warn.slice(0, 5).join(" | ")}`);
  } finally {
    await L.close();
  }
}

const tmp = mkdtempSync(join(tmpdir(), "ccore-lab-webgpu-"));
try {
  // ---- 1. the fallback (always): navigator.gpu with no adapter ----
  const L = await lab({ name: "lab_webgpu", model: join(CCORE, "golden/frame.json"), query: "gpu=webgpu&perf=1" });
  let adapterHere = null;
  try {
    const r = await settled(L, "any");
    check("no adapter: asked for WebGPU, WebGL2 draws", r.using === "WebGL2", JSON.stringify(r));
    check("no adapter: the reason is kept (no adapter, ADR 0020)", /no adapter/.test(r.reason ?? ""), r.reason);
    check("no adapter: no WebGPU canvas over the view", !(await L.evalJs("!!document.querySelector('canvas[data-bb-gpu]')")));
    check("no adapter: WebGL2 drew the model", (await L.evalJs("window.__bbGl.frameTris")) > 0);
    await waitFor("the panel", 20000, () => L.evalJs("!!document.querySelector('[data-bb=perf-fallback]')"));
    const fb = await L.evalJs("document.querySelector('[data-bb=perf-fallback]').textContent.trim()");
    check("no adapter: the performance panel says why WebGPU is not used", /no adapter/.test(fb), fb);
    await L.evalJs("document.querySelector('[data-bb=perf-copy]').click()");
    const text = await waitFor("the copied report", 5000, () => L.evalJs("window.__bbPerfCopied ?? null"));
    console.log(text.split("\n").slice(0, 3).join("\n"));
    check("no adapter: the report's renderer line says WebGL2, asked WebGPU, and why", /^renderer: WebGL2 \(asked: webgpu\); WebGPU not used: .*no adapter/m.test(text));
    check("no adapter: no uncaught page error", L.errors.length === 0, L.errors.join(" | "));
    // ---- 2. a software adapter, when this machine has one ----
    await L.open("gpu=webgpu&perf=1", WEBGPU_FLAGS);
    const r2 = await settled(L, "any");
    adapterHere = /no adapter|no WebGPU/.test(r2.reason ?? "") ? null : r2;
    if (adapterHere) {
      check("a fallback (software) adapter is refused without ?gpuSoftware=1: WebGL2 draws", r2.using === "WebGL2" && /fallback/.test(r2.reason ?? ""), JSON.stringify(r2));
    } else console.log(`SKIP WebGPU parity: no WebGPU adapter in Chromium here (${r2.reason}); the fallback above ran instead`);
  } finally {
    await L.close();
  }
  if (adapterHere) {
    await model("72 elements", join(CCORE, "golden/frame.json"), true);
    const heavy = join(tmp, "heavy-700-1000.json");
    if (!existsSync(heavy)) execFileSync("python3", [join(CCORE, "bench/mesh_heavy_model.py"), "700", "1000", heavy], { stdio: ["ignore", "pipe", "pipe"] });
    await model("mesh-heavy (772 elements)", heavy, true);
  }
} catch (e) {
  check("the run", false, e?.stack ?? String(e));
} finally {
  rmSync(tmp, { recursive: true, force: true });
}
console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
