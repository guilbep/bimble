// Culling on the GPU (ADR 0036 #10): each shape copy is tested against the view (frustum planes,
// smallest size) and the copies kept are packed, per shape draw, into the region the draw reads;
// the draw's instance count is counted here and read by an indirect draw.

struct Cull {
    // Planes as n·p + d >= 0 (layer coordinates).
    planes: array<vec4<f32>, 6>,
    // x: smallest radius drawn, y: copies.
    params: vec4<f32>,
};

// A copy: 13 words (columns x, y, z, t of its placement and its style slot).
const WORDS: u32 = 13u;

@group(0) @binding(0) var<uniform> cull: Cull;
@group(0) @binding(1) var<storage, read> src: array<u32>;
// Per copy: its bounding sphere, and (as u32 bits) its shape draw and that draw's first copy.
struct Copy {
    sphere: vec4<f32>,
    draw: vec4<u32>,
};
@group(0) @binding(2) var<storage, read> copies: array<Copy>;
@group(0) @binding(3) var<storage, read_write> dst: array<u32>;
// Per shape draw, 9 words: index count, instance count, first index, base vertex, first instance
// (faces); vertex count, instance count, first vertex, first instance (edges).
@group(0) @binding(4) var<storage, read_write> args: array<atomic<u32>>;

@compute @workgroup_size(64)
fn cull_copies(@builtin(global_invocation_id) id: vec3<u32>) {
    let i = id.x;
    if i >= u32(cull.params.y) {
        return;
    }
    let c = copies[i];
    let s = c.sphere;
    if s.w < cull.params.x {
        return;
    }
    for (var k = 0u; k < 6u; k++) {
        let p = cull.planes[k];
        if dot(p.xyz, s.xyz) + p.w < -s.w {
            return;
        }
    }
    let d = c.draw.x;
    let n = atomicAdd(&args[d * 9u + 1u], 1u);
    atomicAdd(&args[d * 9u + 6u], 1u);
    let to = (c.draw.y + n) * WORDS;
    for (var w = 0u; w < WORDS; w++) {
        dst[to + w] = src[i * WORDS + w];
    }
}
