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
// The depth, read as a plain float texture through a nearest sampler: GLSL (GL, WebGL2) cannot
// `textureLoad` a depth texture, and GL ES leaves a depth texture without nearest filtering
// incomplete (issue #59).
@group(0) @binding(1) var depth: texture_2d<f32>;
@group(0) @binding(2) var nearest: sampler;

@vertex
fn vs_full(@builtin(vertex_index) i: u32) -> @builtin(position) vec4<f32> {
    // One triangle over the whole screen.
    let x = f32((i << 1u) & 2u);
    let y = f32(i & 2u);
    return vec4<f32>(x * 2.0 - 1.0, 1.0 - y * 2.0, 0.0, 1.0);
}

fn ndc_of(q: vec2<i32>) -> vec2<f32> {
    return vec2<f32>(
        (f32(q.x) + 0.5) / e.params.x * 2.0 - 1.0,
        1.0 - (f32(q.y) + 0.5) / e.params.y * 2.0,
    );
}

fn unproject(q: vec2<i32>, d: f32) -> vec3<f32> {
    let w = e.inv_view_proj * vec4<f32>(ndc_of(q), d, 1.0);
    return w.xyz / w.w;
}

fn depth_at(q: vec2<i32>) -> f32 {
    return textureSampleLevel(depth, nearest, (vec2<f32>(q) + 0.5) / e.params.xy, 0.0).x;
}

fn clamped(p: vec2<i32>) -> vec2<i32> {
    return clamp(p, vec2<i32>(0), vec2<i32>(e.params.xy) - 1);
}

fn at(p: vec2<i32>) -> vec4<f32> {
    let q = clamped(p);
    let d = depth_at(q);
    // w = 0 marks the background (nothing drawn there).
    return vec4<f32>(unproject(q, d), select(1.0, 0.0, d >= 1.0));
}

// How far apart in the scene two depths a few float steps apart are at `p`: below this, a
// bend is depth rounding, not the surface. Close up, the near plane is far/10⁴ and depth far
// off is coarse: without it, flat faces got dotted and dashed edges.
fn rounding(p: vec2<i32>) -> f32 {
    let q = clamped(p);
    let d = min(depth_at(q), 0.99999);
    // A float near 1 has steps of 6e-8; twice that (measured: no false edges left, 86–99 % of
    // the real ones kept close up; 16× lost up to 60 %).
    let dd = max(d * 1.2e-7, 1e-7) * 2.0;
    return length(unproject(q, min(d + dd, 1.0)) - unproject(q, d));
}

// Whether the surface jumps or bends across `p` along `step`.
fn breaks(p: vec2<i32>, step: vec2<i32>) -> bool {
    // At the picture's border a neighbour is missing (clamping repeats the pixel, a false bend).
    let size = vec2<i32>(e.params.xy);
    if any(p - step < vec2<i32>(0)) || any(p + step >= size) {
        return false;
    }
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
    return bend > e.params.z * steps + rounding(p);
}

@fragment
fn fs_edges(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
    let p = vec2<i32>(pos.xy);
    if !(breaks(p, vec2<i32>(1, 0)) || breaks(p, vec2<i32>(0, 1))) {
        discard;
    }
    return vec4<f32>(e.edge.rgb, 1.0);
}
