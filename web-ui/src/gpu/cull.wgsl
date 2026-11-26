// GPU culling of copies (ticket GPU1, ADR 0083; Rust's gpu_cull, ADR 0036 #10): one thread per
// placed copy. A copy is kept when its rank in its batch lies in the batch's range this frame (the
// CPU's frustum test of the batch, the moving budget's prefix, the proxies: gl/cull.ts, gl/proxies.ts),
// its element's box is not wholly outside one of the six planes of the view, and, in a moving frame,
// it is at least `t` times its own distance big (the budget's screen threshold, per copy instead of
// per batch: never more than the CPU counted). A kept copy is appended to its group's region of the
// visible list and counted in the group's drawIndexedIndirect arguments (instanceCount): one indirect
// draw per group, since WebGPU has no multi-draw-indirect.

struct Cull { planes: array<vec4f, 6>, eye: vec4f, t: f32, size_test: u32, n: u32, pad: u32 }
struct Inst { m0: vec4f, m1: vec4f, m2: vec4f, el: u32, batch: u32, rank: u32, group: u32 }

@group(0) @binding(0) var<uniform> U: Cull;
@group(0) @binding(1) var<storage, read> insts: array<Inst>;
@group(0) @binding(2) var<storage, read> boxes: array<f32>;
@group(0) @binding(3) var<storage, read> ranges: array<vec2u>;
@group(0) @binding(4) var<storage, read_write> visible: array<u32>;
@group(0) @binding(5) var<storage, read_write> args: array<atomic<u32>>;
@group(0) @binding(6) var<storage, read> bases: array<u32>;

@compute @workgroup_size(64) fn main(@builtin(global_invocation_id) gid: vec3u) {
  let i = gid.x;
  if (i >= U.n) { return; }
  let I = insts[i];
  let r = ranges[I.batch];
  if (I.rank < r.x || I.rank >= r.y) { return; }
  let lo = vec3f(boxes[6u * I.el], boxes[6u * I.el + 1u], boxes[6u * I.el + 2u]);
  let hi = vec3f(boxes[6u * I.el + 3u], boxes[6u * I.el + 4u], boxes[6u * I.el + 5u]);
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
  let slot = atomicAdd(&args[5u * I.group + 1u], 1u);
  visible[bases[I.group] + slot] = i;
}
