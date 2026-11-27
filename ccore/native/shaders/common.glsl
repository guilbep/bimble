// Shared by the vertex shaders: the frame's uniforms (SDL_gpu: vertex uniforms in set 1).
layout(set = 1, binding = 0) uniform Frame {
    mat4 vp;      // view-projection, reverse Z (near 1, infinity 0), NDC y up (SDL_gpu)
    vec4 bias;    // x: the edges' pull toward the eye (relative, clip z * (1 + x))
};
// Element colours, one packed sRGB RGBA8 per element (the page's colour table).
layout(std430, set = 0, binding = 0) readonly buffer Colours { uint colour[]; };
vec3 unpack_srgb(uint c) { return vec3(float(c & 255u), float((c >> 8) & 255u), float((c >> 16) & 255u)) / 255.0; }
// NAT2: an element whose colour has alpha 0 is hidden (nv_render_set_state): its vertices go behind
// the near plane, so its triangles and edge segments are clipped away.
bool hidden_elem(uint e) { return (colour[e] >> 24) == 0u; }
