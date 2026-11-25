// Reference plans over a 2D view (ticket L1; Rust: viewing.rs draw_references): the PDF (later DXF)
// lines a view holds (ccore/session/links_ref.h, `query view_references`, placed in C: world plan
// mm), drawn on the overlay canvas in the reference edge colour (bimble-core style.rs
// `reference_edges`), 1 px, under the annotations. The page asks again when the model's rev or
// the view changes (a link, undo, redo), never per frame.

import { viewReferences, type ViewReferences } from "../bimble";

/** style.rs `reference_edges`: rgb(52, 110, 180). */
export const REFERENCE_PLAN_CSS = "rgb(52, 110, 180)";

export interface ReferenceLayer {
  /** The references of the view as last fetched (null: none, or not a 2D view). */
  refs: ViewReferences | null;
  /** Asks the C core again when `key` (rev and view) changed: true when it did (draw again). */
  refresh(key: string, inPlan: boolean): boolean;
  /** Draws the lines with `toLocal` (world plan mm -> overlay CSS px): the segment count drawn. */
  draw(ctx: CanvasRenderingContext2D, toLocal: (x: number, y: number) => [number, number] | null): number;
}

/** Test hook (`window.__bbRefs`): draws, segments in the last draw, its ms (stroke submitted). */
export interface ReferenceStats { draws: number; segments: number; lastDrawMs: number }
const stats: ReferenceStats = { draws: 0, segments: 0, lastDrawMs: 0 };
(globalThis as { __bbRefs?: ReferenceStats }).__bbRefs = stats;

export function referenceLayer(): ReferenceLayer {
  let key = "";
  const layer: ReferenceLayer = {
    refs: null,
    refresh(k, inPlan) {
      const want = inPlan ? k : "";
      if (want === key) return false;
      key = want;
      layer.refs = inPlan ? viewReferences() : null;
      return true;
    },
    draw(ctx, toLocal) {
      const refs = layer.refs;
      if (!refs || refs.shown === 0) return (stats.segments = 0);
      const t0 = performance.now();
      // Two projected points give the affine map world mm -> px of an orthographic plan: the
      // lines are mapped with it, not one matrix product per point.
      const o = toLocal(0, 0), ex = toLocal(1000, 0), ey = toLocal(0, 1000);
      if (!o || !ex || !ey) return 0;
      const ax = (ex[0] - o[0]) / 1000, bx = (ex[1] - o[1]) / 1000, ay = (ey[0] - o[0]) / 1000, by = (ey[1] - o[1]) / 1000;
      ctx.strokeStyle = REFERENCE_PLAN_CSS;
      ctx.lineWidth = 1;
      ctx.setLineDash([]);
      ctx.beginPath();
      let n = 0;
      for (const r of refs.references) {
        if (!r.visible) continue;
        for (const pl of r.polylines) {
          for (let i = 0; i + 1 < pl.length; i += 2) {
            const x = pl[i]!, y = pl[i + 1]!;
            const px = o[0] + ax * x + ay * y, py = o[1] + bx * x + by * y;
            if (i === 0) ctx.moveTo(px, py);
            else { ctx.lineTo(px, py); n++; }
          }
        }
      }
      ctx.stroke();
      stats.draws++;
      stats.segments = n;
      stats.lastDrawMs = performance.now() - t0;
      return n;
    },
  };
  return layer;
}
