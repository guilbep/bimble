// Shared by the vertex shaders: the frame's uniforms (SDL_gpu: vertex uniforms in set 1).
layout(set = 1, binding = 0) uniform Frame {
    mat4 vp;      // view-projection, reverse Z (near 1, infinity 0), NDC y up (SDL_gpu)
    vec4 bias;    // x: the edges' pull toward the eye (relative, clip z * (1 + x))
    vec4 cmin;    // NUI2: the section box (scene mm; w 1: on) for the clip-distance variants (-DCLIP)
    vec4 cmax;
};
#ifdef CLIP
// NUI2: the section box as six clip planes in the vertex stage (no `discard` in the fragment stage: a tile-based GPU keeps
// its hidden-surface removal; the research note). The fragment variant is face.frag with NO_DISCARD.
out gl_PerVertex { vec4 gl_Position; float gl_ClipDistance[6]; };
void clip_box(vec3 w) {
    bool on = cmin.w > 0.5;
    gl_ClipDistance[0] = on ? w.x - cmin.x : 1.0;
    gl_ClipDistance[1] = on ? w.y - cmin.y : 1.0;
    gl_ClipDistance[2] = on ? w.z - cmin.z : 1.0;
    gl_ClipDistance[3] = on ? cmax.x - w.x : 1.0;
    gl_ClipDistance[4] = on ? cmax.y - w.y : 1.0;
    gl_ClipDistance[5] = on ? cmax.z - w.z : 1.0;
}
#else
void clip_box(vec3 w) {}
#endif
// Element colours, one packed sRGB RGBA8 per element (the page's colour table).
layout(std430, set = 0, binding = 0) readonly buffer Colours { uint colour[]; };
vec3 unpack_srgb(uint c) { return vec3(float(c & 255u), float((c >> 8) & 255u), float((c >> 16) & 255u)) / 255.0; }
// NAT2: an element whose colour has alpha 0 is hidden (nv_render_set_state): its vertices go behind
// the near plane, so its triangles and edge segments are clipped away.
bool hidden_elem(uint e) { return (colour[e] >> 24) == 0u; }
