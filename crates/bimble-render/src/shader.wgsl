// Drawing-style shading: flat faces, a soft directional light, and screen-space hatching per
// material family. No textures: cheap on old integrated GPUs.

struct Uniforms {
    view_proj: mat4x4<f32>,
    // xyz: light direction (world, towards the light)
    light: vec4<f32>,
    // rgb: edge colour, a: unused
    edge: vec4<f32>,
    // x: hatch scale (pixels per hatch unit), y: hatch strength 0..1
    hatch: vec4<f32>,
};

@group(0) @binding(0) var<uniform> u: Uniforms;

struct FaceIn {
    @location(0) pos: vec3<f32>,
    @location(1) normal: vec3<f32>,
    // rgb: base colour, a: hatch pattern id / 255
    @location(2) color: vec4<f32>,
};

struct FaceOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) color: vec3<f32>,
    @location(1) @interpolate(flat) pattern: u32,
};

@vertex
fn vs_face(v: FaceIn) -> FaceOut {
    var o: FaceOut;
    o.clip = u.view_proj * vec4<f32>(v.pos, 1.0);
    let l = normalize(u.light.xyz);
    // Two-sided Lambert with a high ambient: reads like a shaded drawing.
    let shade = 0.62 + 0.38 * abs(dot(normalize(v.normal), l));
    // Palette colours are sRGB; the target is an sRGB texture, so shade in linear space.
    o.color = srgb_to_linear(v.color.rgb) * shade;
    o.pattern = u32(round(v.color.a * 255.0));
    return o;
}

fn srgb_to_linear(c: vec3<f32>) -> vec3<f32> {
    let lo = c / 12.92;
    let hi = pow((c + 0.055) / 1.055, vec3<f32>(2.4));
    return select(hi, lo, c <= vec3<f32>(0.04045));
}

fn stripe(t: f32, width: f32) -> f32 {
    // 1 inside a stripe of relative `width`, 0 elsewhere.
    return select(0.0, 1.0, fract(t) < width);
}

@fragment
fn fs_face(i: FaceOut) -> @location(0) vec4<f32> {
    let p = i.clip.xy / max(u.hatch.x, 1.0);
    var ink = 0.0;
    switch i.pattern {
        // 1 concrete: sparse stipple
        case 1u: {
            let c = floor(p * 1.6);
            let h = fract(sin(dot(c, vec2<f32>(12.9898, 78.233))) * 43758.5453);
            ink = select(0.0, 0.8, h > 0.93);
        }
        // 2 timber: 45° lines
        case 2u: { ink = stripe((p.x + p.y) / 9.0, 0.1); }
        // 3 steel: dense 45° lines the other way
        case 3u: { ink = stripe((p.x - p.y) / 5.0, 0.12); }
        // 4 masonry: 45° lines, wider spacing, both families
        case 4u: { ink = max(stripe((p.x + p.y) / 12.0, 0.08), stripe((p.x - p.y) / 24.0, 0.05)); }
        // 5 insulation: horizontal dashes
        case 5u: { ink = stripe(p.y / 8.0, 0.12) * stripe(p.x / 10.0, 0.6); }
        // 6 earth: cross-hatch
        case 6u: { ink = max(stripe((p.x + p.y) / 10.0, 0.08), stripe((p.x - p.y) / 10.0, 0.08)); }
        default: {}
    }
    let c = mix(i.color, i.color * 0.55, ink * u.hatch.y);
    return vec4<f32>(c, 1.0);
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
    return vec4<f32>(u.edge.rgb, 1.0);
}
