// Reference planes over the 3D view, plans and sections (ticket WAVE1B, ADR 0075): in 3D each
// stored plane is its bounded outline with its name; in a view that cuts (a plan's cut plane, a
// section's plane) it is the line where it meets that plane, dashed, with its name at one end. Flat
// strokes in one palette colour (the active work plane thicker), a fixed size on screen, on the
// overlay with the viewport's projection (`toLocal`: world mm -> overlay CSS px). The three-point
// tool's points so far are small crosses. Rust v0.19.0 has no reference plane (DEVIATIONS 32).

import type { RefPlanes } from "../bimble";

/** Reference planes: the palette's moss, as Revit's green work planes. */
export const REF_PLANE_CSS = "rgb(62, 120, 72)";

export interface RefPlaneDrawn { id: string; kind: "outline" | "line" | "point"; at: [number, number] }
type ToLocal = (x: number, y: number, z: number) => [number, number] | null;
type V3 = [number, number, number];
/** A plane that cuts the view: a point on it and its normal (world mm). */
export interface CutPlane { at: V3; n: V3 }

const dot = (a: V3, b: V3) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];

/** The segment where the outline `c` (4 corners) meets the cut plane, or null. */
export function cutSegment(c: V3[], cut: CutPlane): [V3, V3] | null {
  const d = c.map((p) => { const x = dot([p[0] - cut.at[0], p[1] - cut.at[1], p[2] - cut.at[2]], cut.n); return Math.abs(x) < 1e-6 ? 0 : x; });
  if (d.every((x) => x === 0)) return null; // lies in the cut plane: no line
  const pts: V3[] = [];
  for (let i = 0; i < c.length; i++) {
    const j = (i + 1) % c.length, a = c[i]!, b = c[j]!, da = d[i]!, db = d[j]!;
    if ((da < 0 && db > 0) || (da > 0 && db < 0)) {
      const t = da / (da - db);
      pts.push([a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1]), a[2] + t * (b[2] - a[2])]);
    } else if (da === 0 && pts.length < 2) pts.push(a);
  }
  return pts.length >= 2 ? [pts[0]!, pts[1]!] : null;
}

/** Draws the planes; returns where each was drawn (overlay CSS px), for the lab tests. */
export function drawRefPlanes(ctx: CanvasRenderingContext2D, rp: RefPlanes | null, toLocal: ToLocal, cut: CutPlane | null): RefPlaneDrawn[] {
  const out: RefPlaneDrawn[] = [];
  if (!rp) return out;
  ctx.save();
  ctx.strokeStyle = REF_PLANE_CSS;
  ctx.fillStyle = REF_PLANE_CSS;
  ctx.font = "12px 'Source Sans 3', sans-serif";
  for (const p of rp.planes) {
    const active = rp.active?.kind === "plane" && rp.active.id === p.id;
    ctx.lineWidth = active ? 2.5 : 1.25;
    if (cut) {
      const s = cutSegment(p.corners, cut);
      const a = s ? toLocal(...s[0]) : null, b = s ? toLocal(...s[1]) : null;
      if (!a || !b) continue;
      ctx.setLineDash([12, 4, 3, 4]);
      ctx.beginPath(); ctx.moveTo(a[0], a[1]); ctx.lineTo(b[0], b[1]); ctx.stroke();
      ctx.setLineDash([]);
      ctx.fillText(p.name, b[0] + 4, b[1] - 4);
      out.push({ id: p.id, kind: "line", at: [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2] });
      continue;
    }
    const q0 = p.corners.map((c) => toLocal(c[0], c[1], c[2]));
    if (q0.some((x) => !x)) continue;
    const q = q0 as [number, number][];
    ctx.setLineDash([8, 4]);
    ctx.beginPath();
    q.forEach((x, i) => (i ? ctx.lineTo(x[0], x[1]) : ctx.moveTo(x[0], x[1])));
    ctx.closePath();
    ctx.stroke();
    ctx.setLineDash([]);
    const top = q.reduce((m, x) => (x[1] < m[1] ? x : m), q[0]!);
    ctx.fillText(p.name, top[0] + 4, top[1] - 4);
    const cx = q.reduce((s, x) => s + x[0], 0) / 4, cy = q.reduce((s, x) => s + x[1], 0) / 4;
    out.push({ id: p.id, kind: "outline", at: [cx, cy] });
  }
  ctx.lineWidth = 1.5;
  for (const w of rp.pick.on ? rp.pick.points : []) {
    const x = toLocal(w[0], w[1], w[2]);
    if (!x) continue;
    ctx.beginPath(); ctx.moveTo(x[0] - 5, x[1] - 5); ctx.lineTo(x[0] + 5, x[1] + 5); ctx.moveTo(x[0] + 5, x[1] - 5); ctx.lineTo(x[0] - 5, x[1] + 5); ctx.stroke();
    out.push({ id: "", kind: "point", at: x });
  }
  ctx.restore();
  return out;
}
