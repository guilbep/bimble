// WebGL2 viewport for the native C host (dev only, `?native=1&gl=1&port=&token=`; ticket 11a,
// docs/research/c-core-spec.md §2). Fetches `GET /mesh` (BMSH, layout in ccore/host/mesh.c) at
// start and whenever the change feed's `rev` changes, and draws it in Bimble's style: flat faces
// (normal from dFdx/dFdy, two-sided Lambert, ambient 0.62, light as bimble-render), one colour per
// material family, the selection in the selection colour, crease edges as gl.LINES. Orbit (left
// drag), pan (right or middle drag, shift-drag), zoom (wheel), fit-all on the first mesh. A click
// without a drag picks on the CPU (ray against triangles) and calls `select([id])`, or `select([])`
// on empty space. No dependency: the few matrix helpers are below.
import { select, state } from "./bimble";

// Palette, sRGB (bimble-core style.rs:302, :335-410), by MaterialCategory index.
const CATEGORY_SRGB: [number, number, number][] = [
  [214, 214, 210], // concrete
  [150, 170, 196], // steel
  [226, 196, 150], // timber
  [206, 150, 128], // masonry
  [236, 220, 120], // insulation
  [176, 160, 132], // earth
  [176, 214, 226], // glass
  [238, 238, 236], // other
];
const BACKGROUND_SRGB = [247, 247, 245];
const EDGE_SRGB = [26, 26, 28];
const SELECTION_SRGB: [number, number, number] = [215, 172, 182];
const AMBIENT = 0.62;
// Camera defaults (bimble-render lib.rs Camera::default): yaw -55°, pitch 28°, vertical fov 35°.
const FOV_Y = (35 * Math.PI) / 180;
const CLICK_PX = 4;

interface Bmsh {
  rev: number;
  origin: [number, number, number];
  ids: string[];
  cats: Uint8Array;
  /** Per element: first vertex, vertex count, first index, index count. */
  v0: Uint32Array;
  nv: Uint32Array;
  i0: Uint32Array;
  pos: Float32Array;
  idx: Uint32Array;
  edges: Float32Array;
}

/** Parses BMSH version 1 (ccore/host/mesh.c). Throws on a malformed buffer. */
export function parseBmsh(ab: ArrayBuffer): Bmsh {
  const dv = new DataView(ab);
  if (ab.byteLength < 52 || dv.getUint32(0, true) !== 0x48534d42) throw new Error("not BMSH");
  if (dv.getUint32(4, true) !== 1) throw new Error(`BMSH version ${dv.getUint32(4, true)}`);
  const u = (o: number) => dv.getUint32(o, true);
  const rev = u(8), nE = u(12), nV = u(16), nI = u(20), nS = u(24);
  const size = 52 + 48 * nE + 12 * nV + 4 * nI + 32 * nS;
  if (size !== ab.byteLength) throw new Error(`BMSH size ${ab.byteLength}, expected ${size}`);
  const origin: [number, number, number] = [dv.getFloat64(28, true), dv.getFloat64(36, true), dv.getFloat64(44, true)];
  const ids: string[] = [];
  const cats = new Uint8Array(nE), v0 = new Uint32Array(nE), nv = new Uint32Array(nE), i0 = new Uint32Array(nE);
  const bytes = new Uint8Array(ab);
  for (let e = 0, o = 52; e < nE; e++, o += 48) {
    ids.push(String.fromCharCode(...bytes.subarray(o, o + 22)));
    cats[e] = bytes[o + 22] ?? 7;
    v0[e] = u(o + 24);
    nv[e] = u(o + 28);
    i0[e] = u(o + 32);
  }
  let o = 52 + 48 * nE;
  const pos = new Float32Array(ab, o, 3 * nV);
  o += 12 * nV;
  const idx = new Uint32Array(ab, o, nI);
  o += 4 * nI;
  const edges = new Float32Array(ab, o, 6 * nS);
  for (let i = 0; i < nI; i++) if ((idx[i] ?? 0) >= nV) throw new Error("BMSH index out of range");
  return { rev, origin, ids, cats, v0, nv, i0, pos, idx, edges };
}

// ---- small vector / matrix helpers (column-major mat4, as WebGL) ----
type V3 = [number, number, number];
const sub = (a: V3, b: V3): V3 => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const add = (a: V3, b: V3): V3 => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
const scale = (a: V3, k: number): V3 => [a[0] * k, a[1] * k, a[2] * k];
const dot = (a: V3, b: V3) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const cross = (a: V3, b: V3): V3 => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
const norm = (a: V3): V3 => scale(a, 1 / (Math.hypot(a[0], a[1], a[2]) || 1));
function mul(a: Float32Array, b: Float32Array): Float32Array {
  const o = new Float32Array(16);
  for (let c = 0; c < 4; c++)
    for (let r = 0; r < 4; r++) {
      let s = 0;
      for (let k = 0; k < 4; k++) s += (a[k * 4 + r] ?? 0) * (b[c * 4 + k] ?? 0);
      o[c * 4 + r] = s;
    }
  return o;
}
function lookAt(eye: V3, target: V3, up: V3): Float32Array {
  const f = norm(sub(target, eye)), s = norm(cross(f, up)), v = cross(s, f);
  return new Float32Array([s[0], v[0], -f[0], 0, s[1], v[1], -f[1], 0, s[2], v[2], -f[2], 0, -dot(s, eye), -dot(v, eye), dot(f, eye), 1]);
}
function perspective(fovy: number, aspect: number, near: number, far: number): Float32Array {
  const f = 1 / Math.tan(fovy / 2), nf = 1 / (near - far);
  return new Float32Array([f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, (far + near) * nf, -1, 0, 0, 2 * far * near * nf, 0]);
}
const toLinear = (c: number) => {
  c /= 255;
  return c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4);
};

const VS = `#version 300 es
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_col;
uniform mat4 u_mvp;
out vec3 v_pos;
out vec3 v_col;
void main() { v_pos = a_pos; v_col = a_col; gl_Position = u_mvp * vec4(a_pos, 1.0); }`;
const FS = `#version 300 es
precision highp float;
in vec3 v_pos;
in vec3 v_col;
uniform vec3 u_light;
uniform float u_ambient; // < 0: unlit (edges)
out vec4 o;
vec3 to_linear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c)); }
vec3 to_srgb(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c)); }
void main() {
  if (u_ambient < 0.0) { o = vec4(v_col, 1.0); return; }
  vec3 n = cross(dFdx(v_pos), dFdy(v_pos));
  float len2 = dot(n, n);
  float lambert = len2 > 1e-30 ? abs(dot(n, u_light)) * inversesqrt(len2) : 1.0;
  float k = u_ambient + (1.0 - u_ambient) * lambert;
  o = vec4(to_srgb(to_linear(v_col) * k), 1.0);
}`;

interface GlStats {
  /** ms from navigation start to the first frame drawn with a mesh. */
  firstFrameMs: number | null;
  meshBytes: number;
  meshFetchMs: number;
  meshRev: number | null;
  frames: number;
  picks: number;
  error: string | null;
}
declare global {
  interface Window {
    __bbGl?: GlStats;
  }
}

/** Draws the host's `/mesh` into #bimble with WebGL2. Returns a function that marks the view dirty. */
export function glViewport(base: string, token: string): () => void {
  const canvas = document.getElementById("bimble");
  const stats: GlStats = { firstFrameMs: null, meshBytes: 0, meshFetchMs: 0, meshRev: null, frames: 0, picks: 0, error: null };
  window.__bbGl = stats;
  if (!(canvas instanceof HTMLCanvasElement)) return () => undefined;
  const gl = canvas.getContext("webgl2", { antialias: false, preserveDrawingBuffer: true });
  if (!gl) {
    stats.error = "WebGL2 is not available";
    console.warn("gl viewport:", stats.error);
    return () => undefined;
  }
  const t = encodeURIComponent(token);

  const compile = (type: number, src: string): WebGLShader => {
    const s = gl.createShader(type);
    if (!s) throw new Error("createShader");
    gl.shaderSource(s, src);
    gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s) ?? "shader");
    return s;
  };
  const prog = gl.createProgram();
  gl.attachShader(prog, compile(gl.VERTEX_SHADER, VS));
  gl.attachShader(prog, compile(gl.FRAGMENT_SHADER, FS));
  gl.linkProgram(prog);
  if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(prog) ?? "link");
  const uMvp = gl.getUniformLocation(prog, "u_mvp");
  const uLight = gl.getUniformLocation(prog, "u_light");
  const uAmbient = gl.getUniformLocation(prog, "u_ambient");

  // One VAO: positions (location 0), per-vertex sRGB colour (location 1, u8 normalized), indices.
  // Edges reuse it: location 0 points at the edge buffer, location 1 is a constant.
  const vao = gl.createVertexArray();
  const posBuf = gl.createBuffer(), colBuf = gl.createBuffer(), idxBuf = gl.createBuffer(), edgeBuf = gl.createBuffer();
  gl.bindVertexArray(vao);
  gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, idxBuf);
  gl.bindVertexArray(null);

  let mesh: Bmsh | null = null;
  let bounds: { center: V3; radius: number } | null = null;
  let selKey = "";
  // Camera (bimble-render Camera): orbit around `target` at `distance`, z up.
  let yaw = (-55 * Math.PI) / 180, pitch = (28 * Math.PI) / 180, distance = 10000;
  let target: V3 = [0, 0, 0];
  let dirty = true;
  let loading = false;
  let wantRev: number | null = null;
  // The host process the mesh came from (`epoch` of GET /change). A restarted host may restore
  // its session and answer the same rev with a new build's mesh: a new epoch also refetches.
  let epoch: string | null = null;
  let seenState = state.value;
  let checking = false;
  const hostEpoch = async (): Promise<string | null> => {
    const r = await fetch(`${base}/change?since=0&t=${t}`, { cache: "no-store" });
    const m = (await r.json()) as { epoch?: unknown };
    return typeof m.epoch === "string" ? m.epoch : null;
  };
  const checkEpoch = async (): Promise<void> => {
    checking = true;
    try {
      const e = await hostEpoch();
      if (e !== null && e !== epoch) {
        if (epoch !== null) wantRev = null; // same rev, another process: load again
        epoch = e;
      }
    } catch {
      // the host may be restarting: its next change checks again
    } finally {
      checking = false;
    }
  };
  let fitted = false;

  const eye = (): V3 => {
    const cp = Math.cos(pitch);
    return add(target, scale([cp * Math.cos(yaw), cp * Math.sin(yaw), Math.sin(pitch)], distance));
  };
  const matrices = () => {
    const aspect = Math.max(1e-3, canvas.width / Math.max(1, canvas.height));
    const r = bounds?.radius ?? distance;
    const far = distance + 2 * r + 1;
    const near = Math.max(1, Math.min(distance - 1.5 * r, far / 1000), far / 100000);
    const e = eye();
    const up: V3 = Math.abs(pitch) > 1.55 ? [-Math.cos(yaw), -Math.sin(yaw), 0] : [0, 0, 1];
    const view = lookAt(e, target, up);
    const proj = perspective(FOV_Y, aspect, near, far);
    return { e, view, proj, mvp: mul(proj, view), aspect, up };
  };
  const fit = () => {
    if (!bounds) return;
    target = [...bounds.center];
    const aspect = Math.max(1e-3, canvas.width / Math.max(1, canvas.height));
    const halfV = FOV_Y / 2, halfH = Math.atan(Math.tan(halfV) * aspect);
    distance = (1.05 * bounds.radius) / Math.sin(Math.min(halfV, halfH));
  };

  const colours = (): Uint8Array => {
    const m = mesh;
    if (!m) return new Uint8Array(0);
    const sel = new Set(state.value.selection);
    const out = new Uint8Array(m.pos.length);
    for (let e = 0; e < m.ids.length; e++) {
      const c = sel.has(m.ids[e] ?? "") ? SELECTION_SRGB : (CATEGORY_SRGB[m.cats[e] ?? 7] ?? CATEGORY_SRGB[7] ?? SELECTION_SRGB);
      const a = m.v0[e] ?? 0, n = m.nv[e] ?? 0;
      for (let v = a; v < a + n; v++) out.set(c, 3 * v);
    }
    return out;
  };

  const upload = (m: Bmsh) => {
    mesh = m;
    const min: V3 = [Infinity, Infinity, Infinity], max: V3 = [-Infinity, -Infinity, -Infinity];
    for (let i = 0; i < m.pos.length; i++) {
      const a = i % 3, v = m.pos[i] ?? 0;
      if (v < min[a]!) min[a] = v;
      if (v > max[a]!) max[a] = v;
    }
    bounds = m.pos.length ? { center: scale(add(min, max), 0.5), radius: Math.max(1, Math.hypot(...sub(max, min)) / 2) } : null;
    gl.bindVertexArray(vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, posBuf);
    gl.bufferData(gl.ARRAY_BUFFER, m.pos, gl.STATIC_DRAW);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, idxBuf);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, m.idx, gl.STATIC_DRAW);
    gl.bindVertexArray(null);
    gl.bindBuffer(gl.ARRAY_BUFFER, edgeBuf);
    gl.bufferData(gl.ARRAY_BUFFER, m.edges, gl.STATIC_DRAW);
    selKey = "\u0000"; // recolour on the next frame
    if (!fitted && bounds) {
      fit();
      fitted = true;
    }
    dirty = true;
  };

  const load = async (): Promise<void> => {
    loading = true;
    try {
      const t0 = performance.now();
      epoch = await hostEpoch().catch(() => null);
      const r = await fetch(`${base}/mesh?t=${t}`, { cache: "no-store" });
      if (!r.ok) throw new Error(`mesh: HTTP ${r.status} ${await r.text()}`);
      const ab = await r.arrayBuffer();
      stats.meshFetchMs = performance.now() - t0;
      stats.meshBytes = ab.byteLength;
      const m = parseBmsh(ab);
      stats.meshRev = m.rev;
      stats.error = null;
      upload(m);
    } catch (e) {
      stats.error = e instanceof Error ? e.message : String(e);
      console.warn("gl viewport:", stats.error);
      await new Promise((res) => setTimeout(res, 1000)); // the host may be restarting: retry
      wantRev = null;
    } finally {
      loading = false;
    }
  };

  const draw = () => {
    gl.viewport(0, 0, canvas.width, canvas.height);
    gl.clearColor(BACKGROUND_SRGB[0]! / 255, BACKGROUND_SRGB[1]! / 255, BACKGROUND_SRGB[2]! / 255, 1);
    gl.clearDepth(1);
    gl.depthMask(true);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    const m = mesh;
    if (!m) return;
    const { e, mvp } = matrices();
    const light = norm(add(norm(sub(e, target)), [-0.3, -0.2, 0.9]));
    gl.useProgram(prog);
    gl.uniformMatrix4fv(uMvp, false, mvp);
    gl.uniform3fv(uLight, light);
    gl.enable(gl.DEPTH_TEST);
    gl.depthFunc(gl.LEQUAL);
    gl.disable(gl.CULL_FACE);
    gl.bindVertexArray(vao);
    // Faces, pushed back so the edges on them win the depth test.
    gl.enable(gl.POLYGON_OFFSET_FILL);
    gl.polygonOffset(1.5, 2);
    gl.uniform1f(uAmbient, AMBIENT);
    gl.bindBuffer(gl.ARRAY_BUFFER, posBuf);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
    gl.bindBuffer(gl.ARRAY_BUFFER, colBuf);
    gl.enableVertexAttribArray(1);
    gl.vertexAttribPointer(1, 3, gl.UNSIGNED_BYTE, true, 0, 0);
    gl.drawElements(gl.TRIANGLES, m.idx.length, gl.UNSIGNED_INT, 0);
    gl.disable(gl.POLYGON_OFFSET_FILL);
    // Edges: the same VAO, location 0 on the edge buffer, a constant colour.
    gl.uniform1f(uAmbient, -1);
    gl.bindBuffer(gl.ARRAY_BUFFER, edgeBuf);
    gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
    gl.disableVertexAttribArray(1);
    gl.vertexAttrib3f(1, EDGE_SRGB[0]! / 255, EDGE_SRGB[1]! / 255, EDGE_SRGB[2]! / 255);
    gl.depthMask(false);
    gl.drawArrays(gl.LINES, 0, m.edges.length / 3);
    gl.depthMask(true);
    gl.bindVertexArray(null);
    stats.frames++;
    if (stats.firstFrameMs === null) stats.firstFrameMs = performance.now();
  };

  /** The element under canvas pixel (x, y) (CSS px from the top-left), or -1. */
  const pick = (x: number, y: number): number => {
    const m = mesh;
    if (!m) return -1;
    const { e, aspect, up } = matrices();
    const f = norm(sub(target, e)), s = norm(cross(f, up)), u = cross(s, f);
    const ndcX = (2 * x) / canvas.clientWidth - 1, ndcY = 1 - (2 * y) / canvas.clientHeight;
    const th = Math.tan(FOV_Y / 2);
    const dir = norm(add(f, add(scale(s, ndcX * th * aspect), scale(u, ndcY * th))));
    let best = Infinity, hit = -1;
    const P = m.pos, I = m.idx;
    const vtx = (i: number): V3 => [P[3 * i] ?? 0, P[3 * i + 1] ?? 0, P[3 * i + 2] ?? 0];
    for (let k = 0; k + 2 < I.length; k += 3) {
      // Möller–Trumbore, two-sided.
      const a = vtx(I[k] ?? 0), b = vtx(I[k + 1] ?? 0), c = vtx(I[k + 2] ?? 0);
      const e1 = sub(b, a), e2 = sub(c, a), p = cross(dir, e2), det = dot(e1, p);
      if (Math.abs(det) < 1e-12) continue;
      const inv = 1 / det, tv = sub(e, a), uu = dot(tv, p) * inv;
      if (uu < 0 || uu > 1) continue;
      const q = cross(tv, e1), vv = dot(dir, q) * inv;
      if (vv < 0 || uu + vv > 1) continue;
      const d = dot(e2, q) * inv;
      if (d > 0 && d < best) {
        best = d;
        hit = k;
      }
    }
    if (hit < 0) return -1;
    // The element whose index range holds the triangle (rows are in index order).
    let lo = 0, hi = m.ids.length - 1;
    while (lo < hi) {
      const mid = (lo + hi + 1) >> 1;
      if ((m.i0[mid] ?? 0) <= hit) lo = mid;
      else hi = mid - 1;
    }
    return lo;
  };

  new ResizeObserver(() => {
    const dpr = window.devicePixelRatio || 1;
    canvas.width = Math.max(1, Math.round(canvas.clientWidth * dpr));
    canvas.height = Math.max(1, Math.round(canvas.clientHeight * dpr));
    dirty = true;
  }).observe(canvas);

  let drag: { pan: boolean; x: number; y: number; x0: number; y0: number; moved: boolean } | null = null;
  canvas.addEventListener("pointerdown", (e) => {
    canvas.setPointerCapture(e.pointerId);
    drag = { pan: e.button === 1 || e.button === 2 || (e.button === 0 && e.shiftKey), x: e.clientX, y: e.clientY, x0: e.clientX, y0: e.clientY, moved: false };
  });
  canvas.addEventListener("pointermove", (e) => {
    if (!drag) return;
    const dx = e.clientX - drag.x, dy = e.clientY - drag.y;
    drag.x = e.clientX;
    drag.y = e.clientY;
    if (Math.hypot(e.clientX - drag.x0, e.clientY - drag.y0) > CLICK_PX) drag.moved = true;
    if (!drag.moved || (dx === 0 && dy === 0)) return;
    if (drag.pan) {
      const { e: ey, up } = matrices();
      const f = norm(sub(target, ey)), s = norm(cross(f, up)), u = cross(s, f);
      const k = (2 * distance * Math.tan(FOV_Y / 2)) / Math.max(1, canvas.clientHeight);
      target = add(target, add(scale(s, -dx * k), scale(u, dy * k)));
    } else {
      yaw -= dx * 0.01;
      pitch = Math.min(1.57, Math.max(-1.57, pitch + dy * 0.01));
    }
    dirty = true;
  });
  const release = (e: PointerEvent) => {
    const d = drag;
    drag = null;
    if (canvas.hasPointerCapture(e.pointerId)) canvas.releasePointerCapture(e.pointerId);
    if (e.type !== "pointerup" || !d || d.moved || e.button !== 0 || d.pan) return;
    const r = canvas.getBoundingClientRect();
    const k = pick(e.clientX - r.left, e.clientY - r.top);
    stats.picks++;
    const id = k >= 0 ? mesh?.ids[k] : undefined;
    const err = select(id ? [id] : []);
    if (err) console.warn("gl viewport: select:", err);
  };
  canvas.addEventListener("pointerup", release);
  canvas.addEventListener("pointercancel", release);
  canvas.addEventListener("contextmenu", (e) => e.preventDefault());
  canvas.addEventListener("wheel", (e) => {
    e.preventDefault();
    distance = Math.max(1, distance * Math.exp(e.deltaY * 0.001));
    dirty = true;
  }, { passive: false });

  const loop = () => {
    const rev = state.value.rev;
    if (state.value !== seenState) {
      seenState = state.value;
      if (!loading && !checking && wantRev === rev) void checkEpoch();
    }
    if (!loading && wantRev !== rev) {
      wantRev = rev;
      void load();
    }
    const key = mesh ? state.value.selection.join(",") : "";
    if (mesh && key !== selKey) {
      selKey = key;
      gl.bindBuffer(gl.ARRAY_BUFFER, colBuf);
      gl.bufferData(gl.ARRAY_BUFFER, colours(), gl.DYNAMIC_DRAW);
      dirty = true;
    }
    if (dirty) {
      dirty = false;
      draw();
    }
    requestAnimationFrame(loop);
  };
  requestAnimationFrame(loop);
  return () => { dirty = true; };
}
