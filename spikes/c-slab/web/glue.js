// Hand-written browser glue: wasm memory -> WebGL2, in Bimble's drawing style.
// ABI: slab_mesh(x0,y0,x1,y1,t) -> 1|0; pos_ptr/pos_len (vertex count, 3 f32 each);
// idx_ptr/idx_len (u32 index count, 3 per triangle); edge_ptr/edge_len (edge count, 6 f32 each).
'use strict';
(() => {
  const AMBIENT = 0.62;
  const FACE_SRGB = [214, 214, 210];
  const EDGE_SRGB = [26, 26, 28];
  const BG_SRGB = [247, 247, 245];

  const canvas = document.getElementById('view');
  const gl = canvas.getContext('webgl2', { antialias: false, preserveDrawingBuffer: true });
  if (!gl) throw new Error('WebGL2 is not available');

  const toLinear = (c) => { c /= 255; return c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4); };

  const VS = `#version 300 es
in vec3 a_pos;
uniform mat4 u_mvp;
out vec3 v_pos;
void main() { v_pos = a_pos; gl_Position = u_mvp * vec4(a_pos, 1.0); }`;
  const FACE_FS = `#version 300 es
precision highp float;
in vec3 v_pos;
uniform vec3 u_light;
uniform vec3 u_color;   // linear
uniform float u_ambient;
out vec4 o;
vec3 to_srgb(vec3 c) {
  return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}
void main() {
  vec3 n = normalize(cross(dFdx(v_pos), dFdy(v_pos)));
  float k = u_ambient + (1.0 - u_ambient) * abs(dot(n, u_light));
  o = vec4(to_srgb(u_color * k), 1.0);
}`;
  const EDGE_FS = `#version 300 es
precision highp float;
uniform vec3 u_color;   // already sRGB
out vec4 o;
void main() { o = vec4(u_color, 1.0); }`;

  function program(vs, fs) {
    const p = gl.createProgram();
    for (const [type, src] of [[gl.VERTEX_SHADER, vs], [gl.FRAGMENT_SHADER, fs]]) {
      const s = gl.createShader(type);
      gl.shaderSource(s, src);
      gl.compileShader(s);
      if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s));
      gl.attachShader(p, s);
    }
    gl.bindAttribLocation(p, 0, 'a_pos');
    gl.linkProgram(p);
    if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p));
    return p;
  }
  const faceProg = program(VS, FACE_FS);
  const edgeProg = program(VS, EDGE_FS);

  const faceVao = gl.createVertexArray(), edgeVao = gl.createVertexArray();
  const posBuf = gl.createBuffer(), idxBuf = gl.createBuffer(), edgeBuf = gl.createBuffer();
  gl.bindVertexArray(faceVao);
  gl.bindBuffer(gl.ARRAY_BUFFER, posBuf);
  gl.enableVertexAttribArray(0);
  gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
  gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, idxBuf);
  gl.bindVertexArray(edgeVao);
  gl.bindBuffer(gl.ARRAY_BUFFER, edgeBuf);
  gl.enableVertexAttribArray(0);
  gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
  gl.bindVertexArray(null);

  let mesh = null; // { idxCount, edgeVerts, min, max }

  // --- small vector / matrix helpers (column-major mat4) ---
  const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
  const add = (a, b) => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
  const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
  const norm = (a) => { const l = Math.hypot(a[0], a[1], a[2]); return [a[0] / l, a[1] / l, a[2] / l]; };
  function mul(a, b) {
    const o = new Float32Array(16);
    for (let c = 0; c < 4; c++) for (let r = 0; r < 4; r++) {
      let s = 0; for (let k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k];
      o[c * 4 + r] = s;
    }
    return o;
  }
  function lookAt(eye, target, up) {
    const f = norm(sub(target, eye)), s = norm(cross(f, up)), u = cross(s, f);
    return [s[0], u[0], -f[0], 0, s[1], u[1], -f[1], 0, s[2], u[2], -f[2], 0,
      -dot(s, eye), -dot(u, eye), dot(f, eye), 1];
  }
  function perspective(fovy, aspect, near, far) {
    const f = 1 / Math.tan(fovy / 2), nf = 1 / (near - far);
    return [f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, (far + near) * nf, -1, 0, 0, 2 * far * near * nf, 0];
  }

  function camera() {
    const target = [(mesh.min[0] + mesh.max[0]) / 2, (mesh.min[1] + mesh.max[1]) / 2, (mesh.min[2] + mesh.max[2]) / 2];
    const radius = Math.hypot(...sub(mesh.max, mesh.min)) / 2;
    const yaw = 30 * Math.PI / 180, pitch = 35 * Math.PI / 180, fovy = 45 * Math.PI / 180;
    const aspect = canvas.width / canvas.height;
    const half = Math.min(fovy, 2 * Math.atan(Math.tan(fovy / 2) * aspect)) / 2;
    const dist = 1.1 * radius / Math.sin(half);
    const eye = add(target, [dist * Math.cos(pitch) * Math.cos(yaw), -dist * Math.cos(pitch) * Math.sin(yaw), dist * Math.sin(pitch)]);
    const mvp = mul(perspective(fovy, aspect, dist - radius * 1.5 > 1 ? dist - radius * 1.5 : 1, dist + radius * 1.5),
      lookAt(eye, target, [0, 0, 1]));
    const light = norm(add(norm(sub(eye, target)), [-0.3, -0.2, 0.9]));
    return { mvp, light };
  }

  function draw() {
    const t0 = performance.now();
    gl.viewport(0, 0, canvas.width, canvas.height);
    gl.clearColor(...BG_SRGB.map((c) => c / 255), 1);
    gl.clearDepth(1);
    gl.depthMask(true);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    if (mesh) {
      const { mvp, light } = camera();
      gl.disable(gl.CULL_FACE);
      gl.enable(gl.DEPTH_TEST);
      gl.depthFunc(gl.LESS);
      // faces
      gl.enable(gl.POLYGON_OFFSET_FILL);
      gl.polygonOffset(1.5, 2);
      gl.useProgram(faceProg);
      gl.uniformMatrix4fv(gl.getUniformLocation(faceProg, 'u_mvp'), false, mvp);
      gl.uniform3fv(gl.getUniformLocation(faceProg, 'u_light'), light);
      gl.uniform3fv(gl.getUniformLocation(faceProg, 'u_color'), FACE_SRGB.map(toLinear));
      gl.uniform1f(gl.getUniformLocation(faceProg, 'u_ambient'), AMBIENT);
      gl.bindVertexArray(faceVao);
      gl.drawElements(gl.TRIANGLES, mesh.idxCount, gl.UNSIGNED_INT, 0);
      gl.disable(gl.POLYGON_OFFSET_FILL);
      // edges
      gl.depthMask(false);
      gl.useProgram(edgeProg);
      gl.uniformMatrix4fv(gl.getUniformLocation(edgeProg, 'u_mvp'), false, mvp);
      gl.uniform3fv(gl.getUniformLocation(edgeProg, 'u_color'), EDGE_SRGB.map((c) => c / 255));
      gl.bindVertexArray(edgeVao);
      gl.drawArrays(gl.LINES, 0, mesh.edgeVerts);
      gl.bindVertexArray(null);
      gl.depthMask(true);
    }
    gl.finish();
    return performance.now() - t0;
  }

  async function reload(url) {
    const sep = url.includes('?') ? '&' : '?';
    const t0 = performance.now();
    const res = await fetch(`${url}${sep}v=${Date.now()}`, { cache: 'no-store' });
    if (!res.ok) throw new Error(`fetch ${url}: ${res.status}`);
    const { instance } = await WebAssembly.instantiate(await res.arrayBuffer(), {});
    const instantiate_ms = performance.now() - t0;

    const t1 = performance.now();
    const e = instance.exports;
    if (e.slab_mesh(0, 0, 6000, 4000, 220) !== 1) throw new Error('slab_mesh returned an error');
    const buf = e.memory.buffer;
    const pos = new Float32Array(buf, e.pos_ptr(), e.pos_len() * 3);
    const idx = new Uint32Array(buf, e.idx_ptr(), e.idx_len());
    const edges = new Float32Array(buf, e.edge_ptr(), e.edge_len() * 6);
    const min = [Infinity, Infinity, Infinity], max = [-Infinity, -Infinity, -Infinity];
    for (let i = 0; i < pos.length; i++) {
      const a = i % 3; if (pos[i] < min[a]) min[a] = pos[i]; if (pos[i] > max[a]) max[a] = pos[i];
    }
    for (let i = 0; i < idx.length; i++) if (idx[i] >= pos.length / 3) throw new Error(`index ${idx[i]} out of range`);
    gl.bindBuffer(gl.ARRAY_BUFFER, posBuf);
    gl.bufferData(gl.ARRAY_BUFFER, pos, gl.STATIC_DRAW);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, idxBuf);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, idx, gl.STATIC_DRAW);
    gl.bindBuffer(gl.ARRAY_BUFFER, edgeBuf);
    gl.bufferData(gl.ARRAY_BUFFER, edges, gl.STATIC_DRAW);
    gl.bindBuffer(gl.ARRAY_BUFFER, null);
    mesh = { idxCount: idx.length, edgeVerts: edges.length / 3, min, max };
    const upload_ms = performance.now() - t1;

    const draw_ms = draw();
    window.slab.stats = { instantiate_ms, upload_ms, draw_ms, vertices: pos.length / 3, triangles: idx.length / 3, edges: edges.length / 6 };
    return window.slab.stats;
  }

  // Pixel helpers for tests: (x, y) from the top-left corner, in canvas pixels.
  function readAll() {
    const w = canvas.width, h = canvas.height, px = new Uint8Array(w * h * 4);
    gl.readPixels(0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, px);
    return px;
  }
  function pixel(x, y) {
    const px = new Uint8Array(4);
    gl.readPixels(x, canvas.height - 1 - y, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
    return [px[0], px[1], px[2]];
  }
  function countNear(r, g, b, tol) {
    const px = readAll();
    let n = 0;
    for (let i = 0; i < px.length; i += 4) {
      if (Math.abs(px[i] - r) <= tol && Math.abs(px[i + 1] - g) <= tol && Math.abs(px[i + 2] - b) <= tol) n++;
    }
    return n;
  }

  window.slab = { reload, draw, stats: null, pixel, countNear, size: () => [canvas.width, canvas.height] };
  const wasm = new URLSearchParams(location.search).get('wasm') || 'stub.wasm';
  reload(wasm).catch((err) => console.error(err));
})();
