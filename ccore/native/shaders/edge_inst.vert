#version 450
#include "common.glsl"
struct Placement { vec4 x; vec4 y; vec4 z; vec4 t; };
layout(std430, set = 0, binding = 1) readonly buffer Placements { Placement placement[]; };
layout(std430, set = 0, binding = 2) readonly buffer Visible { uint visible[]; };
layout(location = 0) in vec3 pos;
layout(location = 0) out vec3 v_pos;
layout(location = 1) flat out vec3 v_col;
layout(location = 2) flat out uint v_id;
void main() {
    Placement p = placement[visible[gl_InstanceIndex]];
    vec3 w = p.x.xyz * pos.x + p.y.xyz * pos.y + p.z.xyz * pos.z + p.t.xyz;
    v_pos = w;
    v_col = vec3(-1.0);
    v_id = floatBitsToUint(p.t.w) + 1u;
    gl_Position = vp * vec4(w, 1.0);
    gl_Position.z *= 1.0 + bias.x;
    if (hidden_elem(floatBitsToUint(p.t.w))) gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
}
