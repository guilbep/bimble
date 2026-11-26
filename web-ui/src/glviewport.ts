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
// The box's edges and one arrowhead per face (ISS71: pointing out along the face's normal, constant size
// on screen, larger under the pointer) are drawn; a left press on an arrowhead is the C core's
// box tool (op tool box_press / box_move / box_release: drag state in C, one session change on release).
// Linked models (ticket B5, ccore/session/links.h): /mesh may end with the LNKS trailer, one BMSH per
// link. They are merged after the open model's elements, moved by (their origin - ours) computed in
// f64 (one scene origin, lib.rs:1499), drawn in Rust's reference colours (style.rs `reference`,
// `reference_edges`), not drawn when hidden (Manage links), never touched by the open model's
// visibility; a click on one selects it as `{link, ids}` (the properties show it read-only).
// Plan views (ticket C1, ccore/session/views.h, ADR 0068 rule 9): when the current tab is a plan
// (`open_views`, then the C core's `query view_clip`), the camera is orthographic, straight down
// (a ceiling plan: straight up, mirrored so east stays right, as Rust's reflected ceiling plan),
// north up; drags pan, the wheel zooms about the pointer, no orbit. What the plan shows is the
// clip box C gives (the model's bounds cut from the view depth to the cut plane), drawn with B3's
// passes: cut solids show filled at the cut shade, what lies below the cut in projection with its
// edges. Rays are orthographic (straight down from the pointer), so the place tool and picking work
// as in 3D. Each view keeps its camera; back to the 3D tab, the 3D camera comes back.
// Dimensions and texts (ticket C4, ccore/src/annot.h): in a plan, the view's annotations (`query
// view_annotations`, resolved in C: anchors on element features follow their element) are drawn on
// a 2D canvas over the scene: extension lines, the dimension line, 45° ticks, the value; texts at
// their point. Their size is Rust's paper size (2.25 mm for a dimension's value, the text's
// size_mm) times the view's scale, within 8..40 px (viewing.rs draw_annotations). A click on one
// selects it (not an element), Delete removes it (verb delete_annotation), a drag moves it
// (move_annotation: a text by the drag, a dimension's line across itself). While the annotate tool
// is on (bimble.ts startDimension / startText), pointer moves and clicks go to it (op tool, as the
// place tool) and its preview is drawn dashed; Esc stops it.
// View cube (ticket VC1, viewcube.ts, Rust viewcube.rs): top right of the 3D view, its own canvas
// (a click on it never picks); a click on a face, edge or corner turns the camera there in 0.3 s, a
// drag orbits, Home and Fit all under it; it follows project north (viewcube.ts projectNorth). In a
// plan the cube and Home are off, Fit all stays (ADR 0021).
// The measure tool (ticket T1, ccore/session/measure.h, gl/measure.ts): while it is on (action
// `measure`), pointer moves and clicks go to the C core (op tool measure_pointer / measure_click:
// the ray, the ray 12 px beside it; Alt: no snap, Shift: along an axis, Ctrl: go on from there),
// which picks, snaps to a corner, an edge or a face and keeps the measurements (session state);
// the overlay draws them, the rubber line and the snap marker, in 3D and in a plan. Esc: the first
// point goes, then the tool.
// Navigation (ticket NAV1, gl/nav.ts, Rust lib.rs wheel_pivot / orbit_pivot_at, bimble-render Camera):
// in 3D the wheel zooms about the geometry under the cursor, else about the plane through the orbit
// target facing the viewer, and keeps that pivot while only the wheel moves the camera; a left drag
// orbits about the geometry under the press, else the selection's centre, else the target (0.008
// rad/px, a marker at the pivot while it turns); the distance stays within 10 mm .. 1e8 mm; the clip
// planes come from the model's sphere seen from the eye; a double-click fits all (a plan: fits the
// plan). Plans keep their pan and zoom about the pointer.
// Sections (ticket SEC1, gl/section.ts): a section tab takes the plan's path with its own frame:
// `view_clip` gives the box (the model's bounds cut at the section's line, as deep as its depth,
// cropped) and the look `dir`; the camera is orthographic, looking along it from the near side of
// the cut, z up, screen right = the section's right (views.rs camera_angles, section_right). Cut
// solids show filled at the cut shade (B3's passes, no hatching), pan and zoom as a plan, pick and
// select work (rays along the look). The plan overlays (annotations, references, grid axes) stay
// plans'. What the view hides by class, family, storey or material (visibility_sets `by_view`) is
// not drawn. A building turned in its site (yaw != 0): a section view's box is not turned here (see SEC1's report);
// the 3D section box is (ISS71: its yaw, project or true north; the shader, the picks, edges and handles turn it).
import {
  actions, viewActions, // Q2: the view switches (edges, orthographic, fit, top)
  viewportKeys, // KEY1: Esc and Delete in the viewport (shell/keys.ts)
  pointer, coordinates, // KEY1: the status bar's pointer coordinates
  annotate, annotateCall, boxTool, deleteAnnotation, moveAnnotation, openViews, placing, select, selectLinked, state, toolCall,
  viewAnnotations, viewClip, visibility, visibilitySets, measure, measureCall, type MeasureState,
  type AnnotateState, type ToolPointer, type ToolState, type ViewAnnotation, type ViewAnnotations, type ViewClip, type Visibility,
  type VisibilitySets,
} from "./bimble";
import { drawMeasure } from "./gl/measure";
import { referenceLayer } from "./gl/reference"; // L1: reference plans
import { sectionMatrices, sectionFit, isSection } from "./gl/section"; // SEC1
import { gridLayer } from "./gl/grids"; // ---- G2: grid axes in plans ---- / ---- end G2 ----
import { clipPlanes, onTargetPlane, orbitAbout, ORBIT_RAD_PER_PX, sameCam, wheelFactor, zoomAt, type OrbitCam } from "./gl/nav";
import { HOME_PITCH, HOME_YAW, PITCH_LIMIT, projectNorth, startTurn, turnAt, viewCube, type CubeStats, type Dir, type Turn } from "./viewcube";

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
// Linked (reference) models: bimble-core style.rs:340-341 `reference`, `reference_edges`.
const REFERENCE_SRGB: [number, number, number] = [190, 214, 238];
const REFERENCE_EDGE_SRGB: [number, number, number] = [52, 110, 180];
// Revealed hidden elements (Reveal hidden): the comparison colour, as Rust's Style::OnlyHere.
const REVEALED_SRGB: [number, number, number] = [232, 150, 64];
// bimble-render Style::cut_shade: the brightness of cut surfaces (ADR 0014).
const CUT_SHADE = 0.72;
// The section box's edges and handles.
const BOX_SRGB: [number, number, number] = [40, 110, 200];
// ---- ISS71 ---- (issue #71) one arrowhead per face, pointing out along the face's normal: its length
// on screen (CSS px, constant), the press radius about the face centre (the box tool's ray_tol: 32 px
// across), its fill (the lab's "do" accent token, read each frame: the theme's) and its outline (the
// viewport's ground stays light in every theme, BACKGROUND_SRGB: a dark outline shows on it).
const ARROW_PX = 18;
const ARROW_HOVER = 1.3;
const ARROW_HIT_PX = 16;
const ARROW_FILL_FALLBACK = "#C06A41";
const ARROW_OUTLINE_CSS = "rgba(20, 20, 20, 0.9)";
// ---- end ISS71 ----
// The ghost of the place tool: translucent blue faces, its edges and the snap marker darker.
const GHOST_SRGB: [number, number, number] = [74, 128, 214];
const GHOST_EDGE_SRGB: [number, number, number] = [32, 70, 150];
const GHOST_ALPHA = 0.45;
const SNAP_PX = 12; // lib.rs placing_point: the snap tolerance is 12 screen pixels
const AMBIENT = 0.62;
// Camera defaults (bimble-render lib.rs Camera::default): yaw -55°, pitch 28°, vertical fov 35°.
const FOV_Y = (35 * Math.PI) / 180;
const CLICK_PX = 4;
// Annotations (viewing.rs draw_annotations): the dimension colour, the selected one's, the preview's.
const ANNOT_CSS = "rgb(32, 70, 150)";
const ANNOT_SEL_CSS = "rgb(200, 60, 90)";
const ANNOT_PREVIEW_CSS = "rgba(32, 70, 150, 0.7)";
const DIM_TEXT_MM = 2.25; // a dimension's value without a dimension type (paper mm)
const ANNOT_PICK_PX = 6;
// The orbit pivot's marker (lib.rs handle_view_input: a 4 px disc in the selection fill, white ring).
const NAV_PIVOT_CSS = "rgb(31, 78, 140)";

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
  /** Bytes of this BMSH block. */
  size: number;
  /** Per element: 0 the open model, k + 1 the link k (merged meshes only). */
  model?: Uint8Array;
  /** The links of a merged mesh, in link order. */
  links?: { name: string; visible: boolean; elements: number }[];
}

/** Parses BMSH version 1 (ccore/session/mesh.c) at byte `at`; `exact`: it must end the buffer.
 *  Throws on a malformed buffer. */
export function parseBmsh(ab: ArrayBuffer, at = 0, exact = true): Bmsh {
  const dv = new DataView(ab);
  if (ab.byteLength < at + 52 || dv.getUint32(at, true) !== 0x48534d42) throw new Error("not BMSH");
  if (dv.getUint32(at + 4, true) !== 1) throw new Error(`BMSH version ${dv.getUint32(at + 4, true)}`);
  const u = (o: number) => dv.getUint32(at + o, true);
  const rev = u(8), nE = u(12), nV = u(16), nI = u(20), nS = u(24);
  const size = 52 + 48 * nE + 12 * nV + 4 * nI + 32 * nS;
  if (exact ? size !== ab.byteLength - at : size > ab.byteLength - at) throw new Error(`BMSH size ${ab.byteLength - at}, expected ${size}`);
  const origin: [number, number, number] = [dv.getFloat64(at + 28, true), dv.getFloat64(at + 36, true), dv.getFloat64(at + 44, true)];
  const ids: string[] = [];
  const cats = new Uint8Array(nE), v0 = new Uint32Array(nE), nv = new Uint32Array(nE), i0 = new Uint32Array(nE), ni = new Uint32Array(nE);
  const s0 = new Uint32Array(nE), ns = new Uint32Array(nE);
  const bytes = new Uint8Array(ab, at);
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
  let o = at + 52 + 48 * nE;
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
  return { rev, origin, ids, cats, v0, nv, i0, ni, s0, ns, pos, idx, edges, normals, size };
}

// ---- TEST1: an empty BMSH (no element; what the host answers with no model open) ----
const EMPTY_BMSH = (): ArrayBuffer => {
  const ab = new ArrayBuffer(52), dv = new DataView(ab);
  dv.setUint32(0, 0x48534d42, true); // "BMSH"
  dv.setUint32(4, 1, true); // version 1; rev, counts and origin 0
  return ab;
};
// ---- end TEST1 ----

/** GET /mesh: the open model's BMSH, then (with links) the LNKS trailer of ccore/session/links.h,
 *  merged into one mesh: the links' elements after ours (`model` k + 1), moved by their origin minus
 *  ours (f64, then f32). Without a trailer: the BMSH as parseBmsh gives it. */
export function parseMesh(ab: ArrayBuffer): Bmsh {
  const main = parseBmsh(ab, 0, false);
  if (main.size === ab.byteLength) return main;
  const dv = new DataView(ab);
  let o = main.size;
  if (ab.byteLength < o + 12 || dv.getUint32(o, true) !== 0x534b4e4c) throw new Error("BMSH: trailing bytes, not LNKS");
  if (dv.getUint32(o + 4, true) !== 1) throw new Error(`LNKS version ${dv.getUint32(o + 4, true)}`);
  const n = dv.getUint32(o + 8, true);
  o += 12;
  const parts: { name: string; visible: boolean; m: Bmsh }[] = [];
  const dec = new TextDecoder();
  for (let k = 0; k < n; k++) {
    if (ab.byteLength < o + 4) throw new Error("LNKS truncated");
    const L = dv.getUint32(o, true);
    if (ab.byteLength < o + 12 + L) throw new Error("LNKS truncated");
    const name = dec.decode(new Uint8Array(ab, o + 4, L));
    o += 4 + L + ((4 - (L % 4)) % 4);
    const flags = dv.getUint32(o, true), B = dv.getUint32(o + 4, true);
    o += 8;
    if (ab.byteLength < o + B) throw new Error("LNKS truncated");
    const m = parseBmsh(ab, o, false);
    if (m.size !== B) throw new Error(`LNKS ${name}: BMSH size ${B}, expected ${m.size}`);
    o += B;
    parts.push({ name, visible: (flags & 1) === 1, m });
  }
  if (o !== ab.byteLength) throw new Error(`LNKS: ${ab.byteLength - o} trailing bytes`);
  const all = [main, ...parts.map((p) => p.m)];
  const sum = (f: (m: Bmsh) => number) => all.reduce((a, m) => a + f(m), 0);
  const nE = sum((m) => m.ids.length), nV = sum((m) => m.pos.length / 3), nI = sum((m) => m.idx.length), nS = sum((m) => m.edges.length / 6);
  const out: Bmsh = {
    rev: main.rev, origin: main.origin, ids: [], cats: new Uint8Array(nE), v0: new Uint32Array(nE), nv: new Uint32Array(nE),
    i0: new Uint32Array(nE), ni: new Uint32Array(nE), s0: new Uint32Array(nE), ns: new Uint32Array(nE),
    pos: new Float32Array(3 * nV), idx: new Uint32Array(nI), edges: new Float32Array(6 * nS), normals: new Int8Array(8 * nS),
    size: ab.byteLength, model: new Uint8Array(nE), links: parts.map((p) => ({ name: p.name, visible: p.visible, elements: p.m.ids.length })),
  };
  let e0 = 0, v0 = 0, i0 = 0, s0 = 0;
  all.forEach((m, k) => {
    const d = [m.origin[0] - main.origin[0], m.origin[1] - main.origin[1], m.origin[2] - main.origin[2]];
    for (let e = 0; e < m.ids.length; e++) {
      out.ids.push(m.ids[e] ?? "");
      out.cats[e0 + e] = m.cats[e] ?? 7;
      out.v0[e0 + e] = (m.v0[e] ?? 0) + v0, out.nv[e0 + e] = m.nv[e] ?? 0;
      out.i0[e0 + e] = (m.i0[e] ?? 0) + i0, out.ni[e0 + e] = m.ni[e] ?? 0;
      out.s0[e0 + e] = (m.s0[e] ?? 0) + s0, out.ns[e0 + e] = m.ns[e] ?? 0;
      out.model![e0 + e] = k;
    }
    for (let i = 0; i < m.pos.length; i++) out.pos[3 * v0 + i] = (m.pos[i] ?? 0) + (d[i % 3] ?? 0);
    for (let i = 0; i < m.idx.length; i++) out.idx[i0 + i] = (m.idx[i] ?? 0) + v0;
    for (let i = 0; i < m.edges.length; i++) out.edges[6 * s0 + i] = (m.edges[i] ?? 0) + (d[i % 3] ?? 0);
    out.normals.set(m.normals, 8 * s0);
    e0 += m.ids.length, v0 += m.pos.length / 3, i0 += m.idx.length, s0 += m.edges.length / 6;
  });
  return out;
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
uniform float u_eye_dir; // 1: u_eye is the direction to the eye (an orthographic plan camera)
uniform float u_point;
uniform float u_zbias; // NDC depth pulled toward the eye (a plan's edges: over a coplanar cut face)
out vec3 v_pos;
out vec3 v_col;
flat out float v_back;
void main() {
  v_pos = a_pos; v_col = a_col;
  vec3 to_eye = u_eye_dir > 0.5 ? u_eye : u_eye - a_pos;
  v_back = (dot(a_n1.xyz, to_eye) < 0.0 && dot(a_n2.xyz, to_eye) < 0.0) ? 1.0 : 0.0;
  gl_PointSize = u_point;
  gl_Position = u_mvp * vec4(a_pos, 1.0);
  gl_Position.z -= u_zbias * gl_Position.w;
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
uniform vec2 u_clip_rot; // ISS71: (cos, sin) of the box's yaw: the fragment turned by -yaw into its frame
uniform float u_cut;     // brightness of cut surfaces (back faces while the box is on)
uniform float u_ambient; // < 0: unlit (edges)
uniform float u_alpha;
out vec4 o;
vec3 to_linear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c)); }
vec3 to_srgb(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c)); }
void main() {
  vec3 q = vec3(u_clip_rot.x * v_pos.x + u_clip_rot.y * v_pos.y, u_clip_rot.x * v_pos.y - u_clip_rot.y * v_pos.x, v_pos.z);
  if (u_clip == 1 && (any(lessThan(q, u_clip_min)) || any(greaterThan(q, u_clip_max)))) discard;
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
  /** ISS71: right-click menus opened; the section box's arrowheads as last drawn (canvas CSS px: the
   *  face centre, the outward direction on screen), the face under the pointer. */
  menus: number;
  boxHandles: { face: number; x: number; y: number; dx: number; dy: number }[];
  boxHover: number | null;
  /** Test hook: the GlobalId a click at client (x, y) would pick, or null (selects nothing). */
  pickAt?: (x: number, y: number) => string | null;
  /** Test hook: what a click at client (x, y) would pick: its id and its link (null: the open model). */
  pickInfoAt?: (x: number, y: number) => { id: string; link: string | null } | null;
  /** Linked models in the mesh (ccore/session/links.h): their names, elements and triangles drawn. */
  links: { name: string; visible: boolean; elements: number }[];
  linkTris: number;
  /** Test hook: a world point (mm) -> CSS px in the viewport (clientX, clientY), or null. */
  toScreen?: (x: number, y: number, z: number) => [number, number] | null;
  /** Dimensions and texts (ticket C4): the view's annotations as last fetched, the strings the
   *  overlay drew in its last frame, the selected annotation, the annotate tool's state and calls,
   *  the last annotation error. */
  annotations: ViewAnnotations | null;
  overlayTexts: string[];
  selectedAnnotation: string | null;
  annotate: AnnotateState | null;
  annotateCalls: number;
  annotError: string | null;
  /** The GlobalId the annotate tool's last click added. */
  annotAdded: string | null;
  /** Test hook: where an annotation is drawn (client px): a dimension's line middle, a text's point. */
  annotationScreen?: (id: string) => [number, number] | null;
  /** The camera: "3d" (orbit, perspective) or "plan" (orthographic, top-down), the view shown, the
   *  plan's half height in mm, the camera's yaw and pitch (radians), its distance (mm) and whether a
   *  view-cube turn is running. */
  camera?: { mode: "3d" | "plan"; view: string | null; look: string | null; half: number; yaw: number; pitch: number; target: number[];
    distance: number; turning: boolean };
  /** The last view-cube turn as drawn: one entry per frame (ms since the turn started, angles,
   *  arrived); `n` counts the turns started. Read in the page, for the tests (KEY1). */
  turnLog?: { n: number; frames: { ms: number; yaw: number; pitch: number; done: boolean }[] };
  /** Test hook: the turns' clock runs at this rate (1: real time; 0.1: ten times slower). */
  clockScale?: number;
  /** KEY1: `coordinates` queries for the status bar's pointer readout (one per frame at most, the pointer moved). */
  pointerQueries: number;
  /** The view cube (viewcube.ts): shown, hovered target, clicks and drags. */
  viewCube?: CubeStats;
  /** Test hooks: a cube target's cell centre, a compass letter (client px), the target under client (x, y). */
  cubeCellAt?: (d: Dir) => [number, number] | null;
  cubeLetterAt?: (t: "N" | "E" | "S" | "W") => [number, number] | null;
  cubeHitAt?: (x: number, y: number) => Dir | null;
  /** The measure tool (ticket T1): its state as last answered, calls made, the last error. */
  measure: MeasureState | null;
  measureCalls: number;
  measureError: string | null;
  /** Navigation (ticket NAV1): pivot picks made (a test hook: the wheel keeps its pivot without
   *  picking again), the wheel's last pivot and the current drag's pivot (world mm; null: none) and
   *  what they came from, double-click fits. */
  nav: { picks: number; zoomPivot: number[] | null; zoomFrom: "hit" | "plane" | null; orbitPivot: number[] | null;
    orbitFrom: "hit" | "selection" | "target" | null; fits: number };
  /** Test hooks (NAV1): the eye (world mm, 3D), an element's box (world mm) by GlobalId (the open model). */
  navEye?: () => number[] | null;
  elementBox?: (id: string) => { min: number[]; max: number[] } | null;
  /** Q2: the ribbon's switches as drawn: edges on, the 3D camera orthographic, edge segments drawn last frame. */
  q2?: { edges: boolean; ortho: boolean; edgeSegs: number };
  /** BUG1: frame loops run (each one takes the page's state and draws it when it changed), and what the last
   *  draw drew: its frame number, the mesh's rev, the buffer size. A test reads the pixels after a loop that
   *  began after its action, with the state's rev drawn at the canvas's layout size (no fixed sleep). */
  loops?: number;
  drawn?: { frame: number; rev: number | null; w: number; h: number };
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
    hiddenElements: 0, drawnTris: 0, clip: null, boxFace: null, boxCalls: 0, boxError: null, menus: 0, boxHandles: [], boxHover: null, links: [], linkTris: 0,
    annotations: null, overlayTexts: [], selectedAnnotation: null, annotate: null, annotateCalls: 0, annotError: null, annotAdded: null,
    measure: null, measureCalls: 0, measureError: null,
    nav: { picks: 0, zoomPivot: null, zoomFrom: null, orbitPivot: null, orbitFrom: null, fits: 0 }, pointerQueries: 0 };
  window.__bbGl = stats;
  if (!(canvas instanceof HTMLCanvasElement)) return () => undefined;
  const gl = canvas.getContext("webgl2", { antialias: false, preserveDrawingBuffer: true });
  if (!gl) {
    stats.error = "WebGL2 is not available";
    console.warn("gl viewport:", stats.error);
    return () => undefined;
  }
  const t = encodeURIComponent(token);
  // The 2D overlay for dimensions and texts: a canvas over #bimble, not hit by the pointer.
  const overlay = document.createElement("canvas");
  overlay.setAttribute("aria-hidden", "true");
  overlay.dataset.bbOverlay = "annotations";
  overlay.style.cssText = "position:absolute;inset:0;width:100%;height:100%;pointer-events:none;display:block";
  canvas.insertAdjacentElement("afterend", overlay);
  const octx = overlay.getContext("2d");
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
  const uEyeDir = gl.getUniformLocation(prog, "u_eye_dir");
  const uPoint = gl.getUniformLocation(prog, "u_point");
  const uClip = gl.getUniformLocation(prog, "u_clip");
  const uClipMin = gl.getUniformLocation(prog, "u_clip_min");
  const uClipMax = gl.getUniformLocation(prog, "u_clip_max");
  const uClipRot = gl.getUniformLocation(prog, "u_clip_rot"); // ISS71
  const uCut = gl.getUniformLocation(prog, "u_cut");
  const uZbias = gl.getUniformLocation(prog, "u_zbias");

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
  let faceN = 0, faceMainN = 0, edgeN = 0, edgeMainN = 0;
  // Visibility (query visibility and visibility_sets) and the box drag's preview.
  let vis: Visibility | null = null;
  let sets: VisibilitySets | null = null;
  let seenVis: Visibility | null | undefined;
  let visKey = "\u0000";
  /** Per element of the mesh: 0 not drawn, 1 drawn, 2 drawn revealed (hidden, Reveal hidden on). */
  let shown = new Uint8Array(0);
  let preview: { min: number[]; max: number[]; yaw?: number } | null = null; // ISS71: yaw, the box's turn
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
  // The view shown (open_views current, view_clip): a plan makes the camera orthographic.
  let plan: ViewClip | null = null;
  let viewId: string | null = null;
  let seenViews: unknown;
  let half = 5000; // a plan's half height on screen, mm
  /** Each view's camera, kept while another is shown (viewing.rs view_cameras). */
  const cams = new Map<string, { yaw: number; pitch: number; distance: number; target: V3; half: number }>();
  const mirror = () => plan?.look === "up";
  // ---- Q2: the ribbon's view switches (session state in the C core, `actions` pressed) ----
  const pressed = (name: string, dflt: boolean) => actions.value.find((a) => a.name === name)?.pressed ?? dflt;
  const ortho3d = () => !plan && pressed("orthographic", false);
  let seenActions: unknown;
  // ---- end Q2 ----
  let selKey = "";
  // Camera (bimble-render Camera): orbit around `target` at `distance`, z up.
  let yaw = HOME_YAW + projectNorth(), pitch = HOME_PITCH, distance = 10000;
  /** A view-cube turn in progress (lib.rs `turn`). */
  let turn: Turn | null = null;
  // KEY1: each turn started is logged frame by frame (stats.turnLog); its clock is stats.clockScale's.
  const turnTo = (y: number, p: number): Turn => {
    stats.turnLog = { n: (stats.turnLog?.n ?? 0) + 1, frames: [] };
    return startTurn(yaw, pitch, y, p, performance.now());
  };
  let target: V3 = [0, 0, 0];
  let dirty = true;
  let loading = false;
  // What the mesh shows: the model's rev and the links' counter (change JSON `links`).
  const meshKey = () => `${state.value.rev}/${state.value.links ?? 0}`;
  let wantRev: string | null = null;
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
    if (isSection(plan)) return sectionMatrices(plan, target, r, half, aspect); // SEC1
    if (plan) { // orthographic, straight down (up: from below, mirrored), north up
      const h = 4 * r + 1000, up: V3 = [0, 1, 0];
      const e = add(target, [0, 0, plan.look === "up" ? -h : h]);
      const view = lookAt(e, target, up);
      const near = 1, far = 2 * h, w = half * aspect, m = mirror() ? -1 : 1;
      const proj = new Float32Array([m / w, 0, 0, 0, 0, 1 / half, 0, 0, 0, 0, -2 / (far - near), 0, 0, 0, -(far + near) / (far - near), 1]);
      return { e, view, proj, mvp: mul(proj, view), aspect, up };
    }
    // ---- navigation (NAV1): clip planes from the model's sphere seen from the eye (Camera::view_proj) ----
    const { near, far } = clipPlanes({ yaw, pitch, distance, target }, bounds?.center ?? target, r);
    const e = eye();
    const up: V3 = Math.abs(pitch) > 1.55 ? [-Math.cos(yaw), -Math.sin(yaw), 0] : [0, 0, 1];
    const view = lookAt(e, target, up);
    if (ortho3d()) { // Q2: bimble-render lib.rs:281-284: h = distance tan(fov/2), depth d ± r (near may be behind the eye)
      const h = distance * Math.tan(FOV_Y / 2), w = h * aspect, rr = r * 1.01;
      const d = dot(sub(bounds?.center ?? target, e), norm(sub(target, e)));
      const n = d - rr, f = Math.max(d + rr, 1);
      const proj = new Float32Array([1 / w, 0, 0, 0, 0, 1 / h, 0, 0, 0, 0, -2 / (f - n), 0, 0, 0, -(f + n) / (f - n), 1]);
      return { e, view, proj, mvp: mul(proj, view), aspect, up };
    }
    const proj = perspective(FOV_Y, aspect, near, far);
    return { e, view, proj, mvp: mul(proj, view), aspect, up };
  };
  /** A plan fitted: the clip's x, y on screen (the model's bounds), its middle height as target. */
  const fitPlan = () => {
    const c = plan?.clip;
    if (!c) { if (bounds) target = [...bounds.center], half = bounds.radius; return; }
    if (isSection(plan)) { ({ target, half } = sectionFit(plan, c, Math.max(1e-3, canvas.width / Math.max(1, canvas.height)))); return; } // SEC1
    const aspect = Math.max(1e-3, canvas.width / Math.max(1, canvas.height));
    target = [((c.min[0] ?? 0) + (c.max[0] ?? 0)) / 2, ((c.min[1] ?? 0) + (c.max[1] ?? 0)) / 2, ((c.min[2] ?? 0) + (c.max[2] ?? 0)) / 2];
    half = 1.05 * Math.max(((c.max[1] ?? 0) - (c.min[1] ?? 0)) / 2, ((c.max[0] ?? 0) - (c.min[0] ?? 0)) / 2 / aspect, 100);
  };
  /** The view shown changed (a tab): keep the camera of the one left, take the new one's. */
  const switchView = (vc: ViewClip | null, id: string | null) => {
    if (id === viewId) {
      plan = vc?.type === "plan" || vc?.type === "section" ? vc : null; // the same view, its range or the model changed (SEC1: a section too)
      dirty = true;
      return;
    }
    turn = null;
    if (viewId !== null) cams.set(viewId, { yaw, pitch, distance, target: [...target], half });
    viewId = id;
    plan = vc?.type === "plan" || vc?.type === "section" ? vc : null; // SEC1: a section too
    const c = id === null ? undefined : cams.get(id);
    if (c) ({ yaw, pitch, distance, half } = c), (target = [...c.target]);
    else if (plan) fitPlan();
    else if (fitted) { yaw = HOME_YAW + projectNorth(); pitch = HOME_PITCH; fit(); }
    preview = null;
    dirty = true;
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
    const linked = state.value.linked ?? null;
    // A linked element selected: its id is the selection; the open model's same id is not selected.
    const sel = new Set(linked ? [] : state.value.selection);
    const out = new Uint8Array(m.pos.length);
    for (let e = 0; e < m.ids.length; e++) {
      const k = m.model?.[e] ?? 0;
      const c = k > 0
        ? (linked && linked.id === m.ids[e] && linked.link === m.links?.[k - 1]?.name ? SELECTION_SRGB : REFERENCE_SRGB)
        : sel.has(m.ids[e] ?? "") ? SELECTION_SRGB : shown[e] === 2 ? REVEALED_SRGB
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
      if (plan) fitPlan();
      else fit();
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
      const m = parseMesh(ab);
      // ---- TEST1: the model closed while this answer was on its way: not drawn (the frame loop cleared the view) ----
      if (state.value.file === null) return;
      // ---- end TEST1 ----
      stats.meshRev = m.rev;
      stats.links = m.links ?? [];
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
    const byView = new Set(st.by_view ?? []); // SEC1: the view's class, family, storey, material lists
    const hid = new Set(st.hidden_in_view), iso = new Set(st.isolated_in_view), now = st.now ? new Set(st.now.ids) : null;
    const isolateNow = st.now?.isolate === true;
    return (id: string) => hid.has(id) || byView.has(id) || (iso.size > 0 && !iso.has(id)) || (now !== null && now.has(id) !== isolateNow);
  };
  /** The draw lists: the triangles and edge segments of the elements shown. */
  const drawLists = () => {
    const m = mesh;
    if (!m) return;
    const hidden = hiddenTest(), reveal = sets?.reveal === true;
    shown = new Uint8Array(m.ids.length);
    let nf = 0, ne = 0, nh = 0, lf = 0;
    for (let e = 0; e < m.ids.length; e++) {
      const lk = m.model?.[e] ?? 0;
      if (lk > 0) { // a linked element: its link shown or not; the open model's visibility is not its
        shown[e] = m.links?.[lk - 1]?.visible ? 1 : 0;
        if (shown[e]) nf += m.ni[e] ?? 0, ne += m.ns[e] ?? 0, lf += m.ni[e] ?? 0;
        continue;
      }
      const h = hidden(m.ids[e] ?? "");
      shown[e] = !h ? 1 : reveal ? 2 : 0;
      if (shown[e]) nf += m.ni[e] ?? 0, ne += m.ns[e] ?? 0;
      else nh++;
    }
    const fi = new Uint32Array(nf), ei = new Uint32Array(2 * ne);
    let f = 0, k = 0;
    // Edges: the open model's first, then the links' (drawn in their own colour).
    for (const links of [false, true]) {
      for (let e = 0; e < m.ids.length; e++) {
        if (!shown[e] || ((m.model?.[e] ?? 0) > 0) !== links) continue;
        const i0 = m.i0[e] ?? 0, n = m.ni[e] ?? 0;
        fi.set(m.idx.subarray(i0, i0 + n), f);
        f += n;
        const s0 = m.s0[e] ?? 0, ns = m.ns[e] ?? 0;
        for (let q = s0; q < s0 + ns; q++) ei[k++] = 2 * q, ei[k++] = 2 * q + 1;
      }
      if (!links) edgeMainN = k, faceMainN = f;
    }
    stats.linkTris = lf / 3;
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
  const clipBox = (): { min: number[]; max: number[]; yaw?: number } | null => {
    if (plan) return plan.clip; // a plan: what its range shows (the section box is the 3D view's)
    if (preview) return preview;
    const sb = vis?.section_box;
    return sb?.on && sb.box ? { min: sb.box.min, max: sb.box.max, yaw: sb.box.yaw } : null; // ISS71: yaw
  };
  // ---- ISS71 ---- the box's frame: a point of it in the scene (ClipBox::from_frame, turned by yaw
  // about the vertical through the scene origin) and back (to_frame).
  const turnZ = (p: V3, a: number): V3 => {
    if (!a) return p;
    const c = Math.cos(a), s = Math.sin(a);
    return [c * p[0] - s * p[1], s * p[0] + c * p[1], p[2]];
  };
  /** A scene point -> canvas CSS px, or null behind the eye. */
  const sceneToLocal = (p: V3): [number, number] | null => {
    const { mvp } = matrices();
    const c = [0, 1, 3].map((r) => (mvp[r] ?? 0) * p[0] + (mvp[4 + r] ?? 0) * p[1] + (mvp[8 + r] ?? 0) * p[2] + (mvp[12 + r] ?? 0));
    if ((c[2] ?? 0) <= 0) return null;
    return [((c[0]! / c[2]! + 1) / 2) * canvas.clientWidth, ((1 - c[1]! / c[2]!) / 2) * canvas.clientHeight];
  };
  /** Each face's arrowhead: the face centre on screen and its outward normal's direction there. A face
   *  seen head on (its normal along the view) points up the screen; one behind the eye has none. */
  const boxHandles = (b: { min: number[]; max: number[]; yaw?: number }) => {
    const out: GlStats["boxHandles"] = [];
    const yaw = b.yaw ?? 0, diag = Math.hypot(...[0, 1, 2].map((k) => (b.max[k] ?? 0) - (b.min[k] ?? 0)));
    for (let f = 0; f < 6; f++) {
      const c = faceCenter(b, f), n: V3 = [0, 0, 0];
      n[Math.floor(f / 2)] = f & 1 ? 1 : -1;
      const p = sceneToLocal(turnZ(c, yaw)), q = sceneToLocal(turnZ(add(c, scale(n, 0.01 * Math.max(1, diag))), yaw));
      if (!p) continue;
      let dx = q ? q[0] - p[0] : 0, dy = q ? q[1] - p[1] : -1;
      const l = Math.hypot(dx, dy);
      if (l < 1e-3) dx = 0, dy = -1;
      else dx /= l, dy /= l;
      out.push({ face: f, x: p[0], y: p[1], dx, dy });
    }
    return out;
  };
  /** The face whose arrowhead is under canvas px (x, y) (within ARROW_HIT_PX of its centre), or null. */
  const handleAt = (x: number, y: number): number | null => {
    let best: number | null = null, bd = ARROW_HIT_PX;
    for (const h of stats.boxHandles) {
      const d = Math.hypot(h.x - x, h.y - y);
      if (d <= bd) best = h.face, bd = d;
    }
    return best;
  };
  /** The arrowheads, on the overlay: a filled triangle per face, centred on the face centre. */
  const drawHandles = (ctx: CanvasRenderingContext2D) => {
    const fill = getComputedStyle(document.documentElement).getPropertyValue("--bb-clay").trim() || ARROW_FILL_FALLBACK;
    const held = stats.boxFace;
    for (const h of stats.boxHandles) {
      const hot = h.face === held || h.face === stats.boxHover;
      const L = ARROW_PX * (hot ? ARROW_HOVER : 1), px = -h.dy, py = h.dx;
      ctx.beginPath();
      ctx.moveTo(h.x + h.dx * L * 0.55, h.y + h.dy * L * 0.55);
      ctx.lineTo(h.x - h.dx * L * 0.45 + px * L * 0.45, h.y - h.dy * L * 0.45 + py * L * 0.45);
      ctx.lineTo(h.x - h.dx * L * 0.2, h.y - h.dy * L * 0.2);
      ctx.lineTo(h.x - h.dx * L * 0.45 - px * L * 0.45, h.y - h.dy * L * 0.45 - py * L * 0.45);
      ctx.closePath();
      ctx.fillStyle = fill;
      ctx.fill();
      ctx.lineJoin = "round";
      ctx.lineWidth = hot ? 2 : 1.25;
      ctx.strokeStyle = ARROW_OUTLINE_CSS;
      ctx.stroke();
    }
  };
  // ---- end ISS71 ----
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
    if (!m) { stats.boxHandles = []; drawOverlay(); return; } // ISS71: no handles
    const { e, mvp } = matrices();
    const light = norm(add(norm(sub(e, target)), [-0.3, -0.2, 0.9]));
    const box = clipBox();
    stats.clip = box;
    gl.useProgram(prog);
    gl.uniformMatrix4fv(uMvp, false, mvp);
    gl.uniform3fv(uLight, light);
    // A plan's eye is at infinity: the back-edge test takes its direction (a vertical side's edges stay).
    gl.uniform3fv(uEye, plan || ortho3d() ? norm(sub(e, target)) : e); // Q2: an orthographic 3D eye is at infinity too
    gl.uniform1f(uEyeDir, plan || ortho3d() ? 1 : 0);
    gl.uniform1f(uPoint, 1);
    gl.uniform1f(uAlpha, 1);
    gl.uniform1i(uClip, box ? 1 : 0);
    gl.uniform3fv(uClipMin, box ? box.min : [0, 0, 0]);
    gl.uniform3fv(uClipMax, box ? box.max : [0, 0, 0]);
    gl.uniform2f(uClipRot, Math.cos(box?.yaw ?? 0), Math.sin(box?.yaw ?? 0)); // ISS71: the turned box
    gl.uniform1f(uCut, CUT_SHADE);
    gl.uniform1f(uZbias, 0);
    gl.enable(gl.DEPTH_TEST);
    gl.depthFunc(gl.LEQUAL);
    gl.disable(gl.CULL_FACE);
    gl.bindVertexArray(vao);
    // Faces, pushed back so the edges on them win the depth test.
    gl.enable(gl.POLYGON_OFFSET_FILL);
    gl.uniform1f(uAmbient, AMBIENT);
    gl.bindBuffer(gl.ARRAY_BUFFER, posBuf);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
    gl.bindBuffer(gl.ARRAY_BUFFER, colBuf);
    gl.enableVertexAttribArray(1);
    gl.vertexAttribPointer(1, 3, gl.UNSIGNED_BYTE, true, 0, 0);
    // The open model's faces, then the links' pushed a hair behind (bimble-render LINKED_DEPTH_PUSH:
    // where both have a face, the model's wins).
    const faces = (push: number) => {
      gl.polygonOffset(1.5 + push, 2 + 2.4 * push); // (1.5, 2), and (4, 8) for the inward pass, as before
      gl.drawElements(gl.TRIANGLES, faceMainN, gl.UNSIGNED_INT, 0);
      gl.polygonOffset(2.5 + push, 16 + 2.4 * push);
      if (faceN > faceMainN) gl.drawElements(gl.TRIANGLES, faceN - faceMainN, gl.UNSIGNED_INT, 4 * faceMainN);
    };
    if (!box) faces(0);
    else {
      // ADR 0014: outward faces lit, then inward faces (seen only through the cut) unlit at the cut
      // shade, pushed back so silhouette corners keep the outward face.
      // A mirrored camera (a ceiling plan) turns the winding: the faces seen from outside are then
      // the back-facing ones.
      gl.enable(gl.CULL_FACE);
      gl.frontFace(mirror() ? gl.CW : gl.CCW);
      gl.cullFace(gl.BACK);
      faces(0);
      gl.cullFace(gl.FRONT);
      // In a plan the cut faces are pulled ahead instead: a column cut above its footing shows its
      // cut where it stands on the footing's top (coplanar), as a plan draws a cut over what lies below.
      faces(plan ? -3 : 2.5);
      gl.frontFace(gl.CCW);
      gl.disable(gl.CULL_FACE);
    }
    gl.disable(gl.POLYGON_OFFSET_FILL);
    // Edges: their VAO (with the face normals for the back-edge test), a constant colour.
    gl.uniform1f(uAmbient, -1);
    gl.bindVertexArray(edgeVao);
    gl.vertexAttrib3f(1, EDGE_SRGB[0]! / 255, EDGE_SRGB[1]! / 255, EDGE_SRGB[2]! / 255);
    gl.depthMask(false);
    // A plan: edges 2 mm ahead, so the outline of a cut drawn where it stands on something stays.
    if (plan) gl.uniform1f(uZbias, (2 * 2) / (2 * (4 * (bounds?.radius ?? distance) + 1000) - 1));
    const edgesOn = pressed("edges", true); // Q2: ribbon.rs:695 Edges (opts.edges): no edge drawn when off
    stats.q2 = { edges: edgesOn, ortho: ortho3d(), edgeSegs: edgesOn ? edgeN / 2 : 0 };
    if (edgesOn) gl.drawElements(gl.LINES, edgeMainN, gl.UNSIGNED_INT, 0);
    if (edgesOn && edgeN > edgeMainN) { // linked models: their edges in the reference edge colour
      gl.vertexAttrib3f(1, REFERENCE_EDGE_SRGB[0] / 255, REFERENCE_EDGE_SRGB[1] / 255, REFERENCE_EDGE_SRGB[2] / 255);
      gl.drawElements(gl.LINES, edgeN - edgeMainN, gl.UNSIGNED_INT, 4 * edgeMainN);
    }
    gl.uniform1f(uZbias, 0);
    gl.depthMask(true);
    gl.bindVertexArray(null);
    gl.uniform1i(uClip, 0);
    if (box && !plan) drawBox(box);
    stats.boxHandles = box && !plan ? boxHandles(box) : []; // ISS71: drawn by drawOverlay
    drawGhost();
    drawOverlay();
    stats.frames++;
    if (stats.firstFrameMs === null) stats.firstFrameMs = performance.now();
  };

  /** The section box: its 12 edges, over the model (ISS71: turned by its yaw; the handles: arrowheads
   *  on the overlay, drawHandles). */
  const drawBox = (b: { min: number[]; max: number[]; yaw?: number }) => {
    const c = (i: number): number[] => turnZ([0, 1, 2].map((k) => ((i >> k) & 1 ? b.max[k] : b.min[k]) ?? 0) as V3, b.yaw ?? 0);
    const v: number[] = [];
    for (let i = 0; i < 8; i++)
      for (let k = 0; k < 3; k++) if (!((i >> k) & 1)) v.push(...c(i), ...c(i | (1 << k)));
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

  const refLayer = referenceLayer(); // L1: a 2D view's reference plans, under the annotations
  const gridAxesLayer = gridLayer(); // ---- G2: the grid axes of a plan, under the annotations ---- / ---- end G2 ----
  // ---- dimensions and texts (ticket C4) ----
  let anns: ViewAnnotations | null = null;
  let annKey = "";
  let selAnn: string | null = null;
  /** A drag of an annotation: its id, the start (client px) and the drag so far (world mm). */
  let annDrag: { id: string; x0: number; y0: number; by: [number, number] } | null = null;
  /** A world plan point (mm) -> overlay CSS px, or null. */
  const toLocal = (x: number, y: number): [number, number] | null => {
    const m = mesh;
    if (!m) return null;
    const { mvp } = matrices();
    const z = (plan?.elevation ?? 0) - m.origin[2];
    const p = [x - m.origin[0], y - m.origin[1], z, 1];
    const c = [0, 1, 3].map((r) => (mvp[r] ?? 0) * p[0]! + (mvp[4 + r] ?? 0) * p[1]! + (mvp[8 + r] ?? 0) * p[2]! + (mvp[12 + r] ?? 0) * p[3]!);
    if ((c[2] ?? 0) <= 0) return null;
    return [((c[0]! / c[2]! + 1) / 2) * canvas.clientWidth, ((1 - c[1]! / c[2]!) / 2) * canvas.clientHeight];
  };
  const pxPerMm = () => canvas.clientHeight / Math.max(1e-9, 2 * half);
  const fontPx = (sizeMm: number) => Math.min(40, Math.max(8, sizeMm * (anns?.scale ?? 100) * pxPerMm()));
  /** What the overlay draws of one annotation, in CSS px: segments, the text, its anchor and angle. */
  interface Shape { segs: [number, number, number, number][]; text: string; at: [number, number]; angle: number; px: number; base: "dim" | "text" }
  const shapeOf = (a: ViewAnnotation, by: [number, number] = [0, 0]): Shape | null => {
    const L = (p: [number, number]) => toLocal(p[0] + by[0], p[1] + by[1]);
    if (a.type === "text") {
      const at = L(a.at);
      return at ? { segs: [], text: a.text, at, angle: (-a.angle_deg * Math.PI) / 180, px: fontPx(a.size_mm), base: "text" } : null;
    }
    if (!a.qa || !a.qb || !a.mid || !a.dir) return null;
    const pa = L(a.a), pb = L(a.b), qa = L(a.qa), qb = L(a.qb), mid = L(a.mid), d2 = L([a.mid[0] + a.dir[0], a.mid[1] + a.dir[1]]);
    if (!pa || !pb || !qa || !qb || !mid || !d2) return null;
    let angle = Math.atan2(d2[1] - mid[1], d2[0] - mid[0]);
    if (Math.abs(angle) > Math.PI / 2 + 1e-9) angle += Math.PI; // upright (a ceiling plan is mirrored)
    const t = 4, c = Math.cos(angle), s = Math.sin(angle);
    // 45° ticks across the line at its ends (screen px).
    const tick = (p: [number, number]): [number, number, number, number] => {
      const k = [(c - s) * t, (s + c) * t];
      return [p[0] - k[0]!, p[1] - k[1]!, p[0] + k[0]!, p[1] + k[1]!];
    };
    return { segs: [[pa[0], pa[1], qa[0], qa[1]], [pb[0], pb[1], qb[0], qb[1]], [qa[0], qa[1], qb[0], qb[1]], tick(qa), tick(qb)],
      text: a.text, at: mid, angle, px: fontPx(DIM_TEXT_MM), base: "dim" };
  };
  const stroke = (ctx: CanvasRenderingContext2D, sh: Shape, css: string, dash: number[] = []) => {
    ctx.strokeStyle = css;
    ctx.fillStyle = css;
    ctx.lineWidth = 1;
    ctx.setLineDash(dash);
    ctx.beginPath();
    for (const g of sh.segs) { ctx.moveTo(g[0], g[1]); ctx.lineTo(g[2], g[3]); }
    ctx.stroke();
    ctx.setLineDash([]);
    if (!sh.text) return;
    ctx.save();
    ctx.translate(sh.at[0], sh.at[1]);
    ctx.rotate(sh.angle);
    ctx.font = `${sh.px.toFixed(1)}px sans-serif`;
    if (sh.base === "dim") { ctx.textAlign = "center"; ctx.textBaseline = "bottom"; ctx.fillText(sh.text, 0, -2); }
    else {
      ctx.textAlign = "left"; ctx.textBaseline = "alphabetic";
      sh.text.split("\n").forEach((line, i) => ctx.fillText(line, 0, i * sh.px * 1.25));
    }
    ctx.restore();
  };
  /** The annotation under overlay px (x, y), the last drawn first, or null. */
  const annotationAt = (x: number, y: number): string | null => {
    if (!plan || !anns || !octx) return null;
    for (const a of [...anns.items].reverse()) {
      const sh = shapeOf(a);
      if (!sh) continue;
      const segs = sh.base === "dim" ? sh.segs.slice(2, 3) : [];
      for (const g of segs) {
        const dx = g[2] - g[0], dy = g[3] - g[1], l2 = dx * dx + dy * dy;
        const u = l2 > 0 ? Math.min(1, Math.max(0, ((x - g[0]) * dx + (y - g[1]) * dy) / l2)) : 0;
        if (Math.hypot(x - g[0] - u * dx, y - g[1] - u * dy) <= ANNOT_PICK_PX) return a.id;
      }
      // the text's box, in its own axes
      octx.font = `${sh.px.toFixed(1)}px sans-serif`;
      const lines = sh.text.split("\n");
      const w = Math.max(sh.px, ...lines.map((l) => octx.measureText(l).width));
      const c = Math.cos(-sh.angle), s = Math.sin(-sh.angle), rx = x - sh.at[0], ry = y - sh.at[1];
      const lx = rx * c - ry * s, ly = rx * s + ry * c;
      const [x0, x1] = sh.base === "dim" ? [-w / 2, w / 2] : [0, w];
      const [y0, y1] = sh.base === "dim" ? [-sh.px - 2, 0] : [-sh.px, (lines.length - 1) * sh.px * 1.25 + 0.3 * sh.px];
      const p = ANNOT_PICK_PX / 2;
      if (lx >= x0 - p && lx <= x1 + p && ly >= y0 - p && ly <= y1 + p) return a.id;
    }
    return null;
  };
  stats.annotationScreen = (id) => {
    const a = anns?.items.find((x) => x.id === id);
    const sh = a ? shapeOf(a) : null;
    if (!a || !sh) return null;
    const r = canvas.getBoundingClientRect();
    if (a.type === "dimension" && a.qa && a.qb) {
      const qa = toLocal(...a.qa), qb = toLocal(...a.qb);
      if (qa && qb) return [r.left + (qa[0] + qb[0]) / 2, r.top + (qa[1] + qb[1]) / 2];
    }
    return [r.left + sh.at[0] + 2, r.top + sh.at[1] - sh.px / 3];
  };
  const drawOverlay = () => {
    const dpr = window.devicePixelRatio || 1;
    const w = Math.max(1, Math.round(canvas.clientWidth * dpr)), h = Math.max(1, Math.round(canvas.clientHeight * dpr));
    if (overlay.width !== w || overlay.height !== h) { overlay.width = w; overlay.height = h; }
    const ctx = octx;
    if (!ctx) return;
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.clearRect(0, 0, overlay.width, overlay.height);
    // NAV1: the orbit pivot's marker cleared again by its own square: headless Chromium was seen to
    // keep it on screen after the whole-canvas clear above when nothing else was drawn.
    if (navMark) ctx.clearRect((navMark[0] - 8) * dpr, (navMark[1] - 8) * dpr, 16 * dpr, 16 * dpr), (navMark = null);
    const texts: string[] = [];
    if (plan && !isSection(plan) && mesh) { // SEC1: the plan overlays are plans'
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
      refLayer.draw(ctx, toLocal); // L1
      gridAxesLayer.draw(ctx, toLocal); // ---- G2 ---- / ---- end G2 ----
      for (const a of anns?.items ?? []) {
        const moving = annDrag?.id === a.id;
        const sh = shapeOf(a, moving ? annDrag!.by : [0, 0]);
        if (!sh) continue;
        stroke(ctx, sh, a.id === selAnn ? ANNOT_SEL_CSS : ANNOT_CSS);
        if (sh.text) texts.push(sh.text);
      }
      const st = stats.annotate;
      if (st?.active && st.preview) {
        const p = st.preview;
        const sh = shapeOf({ ...p, type: "dimension", id: "", kind: null, axis: "aligned", anchors: [] as never } as ViewAnnotation);
        if (sh) stroke(ctx, sh, ANNOT_PREVIEW_CSS, [4, 3]);
        if (sh?.text) texts.push(sh.text);
      }
      if (st?.active && st.hover) { // the pointer: a cross, a circle when on an element's feature
        const q = toLocal(...st.hover.at);
        if (q) {
          ctx.strokeStyle = ANNOT_CSS;
          ctx.beginPath();
          ctx.moveTo(q[0] - 6, q[1]); ctx.lineTo(q[0] + 6, q[1]); ctx.moveTo(q[0], q[1] - 6); ctx.lineTo(q[0], q[1] + 6);
          if (st.anchor) { ctx.moveTo(q[0] + 5, q[1]); ctx.arc(q[0], q[1], 5, 0, 2 * Math.PI); }
          ctx.stroke();
        }
      }
    }
    // ---- the measure tool (ticket T1): its measurements and pointer, in 3D and in a plan ----
    if (mesh && stats.measure) {
      const b = canvas.getBoundingClientRect();
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
      drawMeasure(ctx, stats.measure, (x, y, z) => {
        const p = stats.toScreen?.(x, y, z);
        return p ? [p[0] - b.left, p[1] - b.top] : null;
      }, texts);
    }
    if (stats.boxHandles.length) { ctx.setTransform(dpr, 0, 0, dpr, 0, 0); drawHandles(ctx); } // ISS71
    // ---- navigation (NAV1): where the drag turns the view about (lib.rs handle_view_input) ----
    const op = drag?.orbit;
    if (op && mesh && !plan) {
      const b = canvas.getBoundingClientRect(), o = mesh.origin;
      const q = stats.toScreen?.(op[0] + o[0], op[1] + o[1], op[2] + o[2]);
      if (q) {
        ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
        ctx.beginPath();
        navMark = [q[0] - b.left, q[1] - b.top];
        ctx.arc(navMark[0], navMark[1], 4, 0, 2 * Math.PI);
        ctx.fillStyle = NAV_PIVOT_CSS;
        ctx.fill();
        ctx.strokeStyle = "white";
        ctx.lineWidth = 1.5;
        ctx.stroke();
        ctx.beginPath();
      }
    }
    stats.overlayTexts = texts;
  };
  /** NAV1: where the orbit pivot's marker was last drawn (overlay CSS px), or null. */
  let navMark: [number, number] | null = null;
  /** The annotations of the view shown, again when the model or the view changed. */
  const refreshAnnotations = () => {
    const key = plan ? `${state.value.rev}|${viewId}` : "";
    if (key === annKey) return;
    annKey = key;
    anns = plan && !isSection(plan) ? viewAnnotations() : null; // SEC1: a section's are not drawn here
    stats.annotations = anns;
    if (selAnn && !anns?.items.some((a) => a.id === selAnn)) selAnn = stats.selectedAnnotation = null;
    dirty = true;
  };
  const annotatingNow = () => annotate.value?.active === true && plan !== null && mesh !== null;
  /** One annotate tool call for the pointer at client (x, y). */
  const annotAt = (name: "pointer" | "click", cx: number, cy: number, mods: { shiftKey: boolean; ctrlKey: boolean; metaKey: boolean }) => {
    const r = canvas.getBoundingClientRect();
    const x = cx - r.left, y = cy - r.top;
    stats.annotateCalls++;
    const { state: st, error } = annotateCall(name, { ray: rayAt(x, y), ray_tol: rayAt(x + SNAP_PX, y), free: mods.shiftKey, ortho: mods.ctrlKey || mods.metaKey });
    stats.annotError = error;
    if (error) console.warn(`gl viewport: annotate ${name}:`, error);
    stats.annotate = st;
    if (st?.added) { selAnn = stats.selectedAnnotation = stats.annotAdded = st.added; annKey = ""; }
    dirty = true;
  };
  const annotCancel = () => {
    stats.annotateCalls++;
    const { state: st, error } = annotateCall("cancel");
    stats.annotError = error;
    stats.annotate = st;
    dirty = true;
  };

  /** The camera ray through canvas CSS pixel (x, y): eye and direction, scene coordinates. */
  const rayAt = (x: number, y: number): number[] => {
    const { e, aspect, up } = matrices();
    const f = norm(sub(target, e)), s = norm(cross(f, up)), u = cross(s, f);
    const ndcX = (2 * x) / canvas.clientWidth - 1, ndcY = 1 - (2 * y) / canvas.clientHeight;
    if (plan) { // orthographic: parallel rays from the eye's plane, straight down (up)
      const o = add(e, add(scale(s, (mirror() ? -1 : 1) * ndcX * half * aspect), scale(u, ndcY * half)));
      return [...o, ...f];
    }
    if (ortho3d()) { // Q2: the orthographic 3D camera: parallel rays from the eye's plane
      const h = distance * Math.tan(FOV_Y / 2);
      return [...add(e, add(scale(s, ndcX * h * aspect), scale(u, ndcY * h))), ...f];
    }
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
  // ---- the measure tool (ticket T1) ----
  const measuringNow = () => measure.value?.active === true && mesh !== null && !placingNow();
  /** One measure tool call for the pointer at client (x, y); the answer is drawn. */
  const measureAt = (name: "measure_pointer" | "measure_click", cx: number, cy: number,
    mods: { shiftKey: boolean; altKey: boolean; ctrlKey: boolean; metaKey: boolean }) => {
    const r = canvas.getBoundingClientRect();
    const x = cx - r.left, y = cy - r.top;
    stats.measureCalls++;
    const { state: st, error } = measureCall(name, {
      ray: rayAt(x, y), ray_tol: rayAt(x + SNAP_PX, y), tol_px: SNAP_PX, free: mods.altKey, ortho: mods.shiftKey, chain: mods.ctrlKey || mods.metaKey,
    });
    stats.measureError = error;
    if (error) console.warn(`gl viewport: ${name}:`, error);
    stats.measure = st;
    dirty = true;
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
  const pick = (x: number, y: number): number => pickHit(x, y)?.el ?? -1;
  /** The element under canvas pixel (x, y) and the point hit (scene mm), or null. */
  const pickHit = (x: number, y: number): { el: number; point: V3 } | null => {
    const m = mesh;
    if (!m) return null;
    const r = rayAt(x, y);
    const e: V3 = [r[0] ?? 0, r[1] ?? 0, r[2] ?? 0], dir: V3 = [r[3] ?? 0, r[4] ?? 0, r[5] ?? 0];
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
      const p = turnZ([e[0] + dir[0] * d, e[1] + dir[1] * d, e[2] + dir[2] * d], -(box.yaw ?? 0)); // ISS71: in the box's frame
      for (let k = 0; k < 3; k++) if (p[k]! < (box.min[k] ?? 0) || p[k]! > (box.max[k] ?? 0)) return true;
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
    return hit < 0 ? null : { el: hitE, point: add(e, scale(dir, best)) };
  };

  stats.pickAt = (cx, cy) => {
    const r = canvas.getBoundingClientRect();
    const k = pick(cx - r.left, cy - r.top);
    return k >= 0 ? mesh?.ids[k] ?? null : null;
  };
  const linkOf = (k: number): string | null => {
    const lk = mesh?.model?.[k] ?? 0;
    return lk > 0 ? mesh?.links?.[lk - 1]?.name ?? null : null;
  };
  stats.pickInfoAt = (cx, cy) => {
    const r = canvas.getBoundingClientRect();
    const k = pick(cx - r.left, cy - r.top);
    const id = k >= 0 ? mesh?.ids[k] : undefined;
    return id === undefined ? null : { id, link: linkOf(k) };
  };

  // ---- navigation (ticket NAV1, gl/nav.ts): the pivots of the wheel and of an orbit drag ----
  const camNow = (): OrbitCam & { half: number } => ({ yaw, pitch, distance, target: [...target], half });
  const setCam = (c: OrbitCam) => { yaw = c.yaw; pitch = c.pitch; distance = c.distance; target = c.target; };
  const toWorld = (p: V3): number[] => { const o = mesh?.origin ?? [0, 0, 0]; return [p[0] + o[0], p[1] + o[1], p[2] + o[2]]; };
  /** The wheel's last pivot (lib.rs zoom_pivot): the pointer (canvas px), the camera after that zoom, the pivot (scene mm). */
  let zoomPivot: { x: number; y: number; cam: OrbitCam & { half: number }; p: V3 } | null = null;
  /** lib.rs wheel_pivot in 3D: the geometry under the cursor, else the plane through the orbit target. */
  const wheelPivot = (x: number, y: number): V3 => {
    stats.nav.picks++;
    const h = pickHit(x, y);
    stats.nav.zoomFrom = h ? "hit" : "plane";
    if (h) return h.point;
    const r = rayAt(x, y);
    return onTargetPlane(camNow(), [r[0] ?? 0, r[1] ?? 0, r[2] ?? 0], [r[3] ?? 0, r[4] ?? 0, r[5] ?? 0]);
  };
  /** The centre of the selection's box (lib.rs: ClipBox::around the selected element), or null. */
  const selectionCentre = (): V3 | null => {
    const m = mesh;
    if (!m) return null;
    const linked = state.value.linked ?? null;
    const sel = new Set(linked ? [] : state.value.selection);
    const lo: V3 = [Infinity, Infinity, Infinity], hi: V3 = [-Infinity, -Infinity, -Infinity];
    for (let el = 0; el < m.ids.length; el++) {
      const k = m.model?.[el] ?? 0, id = m.ids[el] ?? "";
      const on = k === 0 ? sel.has(id) : linked !== null && linked.id === id && linked.link === m.links?.[k - 1]?.name;
      if (!on || !(m.nv[el] ?? 0)) continue;
      for (let a = 0; a < 3; a++) {
        lo[a] = Math.min(lo[a]!, boxes[6 * el + a] ?? 0);
        hi[a] = Math.max(hi[a]!, boxes[6 * el + 3 + a] ?? 0);
      }
    }
    return lo[0] <= hi[0] ? scale(add(lo, hi), 0.5) : null;
  };
  /** lib.rs orbit_pivot_at: the geometry under the press, else the selection's centre, else null (the target). */
  const orbitPivotAt = (x: number, y: number): V3 | null => {
    stats.nav.picks++;
    const h = pickHit(x, y);
    const p = h ? h.point : selectionCentre();
    stats.nav.orbitFrom = h ? "hit" : p ? "selection" : "target";
    stats.nav.orbitPivot = p ? toWorld(p) : null;
    return p;
  };
  /** lib.rs handle_view_input, the wheel in 3D: zoom about the pivot, kept while the pointer and the
   *  camera are those of the last zoom (zooming about a point leaves it under the cursor). */
  const zoom3d = (e: WheelEvent) => {
    const r = canvas.getBoundingClientRect(), x = e.clientX - r.left, y = e.clientY - r.top;
    const z = zoomPivot;
    const p = !mesh ? null : z && z.x === x && z.y === y && sameCam(z.cam, camNow()) ? z.p : wheelPivot(x, y);
    setCam(zoomAt(camNow(), wheelFactor(e), p ?? target));
    zoomPivot = p ? { x, y, cam: camNow(), p } : null;
    stats.nav.zoomPivot = p ? toWorld(p) : null;
  };
  stats.navEye = () => (mesh && !plan ? toWorld(eye()) : null);
  stats.elementBox = (id) => {
    const m = mesh, el = m ? m.ids.findIndex((x, k) => x === id && !(m.model?.[k] ?? 0)) : -1;
    if (!m || el < 0) return null;
    const b = Array.from(boxes.subarray(6 * el, 6 * el + 6)), o = m.origin;
    return { min: [b[0]! + o[0], b[1]! + o[1], b[2]! + o[2]], max: [b[3]! + o[0], b[4]! + o[1], b[5]! + o[2]] };
  };
  // ---- end of navigation ----

  // ---- BUG1: the drawing buffer takes the canvas's layout size at the frame, before the draw. A resize
  // observer alone runs after the frame's rAF callbacks: the first frame after a layout change (the
  // properties panel shown or gone with the selection) was drawn at the old size, stretched, then the
  // observer's resize cleared it until the next frame. Assigned only on a change (a write clears). ----
  const fitCanvas = (): void => {
    const dpr = window.devicePixelRatio || 1;
    const w = Math.max(1, Math.round(canvas.clientWidth * dpr)), h = Math.max(1, Math.round(canvas.clientHeight * dpr));
    if (canvas.width === w && canvas.height === h) return;
    canvas.width = w;
    canvas.height = h;
    dirty = true;
  };
  new ResizeObserver(fitCanvas).observe(canvas);
  // ---- end BUG1 ----

  /** A press held on the canvas; `orbit`: the pivot of an orbit drag (NAV1), picked when it starts (null: the target). */
  let drag: { pan: boolean; box: boolean; annot?: string; x: number; y: number; x0: number; y0: number; moved: boolean; orbit?: V3 | null } | null = null;
  /** One call of the C core's box tool for the pointer at client (x, y). */
  const boxAt = (name: "box_press" | "box_move" | "box_release", cx: number, cy: number) => {
    const r = canvas.getBoundingClientRect();
    const x = cx - r.left, y = cy - r.top;
    stats.boxCalls++;
    const { state: b, error } = boxTool(name, name === "box_press" ? { ray: rayAt(x, y), ray_tol: rayAt(x + ARROW_HIT_PX, y) } : { ray: rayAt(x, y) }); // ISS71: the arrowhead's press radius
    stats.boxError = error;
    if (error) console.warn(`gl viewport: ${name}:`, error);
    preview = b?.active && b.box ? { min: b.box.min, max: b.box.max, yaw: b.box.yaw } : null; // ISS71: yaw
    stats.boxFace = b?.active ? b.face : null;
    dirty = true;
    return b;
  };
  let pendingBox: PointerEvent | null = null;
  canvas.addEventListener("pointerdown", (e) => {
    canvas.setPointerCapture(e.pointerId);
    // A left press on a handle of the section box: the box tool holds that face (no orbit).
    const onHandle = e.button === 0 && !e.shiftKey && !placingNow() && !measuringNow() && !plan && clipBox() !== null && boxAt("box_press", e.clientX, e.clientY)?.active === true;
    drag = { pan: e.button === 1 || e.button === 2 || (e.button === 0 && e.shiftKey), box: onHandle, x: e.clientX, y: e.clientY, x0: e.clientX, y0: e.clientY, moved: false };
    if (onHandle) canvas.style.cursor = "grabbing"; // ISS71
    // A left press on a dimension or a text of the plan: it is held (a click selects it, a drag moves it).
    if (plan && e.button === 0 && !e.shiftKey && !placingNow() && !annotatingNow() && !measuringNow()) {
      const r = canvas.getBoundingClientRect();
      const id = annotationAt(e.clientX - r.left, e.clientY - r.top);
      if (id) drag.annot = id;
    }
  });
  canvas.addEventListener("pointermove", (e) => {
    if (!drag) {
      if (placingNow() || annotatingNow() || measuringNow()) pendingMove = e;
      // ---- ISS71 ---- the arrowhead under the pointer: drawn larger, the grab cursor
      const r = canvas.getBoundingClientRect();
      const hv = !placingNow() && !measuringNow() && !plan ? handleAt(e.clientX - r.left, e.clientY - r.top) : null;
      if (hv !== stats.boxHover) { stats.boxHover = hv; canvas.style.cursor = hv === null ? "" : "grab"; dirty = true; }
      // ---- end ISS71 ----
      return;
    }
    if (drag.annot) { // the held annotation follows the pointer (world mm: the orthographic rays' offset)
      if (Math.hypot(e.clientX - drag.x0, e.clientY - drag.y0) > CLICK_PX) drag.moved = true;
      if (!drag.moved) return;
      const r = canvas.getBoundingClientRect();
      const a = rayAt(drag.x0 - r.left, drag.y0 - r.top), b = rayAt(e.clientX - r.left, e.clientY - r.top);
      annDrag = { id: drag.annot, x0: drag.x0, y0: drag.y0, by: [(b[0] ?? 0) - (a[0] ?? 0), (b[1] ?? 0) - (a[1] ?? 0)] };
      dirty = true;
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
    turn = null; // the hand takes over from a view-cube turn
    if (drag.pan || plan) { // a plan has no orbit: any drag pans
      const { e: ey, up } = matrices();
      const f = norm(sub(target, ey)), s = norm(cross(f, up)), u = cross(s, f);
      const k = plan ? (2 * half) / Math.max(1, canvas.clientHeight) : (2 * distance * Math.tan(FOV_Y / 2)) / Math.max(1, canvas.clientHeight);
      const sr = plan && mirror() ? scale(s, -1) : s;
      target = add(target, add(scale(sr, -dx * k), scale(u, dy * k)));
    } else { // ---- navigation (NAV1): orbit about the pivot picked where the drag started ----
      if (drag.orbit === undefined) {
        const r = canvas.getBoundingClientRect();
        drag.orbit = orbitPivotAt(drag.x0 - r.left, drag.y0 - r.top);
      }
      setCam(orbitAbout(camNow(), dx * ORBIT_RAD_PER_PX, dy * ORBIT_RAD_PER_PX, drag.orbit));
    }
    dirty = true;
  });
  const release = (e: PointerEvent) => {
    const d = drag;
    drag = null;
    if (canvas.hasPointerCapture(e.pointerId)) canvas.releasePointerCapture(e.pointerId);
    if (d && d.orbit !== undefined) { stats.nav.orbitPivot = null; dirty = true; } // NAV1: the pivot's marker goes
    if (d?.box) { // one session change: the released box
      pendingBox = null;
      canvas.style.cursor = stats.boxHover === null ? "" : "grab"; // ISS71
      if (e.type === "pointerup") boxAt("box_release", e.clientX, e.clientY);
      else { stats.boxCalls++; boxTool("box_cancel"); preview = null; stats.boxFace = null; dirty = true; }
      return;
    }
    if (d?.annot) { // a held annotation: a click selects it, a drag moves it (one verb)
      const by = annDrag?.by;
      annDrag = null;
      selAnn = stats.selectedAnnotation = d.annot;
      dirty = true;
      if (e.type === "pointerup" && d.moved && by && anns && (by[0] !== 0 || by[1] !== 0)) {
        const err = moveAnnotation(anns.view, d.annot, [Math.round(by[0]), Math.round(by[1])]);
        stats.annotError = err;
        if (err) console.warn("gl viewport: move_annotation:", err);
      }
      return;
    }
    // ---- ISS71 ---- a right click without a drag: the selection's menu (shell/ContextMenu.vue); the
    // element under the pointer is selected first when it is not (an empty spot keeps the selection).
    if (e.type === "pointerup" && d && !d.moved && e.button === 2 && !placingNow() && !annotatingNow() && !measuringNow()) {
      const r = canvas.getBoundingClientRect();
      const k = pick(e.clientX - r.left, e.clientY - r.top);
      const id = k >= 0 ? mesh?.ids[k] : undefined;
      if (id && !state.value.selection.includes(id)) {
        const link = linkOf(k);
        const err = link !== null ? selectLinked(link, id) : select([id]);
        if (err) console.warn("gl viewport: select:", err);
      }
      stats.menus++;
      window.dispatchEvent(new CustomEvent("bb-viewport-menu", { detail: { x: e.clientX, y: e.clientY } }));
      return;
    }
    // ---- end ISS71 ----
    if (e.type !== "pointerup" || !d || d.moved || e.button !== 0) return;
    if (placingNow()) { // a click while placing goes to the tool (Shift: free, no pan)
      pendingMove = null;
      toolAt("click", e.clientX, e.clientY, e);
      return;
    }
    if (annotatingNow()) { // a click while annotating goes to the annotate tool
      pendingMove = null;
      annotAt("click", e.clientX, e.clientY, e);
      return;
    }
    if (measuringNow()) { // a click while measuring goes to the measure tool (ticket T1)
      pendingMove = null;
      measureAt("measure_click", e.clientX, e.clientY, e);
      return;
    }
    if (selAnn) { selAnn = stats.selectedAnnotation = null; dirty = true; } // a click elsewhere: the annotation is let go
    if (d.pan) return;
    if (plan && Math.hypot(e.clientX - d.x0, e.clientY - d.y0) > CLICK_PX) return;
    const r = canvas.getBoundingClientRect();
    const k = pick(e.clientX - r.left, e.clientY - r.top);
    stats.picks++;
    const id = k >= 0 ? mesh?.ids[k] : undefined;
    const link = k >= 0 ? linkOf(k) : null;
    const err = id && link !== null ? selectLinked(link, id) : select(id ? [id] : []);
    if (err) console.warn("gl viewport: select:", err);
  };
  canvas.addEventListener("pointerup", release);
  canvas.addEventListener("pointercancel", release);
  canvas.addEventListener("contextmenu", (e) => e.preventDefault());
  // ---- KEY1 ---- the keys' viewport part (shell/keys.ts owns the one keydown listener and the order)
  // Esc (lib.rs:3031-3044): the annotate tool, then placing (stops, drops the points), then the
  // measure tool (its first point, then the tool: ticket T1).
  viewportKeys.cancelTool = () => {
    if (annotate.value?.active === true) { annotCancel(); return true; }
    if (placing.value?.active === true) {
      stats.toolCalls++;
      const { state: t, error } = toolCall("cancel");
      stats.toolError = error;
      showTool(t);
      return true;
    }
    if (measuringNow()) {
      stats.measureCalls++;
      const { state: st, error } = measureCall("measure_cancel");
      stats.measureError = error;
      stats.measure = st;
      dirty = true;
      return true;
    }
    return false;
  };
  viewportKeys.selectedAnnotation = () => (plan && anns ? selAnn : null);
  viewportKeys.dropAnnotation = () => { selAnn = stats.selectedAnnotation = null; dirty = true; };
  viewportKeys.deleteAnnotation = () => { // Delete: the selected annotation goes
    if (!selAnn || !anns || !plan) return false;
    const err = deleteAnnotation(anns.view, selAnn);
    stats.annotError = err;
    if (err) console.warn("gl viewport: delete_annotation:", err);
    else selAnn = stats.selectedAnnotation = null;
    dirty = true;
    return true;
  };
  // ---- KEY1 ---- the status bar's pointer coordinates (lib.rs:2826-2864 pointer_world, :4603
  // `pointer`): the point under the pointer on an element (the pick), else in a plan on its cut
  // plane (the range's top), else none; as `coordinates` says it (the model's georeference: survey
  // E/N and height, or x y without one). One query per frame at most, none while the pointer is still.
  let ptrAt: [number, number] | null = null, ptrDone = "";
  canvas.addEventListener("pointermove", (e) => { const r = canvas.getBoundingClientRect(); ptrAt = [e.clientX - r.left, e.clientY - r.top]; });
  canvas.addEventListener("pointerleave", () => { ptrAt = null; ptrDone = ""; pointer.value = null; });
  const pointerWorld = (x: number, y: number): [number, number, number] | null => {
    const h = pickHit(x, y);
    let p: V3 | null = h ? h.point : null;
    const box = plan ? clipBox() : null;
    if (!p && box) {
      const r = rayAt(x, y), dz = r[5] ?? 0, z = box.max[2] ?? 0;
      if (Math.abs(dz) > 1e-9) { const t = (z - (r[2] ?? 0)) / dz; p = [(r[0] ?? 0) + (r[3] ?? 0) * t, (r[1] ?? 0) + (r[4] ?? 0) * t, z]; }
    }
    if (!p) return null;
    const w = toWorld(p);
    return [w[0] ?? 0, w[1] ?? 0, w[2] ?? 0];
  };
  const pointerFrame = () => {
    if (!ptrAt || !mesh) return;
    const key = `${ptrAt[0]},${ptrAt[1]}`;
    if (key === ptrDone) return;
    ptrDone = key;
    stats.pointerQueries++;
    const w = pointerWorld(ptrAt[0], ptrAt[1]);
    const c = w ? coordinates(w) : null;
    pointer.value = w && c ? { world_mm: w, text: c.text } : null;
  };
  // ---- end KEY1 ----
  const wheel = (e: WheelEvent) => {
    if (plan) { // about the pointer: the plan point under it stays put
      const r = canvas.getBoundingClientRect(), x = e.clientX - r.left, y = e.clientY - r.top;
      const a = rayAt(x, y);
      half = Math.min(1e7, Math.max(10, half * wheelFactor(e)));
      const b = rayAt(x, y);
      target = [target[0] + (a[0] ?? 0) - (b[0] ?? 0), target[1] + (a[1] ?? 0) - (b[1] ?? 0), target[2] + (isSection(plan) ? (a[2] ?? 0) - (b[2] ?? 0) : 0)]; // SEC1: a section's screen holds z
    } else zoom3d(e); // NAV1: about the geometry under the cursor
    dirty = true;
  };
  canvas.addEventListener("wheel", (e) => { e.preventDefault(); wheel(e); }, { passive: false });
  // NAV1, lib.rs handle_view_input: a double-click fits all (a plan: the plan), except while a tool takes the clicks.
  canvas.addEventListener("dblclick", (e) => {
    if (e.button !== 0 || placingNow() || annotatingNow() || measuringNow()) return;
    const r = canvas.getBoundingClientRect();
    if (plan && annotationAt(e.clientX - r.left, e.clientY - r.top)) return;
    turn = null;
    if (plan) fitPlan();
    else fit();
    stats.nav.fits++;
    dirty = true;
  });

  // The view cube, Home and Fit all (viewcube.ts): the camera stays here, the cube asks.
  const cube = viewCube(canvas, {
    look: (y, p) => { turn = turnTo(y, p); dirty = true; },
    orbit: (dx, dy) => {
      turn = null;
      yaw -= dx;
      pitch = Math.min(PITCH_LIMIT, Math.max(-PITCH_LIMIT, pitch + dy));
      dirty = true;
    },
    home: () => { // lib.rs `navigation` Home: turn to the default view (project north), fit all
      if (plan) return;
      turn = turnTo(HOME_YAW + projectNorth(), HOME_PITCH);
      fit();
      dirty = true;
    },
    fitAll: () => { if (plan) fitPlan(); else fit(); dirty = true; },
    wheel,
  });
  stats.viewCube = cube.stats;
  stats.cubeCellAt = (d) => cube.cellAt(d);
  stats.cubeLetterAt = (t) => cube.letterAt(t);
  stats.cubeHitAt = (x, y) => cube.hitAt(x, y);

  // ---- Q2: Fit all and Top view from the ribbon or the palette (ribbon.rs:707-718) ----
  viewActions.fit = () => { turn = null; if (plan) fitPlan(); else fit(); dirty = true; };
  viewActions.top = () => {
    if (plan) return;
    turn = null;
    pitch = 1.5;
    yaw = -Math.PI / 2 + projectNorth();
    dirty = true;
  };
  // ---- end Q2 ----

  let seenAnnot: AnnotateState | null = null;
  const loop = () => {
    if (openViews.value !== seenViews) { // a tab, a view or the model changed: the view shown
      seenViews = openViews.value;
      const cur = openViews.value.current;
      const vc = cur?.type === "view" ? viewClip() : null;
      switchView(vc, cur?.type === "view" ? cur.id : null);
    }
    if (actions.value !== seenActions) { seenActions = actions.value; dirty = true; } // Q2: a switch turned
    pointerFrame(); // KEY1: the status bar's coordinates, once a frame at most
    if (turn) { // a view-cube turn: one step per frame (lib.rs `animate`)
      const ms = performance.now() - turn.start;
      const k = turnAt(turn, turn.start + ms * (stats.clockScale ?? 1));
      yaw = k.yaw;
      pitch = k.pitch;
      stats.turnLog?.frames.push({ ms, yaw, pitch, done: k.done });
      if (k.done) turn = null;
      dirty = true;
    }
    cube.update(yaw, pitch, mesh !== null, plan !== null);
    stats.camera = { mode: plan ? "plan" : "3d", view: viewId, look: plan?.look ?? null, half, yaw, pitch, target: [...target], distance, turning: turn !== null };
    const rev = meshKey();
    if (state.value !== seenState) {
      seenState = state.value;
      if (!loading && !checking && wantRev === rev) void checkEpoch();
    }
    // ---- TEST1: no model open (state.file null): nothing to ask the host for (it would answer an empty
    // mesh; GET /mesh used to 404 there and the error path retried every second). The last model's
    // picture goes; the next model loads whatever its rev (wantRev forgotten). ----
    if (state.value.file === null) {
      wantRev = null;
      if (mesh !== null && mesh.ids.length > 0) upload(parseMesh(EMPTY_BMSH()));
    } else if (!loading && wantRev !== rev) {
      // ---- end TEST1 ----
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
      else if (annotatingNow()) annotAt("pointer", e.clientX, e.clientY, e);
      else if (measuringNow()) measureAt("measure_pointer", e.clientX, e.clientY, e);
    }
    if (measure.value !== stats.measure) { stats.measure = measure.value; dirty = true; } // the action, a clear, another model
    if (annotate.value !== seenAnnot) { // started or stopped (bimble.ts startDimension, startText)
      seenAnnot = annotate.value;
      stats.annotate = seenAnnot;
      dirty = true;
    }
    if (annotate.value?.active === true && !plan) annotCancel(); // the plan was left
    refreshAnnotations();
    if (refLayer.refresh(`${state.value.rev}|${viewId}`, plan !== null && !isSection(plan))) dirty = true; // L1 (SEC1: plans)
    // ---- G2: grid axes, again when the model, its links or the view change ----
    if (gridAxesLayer.refresh(`${state.value.rev}|${state.value.file}|${state.value.links ?? 0}|${viewId}`, plan !== null && !isSection(plan))) dirty = true; // SEC1: plans
    // ---- end G2 ----
    if (tool && placing.value?.active !== true) showTool(null); // placing stopped elsewhere
    const key = mesh ? `${state.value.selection.join(",")}|${state.value.linked?.link ?? ""}` : "";
    if (mesh && key !== selKey) {
      selKey = key;
      gl.bindBuffer(gl.ARRAY_BUFFER, colBuf);
      gl.bufferData(gl.ARRAY_BUFFER, colours(), gl.DYNAMIC_DRAW);
      dirty = true;
    }
    fitCanvas(); // BUG1
    if (dirty) {
      dirty = false;
      draw();
      stats.drawn = { frame: stats.frames, rev: mesh?.rev ?? null, w: canvas.width, h: canvas.height }; // BUG1
    }
    stats.loops = (stats.loops ?? 0) + 1; // BUG1
    requestAnimationFrame(loop);
  };
  requestAnimationFrame(loop);
  return () => { dirty = true; };
}
