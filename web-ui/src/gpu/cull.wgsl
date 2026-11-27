// GPU culling of copies (tickets GPU1, GPU2; ADR 0083; Rust's gpu_cull, ADR 0036 #10): one thread per
// placed copy of one model (a dispatch per model holding copies). A copy is kept when its rank in its
// batch lies in the batch's range this frame (the CPU's frustum test of the batch, the moving budget's
// prefix, the proxies: gl/cull.ts, gl/proxies.ts), its element's box (in its model's own frame) is not
// wholly outside one of the six planes of the view (GPU2: the planes and the eye turned into the model's
// frame: a link's move changes this uniform, never a buffer), and, in a moving frame, it is at least `t`
// times its own distance big. A kept copy is appended to its group's region of the model's visible list
// and counted in the group's drawIndexedIndirect arguments: one indirect draw per group (no
// multi-draw-indirect in WebGPU).

struct Cull { planes: array<vec4f, 6>, eye: vec4f, t: f32, size_test: u32, n: u32, pad: u32 }

@group(0) @binding(0) var<uniform> U: Cull;
@group(0) @binding(1) var<storage, read> iel: array<u32>;
@group(0) @binding(2) var<storage, read> boxes: array<f32>;
@group(0) @binding(3) var<storage, read> cmeta: array<vec4u>;
@group(0) @binding(4) var<storage, read> ranges: array<vec2u>;
@group(0) @binding(5) var<storage, read_write> visible: array<u32>;
@group(0) @binding(6) var<storage, read_write> args: array<atomic<u32>>;
@group(0) @binding(7) var<storage, read> bases: array<u32>;

@compute @workgroup_size(64) fn main(@builtin(global_invocation_id) gid: vec3u) {
  let i = gid.x;
  if (i >= U.n) { return; }
  let m = cmeta[i];
  let r = ranges[m.x];
  if (m.y < r.x || m.y >= r.y) { return; }
  let el = iel[i];
  let lo = vec3f(boxes[6u * el], boxes[6u * el + 1u], boxes[6u * el + 2u]);
  let hi = vec3f(boxes[6u * el + 3u], boxes[6u * el + 4u], boxes[6u * el + 5u]);
  for (var k = 0u; k < 6u; k++) {
    let p = U.planes[k];
    let far = select(lo, hi, p.xyz >= vec3f(0.0)); // the corner farthest along the plane's normal
    if (dot(p.xyz, far) + p.w < 0.0) { return; }
  }
  if (U.size_test == 1u) {
    let size = length(hi - lo) * 0.5;
    var dist = 1.0;
    if (U.eye.w < 0.5) { dist = max(1.0, distance(U.eye.xyz, clamp(U.eye.xyz, lo, hi))); }
    if (size < U.t * dist) { return; }
  }
  let slot = atomicAdd(&args[5u * m.z + 1u], 1u);
  visible[bases[m.z] + slot] = i;
}
