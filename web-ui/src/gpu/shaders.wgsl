// The 3D view's look in WebGPU (ticket GPU1, ADR 0083): the same as glviewport.ts's VS/FS (WebGL2).
// Flat faces: the normal from the screen derivatives of the position, two-sided Lambert, ambient
// 0.62, light as bimble-render; one sRGB colour per element, turned to linear, lit, turned back. The
// section box discards outside it (turned by its yaw); a back face seen through the cut is drawn unlit
// at the cut shade; an edge whose two faces both look away is not drawn while it cuts. The matrices
// are WebGL's (depth -1..1): the clip z is mapped to WebGPU's 0..1 here, after the plan edges' pull.
// GPU2 (ADR 0078 amendment 3, as WebGL2 since LNK3D): each model (the open one, each link) in its own
// buffers, placed by its row of `models` (gl/cull.ts xfRows; a move rewrites 64 bytes); each element's
// state from ONE source shared with WebGL2, the RGBA8 texel of gl/cull.ts packState (colour; a = model
// << 1 | drawn) read in the vertex stage by the element every vertex (faces AND edges: 4 bytes an edge
// vertex) or every copy carries; a hidden element's vertices leave the clip volume. A model all shown in
// its own colour reads no state (`plain`, WebGL2's u_plain).

struct Frame {
  mvp: mat4x4f,
  eye: vec4f,      // xyz; w = 1: xyz is the direction to the eye (orthographic)
  light: vec4f,    // xyz
  clip_min: vec4f, // xyz (scene mm); w = 1: the box cuts
  clip_max: vec4f, // xyz; w: the cut shade
  clip_rot: vec4f, // cos, sin of the box's yaw; z: the edges' depth pull (NDC); w: ambient
}
// A model's placement's three rows (R row, t) and its plain colour (w = 1: every element shown in it).
struct Model { m0: vec4f, m1: vec4f, m2: vec4f, plain: vec4f }
// A draw: its model, whether its copies come from the compacted list (GPU culling) and that list's start,
// whether faces take the element's colour (1) or `colour` (edges, proxies, lines; w: alpha).
struct Draw { model: u32, use_list: u32, base: u32, el_col: u32, colour: vec4f }

@group(0) @binding(0) var<uniform> F: Frame;
@group(0) @binding(1) var<storage, read> states: array<u32>;
@group(0) @binding(2) var<storage, read> models: array<Model>;
@group(0) @binding(3) var<uniform> D: Draw;
// a model's copies: their placements (12 floats: columns x, y, z, t, WebGL2's instMat) and elements
@group(1) @binding(0) var<storage, read> imat: array<f32>;
@group(1) @binding(1) var<storage, read> iel: array<u32>;
@group(1) @binding(2) var<storage, read> visible: array<u32>;

struct Out {
  @builtin(position) pos: vec4f,
  @location(0) world: vec3f,
  @location(1) @interpolate(flat) col: vec3f,
  @location(2) @interpolate(flat) back: f32,
}

const NONE = 0xffffffffu;
fn unpack(c: u32) -> vec3f { return vec3f(f32(c & 255u), f32((c >> 8u) & 255u), f32((c >> 16u) & 255u)) / 255.0; }
fn unpack_rgb(c: u32) -> vec3f { return vec3f(f32((c >> 16u) & 255u), f32((c >> 8u) & 255u), f32(c & 255u)) / 255.0; }
fn to_clip(p: vec3f, pull: f32) -> vec4f {
  var c = F.mvp * vec4f(p, 1.0);
  c.z = (c.z - pull * c.w + c.w) * 0.5;
  return c;
}
fn gone() -> Out { var o: Out; o.pos = vec4f(2.0, 2.0, 2.0, 1.0); o.world = vec3f(0.0); o.col = vec3f(0.0); o.back = 0.0; return o; }
/** The element's colour and whether it is drawn (w): its state texel, the model's plain colour, or the draw's. */
fn el_colour(el: u32) -> vec4f {
  if (el == NONE) { return vec4f(D.colour.rgb, 1.0); }
  let M = models[D.model];
  if (M.plain.w > 0.5) { return vec4f(select(D.colour.rgb, M.plain.rgb, D.el_col == 1u), 1.0); }
  let s = states[el];
  if (((s >> 24u) & 1u) == 0u) { return vec4f(0.0); }
  return vec4f(select(D.colour.rgb, unpack(s), D.el_col == 1u), 1.0);
}
fn in_model(p: vec3f) -> vec3f { let M = models[D.model]; return vec3f(dot(M.m0.xyz, p) + M.m0.w, dot(M.m1.xyz, p) + M.m1.w, dot(M.m2.xyz, p) + M.m2.w); }
fn turn_model(n: vec3f) -> vec3f { let M = models[D.model]; return vec3f(dot(M.m0.xyz, n), dot(M.m1.xyz, n), dot(M.m2.xyz, n)); }
fn copy_of(ii: u32) -> u32 { return select(ii, visible[D.base + ii], D.use_list == 1u); }
fn col_of(i: u32, c: u32) -> vec3f { return vec3f(imat[12u * i + 3u * c], imat[12u * i + 3u * c + 1u], imat[12u * i + 3u * c + 2u]); }
fn place(i: u32, p: vec3f) -> vec3f { return col_of(i, 0u) * p.x + col_of(i, 1u) * p.y + col_of(i, 2u) * p.z + col_of(i, 3u); }
fn lin(i: u32, n: vec3f) -> vec3f { return col_of(i, 0u) * n.x + col_of(i, 1u) * n.y + col_of(i, 2u) * n.z; }
fn to_eye(p: vec3f) -> vec3f { return select(F.eye.xyz - p, F.eye.xyz, F.eye.w > 0.5); }
fn back_of(p: vec3f, n1: vec3f, n2: vec3f) -> f32 {
  let e = to_eye(p);
  return select(0.0, 1.0, dot(n1, e) < 0.0 && dot(n2, e) < 0.0);
}

// faces of a model's merged list: a vertex knows its element
@vertex fn vs_merged(@location(0) p: vec3f, @location(1) el: u32) -> Out {
  let c = el_colour(el);
  if (c.w < 0.5) { return gone(); }
  let w = in_model(p);
  var o: Out;
  o.pos = to_clip(w, 0.0); o.world = w; o.col = c.rgb; o.back = 0.0;
  return o;
}
// faces of a placed shape's copies: the copy from the compacted list (GPU culling) or by its index
@vertex fn vs_inst(@location(0) p: vec3f, @builtin(instance_index) ii: u32) -> Out {
  let i = copy_of(ii);
  let c = el_colour(iel[i]);
  if (c.w < 0.5) { return gone(); }
  let w = in_model(place(i, p));
  var o: Out;
  o.pos = to_clip(w, 0.0); o.world = w; o.col = c.rgb; o.back = 0.0;
  return o;
}
// faces of the open model's HLOD proxies: a colour per vertex (its part's family)
@vertex fn vs_proxy(@location(0) p: vec3f, @location(1) c: vec4f) -> Out {
  var o: Out;
  o.pos = to_clip(p, 0.0); o.world = p; o.col = c.rgb; o.back = 0.0;
  return o;
}
// faces of a link's baked proxies: the draw's colour (the link's), placed by its model
@vertex fn vs_lprx(@location(0) p: vec3f) -> Out {
  let w = in_model(p);
  var o: Out;
  o.pos = to_clip(w, 0.0); o.world = w; o.col = D.colour.rgb; o.back = 0.0;
  return o;
}
// edges: the draw's colour, the element's drawn flag; the two face normals for the back-edge test
@vertex fn vs_edge(@location(0) p: vec3f, @location(1) n1: vec4f, @location(2) n2: vec4f, @location(3) el: u32) -> Out {
  let c = el_colour(el);
  if (c.w < 0.5) { return gone(); }
  let w = in_model(p);
  var o: Out;
  o.pos = to_clip(w, F.clip_rot.z); o.world = w; o.col = D.colour.rgb; o.back = back_of(w, turn_model(n1.xyz), turn_model(n2.xyz));
  return o;
}
@vertex fn vs_edge_inst(@location(0) p: vec3f, @location(1) n1: vec4f, @location(2) n2: vec4f, @builtin(instance_index) ii: u32) -> Out {
  let i = copy_of(ii);
  let c = el_colour(iel[i]);
  if (c.w < 0.5) { return gone(); }
  let w = in_model(place(i, p));
  var o: Out;
  o.pos = to_clip(w, F.clip_rot.z); o.world = w; o.col = D.colour.rgb; o.back = back_of(w, turn_model(lin(i, n1.xyz)), turn_model(lin(i, n2.xyz)));
  return o;
}
// lines of a box (an unfinished batch's outline: the unit cube placed per instance; the section box)
@vertex fn vs_box(@location(0) p: vec3f, @location(1) x: vec3f, @location(2) y: vec3f, @location(3) z: vec3f, @location(4) t: vec3f) -> Out {
  let w = x * p.x + y * p.y + z * p.z + t;
  var o: Out;
  o.pos = to_clip(w, 0.0); o.world = w; o.col = D.colour.rgb; o.back = 0.0;
  return o;
}
// the place tool's ghost and markers (scene mm): the draw's colour
@vertex fn vs_plain(@location(0) p: vec3f) -> Out {
  var o: Out;
  o.pos = to_clip(p, 0.0); o.world = p; o.col = D.colour.rgb; o.back = 0.0;
  return o;
}

fn to_linear(c: vec3f) -> vec3f { return select(pow((c + 0.055) / 1.055, vec3f(2.4)), c / 12.92, c < vec3f(0.04045)); }
fn to_srgb(c: vec3f) -> vec3f { return select(1.055 * pow(c, vec3f(1.0 / 2.4)) - 0.055, c * 12.92, c < vec3f(0.0031308)); }
fn outside(p: vec3f) -> bool {
  let r = F.clip_rot;
  let q = vec3f(r.x * p.x + r.y * p.y, r.x * p.y - r.y * p.x, p.z);
  return F.clip_min.w > 0.5 && (any(q < F.clip_min.xyz) || any(q > F.clip_max.xyz));
}
fn lit(i: Out, n: vec3f) -> vec3f {
  let len2 = dot(n, n);
  var lambert = 1.0;
  if (len2 > 1e-30) { lambert = abs(dot(n, F.light.xyz)) * inverseSqrt(len2); }
  let k = F.clip_rot.w + (1.0 - F.clip_rot.w) * lambert;
  return to_srgb(to_linear(i.col) * k);
}

@fragment fn fs_face(i: Out, @builtin(front_facing) front: bool) -> @location(0) vec4f {
  let n = cross(dpdx(i.world), dpdy(i.world)); // first: derivatives in uniform control flow
  if (outside(i.world)) { discard; }
  if (F.clip_min.w > 0.5 && !front) { return vec4f(to_srgb(to_linear(i.col) * F.clip_max.w), 1.0); }
  return vec4f(lit(i, n), 1.0);
}
// the ghost: lit, translucent (the draw's alpha), never cut
@fragment fn fs_ghost(i: Out) -> @location(0) vec4f {
  let n = cross(dpdx(i.world), dpdy(i.world));
  return vec4f(lit(i, n), D.colour.w);
}
@fragment fn fs_edge(i: Out) -> @location(0) vec4f {
  if (outside(i.world)) { discard; }
  if (F.clip_min.w > 0.5 && i.back > 0.5) { discard; }
  return vec4f(i.col, 1.0);
}
@fragment fn fs_line(i: Out) -> @location(0) vec4f { return vec4f(i.col, D.colour.w); }
