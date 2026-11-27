#version 450
// Faces of a shared shape, one instance per copy: visible[gl_InstanceIndex] names the copy (the GPU
// cull writes that list; without it the list is 0..n-1), placements[copy] its Placement32 and element.
#include "common.glsl"
struct Placement { vec4 x; vec4 y; vec4 z; vec4 t; }; // t.w: the element index (uint bits)
layout(std430, set = 0, binding = 1) readonly buffer Placements { Placement placement[]; };
layout(std430, set = 0, binding = 2) readonly buffer Visible { uint visible[]; };
layout(location = 0) in vec3 pos;
layout(location = 0) out vec3 v_pos;
layout(location = 1) flat out vec3 v_col;
layout(location = 2) flat out uint v_id;
void main() {
    Placement p = placement[visible[gl_InstanceIndex]];
    vec3 w = p.x.xyz * pos.x + p.y.xyz * pos.y + p.z.xyz * pos.z + p.t.xyz;
    uint elem = floatBitsToUint(p.t.w);
    v_pos = w;
    v_col = unpack_srgb(colour[elem]);
    v_id = elem + 1u;
    gl_Position = vp * vec4(w, 1.0);
    if (hidden_elem(elem)) gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
}
