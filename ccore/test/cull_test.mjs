// The viewport's batches and moving budget (ticket 3D1, ADR 0078: web-ui/src/gl/cull.ts), no browser:
//   node ccore/test/cull_test.mjs
// - nextMovingPx: Rust's two tests (bimble-viewer lib.rs the_moving_threshold_follows_the_frame_rate,
//   the_screen_refresh_caps_the_target), same inputs, same expectations.
// - frustumPlanes / boxInFrustum: a box in front of a perspective camera is in, one behind it, one far
//   to the side and one past the far plane are out; an orthographic camera likewise.
// - makeBatches: every item lands in exactly one batch, a batch holds at most the cap (unless one item
//   alone is larger), sizes descend inside a batch, cum / ecum add up, the batch box holds its items;
//   prefixCount equals a count of the sizes at least the threshold.
// Fails on the code before 3D1: web-ui/src/gl/cull.ts did not exist.
import { dirname, join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const C = await import(pathToFileURL(join(here, "../../web-ui/src/gl/cull.ts")).href);
let passed = 0, failed = 0;
const check = (what, ok, more = "") => { if (ok) passed++; else { failed++; console.log(`FAIL ${what} ${more}`); } };

// ---- Rust's tests of next_moving_px ----
{
  const inf = Infinity;
  let px = 0;
  for (let i = 0; i < 4; i++) px = C.nextMovingPx(px, 0.060, 30, inf);
  check("slow frames raise the threshold over 2 px", px > 2, String(px));
  check("a pause changes nothing", C.nextMovingPx(px, 1.0, 30, inf) === px);
  check("a frame on target changes nothing", C.nextMovingPx(px, 0.033, 30, inf) === px);
  for (let i = 0; i < 20; i++) px = C.nextMovingPx(px, 0.010, 30, inf);
  check("fast frames bring it back to 0", px === 0, String(px));
  check("at most 48 px", C.nextMovingPx(40, 0.2, 30, inf) <= 48);
  const hz60 = 1 / 60;
  for (const fps of [60, 120, 240]) {
    let p = 0;
    for (let i = 0; i < 50; i++) p = C.nextMovingPx(p, hz60, fps, hz60);
    check(`${fps} fps on a 60 Hz screen: nothing left out`, p === 0, String(p));
    p = C.nextMovingPx(p, 0.040, fps, hz60);
    check(`${fps} fps: a slow frame raises it`, p > 0);
    for (let i = 0; i < 20; i++) p = C.nextMovingPx(p, hz60, fps, hz60);
    check(`${fps} fps comes back`, p === 0, String(p));
  }
  check("120 Hz screen, 60 fps frames: thin", C.nextMovingPx(0, hz60, 120, 1 / 120) > 0);
}

// ---- frustum ----
{
  const persp = (fovy, aspect, n, f) => { const t = 1 / Math.tan(fovy / 2), nf = 1 / (n - f); return new Float32Array([t / aspect, 0, 0, 0, 0, t, 0, 0, 0, 0, (f + n) * nf, -1, 0, 0, 2 * f * n * nf, 0]); };
  const P = C.frustumPlanes(persp(Math.PI / 3, 1, 1, 1000)); // the eye at 0 looking down -z
  const box = (x0, y0, z0, x1, y1, z1) => new Float32Array([x0, y0, z0, x1, y1, z1]);
  check("in front: in", C.boxInFrustum(P, box(-1, -1, -11, 1, 1, -9), 0));
  check("behind the eye: out", !C.boxInFrustum(P, box(-1, -1, 9, 1, 1, 11), 0));
  check("far to the side: out", !C.boxInFrustum(P, box(100, -1, -11, 102, 1, -9), 0));
  check("past the far plane: out", !C.boxInFrustum(P, box(-1, -1, -2000, 1, 1, -1500), 0));
  check("straddling the near plane: in", C.boxInFrustum(P, box(-1, -1, -2, 1, 1, 5), 0));
  const ortho = new Float32Array([0.1, 0, 0, 0, 0, 0.1, 0, 0, 0, 0, -0.01, 0, 0, 0, -1, 1]); // x, y in -10..10, z in -200..0
  const O = C.frustumPlanes(ortho);
  check("orthographic: in", C.boxInFrustum(O, box(-1, -1, -50, 1, 1, -40), 0));
  check("orthographic: beside, out", !C.boxInFrustum(O, box(11, -1, -50, 12, 1, -40), 0));
}

// ---- batches ----
{
  let seed = 7;
  const rnd = () => ((seed = (seed * 1103515245 + 12345) % 2147483648) / 2147483648);
  const n = 3000, boxes = new Float32Array(6 * n), tris = [], segs = [];
  for (let e = 0; e < n; e++) {
    const x = rnd() * 1e5, y = rnd() * 1e5, z = rnd() * 2e4, s = 10 + rnd() * (e % 50 === 0 ? 5000 : 500);
    boxes.set([x, y, z, x + s, y + s * rnd(), z + s], 6 * e);
    tris.push(e % 997 === 0 ? 50000 : 12 + Math.floor(rnd() * 500));
    segs.push(Math.floor(rnd() * 40));
  }
  const items = [...Array(n).keys()].filter((e) => e % 7 !== 3);
  const { order, batches } = C.makeBatches(items, boxes, (e) => tris[e], (e) => segs[e], C.BATCH_TRIS);
  check("every item once", order.length === items.length && new Set(order).size === items.length && order.every((e) => items.includes(e)));
  let ok = true, contiguous = true, at = 0;
  for (const b of batches) {
    contiguous &&= b.first === at;
    at += b.n;
    const part = order.slice(b.first, b.first + b.n);
    const t = part.reduce((a, e) => a + tris[e], 0);
    ok &&= b.n === 1 || t <= C.BATCH_TRIS;
    ok &&= b.cum[b.n] === t && b.ecum[b.n] === part.reduce((a, e) => a + segs[e], 0);
    for (let k = 1; k < b.n; k++) ok &&= b.size[k - 1] >= b.size[k];
    for (const e of part) for (let a = 0; a < 3; a++) ok &&= b.box[a] <= boxes[6 * e + a] && b.box[3 + a] >= boxes[6 * e + 3 + a];
    for (const r of [0, 50, 200, 400, 1e9]) ok &&= C.prefixCount(b, r) === Array.from(b.size).filter((s) => s >= r).length;
  }
  check("batches contiguous, under the cap, largest first, sums and boxes right, prefixes counted", ok && contiguous && at === items.length);
  check(`several batches (${batches.length})`, batches.length > 10);
}

console.log(`cull: ${passed} passed, ${failed} failed`);
process.exitCode = failed ? 1 : 0;
