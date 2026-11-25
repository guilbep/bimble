// WebGL2 viewport for the native C host (dev only, `?native=1&gl=1&port=&token=`; ticket 11a,
// docs/research/c-core-spec.md §2). Fetches `GET /mesh` (BMSH, layout in ccore/session/mesh.c) at
// start and whenever the change feed's `rev` changes, and draws it in Bimble's style: flat faces
// (normal from dFdx/dFdy, two-sided Lambert, ambient 0.62, light as bimble-render), one colour per
// material family, the selection in the selection colour, crease edges as gl.LINES. Orbit (left
// drag), pan (right or middle drag, shift-drag), zoom (wheel), fit-all on the first mesh. A click
// without a drag picks on the CPU (ray against triangles) and calls `select([id])`, or `select([])`
// on empty space. No dependency: the few matrix helpers are below.
// The place tool (ticket B4, ADR 0068 rule 2): while `placing` is active, pointer moves and clicks go
// to the C core's tool (op `tool`: the pointer ray in scene coordinates and the ray 12 px beside it;
// Shift: free, no snap, as Rust; Ctrl or Cmd: ortho), which snaps, keeps the points and answers the
// ghost (the element the next click adds, meshed in C); the page draws it translucent, with a marker
// at the snapped point and a line from the last point. Esc cancels (stops placing, as Rust).
// Visibility and the section box (ticket B3, ADR 0014): what `query visibility` and `visibility_sets`
// say is hidden is not drawn (Reveal hidden: drawn in the comparison colour) and cannot be picked.
// With the box on, faces and edges outside it are discarded in the fragment shader; two passes, as
// bimble-render: outward faces lit, then inward faces pushed back and drawn unlit at the cut shade
// (0.72), so a cut solid shows a filled section; an edge whose two faces both look away (BMSH edge
// normals) is not drawn, as Rust hides back edges in sections. Picking ignores hits outside the box.
// Six handles (face centres) and the box's edges are drawn; a left press on a handle is the C core's
// box tool (op tool box_press / box_move / box_release: drag state in C, one session change on release).
import {
  boxTool, placing, select, state, toolCall, visibility, visibilitySets,
  type ToolPointer, type ToolState, type Visibility, type VisibilitySets,
} from "./bimble";

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
// Revealed hidden elements (Reveal hidden): the comparison colour, as Rust's Style::OnlyHere.
const REVEALED_SRGB: [number, number, number] = [232, 150, 64];
// bimble-render Style::cut_shade: the brightness of cut surfaces (ADR 0014).
const CUT_SHADE = 0.72;
// The section box's edges and handles.
const BOX_SRGB: [number, number, number] = [40, 110, 200];
const HANDLE_PX = 11;
// The ghost of the place tool: translucent blue faces, its edges and the snap marker darker.
const GHOST_SRGB: [number, number, number] = [74, 128, 214];
const GHOST_EDGE_SRGB: [number, number, number] = [32, 70, 150];
const GHOST_ALPHA = 0.45;
const SNAP_PX = 12; // lib.rs placing_point: the snap tolerance is 12 screen pixels
const AMBIENT = 0.62;
// Camera defaults (bimble-render lib.rs Camera::default): yaw -55°, pitch 28°, vertical fov 35°.
const FOV_Y = (35 * Math.PI) / 180;
const CLICK_PX = 4;

interface Bmsh {
  rev: number;
  origin: [number, number, number];
  ids: string[];
  cats: Uint8Array;
  /** Per element: first vertex, vertex count, first index, index count, first segment, segment count. */
  v0: Uint32Array;
  nv: Uint32Array;
  i0: Uint32Array;
  ni: Uint32Array;
  s0: Uint32Array;
  ns: Uint32Array;
  pos: Float32Array;
  idx: Uint32Array;
  edges: Float32Array;
  /** Per segment: the two face normals, snorm8 xyzw xyzw (0: unknown). */
  normals: Int8Array;
}

/** Parses BMSH version 1 (ccore/session/mesh.c). Throws on a malformed buffer. */
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
  const cats = new Uint8Array(nE), v0 = new Uint32Array(nE), nv = new Uint32Array(nE), i0 = new Uint32Array(nE), ni = new Uint32Array(nE);
  const s0 = new Uint32Array(nE), ns = new Uint32Array(nE);
  const bytes = new Uint8Array(ab);
  for (let e = 0, o = 52; e < nE; e++, o += 48) {
    ids.push(String.fromCharCode(...bytes.subarray(o, o + 22)));
    cats[e] = bytes[o + 22] ?? 7;
    v0[e] = u(o + 24);
    nv[e] = u(o + 28);
    i0[e] = u(o + 32);
    ni[e] = u(o + 36);
    s0[e] = u(o + 40);
    ns[e] = u(o + 44);
  }
  let o = 52 + 48 * nE;
  const pos = new Float32Array(ab, o, 3 * nV);
  o += 12 * nV;
  const idx = new Uint32Array(ab, o, nI);
  o += 4 * nI;
  const edges = new Float32Array(ab, o, 6 * nS);
  o += 24 * nS;
  const normals = new Int8Array(ab, o, 8 * nS);
  for (let i = 0; i < nI; i++) if ((idx[i] ?? 0) >= nV) throw new Error("BMSH index out of range");
  for (let e = 0; e < nE; e++)
    if ((i0[e] ?? 0) + (ni[e] ?? 0) > nI || (v0[e] ?? 0) + (nv[e] ?? 0) > nV || (s0[e] ?? 0) + (ns[e] ?? 0) > nS) throw new Error("BMSH row out of range");
  return { rev, origin, ids, cats, v0, nv, i0, ni, s0, ns, pos, idx, edges, normals };
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
layout(location = 2) in vec4 a_n1; // an edge's two face normals (snorm8); 0 or unset: unknown
layout(location = 3) in vec4 a_n2;
uniform mat4 u_mvp;
uniform vec3 u_eye;
uniform float u_point;
out vec3 v_pos;
out vec3 v_col;
flat out float v_back;
void main() {
  v_pos = a_pos; v_col = a_col;
  vec3 to_eye = u_eye - a_pos;
  v_back = (dot(a_n1.xyz, to_eye) < 0.0 && dot(a_n2.xyz, to_eye) < 0.0) ? 1.0 : 0.0;
  gl_PointSize = u_point;
  gl_Position = u_mvp * vec4(a_pos, 1.0);
}`;
const FS = `#version 300 es
precision highp float;
in vec3 v_pos;
in vec3 v_col;
flat in float v_back;
uniform vec3 u_light;
uniform int u_clip;      // 1: the section box is on
uniform vec3 u_clip_min; // scene coordinates
uniform vec3 u_clip_max;
uniform float u_cut;     // brightness of cut surfaces (back faces while the box is on)
uniform float u_ambient; // < 0: unlit (edges)
uniform float u_alpha;
out vec4 o;
vec3 to_linear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c)); }
vec3 to_srgb(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c)); }
void main() {
  if (u_clip == 1 && (any(lessThan(v_pos, u_clip_min)) || any(greaterThan(v_pos, u_clip_max)))) discard;
  if (u_ambient < 0.0) {
    if (u_clip == 1 && v_back > 0.5) discard; // a back edge, only ever seen through the cut
    o = vec4(v_col, u_alpha);
    return;
  }
  if (u_clip == 1 && !gl_FrontFacing) { o = vec4(to_srgb(to_linear(v_col) * u_cut), u_alpha); return; }
  vec3 n = cross(dFdx(v_pos), dFdy(v_pos));
  float len2 = dot(n, n);
  float lambert = len2 > 1e-30 ? abs(dot(n, u_light)) * inversesqrt(len2) : 1.0;
  float k = u_ambient + (1.0 - u_ambient) * lambert;
  o = vec4(to_srgb(to_linear(v_col) * k), u_alpha);
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
  /** The place tool: calls made, the last answer's ghost triangles and hover, the last error. */
  toolCalls: number;
  ghostTris: number;
  hover: ToolState["hover"];
  toolError: string | null;
  /** The GlobalId the last completed gesture added. */
  placed: string | null;
  /** Visibility and the section box: elements not drawn, triangles drawn, the box as drawn (scene mm,
   *  the drag's preview while one runs), the face held, box tool calls and the last box error. */
  hiddenElements: number;
  drawnTris: number;
  clip: { min: number[]; max: number[] } | null;
  boxFace: number | null;
  boxCalls: number;
  boxError: string | null;
  /** Test hook: the GlobalId a click at client (x, y) would pick, or null (selects nothing). */
  pickAt?: (x: number, y: number) => string | null;
  /** Test hook: a world point (mm) -> CSS px in the viewport (clientX, clientY), or null. */
  toScreen?: (x: number, y: number, z: number) => [number, number] | null;
}
declare global {
  interface Window {
    __bbGl?: GlStats;
  }
}

/** Where the mesh comes from: the native host over HTTP (`glViewport(base, token)`), or a function
 *  (the C core's wasm module in the page, `?ccore=1`) returning the BMSH bytes, with no process
 *  epoch (one module per page). */
export interface MeshSource {
  /** The BMSH bytes now (GET /mesh). */
  mesh(): Promise<ArrayBuffer>;
  /** The process the mesh came from (GET /change `epoch`); null when there is none. */
  epoch(): Promise<string | null>;
}

/** Draws the host's `/mesh` (or `source`'s) into #bimble with WebGL2. Returns a function that marks the view dirty. */
export function glViewport(base: string | MeshSource, token = ""): () => void {
  const canvas = document.getElementById("bimble");
  const stats: GlStats = { firstFrameMs: null, meshBytes: 0, meshFetchMs: 0, meshRev: null, frames: 0, picks: 0, error: null,
    toolCalls: 0, ghostTris: 0, hover: null, toolError: null, placed: null,
    hiddenElements: 0, drawnTris: 0, clip: null, boxFace: null, boxCalls: 0, boxError: null };
  window.__bbGl = stats;
  if (!(canvas instanceof HTMLCanvasElement)) return () => undefined;
  const gl = canvas.getContext("webgl2", { antialias: false, preserveDrawingBuffer: true });
  if (!gl) {
    stats.error = "WebGL2 is not available";
    console.warn("gl viewport:", stats.error);
    return () => undefined;
  }
  const t = encodeURIComponent(token);
  const source: MeshSource = typeof base !== "string" ? base : {
    epoch: async () => {
      const r = await fetch(`${base}/change?since=0&t=${t}`, { cache: "no-store" });
      const m = (await r.json()) as { epoch?: unknown };
      return typeof m.epoch === "string" ? m.epoch : null;
    },
    mesh: async () => {
      const r = await fetch(`${base}/mesh?t=${t}`, { cache: "no-store" });
      if (!r.ok) throw new Error(`mesh: HTTP ${r.status} ${await r.text()}`);
      return r.arrayBuffer();
    },
  };

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
  const uAlpha = gl.getUniformLocation(prog, "u_alpha");
  const uEye = gl.getUniformLocation(prog, "u_eye");
  const uPoint = gl.getUniformLocation(prog, "u_point");
  const uClip = gl.getUniformLocation(prog, "u_clip");
  const uClipMin = gl.getUniformLocation(prog, "u_clip_min");
  const uClipMax = gl.getUniformLocation(prog, "u_clip_max");
  const uCut = gl.getUniformLocation(prog, "u_cut");

  // One VAO: positions (location 0), per-vertex sRGB colour (location 1, u8 normalized), indices.
  // Edges reuse it: location 0 points at the edge buffer, location 1 is a constant.
  const vao = gl.createVertexArray();
  const posBuf = gl.createBuffer(), colBuf = gl.createBuffer(), idxBuf = gl.createBuffer(), edgeBuf = gl.createBuffer();
  gl.bindVertexArray(vao);
  gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, idxBuf);
  gl.bindVertexArray(null);
  // Edges: their own VAO (positions, the two face normals per vertex, the visible segments' indices).
  const edgeVao = gl.createVertexArray();
  const edgeNrm = gl.createBuffer(), edgeIdx = gl.createBuffer();
  gl.bindVertexArray(edgeVao);
  gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, edgeIdx);
  gl.bindBuffer(gl.ARRAY_BUFFER, edgeBuf);
  gl.enableVertexAttribArray(0);
  gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
  gl.bindBuffer(gl.ARRAY_BUFFER, edgeNrm);
  gl.enableVertexAttribArray(2);
  gl.vertexAttribPointer(2, 4, gl.BYTE, true, 8, 0);
  gl.enableVertexAttribArray(3);
  gl.vertexAttribPointer(3, 4, gl.BYTE, true, 8, 4);
  gl.bindVertexArray(null);
  let faceN = 0, edgeN = 0;
  // Visibility (query visibility and visibility_sets) and the box drag's preview.
  let vis: Visibility | null = null;
  let sets: VisibilitySets | null = null;
  let seenVis: Visibility | null | undefined;
  let visKey = "\u0000";
  /** Per element of the mesh: 0 not drawn, 1 drawn, 2 drawn revealed (hidden, Reveal hidden on). */
  let shown = new Uint8Array(0);
  let preview: { min: number[]; max: number[] } | null = null;
  const boxBuf = gl.createBuffer();
  // The place tool's ghost (its own VAO) and the marker lines (snap mark, clicked points, rubber line).
  const ghostVao = gl.createVertexArray();
  const ghostPos = gl.createBuffer(), ghostIdx = gl.createBuffer(), ghostEdges = gl.createBuffer(), markBuf = gl.createBuffer();
  gl.bindVertexArray(ghostVao);
  gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, ghostIdx);
  gl.bindVertexArray(null);
  let ghostN = 0, ghostEdgeN = 0, markN = 0;
  let tool: ToolState | null = null;

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
  const hostEpoch = (): Promise<string | null> => source.epoch();
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
      const c = sel.has(m.ids[e] ?? "") ? SELECTION_SRGB : shown[e] === 2 ? REVEALED_SRGB
        : (CATEGORY_SRGB[m.cats[e] ?? 7] ?? CATEGORY_SRGB[7] ?? SELECTION_SRGB);
      const a = m.v0[e] ?? 0, n = m.nv[e] ?? 0;
      for (let v = a; v < a + n; v++) out.set(c, 3 * v);
    }
    return out;
  };

  /** Per element: its vertices' box (min xyz, max xyz), for the pick's ray-box test. */
  let boxes = new Float32Array(0);
  const upload = (m: Bmsh) => {
    mesh = m;
    boxes = new Float32Array(6 * m.ids.length);
    for (let e = 0; e < m.ids.length; e++) {
      let x0 = Infinity, y0 = Infinity, z0 = Infinity, x1 = -Infinity, y1 = -Infinity, z1 = -Infinity;
      const a = m.v0[e] ?? 0, n = m.nv[e] ?? 0, P = m.pos;
      for (let v = 3 * a; v < 3 * (a + n); v += 3) {
        const x = P[v] ?? 0, y = P[v + 1] ?? 0, z = P[v + 2] ?? 0;
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
        if (z < z0) z0 = z;
        if (z > z1) z1 = z;
      }
      boxes.set([x0, y0, z0, x1, y1, z1], 6 * e);
    }
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
    gl.bindVertexArray(null);
    gl.bindBuffer(gl.ARRAY_BUFFER, edgeBuf);
    gl.bufferData(gl.ARRAY_BUFFER, m.edges, gl.STATIC_DRAW);
    // The two face normals of each segment, for both of its vertices.
    const nrm = new Int8Array(16 * (m.edges.length / 6));
    for (let k = 0; k < nrm.length / 16; k++) {
      const src = m.normals.subarray(8 * k, 8 * k + 8);
      nrm.set(src, 16 * k);
      nrm.set(src, 16 * k + 8);
    }
    gl.bindBuffer(gl.ARRAY_BUFFER, edgeNrm);
    gl.bufferData(gl.ARRAY_BUFFER, nrm, gl.STATIC_DRAW);
    visKey = "\u0000"; // the draw lists on the next frame
    selKey = "\u0000"; // recolour on the next frame
    if (tool) showTool(tool); // the ghost against the new mesh's origin
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
      const ab = await source.mesh();
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

  /** visibility.rs hidden_by over the whole sets. */
  const hiddenTest = () => {
    const st = sets;
    if (!st) return () => false;
    const hid = new Set(st.hidden_in_view), iso = new Set(st.isolated_in_view), now = st.now ? new Set(st.now.ids) : null;
    const isolateNow = st.now?.isolate === true;
    return (id: string) => hid.has(id) || (iso.size > 0 && !iso.has(id)) || (now !== null && now.has(id) !== isolateNow);
  };
  /** The draw lists: the triangles and edge segments of the elements shown. */
  const drawLists = () => {
    const m = mesh;
    if (!m) return;
    const hidden = hiddenTest(), reveal = sets?.reveal === true;
    shown = new Uint8Array(m.ids.length);
    let nf = 0, ne = 0, nh = 0;
    for (let e = 0; e < m.ids.length; e++) {
      const h = hidden(m.ids[e] ?? "");
      shown[e] = !h ? 1 : reveal ? 2 : 0;
      if (shown[e]) nf += m.ni[e] ?? 0, ne += m.ns[e] ?? 0;
      else nh++;
    }
    const fi = new Uint32Array(nf), ei = new Uint32Array(2 * ne);
    let f = 0, k = 0;
    for (let e = 0; e < m.ids.length; e++) {
      if (!shown[e]) continue;
      const i0 = m.i0[e] ?? 0, n = m.ni[e] ?? 0;
      fi.set(m.idx.subarray(i0, i0 + n), f);
      f += n;
      const s0 = m.s0[e] ?? 0, ns = m.ns[e] ?? 0;
      for (let q = s0; q < s0 + ns; q++) ei[k++] = 2 * q, ei[k++] = 2 * q + 1;
    }
    gl.bindVertexArray(vao);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, idxBuf);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, fi, gl.DYNAMIC_DRAW);
    gl.bindVertexArray(edgeVao);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, edgeIdx);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, ei, gl.DYNAMIC_DRAW);
    gl.bindVertexArray(null);
    faceN = nf;
    edgeN = 2 * ne;
    stats.hiddenElements = nh;
    stats.drawnTris = nf / 3;
    selKey = "\u0000"; // revealed elements change colour
    dirty = true;
  };
  /** The box that cuts now: the drag's preview, else the session's box when it is on. */
  const clipBox = (): { min: number[]; max: number[] } | null => {
    if (preview) return preview;
    const sb = vis?.section_box;
    return sb?.on && sb.box ? { min: sb.box.min, max: sb.box.max } : null;
  };
  const faceCenter = (b: { min: number[]; max: number[] }, f: number): V3 => {
    const c: V3 = [0, 1, 2].map((k) => ((b.min[k] ?? 0) + (b.max[k] ?? 0)) / 2) as V3;
    const a = Math.floor(f / 2);
    c[a] = (f & 1 ? b.max[a] : b.min[a]) ?? 0;
    return c;
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
    const box = clipBox();
    stats.clip = box;
    gl.useProgram(prog);
    gl.uniformMatrix4fv(uMvp, false, mvp);
    gl.uniform3fv(uLight, light);
    gl.uniform3fv(uEye, e);
    gl.uniform1f(uPoint, 1);
    gl.uniform1f(uAlpha, 1);
    gl.uniform1i(uClip, box ? 1 : 0);
    gl.uniform3fv(uClipMin, box ? box.min : [0, 0, 0]);
    gl.uniform3fv(uClipMax, box ? box.max : [0, 0, 0]);
    gl.uniform1f(uCut, CUT_SHADE);
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
    if (!box) gl.drawElements(gl.TRIANGLES, faceN, gl.UNSIGNED_INT, 0);
    else {
      // ADR 0014: outward faces lit, then inward faces (seen only through the cut) unlit at the cut
      // shade, pushed back so silhouette corners keep the outward face.
      gl.enable(gl.CULL_FACE);
      gl.cullFace(gl.BACK);
      gl.drawElements(gl.TRIANGLES, faceN, gl.UNSIGNED_INT, 0);
      gl.cullFace(gl.FRONT);
      gl.polygonOffset(4, 8);
      gl.drawElements(gl.TRIANGLES, faceN, gl.UNSIGNED_INT, 0);
      gl.disable(gl.CULL_FACE);
    }
    gl.disable(gl.POLYGON_OFFSET_FILL);
    // Edges: their VAO (with the face normals for the back-edge test), a constant colour.
    gl.uniform1f(uAmbient, -1);
    gl.bindVertexArray(edgeVao);
    gl.vertexAttrib3f(1, EDGE_SRGB[0]! / 255, EDGE_SRGB[1]! / 255, EDGE_SRGB[2]! / 255);
    gl.depthMask(false);
    gl.drawElements(gl.LINES, edgeN, gl.UNSIGNED_INT, 0);
    gl.depthMask(true);
    gl.bindVertexArray(null);
    gl.uniform1i(uClip, 0);
    if (box) drawBox(box);
    drawGhost();
    stats.frames++;
    if (stats.firstFrameMs === null) stats.firstFrameMs = performance.now();
  };

  /** The section box: its 12 edges and the 6 face handles, over the model. */
  const drawBox = (b: { min: number[]; max: number[] }) => {
    const c = (i: number): number[] => [0, 1, 2].map((k) => ((i >> k) & 1 ? b.max[k] : b.min[k]) ?? 0);
    const v: number[] = [];
    for (let i = 0; i < 8; i++)
      for (let k = 0; k < 3; k++) if (!((i >> k) & 1)) v.push(...c(i), ...c(i | (1 << k)));
    for (let f = 0; f < 6; f++) v.push(...faceCenter(b, f));
    gl.bindVertexArray(ghostVao);
    gl.bindBuffer(gl.ARRAY_BUFFER, boxBuf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(v), gl.DYNAMIC_DRAW);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
    gl.disableVertexAttribArray(1);
    gl.uniform1f(uAmbient, -1);
    gl.vertexAttrib3f(1, BOX_SRGB[0] / 255, BOX_SRGB[1] / 255, BOX_SRGB[2] / 255);
    gl.disable(gl.DEPTH_TEST);
    gl.drawArrays(gl.LINES, 0, 24);
    gl.uniform1f(uPoint, HANDLE_PX);
    gl.drawArrays(gl.POINTS, 24, 6);
    gl.uniform1f(uPoint, 1);
    gl.enable(gl.DEPTH_TEST);
    gl.bindVertexArray(null);
  };

  /** The ghost: translucent faces over the model (depth-tested, no depth write), its edges, then the
   *  marker lines on top. Canvas alpha stays 1 (the blend keeps the destination's). */
  const drawGhost = () => {
    if (!ghostN && !markN) return;
    gl.bindVertexArray(ghostVao);
    gl.enable(gl.BLEND);
    gl.blendFuncSeparate(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA, gl.ZERO, gl.ONE);
    gl.depthMask(false);
    gl.disableVertexAttribArray(1);
    gl.enableVertexAttribArray(0);
    if (ghostN) {
      gl.uniform1f(uAmbient, AMBIENT);
      gl.uniform1f(uAlpha, GHOST_ALPHA);
      gl.vertexAttrib3f(1, GHOST_SRGB[0] / 255, GHOST_SRGB[1] / 255, GHOST_SRGB[2] / 255);
      gl.bindBuffer(gl.ARRAY_BUFFER, ghostPos);
      gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
      gl.drawElements(gl.TRIANGLES, ghostN, gl.UNSIGNED_INT, 0);
    }
    gl.uniform1f(uAmbient, -1);
    gl.uniform1f(uAlpha, 0.9);
    gl.vertexAttrib3f(1, GHOST_EDGE_SRGB[0] / 255, GHOST_EDGE_SRGB[1] / 255, GHOST_EDGE_SRGB[2] / 255);
    if (ghostEdgeN) {
      gl.bindBuffer(gl.ARRAY_BUFFER, ghostEdges);
      gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
      gl.drawArrays(gl.LINES, 0, ghostEdgeN);
    }
    if (markN) {
      gl.disable(gl.DEPTH_TEST);
      gl.bindBuffer(gl.ARRAY_BUFFER, markBuf);
      gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
      gl.drawArrays(gl.LINES, 0, markN);
      gl.enable(gl.DEPTH_TEST);
    }
    gl.depthMask(true);
    gl.disable(gl.BLEND);
    gl.uniform1f(uAlpha, 1);
    gl.bindVertexArray(null);
  };

  /** Uploads the tool's answer: the ghost (moved from its origin to the mesh's) and the markers. */
  const showTool = (t: ToolState | null) => {
    tool = t;
    const m = mesh;
    stats.hover = t?.hover ?? null;
    if (!m || !t || !t.active) {
      ghostN = ghostEdgeN = markN = 0;
      stats.ghostTris = 0;
      dirty = true;
      return;
    }
    const g = t.ghost;
    ghostN = ghostEdgeN = 0;
    if (g) {
      const d = [g.origin[0] - m.origin[0], g.origin[1] - m.origin[1], g.origin[2] - m.origin[2]];
      const pos = new Float32Array(g.pos.length), edges = new Float32Array(g.edges.length);
      for (let i = 0; i < pos.length; i++) pos[i] = (g.pos[i] ?? 0) + (d[i % 3] ?? 0);
      for (let i = 0; i < edges.length; i++) edges[i] = (g.edges[i] ?? 0) + (d[i % 3] ?? 0);
      gl.bindVertexArray(ghostVao);
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, ghostIdx);
      gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, new Uint32Array(g.idx), gl.DYNAMIC_DRAW);
      gl.bindVertexArray(null);
      gl.bindBuffer(gl.ARRAY_BUFFER, ghostPos);
      gl.bufferData(gl.ARRAY_BUFFER, pos, gl.DYNAMIC_DRAW);
      gl.bindBuffer(gl.ARRAY_BUFFER, ghostEdges);
      gl.bufferData(gl.ARRAY_BUFFER, edges, gl.DYNAMIC_DRAW);
      ghostN = g.idx.length;
      ghostEdgeN = edges.length / 3;
    }
    stats.ghostTris = ghostN / 3;
    // Markers in scene coordinates: a cross at the hover point and at each clicked point, a line from
    // the last point to the hover. Size: 1.5 % of the view distance.
    const lines: number[] = [];
    const z = (t.hover?.z ?? 0) - m.origin[2], r = 0.015 * distance;
    const cross = (x: number, y: number, k: number) => {
      const sx = x - m.origin[0], sy = y - m.origin[1];
      lines.push(sx - k, sy, z, sx + k, sy, z, sx, sy - k, z, sx, sy + k, z);
    };
    for (const p of t.points) cross(p[0], p[1], r * 0.6);
    if (t.hover) {
      cross(t.hover.at[0], t.hover.at[1], r);
      const last = t.points[t.points.length - 1];
      if (last) lines.push(last[0] - m.origin[0], last[1] - m.origin[1], z, t.hover.at[0] - m.origin[0], t.hover.at[1] - m.origin[1], z);
    }
    gl.bindBuffer(gl.ARRAY_BUFFER, markBuf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(lines), gl.DYNAMIC_DRAW);
    markN = lines.length / 3;
    dirty = true;
  };

  /** The camera ray through canvas CSS pixel (x, y): eye and direction, scene coordinates. */
  const rayAt = (x: number, y: number): number[] => {
    const { e, aspect, up } = matrices();
    const f = norm(sub(target, e)), s = norm(cross(f, up)), u = cross(s, f);
    const ndcX = (2 * x) / canvas.clientWidth - 1, ndcY = 1 - (2 * y) / canvas.clientHeight;
    const th = Math.tan(FOV_Y / 2);
    const dir = norm(add(f, add(scale(s, ndcX * th * aspect), scale(u, ndcY * th))));
    return [...e, ...dir];
  };
  const placingNow = () => placing.value?.active === true && mesh !== null;
  /** One tool call for the pointer at client (x, y); the answer is drawn. */
  const toolAt = (name: "pointer" | "click", cx: number, cy: number, mods: { shiftKey: boolean; ctrlKey: boolean; metaKey: boolean }) => {
    const r = canvas.getBoundingClientRect();
    const x = cx - r.left, y = cy - r.top;
    const args: ToolPointer = { ray: rayAt(x, y), ray_tol: rayAt(x + SNAP_PX, y), free: mods.shiftKey, ortho: mods.ctrlKey || mods.metaKey };
    stats.toolCalls++;
    const { state: t, error } = toolCall(name, args);
    stats.toolError = error;
    if (error) console.warn(`gl viewport: tool ${name}:`, error);
    if (t?.placed) stats.placed = t.placed;
    showTool(t);
  };
  // Pointer moves while placing: at most one tool call per frame (the latest position).
  let pendingMove: PointerEvent | null = null;
  stats.toScreen = (x, y, z) => {
    const m = mesh;
    if (!m) return null;
    const { mvp } = matrices();
    const p = [x - m.origin[0], y - m.origin[1], z - m.origin[2], 1];
    const c = [0, 1, 2, 3].map((r) => (mvp[r] ?? 0) * p[0]! + (mvp[4 + r] ?? 0) * p[1]! + (mvp[8 + r] ?? 0) * p[2]! + (mvp[12 + r] ?? 0) * p[3]!);
    if ((c[3] ?? 0) <= 0) return null;
    const rect = canvas.getBoundingClientRect();
    return [rect.left + ((c[0]! / c[3]! + 1) / 2) * canvas.clientWidth, rect.top + ((1 - c[1]! / c[3]!) / 2) * canvas.clientHeight];
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
    // Per element: the ray against its box first (slabs, padded by 1e-4 of the box size and 1 mm
    // against f32 rounding); only the triangles of an element whose box the ray enters before the
    // best hit so far. Same result as testing every triangle (elements in index order, a strictly
    // nearer hit replaces).
    const inv0 = 1 / dir[0], inv1 = 1 / dir[1], inv2 = 1 / dir[2];
    let hitE = -1;
    const box = clipBox();
    const outside = (d: number): boolean => {
      if (!box) return false;
      for (let k = 0; k < 3; k++) {
        const p = e[k]! + dir[k]! * d;
        if (p < (box.min[k] ?? 0) || p > (box.max[k] ?? 0)) return true;
      }
      return false;
    };
    for (let el = 0; el < m.ids.length; el++) {
      if (!shown[el]) continue; // hidden: not drawn, not picked (Reveal hidden: drawn and picked)
      const o = 6 * el;
      let tn = -Infinity, tf = Infinity;
      for (let ax = 0; ax < 3; ax++) {
        const lo = boxes[o + ax] ?? 0, hi = boxes[o + 3 + ax] ?? 0, pad = 1 + 1e-4 * (hi - lo);
        const inv = ax === 0 ? inv0 : ax === 1 ? inv1 : inv2;
        let t1 = (lo - pad - e[ax]!) * inv, t2 = (hi + pad - e[ax]!) * inv;
        if (Number.isNaN(t1) || Number.isNaN(t2)) { t1 = -Infinity; t2 = Infinity; } // parallel, on the slab edge
        if (t1 > t2) { const t = t1; t1 = t2; t2 = t; }
        if (t1 > tn) tn = t1;
        if (t2 < tf) tf = t2;
      }
      if (tn > tf || tf <= 0 || tn >= best) continue;
      const k0 = m.i0[el] ?? 0, k1 = k0 + (m.ni[el] ?? 0);
      for (let k = k0; k + 2 < k1; k += 3) {
        // Möller–Trumbore, two-sided.
        const a = vtx(I[k] ?? 0), b = vtx(I[k + 1] ?? 0), c = vtx(I[k + 2] ?? 0);
        const e1 = sub(b, a), e2 = sub(c, a), p = cross(dir, e2), det = dot(e1, p);
        if (Math.abs(det) < 1e-12) continue;
        const inv = 1 / det, tv = sub(e, a), uu = dot(tv, p) * inv;
        if (uu < 0 || uu > 1) continue;
        const q = cross(tv, e1), vv = dot(dir, q) * inv;
        if (vv < 0 || uu + vv > 1) continue;
        const d = dot(e2, q) * inv;
        if (d > 0 && d < best && !outside(d)) { // ADR 0014: what the box removes is not picked
          best = d;
          hit = k;
          hitE = el;
        }
      }
    }
    return hit < 0 ? -1 : hitE;
  };

  stats.pickAt = (cx, cy) => {
    const r = canvas.getBoundingClientRect();
    const k = pick(cx - r.left, cy - r.top);
    return k >= 0 ? mesh?.ids[k] ?? null : null;
  };

  new ResizeObserver(() => {
    const dpr = window.devicePixelRatio || 1;
    canvas.width = Math.max(1, Math.round(canvas.clientWidth * dpr));
    canvas.height = Math.max(1, Math.round(canvas.clientHeight * dpr));
    dirty = true;
  }).observe(canvas);

  let drag: { pan: boolean; box: boolean; x: number; y: number; x0: number; y0: number; moved: boolean } | null = null;
  /** One call of the C core's box tool for the pointer at client (x, y). */
  const boxAt = (name: "box_press" | "box_move" | "box_release", cx: number, cy: number) => {
    const r = canvas.getBoundingClientRect();
    const x = cx - r.left, y = cy - r.top;
    stats.boxCalls++;
    const { state: b, error } = boxTool(name, name === "box_press" ? { ray: rayAt(x, y), ray_tol: rayAt(x + SNAP_PX, y) } : { ray: rayAt(x, y) });
    stats.boxError = error;
    if (error) console.warn(`gl viewport: ${name}:`, error);
    preview = b?.active && b.box ? { min: b.box.min, max: b.box.max } : null;
    stats.boxFace = b?.active ? b.face : null;
    dirty = true;
    return b;
  };
  let pendingBox: PointerEvent | null = null;
  canvas.addEventListener("pointerdown", (e) => {
    canvas.setPointerCapture(e.pointerId);
    // A left press on a handle of the section box: the box tool holds that face (no orbit).
    const onHandle = e.button === 0 && !e.shiftKey && !placingNow() && clipBox() !== null && boxAt("box_press", e.clientX, e.clientY)?.active === true;
    drag = { pan: e.button === 1 || e.button === 2 || (e.button === 0 && e.shiftKey), box: onHandle, x: e.clientX, y: e.clientY, x0: e.clientX, y0: e.clientY, moved: false };
  });
  canvas.addEventListener("pointermove", (e) => {
    if (!drag) {
      if (placingNow()) pendingMove = e;
      return;
    }
    if (drag.box) {
      if (Math.hypot(e.clientX - drag.x0, e.clientY - drag.y0) > CLICK_PX) drag.moved = true;
      pendingBox = e;
      return;
    }
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
    if (d?.box) { // one session change: the released box
      pendingBox = null;
      if (e.type === "pointerup") boxAt("box_release", e.clientX, e.clientY);
      else { stats.boxCalls++; boxTool("box_cancel"); preview = null; stats.boxFace = null; dirty = true; }
      return;
    }
    if (e.type !== "pointerup" || !d || d.moved || e.button !== 0) return;
    if (placingNow()) { // a click while placing goes to the tool (Shift: free, no pan)
      pendingMove = null;
      toolAt("click", e.clientX, e.clientY, e);
      return;
    }
    if (d.pan) return;
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
  // Esc while placing: the tool cancels (stops placing, drops the points), as Rust's Esc.
  window.addEventListener("keydown", (e) => {
    if (e.key !== "Escape" || placing.value?.active !== true) return;
    const el = e.target as HTMLElement | null;
    if (el && (el.tagName === "INPUT" || el.tagName === "TEXTAREA" || el.isContentEditable)) return;
    stats.toolCalls++;
    const { state: t, error } = toolCall("cancel");
    stats.toolError = error;
    showTool(t);
  });
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
    if (visibility.value !== seenVis) { // a change (or the box tool): what is hidden, the box
      seenVis = visibility.value;
      vis = seenVis;
      sets = visibilitySets();
      dirty = true;
      const key = JSON.stringify(sets);
      if (key !== visKey) {
        visKey = key;
        drawLists();
      }
    }
    if (mesh && visKey === "\u0000") {
      visKey = JSON.stringify(sets);
      drawLists();
    }
    if (pendingBox && drag?.box) {
      const e = pendingBox;
      pendingBox = null;
      boxAt("box_move", e.clientX, e.clientY);
    }
    if (pendingMove) {
      const e = pendingMove;
      pendingMove = null;
      if (placingNow()) toolAt("pointer", e.clientX, e.clientY, e);
    }
    if (tool && placing.value?.active !== true) showTool(null); // placing stopped elsewhere
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
