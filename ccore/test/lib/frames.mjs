// The picture an action left, without a fixed sleep (ticket BUG1's settled(), shared by TEST1):
//   import { loops, settled } from "./lib/frames.mjs";
//   const l0 = await loops(evalJs);            // right after the action
//   await settled(evalJs, "the place drawn", l0); // then read the pixels
// Waits for a frame loop of the WebGL viewport (web-ui/src/glviewport.ts) that began after the
// action (__bbGl.loops > l0), whose last draw (__bbGl.drawn) shows the expected rev at the canvas's
// layout size (its buffer = its CSS box x devicePixelRatio). A fixed 300 ms sleep raced this under
// load: no frame came for > 300 ms and the reference picture was the old one (BUG1).
// Options:
//   rev    where the expected rev comes from: "state" (default: the page's state, through Vite's
//          /src/bimble.ts), "mesh" (the last mesh loaded, __bbGl.meshRev: a built page has no
//          /src/), or a number.
//   frame  a frame count read before the action (__bbGl.frames): also wait for a draw after it (a
//          camera move changes no rev).
//   ms     the timeout (default 10000); throws "timeout waiting for <what>".
// Lives in test/lib/, not test/: ci/run.sh runs every test/*.mjs as a suite.

/** The viewport's frame loops so far (-1 before the first). */
export const loops = (evalJs) => evalJs("window.__bbGl?.loops ?? -1");

/** The page expression: true once the picture is the settled one. */
export function settledExpr(l0, { rev = "state", frame = null } = {}) {
  const want = rev === "state" ? "(await import('/src/bimble.ts')).state.value.rev" : rev === "mesh" ? "g.meshRev" : JSON.stringify(rev);
  return `(async () => { const c = document.getElementById('bimble'), g = window.__bbGl, d = g?.drawn, dpr = window.devicePixelRatio || 1;
    if (!g || !d || !c || !(g.loops > ${Number(l0)})) return false;
    ${frame === null ? "" : `if (!(d.frame > ${Number(frame)})) return false;`}
    return d.rev === ${want} && d.w === c.width && d.h === c.height
      && c.width === Math.max(1, Math.round(c.clientWidth * dpr)) && c.height === Math.max(1, Math.round(c.clientHeight * dpr)); })()`;
}

/** Resolves once settledExpr holds; polls every 25 ms. */
export async function settled(evalJs, what, l0, opts = {}) {
  const expr = settledExpr(l0, opts), until = Date.now() + (opts.ms ?? 10000);
  let last;
  while (Date.now() < until) {
    try {
      if (await evalJs(expr)) return true;
    } catch (e) {
      last = e;
    }
    await new Promise((r) => setTimeout(r, 25));
  }
  throw new Error(`timeout waiting for ${what}${last ? `: ${last.message ?? last}` : ""}`);
}
