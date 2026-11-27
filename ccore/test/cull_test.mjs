// The viewport's batches and moving budget (ticket 3D1, ADR 0078: web-ui/src/gl/cull.ts), no browser:
//   node ccore/test/cull_test.mjs
// - budgetCut (ADR 0078 decision 4, amended: never an empty picture while moving): on the 20,026-element
//   model of bench/large_model.py (its BMSH from out/tcc/bimble-ccore, element boxes as the page makes
//   them, batches as the page cuts them), seen from the home camera at the fitted distance: a moving
//   frame's cut draws more than 0 triangles and at most the budget (MIN_MOVING_TRIS, and 4x it); the
//   items drawn are the largest on screen (nothing left out is larger than something drawn, per
//   batch); slices of the same budget from where the last one stopped end with every triangle drawn
//   once. A synthetic case: one item larger than the budget is still drawn alone (never zero).
// - frustumPlanes / boxInFrustum: a box in front of a perspective camera is in, one behind it, one far
//   to the side and one past the far plane are out; an orthographic camera likewise.
// - makeBatches: every item lands in exactly one batch, a batch holds at most the cap (unless one item
//   alone is larger), sizes descend inside a batch, cum / ecum add up, the batch box holds its items;
//   prefixCount equals a count of the sizes at least the threshold.
// - 3D2 (HLOD proxies, web-ui/src/gl/proxies.ts, ccore/src/hlod.h): the same model's proxies served in steps
//   (GET /mesh?proxies=N: 202 until built, then 200 with BPRX), parsed: every element in one leaf, a
//   cell's leaves hold its children's; from far away the proxies stand for every leaf, at 0 px for none;
//   a hidden element keeps every cell above it from standing; stand-ins never cover a leaf twice.
// - VIS2 (the render origin, web-ui/src/gl/origin.ts): a link placed 2,600 km from the scene's origin (georeferenced
//   links in an empty project), a long beam turned 30 degrees and a thin plate 20 mm in front of a wall, seen from 3 m:
//   the GPU's f32 arithmetic emulated (placement, then u_mvp) puts every vertex within 1 mm of its f64 projection with
//   relMvp and the render origin (the old arithmetic, f32 matrix and coordinates: hundreds of mm off, checked too so
//   the case stays one that catches it); element boxes placed into f64 hold the placed vertices (f32: they do not);
//   the plate is not hidden behind the wall's depth as the GPU draws it (boxHidden, gl/hiz.ts).
// Fails on the code before 3D1: web-ui/src/gl/cull.ts did not exist (3D2: gl/proxies.ts, /mesh?proxies; VIS2:
// gl/origin.ts, and xfBox wrote f32 boxes only).
import { execFileSync, spawn } from "node:child_process";
import { existsSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const C = await import(pathToFileURL(join(here, "../../web-ui/src/gl/cull.ts")).href);
let passed = 0, failed = 0;
const check = (what, ok, more = "") => { if (ok) passed++; else failed++; console.log(`${ok ? "ok  " : "FAIL"} ${what} ${more}`); };

// ---- VIS2: georeferenced links, f32 on the GPU, f64 on the CPU ----
{
  const O = await import(pathToFileURL(join(here, "../../web-ui/src/gl/origin.ts")).href);
  const H = await import(pathToFileURL(join(here, "../../web-ui/src/gl/hiz.ts")).href);
  const f = Math.fround;
  const sub3 = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]], dot3 = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  const cross3 = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
  const nrm = (a) => { const l = Math.hypot(...a); return a.map((v) => v / l); };
  const mul4 = (a, b) => { const o = new Float64Array(16); for (let c = 0; c < 4; c++) for (let r = 0; r < 4; r++) { let x = 0; for (let k = 0; k < 4; k++) x += a[k * 4 + r] * b[c * 4 + k]; o[c * 4 + r] = x; } return o; };
  const look = (e, t) => { const fw = nrm(sub3(t, e)), sd = nrm(cross3(fw, [0, 0, 1])), up = cross3(sd, fw); return new Float64Array([sd[0], up[0], -fw[0], 0, sd[1], up[1], -fw[1], 0, sd[2], up[2], -fw[2], 0, -dot3(sd, e), -dot3(up, e), dot3(fw, e), 1]); };
  const n = 100, far = 200000, persp = (() => { const t = 1 / Math.tan(Math.PI / 6), nf = 1 / (n - far); return new Float64Array([t / 1.6, 0, 0, 0, 0, t, 0, 0, 0, 0, (far + n) * nf, -1, 0, 0, 2 * far * n * nf, 0]); })();
  // the link's placement (synthetic: 2,600 km east, 1,200 km north, 450 m up) and the camera 3 m from the wall
  const t = [2600000123.4, 1200000456.7, 450000], xf = { c: Math.cos(0.3), s: Math.sin(0.3), t };
  const placed = (p) => [xf.c * p[0] - xf.s * p[1] + t[0], xf.s * p[0] + xf.c * p[1] + t[1], p[2] + t[2]];
  const target = placed([10000, 0, 1500]), eye = placed([10000, -3000, 1700]);
  const mvp = mul4(persp, look(eye, target));
  const ndc = (m, p) => { const c = [0, 1, 2, 3].map((r) => m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r]); return [c[0] / c[3], c[1] / c[3], c[2] / c[3], c[3]]; };
  const ndc32 = (m, p) => { const c = [0, 1, 2, 3].map((r) => f(f(f(f(m[r] * p[0]) + f(m[4 + r] * p[1])) + f(m[8 + r] * p[2])) + m[12 + r])); return [c[0] / c[3], c[1] / c[3], c[2] / c[3], c[3]]; };
  // the shader: p = R p_local + (t - ro) in f32 (u_m rows), then u_mvp p (f32); the old one: + t, the f32 matrix of the scene frame
  const gpuNew = (pl, ro) => { const tr = [f(t[0] - ro[0]), f(t[1] - ro[1]), f(t[2] - ro[2])], c = f(xf.c), s = f(xf.s); return ndc32(O.relMvp(mvp, ro), [f(f(f(c * pl[0]) - f(s * pl[1])) + tr[0]), f(f(f(s * pl[0]) + f(c * pl[1])) + tr[1]), f(pl[2] + tr[2])]); };
  const gpuOld = (pl) => { const c = f(xf.c), s = f(xf.s); return ndc32(Float32Array.from(mvp), [f(f(f(c * pl[0]) - f(s * pl[1])) + f(t[0])), f(f(f(s * pl[0]) + f(c * pl[1])) + f(t[1])), f(pl[2] + f(t[2]))]); };
  const depthOf = (zn) => (2 * far * n) / (far + n - zn * (far - n)); // NDC z back to the distance along the view (mm)
  const ro = O.renderOrigin([(eye[0] + target[0]) / 2, (eye[1] + target[1]) / 2, (eye[2] + target[2]) / 2]);
  check("VIS2: the render origin of a scene 2,600 km away is near it (1 m grid), 0 for a scene near its origin",
    Math.abs(ro[0] - target[0]) < 5000 && ro[0] % 1000 === 0 && O.renderOrigin([5e5, -2e5, 1e4]).every((v) => v === 0));
  // a long beam (12 m, turned 30 degrees about z in the link, 45 degrees up) and points across the wall
  const pts = [];
  for (let i = 0; i <= 12; i++) pts.push([9000 + 1000 * i * Math.cos(Math.PI / 6), 1000 * i * Math.sin(Math.PI / 6) * 0.1, 800 + 50 * i]);
  for (let i = 0; i < 9; i++) pts.push([8000 + 500 * i, 0, 1000 + 120 * i]);
  let errNew = 0, errOld = 0;
  for (const pl of pts) {
    const exact = ndc(mvp, placed(pl)), a = gpuNew(pl, ro), b = gpuOld(pl);
    const mmPerNdc = exact[3] * Math.tan(Math.PI / 6); // NDC y of 1 spans this many mm at the point's depth
    const e1 = Math.max(Math.abs(a[0] - exact[0]) * mmPerNdc * 1.6, Math.abs(a[1] - exact[1]) * mmPerNdc, Math.abs(depthOf(a[2]) - depthOf(exact[2])));
    const e2 = Math.max(Math.abs(b[0] - exact[0]) * mmPerNdc * 1.6, Math.abs(b[1] - exact[1]) * mmPerNdc, Math.abs(depthOf(b[2]) - depthOf(exact[2])));
    errNew = Math.max(errNew, e1), errOld = Math.max(errOld, e2);
  }
  check(`VIS2: vertices 2,600 km from the origin drawn within 1 mm of their f64 projection with the render origin (${errNew.toFixed(3)} mm)`, errNew < 1);
  check(`VIS2: (the case catches the old arithmetic: f32 matrix and coordinates put them ${errOld.toFixed(0)} mm off)`, errOld > 50);
  // element boxes: a placed box in f64 holds the placed vertices; rounded to f32 it does not
  const lb = new Float32Array([9000, -50, 790, 19400, 650, 1460]);
  const b64 = new Float64Array(6), b32 = new Float32Array(6);
  C.xfBox(xf, lb, 0, b64, 0), C.xfBox(xf, lb, 0, b32, 0);
  const corners = [0, 1, 2, 3].map((k) => placed([k & 1 ? lb[3] : lb[0], k & 2 ? lb[4] : lb[1], lb[2]]));
  const holds = (b) => corners.every((p) => p[0] >= b[0] && p[0] <= b[3] && p[1] >= b[1] && p[1] <= b[4]);
  check("VIS2: a link's element box placed into f64 holds its placed corners (into f32: it does not)", holds(b64) && !holds(b32));
  // a thin plate 20 mm in front of a wall, seen through the wall's depth as the GPU draws it: not hidden
  const wall = [[8000, 0, 600], [14000, 0, 600], [8000, 0, 2600], [14000, 0, 2600]].map((p) => gpuNew(p, ro));
  const wd = Math.min(...wall.map((q) => (q[2] + 1) / 2));
  const W = 64, levels = [new Float32Array(W * W).fill(wd)];
  for (let w = W; w > 1; w /= 2) levels.push(new Float32Array((w / 2) * (w / 2)).fill(wd));
  const pyr = { key: "", serial: 1, complete: true, w: W, h: W, cw: 4 * W, ch: 4 * W, mvp, levels, lw: levels.map((_, i) => W >> i), lh: levels.map((_, i) => W >> i) };
  const plate = new Float32Array([10000, -25, 1200, 10400, -20, 1600]), pb = new Float64Array(6);
  C.xfBox(xf, plate, 0, pb, 0);
  const behind = new Float32Array([10000, 200, 1200, 10400, 220, 1600]), bb = new Float64Array(6);
  C.xfBox(xf, behind, 0, bb, 0);
  check("VIS2: a plate 20 mm in front of the wall is not hidden by the wall's depth; a part 200 mm behind it is", !H.boxHidden(pyr, pb, 0) && H.boxHidden(pyr, bb, 0));
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

// ---- the moving budget on the 20,026-element model ----
{
  const one = { first: 0, n: 2, size: new Float32Array([5000, 10]), cum: new Uint32Array([0, 90000, 90012]), ecum: new Uint32Array(3), box: new Float32Array(6) };
  const c1 = C.budgetCut([{ b: one, dist: 1000, k0: 0 }], 20000);
  check("one item over the budget: drawn alone, never zero", c1.tris === 90000, JSON.stringify(c1));
  const BIN = process.env.HOST_BIN ?? join(here, "../out/tcc/bimble-ccore");
  if (!existsSync(BIN)) check(`the host ${BIN} (ccore/build.sh tcc)`, false);
  else {
    const { port } = await import("./ports.mjs");
    const { hostEnv } = await import("./lib/host_env.mjs");
    const tmp = mkdtempSync(join(tmpdir(), "cull-test-")), model = join(tmp, "large.bimble"), PORT = port("cull_test"), TOKEN = "cull";
    execFileSync("python3", [join(here, "../bench/large_model.py"), "20000", model]);
    const host = spawn(BIN, [String(PORT), model], { stdio: ["ignore", "ignore", "pipe"], env: hostEnv({ BIMBLE_TOKEN: TOKEN }) });
    let log = "";
    host.stderr.on("data", (d) => (log += d));
    try {
      for (let i = 0; i < 4000 && !log.includes("listening") && host.exitCode === null; i++) await new Promise((r) => setTimeout(r, 5));
      const ab = await (await fetch(`http://127.0.0.1:${PORT}/mesh?t=${TOKEN}`)).arrayBuffer(); // version 1: every mesh placed
      const dv = new DataView(ab), u = (o) => dv.getUint32(o, true);
      const nE = u(12), nV = u(16), pos = new Float32Array(ab, 52 + 48 * nE, 3 * nV);
      const boxes = new Float32Array(6 * nE), tris = [], segs = [];
      for (let e = 0; e < nE; e++) {
        const o = 52 + 48 * e, v0 = u(o + 24), nv = u(o + 28);
        tris.push(u(o + 36) / 3), segs.push(u(o + 44));
        const b = [Infinity, Infinity, Infinity, -Infinity, -Infinity, -Infinity];
        for (let v = v0; v < v0 + nv; v++) for (let a = 0; a < 3; a++) b[a] = Math.min(b[a], pos[3 * v + a]), b[3 + a] = Math.max(b[3 + a], pos[3 * v + a]);
        boxes.set(b, 6 * e);
      }
      check("the 20,026-element model", nE === 20026, String(nE));
      const { batches } = C.makeBatches([...Array(nE).keys()], boxes, (e) => tris[e], (e) => segs[e], C.BATCH_TRIS);
      const total = tris.reduce((a, b) => a + b, 0);
      // the page's fit (glviewport.ts fit) and home camera (viewcube.ts HOME_YAW, HOME_PITCH)
      const lo = [0, 1, 2].map((a) => Math.min(...batches.map((b) => b.box[a]))), hi = [0, 1, 2].map((a) => Math.max(...batches.map((b) => b.box[3 + a])));
      const centre = lo.map((x, a) => (x + hi[a]) / 2), radius = Math.hypot(...lo.map((x, a) => hi[a] - x)) / 2;
      const fov = (35 * Math.PI) / 180, aspect = 984 / 547, dist = (1.05 * radius) / Math.sin(Math.min(fov / 2, Math.atan(Math.tan(fov / 2) * aspect)));
      const yaw = (-55 * Math.PI) / 180, pitch = (28 * Math.PI) / 180;
      const eye = [centre[0] + dist * Math.cos(pitch) * Math.cos(yaw), centre[1] + dist * Math.cos(pitch) * Math.sin(yaw), centre[2] + dist * Math.sin(pitch)];
      const cutsOf = (k0s) => batches.map((b, i) => {
        const q = [0, 1, 2].map((a) => Math.min(Math.max(eye[a], b.box[a]), b.box[3 + a]));
        return { b, dist: Math.max(1, Math.hypot(q[0] - eye[0], q[1] - eye[1], q[2] - eye[2])), k0: k0s ? k0s[i] : 0 };
      });
      for (const budget of [C.MIN_MOVING_TRIS, 4 * C.MIN_MOVING_TRIS]) {
        const cs = cutsOf(null), cut = C.budgetCut(cs, budget);
        const drawn = cs.reduce((a, c) => a + c.b.cum[C.cutEnd(c, cut.t)], 0);
        check(`budget ${budget}: a moving frame draws ${drawn} of ${total} triangles (> 0, <= the budget)`, drawn > 0 && drawn <= budget && drawn === cut.tris);
        let largest = true;
        for (const c of cs) {
          const k = C.cutEnd(c, cut.t);
          if (k > 0 && k < c.b.n) largest &&= c.b.size[k - 1] / c.dist >= c.b.size[k] / c.dist;
          if (k < c.b.n) largest &&= c.b.size[k] / c.dist < cut.t || c.b.size[k] === 0; // what is left out is under the cut
        }
        check(`budget ${budget}: the largest on screen first`, largest);
      }
      // VIS1 (P0-c): the bucketed cut gives the same kind of answer: at most the budget, never empty, the largest
      // first, and close to the bisection's (its threshold within two buckets: an eighth of an octave); and fast
      for (const budget of [C.MIN_MOVING_TRIS, 4 * C.MIN_MOVING_TRIS, 1000000]) {
        const cs = cutsOf(null), a = C.budgetCut(cs, budget), b = C.budgetCutFast(cs, budget, "k");
        const drawn = cs.reduce((n, c) => n + c.b.cum[C.cutEnd(c, b.t)], 0);
        let largest = true;
        for (const c of cs) { const k = C.cutEnd(c, b.t); if (k < c.b.n) largest &&= c.b.size[k] / c.dist < b.t || c.b.size[k] === 0; }
        check(`VIS1: budgetCutFast ${budget}: ${drawn} triangles (bisection ${a.tris}), threshold ${b.t.toPrecision(3)} against ${a.t.toPrecision(3)}`,
          drawn > 0 && drawn <= Math.max(budget, a.tris) && drawn === b.tris && largest && (a.t === 0 ? b.t === 0 : b.t >= a.t / 1.0001 && b.t <= a.t * 2 ** (2 / 16)));
      }
      {
        const cs = cutsOf(null), many = Array.from({ length: 5200 }, () => cs).flat(); // 5,200 times the batches: a project of links' count (46,800)
        let t0 = performance.now();
        C.budgetCut(many, C.MIN_MOVING_TRIS);
        const slow = performance.now() - t0;
        C.budgetCutFast(many, C.MIN_MOVING_TRIS, "warm");
        t0 = performance.now();
        C.budgetCutFast(many, C.MIN_MOVING_TRIS, "warm");
        const fast = performance.now() - t0;
        check(`VIS1: ${many.length} batches: the bucketed cut ${fast.toFixed(1)} ms against the bisection ${slow.toFixed(1)} ms`, fast < slow / 2);
      }
      // slices of the budget from where the last stopped: every triangle once
      let k0s = batches.map(() => 0), sum = 0, slices = 0, over = 0;
      for (; slices < 1000; slices++) {
        const cs = cutsOf(k0s), cut = C.budgetCut(cs, C.MIN_MOVING_TRIS);
        if (cut.tris > C.MIN_MOVING_TRIS) over++;
        sum += cut.tris;
        k0s = cs.map((c) => C.cutEnd(c, cut.t));
        if (cut.t === 0) break;
      }
      check(`slices of ${C.MIN_MOVING_TRIS}: ${slices + 1} slices draw every triangle once (${sum} of ${total}), ${over} over the budget (a single item larger)`, sum === total && k0s.every((k, i) => k === batches[i].n));
      { // VIS1: the fill's slices through the bucketed cut too: every triangle once, each slice within the budget
        let k0f = batches.map(() => 0), sumF = 0, n = 0, overF = 0;
        for (; n < 1000; n++) {
          const cs = cutsOf(k0f), cut = C.budgetCutFast(cs, C.MIN_MOVING_TRIS, "s");
          if (cut.tris > C.MIN_MOVING_TRIS) overF++;
          sumF += cut.tris;
          k0f = cs.map((c) => C.cutEnd(c, cut.t));
          if (cut.t === 0) break;
        }
        check(`VIS1: slices through budgetCutFast: ${n + 1} slices draw every triangle once (${sumF} of ${total}), ${overF} over the budget`, sumF === total && k0f.every((k, i) => k === batches[i].n) && overF <= over);
      }
      // ---- 3D2: the HLOD proxies of the same model, served in steps (GET /mesh?proxies=N) ----
      const X = await import(pathToFileURL(join(here, "../../web-ui/src/gl/proxies.ts")).href);
      let r, steps = 0, progress = "";
      for (; steps < 1000; steps++) {
        r = await fetch(`http://127.0.0.1:${PORT}/mesh?t=${TOKEN}&proxies=20000`);
        if (r.status !== 202) break;
        progress = await r.text();
      }
      check(`proxies: built in ${steps + 1} steps of 20,000 triangles (the last 202 said "${progress}"), then 200`, r.status === 200 && steps > 3 && /^\d+ \d+$/.test(progress));
      const P = X.parseBprx(await r.arrayBuffer());
      const again = await fetch(`http://127.0.0.1:${PORT}/mesh?t=${TOKEN}&proxies=1`);
      check("proxies: the same bytes again once built", again.status === 200 && (await again.arrayBuffer()).byteLength > 0);
      const inLeaf = Array.from(P.leafOf).filter((l) => l >= 0).length;
      const withProxy = P.cells.filter((c) => c.parts.length).length, ptris = P.cells.reduce((a, c) => a + c.parts.reduce((n, q) => n + q.ni / 3, 0), 0);
      check(`proxies: ${P.cells.length} cells, ${withProxy} with a proxy (${ptris} triangles), ${P.leafCount} leaves; every element in one leaf`,
        P.nElements === nE && inLeaf === nE && P.cells[0].leaves[0] === 0 && P.cells[0].leaves[1] === P.leafCount && withProxy > 0);
      let ranges = true;
      for (const c of P.cells) for (const k of c.children) ranges &&= P.cells[k].leaves[0] >= c.leaves[0] && P.cells[k].leaves[1] <= c.leaves[1] && P.cells[k].depth === c.depth + 1;
      check("proxies: a cell's leaves hold its children's", ranges);
      const all = X.usableCells(P, () => true);
      const eyeFar = eye.map((x, a) => centre[a] + 50 * (x - centre[a])); // far away: the coarsest proxies stand
      const k = (2 * Math.tan(fov / 2)) / 800, mmPerPx = (d) => d * k;
      const far = X.cutProxies(P, all, null, eyeFar, mmPerPx, 3);
      const covered = far.replaced.reduce((a, x) => a + x, 0);
      check(`proxies: from 50 times the fitted distance, ${far.cells.length} cells stand for ${covered} of ${P.leafCount} leaves`, far.cells.length > 0 && covered === P.leafCount);
      check("proxies: 0 px, none", X.cutProxies(P, all, null, eyeFar, mmPerPx, 0).cells.length === 0);
      { // fitProxies: under a cap of 0 every cell gives way (its leaves back to their batches); under a large one nothing changes
        const a = X.cutProxies(P, all, null, eyeFar, mmPerPx, 3), z = X.fitProxies(P, all, a.cells, a.replaced, 0);
        const b = X.cutProxies(P, all, null, eyeFar, mmPerPx, 3), y = X.fitProxies(P, all, b.cells, b.replaced, 1e12);
        check("proxies: fitted under 0 triangles none stands, under plenty all stay", z.length === 0 && a.replaced.every((x) => x === 0) && y.length === b.cells.length);
      }
      const near = X.cutProxies(P, all, null, eye, mmPerPx, 3), nearTris = near.cells.reduce((a, c) => a + P.cells[c].parts.reduce((n, q) => n + q.ni / 3, 0), 0);
      console.log(`  at the fitted distance, 3 px: ${near.cells.length} cells, ${nearTris} proxy triangles; 1 px: ${X.cutProxies(P, all, null, eye, mmPerPx, 1).cells.length} cells`);
      // a hidden element: no cell above it stands for it
      const hid = P.elements[0], notHid = X.usableCells(P, (e) => e !== hid);
      let ok = true;
      P.cells.forEach((c, i) => { const has = Array.from(P.elements.subarray(c.el0, c.el0 + c.nel)).includes(hid); if (has && notHid[i]) ok = false; if (!has && notHid[i] !== all[i]) ok = false; });
      const hidFar = X.cutProxies(P, notHid, null, eyeFar, mmPerPx, 3);
      check("proxies: a hidden element keeps every cell above it from standing (the others unchanged), its leaf drawn as it is", ok && !hidFar.replaced[P.leafOf[hid]]);
      // stand-ins for unfinished leaves: the shallowest usable cells, no leaf twice
      const rep = new Uint8Array(P.leafCount), si = X.standIns(P, all, [...Array(P.leafCount).keys()], rep);
      let twice = false; const seenL = new Uint8Array(P.leafCount);
      for (const c of si) for (let l = P.cells[c].leaves[0]; l < P.cells[c].leaves[1]; l++) { if (seenL[l]) twice = true; seenL[l] = 1; }
      check(`proxies: stand-ins for every leaf: ${si.length} cells, no leaf twice`, si.length > 0 && !twice);
    } finally {
      host.kill("SIGKILL");
      rmSync(tmp, { recursive: true, force: true });
    }
  }
}

console.log(`cull: ${passed} passed, ${failed} failed`);
process.exitCode = failed ? 1 : 0;
