// The 3D view's look in WebGPU (ticket GPU1, ADR 0083): the same as glviewport.ts's VS/FS (WebGL2).
// Flat faces: the normal from the screen derivatives of the position, two-sided Lambert, ambient
// 0.62, light as bimble-render; one sRGB colour per element (its material family, the selection, a
// link's, the revealed colour), turned to linear, lit, turned back. The section box discards outside
// it (turned by its yaw); a back face seen through the cut is drawn unlit at the cut shade; an edge
// whose two faces both look away is not drawn while it cuts. The matrices are WebGL's (depth -1..1):
// the clip z is mapped to WebGPU's 0..1 here, after the plan edges' pull toward the eye.

struct Frame {
  mvp: mat4x4f,
  eye: vec4f,      // xyz; w = 1: xyz is the direction to the eye (orthographic)
  light: vec4f,    // xyz
  clip_min: vec4f, // xyz (scene mm); w = 1: the box cuts
  clip_max: vec4f, // xyz; w: the cut shade
  clip_rot: vec4f, // cos, sin of the box's yaw; z: the edges' depth pull (NDC); w: ambient
}
struct Draw { base: u32, use_list: u32, pad0: u32, pad1: u32, colour: vec4f }
// A placed copy: columns x, y, z, t of its placement in m0..m2 (xx xy xz yx | yy yz zx zy | zz tx ty tz),
// its element, its batch (index into the frame's ranges), its rank in the batch, its group.
struct Inst { m0: vec4f, m1: vec4f, m2: vec4f, el: u32, batch: u32, rank: u32, group: u32 }

@group(0) @binding(0) var<uniform> F: Frame;
@group(0) @binding(1) var<storage, read> colours: array<u32>;
@group(0) @binding(2) var<storage, read> insts: array<Inst>;
@group(0) @binding(3) var<storage, read> visible: array<u32>;
@group(0) @binding(4) var<uniform> D: Draw;

struct Out {
  @builtin(position) pos: vec4f,
  @location(0) world: vec3f,
  @location(1) @interpolate(flat) col: vec3f,
  @location(2) @interpolate(flat) back: f32,
}

fn unpack(c: u32) -> vec3f { return vec3f(f32((c >> 16u) & 255u), f32((c >> 8u) & 255u), f32(c & 255u)) / 255.0; }
fn to_clip(p: vec3f, pull: f32) -> vec4f {
  var c = F.mvp * vec4f(p, 1.0);
  c.z = (c.z - pull * c.w + c.w) * 0.5;
  return c;
}
fn inst_of(ii: u32) -> Inst {
  var id = ii;
  if (D.use_list == 1u) { id = visible[D.base + ii]; }
  return insts[id];
}
fn place(I: Inst, p: vec3f) -> vec3f {
  let x = vec3f(I.m0.x, I.m0.y, I.m0.z); let y = vec3f(I.m0.w, I.m1.x, I.m1.y);
  let z = vec3f(I.m1.z, I.m1.w, I.m2.x); let t = vec3f(I.m2.y, I.m2.z, I.m2.w);
  return x * p.x + y * p.y + z * p.z + t;
}
fn lin(I: Inst, n: vec3f) -> vec3f { return place(I, n) - vec3f(I.m2.y, I.m2.z, I.m2.w); }
fn to_eye(p: vec3f) -> vec3f { return select(F.eye.xyz - p, F.eye.xyz, F.eye.w > 0.5); }
fn back_of(p: vec3f, n1: vec3f, n2: vec3f) -> f32 {
  let e = to_eye(p);
  return select(0.0, 1.0, dot(n1, e) < 0.0 && dot(n2, e) < 0.0);
}

// faces of the merged lists: a vertex knows its element (its colour)
@vertex fn vs_merged(@location(0) p: vec3f, @location(1) el: u32) -> Out {
  var o: Out;
  o.pos = to_clip(p, 0.0); o.world = p; o.col = unpack(colours[el]); o.back = 0.0;
  return o;
}
// faces of a placed shape's copies: the copy from the compacted list (GPU culling) or by its index
@vertex fn vs_inst(@location(0) p: vec3f, @builtin(instance_index) ii: u32) -> Out {
  let I = inst_of(ii);
  let w = place(I, p);
  var o: Out;
  o.pos = to_clip(w, 0.0); o.world = w; o.col = unpack(colours[I.el]); o.back = 0.0;
  return o;
}
// faces of an HLOD proxy: a colour per vertex (its part's family)
@vertex fn vs_proxy(@location(0) p: vec3f, @location(1) c: vec4f) -> Out {
  var o: Out;
  o.pos = to_clip(p, 0.0); o.world = p; o.col = c.rgb; o.back = 0.0;
  return o;
}
// edges: the colour of the draw; the two face normals for the back-edge test
@vertex fn vs_edge(@location(0) p: vec3f, @location(1) n1: vec4f, @location(2) n2: vec4f) -> Out {
  var o: Out;
  o.pos = to_clip(p, F.clip_rot.z); o.world = p; o.col = D.colour.rgb; o.back = back_of(p, n1.xyz, n2.xyz);
  return o;
}
@vertex fn vs_edge_inst(@location(0) p: vec3f, @location(1) n1: vec4f, @location(2) n2: vec4f, @builtin(instance_index) ii: u32) -> Out {
  let I = inst_of(ii);
  let w = place(I, p);
  var o: Out;
  o.pos = to_clip(w, F.clip_rot.z); o.world = w; o.col = D.colour.rgb; o.back = back_of(w, lin(I, n1.xyz), lin(I, n2.xyz));
  return o;
}
// lines of a box (an unfinished batch's outline: the unit cube placed per instance; the section box)
@vertex fn vs_box(@location(0) p: vec3f, @location(1) x: vec3f, @location(2) y: vec3f, @location(3) z: vec3f, @location(4) t: vec3f) -> Out {
  let w = x * p.x + y * p.y + z * p.z + t;
  var o: Out;
  o.pos = to_clip(w, 0.0); o.world = w; o.col = D.colour.rgb; o.back = 0.0;
  return o;
}

fn to_linear(c: vec3f) -> vec3f { return select(pow((c + 0.055) / 1.055, vec3f(2.4)), c / 12.92, c < vec3f(0.04045)); }
fn to_srgb(c: vec3f) -> vec3f { return select(1.055 * pow(c, vec3f(1.0 / 2.4)) - 0.055, c * 12.92, c < vec3f(0.0031308)); }
fn outside(p: vec3f) -> bool {
  let r = F.clip_rot;
  let q = vec3f(r.x * p.x + r.y * p.y, r.x * p.y - r.y * p.x, p.z);
  return F.clip_min.w > 0.5 && (any(q < F.clip_min.xyz) || any(q > F.clip_max.xyz));
}

@fragment fn fs_face(i: Out, @builtin(front_facing) front: bool) -> @location(0) vec4f {
  let n = cross(dpdx(i.world), dpdy(i.world)); // first: derivatives in uniform control flow
  if (outside(i.world)) { discard; }
  if (F.clip_min.w > 0.5 && !front) { return vec4f(to_srgb(to_linear(i.col) * F.clip_max.w), 1.0); }
  let len2 = dot(n, n);
  var lambert = 1.0;
  if (len2 > 1e-30) { lambert = abs(dot(n, F.light.xyz)) * inverseSqrt(len2); }
  let k = F.clip_rot.w + (1.0 - F.clip_rot.w) * lambert;
  return vec4f(to_srgb(to_linear(i.col) * k), 1.0);
}
@fragment fn fs_edge(i: Out) -> @location(0) vec4f {
  if (outside(i.world)) { discard; }
  if (F.clip_min.w > 0.5 && i.back > 0.5) { discard; }
  return vec4f(i.col, 1.0);
}
@fragment fn fs_line(i: Out) -> @location(0) vec4f { return vec4f(i.col, 1.0); }
