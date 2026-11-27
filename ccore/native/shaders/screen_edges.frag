#version 450
// Screen-space edges from the id and depth buffers in one fullscreen pass (what the web lab cannot
// read back cheaply): a pixel is an edge where its element differs from a neighbour's, or where the
// depth bends (a crease inside one element: the second difference of 1/depth against its size).
layout(set = 2, binding = 0) uniform usampler2D ids;
layout(set = 2, binding = 1) uniform sampler2D depth;
layout(set = 3, binding = 0) uniform Edge { vec4 colour; vec4 k; }; // k.x: crease threshold
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    uint c = texelFetch(ids, p, 0).r;
    uint r = texelFetch(ids, p + ivec2(1, 0), 0).r, d = texelFetch(ids, p + ivec2(0, 1), 0).r;
    bool edge = c != r || c != d;
    if (!edge && c != 0u) {
        // reverse Z: depth is near/distance, linear in screen space on a plane
        float z = texelFetch(depth, p, 0).r;
        float zl = texelFetch(depth, p - ivec2(1, 0), 0).r, zr = texelFetch(depth, p + ivec2(1, 0), 0).r;
        float zu = texelFetch(depth, p - ivec2(0, 1), 0).r, zd = texelFetch(depth, p + ivec2(0, 1), 0).r;
        float bend = max(abs(zl + zr - 2.0 * z), abs(zu + zd - 2.0 * z));
        edge = bend > k.x * z;
    }
    if (!edge) discard;
    o = vec4(colour.rgb, 1.0);
}
