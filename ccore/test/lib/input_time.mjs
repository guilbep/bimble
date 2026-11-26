// An input's response time, split into what the page controls and what the GPU adds (ADR 0074,
// ticket FIRST1's follow-up), for every lab test that asserts 200 ms on an input:
//   import { adapter, watchSource, readTimes, verdict } from "./lib/input_time.mjs";
//   const gpu = await adapter(evalJs);                      // once per page
//   await evalJs(watchSource({ kind: "pointerdown", page: "<expr>", presented: "<expr>" }));
//   ... dispatch the input (CDP Input.dispatch*) ...
//   const t = await readTimes(evalJs);                      // { page_ms, presented_ms }
//   const v = verdict(t, gpu); check(`${name}: ${v.line}`, v.ok);
// page_ms: from the input event's timeStamp to the moment the page has done its part: the `page`
//   expression is true, checked after every DOM change (MutationObserver), after the input's own
//   events (pointerup, click, keyup...) and in every animation frame (a canvas drawn in a frame
//   callback counts in that callback). Asserted <= 200 ms, always.
// presented_ms: from the same timeStamp to the first animation frame whose callbacks see the
//   `presented` expression true (the frame then goes to the compositor; what lab tests measured
//   before). Asserted <= 200 ms on a hardware GPU; on a software rasteriser (SwiftShader, llvmpipe:
//   headless Chromium here) it is printed and fails only above 1,000 ms, since that GPU process
//   compiles each new kind of drawing at its first frame (200 to 900 ms, the page idle; FIRST1).
// The watcher is armed before the input (a check started after it can miss the frame and time a
// later one). Lives in test/lib/, not test/: ci/run.sh runs every test/*.mjs as a suite.

export const PAGE_LIMIT_MS = 200, PRESENTED_LIMIT_MS = 200, SOFTWARE_PRESENTED_LIMIT_MS = 1000;

/** The page's WebGL renderer and whether it is a software rasteriser. */
export const adapter = (evalJs) => evalJs(`(() => {
  const gl = document.createElement('canvas').getContext('webgl2') ?? document.createElement('canvas').getContext('webgl');
  const ext = gl?.getExtension('WEBGL_debug_renderer_info');
  const renderer = gl ? String(gl.getParameter(ext ? ext.UNMASKED_RENDERER_WEBGL : gl.RENDERER)) : 'none';
  gl?.getExtension('WEBGL_lose_context')?.loseContext();
  return { renderer, software: !gl || /swiftshader|llvmpipe|softpipe|software/i.test(renderer) };
})()`);

/** A page statement that arms the watcher (embed it in a larger expression, or evaluate it). `kind`:
 *  the event whose timeStamp starts the clock; `page`, `presented`: page expressions (sync);
 *  `stamp`: an expression giving that timeStamp (or null) when the page's own listeners may stop
 *  the event before ours (keys.ts: Ctrl K), read from a listener installed before the page's. */
export function watchSource({ kind, page, presented = page, stamp = null }) {
  return `(() => {
    const W = window.__bbInput = { t: null, page_ms: null, presented_ms: null };
    const holds = (src) => { try { return !!(src === 'p' ? (${page}) : (${presented})); } catch { return false; } };
    const stamp = () => { if (W.t === null) { try { const t = ${stamp ?? "null"}; if (typeof t === 'number') W.t = t; } catch {} } };
    const check = () => {
      stamp();
      if (W.t === null || W.page_ms !== null) return;
      if (holds('p')) { W.page_ms = performance.now() - W.t; mo.disconnect(); }
    };
    const mo = new MutationObserver(check);
    mo.observe(document, { subtree: true, childList: true, attributes: true, characterData: true });
    addEventListener(${JSON.stringify(kind)}, (e) => { if (W.t === null) W.t = e.timeStamp; queueMicrotask(check); }, { capture: true });
    for (const k of ['pointerdown', 'pointerup', 'click', 'keydown', 'keyup', 'pointermove']) addEventListener(k, () => { queueMicrotask(check); setTimeout(check, 0); });
    const frame = () => {
      stamp();
      if (W.t !== null) {
        check();
        if (holds('q')) { W.presented_ms = performance.now() - W.t; if (W.page_ms === null) W.page_ms = W.presented_ms; return; }
        if (performance.now() - W.t > 5000) return;
      }
      requestAnimationFrame(frame);
    };
    requestAnimationFrame(frame);
    return true;
  })()`;
}

/** Waits (up to `ms`) for both times; nulls for those not reached. */
export async function readTimes(evalJs, ms = 8000) {
  const until = Date.now() + ms;
  let w = null;
  while (Date.now() < until) {
    w = await evalJs("window.__bbInput ? { t: window.__bbInput.t, page_ms: window.__bbInput.page_ms, presented_ms: window.__bbInput.presented_ms } : null").catch(() => null);
    if (w && w.page_ms !== null && w.presented_ms !== null) break;
    await new Promise((r) => setTimeout(r, 50));
  }
  return { page_ms: w?.page_ms ?? null, presented_ms: w?.presented_ms ?? null, input: w?.t ?? null };
}

const f = (x) => (typeof x === "number" ? `${x.toFixed(1)} ms` : "none");

/** The checks of ADR 0074 on one input's times: ok, and the log line naming the rule applied. */
export function verdict({ page_ms, presented_ms }, gpu) {
  const pageOk = typeof page_ms === "number" && page_ms <= PAGE_LIMIT_MS;
  const limit = gpu.software ? SOFTWARE_PRESENTED_LIMIT_MS : PRESENTED_LIMIT_MS;
  const presOk = typeof presented_ms === "number" && presented_ms <= limit;
  const rule = gpu.software
    ? `software rasteriser (${gpu.renderer.slice(0, 40)}): presented time reported, not asserted at ${PRESENTED_LIMIT_MS} ms (fails above ${SOFTWARE_PRESENTED_LIMIT_MS})`
    : `hardware GPU: presented time asserted at ${PRESENTED_LIMIT_MS} ms`;
  return { ok: pageOk && presOk, pageOk, presOk, line: `page ${f(page_ms)} (<= ${PAGE_LIMIT_MS}), presented ${f(presented_ms)}; ${rule}` };
}
