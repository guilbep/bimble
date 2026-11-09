// Drawing-style shading: flat faces, a soft directional light, and screen-space hatching per
// material family. No textures: cheap on old integrated GPUs.

// One entry of the style table (see style.rs, GpuFill). Lengths are in pixels; colours are
// linear (converted once on the CPU).
struct Fill {
    // rgb: fill, a: pattern bits (1: lines[0], 2: lines[1], 4: stipple)
    fill: vec4<f32>,
    // rgb: hatch ink, a: 1 = ink is its own colour (shaded), 0 = darkened fill (already shaded)
    ink: vec4<f32>,
    // per line family: xy = normal / spacing, z = line width (fraction), w = 1 if present
    lines: array<vec4<f32>, 2>,
    // per line family: xy = direction along the lines / dash period, z = on fraction, w = 1 if dashed
    dashes: array<vec4<f32>, 2>,
    // x: 1 / cell size, y: density, z: strength
    stipple: vec4<f32>,
};

struct Uniforms {
    view_proj: mat4x4<f32>,
    // xyz: light direction (world, towards the light)
    light: vec4<f32>,
    // rgb: edge colour (sRGB), a: unused
    edge: vec4<f32>,
    // x: hatch scale (pixels per hatch unit), y: hatch strength 0..1, z: ambient light 0..1,
    // w: brightness of cut surfaces (clipping box) 0..1
    params: vec4<f32>,
    // Clipping box in scene coordinates; clip_min.w = 1 when it is on.
    clip_min: vec4<f32>,
    clip_max: vec4<f32>,
    // xyz: camera position (scene coordinates), w: 1 for orthographic
    eye: vec4<f32>,
    // xyz: view direction
    view_dir: vec4<f32>,
    // x: depth push of this layer (linked models lose against the model where they coincide)
    layer: vec4<f32>,
    table: array<Fill, 16>,
};

@group(0) @binding(0) var<uniform> u: Uniforms;

struct FaceIn {
    @location(0) pos: vec3<f32>,
    // a: style table slot / 255
    @location(1) slot: vec4<f32>,
};

// A face's normal (not normalized, either sign) from how the position changes across the screen:
// vertices are shared by the faces around them (indexed, ADR 0036 step 2), so they carry none.
// Called before any discard (derivatives need the whole 2×2 pixel quad).
fn screen_normal(p: vec3<f32>) -> vec3<f32> {
    return cross(dpdx(p), dpdy(p));
}

// Every draw is instanced (ADR 0034): a shared shape once per element that is it, and merged
// geometry as one identity instance. Columns of the placement, and the element's style slot;
// slot 255 means "the vertex's own" (merged geometry: one buffer, many elements).
struct Inst {
    @location(3) x: vec3<f32>,
    @location(4) y: vec3<f32>,
    @location(5) z: vec3<f32>,
    @location(6) t: vec3<f32>,
    @location(7) slot: vec4<f32>,
};

fn place(i: Inst, p: vec3<f32>) -> vec3<f32> {
    return i.x * p.x + i.y * p.y + i.z * p.z + i.t;
}

// Normals turn with the shape (exact for rotations and mirrors; edges' back-facing test).
fn turn(i: Inst, n: vec3<f32>) -> vec3<f32> {
    return i.x * n.x + i.y * n.y + i.z * n.z;
}

fn slot_of(v: FaceIn, i: Inst) -> u32 {
    if i.slot.a > 0.999 {
        return face_slot(v.slot);
    }
    return face_slot(i.slot);
}

// Two variants of each shader: a lean one for normal viewing, and one for the section box
// (scene position passed to pixels, cut surfaces, back edges). Extra per-vertex outputs cost
// memory and time on software and old GPUs, so normal viewing must not pay for the box.

// Two-sided Lambert with a high ambient: reads like a shaded drawing.
// A degenerate normal (a face seen edge-on, a sliver) is lit fully rather than NaN.
fn face_shade(normal: vec3<f32>) -> f32 {
    let l = normalize(u.light.xyz);
    let ambient = u.params.z;
    let len2 = dot(normal, normal);
    let lambert = select(1.0, abs(dot(normal, l)) * inverseSqrt(len2), len2 > 1e-30);
    return ambient + (1.0 - ambient) * lambert;
}

fn face_slot(color: vec4<f32>) -> u32 {
    return min(u32(round(color.a * 255.0)), 15u);
}

fn linear_to_srgb(c: vec3<f32>) -> vec3<f32> {
    let lo = c * 12.92;
    let hi = 1.055 * pow(max(c, vec3<f32>(0.0)), vec3<f32>(1.0 / 2.4)) - 0.055;
    return select(hi, lo, c <= vec3<f32>(0.0031308));
}

fn stripe(t: f32, width: f32) -> f32 {
    // 1 inside a stripe of relative `width`, 0 elsewhere.
    return select(0.0, 1.0, fract(t) < width);
}

// Ink of one line family at pixel p: 0 or 1.
fn family(p: vec2<f32>, l: vec4<f32>, dash: vec4<f32>) -> f32 {
    var ink = stripe(dot(p, l.xy), l.z);
    if dash.w != 0.0 {
        ink = ink * stripe(dot(p, dash.xy), dash.z);
    }
    return ink;
}

// Hatch ink (0..1) of a style slot at a pixel. Families without hatch (and hatching off) skip the
// pattern code entirely; otherwise only the patterns the family has are evaluated.
fn hatch(pixel: vec2<f32>, slot: u32) -> f32 {
    let bits = u32(u.table[slot].fill.a);
    if u.params.y <= 0.0 || bits == 0u {
        return 0.0;
    }
    let p = pixel / max(u.params.x, 1.0);
    var ink = 0.0;
    if (bits & 1u) != 0u {
        ink = family(p, u.table[slot].lines[0], u.table[slot].dashes[0]);
    }
    if (bits & 2u) != 0u {
        ink = max(ink, family(p, u.table[slot].lines[1], u.table[slot].dashes[1]));
    }
    if (bits & 4u) != 0u {
        let st = u.table[slot].stipple;
        let cell = floor(p * st.x);
        let h = fract(sin(dot(cell, vec2<f32>(12.9898, 78.233))) * 43758.5453);
        ink = max(ink, select(0.0, st.z, h > 1.0 - st.y));
    }
    return ink * u.params.y;
}

// ---------------------------------------------------------------- faces, normal viewing

struct FaceOut {
    @builtin(position) clip: vec4<f32>,
    // Scene position relative to the eye (small numbers: precise derivatives close up).
    @location(0) rel: vec3<f32>,
    @location(1) @interpolate(flat) slot: u32,
};

@vertex
fn vs_face(v: FaceIn, i: Inst) -> FaceOut {
    var o: FaceOut;
    let pos = place(i, v.pos);
    o.clip = u.view_proj * vec4<f32>(pos, 1.0);
    o.clip.z += u.layer.x * o.clip.w;
    o.slot = slot_of(v, i);
    o.rel = pos - u.eye.xyz;
    return o;
}

// Fill and hatch ink shaded alike (a darkened fill was computed from the unshaded fill on the
// CPU).
fn shaded(pixel: vec2<f32>, slot: u32, shade: f32) -> vec4<f32> {
    let c = mix(u.table[slot].fill.rgb, u.table[slot].ink.rgb, hatch(pixel, slot)) * shade;
    // The target is a plain Unorm texture holding gamma-encoded colour.
    return vec4<f32>(linear_to_srgb(c), 1.0);
}

@fragment
fn fs_face(i: FaceOut) -> @location(0) vec4<f32> {
    return shaded(i.clip.xy, i.slot, face_shade(screen_normal(i.rel)));
}

// ---------------------------------------------------------------- faces, section box

struct FaceClipOut {
    @builtin(position) clip: vec4<f32>,
    // Scene position relative to the eye: the face's normal, and the clipping box.
    @location(0) rel: vec3<f32>,
    @location(1) @interpolate(flat) slot: u32,
};

// True if p is outside the clipping box.
fn clipped(p: vec3<f32>) -> bool {
    return any(p < u.clip_min.xyz) || any(p > u.clip_max.xyz);
}

@vertex
fn vs_face_clip(v: FaceIn, i: Inst) -> FaceClipOut {
    var o: FaceClipOut;
    let pos = place(i, v.pos);
    o.clip = u.view_proj * vec4<f32>(pos, 1.0);
    o.clip.z += u.layer.x * o.clip.w;
    o.slot = slot_of(v, i);
    o.rel = pos - u.eye.xyz;
    return o;
}

// Outward faces inside the box.
@fragment
fn fs_face_clip(i: FaceClipOut) -> @location(0) vec4<f32> {
    let shade = face_shade(screen_normal(i.rel));
    if clipped(i.rel + u.eye.xyz) {
        discard;
    }
    return shaded(i.clip.xy, i.slot, shade);
}

// Inward faces inside the box: where the box removed a solid's front we see its back faces
// (meshes are closed and wound outward). Drawn flat and darker, they show the cut surface,
// hatched like the material.
@fragment
fn fs_face_cut(i: FaceClipOut) -> @location(0) vec4<f32> {
    if clipped(i.rel + u.eye.xyz) {
        discard;
    }
    return shaded(i.clip.xy, i.slot, u.params.w);
}

// ---------------------------------------------------------------- edges

struct LineOut {
    @builtin(position) clip: vec4<f32>,
};

@vertex
fn vs_line(@location(0) local: vec3<f32>, i: Inst) -> LineOut {
    var o: LineOut;
    o.clip = u.view_proj * vec4<f32>(place(i, local), 1.0);
    o.clip.z += u.layer.x * o.clip.w;
    return o;
}

@fragment
fn fs_line(i: LineOut) -> @location(0) vec4<f32> {
    // Already sRGB-encoded, like the target.
    return vec4<f32>(u.edge.rgb, 1.0);
}

struct LineClipOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) pos: vec3<f32>,
    // 1 if both faces of the edge face away from the camera
    @location(1) @interpolate(flat) back: u32,
};

@vertex
fn vs_line_clip(
    @location(0) local: vec3<f32>,
    @location(1) n1l: vec4<f32>,
    @location(2) n2l: vec4<f32>,
    i: Inst,
) -> LineClipOut {
    var o: LineClipOut;
    let pos = place(i, local);
    let n1 = vec4<f32>(turn(i, n1l.xyz), 0.0);
    let n2 = vec4<f32>(turn(i, n2l.xyz), 0.0);
    o.clip = u.view_proj * vec4<f32>(pos, 1.0);
    o.clip.z += u.layer.x * o.clip.w;
    o.pos = pos;
    // Towards the viewer from this point.
    let v = select(u.eye.xyz - pos, -u.view_dir.xyz, u.eye.w != 0.0);
    o.back = select(0u, 1u, dot(n1.xyz, v) < 0.0 && dot(n2.xyz, v) < 0.0);
    return o;
}

// Section box without edge normals (fallback): clip, but back edges can't be told apart.
@vertex
fn vs_line_clip_lean(@location(0) local: vec3<f32>, i: Inst) -> LineClipOut {
    var o: LineClipOut;
    let pos = place(i, local);
    o.clip = u.view_proj * vec4<f32>(pos, 1.0);
    o.clip.z += u.layer.x * o.clip.w;
    o.pos = pos;
    o.back = 0u;
    return o;
}

@fragment
fn fs_line_clip(i: LineClipOut) -> @location(0) vec4<f32> {
    // An edge behind its own solid is only visible through a section cut: inside the cut
    // surface, where it would make the solid look hollow.
    if clipped(i.pos) || i.back != 0u {
        discard;
    }
    return vec4<f32>(u.edge.rgb, 1.0);
}
