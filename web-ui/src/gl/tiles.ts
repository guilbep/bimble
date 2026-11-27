// TILE1 (ADR 0078 amendment 6, ccore/src/tiles.h): a link's tile pyramid read in the page: the table of contents
// (GET /mesh?link=k&name=&key=&tiles=toc), the tiles' answer (…&tile=a,b,c: "TILS"), each proxy (TPX1: 16-bit
// positions in the tile's box, u16 or u32 indices, a category per part) and each leaf (TLF1: its elements' rows and
// placements, the cache's BMG1 blocks, each shape once). Pure functions on bytes (the existing worker may run them);
// no drawing: the frame's walk and the GPU are the viewport's. Every count and offset is checked against what is
// there; a damaged answer throws. The rules are the C reader's (cc_tiles_check): the same section passes or fails both.

export const TILE_LEAF = 1, TILE_PROXY = 2, TILE_OWN = 4, TILE_OCC = 8, TILE_EXTERIOR = 16; // 16 reserved (SHELL1)
export const EL_CLOSED = 1, EL_OPAQUE = 2, EL_EXTERIOR = 4; // 4 reserved (SHELL1): seen from outside
export const BAKE_SHELL = 1; // header bake flags: the exterior shell computed (0: unknown, never "empty")
const HEAD = 48, REC = 80, NONE = 0xffffffff;

export interface TilesToc {
  ntiles: number; nelements: number; nlist: number; nocc: number; levels: number; leafTarget: number; tocBytes: number; blobBytes: number;
  bake: number; // BAKE_SHELL
  parent: Uint32Array; skip: Uint32Array; nchildren: Uint16Array; level: Uint8Array; flags: Uint8Array;
  box: Float32Array; // 6 per tile: min xyz, max xyz (link scene mm)
  error: Float32Array; // mm; 0 leaf; +inf: no proxy (descend)
  el0: Uint32Array; nel: Uint32Array; // subtree's elements: list[el0 .. el0 + nel)
  off: Float64Array; len: Uint32Array; tris: Uint32Array; verts: Uint32Array; occ0: Uint32Array; nocc_: Uint16Array; detail: Uint32Array;
  list: Uint32Array; // element rows, DFS: each subtree a range
  elFlags: Uint8Array; // per list entry: EL_CLOSED (closed and outward), EL_OPAQUE
  occ: Float32Array; // 9 per occluder triangle (empty when only the table was given: parseOcc reads GET …&tiles=occ)
  blobAt: number; // the blobs' offset in the section: pad8(tocBytes + 36 nocc)
}

const fail = (why: string): never => { throw new Error(`tiles: ${why}`); };

/** The table of contents (the section's first `tocBytes`, or the whole section). `nelements`: the link's rows, or -1. */
export function parseToc(ab: ArrayBuffer | Uint8Array, nelements = -1): TilesToc {
  const u8 = ab instanceof Uint8Array ? ab : new Uint8Array(ab);
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength), n = u8.byteLength;
  if (n < HEAD || dv.getUint32(0, true) !== 0x314c4954 /* "TIL1" */ || dv.getUint32(4, true) !== 1) fail("not TIL1 version 1");
  const nt = dv.getUint32(8, true), ne = dv.getUint32(12, true), nl = dv.getUint32(16, true), no = dv.getUint32(20, true), tb = dv.getUint32(24, true);
  if (nelements >= 0 && ne !== nelements) fail("of another scene");
  if (nt > 1 << 22 || nl > ne || no > 64 * nt) fail("counts");
  const pad4 = (x: number) => (x + 3) & ~3;
  if (tb !== HEAD + REC * nt + 4 * nl + pad4(nl) || n < tb) fail("table size"); // the occluders follow the table, then the blobs
  const blobBytes = dv.getUint32(32, true) + dv.getUint32(36, true) * 2 ** 32;
  const t: TilesToc = {
    ntiles: nt, nelements: ne, nlist: nl, nocc: no, levels: dv.getUint32(40, true), leafTarget: dv.getUint32(28, true), tocBytes: tb, blobBytes,
    bake: dv.getUint32(44, true),
    parent: new Uint32Array(nt), skip: new Uint32Array(nt), nchildren: new Uint16Array(nt), level: new Uint8Array(nt), flags: new Uint8Array(nt),
    box: new Float32Array(6 * nt), error: new Float32Array(nt), el0: new Uint32Array(nt), nel: new Uint32Array(nt), off: new Float64Array(nt),
    len: new Uint32Array(nt), tris: new Uint32Array(nt), verts: new Uint32Array(nt), occ0: new Uint32Array(nt), nocc_: new Uint16Array(nt),
    detail: new Uint32Array(nt), list: new Uint32Array(nl), elFlags: new Uint8Array(nl), occ: new Float32Array(n >= tb + 36 * no ? 9 * no : 0),
    blobAt: (tb + 36 * no + 7) & ~7,
  };
  for (let i = 0; i < nt; i++) {
    const r = HEAD + REC * i;
    t.parent[i] = dv.getUint32(r, true), t.skip[i] = dv.getUint32(r + 4, true), t.nchildren[i] = dv.getUint16(r + 8, true);
    t.level[i] = u8[r + 10]!, t.flags[i] = u8[r + 11]!;
    for (let k = 0; k < 6; k++) t.box[6 * i + k] = dv.getFloat32(r + 12 + 4 * k, true);
    t.error[i] = dv.getFloat32(r + 36, true), t.el0[i] = dv.getUint32(r + 40, true), t.nel[i] = dv.getUint32(r + 44, true);
    t.off[i] = dv.getUint32(r + 48, true) + dv.getUint32(r + 52, true) * 2 ** 32, t.len[i] = dv.getUint32(r + 56, true);
    t.tris[i] = dv.getUint32(r + 60, true), t.verts[i] = dv.getUint32(r + 64, true), t.occ0[i] = dv.getUint32(r + 68, true);
    t.nocc_[i] = dv.getUint16(r + 72, true), t.detail[i] = dv.getUint32(r + 76, true);
  }
  const lo = HEAD + REC * nt;
  for (let i = 0; i < nl; i++) { const e = dv.getUint32(lo + 4 * i, true); if (e >= ne) fail("an element"); t.list[i] = e; }
  t.elFlags.set(u8.subarray(lo + 4 * nl, lo + 5 * nl));
  if (t.bake & ~BAKE_SHELL || t.levels > 40) fail("levels or bake flags");
  for (let i = 0; i < nl; i++) if (t.elFlags[i]! & ~7) fail("an element's flags");
  const seen = new Uint8Array(ne);
  for (let i = 0; i < nl; i++) { if (seen[t.list[i]!]) fail("an element twice"); seen[t.list[i]!] = 1; }
  for (let i = 0; i < t.occ.length; i++) { const f = dv.getFloat32(tb + 4 * i, true); if (!(Math.abs(f) < 3e38)) fail("an occluder"); t.occ[i] = f; }
  let leavesEl = 0;
  for (let i = 0; i < nt; i++) {
    const f = t.flags[i]!, b = 6 * i;
    if (i === 0 ? t.parent[0] !== NONE || t.level[0] !== 0 || t.skip[0] !== nt : t.parent[i]! >= i) fail("a parent");
    if (f & ~31) fail("flags");
    for (let k = 0; k < 6; k++) if (!(Math.abs(t.box[b + k]!) < 3e38)) fail("a box");
    if (!(t.box[b]! <= t.box[b + 3]! && t.box[b + 1]! <= t.box[b + 4]! && t.box[b + 2]! <= t.box[b + 5]!)) fail("a box");
    if (t.el0[i]! + t.nel[i]! > nl) fail("an element range");
    if (t.occ0[i]! + t.nocc_[i]! > no || (t.nocc_[i] === 0) !== !(f & TILE_OCC)) fail("occluders");
    if (f & TILE_LEAF) {
      if (t.nchildren[i] || f & TILE_PROXY || t.len[i] || t.error[i] !== 0) fail("a leaf");
      leavesEl += t.nel[i]!;
    } else {
      if (!t.nchildren[i] || f & TILE_OWN) fail("an inner tile");
      if (f & TILE_PROXY) { if (!(t.error[i]! >= 0 && t.error[i]! < 3e38) || t.len[i]! < 16 || t.off[i]! % 4 || t.off[i]! + t.len[i]! > blobBytes) fail("a proxy"); }
      else if (t.error[i] !== Infinity || t.len[i]) fail("an inner tile without proxy");
    }
    if (!t.skip[i] || i + t.skip[i]! > nt) fail("a subtree");
    if (t.nchildren[i]) {
      let c = i + 1, at = t.el0[i]!;
      const end = i + t.skip[i]!;
      for (let q = 0; q < t.nchildren[i]!; q++) {
        if (c >= end || t.parent[c] !== i || t.level[c] !== t.level[i]! + 1 || t.el0[c] !== at || !t.skip[c] || c + t.skip[c]! > end) fail("children");
        for (let k = 0; k < 3; k++) if (t.box[6 * c + k]! < t.box[b + k]! || t.box[6 * c + 3 + k]! > t.box[b + 3 + k]!) fail("a child outside its parent");
        at += t.nel[c]!, c += t.skip[c]!;
      }
      if (c !== end || at !== t.el0[i]! + t.nel[i]!) fail("children");
    } else if (t.skip[i] !== 1) fail("a subtree");
  }
  if (nt && (leavesEl !== nl || t.el0[0] !== 0 || t.nel[0] !== nl)) fail("an element in no leaf");
  return t;
}

/** The occluders (GET …&tiles=occ: 36 bytes a triangle, three corners in link scene mm; tile i's are
 *  [occ0[i], occ0[i] + nocc_[i])), checked against the table. */
export function parseOcc(ab: ArrayBuffer | Uint8Array, toc: TilesToc): Float32Array {
  const u8 = ab instanceof Uint8Array ? ab : new Uint8Array(ab);
  if (u8.byteLength !== 36 * toc.nocc) fail("occluders: size");
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength), out = new Float32Array(9 * toc.nocc);
  for (let i = 0; i < out.length; i++) { const f = dv.getFloat32(4 * i, true); if (!(Math.abs(f) < 3e38)) fail("an occluder"); out[i] = f; }
  return out;
}

/** A tile's children ids. */
export function children(t: TilesToc, i: number): number[] {
  const out: number[] = [];
  for (let c = i + 1, q = 0; q < t.nchildren[i]!; q++) out.push(c), (c += t.skip[c]!);
  return out;
}

export interface TileAnswer { id: number; kind: 0 | 1 | 2; bytes: Uint8Array }

/** "TILS" u32 count, then {u32 id, u32 kind, u32 len, bytes, pad 4}. */
export function parseTils(ab: ArrayBuffer | Uint8Array): TileAnswer[] {
  const u8 = ab instanceof Uint8Array ? ab : new Uint8Array(ab);
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength), n = u8.byteLength;
  if (n < 8 || dv.getUint32(0, true) !== 0x534c4954 /* "TILS" */) fail("not TILS");
  const cnt = dv.getUint32(4, true), out: TileAnswer[] = [];
  let at = 8;
  for (let i = 0; i < cnt; i++) {
    if (at + 12 > n) fail("TILS cut short");
    const id = dv.getUint32(at, true), kind = dv.getUint32(at + 4, true), len = dv.getUint32(at + 8, true);
    if (kind > 2 || at + 12 + len > n) fail("TILS entry");
    out.push({ id, kind: kind as 0 | 1 | 2, bytes: u8.subarray(at + 12, at + 12 + len) });
    at += 12 + ((len + 3) & ~3);
  }
  if (at !== n) fail("TILS trailing bytes");
  return out;
}

export interface ProxyPart { category: number; flags: number; first: number; count: number }
export interface Proxy {
  nv: number; ni: number;
  pos16: Uint16Array; // 3 per vertex: p = lo + q (hi - lo) / 65535
  idx: Uint16Array | Uint32Array;
  parts: ProxyPart[]; // per category (cc_category), index ranges
}

/** A TPX1 proxy (the GPU can take pos16 and idx as they are). */
export function decodeProxy(u8: Uint8Array): Proxy {
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength), n = u8.byteLength;
  if (n < 16 || dv.getUint32(0, true) !== 0x31585054 /* "TPX1" */) fail("not TPX1");
  const nv = dv.getUint32(4, true), ni = dv.getUint32(8, true), np = dv.getUint32(12, true);
  if (ni % 3 || np > 8) fail("a proxy's counts");
  const wide = nv > 65536, pa = (16 + 16 * np + 6 * nv + 3) & ~3, end = pa + (wide ? 4 : 2) * ni;
  if (end > n || ((end + 3) & ~3) !== n && end !== n) fail("a proxy's size");
  const parts: ProxyPart[] = [];
  let i0 = 0;
  for (let k = 0; k < np; k++) {
    const q = 16 + 16 * k, p = { category: dv.getUint32(q, true), flags: dv.getUint32(q + 4, true), first: dv.getUint32(q + 8, true), count: dv.getUint32(q + 12, true) };
    if (p.category >= 8 || p.flags & ~1 || p.first !== i0 || p.count % 3) fail("a proxy's parts");
    i0 += p.count, parts.push(p);
  }
  if (i0 !== ni) fail("a proxy's parts");
  const pos16 = new Uint16Array(3 * nv), idx = wide ? new Uint32Array(ni) : new Uint16Array(ni);
  for (let v = 0; v < 3 * nv; v++) pos16[v] = dv.getUint16(16 + 16 * np + 2 * v, true);
  for (let k = 0; k < ni; k++) { const x = wide ? dv.getUint32(pa + 4 * k, true) : dv.getUint16(pa + 2 * k, true); if (x >= nv) fail("a proxy's index"); idx[k] = x; }
  return { nv, ni, pos16, idx, parts };
}

/** A proxy's positions in link scene mm (f32), from its tile's box. */
export function proxyPositions(p: Proxy, box: ArrayLike<number>): Float32Array {
  const out = new Float32Array(3 * p.nv);
  for (let v = 0; v < p.nv; v++) for (let k = 0; k < 3; k++) out[3 * v + k] = box[k]! + (p.pos16[3 * v + k]! * (box[3 + k]! - box[k]!)) / 65535;
  return out;
}

// ---- BMG1 (ccore/src/meshcodec.h): positions and indices only, edges and normals read past ----
const STEP = 0.0625;
function bmg1(u8: Uint8Array, at: number, end: number): { pos: Float32Array; idx: Uint32Array } {
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
  let o = at;
  const byte = () => { if (o >= end) fail("a block cut short"); return u8[o++]!; };
  const varint = () => { let v = 0, m = 1; for (let i = 0; i < 10; i++) { const x = byte(); v += (x & 0x7f) * m; if (x < 0x80) return v; m *= 128; } return fail("varint"); };
  const zig = () => { const v = varint(); return v % 2 === 0 ? v / 2 : -(v + 1) / 2; };
  const f32 = () => { if (o + 4 > end) fail("a block cut short"); const f = dv.getFloat32(o, true); o += 4; return f; };
  if (end - at < 5 || u8[at] !== 0x42 || u8[at + 1] !== 0x4d || u8[at + 2] !== 0x47 || u8[at + 3] !== 0x31) fail("not a BMG1 block");
  o += 4;
  const mode = byte();
  if (mode > 1) fail("BMG1 mode");
  const nv = varint(), ni = varint(), ne = varint(), nn = varint();
  if (nn > ne || ni % 3 || nv * (mode ? 3 : 12) + ni + 2 * ne + 2 * nn > end - o) fail("BMG1 counts");
  const lo = [0, 0, 0], pv = [0, 0, 0], pos = new Float32Array(3 * nv), idx = new Uint32Array(ni), F = Math.fround;
  if (mode) for (let k = 0; k < 3; k++) lo[k] = f32();
  const point = (out: Float32Array | null, q: number, pp: number[]) => {
    for (let k = 0; k < 3; k++) { if (mode) { pp[k] = pp[k]! + zig(); if (out) out[q + k] = F(lo[k]! + pp[k]! * STEP); } else { const f = f32(); if (out) out[q + k] = f; } }
  };
  for (let i = 0; i < nv; i++) point(pos, 3 * i, pv);
  let last = 0;
  for (let i = 0; i < ni; i++) { last += zig(); if (last < 0 || last >= nv) fail("BMG1 index"); idx[i] = last; }
  const ep = [0, 0, 0];
  for (let e = 0; e < 2 * ne; e++) { const c = varint(); if (c === 1) point(null, 0, ep); else if (c % 2) fail("BMG1 edge"); }
  const nt = varint();
  if (3 * nt > end - o) fail("a block cut short");
  o += 3 * nt;
  for (let e = 0; e < 2 * nn; e++) if (varint() >= nt) fail("BMG1 normal");
  if (o !== end) fail("BMG1 trailing bytes");
  return { pos, idx };
}

export interface LeafShape { pos: Float32Array; idx: Uint32Array; instances: { row: number; placement: Float32Array }[] }
export interface Leaf {
  id: number;
  rows: Uint32Array; // its elements, in the list's order
  // own meshes merged (scene mm): one vertex block, one index block, runs per element
  pos: Float32Array; idx: Uint32Array; runs: Uint32Array; // 3 per run: row, first index, index count
  shapes: LeafShape[]; // shared shapes (own frame) with their copies (placement: columns x, y, z then t)
  triangles: number;
}

/** A TLF1 leaf answer: own meshes merged (with their element runs), shapes with their instances. */
export function decodeLeaf(u8: Uint8Array): Leaf {
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength), n = u8.byteLength;
  if (n < 16 || dv.getUint32(0, true) !== 0x31464c54 /* "TLF1" */) fail("not TLF1");
  const id = dv.getUint32(4, true), nel = dv.getUint32(8, true), nb = dv.getUint32(12, true);
  if (nel > n / 56 || nb > nel || 16 + 56 * nel + 4 * nb > n) fail("TLF1 counts");
  const lens = 16 + 56 * nel;
  const blocks: { pos: Float32Array; idx: Uint32Array }[] = [];
  let at = lens + 4 * nb;
  for (let b = 0; b < nb; b++) { const ln = dv.getUint32(lens + 4 * b, true); if (at + ln > n) fail("TLF1 block"); blocks.push(bmg1(u8, at, at + ln)); at += ln; }
  if (at !== n) fail("TLF1 trailing bytes");
  const rows = new Uint32Array(nel), uses = new Uint32Array(nb), ownOf = new Int32Array(nb).fill(-1);
  const el: { row: number; b: number; pl: Float32Array }[] = [];
  for (let i = 0; i < nel; i++) {
    const e = 16 + 56 * i, row = dv.getUint32(e, true), b = dv.getUint32(e + 4, true);
    if (b >= nb) fail("TLF1 block index");
    const pl = new Float32Array(12);
    for (let k = 0; k < 12; k++) pl[k] = dv.getFloat32(e + 8 + 4 * k, true);
    rows[i] = row, uses[b]!++, el.push({ row, b, pl });
  }
  // a block used once with the identity placement is an own mesh (scene mm); the others are shapes
  const ident = (p: Float32Array) => p[0] === 1 && p[4] === 1 && p[8] === 1 && p[1] === 0 && p[2] === 0 && p[3] === 0 && p[5] === 0 && p[6] === 0 && p[7] === 0 && p[9] === 0 && p[10] === 0 && p[11] === 0;
  let nvOwn = 0, niOwn = 0, nOwn = 0;
  for (const x of el) if (uses[x.b] === 1 && ident(x.pl)) ownOf[x.b] = 1, nvOwn += blocks[x.b]!.pos.length / 3, niOwn += blocks[x.b]!.idx.length, nOwn++;
  const pos = new Float32Array(3 * nvOwn), idx = new Uint32Array(niOwn), runs = new Uint32Array(3 * nOwn);
  const shapes: LeafShape[] = [], shapeOf = new Int32Array(nb).fill(-1);
  let v0 = 0, i0 = 0, r = 0, tris = 0;
  for (const x of el) {
    const m = blocks[x.b]!;
    tris += m.idx.length / 3;
    if (ownOf[x.b] === 1) {
      pos.set(m.pos, 3 * v0);
      for (let k = 0; k < m.idx.length; k++) idx[i0 + k] = v0 + m.idx[k]!;
      runs[3 * r] = x.row, runs[3 * r + 1] = i0, runs[3 * r + 2] = m.idx.length, r++;
      v0 += m.pos.length / 3, i0 += m.idx.length;
    } else {
      if (shapeOf[x.b] === -1) shapeOf[x.b] = shapes.length, shapes.push({ pos: m.pos, idx: m.idx, instances: [] });
      shapes[shapeOf[x.b]!]!.instances.push({ row: x.row, placement: x.pl });
    }
  }
  return { id, rows, pos, idx, runs, shapes, triangles: tris };
}

// ---- compact positions for the GPU (tiles.h "Compact vertices") ----

/** 16-bit positions in a box (the proxies' form; for a leaf, a quantum of (hi - lo) / 65535). */
export function pack16(pos: Float32Array, box: ArrayLike<number>): Uint16Array {
  const out = new Uint16Array(pos.length);
  for (let i = 0; i < pos.length; i++) {
    const k = i % 3, s = box[3 + k]! - box[k]!;
    out[i] = s > 0 ? Math.max(0, Math.min(65535, Math.round(((pos[i]! - box[k]!) / s) * 65535))) : 0;
  }
  return out;
}

/** The link's 1/16 mm lattice for a leaf's box: shift k, origin o (1/16 mm units). */
export function lattice(box: ArrayLike<number>): { k: number; o: [number, number, number] } {
  let span = 0;
  const lo = [0, 0, 0];
  for (let j = 0; j < 3; j++) { lo[j] = Math.floor(Math.fround(box[j]!) * 16); const hi = Math.ceil(Math.fround(box[3 + j]!) * 16); span = Math.max(span, hi - lo[j]!); }
  let k = 0;
  while (k < 40 && Math.floor(span / 2 ** k) + 1 >= 2 ** 21) k++;
  return { k, o: [Math.floor(lo[0]! / 2 ** k) * 2 ** k, Math.floor(lo[1]! / 2 ** k) * 2 ** k, Math.floor(lo[2]! / 2 ** k) * 2 ** k] };
}

/** 21 bits an axis on that lattice, two u32 a vertex: word0 = qx | qy << 21, word1 = qy >> 11 | qz << 10.
 *  A vertex shared by two leaves of the same k lands on one lattice point (no crack). */
export function pack21(pos: Float32Array, box: ArrayLike<number>): { words: Uint32Array; k: number; o: [number, number, number] } {
  const { k, o } = lattice(box), nv = pos.length / 3, words = new Uint32Array(2 * nv), q = [0, 0, 0];
  for (let v = 0; v < nv; v++) {
    for (let j = 0; j < 3; j++) {
      const x = pos[3 * v + j]! * 16, r = x < 0 ? -Math.round(-x) : Math.round(x); // half away from zero, as the C
      let d = r - o[j]!;
      if (k) d = Math.floor((d + 2 ** (k - 1)) / 2 ** k);
      q[j] = Math.max(0, Math.min(0x1fffff, d));
    }
    words[2 * v] = (q[0]! | (q[1]! << 21)) >>> 0;
    words[2 * v + 1] = ((q[1]! >>> 11) | (q[2]! << 10)) >>> 0;
  }
  return { words, k, o };
}

/** pack21's words back to mm (a shader does the same: (o + q 2^k) / 16). */
export function unpack21(words: Uint32Array, k: number, o: [number, number, number]): Float32Array {
  const nv = words.length / 2, out = new Float32Array(3 * nv);
  for (let v = 0; v < nv; v++) {
    const a = words[2 * v]!, b = words[2 * v + 1]!;
    const q = [a & 0x1fffff, (a >>> 21) | ((b & 0x3ff) << 11), b >>> 10];
    for (let j = 0; j < 3; j++) out[3 * v + j] = (o[j]! + q[j]! * 2 ** k) / 16;
  }
  return out;
}
