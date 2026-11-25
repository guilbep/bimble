// 3D navigation of the lab's WebGL viewport (ticket NAV1), ported from the Rust desktop:
// bimble-render `Camera` (zoom_at, orbit_about, on_target_plane, view_proj's clip planes) and
// bimble-viewer lib.rs (`wheel_pivot`, `orbit_pivot_at`, `handle_view_input`). Pure functions on the
// camera's numbers; glviewport.ts keeps the state and the picking.
//   wheel: zooms about the geometry under the cursor, else about the plane through the orbit target
//     facing the viewer; the pivot is kept while only the wheel moves the camera (same pointer, same
//     camera as after the last zoom), so a big model is not picked again at every notch;
//   orbit drag: turns rigidly about the geometry under the press, else the selection's centre, else
//     (no pivot) about the orbit target; 0.008 rad per pixel, pitch within ±1.57;
//   distance: 10 mm .. 1e8 mm; at the clamp the target does not keep sliding.
export type V3 = [number, number, number];

/** The orbit camera's numbers (bimble-render Camera: target, yaw, pitch, distance; z up). */
export interface OrbitCam {
  yaw: number;
  pitch: number;
  distance: number;
  target: V3;
}

/** lib.rs handle_view_input: a drag turns the view by 0.008 rad per pixel. */
export const ORBIT_RAD_PER_PX = 0.008;
/** Camera::orbit: the pitch stays within ±1.57 rad. */
export const PITCH_MAX = 1.57;
/** Camera::zoom_at: the distance stays within 10 mm .. 1e8 mm. */
export const DISTANCE_MIN = 10;
export const DISTANCE_MAX = 1e8;

const sub = (a: V3, b: V3): V3 => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const add = (a: V3, b: V3): V3 => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
const scale = (a: V3, k: number): V3 => [a[0] * k, a[1] * k, a[2] * k];
const dot = (a: V3, b: V3) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const cross = (a: V3, b: V3): V3 => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];

/** `v` turned by `angle` radians about the unit axis `k` (Rodrigues). */
function rotate(v: V3, k: V3, angle: number): V3 {
  const c = Math.cos(angle), s = Math.sin(angle);
  return add(add(scale(v, c), scale(cross(k, v), s)), scale(k, dot(k, v) * (1 - c)));
}

export function eyeOf(c: OrbitCam): V3 {
  const cp = Math.cos(c.pitch);
  return add(c.target, scale([cp * Math.cos(c.yaw), cp * Math.sin(c.yaw), Math.sin(c.pitch)], c.distance));
}

/** Camera::zoom_at: the camera scaled about `pivot` (scene mm), so the pivot stays where it is on
 *  screen. The factor actually applied is the clamped one. */
export function zoomAt(c: OrbitCam, factor: number, pivot: V3): OrbitCam {
  const d = Math.min(DISTANCE_MAX, Math.max(DISTANCE_MIN, c.distance * factor));
  const f = d / c.distance;
  return { ...c, distance: d, target: add(pivot, scale(sub(c.target, pivot), f)) };
}

/** Camera::orbit, then (with a pivot) Camera::orbit_about: the camera turns rigidly about the pivot,
 *  by the turn actually made (the pitch is clamped): about Z, then about the horizontal axis the pitch
 *  turns about (right of the view, before the yaw). */
export function orbitAbout(c: OrbitCam, dx: number, dy: number, pivot: V3 | null): OrbitCam {
  const yaw = c.yaw - dx;
  const pitch = Math.min(PITCH_MAX, Math.max(-PITCH_MAX, c.pitch + dy));
  if (!pivot) return { ...c, yaw, pitch };
  const h: V3 = [-Math.sin(c.yaw), Math.cos(c.yaw), 0];
  const v = rotate(rotate(sub(c.target, pivot), h, -(pitch - c.pitch)), [0, 0, 1], yaw - c.yaw);
  return { ...c, yaw, pitch, target: add(pivot, v) };
}

/** Camera::on_target_plane: where the ray (origin, unit direction) meets the plane through the
 *  target facing the viewer; the target when the ray runs along it. */
export function onTargetPlane(c: OrbitCam, origin: V3, dir: V3): V3 {
  const f = sub(c.target, eyeOf(c));
  const n = scale(f, 1 / (Math.hypot(f[0], f[1], f[2]) || 1));
  const den = dot(dir, n);
  if (Math.abs(den) < 1e-6) return [...c.target];
  return add(origin, scale(dir, dot(sub(c.target, origin), n) / den));
}

/** Camera::view_proj (perspective): the near and far planes from the scene's bounding sphere seen
 *  from the eye, not from the orbit target, so zooming or orbiting about a pivot away from the
 *  target never clips the model. Far/near under ~1e4, 1 mm minimum. */
export function clipPlanes(c: OrbitCam, center: V3, radius: number): { near: number; far: number } {
  const eye = eyeOf(c);
  const f = sub(c.target, eye);
  const len = Math.hypot(f[0], f[1], f[2]);
  const fwd: V3 = len > 0 ? scale(f, 1 / len) : [0, 0, -1];
  const r = Math.max(1, radius) * 1.01;
  const d = dot(sub(center, eye), fwd);
  const far = Math.max(1, d + r);
  const near = Math.max(1, d - r, far * 1e-4);
  return { near, far: Math.max(far, 2 * near) };
}

/** The wheel's zoom factor (< 1 zooms in). Pixel deltas as before (e^(0.001 px)); line and page
 *  deltas (Firefox with some settings) counted as 40 and 800 px. */
export function wheelFactor(e: { deltaY: number; deltaMode: number }): number {
  const px = e.deltaMode === 1 ? 40 * e.deltaY : e.deltaMode === 2 ? 800 * e.deltaY : e.deltaY;
  return Math.exp(px * 0.001);
}

/** The same camera (exactly): the wheel's pivot is kept only while nothing else moved it. */
export function sameCam(a: OrbitCam & { half?: number }, b: OrbitCam & { half?: number }): boolean {
  return a.yaw === b.yaw && a.pitch === b.pitch && a.distance === b.distance && a.half === b.half &&
    a.target[0] === b.target[0] && a.target[1] === b.target[1] && a.target[2] === b.target[2];
}
