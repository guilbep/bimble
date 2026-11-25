// Grid axes over a plan view (ticket G2). Rust draws grid axes on paper only (crates/bimble-viewer
// has no on-screen grid; bimble-core style.rs `axis_pen_mm`, `axis_dash_mm`), so this follows the
// paper look of ccore/src/draw_annot.c `cc_annot_axis_lines` at screen scale (1 paper mm = 3 px):
// a thin ink line, dash-dot 12 / 1.5 / 0.5 / 1.5 mm, and the tag in a bubble of 8 mm. On paper the
// bubble sits at the top or left end only; on screen there is one at each end, so a tag stays in
// sight when the plan is zoomed in on either end. The axes are the C core's `query grid_axes`
// (session/views.h: the open model's grids or a read-only scene's axes, then each visible linked
// model's), world plan mm, drawn on the overlay under the annotations. The page asks again when the
// model's rev, the links or the view change, never per frame.

import { gridAxes, type GridAxes } from "../bimble";

/** Paper ink (draw_style.c pens are black; style.rs `edges` on screen). */
export const GRID_AXIS_CSS = "rgb(26, 26, 28)";
/** 1 paper mm on screen, px. */
const MM = 3;
/** draw_style.c AXES: dash 12, gap 1.5, dot 0.5, gap 1.5 (paper mm). */
const DASH = [12 * MM, 1.5 * MM, 0.5 * MM, 1.5 * MM];
/** draw_annot.c BUBBLE: 8 mm across. */
export const BUBBLE_R = 4 * MM;

export interface GridBubble { tag: string; at: [number, number] }
export interface GridLayer {
  /** The axes as last fetched (null: none, or not a plan). */
  axes: GridAxes | null;
  /** Asks the C core again when `key` changed: true when it did (draw again). */
  refresh(key: string, inPlan: boolean): boolean;
  /** Draws the axes with `toLocal` (world plan mm -> overlay CSS px): the number of axes drawn. */
  draw(ctx: CanvasRenderingContext2D, toLocal: (x: number, y: number) => [number, number] | null): number;
}

/** Test hook (`window.__bbGrids`): draws, axes in the last draw, the bubbles drawn (overlay CSS px). */
export interface GridStats { draws: number; axes: number; bubbles: GridBubble[] }
const stats: GridStats = { draws: 0, axes: 0, bubbles: [] };
(globalThis as { __bbGrids?: GridStats }).__bbGrids = stats;

export function gridLayer(): GridLayer {
  let key = "";
  const layer: GridLayer = {
    axes: null,
    refresh(k, inPlan) {
      const want = inPlan ? k : "";
      if (want === key) return false;
      key = want;
      layer.axes = inPlan ? gridAxes() : null;
      return true;
    },
    draw(ctx, toLocal) {
      stats.bubbles = [];
      const g = layer.axes;
      const all = g ? [...g.axes, ...g.links.filter((l) => l.visible).flatMap((l) => l.axes)] : [];
      if (!all.length) return (stats.axes = 0);
      const bubbles: GridBubble[] = [];
      ctx.strokeStyle = GRID_AXIS_CSS;
      ctx.lineWidth = 1;
      ctx.setLineDash(DASH);
      ctx.beginPath();
      for (const a of all) {
        const p = toLocal(a.start_mm[0], a.start_mm[1]), q = toLocal(a.end_mm[0], a.end_mm[1]);
        if (!p || !q) continue;
        const dx = q[0] - p[0], dy = q[1] - p[1], len = Math.hypot(dx, dy);
        if (len < 1e-6) continue;
        const ux = dx / len, uy = dy / len;
        ctx.moveTo(p[0], p[1]);
        ctx.lineTo(q[0], q[1]);
        // The bubbles just past each end, on the axis.
        bubbles.push({ tag: a.tag, at: [p[0] - ux * BUBBLE_R, p[1] - uy * BUBBLE_R] });
        bubbles.push({ tag: a.tag, at: [q[0] + ux * BUBBLE_R, q[1] + uy * BUBBLE_R] });
      }
      ctx.stroke();
      ctx.setLineDash([]);
      ctx.font = `${(2.5 * MM * 1.4).toFixed(1)}px sans-serif`;
      ctx.textAlign = "center";
      ctx.textBaseline = "middle";
      for (const b of bubbles) {
        ctx.beginPath();
        ctx.arc(b.at[0], b.at[1], BUBBLE_R, 0, 2 * Math.PI);
        ctx.fillStyle = "rgb(255, 255, 255)";
        ctx.fill();
        ctx.stroke();
        ctx.fillStyle = GRID_AXIS_CSS;
        ctx.fillText(b.tag, b.at[0], b.at[1] + 0.5);
      }
      stats.draws++;
      stats.axes = bubbles.length / 2;
      stats.bubbles = bubbles;
      return stats.axes;
    },
  };
  return layer;
}
