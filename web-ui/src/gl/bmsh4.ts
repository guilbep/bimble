// SIZE2: BMSH version 4 (ccore/src/scene_bmsh.h): version 3 with its geometry in the compact mesh codec
// (ccore/src/meshcodec.h, "BMG1": 1/16 mm steps from each block's lower corner, varint deltas), what the
// native host serves a link as (GET /mesh?link=k): about a third of version 3's bytes. Read back here into
// the version 3 bytes it stands for (the C reader is cc_bmsh4_to_bmsh3: the same answer, the same checks),
// so the viewport parses one form. Every count is checked against the header and what is left; a damaged
// answer throws. The decode yields to the page every SLICE_MS (ADR 0074: no long task) when `yieldTo` is given.

const STEP = 0.0625; // 1/16 mm (meshcodec.c STEP)
const SLICE_MS = 25;

class Rd {
  o: number;
  constructor(readonly b: Uint8Array, o: number, readonly end: number) { this.o = o; }
  byte(): number { if (this.o >= this.end) throw new Error("BMSH 4: block cut short"); return this.b[this.o++]!; }
  varint(): number {
    let v = 0, mul = 1;
    for (let i = 0; i < 10; i++) {
      const x = this.byte();
      v += (x & 0x7f) * mul;
      if (x < 0x80) return v;
      mul *= 128;
    }
    throw new Error("BMSH 4: varint too long");
  }
  zig(): number { const v = this.varint(); return v % 2 === 0 ? v / 2 : -(v + 1) / 2; }
  f32(dv: DataView): number { if (this.o + 4 > this.end) throw new Error("BMSH 4: block cut short"); const f = dv.getFloat32(this.o, true); this.o += 4; return f; }
}

/** One BMG1 mesh at [at, end) into the version 3 arrays at the block's first vertex, index and segment. */
function block(b: Uint8Array, dv: DataView, at: number, end: number, out: { pos: Float32Array; idx: Uint32Array; edges: Float32Array; normals: Int8Array },
  fv: number, fi: number, fs: number): { nv: number; ni: number; ns: number } {
  const r = new Rd(b, at, end);
  if (end - at < 5 || b[at] !== 0x42 || b[at + 1] !== 0x4d || b[at + 2] !== 0x47 || b[at + 3] !== 0x31) throw new Error("BMSH 4: not a BMG1 block");
  r.o += 4;
  const mode = r.byte();
  if (mode > 1) throw new Error("BMSH 4: unknown mode");
  const nv = r.varint(), ni = r.varint(), ne = r.varint(), nn = r.varint();
  if (nn > ne || ni % 3 !== 0) throw new Error("BMSH 4: counts");
  if (nv * (mode ? 3 : 12) + ni + 2 * ne + 2 * nn > end - r.o) throw new Error("BMSH 4: block cut short");
  if (3 * (fv + nv) > out.pos.length || fi + ni > out.idx.length || 6 * (fs + ne) > out.edges.length) throw new Error("BMSH 4: blocks past the counts");
  const lo = [0, 0, 0];
  if (mode) for (let k = 0; k < 3; k++) lo[k] = r.f32(dv);
  const P = out.pos, f = Math.fround;
  const prev = [0, 0, 0];
  const point = (o: Float32Array, at: number, pv: number[]) => {
    for (let k = 0; k < 3; k++) {
      if (mode) { pv[k] = pv[k]! + r.zig(); o[at + k] = f(lo[k]! + pv[k]! * STEP); } else o[at + k] = r.f32(dv);
    }
  };
  for (let i = 0; i < nv; i++) point(P, 3 * (fv + i), prev);
  let last = 0;
  for (let i = 0; i < ni; i++) {
    last += r.zig();
    if (last < 0 || last >= nv) throw new Error("BMSH 4: index out of range");
    out.idx[fi + i] = fv + last;
  }
  let elast = 0;
  const eprev = [0, 0, 0], E = out.edges;
  for (let e = 0; e < ne; e++)
    for (let j = 0; j < 2; j++) {
      const c = r.varint(), o = 6 * (fs + e) + 3 * j;
      if (c === 1) { point(E, o, eprev); continue; }
      if (c % 2) throw new Error("BMSH 4: edge code");
      const d = c / 2;
      elast += d % 2 === 0 ? d / 2 : -(d + 1) / 2;
      if (elast < 0 || elast >= nv) throw new Error("BMSH 4: edge vertex");
      const q = 3 * (fv + elast);
      E[o] = P[q]!, E[o + 1] = P[q + 1]!, E[o + 2] = P[q + 2]!;
    }
  const nt = r.varint();
  if (nt * 3 > end - r.o) throw new Error("BMSH 4: block cut short");
  const table = r.o;
  r.o += 3 * nt;
  const N = out.normals;
  for (let e = 0; e < nn; e++)
    for (let j = 0; j < 2; j++) {
      const i = r.varint();
      if (i >= nt) throw new Error("BMSH 4: normal");
      const o = 8 * (fs + e) + 4 * j, t = table + 3 * i;
      N[o] = (b[t]! << 24) >> 24, N[o + 1] = (b[t + 1]! << 24) >> 24, N[o + 2] = (b[t + 2]! << 24) >> 24, N[o + 3] = 0;
    }
  if (r.o !== end) throw new Error("BMSH 4: trailing bytes in a block");
  return { nv, ni, ns: ne };
}

/** BMSH version 4 bytes to the version 3 bytes they stand for (glviewport.ts parseBmsh reads those).
 *  `maxBytes`: a cap on what is written (a damaged count never allocates past it). */
export async function bmsh4ToBmsh3(ab: ArrayBuffer, maxBytes = 2 ** 31, yieldTo?: () => Promise<void>): Promise<ArrayBuffer> {
  const b = new Uint8Array(ab), dv = new DataView(ab), n = ab.byteLength;
  if (n < 52 || dv.getUint32(0, true) !== 0x48534d42 || dv.getUint32(4, true) !== 4) throw new Error("not BMSH version 4");
  const nE = dv.getUint32(12, true), nV = dv.getUint32(16, true), nI = dv.getUint32(20, true), nS = dv.getUint32(24, true);
  if (52 + 52 * nE + 4 > n) throw new Error("BMSH 4: cut short");
  const want = 52 + 48 * nE + 12 * nV + 4 * nI + 32 * nS + 52 * nE;
  if (want > maxBytes) throw new Error("BMSH 4: too large");
  const shapes = 52 + 48 * nE;
  let placed = 0;
  for (let k = 0; k < nE; k++) if (dv.getUint32(shapes + 4 * k, true)) placed++;
  let at = shapes + 4 * nE + 48 * placed;
  if (at + 4 > n) throw new Error("BMSH 4: cut short");
  const out = new ArrayBuffer(want), ob = new Uint8Array(out), odv = new DataView(out);
  ob.set(b.subarray(0, 52 + 48 * nE));
  odv.setUint32(4, 3, true);
  const P = 52 + 48 * nE, I = P + 12 * nV, E = I + 4 * nI, N = E + 24 * nS, T = N + 8 * nS;
  const arrays = { pos: new Float32Array(out, P, 3 * nV), idx: new Uint32Array(out, I, nI), edges: new Float32Array(out, E, 6 * nS), normals: new Int8Array(out, N, 8 * nS) };
  { // the placements, 52 bytes a row (zeros for an own mesh)
    let pl = shapes + 4 * nE;
    for (let k = 0; k < nE; k++) {
      const sh = dv.getUint32(shapes + 4 * k, true);
      odv.setUint32(T + 52 * k, sh, true);
      if (sh) ob.set(b.subarray(pl, pl + 48), T + 52 * k + 4), pl += 48;
    }
  }
  const nB = dv.getUint32(at, true);
  at += 4;
  let fv = 0, fi = 0, fs = 0, t0 = performance.now();
  for (let j = 0; j < nB; j++) {
    if (n - at < 4) throw new Error("BMSH 4: cut short");
    const len = dv.getUint32(at, true);
    at += 4;
    if (len > n - at) throw new Error("BMSH 4: cut short");
    const c = block(b, dv, at, at + len, arrays, fv, fi, fs);
    fv += c.nv, fi += c.ni, fs += c.ns, at += len;
    if (yieldTo && performance.now() - t0 > SLICE_MS) { await yieldTo(); t0 = performance.now(); }
  }
  if (fv !== nV || fi !== nI || fs !== nS) throw new Error("BMSH 4: blocks short of the counts");
  if (at !== n) throw new Error("BMSH 4: trailing bytes");
  return out;
}

// ---- SIZE3 (B): the decode in a Web Worker when the page can start one (gl/bmsh4.worker.ts), else here in
// slices (the wasm lab without workers, a test page) ----
let worker: Worker | null | undefined;
let nextId = 1;
const pending = new Map<number, { res: (b: ArrayBuffer) => void; rej: (e: Error) => void }>();
function decoder(): Worker | null {
  if (worker !== undefined) return worker;
  try {
    worker = typeof Worker === "undefined" ? null : new Worker(new URL("./bmsh4.worker.ts", import.meta.url), { type: "module" });
    if (worker) {
      worker.onmessage = (e: MessageEvent) => {
        const m = e.data as { id: number; out?: ArrayBuffer; rows?: Bmsh4Rows; error?: string };
        const r = rowsPending.get(m.id); // SIZE3: a rows answer
        if (r) {
          rowsPending.delete(m.id);
          if (m.rows) r.res(m.rows);
          else r.rej(new Error(m.error ?? "BMSH 4: the decoder failed"));
          return;
        }
        const p = pending.get(m.id);
        pending.delete(m.id);
        if (!p) return;
        if (m.out) p.res(m.out);
        else p.rej(new Error(m.error ?? "BMSH 4: the decoder failed"));
      };
      worker.onerror = () => { // a worker that cannot start (a blocked script): here from now on
        for (const p of pending.values()) p.rej(new Error("BMSH 4: the decoder failed"));
        for (const p of rowsPending.values()) p.rej(new Error("BMSH 4: the decoder failed"));
        pending.clear(), rowsPending.clear();
        worker?.terminate();
        worker = null;
      };
    }
  } catch { worker = null; }
  return worker;
}
/** Stats of the decodes (performance panel, tests): in a worker or on the page's thread. */
export const bmsh4Stats = { worker: 0, page: 0, maxPageSliceMs: 0 };

/** bmsh4ToBmsh3 off the page's thread when it can be (the bytes `ab` are transferred: unusable after). */
export async function bmsh4Decode(ab: ArrayBuffer, maxBytes = 2 ** 31): Promise<ArrayBuffer> {
  const w = decoder();
  if (w) {
    const id = nextId++;
    try {
      const out = await new Promise<ArrayBuffer>((res, rej) => { pending.set(id, { res, rej }); w.postMessage({ id, ab, max: maxBytes }, [ab]); });
      bmsh4Stats.worker++;
      return out;
    } catch (e) {
      if (!ab.byteLength) throw e; // the bytes went to the worker: its error stands (else: decoded here)
    }
  }
  bmsh4Stats.page++;
  let t0 = performance.now();
  return bmsh4ToBmsh3(ab, maxBytes, () => new Promise<void>((res) => {
    bmsh4Stats.maxPageSliceMs = Math.max(bmsh4Stats.maxPageSliceMs, performance.now() - t0);
    setTimeout(() => { t0 = performance.now(); res(); }, 0);
  }));
}

// ---- SIZE3 (B): a link's rows and boxes without its arrays ----
/** What a link's mesh stands on without its heavy arrays (glviewport.ts dropArrays' result): its rows as parseBmsh
 *  then bake give them (a shape of few copies baked: its own vertices appended), its placements, each element's
 *  box (its own frame: upload's arithmetic) and the lengths its arrays would have. */
export interface Bmsh4Rows {
  rev: number; origin: [number, number, number]; size: number; ids: string[]; cats: Uint8Array;
  v0: Uint32Array; nv: Uint32Array; i0: Uint32Array; ni: Uint32Array; s0: Uint32Array; ns: Uint32Array;
  shape: Uint32Array; place: Float32Array; boxes: Float32Array; counts: { nv: number; ni: number; ns: number };
}

/** The bytes version 3 of these version 4 bytes would take (from the header only). */
export function bmsh3Size(ab: ArrayBuffer): number {
  const dv = new DataView(ab);
  if (ab.byteLength < 52 || dv.getUint32(0, true) !== 0x48534d42 || dv.getUint32(4, true) !== 4) return Infinity;
  const nE = dv.getUint32(12, true), nV = dv.getUint32(16, true), nI = dv.getUint32(20, true), nS = dv.getUint32(24, true);
  return 52 + 48 * nE + 12 * nV + 4 * nI + 32 * nS + 52 * nE;
}

/** Version 4 bytes to their rows and boxes: each block decoded on its own into a scratch (the largest block's
 *  size at most), never the whole mesh. `minCopies`, `minTris`: glviewport.ts's bake rule (INSTANCE_MIN_*). Throws on
 *  damaged bytes, as bmsh4ToBmsh3. */
export function bmsh4Rows(ab: ArrayBuffer, minCopies: number, minTris: number): Bmsh4Rows {
  const b = new Uint8Array(ab), dv = new DataView(ab), n = ab.byteLength;
  if (n < 52 || dv.getUint32(0, true) !== 0x48534d42 || dv.getUint32(4, true) !== 4) throw new Error("not BMSH version 4");
  const rev = dv.getUint32(8, true), nE = dv.getUint32(12, true), nV = dv.getUint32(16, true), nI = dv.getUint32(20, true), nS = dv.getUint32(24, true);
  if (52 + 52 * nE + 4 > n) throw new Error("BMSH 4: cut short");
  const origin: [number, number, number] = [dv.getFloat64(28, true), dv.getFloat64(36, true), dv.getFloat64(44, true)];
  const ids: string[] = [], cats = new Uint8Array(nE), v0 = new Uint32Array(nE), nv = new Uint32Array(nE), i0 = new Uint32Array(nE), ni = new Uint32Array(nE);
  const s0 = new Uint32Array(nE), ns = new Uint32Array(nE), shape = new Uint32Array(nE), place = new Float32Array(12 * nE);
  for (let e = 0, o = 52; e < nE; e++, o += 48) {
    ids.push(String.fromCharCode(...b.subarray(o, o + 22)));
    cats[e] = b[o + 22] ?? 7;
    v0[e] = dv.getUint32(o + 24, true), nv[e] = dv.getUint32(o + 28, true), i0[e] = dv.getUint32(o + 32, true);
    ni[e] = dv.getUint32(o + 36, true), s0[e] = dv.getUint32(o + 40, true), ns[e] = dv.getUint32(o + 44, true);
    if (i0[e]! + ni[e]! > nI || v0[e]! + nv[e]! > nV || s0[e]! + ns[e]! > nS) throw new Error("BMSH row out of range");
  }
  const sh0 = 52 + 48 * nE;
  let pl = sh0 + 4 * nE;
  for (let e = 0; e < nE; e++) {
    shape[e] = dv.getUint32(sh0 + 4 * e, true);
    if (!shape[e]) continue;
    if (pl + 48 > n) throw new Error("BMSH 4: cut short");
    for (let k = 0; k < 12; k++) place[12 * e + k] = dv.getFloat32(pl + 4 * k, true);
    pl += 48;
  }
  // each block's elements: an element's first vertex lies in the block that starts at or before it
  const byV0 = Array.from({ length: nE }, (_, e) => e).sort((x, y) => v0[x]! - v0[y]! || x - y);
  const boxes = new Float32Array(6 * nE);
  let at = pl;
  if (at + 4 > n) throw new Error("BMSH 4: cut short");
  const nB = dv.getUint32(at, true);
  at += 4;
  let fv = 0, fi = 0, fs = 0, q = 0;
  let scratch = { pos: new Float32Array(0), idx: new Uint32Array(0), edges: new Float32Array(0), normals: new Int8Array(0) };
  const f = Math.fround;
  for (let j = 0; j < nB; j++) {
    if (n - at < 4) throw new Error("BMSH 4: cut short");
    const len = dv.getUint32(at, true);
    at += 4;
    if (len > n - at || len < 9) throw new Error("BMSH 4: cut short");
    // the block's counts (BMG1: magic, mode, varint nv ni ne nn): its scratch sized to it
    const r = { o: at + 5 };
    const vint = () => { let v = 0, mul = 1; for (let i = 0; i < 10; i++) { if (r.o >= at + len) throw new Error("BMSH 4: block cut short"); const x = b[r.o++]!; v += (x & 0x7f) * mul; if (x < 0x80) return v; mul *= 128; } throw new Error("BMSH 4: varint too long"); };
    const bv = vint(), bi = vint(), be = vint();
    if (bv > nV || bi > nI || be > nS) throw new Error("BMSH 4: counts");
    if (scratch.pos.length < 3 * bv || scratch.idx.length < bi || scratch.edges.length < 6 * be)
      scratch = { pos: new Float32Array(3 * bv), idx: new Uint32Array(bi), edges: new Float32Array(6 * be), normals: new Int8Array(8 * be) };
    const c = block(b, dv, at, at + len, scratch, 0, 0, 0);
    for (; q < nE && v0[byV0[q]!]! < fv + c.nv; q++) {
      const e = byV0[q]!;
      if (v0[e]! < fv || v0[e]! + nv[e]! > fv + c.nv) throw new Error("BMSH 4: a row across blocks");
      const p = shape[e] ? place.subarray(12 * e, 12 * e + 12) : null;
      let x0 = Infinity, y0 = Infinity, z0 = Infinity, x1 = -Infinity, y1 = -Infinity, z1 = -Infinity;
      const P = scratch.pos;
      for (let v = 3 * (v0[e]! - fv); v < 3 * (v0[e]! - fv + nv[e]!); v += 3) { // (upload's arithmetic: glviewport.ts elementBox)
        let x = P[v]!, y = P[v + 1]!, z = P[v + 2]!;
        if (p) {
          const qx = f(f(f(f(p[0]! * x) + f(p[3]! * y)) + f(p[6]! * z)) + p[9]!), qy = f(f(f(f(p[1]! * x) + f(p[4]! * y)) + f(p[7]! * z)) + p[10]!);
          z = f(f(f(f(p[2]! * x) + f(p[5]! * y)) + f(p[8]! * z)) + p[11]!), x = qx, y = qy;
        }
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
        if (z < z0) z0 = z;
        if (z > z1) z1 = z;
      }
      boxes[6 * e] = x0, boxes[6 * e + 1] = y0, boxes[6 * e + 2] = z0, boxes[6 * e + 3] = x1, boxes[6 * e + 4] = y1, boxes[6 * e + 5] = z1;
    }
    fv += c.nv, fi += c.ni, fs += c.ns, at += len;
  }
  if (fv !== nV || fi !== nI || fs !== nS) throw new Error("BMSH 4: blocks short of the counts");
  if (at !== n) throw new Error("BMSH 4: trailing bytes");
  for (; q < nE; q++) { const e = byV0[q]!; if (nv[e]) throw new Error("BMSH 4: a row past the blocks"); boxes.set([Infinity, Infinity, Infinity, -Infinity, -Infinity, -Infinity], 6 * e); }
  // bake's rows (glviewport.ts bake): a shape of few copies or few triangles becomes own vertices appended
  const copies = new Map<number, number>();
  for (let e = 0; e < nE; e++) if (shape[e]) copies.set(shape[e]!, (copies.get(shape[e]!) ?? 0) + 1);
  let v = nV, i = nI, sg = nS;
  for (let e = 0; e < nE; e++) {
    if (!shape[e] || !((copies.get(shape[e]!) ?? 0) < minCopies || ni[e]! < 3 * minTris)) continue;
    v0[e] = v, i0[e] = i, s0[e] = sg, shape[e] = 0;
    v += nv[e]!, i += ni[e]!, sg += ns[e]!;
  }
  const size = 52 + 48 * nE + 12 * nV + 4 * nI + 32 * nS + 52 * nE;
  return { rev, origin, size, ids, cats, v0, nv, i0, ni, s0, ns, shape, place, boxes, counts: { nv: v, ni: i, ns: sg } };
}

/** bmsh4Rows off the page's thread when it can be (`ab` transferred). */
export async function bmsh4RowsDecode(ab: ArrayBuffer, minCopies: number, minTris: number): Promise<Bmsh4Rows> {
  const w = decoder();
  if (w) {
    const id = nextId++;
    try {
      const out = await new Promise<Bmsh4Rows>((res, rej) => {
        rowsPending.set(id, { res, rej });
        w.postMessage({ id, ab, rows: [minCopies, minTris] }, [ab]);
      });
      bmsh4Stats.worker++;
      return out;
    } catch (e) {
      if (!ab.byteLength) throw e;
    }
  }
  bmsh4Stats.page++;
  return bmsh4Rows(ab, minCopies, minTris);
}
const rowsPending = new Map<number, { res: (r: Bmsh4Rows) => void; rej: (e: Error) => void }>();

/** SIZE3 (B): the host's rows answer of a link (GET /mesh?link=k&rows=1, ccore/session/mesh.h cc_link_rows_answer):
 *  version 4's rows, shapes and placements with no block, then "BOXS" and each element's box (the page's arithmetic,
 *  computed by the host): bmsh4Rows' result without decoding any mesh. Throws on damaged bytes. */
export function bmsh4RowsBoxes(ab: ArrayBuffer, minCopies: number, minTris: number): Bmsh4Rows {
  const b = new Uint8Array(ab), dv = new DataView(ab), n = ab.byteLength;
  if (n < 52 || dv.getUint32(0, true) !== 0x48534d42 || dv.getUint32(4, true) !== 4) throw new Error("not BMSH version 4");
  const rev = dv.getUint32(8, true), nE = dv.getUint32(12, true), nV = dv.getUint32(16, true), nI = dv.getUint32(20, true), nS = dv.getUint32(24, true);
  if (52 + 52 * nE + 4 > n) throw new Error("BMSH 4: cut short");
  const origin: [number, number, number] = [dv.getFloat64(28, true), dv.getFloat64(36, true), dv.getFloat64(44, true)];
  const ids: string[] = [], cats = new Uint8Array(nE), v0 = new Uint32Array(nE), nv = new Uint32Array(nE), i0 = new Uint32Array(nE), ni = new Uint32Array(nE);
  const s0 = new Uint32Array(nE), ns = new Uint32Array(nE), shape = new Uint32Array(nE), place = new Float32Array(12 * nE);
  for (let e = 0, o = 52; e < nE; e++, o += 48) {
    ids.push(String.fromCharCode(...b.subarray(o, o + 22)));
    cats[e] = b[o + 22] ?? 7;
    v0[e] = dv.getUint32(o + 24, true), nv[e] = dv.getUint32(o + 28, true), i0[e] = dv.getUint32(o + 32, true);
    ni[e] = dv.getUint32(o + 36, true), s0[e] = dv.getUint32(o + 40, true), ns[e] = dv.getUint32(o + 44, true);
    if (i0[e]! + ni[e]! > nI || v0[e]! + nv[e]! > nV || s0[e]! + ns[e]! > nS) throw new Error("BMSH row out of range");
  }
  const sh0 = 52 + 48 * nE;
  let at = sh0 + 4 * nE;
  for (let e = 0; e < nE; e++) {
    shape[e] = dv.getUint32(sh0 + 4 * e, true);
    if (!shape[e]) continue;
    if (at + 48 > n) throw new Error("BMSH 4: cut short");
    for (let k = 0; k < 12; k++) place[12 * e + k] = dv.getFloat32(at + 4 * k, true);
    at += 48;
  }
  if (at + 8 + 24 * nE !== n || dv.getUint32(at, true) !== 0 || dv.getUint32(at + 4, true) !== 0x5358_4f42) throw new Error("BMSH 4: not a rows answer");
  const boxes = new Float32Array(6 * nE);
  for (let k = 0; k < 6 * nE; k++) boxes[k] = dv.getFloat32(at + 8 + 4 * k, true);
  const copies = new Map<number, number>();
  for (let e = 0; e < nE; e++) if (shape[e]) copies.set(shape[e]!, (copies.get(shape[e]!) ?? 0) + 1);
  let v = nV, i = nI, sg = nS;
  for (let e = 0; e < nE; e++) { // bake's rows (glviewport.ts bake)
    if (!shape[e] || !((copies.get(shape[e]!) ?? 0) < minCopies || ni[e]! < 3 * minTris)) continue;
    v0[e] = v, i0[e] = i, s0[e] = sg, shape[e] = 0;
    v += nv[e]!, i += ni[e]!, sg += ns[e]!;
  }
  const size = 52 + 48 * nE + 12 * nV + 4 * nI + 32 * nS + 52 * nE;
  return { rev, origin, size, ids, cats, v0, nv, i0, ni, s0, ns, shape, place, boxes, counts: { nv: v, ni: i, ns: sg } };
}
