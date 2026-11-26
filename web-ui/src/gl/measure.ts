// The measure tool on the viewport's 2D overlay (ticket T1; C state in ccore/session/measure.h).
// As Rust's `draw_dimensions` (crates/bimble-viewer/src/lib.rs): each kept measurement is a line
// with a dot at each end and its text in a filled label at its middle; while the tool is on, the
// rubber line from the first point to the hover with its length, the first point as a dot, and the
// snap marker under the pointer: a square on a corner, a triangle on an edge, a circle on a face.
// Colour: the style's `dimensions` (bimble-core style.rs: rgb(20, 110, 220)). The same in 3D and
// in a plan: points are world mm, `project` puts them on the overlay.
import type { MeasureState } from "../bimble";

/** A world point (mm) -> overlay CSS px, or null (behind the camera). */
export type Project = (x: number, y: number, z: number) => [number, number] | null;

const COLOUR = "rgb(20, 110, 220)";
const FONT = "13px sans-serif";

/** A filled label centred on `at` (lib.rs `label`: 13 px text, 4 px padding, 3 px corners). */
function label(ctx: CanvasRenderingContext2D, at: [number, number], text: string): void {
  ctx.font = FONT;
  const w = ctx.measureText(text).width + 8, h = 13 + 4;
  const x = at[0] - w / 2, y = at[1] - h / 2;
  ctx.fillStyle = COLOUR;
  ctx.beginPath();
  if (typeof ctx.roundRect === "function") ctx.roundRect(x, y, w, h, 3);
  else ctx.rect(x, y, w, h);
  ctx.fill();
  ctx.fillStyle = "#fff";
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.fillText(text, at[0], at[1] + 0.5);
}

function dot(ctx: CanvasRenderingContext2D, p: [number, number], r: number): void {
  ctx.fillStyle = COLOUR;
  ctx.beginPath();
  ctx.arc(p[0], p[1], r, 0, 2 * Math.PI);
  ctx.fill();
}

const mid = (a: [number, number], b: [number, number]): [number, number] => [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2];

/** Draws the measurements and the tool's pointer; pushes the texts drawn to `texts`. */
export function drawMeasure(ctx: CanvasRenderingContext2D, st: MeasureState | null, project: Project, texts: string[]): void {
  if (!st) return;
  ctx.save();
  ctx.setLineDash([]);
  ctx.strokeStyle = COLOUR;
  for (const m of st.items) {
    const a = project(...m.a), b = project(...m.b);
    if (!a || !b) continue;
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    ctx.moveTo(a[0], a[1]);
    ctx.lineTo(b[0], b[1]);
    ctx.stroke();
    dot(ctx, a, 3);
    dot(ctx, b, 3);
    label(ctx, mid(a, b), m.text);
    texts.push(m.text);
  }
  if (st.active) {
    const a = st.first ? project(...st.first) : null, h = st.hover ? project(...st.hover.at) : null;
    if (a && h && st.preview) {
      ctx.strokeStyle = COLOUR;
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.moveTo(a[0], a[1]);
      ctx.lineTo(h[0], h[1]);
      ctx.stroke();
      label(ctx, mid(a, h), st.preview.text);
      texts.push(st.preview.text);
    }
    if (a) dot(ctx, a, 3.5);
    if (h && st.hover) {
      ctx.strokeStyle = COLOUR;
      ctx.lineWidth = 2;
      ctx.beginPath();
      if (st.hover.snap === "corner") ctx.rect(h[0] - 5, h[1] - 5, 10, 10);
      else if (st.hover.snap === "edge") {
        ctx.moveTo(h[0], h[1] - 6);
        ctx.lineTo(h[0] + 6, h[1] + 5);
        ctx.lineTo(h[0] - 6, h[1] + 5);
        ctx.closePath();
      } else ctx.arc(h[0], h[1], 5, 0, 2 * Math.PI);
      ctx.stroke();
    }
  }
  ctx.restore();
}
