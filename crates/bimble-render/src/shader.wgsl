// Drawing-style shading: flat faces, a soft directional light, and screen-space hatching per
// material family. No textures: cheap on old integrated GPUs.

// One entry of the style table (see style.rs, GpuFill). Lengths are in pixels.
struct Fill {
    // rgb: fill (sRGB), a: 1 = hatch uses `ink.rgb`, 0 = hatch darkens the fill
    fill: vec4<f32>,
    // rgb: ink (sRGB), a: darkening factor when the fill is darkened
    ink: vec4<f32>,
    // per line family: xy = normal / spacing, z = line width (fraction), w = 1 if present
    lines: array<vec4<f32>, 2>,
    // xy: dashes of family 0 (1 / period, on fraction), zw: same for family 1
    dashes: vec4<f32>,
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
    @location(0) color: vec3<f32>,
    @location(1) @interpolate(flat) slot: u32,
    @location(2) @interpolate(flat) shade: f32,
};

@vertex
fn vs_face(v: FaceIn) -> FaceOut {
    var o: FaceOut;
    o.clip = u.view_proj * vec4<f32>(v.pos, 1.0);
    o.slot = min(u32(round(v.color.a * 255.0)), 15u);
    let l = normalize(u.light.xyz);
    // Two-sided Lambert with a high ambient: reads like a shaded drawing.
    let ambient = u.params.z;
    o.shade = ambient + (1.0 - ambient) * abs(dot(normalize(v.normal), l));
    // Style colours are sRGB: shade in linear space, encode back in the fragment shader.
    o.color = srgb_to_linear(u.table[o.slot].fill.rgb) * o.shade;
    return o;
}

fn srgb_to_linear(c: vec3<f32>) -> vec3<f32> {
    let lo = c / 12.92;
    let hi = pow((c + 0.055) / 1.055, vec3<f32>(2.4));
    return select(hi, lo, c <= vec3<f32>(0.04045));
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
fn family(p: vec2<f32>, l: vec4<f32>, dash: vec2<f32>) -> f32 {
    if l.w == 0.0 {
        return 0.0;
    }
    var ink = stripe(dot(p, l.xy), l.z);
    if dash.x > 0.0 {
        // Along the lines: perpendicular to the normal.
        let along = normalize(vec2<f32>(l.y, -l.x));
        ink = ink * stripe(dot(p, along) * dash.x, dash.y);
    }
    return ink;
}

@fragment
fn fs_face(i: FaceOut) -> @location(0) vec4<f32> {
    let f = u.table[i.slot];
    let p = i.clip.xy / max(u.params.x, 1.0);
    var ink = max(family(p, f.lines[0], f.dashes.xy), family(p, f.lines[1], f.dashes.zw));
    if f.stipple.y > 0.0 {
        let c = floor(p * f.stipple.x);
        let h = fract(sin(dot(c, vec2<f32>(12.9898, 78.233))) * 43758.5453);
        ink = max(ink, select(0.0, f.stipple.z, h > 1.0 - f.stipple.y));
    }
    let target_ink = select(i.color * f.ink.w, srgb_to_linear(f.ink.rgb) * i.shade, f.fill.w > 0.5);
    let c = mix(i.color, target_ink, ink * u.params.y);
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
