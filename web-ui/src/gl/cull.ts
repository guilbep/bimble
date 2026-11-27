// 3D speed of the viewport (ticket 3D1, ADR 0078): batches culled as wholes against the view, and
// the moving budget (ADR 0035). Pure functions, no WebGL: glviewport.ts draws.
//
// Batches (ADR 0036 step 1, bimble-render's clusters): the merged geometry is cut into spatial
// batches (Morton order of the element centres, at most BATCH_TRIS triangles each, an element larger
// than that alone); a shape's instances likewise (at most BATCH_TRIS triangles of copies each).
// Inside a batch the elements go largest first (half the box diagonal), so "everything at least r
// big" is a prefix of each batch (ADR 0035 decision 1): culling and the moving budget cost one test
// per batch and binary searches, not a pass over the elements. The moving budget is in triangles
// (ADR 0078, amended): the elements largest on screen first until the frame's budget is spent.

/** Triangles a batch holds at most (bimble-render: 32,768 a cluster). */
export const BATCH_TRIS = 32768;

/** A moving frame's triangles at least (ADR 0078 decision 4, amended): never an empty picture. */
export const MIN_MOVING_TRIS = 20000;

/** A batch as a frame sees it: its distance from the eye (mm; 1 for an orthographic view) and the
 *  items already drawn (k0: a progressive fill continues from there). */
export interface Cut { b: Batch; dist: number; k0: number }

/** The items of `c` this frame draws for a screen threshold t: those at least t * dist big (a prefix:
 *  largest first), from k0. */
export function cutEnd(c: Cut, t: number): number {
  return Math.max(c.k0, prefixCount(c.b, t * c.dist));
}

/** The triangle budget's cut: the smallest screen threshold t (size / distance) whose items, over
 *  every batch, hold at most `budget` triangles; at least one item (the largest on screen) when even
 *  that one is over the budget, so a frame is never empty while anything is left. t = 0: everything. */
export function budgetCut(cs: Cut[], budget: number): { t: number; tris: number } {
  // LNK3D: the triangles of the items shown (vcum, when set): a hidden element costs no budget
  const at = (t: number) => { let n = 0; for (const c of cs) { const k = c.b.vcum ?? c.b.cum; n += k[cutEnd(c, t)]! - k[c.k0]!; } return n; };
  const all = at(0);
  if (all <= budget) return { t: 0, tris: all };
  let hi = 0;
  for (const c of cs) if (c.k0 < c.b.n) hi = Math.max(hi, c.b.size[c.k0]! / Math.max(c.dist, 1e-9));
  const top = hi;
  hi = hi * (1 + 1e-9) + 1e-300;
  let lo = 0; // at(lo) > budget >= at(hi)
  for (let i = 0; i < 60; i++) {
    const mid = (lo + hi) / 2;
    if (at(mid) <= budget) hi = mid;
    else lo = mid;
  }
  let tris = at(hi);
  if (tris === 0) hi = top, tris = at(top); // one item over the budget: the largest one(s) on screen alone
  return { t: hi, tris };
}

/** The six planes (a, b, c, d: inside when a x + b y + c z + d >= 0) of a column-major view-projection
 *  matrix (Gribb and Hartmann): left, right, bottom, top, near, far. */
export function frustumPlanes(m: Float32Array): Float64Array {
  const r = (i: number) => [m[i]!, m[4 + i]!, m[8 + i]!, m[12 + i]!];
  const x = r(0), y = r(1), z = r(2), w = r(3);
  const out = new Float64Array(24);
  [[1, x], [-1, x], [1, y], [-1, y], [1, z], [-1, z]].forEach(([s, v], k) => {
    for (let j = 0; j < 4; j++) out[4 * k + j] = w[j]! + (s as number) * (v as number[])[j]!;
  });
  return out;
}

/** False when the box (min xyz, max xyz at `o`) lies wholly outside one plane: not on screen. */
export function boxInFrustum(p: Float64Array, b: Float32Array, o: number): boolean {
  for (let k = 0; k < 24; k += 4) {
    const a = p[k]!, bb = p[k + 1]!, c = p[k + 2]!, d = p[k + 3]!;
    // the box corner farthest along the plane's normal
    const x = a >= 0 ? b[o + 3]! : b[o]!, y = bb >= 0 ? b[o + 4]! : b[o + 1]!, z = c >= 0 ? b[o + 5]! : b[o + 2]!;
    if (a * x + bb * y + c * z + d < 0) return false;
  }
  return true;
}

/** 10 bits of v spread to every third bit. */
function spread(v: number): number {
  let x = v & 1023;
  x = (x | (x << 16)) & 0x030000ff;
  x = (x | (x << 8)) & 0x0300f00f;
  x = (x | (x << 4)) & 0x030c30c3;
  x = (x | (x << 2)) & 0x09249249;
  return x;
}

/** A batch: items [first, first + n) of its list (elements or instances), largest first; `size` their
 *  sizes (mm, descending), `cum[i]` the triangles (or instances' indices) of the first i items, the
 *  same for edge segments in `ecum`; `box` the items' box. */
export interface Batch { first: number; n: number; size: Float32Array; cum: Uint32Array; ecum: Uint32Array; box: Float32Array;
  /** 3D2: the HLOD leaf its items are in (-1: none); a batch never spans two leaves. */
  group: number;
  /** LNK3D: `vcum[i]` the triangles of the first i items that are shown (hidden ones count 0; unset:
   *  `cum`), and how many items are shown (0: the batch is not drawn). Set by the viewport when what
   *  is hidden changes; the budget (budgetCut) counts these. */
  vcum?: Uint32Array; shown?: number }

/** Cuts `items` (indices into `boxes`, 6 floats each) into batches: Morton order of the box centres
 *  inside `bounds`, a new batch when the next item would pass `cap` (its `cost`), then largest first
 *  inside each. 3D2: with `group` (an item's HLOD leaf), the items go by group first (leaves in
 *  their depth-first order, so a cell's batches lie side by side) and a batch never spans two groups.
 *  Returns the items in their new order and the batches. */
export function makeBatches(items: number[], boxes: Float32Array, cost: (e: number) => number, edges: (e: number) => number,
  cap: number, group: (e: number) => number = () => -1): { order: number[]; batches: Batch[] } {
  if (!items.length) return { order: [], batches: [] };
  const lo = [Infinity, Infinity, Infinity], hi = [-Infinity, -Infinity, -Infinity];
  for (const e of items) for (let a = 0; a < 3; a++) lo[a] = Math.min(lo[a]!, boxes[6 * e + a]!), hi[a] = Math.max(hi[a]!, boxes[6 * e + 3 + a]!);
  const key = new Map<number, number>();
  for (const e of items) {
    let k = 0;
    for (let a = 0; a < 3; a++) {
      const c = (boxes[6 * e + a]! + boxes[6 * e + 3 + a]!) / 2, span = hi[a]! - lo[a]!;
      k |= spread(span > 0 ? Math.min(1023, Math.floor(((c - lo[a]!) / span) * 1024)) : 0) << a;
    }
    key.set(e, k >>> 0);
  }
  const sorted = [...items].sort((a, b) => group(a) - group(b) || key.get(a)! - key.get(b)! || a - b);
  const sizeOf = (e: number) => Math.hypot(boxes[6 * e + 3]! - boxes[6 * e]!, boxes[6 * e + 4]! - boxes[6 * e + 1]!, boxes[6 * e + 5]! - boxes[6 * e + 2]!) / 2;
  const order: number[] = [], batches: Batch[] = [];
  let i = 0;
  while (i < sorted.length) {
    let j = i, c = 0;
    const g = group(sorted[i]!);
    while (j < sorted.length && (j === i || (c + cost(sorted[j]!) <= cap && group(sorted[j]!) === g))) c += cost(sorted[j]!), j++;
    const part = sorted.slice(i, j).sort((a, b) => sizeOf(b) - sizeOf(a) || a - b);
    const n = part.length, size = new Float32Array(n), cum = new Uint32Array(n + 1), ecum = new Uint32Array(n + 1), box = new Float32Array([Infinity, Infinity, Infinity, -Infinity, -Infinity, -Infinity]);
    part.forEach((e, k) => {
      size[k] = sizeOf(e);
      cum[k + 1] = cum[k]! + cost(e);
      ecum[k + 1] = ecum[k]! + edges(e);
      for (let a = 0; a < 3; a++) box[a] = Math.min(box[a]!, boxes[6 * e + a]!), box[3 + a] = Math.max(box[3 + a]!, boxes[6 * e + 3 + a]!);
    });
    batches.push({ first: order.length, n, size, cum, ecum, box, group: g });
    order.push(...part);
    i = j;
  }
  return { order, batches };
}

/** How many items of a batch are at least `minSize` big (a prefix: sizes descend). */
export function prefixCount(b: Batch, minSize: number): number {
  if (minSize <= 0) return b.n;
  let lo = 0, hi = b.n;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (b.size[mid]! >= minSize) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

// ---- LNK3D (ADR 0078 amendment 3): what both renderers (WebGL glviewport.ts, WebGPU gpu/) share to
// draw linked models without rebuilding buffers: a placement per model and a state per element. ----

/** A model's placement in the scene (mm, relative to the open scene's origin): a point p of the model
 *  is drawn at R p + t, R the turn about Z by (c, s) = (cos, sin). The open model: IDENTITY_XF. A link:
 *  the LNKS trailer (version 2) or `query link_draw` (ccore/session/links.h), t = its placed origin
 *  minus the open scene's origin. A move, a turn or an offset changes this only (one uniform). */
export interface ModelXf { c: number; s: number; t: [number, number, number] }
export const IDENTITY_XF: ModelXf = { c: 1, s: 0, t: [0, 0, 0] };
export const sameXf = (a: ModelXf, b: ModelXf) => a.c === b.c && a.s === b.s && a.t[0] === b.t[0] && a.t[1] === b.t[1] && a.t[2] === b.t[2];

/** The matrix's three rows (R row, t), 12 floats: what a shader reads per model. */
export function xfRows(x: ModelXf, out = new Float32Array(12), o = 0): Float32Array {
  out.set([x.c, -x.s, 0, x.t[0], x.s, x.c, 0, x.t[1], 0, 0, 1, x.t[2]], o);
  return out;
}

/** The box `src[so .. so + 6]` (min xyz, max xyz, the model's frame) placed by x, as the axis-aligned
 *  box around it at `dst[d0 ..]` (exact for no turn; a turned box's 4 corners in plan otherwise). */
export function xfBox(x: ModelXf, src: Float32Array, so: number, dst: Float32Array, d0: number): void {
  const x0 = src[so]!, y0 = src[so + 1]!, x1 = src[so + 3]!, y1 = src[so + 4]!;
  if (!(x0 <= x1)) { dst.set(src.subarray(so, so + 6), d0); return; } // an empty box stays empty
  let lx = Infinity, ly = Infinity, hx = -Infinity, hy = -Infinity;
  for (const [px, py] of [[x0, y0], [x1, y0], [x0, y1], [x1, y1]] as const) {
    const qx = x.c * px - x.s * py, qy = x.s * px + x.c * py;
    if (qx < lx) lx = qx;
    if (qx > hx) hx = qx;
    if (qy < ly) ly = qy;
    if (qy > hy) hy = qy;
  }
  dst[d0] = lx + x.t[0], dst[d0 + 1] = ly + x.t[1], dst[d0 + 2] = src[so + 2]! + x.t[2];
  dst[d0 + 3] = hx + x.t[0], dst[d0 + 4] = hy + x.t[1], dst[d0 + 5] = src[so + 5]! + x.t[2];
}

/** A ray (scene point e, direction d) in the model's own frame: R^T (e - t), R^T d. A rigid placement
 *  keeps the ray's parameter, so a hit's distance holds in both frames (the page's pick). */
export function xfRayToLocal(x: ModelXf, e: number[], d: number[]): { e: [number, number, number]; d: [number, number, number] } {
  const ex = e[0]! - x.t[0], ey = e[1]! - x.t[1];
  return { e: [x.c * ex + x.s * ey, -x.s * ex + x.c * ey, e[2]! - x.t[2]], d: [x.c * d[0]! + x.s * d[1]!, -x.s * d[0]! + x.c * d[1]!, d[2]!] };
}
/** A point of the model's frame in the scene: R p + t. */
export const xfPoint = (x: ModelXf, p: number[]): [number, number, number] =>
  [x.c * p[0]! - x.s * p[1]! + x.t[0], x.s * p[0]! + x.c * p[1]! + x.t[1], p[2]! + x.t[2]];

/** Per element draw state (LNK3D): 0 hidden (not drawn, not picked, no budget), 1 shown, 2 shown revealed
 *  (hidden, Reveal hidden on: the comparison colour). Selection and a link's colour override are its colour. */
export const EL_HIDDEN = 0, EL_SHOWN = 1, EL_REVEALED = 2;
/** The state texture (RGBA8, a texel per element, STATE_TEX_W a row): r, g, b the element's colour (sRGB),
 *  a = model << 1 | drawn (model 0: the open one, k + 1: link k; at most 127 models). A vertex without an
 *  element (proxies, outlines, the ghost) carries STATE_NONE and takes its own colour and the draw's model. */
export const STATE_TEX_W = 2048, STATE_NONE = 0xffffffff;
/** Writes element e's texel. */
export function packState(out: Uint8Array, e: number, rgb: readonly number[], model: number, drawn: boolean): void {
  out[4 * e] = rgb[0]!, out[4 * e + 1] = rgb[1]!, out[4 * e + 2] = rgb[2]!, out[4 * e + 3] = ((model & 127) << 1) | (drawn ? 1 : 0);
}
/** A batch's shown items (vcum, shown) from the per-element state: `items` its elements in batch order,
 *  `tris(e)` an item's triangles. */
export function batchShown(b: Batch, items: ArrayLike<number>, shown: Uint8Array, tris: (e: number) => number): void {
  const v = b.vcum && b.vcum.length === b.n + 1 ? b.vcum : new Uint32Array(b.n + 1);
  let n = 0;
  for (let i = 0; i < b.n; i++) { const e = items[i]!, on = shown[e]! > 0; v[i + 1] = v[i]! + (on ? tris(e) : 0); if (on) n++; }
  b.vcum = v, b.shown = n;
}

// ---- VIS1 (ADR 0078 amendment 7, P0-c): the budget's cut for tens of thousands of batches. budgetCut bisects the
// threshold 60 times, each time over every batch (a binary search in each): on a project of 17 links (46,800
// batches) that is millions of searches a moving frame. Here each batch's items are counted once per state in
// buckets of their size (a sixteenth of an octave), a frame adds the buckets shifted by the batch's distance, the
// threshold is read from that histogram (a sixteenth of an octave: each batch placed within a bucket), then checked
// exactly (one search a batch) and raised a bucket at a time while over the budget. The same answer's kind: everything at least t big, at most `budget` triangles, never empty. ----
const BPO = 16, B0 = -512, NB = 1536; // buckets per octave (a 4.4 % step); log2(size / dist) from -32 octaves, over 96 octaves
interface SizeHist { key: string; bucket: Int16Array; tris: Float64Array }
const hists = new WeakMap<Batch, SizeHist>();
function histOf(c: Cut, key: string): SizeHist {
  const b = c.b, h = hists.get(b);
  if (h && h.key === key) return h;
  const k = b.vcum ?? b.cum, idx: number[] = [], tr: number[] = [];
  for (let i = 0; i < b.n; i++) {
    const t = k[i + 1]! - k[i]!;
    if (!t) continue;
    const q = Math.floor(BPO * Math.log2(Math.max(1e-6, b.size[i]!)));
    if (idx.length && idx[idx.length - 1] === q) tr[tr.length - 1]! += t;
    else idx.push(q), tr.push(t);
  }
  const out = { key, bucket: Int16Array.from(idx), tris: Float64Array.from(tr) };
  hists.set(b, out);
  return out;
}
/** budgetCut's answer through size buckets (for many batches); `key` changes when what is shown changes. */
export function budgetCutFast(cs: Cut[], budget: number, key: string): { t: number; tris: number } {
  const at = (t: number) => { let n = 0; for (const c of cs) { const k = c.b.vcum ?? c.b.cum; n += k[cutEnd(c, t)]! - k[c.k0]!; } return n; };
  const H = new Float64Array(NB);
  let all = 0;
  for (const c of cs) {
    const shift = Math.floor(BPO * Math.log2(Math.max(c.dist, 1e-9)));
    if (c.k0 > 0) { // a slice of the fill: its items from k0 on (the smaller ones), counted one by one
      const b = c.b, k = b.vcum ?? b.cum;
      for (let i = c.k0; i < b.n; i++) {
        const t = k[i + 1]! - k[i]!;
        if (!t) continue;
        const q = Math.min(NB - 1, Math.max(0, Math.floor(BPO * Math.log2(Math.max(1e-6, b.size[i]!))) - shift - B0));
        H[q]! += t, all += t;
      }
      continue;
    }
    const h = histOf(c, key);
    for (let i = 0; i < h.bucket.length; i++) { const q = Math.min(NB - 1, Math.max(0, h.bucket[i]! - shift - B0)); H[q]! += h.tris[i]!; all += h.tris[i]!; }
  }
  if (all <= budget) return { t: 0, tris: at(0) };
  // from the largest bucket down: the lowest bucket whose sum and those above stay within the budget
  let sum = 0, q = NB - 1;
  for (; q >= 0; q--) { if (sum + H[q]! > budget) break; sum += H[q]!; }
  // each batch is placed within a bucket of its true one: the threshold one bucket up, checked exactly, raised while over
  let t = Math.pow(2, (q + 2 + B0) / BPO), tris = at(t);
  for (let i = 0; i < 64 && tris > budget; i++) t *= Math.pow(2, 1 / BPO), tris = at(t);
  for (let i = 0; i < 2; i++) { const t2 = t / Math.pow(2, 1 / BPO), n2 = at(t2); if (n2 > budget) break; t = t2, tris = n2; } // (and down while within)
  if (tris === 0) { // one item over the budget: the largest on screen alone (budgetCut's rule)
    let hi = 0;
    for (const c of cs) if (c.k0 < c.b.n) hi = Math.max(hi, c.b.size[c.k0]! / Math.max(c.dist, 1e-9));
    t = hi, tris = at(hi);
  }
  return { t, tris };
}
