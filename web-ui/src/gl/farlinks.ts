// REAL1: a model far from the others. A linked IFC without georeference sits at its own origin, thousands of
// km from the georeferenced ones (a local folder of 17 files: one at 2,800 km). The scene's bounds then span
// the whole distance: Fit all shows a dot, and the clip planes (gl/nav.ts clipPlanes: near >= far x 1e-4)
// clip everything within hundreds of metres of the eye. Such a model is left out of the scene's bounds (Fit
// all, the clip planes); it stays where its file puts it, and the Links panel says how far it is.
import { shallowRef } from "vue";

/** By link name: its distance (km) from the others when it is left out of the scene's bounds. */
export const linkFar = shallowRef<Record<string, number>>({});

/** A model is far when its centre is farther than max(FAR_MM, FAR_RADII x the largest radius of the
 *  others' median) from the models' median centre. */
export const FAR_MM = 10_000_000, FAR_RADII = 20;

export interface ModelBox { min: number[]; max: number[] }

/** The far models (index -> distance in mm from the median centre); never all of them. */
export function farModels(boxes: (ModelBox | null)[]): Map<number, number> {
  const live = boxes.map((b, k) => ({ b, k })).filter((x): x is { b: ModelBox; k: number } => x.b !== null);
  const out = new Map<number, number>();
  if (live.length < 2) return out;
  const c = live.map(({ b }) => [0, 1, 2].map((a) => (b.min[a]! + b.max[a]!) / 2));
  const r = live.map(({ b }) => Math.hypot(...[0, 1, 2].map((a) => b.max[a]! - b.min[a]!)) / 2);
  const median = (xs: number[]) => { const s = [...xs].sort((p, q) => p - q); return s[Math.floor((s.length - 1) / 2)]!; };
  const mc = [0, 1, 2].map((a) => median(c.map((p) => p[a]!)));
  const limit = Math.max(FAR_MM, FAR_RADII * median(r));
  live.forEach(({ k }, i) => { const d = Math.hypot(...[0, 1, 2].map((a) => c[i]![a]! - mc[a]!)); if (d > limit) out.set(k, d); });
  if (out.size === live.length) out.clear();
  return out;
}
