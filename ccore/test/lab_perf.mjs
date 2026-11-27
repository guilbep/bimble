// The performance panel (ticket GPU1, web-ui/src/panels/Performance.vue, web-ui/src/perf.ts):
//   CCORE_PORT_BASE=auto node ccore/test/lab_perf.mjs
// The native host on golden/frame.json (72 elements), the lab with ?perf=1, one headless Chromium
// (SwiftShader WebGL2). Checks: the panel opens from the address; it names the renderer (WebGL2) and
// the adapter, and says plainly when it is software (SwiftShader); after an orbit (a left drag) the
// moving frame times, triangles and draw calls are not zero and the drag is in the slowest inputs;
// "Copy report" gives plain text with the renderer line, without the model's file name unless
// "Include file names" is ticked; "Record 10 s" ends with a summary; the status bar's button closes
// and opens the panel. SIZE2: at 1280, 1024 and 800 px wide (native mode, the status line made long with 16
// link chips as a folder of links gives), the button is inside the viewport and is what a click at its centre
// hits (an icon alone under 1100 px), and a real click there toggles the panel; File > Performance… and the
// palette's "performance" open it too (before SIZE2 the chips pushed it out of the window). Prints "N passed,
// M failed"; exits non-zero on a failure.
import { join } from "node:path";
import { CCORE, lab, sleep, waitFor } from "./lib/lab.mjs";
import { LAB_GPU, webgpuFlags } from "./lib/chromium.mjs";
import { adapter as inputAdapter, readTimes, verdict, watchSource } from "./lib/input_time.mjs";

let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? `: ${detail}` : ""}`);
};

// GPU2: a WebGPU adapter for the toggle (SwiftShader's, a fallback adapter, accepted with ?gpuSoftware=1; the GPU's under LAB_GPU=vulkan)
const SOFT = LAB_GPU === "software";
const L = await lab({ name: "lab_perf", model: join(CCORE, "golden/frame.json"), query: SOFT ? "perf=1&gpuSoftware=1" : "perf=1", chromiumFlags: webgpuFlags() });
try {
  const q = (sel) => L.evalJs(`document.querySelector(${JSON.stringify(sel)})?.textContent?.trim() ?? null`);
  await waitFor("the panel", 20000, () => L.evalJs("!!document.querySelector('[data-bb=perf]')"));
  check("?perf=1 opens the performance panel", true);
  check("the panel names the renderer: WebGL2", /^WebGL2\b/.test((await q("[data-bb=perf-renderer]")) ?? ""), await q("[data-bb=perf-renderer]"));
  const adapter = await q("[data-bb=perf-adapter]");
  check("the panel names the adapter", !!adapter && adapter.length > 3, adapter);
  check("a software adapter is said plainly (SwiftShader here)", /swiftshader/i.test(adapter ?? "") ? !!(await q("[data-bb=perf-software]")) : true, await q("[data-bb=perf-software]"));
  const f0 = await L.evalJs("window.__bbGl.frames");
  await L.drag(240, 60, 30);
  await waitFor("frames after the drag", 10000, () => L.evalJs(`window.__bbGl.frames > ${f0}`));
  await sleep(1200); // the fill after the stop, and the panel's 500 ms refresh
  const s = await L.evalJs("(() => { const s = window.__bbPerf.snapshot(); return { moving: s.moving, still: s.still, tris: s.tris, draws: s.draws, synced: s.synced, inputs: s.inputs, gpu: s.gpuBytes, el: s.elements }; })()");
  console.log(`snapshot: ${JSON.stringify(s)}`);
  check("moving frames timed after an orbit (n > 0, median > 0)", s.moving.n > 0 && s.moving.median > 0, JSON.stringify(s.moving));
  check("the frames are waited for while the panel is open (draw to pixels)", s.synced === true);
  check("triangles and draw calls of the last frame are not zero", s.tris > 0 && s.draws > 0, `${s.tris} triangles, ${s.draws} draws`);
  check("GPU bytes and the model's elements are known", s.gpu > 0 && s.el === 72, `${s.gpu} B, ${s.el} elements`);
  check("the drag is among the slowest inputs", s.inputs.some((i) => /drag/.test(i.what) && i.ms > 0), JSON.stringify(s.inputs.slice(0, 3)));
  const shown = await q("[data-bb=perf-moving]");
  check("the panel shows the moving times", !!shown && shown !== "–", shown);
  // Copy report (headless: the clipboard may be refused; the text is kept for the test and shown)
  await L.evalJs("document.querySelector('[data-bb=perf-copy]').click()");
  const text = await waitFor("the copied report", 5000, () => L.evalJs("window.__bbPerfCopied ?? null"));
  console.log("---- report ----\n" + text + "---- end ----");
  check("the report has the renderer line", /^renderer: WebGL2 \(asked: auto\)/m.test(text));
  check("the report has the adapter and the moving frame times", /^adapter: /m.test(text) && /^ {2}moving: n \d+/m.test(text));
  const file = await L.evalJs("(async () => (await import('/src/bimble.ts')).state.value.file)()");
  const base = (file ?? "").split(/[\\/]/).pop();
  check("no file name in the report by default", !!base && !text.includes(base), base);
  await L.evalJs("document.querySelector('[data-bb=perf-names]').click()");
  await sleep(100);
  await L.evalJs("window.__bbPerfCopied = null, document.querySelector('[data-bb=perf-copy]').click()");
  const named = await waitFor("the copied report with names", 5000, () => L.evalJs("window.__bbPerfCopied ?? null"));
  check("ticked: the model's file name is in the report", named.includes(base));
  // Record 10 s while orbiting
  await L.evalJs("document.querySelector('[data-bb=perf-record]').click()");
  check("Record 10 s starts", !!(await waitFor("recording", 3000, () => L.evalJs("!!document.querySelector('[data-bb=perf-recording]')"))));
  for (let i = 0; i < 4; i++) await L.drag(i % 2 ? -200 : 200, 0, 20);
  const rec = await waitFor("the recording's end", 20000, () => q("[data-bb=perf-recorded]"));
  check("Record 10 s ends with a summary of its frames", /\d+/.test(rec ?? ""), rec);
  // the status bar's button
  await L.evalJs("document.querySelector('[data-bb=status-perf]').click()");
  await sleep(200);
  check("the status bar's Performance closes the panel", !(await L.evalJs("!!document.querySelector('[data-bb=perf]')")));
  await L.evalJs("document.querySelector('[data-bb=status-perf]').click()");
  check("and opens it again", !!(await waitFor("the panel again", 3000, () => L.evalJs("!!document.querySelector('[data-bb=perf]')"))));
  // ---- SIZE2: the button always reachable ----
  await L.evalJs(`(() => { const f = document.querySelector('[data-bb=statusbar]'), end = f.querySelector('[data-bb=status-end]') ?? f.querySelector('[data-bb=status-perf]');
    for (let k = 0; k < 16; k++) { const c = document.createElement('span'); c.className = 'chip c-s nx'; c.dataset.bbTest = 'long'; c.textContent = 'Lié : a consultant model with a long file name ' + k; f.insertBefore(c, end); }
    return 1; })()`);
  for (const w of [1280, 1024, 800]) {
    await L.S("Emulation.setDeviceMetricsOverride", { width: w, height: 800, deviceScaleFactor: 1, mobile: false });
    await sleep(300);
    const r = await L.evalJs(`(() => { const b = document.querySelector('[data-bb=status-perf]'), r = b.getBoundingClientRect(), x = r.left + r.width / 2, y = r.top + r.height / 2;
      const hit = document.elementFromPoint(x, y); return { x, y, l: r.left, r: r.right, w: r.width, vw: window.innerWidth, hit: !!hit && b.contains(hit), text: (b.querySelector(".pft")?.offsetWidth ?? 0) > 0 }; })()`);
    const inside = r.l >= 0 && r.r <= r.vw && r.w > 8 && r.hit;
    const open0 = await L.evalJs("!!document.querySelector('[data-bb=perf]')");
    if (inside) {
      await L.S("Input.dispatchMouseEvent", { type: "mousePressed", x: r.x, y: r.y, button: "left", buttons: 1, clickCount: 1 });
      await L.S("Input.dispatchMouseEvent", { type: "mouseReleased", x: r.x, y: r.y, button: "left", buttons: 0, clickCount: 1 });
      await sleep(300);
    }
    const open1 = await L.evalJs("!!document.querySelector('[data-bb=perf]')");
    check(`${w} px wide, a long status line: the Performance button in the window and clickable (${r.text ? "with its name" : "its icon alone"})`, inside && open1 !== open0,
      `button ${r.l.toFixed(0)}..${r.r.toFixed(0)} of ${r.vw}, hit ${r.hit}, panel ${open0} -> ${open1}`);
  }
  await L.evalJs("document.querySelectorAll('[data-bb-test=long]').forEach((e) => e.remove())");
  await L.S("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  await L.evalJs("(async () => { (await import('/src/panels/index.ts')).currentPanel.value = null; (await import('/src/dialogs/index.ts')).currentDialog.value = 'file-menu'; })()");
  const fm = await waitFor("the File menu", 3000, () => L.evalJs("!!document.querySelector('[data-bb=file-menu-performance]')")).catch(() => false);
  if (fm) await L.evalJs("document.querySelector('[data-bb=file-menu-performance]').click()");
  check("File > Performance… opens the panel", !!fm && !!(await waitFor("the panel from the File menu", 3000, () => L.evalJs("!!document.querySelector('[data-bb=perf]')")).catch(() => false)));
  await L.evalJs("(async () => { (await import('/src/panels/index.ts')).currentPanel.value = null; (await import('/src/dialogs/index.ts')).currentDialog.value = 'palette'; })()");
  const pal = await waitFor("the palette", 3000, () => L.evalJs("!!document.querySelector('[data-bb=palette-query]')")).catch(() => false);
  const picked = pal && await L.evalJs(`(async () => { const i = document.querySelector('[data-bb=palette-query]'); i.value = 'performance'; i.dispatchEvent(new Event('input'));
    await new Promise((r) => setTimeout(r, 200));
    const row = [...document.querySelectorAll('[data-bb=palette-item]')].find((e) => /Performance/i.test(e.textContent)); if (!row) return false; row.click(); return true; })()`);
  check("the palette's \"performance\" opens the panel", !!picked && !!(await waitFor("the panel from the palette", 3000, () => L.evalJs("!!document.querySelector('[data-bb=perf]')")).catch(() => false)));
  // ---- GPU2: the renderer toggle, live: the panel's switch, the status bar's chip, the palette ----
  await L.evalJs("(async () => { (await import('/src/dialogs/index.ts')).currentDialog.value = null; (await import('/src/panels/index.ts')).currentPanel.value = 'performance'; })()");
  await waitFor("the panel's switch", 3000, () => L.evalJs("!!document.querySelector('[data-bb=perf-switch-webgpu]')"));
  const gpuInfo = await inputAdapter(L.evalJs);
  const R = () => L.evalJs("(() => { const r = window.__bbPerf.snapshot().renderer; return { using: r.using, asked: r.asked, reason: r.reason, switchMs: r.switchMs ?? null, progress: r.progress ?? null }; })()");
  const clickAt = async (sel) => {
    const c = await L.evalJs(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); e.scrollIntoView({ block: "center" }); const r = e.getBoundingClientRect(); return { x: r.left + r.width / 2, y: r.top + r.height / 2 }; })()`);
    await L.S("Input.dispatchMouseEvent", { type: "mousePressed", x: c.x, y: c.y, button: "left", buttons: 1, clickCount: 1 });
    await L.S("Input.dispatchMouseEvent", { type: "mouseReleased", x: c.x, y: c.y, button: "left", buttons: 0, clickCount: 1 });
  };
  /** A real click on `sel`, timed (input_time.mjs): the page's part is the switch's label pressed and the chip's; presented: the
   *  first frame of the renderer asked, or the progress line while it gets ready (WebGL2's picture stays: never blank). */
  const timedSwitch = async (sel, to) => {
    const f0 = await L.evalJs("window.__bbGl.frames");
    await L.evalJs(watchSource({ kind: "pointerdown",
      page: `document.querySelector('[data-bb=perf-switch-${to}]')?.getAttribute('aria-pressed') === 'true' && /${to === "webgpu" ? "WebG(PU|L2\\*)" : "WebGL2"}/.test(document.querySelector('[data-bb=status-renderer]')?.textContent ?? '')`,
      // presented: the first frame drawn by the renderer asked, or the progress line while it gets ready (the old picture stays)
      presented: `(window.__bbGl.frames > ${f0} && window.__bbPerf.snapshot().renderer.using === ${JSON.stringify(to === "webgpu" ? "WebGPU" : "WebGL2")}) || !!document.querySelector('[data-bb=perf-progress]')` }));
    await clickAt(sel);
    return verdict(await readTimes(L.evalJs), gpuInfo);
  };
  check("the panel's switch says WebGL2 is asked and drawing", /WebGL2/.test((await q("[data-bb=perf-drawing]")) ?? "") && (await L.evalJs("document.querySelector('[data-bb=perf-switch-webgl2]').getAttribute('aria-pressed')")) === "true");
  check("the status bar's chip names the renderer: WebGL2", /^WebGL2/.test((await q("[data-bb=status-renderer]")) ?? ""), await q("[data-bb=status-renderer]"));
  const v1 = await timedSwitch("[data-bb=perf-switch-webgpu]", "webgpu");
  check(`the switch to WebGPU (a real click): ${v1.line}`, v1.ok);
  const r1 = await waitFor("WebGPU drawing (or the reason it does not)", 30000, async () => { const r = await R(); return r.reason !== "starting…" && !r.progress ? r : null; });
  console.log(`WebGPU asked: ${JSON.stringify(r1)}`);
  check("WebGPU asked: WebGPU draws, live (no reload)", r1.using === "WebGPU" && (await L.evalJs("!!document.querySelector('canvas[data-bb-gpu=webgpu]')")), JSON.stringify(r1));
  const sw1 = r1.using === "WebGPU" ? await waitFor("the switch's time", 10000, async () => (await R()).switchMs).catch(() => null) : null; // (its first frame's GPU work done)
  check("the switch's time (the click to the end of the first WebGPU frame) is reported", typeof sw1 === "number", `${sw1?.toFixed?.(1)} ms`);
  check("the chip says WebGPU", /^WebGPU/.test((await q("[data-bb=status-renderer]")) ?? ""), await q("[data-bb=status-renderer]"));
  check("the choice is remembered (the setting renderer)", (await L.evalJs("(async () => (await import('/src/bimble.ts')).userSettings.value?.values.renderer)()")) === "webgpu");
  const v2 = await timedSwitch("[data-bb=status-renderer]", "webgl2");
  check(`the chip switches back to WebGL2 (a real click): ${v2.line}`, v2.ok);
  const r2 = await waitFor("WebGL2 drawing", 30000, async () => { const r = await R(); return r.using === "WebGL2" && !r.progress ? r : null; });
  check("WebGL2 draws again, the WebGPU canvas gone", r2.using === "WebGL2" && !(await L.evalJs("!!document.querySelector('canvas[data-bb-gpu=webgpu]')")), JSON.stringify(r2));
  // the palette's "renderer webgpu"
  await L.evalJs("(async () => { (await import('/src/dialogs/index.ts')).currentDialog.value = 'palette'; })()");
  await waitFor("the palette", 3000, () => L.evalJs("!!document.querySelector('[data-bb=palette-query]')"));
  const palR = await L.evalJs(`(async () => { const i = document.querySelector('[data-bb=palette-query]'); i.value = 'renderer webgpu'; i.dispatchEvent(new Event('input'));
    await new Promise((r) => setTimeout(r, 200));
    const row = [...document.querySelectorAll('[data-bb=palette-item]')].find((e) => /WebGPU/.test(e.textContent)); if (!row) return false; row.click(); return true; })()`);
  const r3 = palR && await waitFor("WebGPU from the palette", 30000, async () => { const r = await R(); return r.using === "WebGPU" && !r.progress ? r : null; }).catch(() => null);
  check("the palette's \"renderer webgpu\" switches to WebGPU", !!r3, JSON.stringify(r3));
  await L.evalJs("(async () => (await import('/src/perf.ts')).chooseRenderer('webgl2'))()");
  await waitFor("WebGL2 again", 30000, async () => { const r = await R(); return r.using === "WebGL2" && !r.progress ? r : null; });
  // "Compare renderers": the same camera path on both, the copied report holds both blocks
  await L.evalJs("(async () => { (await import('/src/dialogs/index.ts')).currentDialog.value = null; (await import('/src/panels/index.ts')).currentPanel.value = 'performance'; })()");
  await waitFor("the Compare button", 3000, () => L.evalJs("!!document.querySelector('[data-bb=perf-compare]')"));
  await clickAt("[data-bb=perf-compare]");
  const busy = await waitFor("comparing", 3000, () => L.evalJs("document.querySelector('[data-bb=perf-comparing]')?.textContent ?? null")).catch(() => null);
  check("Compare renderers starts and says what it does", !!busy, busy);
  await waitFor("the comparison's end", 180000, () => L.evalJs("!document.querySelector('[data-bb=perf-comparing]') && !!window.__bbPerf.compared()"));
  await L.evalJs("window.__bbPerfCopied = null, document.querySelector('[data-bb=perf-copy]').click()");
  const rep = await waitFor("the report", 5000, () => L.evalJs("window.__bbPerfCopied"));
  const block = rep.slice(rep.indexOf("compare renderers"));
  console.log(block);
  check("the copied report holds both renderers side by side (the same path)", /compare renderers \(an orbit/.test(rep) && /WebGL2\s+WebGPU/.test(block) &&
    /moving median \/ p95\s+\S+ ms \/ \S+ ms \(n \d+\)\s+\S+ ms/.test(block) && /time to switch/.test(block) && /GPU bytes\s+[\d.]+ MB\s+[\d.]+ MB/.test(block));
  check("after the comparison the renderer asked before is back (WebGL2)", (await R()).using === "WebGL2" || (await waitFor("WebGL2 back", 30000, async () => (await R()).using === "WebGL2").catch(() => false)));
  // the Optimisations section (the owner's rule: every optimisation a setting, default and off, in the panel and the report)
  const rowsOpt = await L.evalJs("[...document.querySelectorAll('[data-bb^=perf-opt-]')].map((e) => e.dataset.bb)");
  check("the panel lists the optimisations (renderer, moving_fps, moving_proxies, link_gpu_mb, gpu_cull)", ["renderer", "moving_fps", "moving_proxies", "link_gpu_mb", "gpu_cull"].every((k) => rowsOpt.includes(`perf-opt-${k}`)), rowsOpt.join(" "));
  await L.evalJs("(() => { const e = document.querySelector('[data-bb=perf-opt-moving_proxies] input'); e.scrollIntoView({ block: 'center' }); e.click(); })()");
  await sleep(300);
  await L.evalJs("window.__bbPerfCopied = null, document.querySelector('[data-bb=perf-copy]').click()");
  const rep2 = await waitFor("the report", 5000, () => L.evalJs("window.__bbPerfCopied"));
  check("the report holds each optimisation's value; one turned off says so", /optimisations \(setting: value\):/.test(rep2) && /  renderer: \w+/.test(rep2) && /  moving_proxies: false \(default true\), off/.test(rep2),
    rep2.split("\n").filter((l) => /^  (renderer|moving_fps|moving_proxies|link_gpu_mb|gpu_cull):/.test(l)).join(" | "));
  check("no uncaught page error", L.errors.length === 0, L.errors.join(" | "));
} catch (e) {
  check("the run", false, e?.stack ?? String(e));
} finally {
  await L.close();
}
console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
