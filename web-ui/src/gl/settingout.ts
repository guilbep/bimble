// The Setting out tab's markers over the 3D view and plans (ticket SETOUT, ADR 0075): the project
// base point (a circle with a cross, at the world origin, with a north arrow: true north filled,
// project north a thin line when it differs) and the survey point (a triangle, at its place or at
// the base point while not placed), each while its ribbon button is pressed (actions
// show_base_point, show_survey_point: `query setting_out` says `shown`). Flat strokes in palette
// colours, a fixed size on screen (not scaled with the model), drawn on the overlay with the
// viewport's projection (`toLocal`: world mm -> overlay CSS px), so in 3D and in a plan alike.
// Rust has no such markers (DEVIATIONS 32).

import type { SettingOut } from "../bimble";

/** The base point: the annotations' blue (glviewport.ts ANNOT_CSS family). */
export const BASE_POINT_CSS = "rgb(31, 78, 140)";
/** The survey point: the palette's moss. */
export const SURVEY_POINT_CSS = "rgb(62, 120, 72)";
/** True north's arrow: the palette's clay. */
export const NORTH_CSS = "rgb(178, 74, 46)";
const R = 9; // px
const ARROW = 38; // px

export interface MarkerDrawn { kind: "base_point" | "survey_point" | "true_north" | "project_north"; at: [number, number] }
type ToLocal = (x: number, y: number, z: number) => [number, number] | null;

/** The screen direction (unit, CSS px) of a world plan direction at `at`, or null (seen end on). */
function screenDir(toLocal: ToLocal, at: [number, number, number], d: [number, number]): [number, number] | null {
  const p0 = toLocal(at[0], at[1], at[2]), p1 = toLocal(at[0] + d[0] * 1000, at[1] + d[1] * 1000, at[2]);
  if (!p0 || !p1) return null;
  const dx = p1[0] - p0[0], dy = p1[1] - p0[1], l = Math.hypot(dx, dy);
  return l > 1e-6 ? [dx / l, dy / l] : null;
}

/** Draws the markers shown; returns where each was drawn (overlay CSS px), for the lab tests. */
export function drawSettingOut(ctx: CanvasRenderingContext2D, so: SettingOut | null, toLocal: ToLocal): MarkerDrawn[] {
  const out: MarkerDrawn[] = [];
  if (!so) return out;
  ctx.save();
  ctx.lineWidth = 1.5;
  ctx.setLineDash([]);
  if (so.survey_point.shown) {
    const a = so.survey_point.at_mm, p = toLocal(a[0], a[1], a[2]);
    if (p) {
      const h = R * 1.15;
      ctx.beginPath();
      ctx.moveTo(p[0], p[1] - h);
      ctx.lineTo(p[0] + h * 0.94, p[1] + h * 0.6);
      ctx.lineTo(p[0] - h * 0.94, p[1] + h * 0.6);
      ctx.closePath();
      ctx.fillStyle = "rgba(255, 255, 255, 0.85)";
      ctx.fill();
      ctx.strokeStyle = SURVEY_POINT_CSS;
      ctx.stroke();
      ctx.beginPath();
      ctx.arc(p[0], p[1], 1.8, 0, 2 * Math.PI);
      ctx.fillStyle = SURVEY_POINT_CSS;
      ctx.fill();
      out.push({ kind: "survey_point", at: p });
    }
  }
  if (so.base_point.shown) {
    const a = so.base_point.at_mm, p = toLocal(a[0], a[1], a[2]);
    if (p) {
      // North: true north's arrow, project north's line when it points elsewhere.
      const tn = screenDir(toLocal, a, so.north.true), pn = screenDir(toLocal, a, so.north.project);
      if (pn && tn && Math.hypot(pn[0] - tn[0], pn[1] - tn[1]) > 1e-3) {
        const e: [number, number] = [p[0] + pn[0] * ARROW, p[1] + pn[1] * ARROW];
        ctx.beginPath();
        ctx.moveTo(p[0] + pn[0] * R, p[1] + pn[1] * R);
        ctx.lineTo(e[0], e[1]);
        ctx.strokeStyle = BASE_POINT_CSS;
        ctx.stroke();
        out.push({ kind: "project_north", at: e });
      }
      if (tn) {
        const e: [number, number] = [p[0] + tn[0] * ARROW, p[1] + tn[1] * ARROW], n: [number, number] = [-tn[1], tn[0]];
        ctx.beginPath();
        ctx.moveTo(p[0] + tn[0] * R, p[1] + tn[1] * R);
        ctx.lineTo(e[0], e[1]);
        ctx.strokeStyle = NORTH_CSS;
        ctx.stroke();
        ctx.beginPath();
        ctx.moveTo(e[0] + tn[0] * 8, e[1] + tn[1] * 8);
        ctx.lineTo(e[0] + n[0] * 4, e[1] + n[1] * 4);
        ctx.lineTo(e[0] - n[0] * 4, e[1] - n[1] * 4);
        ctx.closePath();
        ctx.fillStyle = NORTH_CSS;
        ctx.fill();
        ctx.font = "600 11px 'DM Mono', monospace";
        ctx.textAlign = "center";
        ctx.textBaseline = "middle";
        ctx.fillText("N", e[0] + tn[0] * 17, e[1] + tn[1] * 17);
        out.push({ kind: "true_north", at: e });
      }
      ctx.beginPath();
      ctx.arc(p[0], p[1], R, 0, 2 * Math.PI);
      ctx.fillStyle = "rgba(255, 255, 255, 0.85)";
      ctx.fill();
      ctx.strokeStyle = BASE_POINT_CSS;
      ctx.stroke();
      ctx.beginPath();
      ctx.moveTo(p[0] - R, p[1]); ctx.lineTo(p[0] + R, p[1]);
      ctx.moveTo(p[0], p[1] - R); ctx.lineTo(p[0], p[1] + R);
      ctx.stroke();
      out.push({ kind: "base_point", at: p });
    }
  }
  ctx.restore();
  return out;
}
