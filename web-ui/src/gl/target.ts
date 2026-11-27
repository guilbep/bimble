// The 3D view's offscreen target (ticket VIS1, ADR 0078 amendment 5). The frame's faces are drawn into three colour
// attachments and a depth texture: the shaded colour, the element (its index + 1 in 24 bits, the model + 1 in alpha;
// 0: background; 0xffffff: a face of no element, a proxy), the face's normal; then one full-screen pass presents the
// colour on the canvas and draws the edges found on screen: where the element changes between neighbouring pixels
// (the nearer one's side), where an element's surface bends by more than the core's crease angle (20 degrees,
// scene_build.h) and where it meets the background. It replaces the line pass of every element's edges, which took
// 55-65 % of a whole frame's GPU time on the M1 Pro through ANGLE (drive.mjs throughput), and a coplanar joint between
// two elements (facade panels) stays an edge, which a depth-only test (Rust's edges.wgsl) loses. The kept picture of
// the fill's slices lives here (the canvas is a copy); the element under a pixel is one readPixels of the second
// attachment (the pick); the depth texture is the occlusion test's (hiz.ts).

const VS = `#version 300 es
void main() { vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2)); gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }`;
const FS = `#version 300 es
precision highp float;
precision highp int;
uniform highp sampler2D u_col;
uniform highp sampler2D u_id;
uniform highp sampler2D u_nrm;
uniform highp sampler2D u_dep;
uniform int u_edges;
uniform vec3 u_e0; // the open model's edge colour
uniform vec3 u_e1; // a link's
uniform float u_cos; // the crease angle's cosine
out vec4 o;
uvec4 idAt(ivec2 q) { return uvec4(texelFetch(u_id, q, 0) * 255.0 + 0.5); }
void main() {
  ivec2 q = ivec2(gl_FragCoord.xy), hi = textureSize(u_col, 0) - 1;
  vec4 c = texelFetch(u_col, q, 0);
  o = c;
  if (u_edges == 0) return;
  uvec4 ic = idAt(q);
  if (ic.a == 0u) return; // background
  float dc = texelFetch(u_dep, q, 0).r;
  vec3 nc = texelFetch(u_nrm, q, 0).xyz * 2.0 - 1.0;
  bool edge = false;
  for (int k = 0; k < 4; k++) {
    ivec2 p = clamp(q + (k == 0 ? ivec2(-1, 0) : k == 1 ? ivec2(1, 0) : k == 2 ? ivec2(0, -1) : ivec2(0, 1)), ivec2(0), hi);
    uvec4 ip = idAt(p);
    if (ip != ic) {
      float dp = texelFetch(u_dep, p, 0).r;
      // the nearer element draws the line (a tie: the larger id): one pixel wide
      if (dc < dp || (dc == dp && (ic.r + 256u * ic.g + 65536u * ic.b) > (ip.r + 256u * ip.g + 65536u * ip.b))) { edge = true; break; }
    } else {
      vec3 np = texelFetch(u_nrm, p, 0).xyz * 2.0 - 1.0;
      if (dot(nc, np) < u_cos * length(nc) * length(np) && (k == 0 || k == 2)) { edge = true; break; } // a crease: one side (left, down)
    }
  }
  if (edge) o = vec4(ic.a > 1u ? u_e1 : u_e0, 1.0);
}`;

export interface SceneTarget {
  /** Binds the target (made or resized to w x h) for the frame's 3D drawing: all three colour attachments. */
  bind(w: number, h: number): void;
  /** The colour attachment alone (edges, outlines, the box, the ghost: no element, no normal), or all three again. */
  colourOnly(on: boolean): void;
  /** Clears colour (the background, sRGB 0-1), element, normal and depth. */
  clear(bg: readonly number[]): void;
  /** The picture onto the canvas (the default framebuffer, left bound), its edges found on screen when `edges`. */
  present(edges: boolean, e0: readonly number[], e1: readonly number[]): void;
  /** The element (index, model) under device pixel (x, y) from the bottom left, or null (background, a proxy). */
  elementAt(x: number, y: number): { el: number; model: number } | null;
  /** Whether the last elementAt's pixel was the background (nothing drawn there). */
  isBackground(x: number, y: number): boolean;
  readonly depth: WebGLTexture | null;
  readonly ids: WebGLTexture | null;
  readonly fb: WebGLFramebuffer | null;
  readonly w: number;
  readonly h: number;
}

const COS20 = Math.cos((20 * Math.PI) / 180);

export function sceneTarget(gl: WebGL2RenderingContext): SceneTarget {
  const sh = (t: number, src: string) => { const s = gl.createShader(t)!; gl.shaderSource(s, src); gl.compileShader(s); if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s) ?? "shader"); return s; };
  const prog = gl.createProgram()!;
  gl.attachShader(prog, sh(gl.VERTEX_SHADER, VS)), gl.attachShader(prog, sh(gl.FRAGMENT_SHADER, FS));
  gl.linkProgram(prog);
  if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(prog) ?? "link");
  const U = (n: string) => gl.getUniformLocation(prog, n);
  const uCol = U("u_col"), uId = U("u_id"), uNrm = U("u_nrm"), uDep = U("u_dep"), uEdges = U("u_edges"), uE0 = U("u_e0"), uE1 = U("u_e1"), uCos = U("u_cos");
  const vao = gl.createVertexArray();
  let fb: WebGLFramebuffer | null = null, col: WebGLTexture | null = null, id: WebGLTexture | null = null, nrm: WebGLTexture | null = null, dep: WebGLTexture | null = null, w = 0, h = 0;
  const UNIT = 8; // texture units 8-11 (the viewport uses 0-3)
  const tex = (ifmt: number, fmt: number, type: number) => {
    const t = gl.createTexture()!;
    gl.bindTexture(gl.TEXTURE_2D, t);
    gl.texImage2D(gl.TEXTURE_2D, 0, ifmt, w, h, 0, fmt, type, null);
    for (const p of [gl.TEXTURE_MIN_FILTER, gl.TEXTURE_MAG_FILTER]) gl.texParameteri(gl.TEXTURE_2D, p, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE), gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    return t;
  };
  const ALL = [gl.COLOR_ATTACHMENT0, gl.COLOR_ATTACHMENT1, gl.COLOR_ATTACHMENT2];
  const make = () => {
    for (const t of [col, id, nrm, dep]) if (t) gl.deleteTexture(t);
    if (fb) gl.deleteFramebuffer(fb);
    gl.activeTexture(gl.TEXTURE0 + UNIT);
    col = tex(gl.RGBA8, gl.RGBA, gl.UNSIGNED_BYTE), id = tex(gl.RGBA8, gl.RGBA, gl.UNSIGNED_BYTE), nrm = tex(gl.RGBA8, gl.RGBA, gl.UNSIGNED_BYTE);
    dep = tex(gl.DEPTH_COMPONENT24, gl.DEPTH_COMPONENT, gl.UNSIGNED_INT); // (no stencil: none is used)
    gl.activeTexture(gl.TEXTURE0);
    fb = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, fb);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, col, 0);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT1, gl.TEXTURE_2D, id, 0);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT2, gl.TEXTURE_2D, nrm, 0);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.DEPTH_ATTACHMENT, gl.TEXTURE_2D, dep, 0);
    gl.drawBuffers(ALL);
  };
  const px = new Uint8Array(4);
  return {
    get depth() { return dep; }, get ids() { return id; }, get fb() { return fb; }, get w() { return w; }, get h() { return h; },
    bind(cw, ch) {
      if (cw !== w || ch !== h || !fb) w = Math.max(1, cw), h = Math.max(1, ch), make();
      gl.bindFramebuffer(gl.FRAMEBUFFER, fb);
      gl.drawBuffers(ALL);
    },
    colourOnly(on) { gl.drawBuffers(on ? [gl.COLOR_ATTACHMENT0, gl.NONE, gl.NONE] : ALL); },
    clear(bg) {
      gl.drawBuffers(ALL);
      gl.clearBufferfv(gl.COLOR, 0, [bg[0]!, bg[1]!, bg[2]!, 1]);
      gl.clearBufferfv(gl.COLOR, 1, [0, 0, 0, 0]);
      gl.clearBufferfv(gl.COLOR, 2, [0.5, 0.5, 0.5, 0]);
      gl.depthMask(true);
      gl.clearBufferfv(gl.DEPTH, 0, [1]);
    },
    present(edges, e0, e1) {
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, w, h);
      gl.useProgram(prog);
      const bindT = (k: number, t: WebGLTexture | null, u: WebGLUniformLocation | null) => { gl.activeTexture(gl.TEXTURE0 + UNIT + k); gl.bindTexture(gl.TEXTURE_2D, t); gl.uniform1i(u, UNIT + k); };
      bindT(0, col, uCol), bindT(1, id, uId), bindT(2, nrm, uNrm), bindT(3, dep, uDep);
      gl.activeTexture(gl.TEXTURE0);
      gl.uniform1i(uEdges, edges ? 1 : 0);
      gl.uniform3f(uE0, e0[0]!, e0[1]!, e0[2]!), gl.uniform3f(uE1, e1[0]!, e1[1]!, e1[2]!);
      gl.uniform1f(uCos, COS20);
      gl.disable(gl.DEPTH_TEST), gl.disable(gl.BLEND), gl.disable(gl.CULL_FACE), gl.disable(gl.POLYGON_OFFSET_FILL);
      gl.depthMask(false);
      gl.bindVertexArray(vao);
      gl.drawArrays(gl.TRIANGLES, 0, 3);
      gl.bindVertexArray(null);
      gl.enable(gl.DEPTH_TEST);
      gl.depthMask(true);
      for (let k = 0; k < 4; k++) { gl.activeTexture(gl.TEXTURE0 + UNIT + k); gl.bindTexture(gl.TEXTURE_2D, null); } // (a texture bound while attached is a feedback loop)
      gl.activeTexture(gl.TEXTURE0);
    },
    elementAt(x, y) {
      if (!fb || x < 0 || y < 0 || x >= w || y >= h) return null;
      gl.bindFramebuffer(gl.READ_FRAMEBUFFER, fb);
      gl.readBuffer(gl.COLOR_ATTACHMENT1);
      gl.readPixels(Math.floor(x), Math.floor(y), 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
      gl.readBuffer(gl.COLOR_ATTACHMENT0);
      gl.bindFramebuffer(gl.READ_FRAMEBUFFER, null);
      const k = px[0]! | (px[1]! << 8) | (px[2]! << 16);
      if (!px[3] || !k || k === 0xffffff) return null;
      return { el: k - 1, model: px[3]! - 1 };
    },
    isBackground(x, y) { return x >= 0 && y >= 0 && x < w && y < h && px[3] === 0; },
  };
}

/** The inverse of a column-major 4x4 matrix (null when singular). */
export function invert4(m: ArrayLike<number>): Float32Array | null {
  const a = Array.from(m), inv = new Float64Array(16);
  inv[0] = a[5]! * a[10]! * a[15]! - a[5]! * a[11]! * a[14]! - a[9]! * a[6]! * a[15]! + a[9]! * a[7]! * a[14]! + a[13]! * a[6]! * a[11]! - a[13]! * a[7]! * a[10]!;
  inv[4] = -a[4]! * a[10]! * a[15]! + a[4]! * a[11]! * a[14]! + a[8]! * a[6]! * a[15]! - a[8]! * a[7]! * a[14]! - a[12]! * a[6]! * a[11]! + a[12]! * a[7]! * a[10]!;
  inv[8] = a[4]! * a[9]! * a[15]! - a[4]! * a[11]! * a[13]! - a[8]! * a[5]! * a[15]! + a[8]! * a[7]! * a[13]! + a[12]! * a[5]! * a[11]! - a[12]! * a[7]! * a[9]!;
  inv[12] = -a[4]! * a[9]! * a[14]! + a[4]! * a[10]! * a[13]! + a[8]! * a[5]! * a[14]! - a[8]! * a[6]! * a[13]! - a[12]! * a[5]! * a[10]! + a[12]! * a[6]! * a[9]!;
  inv[1] = -a[1]! * a[10]! * a[15]! + a[1]! * a[11]! * a[14]! + a[9]! * a[2]! * a[15]! - a[9]! * a[3]! * a[14]! - a[13]! * a[2]! * a[11]! + a[13]! * a[3]! * a[10]!;
  inv[5] = a[0]! * a[10]! * a[15]! - a[0]! * a[11]! * a[14]! - a[8]! * a[2]! * a[15]! + a[8]! * a[3]! * a[14]! + a[12]! * a[2]! * a[11]! - a[12]! * a[3]! * a[10]!;
  inv[9] = -a[0]! * a[9]! * a[15]! + a[0]! * a[11]! * a[13]! + a[8]! * a[1]! * a[15]! - a[8]! * a[3]! * a[13]! - a[12]! * a[1]! * a[11]! + a[12]! * a[3]! * a[9]!;
  inv[13] = a[0]! * a[9]! * a[14]! - a[0]! * a[10]! * a[13]! - a[8]! * a[1]! * a[14]! + a[8]! * a[2]! * a[13]! + a[12]! * a[1]! * a[10]! - a[12]! * a[2]! * a[9]!;
  inv[2] = a[1]! * a[6]! * a[15]! - a[1]! * a[7]! * a[14]! - a[5]! * a[2]! * a[15]! + a[5]! * a[3]! * a[14]! + a[13]! * a[2]! * a[7]! - a[13]! * a[3]! * a[6]!;
  inv[6] = -a[0]! * a[6]! * a[15]! + a[0]! * a[7]! * a[14]! + a[4]! * a[2]! * a[15]! - a[4]! * a[3]! * a[14]! - a[12]! * a[2]! * a[7]! + a[12]! * a[3]! * a[6]!;
  inv[10] = a[0]! * a[5]! * a[15]! - a[0]! * a[7]! * a[13]! - a[4]! * a[1]! * a[15]! + a[4]! * a[3]! * a[13]! + a[12]! * a[1]! * a[7]! - a[12]! * a[3]! * a[5]!;
  inv[14] = -a[0]! * a[5]! * a[14]! + a[0]! * a[6]! * a[13]! + a[4]! * a[1]! * a[14]! - a[4]! * a[2]! * a[13]! - a[12]! * a[1]! * a[6]! + a[12]! * a[2]! * a[5]!;
  inv[3] = -a[1]! * a[6]! * a[11]! + a[1]! * a[7]! * a[10]! + a[5]! * a[2]! * a[11]! - a[5]! * a[3]! * a[10]! - a[9]! * a[2]! * a[7]! + a[9]! * a[3]! * a[6]!;
  inv[7] = a[0]! * a[6]! * a[11]! - a[0]! * a[7]! * a[10]! - a[4]! * a[2]! * a[11]! + a[4]! * a[3]! * a[10]! + a[8]! * a[2]! * a[7]! - a[8]! * a[3]! * a[6]!;
  inv[11] = -a[0]! * a[5]! * a[11]! + a[0]! * a[7]! * a[9]! + a[4]! * a[1]! * a[11]! - a[4]! * a[3]! * a[9]! - a[8]! * a[1]! * a[7]! + a[8]! * a[3]! * a[5]!;
  inv[15] = a[0]! * a[5]! * a[10]! - a[0]! * a[6]! * a[9]! - a[4]! * a[1]! * a[10]! + a[4]! * a[2]! * a[9]! + a[8]! * a[1]! * a[6]! - a[8]! * a[2]! * a[5]!;
  const det = a[0]! * inv[0]! + a[1]! * inv[4]! + a[2]! * inv[8]! + a[3]! * inv[12]!;
  if (!det || !Number.isFinite(det)) return null;
  return Float32Array.from(inv, (v) => v / det);
}
