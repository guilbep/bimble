#version 450
// Faces already in scene coordinates (own meshes, and copies placed into the merged buffers).
#include "common.glsl"
layout(location = 0) in vec3 pos;
layout(location = 1) in uint elem;
layout(location = 0) out vec3 v_pos;
layout(location = 1) flat out vec3 v_col;
layout(location = 2) flat out uint v_id;
void main() {
    v_pos = pos;
    v_col = unpack_srgb(colour[elem]);
    v_id = elem + 1u;
    gl_Position = vp * vec4(pos, 1.0);
    if (hidden_elem(elem)) gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
    clip_box(pos);
}
