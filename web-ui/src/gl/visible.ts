// "Draw only what can be seen" (ticket VIS1, ADR 0078 amendment 7): per batch, which of its items are not hidden
// behind the last picture's depth (gl/hiz.ts), as runs of consecutive items, so a frame submits those ranges only
// (merged faces: one multi-draw of the ranges where WEBGL_multi_draw exists; instanced copies: a draw a run). The
// batch's box is tested first: a batch wholly hidden costs one test. Shared with the WebGPU renderer: it needs the
// same per-batch masks (gpu/ draws batches as ranges too) and a pyramid of its own picture's depth.
import { boxHidden, type Pyramid } from "./hiz";

export interface VisBatch { b: { n: number; box: Float32Array; cum: Uint32Array }; items: ArrayLike<number>;
  /** The item mask (1: may be seen) made for `vkey`, and the runs for [k0, k1) this frame (pairs of item indices). */
  vmask?: Uint8Array | null; vkey?: string; runs?: number[] | null }

/** The mask of a batch's items against the pyramid (made once per pyramid): hidden ones 0. `shown[e]` 0: not drawn
 *  anyway (left out too). Returns the triangles and items left out. */
export function maskBatch(p: Pyramid, d: VisBatch, boxes: Float32Array, shown: Uint8Array, tris: (i: number) => number): { items: number; tris: number } {
  const n = d.b.n, mask = d.vmask && d.vmask.length === n ? d.vmask : new Uint8Array(n);
  let items = 0, t = 0;
  if (boxHidden(p, d.b.box, 0)) { mask.fill(0); items = n; for (let i = 0; i < n; i++) t += tris(i); }
  else for (let i = 0; i < n; i++) {
    const e = d.items[i]!;
    const on = shown[e]! > 0 && !boxHidden(p, boxes, 6 * e);
    mask[i] = on ? 1 : 0;
    if (!on) items++, t += tris(i);
  }
  d.vmask = mask, d.vkey = `${p.key}#${p.serial}`;
  return { items, tris: t };
}

/** The runs [i, j) of items in [k0, k1) the mask keeps. */
export function runsOf(mask: Uint8Array, k0: number, k1: number): number[] {
  const out: number[] = [];
  let i = k0;
  while (i < k1) {
    while (i < k1 && !mask[i]) i++;
    if (i >= k1) break;
    let j = i;
    while (j < k1 && mask[j]) j++;
    out.push(i, j);
    i = j;
  }
  return out;
}
