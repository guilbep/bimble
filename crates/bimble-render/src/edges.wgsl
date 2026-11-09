// Edges found on screen (ADR 0036 #6): from the depth of a single-sample picture, a pixel is on
// an edge where the surface jumps (a silhouette) or bends (a crease) across it. Drawn instead of
// edge lines in pictures drawn while moving on slow machines.

struct EdgeUniforms {
    // From normalized device coordinates (x, y in -1..1, depth 0..1) to scene coordinates.
    inv_view_proj: mat4x4<f32>,
    // rgb: edge colour (sRGB, as the target stores it).
    edge: vec4<f32>,
    // x: width, y: height (pixels), z: how much a bend must be against the steps (0..1).
    params: vec4<f32>,
};

@group(0) @binding(0) var<uniform> e: EdgeUniforms;
@group(0) @binding(1) var depth: texture_depth_2d;

@vertex
fn vs_full(@builtin(vertex_index) i: u32) -> @builtin(position) vec4<f32> {
    // One triangle over the whole screen.
    let x = f32((i << 1u) & 2u);
    let y = f32(i & 2u);
    return vec4<f32>(x * 2.0 - 1.0, 1.0 - y * 2.0, 0.0, 1.0);
}

fn at(p: vec2<i32>) -> vec4<f32> {
    let size = vec2<i32>(e.params.xy);
    let q = clamp(p, vec2<i32>(0), size - 1);
    let d = textureLoad(depth, q, 0);
    let ndc = vec2<f32>(
        (f32(q.x) + 0.5) / e.params.x * 2.0 - 1.0,
        1.0 - (f32(q.y) + 0.5) / e.params.y * 2.0,
    );
    let w = e.inv_view_proj * vec4<f32>(ndc, d, 1.0);
    // w = 0 marks the background (nothing drawn there).
    return vec4<f32>(w.xyz / w.w, select(1.0, 0.0, d >= 1.0));
}

// Whether the surface jumps or bends across `p` along `step`.
fn breaks(p: vec2<i32>, step: vec2<i32>) -> bool {
    let a = at(p - step);
    let b = at(p);
    let c = at(p + step);
    if b.w == 0.0 {
        return false;
    }
    if a.w == 0.0 || c.w == 0.0 {
        return true;
    }
    let bend = length(a.xyz + c.xyz - 2.0 * b.xyz);
    let steps = length(b.xyz - a.xyz) + length(c.xyz - b.xyz);
    return bend > e.params.z * steps;
}

@fragment
fn fs_edges(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
    let p = vec2<i32>(pos.xy);
    if !(breaks(p, vec2<i32>(1, 0)) || breaks(p, vec2<i32>(0, 1))) {
        discard;
    }
    return vec4<f32>(e.edge.rgb, 1.0);
}
