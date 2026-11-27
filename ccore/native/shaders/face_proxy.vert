#version 450
// NUI2: a tile's proxy (TPX1) as stored: u16 positions in the tile's box (link frame), decoded here. Proxies live in
// shared pages (tiles_gpu.c): their positions in one buffer (gl_VertexIndex = the index + the tile's first vertex), and
// per tile one row of a table: its box, the link's placement (turned about Z, moved), a link colour. The instance index
// carries the row and the part's category: row << 3 | category.
#include "common.glsl"
layout(std430, set = 0, binding = 1) readonly buffer Pos { uint pw[]; }; // u16 x 3 per vertex, packed
struct Tile { vec4 lo; vec4 hi; vec4 rot; vec4 off; }; // lo.w: a link colour (RGBA8 as float bits, 0: the category's)
layout(std430, set = 0, binding = 2) readonly buffer Tiles { Tile tile[]; };
layout(location = 0) out vec3 v_pos;
layout(location = 1) flat out vec3 v_col;
layout(location = 2) flat out uint v_id;
float q16(uint i) { uint w = pw[i >> 1]; return float((i & 1u) == 0u ? (w & 0xFFFFu) : (w >> 16)); }
void main() {
    uint inst = uint(gl_InstanceIndex), cat = inst & 7u;
    Tile t = tile[inst >> 3];
    uint b = uint(gl_VertexIndex) * 3u;
    vec3 q = vec3(q16(b), q16(b + 1u), q16(b + 2u));
    vec3 p = t.lo.xyz + q * (t.hi.xyz - t.lo.xyz) / 65535.0;
    vec3 w = vec3(t.rot.x * p.x - t.rot.y * p.y, t.rot.y * p.x + t.rot.x * p.y, p.z) + t.off.xyz;
    uint ov = floatBitsToUint(t.lo.w);
    v_pos = w;
    v_col = unpack_srgb(ov != 0u ? ov : colour[cat]);
    v_id = cat + 1u;
    gl_Position = vp * vec4(w, 1.0);
}
