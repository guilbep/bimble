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
    table: array<Fill, 16>,
};

@group(0) @binding(0) var<uniform> u: Uniforms;

struct FaceIn {
    @location(0) pos: vec3<f32>,
    @location(1) normal: vec3<f32>,
    // a: style table slot / 255 (rgb unused)
    @location(2) color: vec4<f32>,
};

struct FaceOut {
    @builtin(position) clip: vec4<f32>,
    // Faces are flat (one normal per triangle), so colours are constant per triangle: no
    // interpolation needed, which saves work per pixel on software and old GPUs.
    // Unlit fill and ink; `shade` is the light of the face. Cut surfaces use their own light.
    @location(0) @interpolate(flat) color: vec3<f32>,
    @location(1) @interpolate(flat) ink: vec3<f32>,
    @location(2) @interpolate(flat) slot: u32,
    @location(3) @interpolate(flat) shade: f32,
    // Scene position, for the clipping box.
    @location(4) pos: vec3<f32>,
};

// True if p is outside the clipping box (when it is on).
fn clipped(p: vec3<f32>) -> bool {
    return u.clip_min.w != 0.0 && (any(p < u.clip_min.xyz) || any(p > u.clip_max.xyz));
}

@vertex
fn vs_face(v: FaceIn) -> FaceOut {
    var o: FaceOut;
    o.clip = u.view_proj * vec4<f32>(v.pos, 1.0);
    o.slot = min(u32(round(v.color.a * 255.0)), 15u);
    let l = normalize(u.light.xyz);
    // Two-sided Lambert with a high ambient: reads like a shaded drawing.
    let ambient = u.params.z;
    o.shade = ambient + (1.0 - ambient) * abs(dot(normalize(v.normal), l));
    let f = u.table[o.slot];
    o.color = f.fill.rgb;
    o.ink = f.ink.rgb;
    o.pos = v.pos;
    return o;
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

@fragment
fn fs_face(i: FaceOut, @builtin(front_facing) front: bool) -> @location(0) vec4<f32> {
    if clipped(i.pos) {
        discard;
    }
    // Inside a cut solid we see its back faces (meshes are closed and wound outward): drawing
    // them flat and darker shows the cut surface, hatched like the material.
    let cut = u.clip_min.w != 0.0 && !front;
    let shade = select(i.shade, u.params.w, cut);
    var c = i.color * shade;
    let bits = u32(u.table[i.slot].fill.a);
    // Families without hatch (and hatching off) skip the pattern code entirely; otherwise only
    // the patterns the family has are evaluated.
    if u.params.y > 0.0 && bits != 0u {
        let p = i.clip.xy / max(u.params.x, 1.0);
        var ink = 0.0;
        if (bits & 1u) != 0u {
            ink = family(p, u.table[i.slot].lines[0], u.table[i.slot].dashes[0]);
        }
        if (bits & 2u) != 0u {
            ink = max(ink, family(p, u.table[i.slot].lines[1], u.table[i.slot].dashes[1]));
        }
        if (bits & 4u) != 0u {
            let st = u.table[i.slot].stipple;
            let cell = floor(p * st.x);
            let h = fract(sin(dot(cell, vec2<f32>(12.9898, 78.233))) * 43758.5453);
            ink = max(ink, select(0.0, st.z, h > 1.0 - st.y));
        }
        c = mix(c, i.ink * shade, ink * u.params.y);
    }
    // The target is a plain Unorm texture holding gamma-encoded colour.
    return vec4<f32>(linear_to_srgb(c), 1.0);
}

struct LineOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) pos: vec3<f32>,
    // 1 if both faces of the edge face away from the camera
    @location(1) @interpolate(flat) back: u32,
};

@vertex
fn vs_line(
    @location(0) pos: vec3<f32>,
    @location(1) n1: vec4<f32>,
    @location(2) n2: vec4<f32>,
) -> LineOut {
    var o: LineOut;
    o.clip = u.view_proj * vec4<f32>(pos, 1.0);
    o.pos = pos;
    // Towards the viewer from this point.
    let v = select(u.eye.xyz - pos, -u.view_dir.xyz, u.eye.w != 0.0);
    o.back = select(0u, 1u, dot(n1.xyz, v) < 0.0 && dot(n2.xyz, v) < 0.0);
    return o;
}

@fragment
fn fs_line(i: LineOut) -> @location(0) vec4<f32> {
    if clipped(i.pos) {
        discard;
    }
    // An edge behind its own solid is only visible through a section cut: inside the cut
    // surface, where it would make the solid look hollow.
    if u.clip_min.w != 0.0 && i.back != 0u {
        discard;
    }
    // Already sRGB-encoded, like the target.
    return vec4<f32>(u.edge.rgb, 1.0);
}
