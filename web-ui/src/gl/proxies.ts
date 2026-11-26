// HLOD proxies in the lab (ticket 3D2, ADR 0078; bimble-render proxies.rs): the core builds them
// (ccore/src/hlod.h, served in steps as BPRX: GET /mesh?proxies=N, wasm host_proxies); here they are
// parsed, their leaves numbered, and each frame the tree is cut where a cell's error stays under a
// pixel threshold. Pure functions, no WebGL: glviewport.ts uploads and draws.

/** One part of a cell's proxy: a material family's triangles and crease edges. */
export interface ProxyPart { cat: number; v0: number; nv: number; i0: number; ni: number; s0: number; ns: number }
export interface ProxyCell {
  depth: number; error: number; box: Float32Array; children: number[];
  /** Its elements: `elements[el0 .. el0 + nel]` (scene element indices = BMSH rows of the open model). */
  el0: number; nel: number;
  parts: ProxyPart[];
  /** Leaves under it, by their numbers (depth first): `leaves[0] .. leaves[1]`. */
  leaves: [number, number];
  original: number;
}
export interface Proxies {
  rev: number; nElements: number; cells: ProxyCell[]; elements: Uint32Array;
  pos: Float32Array; idx: Uint32Array; edges: Float32Array; normals: Int8Array;
  /** Per scene element, the number of its leaf (-1: none). */
  leafOf: Int32Array; leafCount: number;
  /** Per leaf, its cell. */
  leafCell: Uint32Array;
  /** Per cell, its parent (-1: the root); its proxy's triangles. */
  parent: Int32Array; tris: Uint32Array;
}

/** BPRX (hlod.h cc_hlod_bprx) → cells, element lists, the proxies' vertices, indices and edges. */
export function parseBprx(ab: ArrayBuffer): Proxies {
  const dv = new DataView(ab);
  if (ab.byteLength < 48 || String.fromCharCode(...new Uint8Array(ab, 0, 4)) !== "BPRX") throw new Error("not BPRX");
  const u = (o: number) => dv.getUint32(o, true);
  if (u(4) !== 1) throw new Error(`BPRX version ${u(4)}`);
  const rev = u(8), nE = u(12), nC = u(16), nK = u(20), nL = u(24), nP = u(28), nV = u(32), nI = u(36), nS = u(40);
  let o = 48;
  const raw: { depth: number; error: number; box: Float32Array; k0: number; nk: number; el0: number; nel: number; p0: number; np: number; original: number }[] = [];
  for (let c = 0; c < nC; c++, o += 64) {
    raw.push({ depth: u(o), error: dv.getFloat32(o + 4, true), box: new Float32Array(ab.slice(o + 8, o + 32)), k0: u(o + 32), nk: u(o + 36),
      el0: u(o + 40), nel: u(o + 44), p0: u(o + 48), np: u(o + 52), original: u(o + 56) + 2 ** 32 * u(o + 60) });
  }
  const kids = new Uint32Array(ab, o, nK); o += 4 * nK;
  const elements = new Uint32Array(ab, o, nL); o += 4 * nL;
  const parts: ProxyPart[] = [];
  for (let p = 0; p < nP; p++, o += 28) parts.push({ cat: u(o) & 255, v0: u(o + 4), nv: u(o + 8), i0: u(o + 12), ni: u(o + 16), s0: u(o + 20), ns: u(o + 24) });
  const pos = new Float32Array(ab, o, 3 * nV); o += 12 * nV;
  const idx = new Uint32Array(ab, o, nI); o += 4 * nI;
  const edges = new Float32Array(ab, o, 6 * nS); o += 24 * nS;
  const normals = new Int8Array(ab, o, 8 * nS); o += 8 * nS;
  if (o !== ab.byteLength) throw new Error("BPRX size");
  const cells: ProxyCell[] = raw.map((r) => ({ depth: r.depth, error: r.error, box: r.box, children: Array.from(kids.subarray(r.k0, r.k0 + r.nk)),
    el0: r.el0, nel: r.nel, parts: parts.slice(r.p0, r.p0 + r.np), leaves: [0, 0], original: r.original }));
  // proxies.rs:76: leaves numbered depth first, so each cell's are a range
  const leafOf = new Int32Array(nE).fill(-1), leafCell: number[] = [];
  const stack: [number, boolean][] = cells.length ? [[0, false]] : [];
  while (stack.length) {
    const [k, done] = stack.pop()!;
    const c = cells[k];
    if (!c) continue;
    if (done) {
      const ch = c.children.map((x) => cells[x]!);
      c.leaves = [Math.min(...ch.map((x) => x.leaves[0])), Math.max(...ch.map((x) => x.leaves[1]))];
      continue;
    }
    if (!c.children.length) {
      const n = leafCell.length;
      for (let i = c.el0; i < c.el0 + c.nel; i++) if (elements[i]! < nE) leafOf[elements[i]!] = n;
      c.leaves = [n, n + 1];
      leafCell.push(k);
      continue;
    }
    stack.push([k, true]);
    for (let i = c.children.length - 1; i >= 0; i--) stack.push([c.children[i]!, false]);
  }
  const parent = new Int32Array(cells.length).fill(-1), tris = new Uint32Array(cells.length);
  cells.forEach((c, k) => { for (const x of c.children) parent[x] = k; tris[k] = c.parts.reduce((n, q) => n + q.ni / 3, 0); });
  return { rev, nElements: nE, cells, elements, pos, idx, edges, normals, leafOf, leafCount: leafCell.length, leafCell: Uint32Array.from(leafCell), parent, tris };
}

/** proxies.rs:131 usable: the cells under which every element is drawn plainly (shown, not selected,
 *  not revealed): `plain(e)`. A hidden, selected or cut element must show as it is (or not at all). */
export function usableCells(p: Proxies, plain: (e: number) => boolean): Uint8Array {
  const bad = new Uint32Array(p.elements.length + 1); // prefix counts over the element list
  for (let i = 0; i < p.elements.length; i++) bad[i + 1] = bad[i]! + (plain(p.elements[i]!) ? 0 : 1);
  const out = new Uint8Array(p.cells.length);
  p.cells.forEach((c, k) => { out[k] = c.parts.length > 0 && bad[c.el0 + c.nel] === bad[c.el0] ? 1 : 0; });
  return out;
}

/** lib.rs:625 proxied: from the root, a usable cell in view whose error, at the distance from the eye
 *  to its box, spans at most `px` pixels stands for its elements; else its children are tested. A
 *  cell out of view (`inView` false: cull.ts boxInFrustum) is not descended (its elements' batches
 *  are culled anyway). `mmPerPx(d)`: the millimetres a pixel spans at distance d. Returns the cells
 *  drawn and per leaf 1 when replaced. */
export function cutProxies(p: Proxies, usable: Uint8Array, inView: ((box: Float32Array) => boolean) | null, eye: number[], mmPerPx: (d: number) => number,
  px: number): { cells: number[]; replaced: Uint8Array } {
  const replaced = new Uint8Array(p.leafCount), cells: number[] = [];
  if (px <= 0 || !p.cells.length) return { cells, replaced };
  const stack = [0];
  while (stack.length) {
    const k = stack.pop()!, c = p.cells[k]!, b = c.box;
    if (inView && !inView(b)) continue;
    let d2 = 0;
    for (let a = 0; a < 3; a++) { const q = Math.min(Math.max(eye[a]!, b[a]!), b[3 + a]!) - eye[a]!; d2 += q * q; }
    if (usable[k] && c.error <= px * mmPerPx(Math.sqrt(d2))) {
      cells.push(k);
      replaced.fill(1, c.leaves[0], c.leaves[1]);
    } else stack.push(...c.children);
  }
  return { cells, replaced };
}

/** A moving frame's stand-ins for the leaves it could not finish (3D2, in place of 3D1's outlines):
 *  for each such leaf, the deepest usable cell holding it whose proxy is drawn instead of its leaves;
 *  shallower cells first win, so no leaf is drawn twice. Leaves already replaced are skipped, and a cell
 *  whose proxy has more than `max` triangles is not taken (its leaves keep their outlines). */
export function standIns(p: Proxies, usable: Uint8Array, unfinished: Iterable<number>, replaced: Uint8Array, max = Infinity): number[] {
  const parent = p.parent;
  const pick = new Set<number>();
  for (const leaf of unfinished) {
    if (replaced[leaf]) continue;
    let k = p.leafCell[leaf] ?? -1;
    while (k >= 0 && !usable[k]) k = parent[k]!;
    if (k >= 0 && p.tris[k]! <= max) pick.add(k);
  }
  const out: number[] = [];
  for (const k of [...pick].sort((a, b) => p.cells[a]!.depth - p.cells[b]!.depth)) {
    const c = p.cells[k]!;
    let free = true;
    for (let l = c.leaves[0]; l < c.leaves[1] && free; l++) if (replaced[l]) free = false;
    if (!free) continue;
    replaced.fill(1, c.leaves[0], c.leaves[1]);
    out.push(k);
  }
  return out;
}

/** 3D2: the cells' proxies under `max` triangles (a moving frame's share of its budget): while over, the
 *  deepest drawn cell gives way to its parent (usable, with a proxy), which stands for all its drawn
 *  descendants with fewer triangles; `replaced` grows to the parent's leaves. Returns the cells. */
export function fitProxies(p: Proxies, usable: Uint8Array, cells: number[], replaced: Uint8Array, max: number): number[] {
  let set = new Set(cells), sum = cells.reduce((n, k) => n + p.tris[k]!, 0);
  const tried = new Set<number>();
  while (sum > max) {
    let best = -1;
    for (const k of set) {
      const q = p.parent[k]!;
      if (q < 0 || tried.has(q) || !usable[q]) continue;
      if (best < 0 || p.cells[k]!.depth > p.cells[best]!.depth) best = k;
    }
    if (best < 0) break;
    const q = p.parent[best]!, c = p.cells[q]!;
    tried.add(q);
    let under = 0;
    const keep = new Set<number>();
    for (const k of set) {
      const x = p.cells[k]!;
      if (x.leaves[0] >= c.leaves[0] && x.leaves[1] <= c.leaves[1]) under += p.tris[k]!;
      else keep.add(k);
    }
    if (p.tris[q]! >= under) continue; // no smaller
    keep.add(q);
    set = keep, sum += p.tris[q]! - under;
    replaced.fill(1, c.leaves[0], c.leaves[1]);
  }
  // still over: the largest proxies give way to their elements (drawn within the frame's budget)
  const out = [...set].sort((a, b) => p.tris[a]! - p.tris[b]!);
  while (sum > max && out.length) {
    const k = out.pop()!, c = p.cells[k]!;
    sum -= p.tris[k]!;
    replaced.fill(0, c.leaves[0], c.leaves[1]);
  }
  return out;
}
