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
  const at = (t: number) => { let n = 0; for (const c of cs) n += c.b.cum[cutEnd(c, t)]! - c.b.cum[c.k0]!; return n; };
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
export interface Batch { first: number; n: number; size: Float32Array; cum: Uint32Array; ecum: Uint32Array; box: Float32Array }

/** Cuts `items` (indices into `boxes`, 6 floats each) into batches: Morton order of the box centres
 *  inside `bounds`, a new batch when the next item would pass `cap` (its `cost`), then largest first
 *  inside each. Returns the items in their new order and the batches. */
export function makeBatches(items: number[], boxes: Float32Array, cost: (e: number) => number, edges: (e: number) => number,
  cap: number): { order: number[]; batches: Batch[] } {
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
  const sorted = [...items].sort((a, b) => key.get(a)! - key.get(b)! || a - b);
  const sizeOf = (e: number) => Math.hypot(boxes[6 * e + 3]! - boxes[6 * e]!, boxes[6 * e + 4]! - boxes[6 * e + 1]!, boxes[6 * e + 5]! - boxes[6 * e + 2]!) / 2;
  const order: number[] = [], batches: Batch[] = [];
  let i = 0;
  while (i < sorted.length) {
    let j = i, c = 0;
    while (j < sorted.length && (j === i || c + cost(sorted[j]!) <= cap)) c += cost(sorted[j]!), j++;
    const part = sorted.slice(i, j).sort((a, b) => sizeOf(b) - sizeOf(a) || a - b);
    const n = part.length, size = new Float32Array(n), cum = new Uint32Array(n + 1), ecum = new Uint32Array(n + 1), box = new Float32Array([Infinity, Infinity, Infinity, -Infinity, -Infinity, -Infinity]);
    part.forEach((e, k) => {
      size[k] = sizeOf(e);
      cum[k + 1] = cum[k]! + cost(e);
      ecum[k + 1] = ecum[k]! + edges(e);
      for (let a = 0; a < 3; a++) box[a] = Math.min(box[a]!, boxes[6 * e + a]!), box[3 + a] = Math.max(box[3 + a]!, boxes[6 * e + 3 + a]!);
    });
    batches.push({ first: order.length, n, size, cum, ecum, box });
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
