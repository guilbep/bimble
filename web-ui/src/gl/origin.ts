// The render origin (ticket VIS2, ADR 0078 amendment 9). The page's scene frame is the open model's: an empty project
// whose links are georeferenced (Swiss coordinates: 2,536 km east, 1,153 km north of the open model's origin) put every
// vertex at 2.5e9 mm, where f32 steps are 256 mm. The GPU computed R p + t and then u_mvp p in f32 there, so the
// picture's depth moved by up to that much, while the CPU tested element boxes rounded to f32 against an f32 matrix:
// the culling left visible elements out (4,365 on a close camera of the owner's project of 17 links) and the
// frustum test lost some at the picture's edges. Now the CPU keeps the scene frame in f64 (matrices, element boxes)
// and the GPU draws relative to a point near the geometry: u_mvp = mvp T(ro), a link's translation t - ro, the open
// model's vertices p - ro. Shared with the WebGPU renderer when it draws the same scenes (gpu/: not yet).

/** Under this distance (mm) of the scene's centre from its origin the render origin stays 0 (f32 steps under 0.07 mm). */
export const RO_MIN_MM = 1e6;

/** The render origin for a scene whose bounds' centre is `c` (scene mm): 0 when near, else the centre on a 1 m grid. */
export function renderOrigin(c: ArrayLike<number> | null): [number, number, number] {
  if (!c || Math.max(Math.abs(c[0]!), Math.abs(c[1]!), Math.abs(c[2]!)) <= RO_MIN_MM) return [0, 0, 0];
  return [Math.round(c[0]! / 1000) * 1000, Math.round(c[1]! / 1000) * 1000, Math.round(c[2]! / 1000) * 1000];
}

/** The view-projection the GPU gets (f32): `m` (column-major, f64, the scene frame) times the translation by `ro`, so a
 *  point given relative to `ro` lands where `m` puts the scene point. The translation column is summed in f64 before
 *  the cast: what is left is small near the eye. */
export function relMvp(m: ArrayLike<number>, ro: ArrayLike<number>): Float32Array {
  const o = Float32Array.from(m);
  for (let r = 0; r < 4; r++) o[12 + r] = m[r]! * ro[0]! + m[4 + r]! * ro[1]! + m[8 + r]! * ro[2]! + m[12 + r]!;
  return o;
}
