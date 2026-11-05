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
    // x: hatch scale (pixels per hatch unit), y: hatch strength 0..1, z: ambient light 0..1
    params: vec4<f32>,
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
    @location(0) @interpolate(flat) color: vec3<f32>,
    @location(1) @interpolate(flat) ink: vec3<f32>,
    @location(2) @interpolate(flat) slot: u32,
};

@vertex
fn vs_face(v: FaceIn) -> FaceOut {
    var o: FaceOut;
    o.clip = u.view_proj * vec4<f32>(v.pos, 1.0);
    o.slot = min(u32(round(v.color.a * 255.0)), 15u);
    let l = normalize(u.light.xyz);
    // Two-sided Lambert with a high ambient: reads like a shaded drawing.
    let ambient = u.params.z;
    let shade = ambient + (1.0 - ambient) * abs(dot(normalize(v.normal), l));
    let f = u.table[o.slot];
    o.color = f.fill.rgb * shade;
    // A darkened fill was computed from the unshaded fill on the CPU: shade both alike.
    o.ink = f.ink.rgb * shade;
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
fn fs_face(i: FaceOut) -> @location(0) vec4<f32> {
    var c = i.color;
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
        c = mix(c, i.ink, ink * u.params.y);
    }
    // The target is a plain Unorm texture holding gamma-encoded colour.
    return vec4<f32>(linear_to_srgb(c), 1.0);
}

struct LineOut {
    @builtin(position) clip: vec4<f32>,
};

@vertex
fn vs_line(@location(0) pos: vec3<f32>) -> LineOut {
    var o: LineOut;
    o.clip = u.view_proj * vec4<f32>(pos, 1.0);
    return o;
}

@fragment
fn fs_line(i: LineOut) -> @location(0) vec4<f32> {
    // Already sRGB-encoded, like the target.
    return vec4<f32>(u.edge.rgb, 1.0);
}
