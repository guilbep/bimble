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
    @location(1) normal: vec3<f32>,
    // a: style table slot / 255 (rgb unused)
    @location(2) color: vec4<f32>,
};

// Two variants of each shader: a lean one for normal viewing, and one for the section box
// (scene position passed to pixels, cut surfaces, back edges). Extra per-vertex outputs cost
// memory and time on software and old GPUs, so normal viewing must not pay for the box.

// Two-sided Lambert with a high ambient: reads like a shaded drawing.
fn face_shade(normal: vec3<f32>) -> f32 {
    let l = normalize(u.light.xyz);
    let ambient = u.params.z;
    return ambient + (1.0 - ambient) * abs(dot(normalize(normal), l));
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
    // Faces are flat (one normal per triangle), so colours are constant per triangle: no
    // interpolation needed, which saves work per pixel on software and old GPUs.
    @location(0) @interpolate(flat) color: vec3<f32>,
    @location(1) @interpolate(flat) ink: vec3<f32>,
    @location(2) @interpolate(flat) slot: u32,
};

@vertex
fn vs_face(v: FaceIn) -> FaceOut {
    var o: FaceOut;
    o.clip = u.view_proj * vec4<f32>(v.pos, 1.0);
    o.clip.z += u.layer.x * o.clip.w;
    o.slot = face_slot(v.color);
    let shade = face_shade(v.normal);
    o.color = u.table[o.slot].fill.rgb * shade;
    // A darkened fill was computed from the unshaded fill on the CPU: shade both alike.
    o.ink = u.table[o.slot].ink.rgb * shade;
    return o;
}

@fragment
fn fs_face(i: FaceOut) -> @location(0) vec4<f32> {
    let c = mix(i.color, i.ink, hatch(i.clip.xy, i.slot));
    // The target is a plain Unorm texture holding gamma-encoded colour.
    return vec4<f32>(linear_to_srgb(c), 1.0);
}

// ---------------------------------------------------------------- faces, section box

struct FaceClipOut {
    @builtin(position) clip: vec4<f32>,
    // Unlit fill and ink, and the light of the face: cut surfaces use their own light.
    @location(0) @interpolate(flat) color: vec3<f32>,
    @location(1) @interpolate(flat) ink: vec3<f32>,
    @location(2) @interpolate(flat) slot: u32,
    @location(3) @interpolate(flat) shade: f32,
    // Scene position, for the clipping box.
    @location(4) pos: vec3<f32>,
};

// True if p is outside the clipping box.
fn clipped(p: vec3<f32>) -> bool {
    return any(p < u.clip_min.xyz) || any(p > u.clip_max.xyz);
}

@vertex
fn vs_face_clip(v: FaceIn) -> FaceClipOut {
    var o: FaceClipOut;
    o.clip = u.view_proj * vec4<f32>(v.pos, 1.0);
    o.clip.z += u.layer.x * o.clip.w;
    o.slot = face_slot(v.color);
    o.shade = face_shade(v.normal);
    o.color = u.table[o.slot].fill.rgb;
    o.ink = u.table[o.slot].ink.rgb;
    o.pos = v.pos;
    return o;
}

fn shade_clipped(i: FaceClipOut, shade: f32) -> vec4<f32> {
    let c = mix(i.color * shade, i.ink * shade, hatch(i.clip.xy, i.slot));
    return vec4<f32>(linear_to_srgb(c), 1.0);
}

// Outward faces inside the box.
@fragment
fn fs_face_clip(i: FaceClipOut) -> @location(0) vec4<f32> {
    if clipped(i.pos) {
        discard;
    }
    return shade_clipped(i, i.shade);
}

// Inward faces inside the box: where the box removed a solid's front we see its back faces
// (meshes are closed and wound outward). Drawn flat and darker, they show the cut surface,
// hatched like the material.
@fragment
fn fs_face_cut(i: FaceClipOut) -> @location(0) vec4<f32> {
    if clipped(i.pos) {
        discard;
    }
    return shade_clipped(i, u.params.w);
}

// ---------------------------------------------------------------- edges

struct LineOut {
    @builtin(position) clip: vec4<f32>,
};

@vertex
fn vs_line(@location(0) pos: vec3<f32>) -> LineOut {
    var o: LineOut;
    o.clip = u.view_proj * vec4<f32>(pos, 1.0);
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
    @location(0) pos: vec3<f32>,
    @location(1) n1: vec4<f32>,
    @location(2) n2: vec4<f32>,
) -> LineClipOut {
    var o: LineClipOut;
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
fn vs_line_clip_lean(@location(0) pos: vec3<f32>) -> LineClipOut {
    var o: LineClipOut;
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
