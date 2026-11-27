#version 450
// NUI2: a tile's proxy (TPX1) as stored: u16 positions in the tile's box (link frame), decoded here; the link's
// placement (turned about Z, moved) from the tile's uniforms; the part's category as the instance index (the palette
// slots 0..7 of the colour table), or the link's colour.
#include "common.glsl"
layout(std430, set = 0, binding = 1) readonly buffer Pos { uint pw[]; }; // u16 x 3 per vertex, packed
layout(set = 1, binding = 1) uniform Tile {
    vec4 lo;  // xyz: the box's low corner; w: a link colour (packed RGBA8 as float bits, 0: the category's)
    vec4 hi;  // xyz: the box's high corner
    vec4 rot; // x cos, y sin
    vec4 off; // xyz
};
layout(location = 0) out vec3 v_pos;
layout(location = 1) flat out vec3 v_col;
layout(location = 2) flat out uint v_id;
float q16(uint i) { uint w = pw[i >> 1]; return float((i & 1u) == 0u ? (w & 0xFFFFu) : (w >> 16)); }
void main() {
    uint b = uint(gl_VertexIndex) * 3u;
    vec3 q = vec3(q16(b), q16(b + 1u), q16(b + 2u));
    vec3 p = lo.xyz + q * (hi.xyz - lo.xyz) / 65535.0;
    vec3 w = vec3(rot.x * p.x - rot.y * p.y, rot.y * p.x + rot.x * p.y, p.z) + off.xyz;
    uint cat = uint(gl_InstanceIndex);
    uint ov = floatBitsToUint(lo.w);
    v_pos = w;
    v_col = unpack_srgb(ov != 0u ? ov : colour[cat]);
    v_id = cat + 1u;
    gl_Position = vp * vec4(w, 1.0);
}
