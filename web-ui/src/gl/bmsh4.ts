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
        const m = e.data as { id: number; out?: ArrayBuffer; error?: string };
        const p = pending.get(m.id);
        pending.delete(m.id);
        if (!p) return;
        if (m.out) p.res(m.out);
        else p.rej(new Error(m.error ?? "BMSH 4: the decoder failed"));
      };
      worker.onerror = () => { // a worker that cannot start (a blocked script): here from now on
        for (const p of pending.values()) p.rej(new Error("BMSH 4: the decoder failed"));
        pending.clear();
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
