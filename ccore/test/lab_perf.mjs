// The performance panel (ticket GPU1, web-ui/src/panels/Performance.vue, web-ui/src/perf.ts):
//   CCORE_PORT_BASE=auto node ccore/test/lab_perf.mjs
// The native host on golden/frame.json (72 elements), the lab with ?perf=1, one headless Chromium
// (SwiftShader WebGL2). Checks: the panel opens from the address; it names the renderer (WebGL2) and
// the adapter, and says plainly when it is software (SwiftShader); after an orbit (a left drag) the
// moving frame times, triangles and draw calls are not zero and the drag is in the slowest inputs;
// "Copy report" gives plain text with the renderer line, without the model's file name unless
// "Include file names" is ticked; "Record 10 s" ends with a summary; the status bar's button closes
// and opens the panel. Prints "N passed, M failed"; exits non-zero on a failure.
import { join } from "node:path";
import { CCORE, lab, sleep, waitFor } from "./lib/lab.mjs";

let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? `: ${detail}` : ""}`);
};

const L = await lab({ name: "lab_perf", model: join(CCORE, "golden/frame.json"), query: "perf=1" });
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
  check("no uncaught page error", L.errors.length === 0, L.errors.join(" | "));
} catch (e) {
  check("the run", false, e?.stack ?? String(e));
} finally {
  await L.close();
}
console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
