// A section shown live (ticket SEC1): the camera of a section tab, from the C core's `view_clip`
// (ccore/session/calls.c q_view_clip: the box Rust's views.rs view_clip gives, the look `dir`).
// As Rust's camera_angles for a section: orthographic, the eye on the near side of the cut looking
// along the facing, z up, screen right = section_right(dir) = (dir.y, -dir.x). The plan camera's
// path in glviewport.ts with this frame: pan and zoom as a plan, rays along the look.
import type { SectionClip, ViewClip } from "../bimble";

type V3 = [number, number, number];
const sub = (a: V3, b: V3): V3 => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const add = (a: V3, b: V3): V3 => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
const scale = (a: V3, k: number): V3 => [a[0] * k, a[1] * k, a[2] * k];
const dot = (a: V3, b: V3) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const cross = (a: V3, b: V3): V3 => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
const norm = (a: V3): V3 => scale(a, 1 / (Math.hypot(a[0], a[1], a[2]) || 1));
function mul(a: Float32Array, b: Float32Array): Float32Array {
  const o = new Float32Array(16);
  for (let c = 0; c < 4; c++)
    for (let r = 0; r < 4; r++) {
      let s = 0;
      for (let k = 0; k < 4; k++) s += (a[k * 4 + r] ?? 0) * (b[c * 4 + k] ?? 0);
      o[c * 4 + r] = s;
    }
  return o;
}
function lookAt(eye: V3, target: V3, up: V3): Float32Array {
  const f = norm(sub(target, eye)), s = norm(cross(f, up)), v = cross(s, f);
  return new Float32Array([s[0], v[0], -f[0], 0, s[1], v[1], -f[1], 0, s[2], v[2], -f[2], 0, -dot(s, eye), -dot(v, eye), dot(f, eye), 1]);
}

/** The view shown is a section (a `view_clip` of type "section"). */
export const isSection = (vc: ViewClip | null): vc is SectionClip => vc?.type === "section";

/** The look direction, scene x y z (the facing turned to project north). */
export const lookDir = (vc: SectionClip): V3 => [vc.dir?.[0] ?? 0, vc.dir?.[1] ?? 1, 0];

/** The camera matrices of a section: eye `4r + 1000` mm before the target along the look. */
export function sectionMatrices(vc: SectionClip, target: number[], r: number, half: number, aspect: number) {
  const d = lookDir(vc), t: V3 = [target[0] ?? 0, target[1] ?? 0, target[2] ?? 0];
  const h = 4 * r + 1000, up: V3 = [0, 0, 1];
  const e = add(t, scale(d, -h));
  const view = lookAt(e, t, up);
  const near = 1, far = 2 * h, w = half * aspect;
  const proj = new Float32Array([1 / w, 0, 0, 0, 0, 1 / half, 0, 0, 0, 0, -2 / (far - near), 0, 0, 0, -(far + near) / (far - near), 1]);
  return { e, view, proj, mvp: mul(proj, view), aspect, up };
}

/** A section fitted: the box's middle as target; its width along the cut and its height on screen. */
export function sectionFit(vc: SectionClip, c: { min: number[]; max: number[] }, aspect: number): { target: V3; half: number } {
  const d = lookDir(vc);
  const mid = (k: number) => ((c.min[k] ?? 0) + (c.max[k] ?? 0)) / 2;
  const target: V3 = [mid(0), mid(1), mid(2)];
  const ext = (k: number) => (c.max[k] ?? 0) - (c.min[k] ?? 0);
  const along = Math.abs(d[0]) > Math.abs(d[1]) ? ext(1) : ext(0); // looking east or west: y runs across the screen
  return { target, half: 1.05 * Math.max(ext(2) / 2, along / 2 / aspect, 100) };
}
