#version 450
// The lab's look (glviewport.ts FS): flat faces from the screen derivatives of the position,
// two-sided Lambert, ambient 0.62, sRGB in and out; edges unlit in the edge colour.
layout(set = 3, binding = 0) uniform Light { vec4 light; vec4 edge; vec4 bmin; vec4 bmax; }; // light.w: ambient; NAT2: the section box (bmin.w 1: on)
layout(location = 0) in vec3 v_pos;
layout(location = 1) flat in vec3 v_col;
layout(location = 2) flat in uint v_id;
layout(location = 0) out vec4 o;
#ifdef WITH_ID
layout(location = 1) out uint o_id;
#endif
vec3 to_linear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c)); }
vec3 to_srgb(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c)); }
void main() {
    if (bmin.w > 0.5 && (any(lessThan(v_pos, bmin.xyz)) || any(greaterThan(v_pos, bmax.xyz)))) discard; // NAT2
#ifdef WITH_ID
    o_id = v_id;
#endif
    if (v_col.x < 0.0) { o = vec4(edge.rgb, 1.0); return; }
    vec3 n = cross(dFdx(v_pos), dFdy(v_pos));
    float len2 = dot(n, n);
    float lambert = len2 > 1e-30 ? abs(dot(n, light.xyz)) * inversesqrt(len2) : 1.0;
    float k = light.w + (1.0 - light.w) * lambert;
    o = vec4(to_srgb(to_linear(v_col) * k), 1.0);
}
