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
  runAction, viewAdd, // VIEWSHEET: the section tool
  annotate, annotateCall, boxTool, deleteAnnotation, moveAnnotation, openViews, placing, select, selectLinked, state, toolCall,
  viewAnnotations, viewClip, visibility, visibilitySets, measure, measureCall, type MeasureState,
  settingOut, // SETOUT
  userSettings, // 3D1: moving_fps
  refPlanes, // WAVE1B
  openingAt, openingsOf, selectedOpening, // WAVE1B (B2)
  storeys, viewVisibility, // WAVE1B (A3)
  linkDraw, linkNotes, meshKey as coreMeshKey, type LinkDraw, type LinkNote, // LNK3D
  type AnnotateState, type ToolPointer, type ToolState, type ViewAnnotation, type ViewAnnotations, type ViewClip, type Visibility,
  type VisibilitySets,
} from "./bimble";
import { drawMeasure } from "./gl/measure";
import { BATCH_TRIS, boxInFrustum, budgetCut, budgetCutFast, cutEnd, frustumPlanes, makeBatches, MIN_MOVING_TRIS, type Batch, type Cut } from "./gl/cull"; // 3D1
import { batchShown, IDENTITY_XF, packState, sameXf, STATE_NONE, STATE_TEX_W, xfBox, xfPoint, xfRayToLocal, xfRows, type ModelXf } from "./gl/cull"; // LNK3D
import { cutProxies, fitProxies, parseBprx, standIns, usableCells, type Proxies } from "./gl/proxies"; // 3D2
import { referenceLayer } from "./gl/reference"; // L1: reference plans
import { gpuBudget, linkGpu, linkDetail } from "./gl/linkmem"; // LNK3D (D)
import { farModels, linkFar } from "./gl/farlinks"; // REAL1
import { bmsh3Size, bmsh4Decode, bmsh4RowsBoxes, bmsh4RowsDecode, bmsh4Stats, type Bmsh4Rows } from "./gl/bmsh4"; // SIZE2; SIZE3: in a worker
import { sectionMatrices, sectionFit, isSection } from "./gl/section"; // SEC1
import { gridLayer } from "./gl/grids"; // ---- G2: grid axes in plans ---- / ---- end G2 ----
import { drawSettingOut, type MarkerDrawn } from "./gl/settingout"; // SETOUT: base point, survey point, north
import { drawRefPlanes, type RefPlaneDrawn } from "./gl/refplanes"; // WAVE1B: reference planes
import { clipPlanes, onTargetPlane, orbitAbout, ORBIT_RAD_PER_PX, sameCam, wheelFactor, zoomAt, type OrbitCam } from "./gl/nav";
import { frameTimer, type FrameRec } from "./gl/frametime";
import { sceneTarget } from "./gl/target";
import { hizBuilder } from "./gl/hiz";
import { maskBatch, runsOf } from "./gl/visible";
import { relMvp, renderOrigin } from "./gl/origin"; // VIS2
import { perfFrame, perfInputs, perfMesh, perfPreview, renderer as perfRenderer } from "./perf"; // GPU1: the performance panel's counters (LNK3D: the renderer's reason)
import { gpuSwitch, type GlStream, type GpuModels, type GpuRenderer, type GpuView } from "./gpu/switch"; // GPU1: the WebGPU renderer (ADR 0083)
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
const SELECTION_CSS = `rgb(${SELECTION_SRGB.join(", ")})`; // WAVE1B (B2): the selected opening's box
// Linked (reference) models: bimble-core style.rs:340-341 `reference`, `reference_edges`.
const REFERENCE_SRGB: [number, number, number] = [190, 214, 238];
/** LINKF: a link's colour override (ccore/session/links.h, palette 0..11; LinksPanel.vue's swatches). */
const LINK_SRGB: [number, number, number][] = ["#c0504d", "#4f81bd", "#9bbb59", "#8064a2", "#4bacc6", "#f79646", "#2c4d75", "#7f7f7f", "#d99694", "#95b3d7", "#c3d69b", "#b2a2c7"]
  .map((h) => [parseInt(h.slice(1, 3), 16), parseInt(h.slice(3, 5), 16), parseInt(h.slice(5, 7), 16)]);
const REFERENCE_EDGE_SRGB: [number, number, number] = [52, 110, 180];
// Revealed hidden elements (Reveal hidden): the comparison colour, as Rust's Style::OnlyHere.
const REVEALED_SRGB: [number, number, number] = [232, 150, 64];
// bimble-render Style::cut_shade: the brightness of cut surfaces (ADR 0014).
const CUT_SHADE = 0.72;
// The section box's edges and handles.
const BOX_SRGB: [number, number, number] = [40, 110, 200];
// 3D1: the outline of a batch a moving frame could not finish (its volume stays on screen)
const OUTLINE_SRGB: [number, number, number] = [120, 120, 124];
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
  links?: { name: string; visible: boolean; elements: number; colour?: number;
    /** LNK3D: its placement (the link's vertices, segments and placements stay in its own frame) and its first element. */
    xf: ModelXf; e0: number }[];
  /** 3D1 (BMSH version 3, ccore/src/scene_bmsh.h): per element 0 (its own mesh, scene coordinates) or
   *  k + 1 (it places shape k: its row's ranges are the shape's, in the shape's own coordinates), and
   *  per element its placement (12 floats: columns x, y, z, t). Absent: version 1, every mesh placed. */
  shape?: Uint32Array;
  place?: Float32Array;
  /** LNK3D (D): per model (the open one, then each link) its elements, vertices and segments [first, end):
   *  each model's own GPU buffers hold these. Absent: one model, everything. */
  ranges?: { e0: number; e1: number; v0: number; v1: number; i0: number; i1: number; s0: number; s1: number }[];
  /** SIZE2: per model its heavy arrays (mergeMesh), null once dropped (a link's, on the GPU). Absent: one model. */
  parts?: (MeshPart | null)[];
  /** SIZE3 (B): a link's mesh whose heavy arrays were dropped as soon as read (the open's decoded budget): its
   *  array lengths (vertices, indices, segments) and its elements' boxes (its own frame, as upload computes
   *  them), so that its rows, ranges, boxes and batches stand without them (its detail is asked again). */
  dropped?: { nv: number; ni: number; ns: number; boxes: Float32Array };
  /** SIZE3 (B): a merged mesh: per element its box when its model's arrays were dropped (else unused). */
  preBox?: Float32Array;
}

/** SIZE3 (B): element e's box from its model's vertices `P` (its first vertex `a`, `n` of them), placed by `pl`
 *  (a shape's placement) when given, into out[6 * at ..]: upload's arithmetic (f32, the same order). */
function elementBox(P: Float32Array, a: number, n: number, pl: Float32Array | null, out: Float32Array, at: number): void {
  const f = Math.fround;
  let x0 = Infinity, y0 = Infinity, z0 = Infinity, x1 = -Infinity, y1 = -Infinity, z1 = -Infinity;
  for (let v = 3 * a; v < 3 * (a + n); v += 3) {
    let x = P[v] ?? 0, y = P[v + 1] ?? 0, z = P[v + 2] ?? 0;
    if (pl) { // (3D2, step 5: no array a vertex)
      const qx = f(f(f(f(pl[0]! * x) + f(pl[3]! * y)) + f(pl[6]! * z)) + pl[9]!), qy = f(f(f(f(pl[1]! * x) + f(pl[4]! * y)) + f(pl[7]! * z)) + pl[10]!);
      z = f(f(f(f(pl[2]! * x) + f(pl[5]! * y)) + f(pl[8]! * z)) + pl[11]!), x = qx, y = qy;
    }
    if (x < x0) x0 = x;
    if (x > x1) x1 = x;
    if (y < y0) y0 = y;
    if (y > y1) y1 = y;
    if (z < z0) z0 = z;
    if (z > z1) z1 = z;
  }
  out[6 * at] = x0, out[6 * at + 1] = y0, out[6 * at + 2] = z0, out[6 * at + 3] = x1, out[6 * at + 4] = y1, out[6 * at + 5] = z1;
}

/** SIZE3 (B): a link's mesh from its rows and boxes (gl/bmsh4.ts bmsh4Rows, read in the worker): no heavy arrays. */
function rowsMesh(r: Bmsh4Rows): Bmsh {
  return { rev: r.rev, origin: r.origin, ids: r.ids, cats: r.cats, v0: r.v0, nv: r.nv, i0: r.i0, ni: r.ni, s0: r.s0, ns: r.ns,
    pos: new Float32Array(0), idx: new Uint32Array(0), edges: new Float32Array(0), normals: new Int8Array(0), size: r.size,
    shape: r.shape, place: r.place, dropped: { ...r.counts, boxes: r.boxes } };
}

/** SIZE3 (B): link mesh `m` without its heavy arrays (its boxes and lengths kept, its rows compact): the decoded
 *  buffer they were views of can go. */
export function dropArrays(m: Bmsh): Bmsh {
  const nE = m.ids.length, boxes = new Float32Array(6 * nE);
  for (let e = 0; e < nE; e++) elementBox(m.pos, m.v0[e] ?? 0, m.nv[e] ?? 0, m.shape?.[e] ? m.place!.subarray(12 * e, 12 * e + 12) : null, boxes, e);
  return { ...m, pos: new Float32Array(0), idx: new Uint32Array(0), edges: new Float32Array(0), normals: new Int8Array(0),
    dropped: { nv: m.pos.length / 3, ni: m.idx.length, ns: m.edges.length / 6, boxes } };
}

/** 3D1: a shape with fewer copies, or fewer triangles, is baked into the merged buffers (placed like an
 *  own mesh): a draw call per shape costs more than it saves for few copies (ADR 0034, xeokit's pitfall),
 *  and an instance has a cost of its own (SwiftShader: 20,000 instanced boxes of 12 triangles drew in
 *  1.2 s against 0.19 s merged; frames.md, ADR 0078). */
const INSTANCE_MIN_COPIES = 4, INSTANCE_MIN_TRIS = 64;
/** SIZE3 (B): the decoded link arrays an open keeps to upload at once (the rest: dropped, asked again when needed). */
const OPEN_KEEP_BYTES = 128 << 20;

/** Parses BMSH version 1 (ccore/session/mesh.c) at byte `at`; `exact`: it must end the buffer.
 *  Throws on a malformed buffer. */
export function parseBmsh(ab: ArrayBuffer, at = 0, exact = true): Bmsh {
  const dv = new DataView(ab);
  if (ab.byteLength < at + 52 || dv.getUint32(at, true) !== 0x48534d42) throw new Error("not BMSH");
  const version = dv.getUint32(at + 4, true);
  if (version !== 1 && version !== 3) throw new Error(`BMSH version ${version}`);
  const u = (o: number) => dv.getUint32(at + o, true);
  const rev = u(8), nE = u(12), nV = u(16), nI = u(20), nS = u(24);
  const size = 52 + 48 * nE + 12 * nV + 4 * nI + 32 * nS + (version === 3 ? 52 * nE : 0);
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
  o += 8 * nS;
  let top = 0; // (3D2, step 5: the largest index once, not a test a index)
  for (let i = 0; i < nI; i++) { const x = idx[i]!; if (x > top) top = x; }
  if (nI && top >= nV) throw new Error("BMSH index out of range");
  for (let e = 0; e < nE; e++)
    if ((i0[e] ?? 0) + (ni[e] ?? 0) > nI || (v0[e] ?? 0) + (nv[e] ?? 0) > nV || (s0[e] ?? 0) + (ns[e] ?? 0) > nS) throw new Error("BMSH row out of range");
  const m: Bmsh = { rev, origin, ids, cats, v0, nv, i0, ni, s0, ns, pos, idx, edges, normals, size };
  if (version === 3) { // 3D1: the placements block (u32 shape + 1 or 0, f32[12]), 52 bytes a row
    const shape = new Uint32Array(nE), place = new Float32Array(12 * nE);
    for (let e = 0; e < nE; e++, o += 52) {
      shape[e] = dv.getUint32(o, true);
      for (let k = 0; k < 12; k++) place[12 * e + k] = dv.getFloat32(o + 4 + 4 * k, true);
    }
    m.shape = shape, m.place = place;
    return bake(m);
  }
  return m;
}

/** 3D1: an element of a version-3 mesh whose shape has fewer than INSTANCE_MIN_COPIES copies is placed
 *  into new merged data (as ccore/src/scene.c cc_scene_world_mesh places it: f32, a mirrored placement's
 *  triangles turned) and becomes an own mesh (shape 0). */
function bake(m: Bmsh): Bmsh {
  const S = m.shape!, Pl = m.place!, nE = m.ids.length;
  const copies = new Map<number, number>();
  for (let e = 0; e < nE; e++) if (S[e]) copies.set(S[e]!, (copies.get(S[e]!) ?? 0) + 1);
  let av = 0, ai = 0, as = 0;
  const baked = (e: number) => (copies.get(S[e]!) ?? 0) < INSTANCE_MIN_COPIES || m.ni[e]! < 3 * INSTANCE_MIN_TRIS;
  for (let e = 0; e < nE; e++) if (S[e] && baked(e)) av += m.nv[e]!, ai += m.ni[e]!, as += m.ns[e]!;
  if (!av && !ai && !as) return m;
  const nV = m.pos.length / 3, nI = m.idx.length, nS = m.edges.length / 6;
  const pos = new Float32Array(3 * (nV + av)), idx = new Uint32Array(nI + ai), edges = new Float32Array(6 * (nS + as)), normals = new Int8Array(8 * (nS + as));
  pos.set(m.pos), idx.set(m.idx), edges.set(m.edges), normals.set(m.normals);
  const f = Math.fround;
  let v = nV, i = nI, sg = nS;
  for (let e = 0; e < nE; e++) {
    if (!S[e] || !baked(e)) continue;
    const p = Pl.subarray(12 * e, 12 * e + 12);
    const at = (q: Float32Array, a: number, o: Float32Array, b: number) => {
      for (let j = 0; j < 3; j++) o[b + j] = f(f(f(f(p[j]! * q[a]!) + f(p[3 + j]! * q[a + 1]!)) + f(p[6 + j]! * q[a + 2]!)) + p[9 + j]!);
    };
    const v0 = m.v0[e]!, n = m.nv[e]!;
    for (let k = 0; k < n; k++) at(m.pos, 3 * (v0 + k), pos, 3 * (v + k));
    const c0 = p[4]! * p[8]! - p[5]! * p[7]!, c1 = p[5]! * p[6]! - p[3]! * p[8]!, c2 = p[3]! * p[7]! - p[4]! * p[6]!;
    const mirrors = p[0]! * c0 + p[1]! * c1 + p[2]! * c2 < 0;
    const i0 = m.i0[e]!, ni = m.ni[e]!;
    for (let k = 0; k < ni; k++) {
      const kk = mirrors && k % 3 ? (k % 3 === 1 ? k + 1 : k - 1) : k;
      idx[i + k] = m.idx[i0 + kk]! - v0 + v;
    }
    const s0 = m.s0[e]!, ns = m.ns[e]!;
    for (let k = 0; k < 2 * ns; k++) at(m.edges, 6 * s0 + 3 * k, edges, 6 * sg + 3 * k);
    for (let k = 0; k < 2 * ns; k++) { // a face normal turned by the linear part, as cc_scene_world_mesh (snorm8)
      const a = 8 * s0 + 4 * k, b = 8 * sg + 4 * k, q = m.normals;
      if (!q[a] && !q[a + 1] && !q[a + 2] && !q[a + 3]) continue;
      const x = q[a]! / 127, y = q[a + 1]! / 127, z = q[a + 2]! / 127;
      const w = [0, 1, 2].map((j) => p[j]! * x + p[3 + j]! * y + p[6 + j]! * z);
      const l = Math.max(Math.hypot(w[0]!, w[1]!, w[2]!), 1e-12);
      for (let j = 0; j < 3; j++) normals[b + j] = Math.round((w[j]! / l) * 127);
      normals[b + 3] = 0;
    }
    m.v0[e] = v, m.i0[e] = i, m.s0[e] = sg, S[e] = 0;
    v += n, i += ni, sg += ns;
  }
  return { ...m, pos, idx, edges, normals };
}

// ---- TEST1: an empty BMSH (no element; what the host answers with no model open) ----
const EMPTY_BMSH = (): ArrayBuffer => {
  const ab = new ArrayBuffer(52), dv = new DataView(ab);
  dv.setUint32(0, 0x48534d42, true); // "BMSH"
  dv.setUint32(4, 1, true); // version 1; rev, counts and origin 0
  return ab;
};
// ---- end TEST1 ----

const EMPTY_PART_BMSH: Bmsh = { rev: 0, origin: [0, 0, 0], ids: [], cats: new Uint8Array(0), v0: new Uint32Array(0), nv: new Uint32Array(0),
  i0: new Uint32Array(0), ni: new Uint32Array(0), s0: new Uint32Array(0), ns: new Uint32Array(0), pos: new Float32Array(0), idx: new Uint32Array(0),
  edges: new Float32Array(0), normals: new Int8Array(0), size: 0 };

/** SIZE2: a model's heavy arrays (vertices, indices, segments, normals), each model on its own: the open
 *  model's (indices from 0) and each link's (indices from the link's first vertex: its own BMSH's). A link's
 *  are dropped once its buffers are on the GPU (null) and fetched again when its detail comes back. */
export interface MeshPart { pos: Float32Array; idx: Uint32Array; edges: Float32Array; normals: Int8Array }
/** SIZE2FIX: a link's mesh asked after the links changed (the host answers 204): that fetch is dropped, the heads
 *  are asked again; nothing is logged. */
export class Superseded extends Error { constructor(name: string) { super(`link ${name}: superseded`); } }
/** SIZE2: a link's head in the LNKS trailer (version 3: its mesh asked on its own, GET /mesh?link=k). */
interface LinkHead { name: string; visible: boolean; c: number; s: number; at: number[] | null; colour: number; m: Bmsh | null; nE: number; key: number }

/** The LNKS trailer after the open model's BMSH: version 1 and 2 carry each link's BMSH, version 3 (SIZE2)
 *  only its head (its element count and the links' mesh key). */
export function parseHeads(ab: ArrayBuffer): { main: Bmsh; heads: LinkHead[]; lazy: boolean } {
  const main = parseBmsh(ab, 0, false);
  if (main.size === ab.byteLength) return { main, heads: [], lazy: false };
  const dv = new DataView(ab);
  let o = main.size;
  if (ab.byteLength < o + 12 || dv.getUint32(o, true) !== 0x534b4e4c) throw new Error("BMSH: trailing bytes, not LNKS");
  const ver = dv.getUint32(o + 4, true);
  if (ver !== 1 && ver !== 2 && ver !== 3) throw new Error(`LNKS version ${ver}`);
  const n = dv.getUint32(o + 8, true);
  o += 12;
  // LINKF: version 2 (and 3): per link its turn about Z (cos, sin), its placed origin (world mm) and its colour
  const heads: LinkHead[] = [];
  const dec = new TextDecoder();
  for (let k = 0; k < n; k++) {
    if (ab.byteLength < o + 4) throw new Error("LNKS truncated");
    const L = dv.getUint32(o, true);
    if (ab.byteLength < o + 12 + L) throw new Error("LNKS truncated");
    const name = dec.decode(new Uint8Array(ab, o + 4, L));
    o += 4 + L + ((4 - (L % 4)) % 4);
    const flags = dv.getUint32(o, true);
    let c = 1, sn = 0, at: number[] | null = null, colour = -1;
    if (ver >= 2) {
      if (ab.byteLength < o + 48) throw new Error("LNKS truncated");
      c = dv.getFloat64(o + 4, true), sn = dv.getFloat64(o + 12, true);
      at = [dv.getFloat64(o + 20, true), dv.getFloat64(o + 28, true), dv.getFloat64(o + 36, true)];
      colour = dv.getInt32(o + 44, true);
      o += 44;
    }
    if (ver === 3) { // SIZE2: its element count and the mesh key, no mesh
      if (ab.byteLength < o + 12) throw new Error("LNKS truncated");
      heads.push({ name, visible: (flags & 1) === 1, c, s: sn, at, colour, m: null, nE: dv.getUint32(o + 4, true), key: dv.getUint32(o + 8, true) });
      o += 12;
      continue;
    }
    const B = dv.getUint32(o + 4, true);
    o += 8;
    if (ab.byteLength < o + B) throw new Error("LNKS truncated");
    const m = parseBmsh(ab, o, false);
    if (m.size !== B) throw new Error(`LNKS ${name}: BMSH size ${B}, expected ${m.size}`);
    o += B;
    heads.push({ name, visible: (flags & 1) === 1, c, s: sn, at, colour, m, nE: m.ids.length, key: 0 });
  }
  if (o !== ab.byteLength) throw new Error(`LNKS: ${ab.byteLength - o} trailing bytes`);
  return { main, heads, lazy: ver === 3 };
}

/** The open model and its links as one mesh of rows: the links' elements after ours (`model` k + 1), each in
 *  its own frame placed by `xf` (its origin minus ours, f64; LNK3D), their rows numbered on from ours. The
 *  heavy arrays are not merged (SIZE2): `parts[k]` holds model k's own (`pos`… of the mesh are the open
 *  model's). A link without its mesh (a lazy head not fetched) has no element. */
export function mergeMesh(main: Bmsh, heads: LinkHead[], size: number): Bmsh {
  const all = [main, ...heads.map((h) => h.m ?? EMPTY_PART_BMSH)];
  const sum = (f: (m: Bmsh) => number) => all.reduce((a, m) => a + f(m), 0);
  const placed = all.some((m) => m.shape); // 3D1: version 3: shapes move by their placement, not their vertices
  const ranges: NonNullable<Bmsh["ranges"]> = []; // LNK3D (D)
  const nE = sum((m) => m.ids.length);
  const out: Bmsh = {
    rev: main.rev, origin: main.origin, ids: [], cats: new Uint8Array(nE), v0: new Uint32Array(nE), nv: new Uint32Array(nE),
    i0: new Uint32Array(nE), ni: new Uint32Array(nE), s0: new Uint32Array(nE), ns: new Uint32Array(nE),
    pos: main.pos, idx: main.idx, edges: main.edges, normals: main.normals,
    size, model: new Uint8Array(nE), links: [], parts: all.map((m) => (m.dropped ? null : { pos: m.pos, idx: m.idx, edges: m.edges, normals: m.normals })),
  };
  if (all.some((m) => m.dropped)) out.preBox = new Float32Array(6 * nE); // SIZE3 (B)
  if (placed) out.shape = new Uint32Array(nE), out.place = new Float32Array(12 * nE);
  let e0 = 0, v0 = 0, i0 = 0, s0 = 0, sh0 = 0;
  all.forEach((m, k) => {
    const pl = k > 0 ? heads[k - 1] : null, c = pl?.c ?? 1, sn = pl?.s ?? 0, o = pl?.at ?? m.origin; // LINKF: R p + (placed origin - ours)
    // LNK3D: the link stays in its own frame; its placement R p + d is the draw's (a uniform: a move changes it)
    const d = [o[0]! - main.origin[0], o[1]! - main.origin[1], o[2]! - main.origin[2]];
    if (pl) out.links!.push({ name: pl.name, visible: pl.visible, elements: m.ids.length, colour: pl.colour, xf: { c, s: sn, t: [d[0]!, d[1]!, d[2]!] }, e0 });
    let shMax = 0;
    for (let e = 0; e < m.ids.length; e++) {
      out.ids.push(m.ids[e] ?? "");
      out.cats[e0 + e] = m.cats[e] ?? 7;
      out.v0[e0 + e] = (m.v0[e] ?? 0) + v0, out.nv[e0 + e] = m.nv[e] ?? 0;
      out.i0[e0 + e] = (m.i0[e] ?? 0) + i0, out.ni[e0 + e] = m.ni[e] ?? 0;
      out.s0[e0 + e] = (m.s0[e] ?? 0) + s0, out.ns[e0 + e] = m.ns[e] ?? 0;
      out.model![e0 + e] = k;
      const sh = m.shape?.[e] ?? 0; // 3D1: a shape's vertices and segments stay in its own coordinates
      if (!sh) continue;
      shMax = Math.max(shMax, sh);
      out.shape![e0 + e] = sh + sh0;
      out.place!.set(m.place!.subarray(12 * e, 12 * e + 12), 12 * (e0 + e));
    }
    const nv = m.dropped?.nv ?? m.pos.length / 3, ni = m.dropped?.ni ?? m.idx.length, ns = m.dropped?.ns ?? m.edges.length / 6; // SIZE3 (B)
    if (m.dropped) out.preBox!.set(m.dropped.boxes, 6 * e0);
    ranges.push({ e0, e1: e0 + m.ids.length, v0, v1: v0 + nv, i0, i1: i0 + ni, s0, s1: s0 + ns });
    e0 += m.ids.length, v0 += nv, i0 += ni, s0 += ns, sh0 += shMax;
  });
  if (heads.length) out.ranges = ranges;
  return out;
}

/** GET /mesh with every link's mesh in its trailer (LNKS 1, 2) as one mesh (mergeMesh). */
export function parseMesh(ab: ArrayBuffer): Bmsh {
  const { main, heads } = parseHeads(ab);
  if (!heads.length) return main;
  return mergeMesh(main, heads, ab.byteLength);
}

// ---- small vector / matrix helpers (column-major mat4, as WebGL) ----
type V3 = [number, number, number];
const sub = (a: V3, b: V3): V3 => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const add = (a: V3, b: V3): V3 => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
const scale = (a: V3, k: number): V3 => [a[0] * k, a[1] * k, a[2] * k];
const dot = (a: V3, b: V3) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const cross = (a: V3, b: V3): V3 => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
const norm = (a: V3): V3 => scale(a, 1 / (Math.hypot(a[0], a[1], a[2]) || 1));
function mul(a: Float64Array, b: Float64Array): Float64Array {
  const o = new Float64Array(16);
  for (let c = 0; c < 4; c++)
    for (let r = 0; r < 4; r++) {
      let s = 0;
      for (let k = 0; k < 4; k++) s += (a[k * 4 + r] ?? 0) * (b[c * 4 + k] ?? 0);
      o[c * 4 + r] = s;
    }
  return o;
}
function lookAt(eye: V3, target: V3, up: V3): Float64Array {
  const f = norm(sub(target, eye)), s = norm(cross(f, up)), v = cross(s, f);
  return new Float64Array([s[0], v[0], -f[0], 0, s[1], v[1], -f[1], 0, s[2], v[2], -f[2], 0, -dot(s, eye), -dot(v, eye), dot(f, eye), 1]);
}
function perspective(fovy: number, aspect: number, near: number, far: number): Float64Array {
  const f = 1 / Math.tan(fovy / 2), nf = 1 / (near - far);
  return new Float64Array([f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, (far + near) * nf, -1, 0, 0, 2 * far * near * nf, 0]);
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
// 3D1: a placed shape's columns x, y, z, t (per instance); merged draws: the constants (1,0,0) (0,1,0) (0,0,1) (0,0,0)
layout(location = 4) in vec3 a_ix;
layout(location = 5) in vec3 a_iy;
layout(location = 6) in vec3 a_iz;
layout(location = 7) in vec3 a_it;
// LNK3D (ADR 0078 amendment 3): the element of a vertex (an instance's, per instance); 0xffffffff: none
// (proxies, outlines, the ghost: their own colour). Its state (gl/cull.ts packState: colour, model, drawn) in
// u_state. A draw holds one model's vertices (its own buffers): u_model (0: the open one) and its placement's
// three rows u_m0..u_m2 (gl/cull.ts xfRows) are the draw's uniforms.
layout(location = 8) in uint a_el;
uniform highp sampler2D u_state;
uniform int u_model;
uniform vec4 u_m0, u_m1, u_m2;
// VIS2: the render origin (scene mm): the GPU works relative to it (u_mvp includes its translation, a link's u_m*.w are
// its placement minus it), so f32 keeps sub-millimetre steps near geometry kilometres from the scene's origin
uniform vec3 u_ro;
// LNK3D: a draw whose elements are all shown in one colour (a link with nothing hidden, selected or revealed)
// reads no state: u_plain 1, its colour u_plain_col (a texel fetch a vertex is a cost on a CPU rasteriser)
uniform int u_plain;
uniform vec3 u_plain_col;
uniform int u_el_col; // 1: the element's colour (faces); 0: a_col (edges, a constant)
uniform mat4 u_mvp;
uniform vec3 u_eye;
uniform float u_eye_dir; // 1: u_eye is the direction to the eye (an orthographic plan camera)
uniform float u_point;
uniform float u_zbias; // NDC depth pulled toward the eye (a plan's edges: over a coplanar cut face)
out vec3 v_pos;
out vec3 v_col;
flat out float v_back;
flat out uint v_el; // VIS1: the element (its id drawn by the ground-truth pass)
void main() {
  v_el = a_el;
  vec3 p = a_ix * a_pos.x + a_iy * a_pos.y + a_iz * a_pos.z + a_it;
  mat3 lin = mat3(a_ix, a_iy, a_iz); // the face normals turn as cc_scene_world_mesh turns them
  vec3 col = a_col;
  if (a_el != 0xffffffffu && u_plain == 1) { if (u_el_col == 1) col = u_plain_col; }
  else if (a_el != 0xffffffffu) { // LNK3D: hidden: outside the clip volume (no pixel, no rebuild)
    vec4 st = texelFetch(u_state, ivec2(int(a_el & 2047u), int(a_el >> 11u)), 0);
    if ((int(st.a * 255.0 + 0.5) & 1) == 0) { v_pos = vec3(0.0); v_col = vec3(0.0); v_back = 0.0; gl_Position = vec4(2.0, 2.0, 2.0, 1.0); return; }
    if (u_el_col == 1) col = st.rgb;
  }
  if (u_model != 0) { // LNK3D: the model's placement (a link moved, turned: these uniforms only)
    p = vec3(dot(u_m0.xyz, p) + u_m0.w, dot(u_m1.xyz, p) + u_m1.w, dot(u_m2.xyz, p) + u_m2.w);
    lin = transpose(mat3(u_m0.xyz, u_m1.xyz, u_m2.xyz)) * lin;
  } else p -= u_ro; // (the open model's vertices, outlines, the ghost: scene coordinates)
  v_pos = p; v_col = col;
  vec3 to_eye = u_eye_dir > 0.5 ? u_eye : u_eye - p;
  v_back = (dot(lin * a_n1.xyz, to_eye) < 0.0 && dot(lin * a_n2.xyz, to_eye) < 0.0) ? 1.0 : 0.0;
  gl_PointSize = u_point;
  gl_Position = u_mvp * vec4(p, 1.0);
  gl_Position.z -= u_zbias * gl_Position.w;
}`;
const FS = `#version 300 es
precision highp float;
in vec3 v_pos;
in vec3 v_col;
flat in float v_back;
flat in uint v_el;
uniform int u_cheap; // VIS1 (a bench hook): 1: the colour unlit, no sRGB conversion, no derivative normal
uniform int u_id; // VIS1: 1: the element's index + 1 as the colour (24 bits; the ground-truth pass), 0: shaded
uniform vec3 u_light;
uniform int u_clip;      // 1: the section box is on
uniform vec3 u_clip_min; // scene coordinates
uniform vec3 u_clip_max;
uniform vec2 u_clip_rot; // ISS71: (cos, sin) of the box's yaw: the fragment turned by -yaw into its frame
uniform float u_cut;     // brightness of cut surfaces (back faces while the box is on)
uniform float u_ambient; // < 0: unlit (edges)
uniform float u_alpha;
uniform highp int u_model; // VIS1: the draw's model (the element attachment's alpha)
layout(location = 0) out vec4 o;
layout(location = 1) out vec4 o_id; // VIS1 (gl/target.ts): the element (index + 1; 0xffffff: none) and the model + 1
layout(location = 2) out vec4 o_n;  // VIS1: the face's normal (0.5 + n / 2)
vec3 to_linear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c)); }
vec3 to_srgb(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c)); }
void main() {
  vec3 q = vec3(u_clip_rot.x * v_pos.x + u_clip_rot.y * v_pos.y, u_clip_rot.x * v_pos.y - u_clip_rot.y * v_pos.x, v_pos.z);
  if (u_clip == 1 && (any(lessThan(q, u_clip_min)) || any(greaterThan(q, u_clip_max)))) discard;
  if (u_id == 1) { uint k = v_el + 1u; o = vec4(float(k & 255u), float((k >> 8u) & 255u), float((k >> 16u) & 255u), 255.0) / 255.0; return; }
  if (u_ambient < 0.0) {
    if (u_clip == 1 && v_back > 0.5) discard; // a back edge, only ever seen through the cut
    o = vec4(v_col, u_alpha);
    return;
  }
  vec3 n = cross(dFdx(v_pos), dFdy(v_pos));
  float len2 = dot(n, n);
  uint ke = v_el == 0xffffffffu ? 0xffffffu : v_el + 1u;
  o_id = vec4(float(ke & 255u), float((ke >> 8u) & 255u), float((ke >> 16u) & 255u), float(u_model + 1)) / 255.0;
  o_n = vec4((len2 > 1e-30 ? n * inversesqrt(len2) : vec3(0.0, 0.0, 1.0)) * 0.5 + 0.5, 1.0);
  if (u_cheap == 1) { o = vec4(v_col, 1.0); return; }
  if (u_clip == 1 && !gl_FrontFacing) { o = vec4(to_srgb(to_linear(v_col) * u_cut), u_alpha); return; }
  float lambert = len2 > 1e-30 ? abs(dot(n, u_light)) * inversesqrt(len2) : 1.0;
  float k = u_ambient + (1.0 - u_ambient) * lambert;
  o = vec4(to_srgb(to_linear(v_col) * k), u_alpha);
}`;

interface GlStats {
  /** ms from navigation start to the first frame drawn with a mesh. */
  firstFrameMs: number | null;
  /** 3D2 (step 5): when the last mesh's bytes arrived, and when its picture was first complete (every
   *  batch drawn: a whole frame, or the last pass of the fill that follows a budgeted first frame). */
  meshAt?: number;
  /** 3D2 (step 5): when a large mesh's element boxes were drawn, before its draw lists. */
  previewAt?: number;
  fullFrameMs?: number | null;
  /** 3D2 (step 5): ms of the last mesh's parse, upload, draw lists and colours. */
  loadSteps?: { parse: number; upload: number; lists: number; colours: number };
  firstFrame?: { moving: boolean; tris: number; draws: number; budget: number; t: number; ms: number };
  /** 3D2: the frames after a mesh's first (time, moving / slice / whole, cut, triangles, fill state), 80 at most. */
  frameLog?: string[];
  meshBytes: number;
  meshFetchMs: number;
  meshRev: number | null;
  frames: number;
  /** MEAS1: frames that drew the 2D overlay alone (a measure pointer move), not the model. */
  overlayFrames: number;
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
  /** SETOUT: the setting-out markers last drawn (overlay CSS px) and when (performance.now()). */
  setout?: MarkerDrawn[];
  setoutAt?: number;
  /** WAVE1B: the reference planes last drawn (overlay CSS px). */
  refplanes?: RefPlaneDrawn[];
  /** WAVE1B (B2): the selected opening's box drawn in the last frame. */
  openingDrawn?: boolean;
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
  /** VIEWSHEET: the section tool (action draw_section): its first point and the pointer (world mm), the
   *  view it made last, its error, the time of the second click (performance.now) and of the frame after it. */
  section: { a: number[] | null; hover: number[] | null; added: string | null; error: string | null; clickAt: number | null; drawnAt: number | null };
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
  q2?: { edges: boolean; ortho: boolean; edgeSegs: number;
    /** VIS1: how the edges were drawn: "screen" (found on screen: no segments), "lines" (edgeSegs counts them), "none". */
    edgeMode?: "screen" | "lines" | "none" };
  /** BUG1: frame loops run (each one takes the page's state and draws it when it changed), and what the last
   *  draw drew: its frame number, the mesh's rev, the buffer size. A test reads the pixels after a loop that
   *  began after its action, with the state's rev drawn at the canvas's layout size (no fixed sleep). */
  loops?: number;
  drawn?: { frame: number; rev: number | null; w: number; h: number };
  /** 3D1 (ADR 0078, ccore/tools/frames.mjs): what the last model draw submitted (triangles, draw calls of
   *  faces and edges), the bytes the model's GPU buffers hold, and the benchmark's hook: `frame(dyaw,
   *  moving)` turns the camera by dyaw radians, draws at once and waits for the pixels (a 1x1 readPixels),
   *  returning its ms; `moving` says the camera is moving (a drag frame) or still. */
  frameTris?: number;
  frameDraws?: number;
  gpuBytes?: number;
  /** 3D1: the moving budget of the last frame (px at the target, the size left out under in mm), the
   *  batches and how many drew nothing; a test hook to turn frustum culling off (the same picture). */
  moving?: { moving: boolean; slice: boolean; budget: number; t: number; batches: number; culled: number; outlines: number; tris: number; proxied?: number;
    /** VIS2: the triangles the cut chose (what may be drawn of them) and what remains after it. */
    cutKept?: number; cutRem?: number;
    /** VIS1 (P0-a): in motion (the camera changed within the motion window); capped: moving_fps 0 kept under 100 ms. */
    motion?: boolean; capped?: boolean };
  /** VIS1 (P0-a): the last frame's time, draw start to the GPU's end (a fence polled each loop). */
  frameMs?: number;
  /** VIS1: the frames drawn (gl/frametime.ts; the last 2,000) and whether the GPU's time comes from a timer query. */
  frameRecs?: FrameRec[];
  gpuTimer?: boolean;
  noCull?: boolean;
  /** VIS1 (bench hooks): back faces culled in the face pass; a still frame leaves out what is under stillPx pixels. */
  cullBack?: boolean;
  stillPx?: number;
  /** VIS1 (bench hooks): every model drawn without reading the state texture (one grey); faces unlit, no sRGB. */
  noState?: boolean;
  cheapFrag?: boolean;
  /** VIS1: edges found on screen in 3D (on, off, failed: the depth could not be copied); lineEdges, a bench hook: the line pass. */
  screenEdges?: string;
  lineEdges?: boolean;
  /** VIS1: where the last pick came from (id: the element attachment, then that element's triangles; id-background; scan),
   *  and a bench hook (false: always the scan). */
  pickFrom?: string;
  idPick?: boolean;
  /** VIS1: "draw only what can be seen" (false: off, a bench hook until the setting) and what the last frame left out:
   *  whether a pyramid of this camera was there and used, the items and triangles left out, the batches tested, ms. */
  visible?: boolean;
  vis?: { on: boolean; used: boolean; pyramid: boolean; hidden: number; hiddenTris: number; masked: number; ms: number;
    /** VIS2: why the pyramid was not used: "none" (none back yet; "pending": asked), else the parts of its key that differ
     *  from this frame's (camera, size, shown, lists, box, links, detail): a stale pyramid is never used. */
    why?: string };
  /** VIS1 (a bench hook): every slice of the fill presented as it is drawn (before: one by one on screen). */
  showSlices?: boolean;
  /** VIS1 (a bench hook): the merged ranges nearest first (off: Apple's GPUs remove hidden surfaces per tile, so the order
   *  gains nothing there, and with culling it changed which of two coplanar faces won: edges found on screen moved). */
  frontToBack?: boolean;
  /** VIS1 (a bench hook): a visibility change's feedback drawn as a whole frame whatever the model's size. */
  wholeFeedback?: boolean;
  /** VIS1 (a bench hook): the budget's cut by bisection (budgetCut) whatever the number of batches. */
  exactCut?: boolean;
  /** VIS1: per model whether its draws skip the state texel (u_plain). */
  plain?: boolean[];
  /** 3D2 (ADR 0078): HLOD proxies: the tree served (cells, cells with a proxy, proxy triangles, bytes, the
   *  steps asked and the ms from the first ask to the upload), what the last frame drew of it (cells, triangles), and a test hook
   *  to draw without them (the full-detail picture). */
  proxies?: { cells: number; withProxy: number; tris: number; bytes: number; steps: number; maxStepMs: number; loadMs: number;
    drawnCells: number; drawnTris: number; standIns: number; on: boolean };
  noProxies?: boolean;
  /** VIS2 (a test hook): true releases a link's detail when its proxies come late (SIZE3's way), not kept until the budget does. */
  lateProxyRelease?: boolean;
  /** 3D2 (step 3): occlusion queries on (a test hook, off by default) and the last frame's queries asked
   *  and batches left out as hidden. */
  occlusion?: boolean;
  occ?: { asked: number; hidden: number };
  /** 3D2 (step 2): a bench hook, frames without edges (what the edge pass costs). */
  noEdges?: boolean;
  /** LNK3D (F): a bench hook, the proxies' crease edges drawn as 3D2 drew them. */
  proxyEdges?: boolean;
  /** 3D2 (step 4): selections drawn over the kept picture (the elements whose colour changed), not as a whole frame. */
  overpaints?: number;
  /** LNK3D (ADR 0078 amendment 3): draw lists built (a mesh, the proxies' tree; never a hide), the last
   *  visibility change's ms in the page, the last state texture upload (elements in the rows sent, ms),
   *  /mesh fetches made, and per link what the draw uses (shown, colour, placement). */
  listsBuilt?: number;
  visApplyMs?: number;
  stateUpload?: { elements: number; ms: number };
  meshFetches?: number;
  /** LNK3D (C): each link's baked proxies held (cells, triangles, GPU bytes) and what the last frame drew of them;
   *  the first picture drawn from them before the mesh (when, links, triangles); ms to ask them. */
  linkProxies?: { name: string; cells: number; tris: number; bytes: number; drawnCells: number; drawnTris: number }[];
  proxyPreview?: { at: number; links: number; tris: number };
  linkProxiesMs?: number;
  linkDraw?: { name: string; visible: boolean; colour: number; xf: ModelXf }[];
  /** LNK3D (E): the notes on linked elements as last drawn (client px of the anchor) and the selected one. */
  notes?: { link: string; element: string; text: string; drawn: boolean; x: number; y: number }[];
  noteSelected?: number | null;
  /** LNK3D (D): the GPU budget across links (bytes; a test hook: unset, automatic from memory_share), each link's
   *  detail on the GPU or not (its bytes, its uploads), the last upload of a link's detail (ms). */
  gpuBudget?: number;
  /** REAL1 (gl/farlinks.ts): links left out of the scene's bounds (Fit all, the clip planes): thousands of km away. */
  farLinks?: { name: string; km: number }[];
  linkDetail?: { name: string; detail: boolean; bytes: number; loads: number }[];
  linkReloadMs?: number;
  /** SIZE2FIX: links' meshes found superseded (the links changed while asked): dropped and asked again, no error. */
  linkSuperseded?: number;
  /** SIZE2: the mesh bytes served to this page in all (/mesh and each link's /mesh?link=k, refetches too). */
  meshBytesServed?: number;
  /** SIZE2: links' meshes asked again (a released link's detail back), the last one's ms; picks read back from the GPU. */
  linkRefetches?: number;
  linkRefetchMs?: number;
  gpuPickReads?: number;
  /** SIZE3 (E): links whose proxies came after they were drawn (baked once they were ready). */
  linkProxiesLate?: number;
  /** SIZE3 (B): the open's decoded link arrays at most at once and those kept to upload (bytes); when every link a
   *  frame wanted in detail was on the GPU, none asked (performance.now(), null until then); the decodes made in
   *  the worker and on the page's thread, the longest slice on the page's thread (ms). */
  openPeakDecoded?: number;
  /** SIZE3 (B): a test hook: the open's decoded budget (bytes; unset: OPEN_KEEP_BYTES). */
  openKeepBytes?: number;
  /** SIZE3 (B): a test hook: false opens links from their meshes within openKeepBytes, not from the host's rows answer. */
  openRows?: boolean;
  /** SIZE3 (B): the last link detail asked again: fetch, read (worker), upload ms. */
  refetchSteps?: { fetch: number; decode: number; upload: number };
  openKeptBytes?: number;
  /** SIZE3 (C): the links' GPU budget in force (bytes: the setting link_gpu_mb, or the test hook gpuBudget) and what their detail takes. */
  linkGpuBudget?: number;
  linkGpuUsed?: number;
  linksDetailedAt?: number | null;
  decodes?: { worker: number; page: number; maxPageSliceMs: number };
  /** GPU2: under WebGPU, the bytes each renderer holds and the models taken over (gpu/render.ts). */
  gpuSplit?: { webgpu: number; webgl: number; taken: number; models: number };
  /** SIZE2: the heavy arrays the page keeps (bytes), per model (the open model first). */
  partBytes?: number[];
  /** 3D2 (step 4): ms of the last pick's ray scan in the page (pickHit). */
  pickMs?: number;
  /** 3D2: true while the proxies are being asked for (frames.mjs waits for them). */
  proxiesPending?: boolean;
  bench?: { frame: (dyaw: number, moving: boolean) => number; zoom: (k: number) => void; snap: (dyaw: number, dpitch: number) => { w: number; h: number; rgba: Uint8Array };
    look?: (t: number[], d: number) => void; read?: () => { w: number; h: number; rgba: Uint8Array };
    stateCost?: (n: number, v: number) => { texMs: number; texBytes: number; bufMs: number; bufBytes: number };
    /** VIS1: the ground truth (idCount) and the never-missing check (missing). */
    idCount?: () => { w: number; h: number; pixels: number; elements: number; tris: number; submitted: number; ms: number };
    missing?: () => { n: number; first: string[] } | null;
    /** VIS2: diagnosis of the missing elements and of a link's element boxes against its real vertices. */
    whyMissing?: (max?: number) => { kinds: Record<string, number>; first: unknown[] } | null;
    boxAudit?: (k: number, tol?: number) => Promise<unknown>;
    drawCalls?: (n: number, reps?: number, multi?: boolean) => { n: number; draws: number; tris: number; models: number; submit: number; total: number } | null };
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
  /** 3D2 (ADR 0078): the HLOD proxies in steps (GET /mesh?proxies=N, wasm host_proxies): `budget`
   *  triangles more a call; 202 while building, 200 with the BPRX bytes, else none served. */
  proxies?(budget: number): Promise<{ status: number; bytes: ArrayBuffer; rev: number }>;
  /** LNK3D (ADR 0078 amendment 3): link k's proxies baked in its cache (GET /mesh?link_proxies=k): 200 with
   *  BPRX (rev 0, nE its elements), else none. */
  linkProxies?(k: number): Promise<{ status: number; bytes: ArrayBuffer }>;
  /** SIZE2: link k's mesh on its own (GET /mesh?link=k, BMSH version 4 read back as version 3 by gl/bmsh4.ts),
   *  with the bytes served. Present: `mesh()` answers the links' heads only (LNKS version 3). */
  linkMesh?(k: number, name: string, key: number, keep?: number): Promise<{ bytes?: ArrayBuffer; rows?: Bmsh4Rows; served: number }>;
  /** SIZE3 (B): link k's rows and its elements' boxes from the host (no mesh): what an open needs before any detail. */
  linkRows?(k: number, name: string, key: number): Promise<{ rows?: Bmsh4Rows; served: number }>;
}

/** Draws the host's `/mesh` (or `source`'s) into #bimble with WebGL2. Returns a function that marks the view dirty. */
export function glViewport(base: string | MeshSource, token = ""): () => void {
  const canvas = document.getElementById("bimble");
  const stats: GlStats = { firstFrameMs: null, meshBytes: 0, meshFetchMs: 0, meshRev: null, frames: 0, overlayFrames: 0, picks: 0, error: null,
    toolCalls: 0, ghostTris: 0, hover: null, toolError: null, placed: null,
    hiddenElements: 0, drawnTris: 0, clip: null, boxFace: null, boxCalls: 0, boxError: null, menus: 0, boxHandles: [], boxHover: null, links: [], linkTris: 0,
    annotations: null, overlayTexts: [], selectedAnnotation: null, annotate: null, annotateCalls: 0, annotError: null, annotAdded: null,
    measure: null, measureCalls: 0, measureError: null,
    section: { a: null, hover: null, added: null, error: null, clickAt: null, drawnAt: null }, // VIEWSHEET
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
  perfInputs(canvas); // GPU1
  // ---- GPU1 (ADR 0083): the WebGPU renderer when asked (gpu/switch.ts; null: WebGL2 draws). It only turns the
  // scene, the camera and the states into pixels on its own canvas; everything else of the viewport is shared. ----
  let gpuR: GpuRenderer | null = null;
  gpuSwitch(canvas, stats, (r) => { gpuR = r; dirty = true; }, () => gpuView());
  // ---- GPU2 (ADR 0083, amended): WebGPU draws the viewport's own models (each link in its own buffers, placed by its
  // row, its elements' state texels, its proxies, the memory budget) and takes their buffers over from WebGL2's:
  // read back once, WebGL2's emptied (never both on the GPU), given back before WebGL2 draws again ----
  const GL_BUFS = ["pos", "el", "edge", "edgeNrm", "edgeEl", "shapeIdx", "idx", "edgeIdx", "instMat", "instEl"] as const;
  const gpuOwned = new WeakSet<object>(); // ModelGl whose buffers WebGPU holds (WebGL2's emptied)
  let prxEmptied = false, ghostKey: { tool: unknown; n: number } | null = null, ghostArrays: ReturnType<GpuModels["ghost"]> = null;
  const glRead = (b: WebGLBuffer): ArrayBuffer | null => {
    gl.bindBuffer(gl.COPY_READ_BUFFER, b);
    const n = gl.getBufferParameter(gl.COPY_READ_BUFFER, gl.BUFFER_SIZE) as number;
    const out = n > 0 ? new ArrayBuffer(n) : null;
    if (out) gl.getBufferSubData(gl.COPY_READ_BUFFER, 0, new Uint8Array(out));
    gl.bindBuffer(gl.COPY_READ_BUFFER, null);
    return out;
  };
  const glPut = (b: WebGLBuffer, d: ArrayBuffer | null) => {
    gl.bindBuffer(gl.COPY_WRITE_BUFFER, b);
    if (d) gl.bufferData(gl.COPY_WRITE_BUFFER, new Uint8Array(d), gl.STATIC_DRAW);
    else gl.bufferData(gl.COPY_WRITE_BUFFER, 0, gl.STATIC_DRAW);
    gl.bindBuffer(gl.COPY_WRITE_BUFFER, null);
  };
  const gpuModels = (): GpuModels => ({
    slots, dbs, groups, states: stateTexels, statesKey: stats.stateUpload, xf: modelXf, plain: plainOf, lboxes, listsKey: stats.listsBuilt ?? 0,
    linkedSel: state.value.linked ? { link: state.value.linked.link, id: state.value.linked.id } : null,
    linkPrx: modelPrx, prePrx, linkColour: (c) => LINK_SRGB[c] ?? REFERENCE_SRGB,
    cutLink: (x, xf, usable, planes, eye, px) => cutLinkPrx(x as LinkPrx, xf, usable, planes, eye as V3, px),
    need: (k) => { const x = slots[k]; if (x && k > 0) x.needFrame = drawFrame, x.needAt = performance.now(); },
    want: (k, sz) => { wanted.add(k); if (sz > (wantScore.get(k) ?? 0)) wantScore.set(k, sz); }, // (SIZE3 (B): the largest on screen asked first)
    notWanted: (k) => wantedLast.get(k) === false,
    readGl: (g) => { // (each buffer streamed in chunks by its reader: never a whole copy in the page)
      const mg = g as ModelGl, out: GlStream[] = [];
      for (const n of GL_BUFS) {
        const b = mg[n];
        gl.bindBuffer(gl.COPY_READ_BUFFER, b);
        const size = gl.getBufferParameter(gl.COPY_READ_BUFFER, gl.BUFFER_SIZE) as number;
        gl.bindBuffer(gl.COPY_READ_BUFFER, null);
        if (size > 0) out.push({ name: n, size, read: (off, dst) => { gl.bindBuffer(gl.COPY_READ_BUFFER, b); gl.getBufferSubData(gl.COPY_READ_BUFFER, off, dst); gl.bindBuffer(gl.COPY_READ_BUFFER, null); } });
      }
      return out;
    },
    emptyGl: (g) => { for (const n of GL_BUFS) glPut((g as ModelGl)[n], null); gpuOwned.add(g); if (!prxEmptied) gpuModels().emptyPrx(); },
    fillGl: (g, n, size) => {
      const b = (g as ModelGl)[n];
      gl.bindBuffer(gl.COPY_WRITE_BUFFER, b);
      gl.bufferData(gl.COPY_WRITE_BUFFER, size, gl.STATIC_DRAW);
      gl.bindBuffer(gl.COPY_WRITE_BUFFER, null);
      return (off, src) => { gl.bindBuffer(gl.COPY_WRITE_BUFFER, b); gl.bufferSubData(gl.COPY_WRITE_BUFFER, off, src); gl.bindBuffer(gl.COPY_WRITE_BUFFER, null); };
    },
    ownGl: (g, gpu) => { if (gpu) gpuOwned.add(g); else gpuOwned.delete(g); },
    emptyPrx: () => {
      prxEmptied = true;
      for (const x of linkPrx.values()) for (const b of x.bufs) glPut(b, null);
      for (const b of [prxPos, prxCol, prxIdx, prxEdge, prxNrm]) glPut(b, null);
    },
    fillPrx: () => {
      if (!prxEmptied) return;
      prxEmptied = false;
      for (const x of linkPrx.values()) glPut(x.bufs[0]!, x.p.pos.slice().buffer), glPut(x.bufs[1]!, x.p.idx.slice().buffer);
      if (prx) uploadProxies(prx);
    },
    recover: () => { // the device lost with WebGL2's buffers emptied: the open model from the page, the links asked back
      for (const x of slots) if (x.gl && gpuOwned.has(x.gl)) { gpuOwned.delete(x.gl); if (x.k === 0) loadSlot(x); else releaseSlot(x); }
      gpuModels().fillPrx();
      dirty = true;
    },
    glHeld: () => 4 * STATE_TEX_W * stateRows + (prxEmptied ? 0 : prxBytes + [...linkPrx.values()].reduce((n, x) => n + x.bytes, 0)) +
      slots.reduce((n, x) => n + (x.gl && !gpuOwned.has(x.gl) ? x.bytes : 0), 0),
    ghost: () => {
      if (!ghostN && !markN) return null;
      const n = ghostN * 7 + ghostEdgeN * 3 + markN;
      if (!ghostKey || ghostKey.tool !== tool || ghostKey.n !== n) { // (read back once a tool's answer: small)
        ghostKey = { tool, n };
        const f = (b: WebGLBuffer, k: number) => { const d = k ? glRead(b) : null; return d ? new Float32Array(d) : new Float32Array(0); };
        const i = ghostN ? glRead(ghostIdx) : null;
        ghostArrays = { key: ghostKey, pos: f(ghostPos, ghostN), idx: i ? new Uint32Array(i, 0, ghostN) : new Uint32Array(0), edges: f(ghostEdges, ghostEdgeN).subarray(0, 3 * ghostEdgeN), marks: f(markBuf, markN).subarray(0, 3 * markN) };
      }
      return ghostArrays;
    },
  });
  const gpuView = (): GpuView => {
    const { e, mvp } = matrices(), m = mesh, sel = new Set(state.value.linked ? [] : state.value.selection), k = (2 * Math.tan(FOV_Y / 2)) / Math.max(1, canvas.height);
    return { mesh: m, boxes, shown, colour: elColour, selected: (el) => sel.has(m?.ids[el] ?? ""), prx, proxiesOn: proxiesOn(), mvp, ro: [...ro], eye: e, target,
      ortho: plan !== null || ortho3d(), plan: plan !== null, mirror: mirror(), clip: clipBox(), edges: pressed("edges", true), camKey: camKey(),
      held: () => drag !== null, fps: userSettings.value?.values.moving_fps ?? 30, mmPerPx: plan ? () => (2 * half) / Math.max(1, canvas.height) : ortho3d() ? () => distance * k : (d) => d * k,
      zbias: plan ? (2 * 2) / (2 * (4 * (bounds?.radius ?? distance) + 1000) - 1) : 0, w: canvas.width, h: canvas.height,
      noCull: stats.noCull === true, noProxies: stats.noProxies === true, noEdges: stats.noEdges === true, models: gpuModels() };
  };
  /** The frame drawn by WebGPU (true), or by WebGL2 below (while WebGPU takes the open model over). */
  const gpuDraw = (): boolean => {
    if (!gpuR) return false;
    if (state.value.file === null && !mesh) { gpuR.show(false); return false; } // (no model: WebGL2 clears the view)
    const v = gpuView();
    const ready = !gpuR.prepare || gpuR.prepare(v); // GPU2: WebGL2 draws until WebGPU holds the open model
    if (ready && (perfRenderer.using !== "WebGPU" || perfRenderer.reason)) perfRenderer.using = "WebGPU", perfRenderer.reason = null;
    gpuR.show(ready);
    if (!ready) { dirty = true; return false; }
    drawFrame++; // SIZE2FIX: the budget's clock counts WebGPU's frames too
    gpuR.frame(v);
    if (perfRenderer.progress) dirty = true; // GPU2: links still being taken over (or given back): another frame
    const b = clipBox();
    stats.boxHandles = b && !plan ? boxHandles(b) : [];
    drawOverlay();
    stats.frames++;
    perfFrame(gpuR.last.moving, gpuR.last.t0, gpuR.done());
    if (stats.firstFrameMs === null && mesh) stats.firstFrameMs = performance.now();
    return true;
  };
  // ---- end GPU1 ----
  const source: MeshSource = typeof base !== "string" ? base : {
    epoch: async () => {
      const r = await fetch(`${base}/change?since=0&t=${t}`, { cache: "no-store" });
      const m = (await r.json()) as { epoch?: unknown };
      return typeof m.epoch === "string" ? m.epoch : null;
    },
    mesh: async () => {
      // 3D1: BMSH version 3; SIZE2: the links' heads only (lazy=1), each link's mesh asked on its own (linkMesh)
      const r = await fetch(`${base}/mesh?t=${t}&shapes=1&lazy=1`, { cache: "no-store" });
      if (!r.ok) throw new Error(`mesh: HTTP ${r.status} ${await r.text()}`);
      return r.arrayBuffer();
    },
    linkMesh: async (k: number, name: string, key: number, keep?: number) => { // SIZE2; SIZE2FIX: by its name and the heads' key; SIZE3: `keep`: past these decoded bytes, its rows and boxes only
      const r = await fetch(`${base}/mesh?t=${t}&link=${k}&name=${encodeURIComponent(name)}&key=${key}`, { cache: "no-store" });
      if (r.status === 204) throw new Superseded(name); // removed, replaced or read again since the heads: not an error
      if (!r.ok) throw new Error(`mesh of link ${k}: HTTP ${r.status} ${await r.text()}`);
      const ab = await r.arrayBuffer();
      const served = ab.byteLength;
      if (keep !== undefined && bmsh3Size(ab) > keep) return { rows: await bmsh4RowsDecode(ab, INSTANCE_MIN_COPIES, INSTANCE_MIN_TRIS), served };
      return { bytes: await bmsh4Decode(ab, 2 ** 31), served }; // SIZE3: off the page's thread
    },
    linkRows: async (k: number, name: string, key: number) => { // SIZE3 (B): by its name and the heads' key, as linkMesh
      const r = await fetch(`${base}/mesh?t=${t}&link=${k}&rows=1&name=${encodeURIComponent(name)}&key=${key}`, { cache: "no-store" });
      if (r.status === 204) throw new Superseded(name);
      if (!r.ok) throw new Error(`rows of link ${k}: HTTP ${r.status} ${await r.text()}`);
      const ab = await r.arrayBuffer();
      return { rows: bmsh4RowsBoxes(ab, INSTANCE_MIN_COPIES, INSTANCE_MIN_TRIS), served: ab.byteLength };
    },
    proxies: async (budget: number) => { // 3D2
      const r = await fetch(`${base}/mesh?t=${t}&proxies=${budget}`, { cache: "no-store" });
      return { status: r.status, bytes: await r.arrayBuffer(), rev: Number(r.headers.get("X-Rev") ?? -1) };
    },
    linkProxies: async (k: number) => { // LNK3D
      const r = await fetch(`${base}/mesh?t=${t}&link_proxies=${k}`, { cache: "no-store" });
      return { status: r.status, bytes: await r.arrayBuffer() };
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
  const uRo = gl.getUniformLocation(prog, "u_ro"); // VIS2
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
  const uId = gl.getUniformLocation(prog, "u_id"), uCheap = gl.getUniformLocation(prog, "u_cheap"); // VIS1
  // 3D1: outside an instanced draw a vertex is placed by the identity (generic attribute values: context state)
  gl.vertexAttrib3f(4, 1, 0, 0), gl.vertexAttrib3f(5, 0, 1, 0), gl.vertexAttrib3f(6, 0, 0, 1), gl.vertexAttrib3f(7, 0, 0, 0);
  // ---- LNK3D (ADR 0078 amendment 3): the per-element state texture (unit 1) and the per-model placements
  // (as uniforms, per draw: setModel); a draw without element ids reads STATE_NONE (generic attribute: context state) ----
  gl.vertexAttribI4ui(8, STATE_NONE, 0, 0, 0);
  const uState = gl.getUniformLocation(prog, "u_state");
  const uModel = gl.getUniformLocation(prog, "u_model"), uElCol = gl.getUniformLocation(prog, "u_el_col");
  const uM = [gl.getUniformLocation(prog, "u_m0"), gl.getUniformLocation(prog, "u_m1"), gl.getUniformLocation(prog, "u_m2")];
  const uPlain = gl.getUniformLocation(prog, "u_plain"), uPlainCol = gl.getUniformLocation(prog, "u_plain_col");
  /** Per model: its one colour when every element of it is shown in it (u_plain), else null (colours()). */
  let plainOf: ([number, number, number] | null)[] = [];
  const NO_STATE_SRGB: [number, number, number] = [200, 200, 200];
  const stateTex = gl.createTexture();
  for (const [unit, tex] of [[1, stateTex]] as const) {
    gl.activeTexture(gl.TEXTURE0 + unit);
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  }
  gl.activeTexture(gl.TEXTURE0);
  gl.useProgram(prog);
  gl.uniform1i(uState, 1), gl.uniform1i(uModel, 0), gl.uniform1i(uElCol, 1);
  /** The state texels (RGBA8, STATE_TEX_W a row) and the rows the state texture holds. */
  let stateTexels = new Uint8Array(0), stateRows = 0;
  /** Uploads the state texels of elements [a, b) (whole rows; all when the texture is too small). */
  const uploadStates = (a = 0, b = Infinity) => {
    const n = stateTexels.length / 4, rows = Math.max(1, Math.ceil(n / STATE_TEX_W));
    gl.activeTexture(gl.TEXTURE1);
    gl.bindTexture(gl.TEXTURE_2D, stateTex);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    if (rows !== stateRows || stateTexels.length < 4 * rows * STATE_TEX_W) {
      const all = new Uint8Array(4 * rows * STATE_TEX_W);
      all.set(stateTexels);
      stateTexels = all, stateRows = rows;
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, STATE_TEX_W, rows, 0, gl.RGBA, gl.UNSIGNED_BYTE, stateTexels);
    } else {
      const r0 = Math.floor(Math.max(0, a) / STATE_TEX_W), r1 = Math.min(rows, Math.ceil(Math.min(b, n) / STATE_TEX_W));
      if (r1 > r0) gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, r0, STATE_TEX_W, r1 - r0, gl.RGBA, gl.UNSIGNED_BYTE, stateTexels.subarray(4 * STATE_TEX_W * r0, 4 * STATE_TEX_W * r1));
    }
    gl.activeTexture(gl.TEXTURE0);
  };
  /** Each model's placement: the open one, then the links; a draw of model k sets its uniforms (setModel). */
  let modelXf: ModelXf[] = [IDENTITY_XF];
  const uploadModels = () => { /* the next frame's draws read modelXf */ };
  const xfRow = new Float32Array(12);
  // ---- VIS2: the render origin. The scene's frame is the open model's (its origin); a project whose open model is empty
  // and whose links are georeferenced (2,536 km from it) put every vertex at 2.5e9 mm, where f32 steps are 256 mm: the
  // picture's depth and the CPU's boxes disagreed by that much and the culling left visible elements out. The GPU now
  // draws relative to a point near the geometry (gl/origin.ts); the CPU keeps the scene frame in f64. ----
  let ro: V3 = [0, 0, 0];
  const roFor = (b: { center: V3 } | null): V3 => renderOrigin(b ? b.center : null);
  const gpuMvp = (m: ArrayLike<number>): Float32Array => relMvp(m, ro);
  /** The frame's camera uniforms: the matrix, the render origin, the eye (relative to it unless a direction). */
  const setCamera = (mvp: ArrayLike<number>, eyeU: V3, dir: boolean) => {
    gl.uniformMatrix4fv(uMvp, false, gpuMvp(mvp));
    gl.uniform3f(uRo, ro[0], ro[1], ro[2]);
    gl.uniform3fv(uEye, dir ? eyeU : sub(eyeU, ro));
  };
  const setModel = (k: number) => {
    gl.uniform1i(uModel, k);
    const pc = stats.noState ? NO_STATE_SRGB : k ? plainOf[k] ?? null : null; // (VIS1: noState, a bench hook)
    gl.uniform1i(uPlain, pc ? 1 : 0);
    if (pc) gl.uniform3f(uPlainCol, pc[0] / 255, pc[1] / 255, pc[2] / 255);
    if (!k) return;
    const x = modelXf[k] ?? IDENTITY_XF; // VIS2: its translation relative to the render origin (f64 here, small for the GPU)
    xfRows({ c: x.c, s: x.s, t: [x.t[0] - ro[0], x.t[1] - ro[1], x.t[2] - ro[2]] }, xfRow);
    for (let r = 0; r < 3; r++) gl.uniform4f(uM[r]!, xfRow[4 * r]!, xfRow[4 * r + 1]!, xfRow[4 * r + 2]!, xfRow[4 * r + 3]!);
  };
  // ---- end LNK3D ----

  // ---- LNK3D (D, ADR 0078 amendment 3): each model (the open one, each link) has its own buffers and VAOs
  // (makeModelGl), so a link's detail can leave the GPU and come back; the names below (vao, posBuf, …) are
  // the model being drawn (useModel), as one model's were before. ----
  interface ModelGl { vao: WebGLVertexArrayObject; edgeVao: WebGLVertexArrayObject; instVao: WebGLVertexArrayObject; instEdgeVao: WebGLVertexArrayObject;
    pos: WebGLBuffer; el: WebGLBuffer; idx: WebGLBuffer; edge: WebGLBuffer; edgeNrm: WebGLBuffer; edgeEl: WebGLBuffer; edgeIdx: WebGLBuffer;
    shapeIdx: WebGLBuffer; instMat: WebGLBuffer; instEl: WebGLBuffer; bytes: number }
  const makeModelGl = (): ModelGl => {
    // One VAO: positions (location 0), the element of each vertex (location 8: its colour and state), indices.
    const vao = gl.createVertexArray(), pos = gl.createBuffer(), el = gl.createBuffer(), idx = gl.createBuffer();
    gl.bindVertexArray(vao);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, idx);
    gl.bindBuffer(gl.ARRAY_BUFFER, pos);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
    gl.bindBuffer(gl.ARRAY_BUFFER, el); // LNK3D: each vertex's element
    gl.enableVertexAttribArray(8);
    gl.vertexAttribIPointer(8, 1, gl.UNSIGNED_INT, 4, 0);
    // Edges: their own VAO (positions, the two face normals per vertex, the visible segments' indices).
    const edgeVao = gl.createVertexArray(), edge = gl.createBuffer(), edgeNrm = gl.createBuffer(), edgeEl = gl.createBuffer(), edgeIdx = gl.createBuffer();
    gl.bindVertexArray(edgeVao);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, edgeIdx);
    gl.bindBuffer(gl.ARRAY_BUFFER, edge);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
    gl.bindBuffer(gl.ARRAY_BUFFER, edgeNrm);
    gl.enableVertexAttribArray(2);
    gl.vertexAttribPointer(2, 4, gl.BYTE, true, 8, 0);
    gl.enableVertexAttribArray(3);
    gl.vertexAttribPointer(3, 4, gl.BYTE, true, 8, 4);
    gl.bindBuffer(gl.ARRAY_BUFFER, edgeEl); // LNK3D: each segment vertex's element
    gl.enableVertexAttribArray(8);
    gl.vertexAttribIPointer(8, 1, gl.UNSIGNED_INT, 4, 0);
    // ---- 3D1 (ADR 0078): copies drawn instanced. The shapes' indices packed (shapeIdx), per instance its
    // placement (instMat, 48 bytes) and element (instEl: its colour and state), grouped by (model, shape,
    // mirrored); a group is one drawElementsInstanced (faces) and one drawArraysInstanced (edges). ----
    const shapeIdx = gl.createBuffer(), instMat = gl.createBuffer(), instEl = gl.createBuffer();
    const instVao = gl.createVertexArray(), instEdgeVao = gl.createVertexArray();
    gl.bindVertexArray(instVao);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, shapeIdx);
    gl.bindBuffer(gl.ARRAY_BUFFER, pos);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
    for (const l of [4, 5, 6, 7, 8]) gl.enableVertexAttribArray(l), gl.vertexAttribDivisor(l, 1);
    gl.bindVertexArray(instEdgeVao);
    gl.bindBuffer(gl.ARRAY_BUFFER, edge);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
    gl.bindBuffer(gl.ARRAY_BUFFER, edgeNrm);
    gl.enableVertexAttribArray(2);
    gl.vertexAttribPointer(2, 4, gl.BYTE, true, 8, 0);
    gl.enableVertexAttribArray(3);
    gl.vertexAttribPointer(3, 4, gl.BYTE, true, 8, 4);
    for (const l of [4, 5, 6, 7, 8]) gl.enableVertexAttribArray(l), gl.vertexAttribDivisor(l, 1);
    gl.bindVertexArray(null);
    return { vao, edgeVao, instVao, instEdgeVao, pos, el, idx, edge, edgeNrm, edgeEl, edgeIdx, shapeIdx, instMat, instEl, bytes: 0 };
  };
  const freeModelGl = (g: ModelGl) => {
    for (const v of [g.vao, g.edgeVao, g.instVao, g.instEdgeVao]) gl.deleteVertexArray(v);
    for (const b of [g.pos, g.el, g.idx, g.edge, g.edgeNrm, g.edgeEl, g.edgeIdx, g.shapeIdx, g.instMat, g.instEl]) gl.deleteBuffer(b);
  };
  const g0 = makeModelGl();
  let vao = g0.vao, edgeVao = g0.edgeVao, instVao = g0.instVao, instEdgeVao = g0.instEdgeVao;
  let posBuf = g0.pos, elBuf = g0.el, idxBuf = g0.idx, edgeBuf = g0.edge, edgeNrm = g0.edgeNrm, edgeElBuf = g0.edgeEl, edgeIdx = g0.edgeIdx;
  let shapeIdxBuf = g0.shapeIdx, instMat = g0.instMat, instElBuf = g0.instEl;
  /** Draws (and uploads) model g from now on. */
  const useModel = (g: ModelGl) => {
    ({ vao, edgeVao, instVao, instEdgeVao, pos: posBuf, el: elBuf, idx: idxBuf, edge: edgeBuf, edgeNrm, edgeEl: edgeElBuf, edgeIdx, shapeIdx: shapeIdxBuf, instMat, instEl: instElBuf } = g);
  };
  let faceN = 0, faceMainN = 0, edgeN = 0, edgeMainN = 0;
  // ---- end LNK3D (D) ----
  // 3D1: the outlines of unfinished batches: the 12 edges of the unit cube, placed per box (instanced)
  const cubeBuf = gl.createBuffer(), outlineMat = gl.createBuffer(), outlineVao = gl.createVertexArray();
  {
    const v: number[] = [];
    for (let i = 0; i < 8; i++) for (let k = 0; k < 3; k++) if (!((i >> k) & 1)) v.push(i & 1, (i >> 1) & 1, (i >> 2) & 1, (i | (1 << k)) & 1, ((i | (1 << k)) >> 1) & 1, ((i | (1 << k)) >> 2) & 1);
    gl.bindVertexArray(outlineVao);
    gl.bindBuffer(gl.ARRAY_BUFFER, cubeBuf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(v), gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
    gl.bindBuffer(gl.ARRAY_BUFFER, outlineMat);
    for (let c = 0; c < 4; c++) gl.enableVertexAttribArray(4 + c), gl.vertexAttribDivisor(4 + c, 1), gl.vertexAttribPointer(4 + c, 3, gl.FLOAT, false, 48, 12 * c);
    gl.bindVertexArray(null);
  }
  // ---- 3D2 (ADR 0078): HLOD proxies (gl/proxies.ts): their faces (positions, a colour per vertex from
  // the part's category, indices) and crease edges (two vertices a segment, with the two face normals). ----
  const prxPos = gl.createBuffer(), prxCol = gl.createBuffer(), prxIdx = gl.createBuffer(), prxEdge = gl.createBuffer(), prxNrm = gl.createBuffer();
  const prxVao = gl.createVertexArray(), prxEdgeVao = gl.createVertexArray();
  gl.bindVertexArray(prxVao);
  gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, prxIdx);
  gl.bindBuffer(gl.ARRAY_BUFFER, prxPos);
  gl.enableVertexAttribArray(0);
  gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
  gl.bindBuffer(gl.ARRAY_BUFFER, prxCol);
  gl.enableVertexAttribArray(1);
  gl.vertexAttribPointer(1, 3, gl.UNSIGNED_BYTE, true, 0, 0);
  gl.bindVertexArray(prxEdgeVao);
  gl.bindBuffer(gl.ARRAY_BUFFER, prxEdge);
  gl.enableVertexAttribArray(0);
  gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
  gl.bindBuffer(gl.ARRAY_BUFFER, prxNrm);
  gl.enableVertexAttribArray(2);
  gl.vertexAttribPointer(2, 4, gl.BYTE, true, 8, 0);
  gl.enableVertexAttribArray(3);
  gl.vertexAttribPointer(3, 4, gl.BYTE, true, 8, 4);
  gl.bindVertexArray(null);
  /** The proxies of the mesh shown (null: none served yet, or none for this model). */
  let prx: Proxies | null = null;
  let prxBytes = 0, prxUsable: Uint8Array = new Uint8Array(0), prxUsableKey = "", listsGen = 0, visGen = 0; // LNK3D: visGen, what is shown
  // 3D2 (step 3, ADR 0036 #1): occlusion queries per batch, last frames' answers (off by default, as
  // Rust on WebGL2; frames.mjs turns it on through stats.occlusion to measure). Probe boxes: 36 vertices each.
  const probeBuf = gl.createBuffer(), probeVao = gl.createVertexArray();
  gl.bindVertexArray(probeVao);
  gl.bindBuffer(gl.ARRAY_BUFFER, probeBuf);
  gl.enableVertexAttribArray(0);
  gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
  gl.bindVertexArray(null);
  let occPending = false, occKey = "";
  /** The answers in: true when a hidden batch's box showed again (from another view than the one that
   *  found it hidden: CHC++'s rule as Rust's, so a still view does not redraw forever). */
  const pollOcc = (): boolean => {
    let back = false, pending = false;
    for (const d of dbs) {
      const o = d.o;
      if (!o?.q) continue;
      if (!gl.getQueryParameter(o.q, gl.QUERY_RESULT_AVAILABLE)) { pending = true; continue; }
      const seen = !!gl.getQueryParameter(o.q, gl.QUERY_RESULT);
      gl.deleteQuery(o.q);
      o.q = null;
      if (o.box) { if (seen && o.qAt !== o.hidAt) o.hidden = false, back = true; }
      else if (!seen) o.hidden = true, o.hidAt = o.qAt;
    }
    occPending = pending;
    return back;
  };
  const occReset = () => {
    for (const d of dbs) if (d.o) { if (d.o.q) gl.deleteQuery(d.o.q); delete d.o; }
    occPending = false;
  };
  /** The mesh the proxies are being asked for (a newer mesh abandons the steps). */
  let prxFor: object | null = null;
  // ---- LNK3D (C, ADR 0078 amendment 3): each link's proxies, baked in its cache at conversion and read from
  // disk (GET /mesh?link_proxies=k), asked before the mesh: the first picture of a project of links is
  // drawn from them, then the detail refines it. Each frame a link's tree is cut with the eye in the
  // link's own frame (its placement is rigid: distances hold) and its cells' boxes placed for culling;
  // a leaf drawn as a proxy leaves its batches out (the link's batches are cut by its leaves). ----
  interface LinkPrx { name: string; p: Proxies; vao: WebGLVertexArrayObject; bufs: WebGLBuffer[]; bytes: number;
    usable: Uint8Array; usableKey: string; drawnCells: number; drawnTris: number }
  /** By link name (a link renamed or reloaded with another element count asks again). */
  const linkPrx = new Map<string, LinkPrx>();
  /** Per model of the mesh (k + 1: link k), its proxies (null: none). */
  let modelPrx: (LinkPrx | null)[] = [];
  /** VIS2: per model, link proxies that came late while its detail was on the GPU (swapped in at its release). */
  const pendingPrx = new Map<number, { x: LinkPrx; name: string }>();
  // A link's proxies take its colour (the reference colour or its override), as its elements: no colour per vertex.
  const makeLinkPrx = (name: string, p: Proxies): LinkPrx => {
    const vao = gl.createVertexArray(), pb = gl.createBuffer(), ib = gl.createBuffer();
    gl.bindVertexArray(vao);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, ib);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, p.idx, gl.STATIC_DRAW);
    gl.bindBuffer(gl.ARRAY_BUFFER, pb);
    gl.bufferData(gl.ARRAY_BUFFER, p.pos, gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
    gl.bindVertexArray(null);
    return { name, p, vao, bufs: [pb, ib], bytes: p.pos.byteLength + p.idx.byteLength, usable: new Uint8Array(0), usableKey: "", drawnCells: 0, drawnTris: 0 };
  };
  const dropLinkPrx = (x: LinkPrx) => { gl.deleteVertexArray(x.vao); for (const b of x.bufs) gl.deleteBuffer(b); };
  /** Asks every link's proxies not held yet (rows: query link_draw, in link order). */
  /** SIZE3 (E): per link the proxies' generation its proxies were asked at (query link_draw "proxies"). */
  const prxGen = new Map<string, number>();
  const fetchLinkProxies = async (rows: LinkDraw[]): Promise<number> => {
    const ask = source.linkProxies;
    if (!ask || !proxiesOn()) return 0;
    let got = 0;
    await Promise.all(rows.map(async (r, k) => {
      if (linkPrx.has(r.name)) return;
      prxGen.set(r.name, r.proxies ?? 0);
      try {
        const a = await ask(k);
        if (a.status !== 200) return;
        const p = parseBprx(a.bytes);
        if (p.cells.length) linkPrx.set(r.name, makeLinkPrx(r.name, p)), got++;
      } catch (err) { console.warn("gl viewport: link proxies:", err); }
    }));
    return got;
  };
  /** The links' proxies before their mesh: the first picture (each cell cut at the still threshold, placed by rows). */
  let prePrx: { x: LinkPrx; xf: ModelXf; visible: boolean; colour: number }[] = [];
  const prePrxBounds = (): { center: V3; radius: number } | null => {
    const lo: V3 = [Infinity, Infinity, Infinity], hi: V3 = [-Infinity, -Infinity, -Infinity], b = new Float32Array(6);
    const per = prePrx.map(({ x, xf, visible }) => { // REAL1: a link far from the others left out (gl/farlinks.ts)
      const root = x.p.cells[0];
      if (!visible || !root) return null;
      xfBox(xf, root.box, 0, b, 0);
      return { min: [b[0]!, b[1]!, b[2]!], max: [b[3]!, b[4]!, b[5]!] };
    });
    const far = farModels(per);
    per.forEach((p, k) => { if (p && !far.has(k)) for (let a = 0; a < 3; a++) lo[a] = Math.min(lo[a]!, p.min[a]!), hi[a] = Math.max(hi[a]!, p.max[a]!); });
    return lo[0] <= hi[0] ? { center: scale(add(lo, hi), 0.5), radius: Math.max(1, Math.hypot(...sub(hi, lo)) / 2) } : null;
  };
  const tmpBox = new Float32Array(6);
  /** A link's cut this frame: the cells drawn as proxies and the leaves they stand for. */
  const cutLinkPrx = (x: LinkPrx, xf: ModelXf, usable: Uint8Array, planes: Float64Array | null, eyeW: V3, px: number) => {
    const eyeL = xfRayToLocal(xf, eyeW, [0, 0, 0]).e;
    const k = (2 * Math.tan(FOV_Y / 2)) / Math.max(1, canvas.height);
    const mmPerPx = ortho3d() ? () => distance * k : (d: number) => d * k;
    return cutProxies(x.p, usable, planes ? (b) => { xfBox(xf, b, 0, tmpBox, 0); return boxInFrustum(planes, tmpBox, 0); } : null, eyeL, mmPerPx, px);
  };
  /** Draws a link's proxy cells (faces only: F, never their edges), placed by model `mdl`'s row. */
  const drawLinkCells = (x: LinkPrx, cells: number[], mdl: number, colour: number): number => {
    let tris = 0;
    gl.bindVertexArray(x.vao);
    setModel(mdl);
    const c = LINK_SRGB[colour] ?? REFERENCE_SRGB;
    gl.vertexAttrib3f(1, c[0] / 255, c[1] / 255, c[2] / 255);
    const runs: [number, number][] = [];
    for (const c of [...cells].sort((a, b) => a - b)) for (const q of x.p.cells[c]!.parts) {
      const last = runs[runs.length - 1];
      if (!q.ni) continue;
      if (last && last[0] + last[1] === q.i0) last[1] += q.ni;
      else runs.push([q.i0, q.ni]);
    }
    for (const [a, n] of runs) gl.drawElements(gl.TRIANGLES, n, gl.UNSIGNED_INT, 4 * a), tris += n / 3;
    setModel(0);
    gl.bindVertexArray(null);
    x.drawnCells = cells.length, x.drawnTris = tris;
    return tris;
  };
  /** LNK3D (C): the first picture of a project of links, before their mesh: every visible link's proxies. */
  const drawProxyPreview = () => {
    if (!prePrx.length) return;
    const all = prePrx.filter((q) => q.visible);
    modelXf = [IDENTITY_XF, ...prePrx.map((q) => q.xf)];
    uploadModels();
    const { e, mvp } = matrices();
    gl.useProgram(prog);
    setCamera(mvp, ortho3d() ? norm(sub(e, target)) : e, ortho3d());
    gl.uniform3fv(uLight, norm(add(norm(sub(e, target)), [-0.3, -0.2, 0.9])));
    gl.uniform1f(uEyeDir, ortho3d() ? 1 : 0);
    gl.uniform1f(uPoint, 1), gl.uniform1f(uAlpha, 1), gl.uniform1i(uClip, 0), gl.uniform1f(uZbias, 0), gl.uniform1f(uAmbient, AMBIENT);
    gl.enable(gl.DEPTH_TEST);
    gl.depthFunc(gl.LEQUAL);
    const planes = frustumPlanes(mvp);
    let tris = 0;
    prePrx.forEach((q) => {
      if (!q.visible) return;
      const usable = Uint8Array.from(q.x.p.cells, (c) => (c.parts.length ? 1 : 0));
      const { cells } = cutLinkPrx(q.x, q.xf, usable, planes, e, STILL_PROXY_PX);
      tris += drawLinkCells(q.x, cells, 1 + prePrx.indexOf(q), q.colour);
    });
    stats.proxyPreview = { at: performance.now(), links: all.length, tris };
    if (tris > 0) perfPreview(); // VIS1 (P0-b): the first picture of stand-ins counts in the panel
  };
  // ---- end LNK3D (C) ----
  // ---- end 3D2 ----
  /** A run of instances of one shape: its packed index range, its segments, its instances [first, first + count). */
  interface InstGroup { i0: number; ni: number; s0: number; ns: number; first: number; count: number; mirrored: boolean; link: boolean; model: number; dbs: DrawBatch[];
    /** VIS2: its shape (k + 1): i0 is read again from shapeI0 when its model's buffers are loaded. */
    shape: number }
  /** 3D1 (steps 2, 3): a batch of the merged lists (g null: faces from index f0, edges from e0) or of a
   *  group's instances (from g.first + b.first), and how many of its items this frame draws (k). */
  interface DrawBatch { b: Batch; f0: number; e0: number; link: boolean; g: InstGroup | null; k0: number; k1: number; o?: OccState;
    /** VIS1 (gl/visible.ts): its items that may be seen (made per pyramid) and this frame's runs of them (null: all of [k0, k1)). */
    vmask?: Uint8Array | null; vkey?: string; runs?: number[] | null; dist?: number; runsMemo?: number[]; runsSerial?: number; runsK0?: number; runsK1?: number; hid?: number; hidTris?: number;
    /** VIS2: the triangles of its first i items not left out by the mask (the budget's cut counts these), and their key. */
    kcum?: Uint32Array; kcumKey?: string;
    /** VIS2: the fill (its number) a stalled slice finished this batch in. */
    fillDone?: number;
    /** LNK3D: its model (0 the open one, k + 1 link k) and its elements in batch order (their state: batchShown). */
    model: number; items: Uint32Array;
    /** LNK3D (F): no edges this frame (its box spans under EDGE_MIN_PX on screen, or a moving frame left part of it out). */
    noEdge?: boolean }
  /** 3D2 (step 3): a batch's occlusion query: pending (`q`, asked by its faces or, when hidden, by its
   *  box; `qAt` the camera then), and whether its faces were found hidden (`hidAt`: at which camera). */
  interface OccState { q: WebGLQuery | null; box: boolean; qAt: string; hidden: boolean; hidAt: string }
  let dbs: DrawBatch[] = [];
  /** VIS1 (P0-c): the batches of each model (made once per list of batches). */
  let byModelOf: { dbs: DrawBatch[]; by: Map<number, DrawBatch[]> } | null = null;
  const dbsOf = (mdl: number): DrawBatch[] => {
    if (!byModelOf || byModelOf.dbs !== dbs) {
      const by = new Map<number, DrawBatch[]>();
      for (const d of dbs) { let l = by.get(d.model); if (!l) by.set(d.model, (l = [])); l.push(d); }
      byModelOf = { dbs, by };
    }
    return byModelOf.by.get(mdl) ?? [];
  };
  let groups: InstGroup[] = [];
  /** Per shape (k + 1 - 1): its first index in the packed shape indices. */
  let shapeI0 = new Uint32Array(0);
  /** The elements of the instances, in instance order (for their colours). */
  let instEls = new Uint32Array(0);
  /** Points the instance attributes at instance `first` (WebGL2 has no base instance). */
  const instAttrs = (first: number, _colour: boolean) => {
    gl.bindBuffer(gl.ARRAY_BUFFER, instMat);
    for (let c = 0; c < 4; c++) gl.vertexAttribPointer(4 + c, 3, gl.FLOAT, false, 48, 48 * first + 12 * c);
    gl.bindBuffer(gl.ARRAY_BUFFER, instElBuf); // LNK3D: colour, model and shown from the element's state
    gl.vertexAttribIPointer(8, 1, gl.UNSIGNED_INT, 4, 4 * first);
  };
  // ---- end 3D1 ----
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
  /** SIZE2: per link its mesh key (LNKS version 3): a refetched mesh of another key is stale. */
  let linkKeys: number[] = [];
  let bounds: { center: V3; radius: number } | null = null;
  /** REAL1 (gl/farlinks.ts): the scene's bounds from the elements' boxes, each model's (the open one, each link)
   *  first; a model thousands of km from the others (a file without georeference) is left out (Fit all, the clip
   *  planes), the Links panel told (linkFar) and stats.farLinks set. Null: no element with a mesh. */
  const sceneBounds = (m: Bmsh): { center: V3; radius: number } | null => {
    const L = m.links ?? [], ranges: [number, number][] = [[0, L[0]?.e0 ?? m.ids.length], ...L.map((l): [number, number] => [l.e0, l.e0 + l.elements])];
    const per = ranges.map(([a, b]) => {
      const lo = [Infinity, Infinity, Infinity], hi = [-Infinity, -Infinity, -Infinity];
      for (let e = a; e < b; e++) {
        if (!(m.nv[e] ?? 0)) continue;
        for (let k = 0; k < 3; k++) lo[k] = Math.min(lo[k]!, boxes[6 * e + k] ?? 0), hi[k] = Math.max(hi[k]!, boxes[6 * e + 3 + k] ?? 0);
      }
      return lo[0]! <= hi[0]! ? { min: lo, max: hi } : null;
    });
    const far = farModels(per);
    const min: V3 = [Infinity, Infinity, Infinity], max: V3 = [-Infinity, -Infinity, -Infinity];
    per.forEach((b, k) => { if (b && !far.has(k)) for (let a = 0; a < 3; a++) min[a] = Math.min(min[a]!, b.min[a]!), max[a] = Math.max(max[a]!, b.max[a]!); });
    const named: Record<string, number> = {};
    for (const [k, d] of far) if (k > 0) named[L[k - 1]!.name] = Math.round(d / 1e6);
    if (JSON.stringify(named) !== JSON.stringify(linkFar.value)) linkFar.value = named;
    stats.farLinks = Object.entries(named).map(([name, km]) => ({ name, km }));
    return min[0] <= max[0] ? { center: scale(add(min, max), 0.5), radius: Math.max(1, Math.hypot(...sub(max, min)) / 2) } : null;
  };
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
  let seenSwitches = ""; // VIEWSHEET: the pressed switches the viewport draws with
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
  // MEAS1 (ADR 0074): only the 2D overlay changed (the measure tool's marker, rubber line, tags):
  // the overlay is drawn again, not the model (the 3D scene: when the camera or the model changes).
  let overlayDirty = false;
  // ---- 3D1 (step 3, ADR 0078 amended): while the camera moves, a frame draws the elements largest on
  // screen first until its triangle budget is spent (the setting moving_fps times the triangles a ms
  // the last frames drew, at least MIN_MOVING_TRIS), and the batches it could not finish as outlines of
  // their boxes; STILL_MS after the camera stopped, the rest is added in slices of the same budget, one
  // a frame, without clearing (a move abandons it), then every edge once more. ----
  const STILL_MS = 150;
  // 3D2 (ADR 0078): a far cell's proxy stands for its elements when its error spans at most this many
  // pixels: Rust's MOVING_PROXY_PX (3) while moving, 1 when still (under a pixel: the picture keeps its
  // look, the count of pixels that differ is in frames.md); proxies are asked for a model of at least
  // PROXY_MIN_TRIS triangles (Rust: 1,000,000, on slow GPUs only), PROXY_STEP_TRIS read by the first call,
  // then as many as take about PROXY_STEP_MS.
  const EDGE_MIN_PX = 16; // LNK3D (F): a batch's edges are drawn when its box spans at least this (device px)
  const MOVING_PROXY_PX = 3, STILL_PROXY_PX = 1, PROXY_MIN_TRIS = 250000, PROXY_STEP_TRIS = 20000, PROXY_STEP_MS = 25;
  let lastCamKey: string | null = null, stillDue = 0, lastFrameAt = 0, lastFrameTris = 0, trisPerMs = 0, lastBudgeted = false;
  // ---- VIS1 (P0-a, ADR 0078 amendment 7): "moving" follows input, not only the camera key between two frames
  // drawn: a frame within MOTION_MS (or twice the last frame's time) of a camera change is a moving frame, and
  // the fill waits as long after the last input (a held pointer, a wheel step, a key). ADR 0074's 200 ms is a
  // floor for every setting: with moving_fps 0 ("Draw everything") the whole picture is drawn while moving as
  // long as a whole frame takes at most CAP_MS; above, a budget sized to CAP_TARGET_MS applies (capped) and the
  // whole picture comes in slices once the input stops. A frame's time: a fence polled each loop (no stall). ----
  const MOTION_MS = 250, CAP_MS = 100, CAP_TARGET_MS = 60;
  /** VIS1: the frame being drawn is the ground-truth pass (element ids into idFbo, faces only, every element). */
  /** VIS1: item i of batch d left out of the last frame by the visibility test. */
  const visSkip = (d: { runs?: number[] | null }, i: number): boolean => {
    const r = d.runs;
    if (!r) return false;
    for (let k = 0; k < r.length; k += 2) if (i >= r[k]! && i < r[k + 1]!) return false;
    return true;
  };
  /** VIS1: the camera, size and visibility of the last complete picture presented (its element attachment answers picks), or null. */
  let idPickKey: string | null = null, lastPresented = false, lastPresentAt = 0;
  const PRESENT_MS = 250;
  let idMode = false, idFbo: { fb: WebGLFramebuffer; col: WebGLRenderbuffer; dep: WebGLRenderbuffer; w: number; h: number } | null = null;
  let lastCamChangeAt = -Infinity, lastInputAt = -Infinity, lastFrameMs = 0, wholeRate = 0, lastAllTris = 0, capped = false;
  let frameFence: { s: WebGLSync; t0: number; tris: number; whole: boolean; rec: FrameRec } | null = null;
  const ftime = frameTimer(gl); // VIS1: the frame log (gl/frametime.ts)
  // VIS1: the offscreen target (gl/target.ts): faces, elements, normals and depth; presented with the edges found on screen
  const tgt = (() => { try { return sceneTarget(gl); } catch (e) { console.warn("scene target:", e); return null; } })();
  const BG01 = [BACKGROUND_SRGB[0]! / 255, BACKGROUND_SRGB[1]! / 255, BACKGROUND_SRGB[2]! / 255];
  const E0 = [EDGE_SRGB[0]! / 255, EDGE_SRGB[1]! / 255, EDGE_SRGB[2]! / 255], E1 = [REFERENCE_EDGE_SRGB[0] / 255, REFERENCE_EDGE_SRGB[1] / 255, REFERENCE_EDGE_SRGB[2] / 255];
  const multiDraw = gl.getExtension("WEBGL_multi_draw"); // VIS1: the ranges that may be seen in one call
  // VIS1: the depth pyramid of the last picture (gl/hiz.ts) and its key: what it is valid for
  const hiz = (() => { try { return tgt ? hizBuilder(gl) : null; } catch (e) { console.warn("hiz:", e); return null; } })();
  // (the setting, on by default since VIS2 made it exact on a real project of links; the bench hook `visible` overrides it either way)
  const visibleOn = () => stats.visible !== undefined ? stats.visible : (userSettings.value?.values.draw_visible ?? true);
  // (detailGen: a link's detail released, element boxes placed: the pyramid and the masks made before are stale; VIS2: a
  // detail loaded is not: it adds to a picture whose pyramid counted that link's proxies as the farthest depth)
  const visKeyOf = (key: string, box: unknown) => `${key}|${canvas.width}x${canvas.height}|${visGen}|${listsGen}|${JSON.stringify(box)}|${state.value.links ?? 0}|${detailGen}`;
  /** Edges found on screen: in 3D (not in a plan, where the drawing's lines stay), with the target, unless a bench hook asks the line pass. */
  const screenEdgesNow = () => !!tgt && !plan && stats.lineEdges !== true && opt().edge_mode === "screen" && pressed("edges", true) && !stats.noEdges;
  /** VIS1: the 3D view's optimisations as stored (the owner: each a setting with a good default and an exact value). */
  const opt = () => { const v = userSettings.value?.values; return { edge_mode: v?.edge_mode ?? "screen", cull_back: v?.cull_back ?? false, still_px: v?.still_px ?? 0, refill_offscreen: v?.refill_offscreen ?? true }; };
  stats.frameRecs = ftime.log, stats.gpuTimer = ftime.timer;
  const motionWin = () => Math.max(MOTION_MS, 2 * lastFrameMs);
  /** VIS1: a hide, an isolate, a link shown or hidden: when a whole frame is estimated over FEEDBACK_MS, the next frame
   *  is drawn as a moving one (the largest first, within the budget: the input's feedback) and the picture is completed
   *  after it (the fill, offscreen); a small model's whole frame stays the feedback. */
  const FEEDBACK_MS = 60;
  const feedbackFrame = () => {
    const rate = trisPerMs || wholeRate;
    if (rate > 0 && lastAllTris / rate > FEEDBACK_MS && stats.wholeFeedback !== true) lastCamChangeAt = performance.now(), feedbackPending = true;
  };
  let feedbackPending = false, wholeNext = false, wholeAt = 0; // (after such a frame the next one is the whole picture)
  const inputNow = () => { lastInputAt = performance.now(); };
  canvas.addEventListener("pointermove", (ev) => { if (ev.buttons) inputNow(); }, { capture: true, passive: true });
  canvas.addEventListener("wheel", inputNow, { capture: true, passive: true });
  window.addEventListener("keydown", inputNow, true);
  /** The last frame's fence: its time (draw start to the GPU's end, to a loop's resolution) and, for a whole
   *  frame, the triangles a ms a whole picture is drawn at. */
  const pollFence = () => {
    ftime.poll();
    const f = frameFence;
    if (!f || gl.getSyncParameter(f.s, gl.SYNC_STATUS) !== gl.SIGNALED) return;
    const ms = performance.now() - f.t0;
    lastFrameMs = ms, f.rec.fence = ms;
    if (f.whole && f.tris > 0) wholeRate = wholeRate ? 0.5 * wholeRate + 0.5 * (f.tris / ms) : f.tris / ms;
    gl.deleteSync(f.s);
    frameFence = null;
    stats.frameMs = ms;
  };
  // ---- end VIS1 (P0-a) ----
  /** The progressive fill after a stop: the screen threshold drawn down to (null: none running). */
  let fill: { t: number; id?: number } | null = null, fillEdges = false;
  /** VIS2: each fill's number: a batch finished whole by a stalled slice (DrawBatch.fillDone) is skipped by the rest of it. */
  let fillSerial = 0;
  /** 3D2 (step 5): the first frame of a new mesh is drawn within the moving budget, the rest in slices at
   *  once after it (something on screen within a frame of the data, the picture then refines). */
  let firstOfMesh = false;
  /** 3D2 (step 5): a large mesh's element boxes were drawn as outlines before its draw lists (their index
   *  upload took 294 of 318 ms on the local model under SwiftShader: the boxes are on screen meanwhile). */
  let previewed = false;
  const PREVIEW_BOXES = 4096, FIRST_BUDGET_TRIS = 1000000;
  const previewBoxes = () => {
    const m = mesh;
    if (!m) return;
    const n = m.ids.length, size = new Float32Array(n);
    for (let e = 0; e < n; e++) size[e] = m.nv[e] ? Math.hypot(boxes[6 * e + 3]! - boxes[6 * e]!, boxes[6 * e + 4]! - boxes[6 * e + 1]!, boxes[6 * e + 5]! - boxes[6 * e + 2]!) : -1;
    const order = [...Array(n).keys()].filter((e) => size[e]! >= 0).sort((a, b) => size[b]! - size[a]!).slice(0, PREVIEW_BOXES);
    if (tgt) tgt.bind(canvas.width, canvas.height), tgt.clear(BG01), tgt.colourOnly(true); // VIS1
    gl.viewport(0, 0, canvas.width, canvas.height);
    gl.clearColor(BACKGROUND_SRGB[0]! / 255, BACKGROUND_SRGB[1]! / 255, BACKGROUND_SRGB[2]! / 255, 1);
    gl.clearDepth(1);
    gl.depthMask(true);
    if (!tgt) gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    const { e, mvp } = matrices();
    gl.useProgram(prog);
    setCamera(mvp, plan || ortho3d() ? norm(sub(e, target)) : e, !!plan || ortho3d());
    gl.uniform1f(uEyeDir, plan || ortho3d() ? 1 : 0);
    gl.uniform1f(uPoint, 1), gl.uniform1f(uAlpha, 1), gl.uniform1i(uClip, 0), gl.uniform1f(uZbias, 0);
    gl.enable(gl.DEPTH_TEST);
    drawOutlines(order.map((el) => boxes.subarray(6 * el, 6 * el + 6)));
    if (tgt) tgt.present(false, E0, E1); // VIS1
    stats.previewAt = performance.now();
  };
  // (not the canvas size: a panel shown by a selection resizes it, and that is not a camera move)
  const camKey = () => `${yaw}|${pitch}|${distance}|${target.join(",")}|${half}|${viewId}`;
  /** This frame's triangle budget (Infinity: no budget). */
  const frameBudget = (): number => {
    const fps = userSettings.value?.values.moving_fps ?? 30;
    if (fps <= 0) return capped ? Math.max(MIN_MOVING_TRIS, (trisPerMs || wholeRate) * CAP_TARGET_MS) : Infinity; // VIS1 (P0-a)
    return Math.max(MIN_MOVING_TRIS, trisPerMs * (1000 / fps));
  };
  // ---- end 3D1 ----
  let loading = false;
  // What the mesh shows: the model's rev and the links' counter (change JSON `links`).
  // LNK3D: what the mesh holds (query mesh_key, asked once per rev): a change of views alone (a hide in a
  // view) or of the links' flags, colours and placements (links_mesh stays) asks no new mesh
  let keyRev = -1, drawKey = "";
  const meshKey = () => {
    if (state.value.rev !== keyRev) {
      keyRev = state.value.rev;
      const k = state.value.file === null ? null : coreMeshKey();
      drawKey = k ? `d${k.draw}/${k.links}` : `${state.value.rev}/${state.value.links_mesh ?? state.value.links ?? 0}`;
    }
    return state.value.links_mesh !== undefined ? `${drawKey.split("/")[0]}/${state.value.links_mesh}` : drawKey;
  };
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
  let fitted = false, fittedByPreview = false;

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
      const proj = new Float64Array([m / w, 0, 0, 0, 0, 1 / half, 0, 0, 0, 0, -2 / (far - near), 0, 0, 0, -(far + near) / (far - near), 1]);
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
      const proj = new Float64Array([1 / w, 0, 0, 0, 0, 1 / h, 0, 0, 0, 0, -2 / (f - n), 0, 0, 0, -(f + n) / (f - n), 1]);
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

  /** LNK3D: each element's colour and drawn flag into the state texture (gl/cull.ts packState), not per
   *  vertex: a selection, a hide or a link's colour writes a texel per element (texSubImage2D of the rows
   *  that changed), never a vertex buffer. Returns the elements whose colour changed (overpaint). */
  const colours = (): void => {
    const m = mesh;
    if (!m) return;
    const linked = state.value.linked ?? null;
    // A linked element selected: its id is the selection; the open model's same id is not selected.
    const sel = new Set(linked ? [] : state.value.selection);
    const colourOf = (e: number) => {
      const k = m.model?.[e] ?? 0;
      return k > 0
        ? (linked && linked.id === m.ids[e] && linked.link === m.links?.[k - 1]?.name ? SELECTION_SRGB : shown[e] === 2 ? REVEALED_SRGB
          : LINK_SRGB[m.links?.[k - 1]?.colour ?? -1] ?? REFERENCE_SRGB)
        : sel.has(m.ids[e] ?? "") ? SELECTION_SRGB : shown[e] === 2 ? REVEALED_SRGB
        : (CATEGORY_SRGB[m.cats[e] ?? 7] ?? CATEGORY_SRGB[7] ?? SELECTION_SRGB);
    };
    // 3D2 (step 4): each element's colour kept, the ones that changed listed (a selection is drawn over the
    // kept picture when only colours changed: overpaint below)
    const was = elColour.length === m.ids.length ? elColour : null;
    elColour = new Uint32Array(m.ids.length), recoloured = [];
    const n = m.ids.length;
    if (stateTexels.length < 4 * n) stateTexels = new Uint8Array(4 * n), stateRows = 0;
    let lo = n, hi = 0;
    for (let e = 0; e < n; e++) {
      const c = colourOf(e);
      elColour[e] = (c[0] << 16) | (c[1] << 8) | c[2];
      if (!was || was[e] !== elColour[e]) recoloured.push(e);
      const a = 4 * e, z = (((m.model?.[e] ?? 0) & 127) << 1) | (shown[e] ? 1 : 0);
      if (stateTexels[a] !== c[0] || stateTexels[a + 1] !== c[1] || stateTexels[a + 2] !== c[2] || stateTexels[a + 3] !== z) {
        packState(stateTexels, e, c, m.model?.[e] ?? 0, shown[e]! > 0);
        if (e < lo) lo = e;
        hi = e + 1;
      }
    }
    if (!was) recoloured = null;
    // LNK3D: a link all shown in its own colour draws without reading the state (u_plain)
    plainOf = [null, ...(m.links ?? []).map((l) => {
      const c = LINK_SRGB[l.colour ?? -1] ?? REFERENCE_SRGB, v = (c[0] << 16) | (c[1] << 8) | c[2];
      for (let e = l.e0; e < l.e0 + l.elements; e++) if (shown[e] !== 1 || elColour[e] !== v) return null;
      return c;
    })];
    const t0 = performance.now();
    if (hi > lo || stateRows === 0) uploadStates(lo, hi);
    stats.stateUpload = { elements: Math.max(0, hi - lo), ms: performance.now() - t0 };
    stats.plain = plainOf.map((x) => x !== null); // VIS1
  };

  // ---- 3D2 (step 4; Rust's #3, highlights over the kept picture): a change of colours alone (a selection)
  // redraws the elements whose colour changed over the last complete picture, not the whole model. ----
  let elColour = new Uint32Array(0), recoloured: number[] | null = null;
  /** Per element: its first index in the merged face list and edge list (-1: instanced or not drawn), its
   *  instance and group (-1: merged). */
  let elFace = new Int32Array(0), elEdge = new Int32Array(0), elInst = new Int32Array(0), elGroup = new Int32Array(0), elDb = new Int32Array(0); // (LNK3D: its batch)
  /** The last frame was a complete picture of the open model (whole, or the end of a fill) and which
   *  leaves its proxies stood for. */
  let lastComplete = false, lastReplaced: Uint8Array | null = null, lastLinkRep: (Uint8Array | null)[] = [];
  const OVERPAINT_MAX = 2000;
  const overpaint = (els: number[]): boolean => {
    const m = mesh;
    if (gpuR || !m || !lastComplete || plan || clipBox() || ghostN || markN || els.length > OVERPAINT_MAX || els.length === 0) return false; // GPU1: gpuR
    for (const el of els) {
      if (elFace[el] === undefined || (elFace[el]! < 0 && elInst[el]! < 0 && shown[el])) return false;
      const lf = prx && !((m.model?.[el] ?? 0) > 0) ? prx.leafOf[el] ?? -1 : -1;
      if (lf >= 0 && lastReplaced?.[lf]) return false; // under a proxy: the picture changes more
      const mdl = m.model?.[el] ?? 0, lx = mdl > 0 ? modelPrx[mdl] : null, ll = mdl > 0 ? m.links?.[mdl - 1] : undefined; // LNK3D (C): a link's
      if (lx && ll && lastLinkRep[mdl]?.[lx.p.leafOf[el - ll.e0] ?? -1]) return false;
      if (!slots[mdl]?.gl) return false; // LNK3D (D): its link's detail released
    }
    const tOver = performance.now(); // GPU1
    const { e, mvp } = matrices();
    const scr = screenEdgesNow(); // VIS1: the kept picture is the target's; its edges found again on presenting
    if (tgt) tgt.bind(canvas.width, canvas.height);
    gl.viewport(0, 0, canvas.width, canvas.height);
    gl.useProgram(prog);
    setCamera(mvp, ortho3d() ? norm(sub(e, target)) : e, ortho3d());
    gl.uniform3fv(uLight, norm(add(norm(sub(e, target)), [-0.3, -0.2, 0.9])));
    gl.uniform1f(uEyeDir, ortho3d() ? 1 : 0);
    gl.uniform1f(uPoint, 1), gl.uniform1f(uAlpha, 1), gl.uniform1i(uClip, 0), gl.uniform1f(uZbias, 0), gl.uniform1f(uCut, CUT_SHADE);
    gl.enable(gl.DEPTH_TEST);
    gl.depthFunc(gl.LEQUAL);
    gl.disable(gl.CULL_FACE);
    gl.enable(gl.POLYGON_OFFSET_FILL);
    gl.uniform1f(uAmbient, AMBIENT);
    const host = slots[0]?.gl ?? g0, gOf = (el: number) => slots[m.model?.[el] ?? 0]?.gl ?? host; // LNK3D (D): each element in its model's buffers
    useModel(host);
    gl.bindVertexArray(vao);
    gl.disableVertexAttribArray(1); // LNK3D: the colour is the element's state
    gl.uniform1i(uElCol, 1), setModel(0);
    const shownEls = els.filter((el) => shown[el]);
    for (const el of shownEls) {
      const link = (m.model?.[el] ?? 0) > 0;
      useModel(gOf(el)), setModel(m.model?.[el] ?? 0), gl.bindVertexArray(vao);
      gl.polygonOffset(link ? 2.5 : 1.5, link ? 16 : 2);
      if (elFace[el]! >= 0) { gl.drawElements(gl.TRIANGLES, m.ni[el] ?? 0, gl.UNSIGNED_INT, 4 * elFace[el]!); continue; }
      const g = groups[elGroup[el]!];
      if (!g) continue;
      gl.bindVertexArray(instVao);
      instAttrs(elInst[el]!, true);
      if (g.mirrored) gl.frontFace(gl.CW);
      gl.drawElementsInstanced(gl.TRIANGLES, g.ni, gl.UNSIGNED_INT, 4 * g.i0, 1);
      gl.frontFace(gl.CCW);
      gl.bindVertexArray(vao);
    }
    gl.disable(gl.POLYGON_OFFSET_FILL);
    if (tgt) tgt.colourOnly(true);
    if (pressed("edges", true) && !scr) {
      gl.uniform1f(uAmbient, -1);
      gl.uniform1i(uElCol, 0); // LNK3D: edges in their constant colour
      gl.depthMask(false);
      for (const el of shownEls) {
        const link = (m.model?.[el] ?? 0) > 0, c = link ? REFERENCE_EDGE_SRGB : EDGE_SRGB;
        if (dbs[elDb[el] ?? -1]?.noEdge) continue; // LNK3D (F): as the frame drew it, without edges
        useModel(gOf(el)), setModel(m.model?.[el] ?? 0);
        if (elEdge[el]! >= 0) {
          gl.bindVertexArray(edgeVao);
          gl.vertexAttrib3f(1, c[0]! / 255, c[1]! / 255, c[2]! / 255);
          gl.drawElements(gl.LINES, 2 * (m.ns[el] ?? 0), gl.UNSIGNED_INT, 4 * elEdge[el]!);
          continue;
        }
        const g = groups[elGroup[el]!];
        if (!g || !g.ns) continue;
        gl.bindVertexArray(instEdgeVao);
        gl.vertexAttrib3f(1, c[0]! / 255, c[1]! / 255, c[2]! / 255);
        instAttrs(elInst[el]!, false);
        gl.drawArraysInstanced(gl.LINES, 2 * g.s0, 2 * g.ns, 1);
      }
      gl.depthMask(true);
    }
    gl.uniform1i(uElCol, 1);
    useModel(host), setModel(0);
    gl.bindVertexArray(null);
    if (tgt) tgt.present(scr, E0, E1); // VIS1
    drawOverlay();
    stats.frames++;
    perfFrame(false, tOver, gl); // GPU1
    stats.overpaints = (stats.overpaints ?? 0) + 1;
    return true;
  };
  // ---- end 3D2 (step 4) ----
  /** Per element: its vertices' box (min xyz, max xyz), for the pick's ray-box test. */
  let boxes = new Float64Array(0);
  /** LNK3D: per element its box in its model's own frame; the GPU bytes of the element ids. */
  let lboxes = new Float32Array(0), elBytes = 0;
  /** LNK3D: link elements [a, b) placed by x: their scene boxes from their own. */
  const placeLinkBoxes = (a: number, b: number, x: ModelXf) => { detailGen++; for (let e = a; e < b; e++) xfBox(x, lboxes, 6 * e, boxes, 6 * e); };
  /** VIS1: bumped when a link's detail comes or goes, or element boxes are placed (part of the visible set's key). */
  let detailGen = 0;
  let meshBytes = 0, listBytes = 0; // 3D1: the model's GPU bytes (stats.gpuBytes)
  // ---- LNK3D (D): a model's slot: its ranges in the mesh, its GPU buffers (null: released), its draw lists
  // (kept in the page: a release frees the GPU's copy only), when it was last needed and its GPU bytes ----
  interface Slot { k: number; e0: number; e1: number; v0: number; v1: number; i0: number; i1: number; s0: number; s1: number; gl: ModelGl | null;
    lists: { fi: Uint32Array; ei: Uint32Array; mats: Float32Array; inst: Uint32Array } | null; bytes: number; needAt: number; loads: number;
    /** The last frame drew some of it in detail. */
    drawn?: boolean; listBytes?: number;
    /** SIZE2: its face list is on the GPU (its lists dropped from the page with its heavy arrays); its mesh is being asked again. */
    listsOnGpu?: boolean; fetching?: boolean;
    /** SIZE2FIX: the last frame (drawFrame) that needed it (in view, not stood for by a proxy), the frame its detail came back. */
    needFrame?: number; loadFrame?: number }
  let slots: Slot[] = [];
  /** SIZE2FIX: frames drawn with the batch pass (the budget's clock: a slow machine that draws few frames releases nothing it needs). */
  let drawFrame = 0;
  /** SIZE2: per link model its own elements in list order and its batches (made once per mesh), its instances. */
  let ownOrd: ({ order: number[]; batches: Batch[] } | undefined)[] = [], instOfNow: number[][] = [];
  /** SIZE2: triangles read back from the GPU for picks (a link's arrays dropped), by model and block: bounded. */
  const pickCache = new Map<string, { pos: Float32Array; idx: Uint32Array }>();
  let pickCacheBytes = 0;
  const PICK_CACHE_BYTES = 8 << 20;
  /** SIZE2: slot x's lists from its kept order and its heavy arrays (a released link's detail back). */
  const modelLists = (x: Slot): Slot["lists"] => {
    const m = mesh, P = partOf(x.k), saved = ownOrd[x.k];
    if (!m || !P || !saved) return null;
    let nf = 0, ne = 0;
    for (const e of saved.order) nf += m.ni[e] ?? 0, ne += m.ns[e] ?? 0;
    const fi = new Uint32Array(nf), ei = new Uint32Array(2 * ne);
    let f = 0, k = 0;
    for (const e of saved.order) {
      const a = (m.i0[e] ?? 0) - x.i0, n = m.ni[e] ?? 0;
      for (let i = 0; i < n; i++) fi[f + i] = P.idx[a + i]!;
      f += n;
      const s0 = (m.s0[e] ?? 0) - x.s0, ns = m.ns[e] ?? 0;
      for (let q = s0; q < s0 + ns; q++) ei[k++] = 2 * q, ei[k++] = 2 * q + 1;
    }
    const li = instOfNow[x.k] ?? [], mats = new Float32Array(12 * li.length), Pl = m.place ?? new Float32Array(0);
    li.forEach((e, i) => mats.set(Pl.subarray(12 * e, 12 * e + 12), 12 * i));
    return { fi, ei, mats, inst: Uint32Array.from(li) };
  };
  /** SIZE2: model k's heavy arrays (its own numbering), null when dropped (a link's, its buffers on the GPU). */
  const partOf = (k: number): MeshPart | null => {
    const m = mesh;
    if (!m) return null;
    return m.parts ? m.parts[k] ?? null : { pos: m.pos, idx: m.idx, edges: m.edges, normals: m.normals };
  };
  /** Uploads slot x's mesh (and its lists, when made) into new buffers (SIZE2: from its heavy arrays; false: dropped). */
  const loadSlot = (x: Slot): boolean => {
    // (VIS2: no detailGen++ here: a link's detail coming back only adds geometry, and the last picture drew that link as
    // proxies, which its pyramid counts as the farthest depth: that pyramid stays conservative. A release does make it
    // stale, releaseSlot.)
    const m = mesh, P = partOf(x.k);
    if (!m || !P) return false;
    if (!x.gl) x.gl = makeModelGl();
    const g = x.gl, nV = x.v1 - x.v0, nS = x.s1 - x.s0;
    const ev = new Uint32Array(nV), es = new Uint32Array(2 * nS);
    for (let e = x.e0; e < x.e1; e++) {
      ev.fill(e, m.v0[e]! - x.v0, m.v0[e]! - x.v0 + m.nv[e]!);
      if (!m.shape?.[e]) es.fill(e, 2 * (m.s0[e]! - x.s0), 2 * (m.s0[e]! - x.s0 + m.ns[e]!));
    }
    const pos = P.pos.subarray(0, 3 * nV), edges = P.edges.subarray(0, 6 * nS);
    gl.bindBuffer(gl.ARRAY_BUFFER, g.pos);
    gl.bufferData(gl.ARRAY_BUFFER, pos, gl.STATIC_DRAW);
    gl.bindBuffer(gl.ARRAY_BUFFER, g.el);
    gl.bufferData(gl.ARRAY_BUFFER, ev, gl.STATIC_DRAW);
    gl.bindBuffer(gl.ARRAY_BUFFER, g.edge);
    gl.bufferData(gl.ARRAY_BUFFER, edges, gl.STATIC_DRAW);
    gl.bindBuffer(gl.ARRAY_BUFFER, g.edgeEl);
    gl.bufferData(gl.ARRAY_BUFFER, es, gl.STATIC_DRAW);
    // The two face normals of each segment, for both of its vertices (3D2, step 5: 4-byte words, not a
    // subarray a segment; the blocks start on 4-byte boundaries, mesh.c).
    const nrm = new Int8Array(16 * nS);
    {
      const d = new Uint32Array(nrm.buffer), o = P.normals.byteOffset;
      const sw = o % 4 === 0 ? new Uint32Array(P.normals.buffer, o, 2 * nS) : new Uint32Array(P.normals.slice(0, 8 * nS).buffer, 0, 2 * nS);
      for (let k = 0; k < nS; k++) { const a = sw[2 * k]!, b = sw[2 * k + 1]!; d[4 * k] = a, d[4 * k + 1] = b, d[4 * k + 2] = a, d[4 * k + 3] = b; }
    }
    gl.bindBuffer(gl.ARRAY_BUFFER, g.edgeNrm);
    gl.bufferData(gl.ARRAY_BUFFER, nrm, gl.STATIC_DRAW);
    // its shapes' indices, packed, numbered from its first vertex
    const seen = new Set<number>();
    let si = 0;
    for (let e = x.e0; e < x.e1; e++) { const k = m.shape?.[e] ?? 0; if (k && !seen.has(k)) seen.add(k), shapeI0[k - 1] = si, si += m.ni[e] ?? 0; }
    const sidx = new Uint32Array(si);
    seen.clear();
    for (let e = x.e0; e < x.e1; e++) {
      const k = m.shape?.[e] ?? 0;
      if (!k || seen.has(k)) continue;
      seen.add(k);
      const a = (m.i0[e] ?? 0) - x.i0, n = m.ni[e] ?? 0, o = shapeI0[k - 1]!;
      for (let i = 0; i < n; i++) sidx[o + i] = P.idx[a + i]!; // (SIZE2: a part's indices are its own)
    }
    // VIS2: its groups' first index from these offsets: a link opened without its arrays (SIZE3: its rows only) had its
    // groups made before its shapes were packed, every group at offset 0: each copy drew its model's FIRST shape at its
    // placement (seen on a real project: copies drawn as other shapes, pixels where their boxes are not)
    for (const gr of groups) if (gr.model === x.k) gr.i0 = shapeI0[gr.shape - 1] ?? 0;
    gl.bindVertexArray(g.instVao);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, g.shapeIdx);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, sidx, gl.STATIC_DRAW);
    gl.bindVertexArray(null);
    x.bytes = pos.byteLength + ev.byteLength + edges.byteLength + es.byteLength + nrm.byteLength + sidx.byteLength;
    x.listsOnGpu = false;
    if (x.lists) uploadLists(x);
    x.loads++;
    x.loadFrame = drawFrame; // SIZE2FIX: not released again within NEED_FRAMES of coming back
    return true;
  };
  /** Uploads slot x's draw lists (faces, edges, instances) into its buffers. */
  const uploadLists = (x: Slot) => {
    const g = x.gl, l = x.lists;
    if (!g || !l) return;
    gl.bindVertexArray(g.vao);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, g.idx);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, l.fi, gl.STATIC_DRAW);
    gl.bindVertexArray(g.edgeVao);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, g.edgeIdx);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, l.ei, gl.STATIC_DRAW);
    gl.bindVertexArray(null);
    gl.bindBuffer(gl.ARRAY_BUFFER, g.instMat);
    gl.bufferData(gl.ARRAY_BUFFER, l.mats, gl.STATIC_DRAW);
    gl.bindBuffer(gl.ARRAY_BUFFER, g.instEl);
    gl.bufferData(gl.ARRAY_BUFFER, l.inst, gl.STATIC_DRAW);
    x.bytes += l.fi.byteLength + l.ei.byteLength + l.mats.byteLength + l.inst.byteLength;
    x.listBytes = l.fi.byteLength + l.ei.byteLength + l.mats.byteLength + l.inst.byteLength;
    x.listsOnGpu = true;
  };
  /** SIZE2: a link whose buffers and lists are on the GPU drops its heavy arrays and its lists from the page
   *  (what stays per element: its row, box, placement, state; picks read its triangles back from the GPU). */
  const dropPart = (x: Slot) => {
    const m = mesh;
    if (!m?.parts || x.k === 0 || !x.gl || !x.listsOnGpu || !m.parts[x.k]) return;
    m.parts[x.k] = null;
    x.lists = null;
  };
  /** SIZE2: a released link's detail wanted again: its mesh asked again (GET /mesh?link=k), uploaded, its
   *  lists made from its kept order, its heavy arrays dropped again. */
  const refetchSlot = async (x: Slot): Promise<void> => {
    const m = mesh, ask = source.linkMesh;
    if (!m?.parts || !ask || x.fetching || x.k === 0 || m.parts[x.k]) return;
    x.fetching = true;
    const t0 = performance.now();
    try {
      const name = m.links?.[x.k - 1]?.name ?? "";
      const r = await ask(x.k - 1, name, linkKeys[x.k - 1] ?? 0);
      if (!r.bytes) return;
      const td = performance.now();
      const lm = parseBmsh(r.bytes, 0, true);
      stats.meshBytesServed = (stats.meshBytesServed ?? 0) + r.served;
      if (mesh !== m || lm.ids.length !== x.e1 - x.e0 || lm.rev !== linkKeys[x.k - 1]) return; // another mesh since (it is asked again whole)
      m.parts[x.k] = { pos: lm.pos, idx: lm.idx, edges: lm.edges, normals: lm.normals };
      if (!x.gl || !x.listsOnGpu) { loadSlot(x); x.lists = modelLists(x); if (x.lists) uploadLists(x); }
      dropPart(x);
      stats.linkRefetches = (stats.linkRefetches ?? 0) + 1, stats.linkRefetchMs = performance.now() - t0;
      stats.refetchSteps = { fetch: td - t0, decode: 0, upload: performance.now() - td };
      dirty = true;
    } catch (e) {
      if (e instanceof Superseded) stats.linkSuperseded = (stats.linkSuperseded ?? 0) + 1; // SIZE2FIX: the mesh follows (links_mesh)
      else console.warn("gl viewport: a link's mesh:", e);
    } finally { x.fetching = false; }
  };
  /** What the GPU holds: each model's buffers (lists included), the state texture, the proxies (3D2's, the links'). */
  const gpuNow = () => slots.reduce((n, x) => n + x.bytes, 0) + 4 * STATE_TEX_W * stateRows + prxBytes + [...linkPrx.values()].reduce((n, x) => n + x.bytes, 0);
  /** Frees slot x's GPU buffers (its proxies, when it has some, stand for it). */
  const releaseSlot = (x: Slot) => {
    detailGen++; // VIS1
    const pend = pendingPrx.get(x.k); // VIS2: proxies that came while its detail was on the GPU: swapped in now
    if (pend) {
      pendingPrx.delete(x.k);
      const old = linkPrx.get(pend.name);
      if (old) dropLinkPrx(old);
      linkPrx.set(pend.name, pend.x);
      modelPrx[x.k] = pend.x, ownOrd[x.k] = undefined;
      visKey = "\u0000", dirty = true;
    }
    if (!x.gl) return;
    freeModelGl(x.gl);
    x.gl = null, x.bytes = 0, x.listsOnGpu = false;
    pickCache.clear(), pickCacheBytes = 0; // SIZE2: what was read back from its buffers
  };
  // ---- end LNK3D (D) ----
  const upload = (m: Bmsh) => {
    mesh = m;
    // VIS2: each element's box in its model's own frame (lboxes: f32, small numbers), then in the scene's in f64 (boxes):
    // a link placed kilometres from the scene's origin (georeferenced links in an empty project: 2,536 km) has f32
    // coordinates 256 mm apart there, and its boxes, rounded to them, were no longer around its pixels (VIS2's fault)
    const own = new Float32Array(6 * m.ids.length);
    boxes = new Float64Array(6 * m.ids.length);
    const vbase = (m.ranges ?? []).flatMap((r) => new Array<number>(r.e1 - r.e0).fill(r.v0)); // SIZE2: each model's first vertex
    for (let e = 0; e < m.ids.length; e++) {
      const part = m.parts ? m.parts[m.model?.[e] ?? 0] : undefined;
      if (part === null && m.preBox) { own.set(m.preBox.subarray(6 * e, 6 * e + 6), 6 * e); continue; } // SIZE3 (B): read before its arrays went
      const a = (m.v0[e] ?? 0) - (vbase[e] ?? 0), n = m.nv[e] ?? 0, P = part?.pos ?? m.pos; // SIZE2: its model's own vertices
      const pl = m.shape?.[e] ? m.place!.subarray(12 * e, 12 * e + 12) : null; // 3D1: a placed shape: its placed vertices
      elementBox(P, a, n, pl, own, e);
    }
    // LNK3D: a link's boxes in its own frame (lboxes), placed by its matrix into the scene's (boxes: culling,
    // picking, the budget, the fit); a move places them again, nothing else
    lboxes = own;
    boxes.set(own);
    modelXf = [IDENTITY_XF, ...(m.links ?? []).map((l) => l.xf)];
    for (const l of m.links ?? []) placeLinkBoxes(l.e0, l.e0 + l.elements, l.xf);
    uploadModels();
    stateTexels = new Uint8Array(4 * m.ids.length), stateRows = 0;
    bounds = m.nv.some((n) => n > 0) ? sceneBounds(m) : null; // 3D1: the elements' boxes (placed shapes placed); REAL1: far models left out
    ro = roFor(bounds); // VIS2
    // 3D1: each placed shape's indices once, packed per model (the merged lists hold the own meshes'); shapeI0:
    // a shape's first index in its model's packed list
    let nsh = 0;
    for (let e = 0; e < m.ids.length; e++) nsh = Math.max(nsh, m.shape?.[e] ?? 0);
    shapeI0 = new Uint32Array(nsh);
    // LNK3D (D): a slot per model, its own buffers uploaded (all of them now; the budget may release some)
    for (const x of slots) if (x.gl) freeModelGl(x.gl);
    slots = (m.ranges ?? [{ e0: 0, e1: m.ids.length, v0: 0, v1: m.pos.length / 3, i0: 0, i1: m.idx.length, s0: 0, s1: m.edges.length / 6 }])
      .map((r, k) => ({ k, ...r, gl: null, lists: null, bytes: 0, needAt: performance.now(), loads: 0 }));
    ownOrd = [], instOfNow = [], pickCache.clear(), pickCacheBytes = 0; // SIZE2
    for (const p of pendingPrx.values()) dropLinkPrx(p.x); // VIS2
    pendingPrx.clear();
    wantedLast.clear(); // SIZE3 (B)
    for (const x of slots) loadSlot(x);
    useModel(slots[0]?.gl ?? g0); // (the buffers of the mesh before are gone)
    meshBytes = slots.reduce((n, x) => n + x.bytes, 0) + 4 * m.ids.length; // (LNK3D: + the state texture)
    stats.gpuBytes = gpuNow();
    // 3D2 (step 5): a mesh of at least FIRST_BUDGET_TRIS triangles: its boxes first, then a budgeted first frame and
    // slices (a smaller one is drawn whole at once, as before: its whole frame is short and its lists fast)
    let tris = 0;
    for (let e = 0; e < m.ids.length; e++) tris += (m.ni[e] ?? 0) / 3;
    firstOfMesh = tris >= FIRST_BUDGET_TRIS, stats.fullFrameMs = null, previewed = false;
    visKey = "\u0000"; // the draw lists on the next frame
    selKey = "\u0000"; // recolour on the next frame
    if (tool) showTool(tool); // the ghost against the new mesh's origin
    if (fittedByPreview) fittedByPreview = false, fitted = false; // LNK3D (C): fitted again on the mesh's own bounds
    if (!fitted && bounds) {
      if (plan) fitPlan();
      else fit();
      fitted = true;
    }
    dirty = true;
  };

  // ---- 3D2 (ADR 0078): the proxies, asked in steps after the first frame (one step a frame: the wasm
  // core runs on this thread), then uploaded once; the draw lists are cut again by leaf. ----
  const proxiesOn = () => userSettings.value?.values.moving_proxies ?? true;
  const uploadProxies = (p: Proxies) => {
    const col = new Uint8Array(p.pos.length);
    for (const c of p.cells) for (const q of c.parts) {
      const rgb = CATEGORY_SRGB[q.cat] ?? CATEGORY_SRGB[7]!;
      for (let v = q.v0; v < q.v0 + q.nv; v++) col.set(rgb, 3 * v);
    }
    gl.bindBuffer(gl.ARRAY_BUFFER, prxPos);
    gl.bufferData(gl.ARRAY_BUFFER, p.pos, gl.STATIC_DRAW);
    gl.bindBuffer(gl.ARRAY_BUFFER, prxCol);
    gl.bufferData(gl.ARRAY_BUFFER, col, gl.STATIC_DRAW);
    gl.bindVertexArray(prxVao);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, prxIdx);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, p.idx, gl.STATIC_DRAW);
    gl.bindVertexArray(null);
    gl.bindBuffer(gl.ARRAY_BUFFER, prxEdge);
    gl.bufferData(gl.ARRAY_BUFFER, p.edges, gl.STATIC_DRAW);
    const nrm = new Int8Array(2 * p.normals.length); // the two face normals of a segment, for both its vertices
    for (let k = 0; k < p.normals.length / 8; k++) nrm.set(p.normals.subarray(8 * k, 8 * k + 8), 16 * k), nrm.set(p.normals.subarray(8 * k, 8 * k + 8), 16 * k + 8);
    gl.bindBuffer(gl.ARRAY_BUFFER, prxNrm);
    gl.bufferData(gl.ARRAY_BUFFER, nrm, gl.STATIC_DRAW);
    prxBytes = p.pos.byteLength + col.byteLength + p.idx.byteLength + p.edges.byteLength + nrm.byteLength;
    stats.gpuBytes = gpuNow();
  };
  const loadProxies = async (m: Bmsh): Promise<void> => {
    prx = null, prxBytes = 0, prxFor = m;
    delete stats.proxies;
    const ask = source.proxies;
    if (!ask || !proxiesOn()) return;
    let main = 0, tris = 0;
    for (let e = 0; e < m.ids.length; e++) if (!((m.model?.[e] ?? 0) > 0)) main++, tris += (m.ni[e] ?? 0) / 3;
    if (tris < PROXY_MIN_TRIS) return;
    stats.proxiesPending = true;
    // the tree alone first (a budget of 0: the core splits it, no proxy yet), so that the first draw
    // lists are cut by leaf and the proxies, when they come, need no new lists (their index upload is
    // the slowest step of a large model under SwiftShader); the lists wait for it (treeWait)
    treeWait = m;
    try {
      const r = await ask(0);
      if (r.status === 200 && prxFor === m && mesh === m) {
        const t = parseBprx(r.bytes);
        if (t.rev === m.rev && t.nElements === main) prx = t, visKey = "\u0000"; // (lists not made yet: treeWait)
      }
    } catch { /* the proxies are asked again below; the lists are cut again when they come */ }
    if (treeWait === m) treeWait = null;
    dirty = true;
    try { await askProxies(m, ask, main); } finally { if (prxFor === m) stats.proxiesPending = false; }
  };
  /** The mesh whose octree the draw lists wait for (3D2). */
  let treeWait: Bmsh | null = null;
  const askProxies = async (m: Bmsh, ask: NonNullable<MeshSource["proxies"]>, main: number): Promise<void> => {
    const frame = () => new Promise<void>((res) => requestAnimationFrame(() => res()));
    await frame(); // the first frame of this mesh first (ADR 0074: opening is not slower)
    const t0 = performance.now();
    let steps = 0, budget = PROXY_STEP_TRIS, maxStep = 0;
    for (;;) {
      if (prxFor !== m || mesh !== m) return; // a newer mesh
      let r: { status: number; bytes: ArrayBuffer; rev: number };
      const t1 = performance.now();
      try { r = await ask(Math.round(budget)); } catch { return; }
      maxStep = Math.max(maxStep, performance.now() - t1);
      // a step of about PROXY_STEP_MS (the wasm core blocks this thread meanwhile: ADR 0074)
      budget = Math.min(2 * budget, 200000, Math.max(2000, (budget * PROXY_STEP_MS) / Math.max(1, performance.now() - t1)));
      steps++;
      if (r.status === 202) { await frame(); continue; }
      if (r.status !== 200 || prxFor !== m || mesh !== m) return;
      let p: Proxies;
      try { p = parseBprx(r.bytes); } catch (err) { console.warn("gl viewport: proxies:", err); return; }
      if (p.rev !== m.rev || p.nElements !== main) return; // another rev's: the next mesh asks again
      uploadProxies(p);
      const same = prx !== null && prx.leafCount === p.leafCount && prx.cells.length === p.cells.length && prx.leafOf.every((l, e) => l === p.leafOf[e]);
      prx = p, prxUsableKey = "";
      let withProxy = 0, ptris = 0;
      for (const c of p.cells) if (c.parts.length) withProxy++, ptris += c.parts.reduce((n, q) => n + q.ni / 3, 0);
      stats.proxies = { cells: p.cells.length, withProxy, tris: ptris, bytes: r.bytes.byteLength, steps, maxStepMs: maxStep, loadMs: performance.now() - t0,
        drawnCells: 0, drawnTris: 0, standIns: 0, on: true };
      if (!same) visKey = "\u0000"; // the draw lists again, by leaf (none when the tree came first)
      dirty = true;
      return;
    }
  };
  // ---- end 3D2 ----
  const load = async (): Promise<void> => {
    loading = true;
    try {
      const t0 = performance.now();
      epoch = await hostEpoch().catch(() => null);
      // LNK3D (C): the links' baked proxies first (read from their caches: small), drawn at once when nothing is
      // on screen yet (the first picture of a project of links), then the mesh refines it
      const rows = source.linkProxies ? linkDraw() : []; // ([] without links)
      if (rows.length) {
        const tq = performance.now();
        await fetchLinkProxies(rows);
        stats.linkProxiesMs = performance.now() - tq;
        if (!mesh || !mesh.ids.length) {
          prePrx = rows.flatMap((r) => { const x = linkPrx.get(r.name); return x ? [{ x, xf: { c: r.cos, s: r.sin, t: [r.origin[0], r.origin[1], r.origin[2]] } as ModelXf, visible: r.visible, colour: r.colour }] : []; });
          // (no open scene yet: its origin is taken as 0 here; the mesh's arrival places them exactly)
          if (prePrx.length && !fitted) { bounds = prePrxBounds(); ro = roFor(bounds); if (bounds) fit(), fitted = fittedByPreview = true; }
          if (prePrx.length) fitCanvas(), draw(); // at once: on screen before the mesh is asked (its build may take seconds)
        }
      }
      const ab = await source.mesh();
      stats.meshFetches = (stats.meshFetches ?? 0) + 1; // LNK3D: a hide or a link's move never asks
      stats.meshFetchMs = performance.now() - t0;
      stats.meshAt = performance.now(); // 3D2
      let served = ab.byteLength;
      const tp = performance.now();
      // SIZE2: with LNKS version 3 the links' heads only: each link's mesh asked on its own (BMSH version 4),
      // read back, its rows kept; its heavy arrays go once its buffers are on the GPU (drawLists)
      const { main, heads, lazy } = parseHeads(ab);
      const keys: number[] = [];
      if (lazy) {
        if (!source.linkMesh) throw new Error("LNKS version 3 without a way to ask a link's mesh");
        // SIZE3 (B): one link at a time, the smallest first; each read (in a worker), its boxes taken, and its heavy
        // arrays kept only while the decoded bytes kept stay within OPEN_KEEP_BYTES: the others are dropped at once
        // (their rows, boxes and batches stand; their detail is asked again when a frame needs it, budgetStep)
        for (const h of heads) keys.push(h.key);
        const order = heads.map((_, k) => k).filter((k) => heads[k]!.nE).sort((a, b) => heads[a]!.nE - heads[b]!.nE || a - b);
        let kept = 0, full = true;
        stats.openPeakDecoded = 0;
        for (const k of order) {
          const h = heads[k]!;
          const keep = stats.openKeepBytes ?? OPEN_KEEP_BYTES;
          // SIZE3 (B): each link's mesh while the open's budget lasts (uploaded with the open); once a link did not fit,
          // the rest open as their rows and boxes from the host (no mesh moves), their detail asked when a frame wants it
          const r: { bytes?: ArrayBuffer; rows?: Bmsh4Rows; served: number } = full || !source.linkRows || stats.openRows === false
            ? await source.linkMesh(k, h.name, h.key, Math.max(0, keep - kept)) : await source.linkRows(k, h.name, h.key);
          served += r.served;
          let lm = r.rows ? rowsMesh(r.rows) : parseBmsh(r.bytes!, 0, true);
          if (lm.ids.length !== h.nE || lm.rev !== h.key) throw new Superseded(h.name); // (the host checks it too)
          const b = lm.pos.byteLength + lm.idx.byteLength + lm.edges.byteLength + lm.normals.byteLength;
          stats.openPeakDecoded = Math.max(stats.openPeakDecoded, kept + b);
          if (!lm.dropped && kept + b > keep) lm = dropArrays(lm);
          else kept += b;
          if (lm.dropped) full = false; // (the budget is spent: the host's rows answers from now on)
          h.m = lm;
        }
        stats.openKeptBytes = kept, stats.linksDetailedAt = null;
      }
      const m = heads.length ? mergeMesh(main, heads, served) : main;
      stats.meshBytes = served, stats.meshBytesServed = (stats.meshBytesServed ?? 0) + served;
      linkKeys = keys;
      const tu = performance.now();
      // ---- TEST1: the model closed while this answer was on its way: not drawn (the frame loop cleared the view) ----
      if (state.value.file === null) return;
      // ---- end TEST1 ----
      stats.meshRev = m.rev;
      stats.links = m.links ?? [];
      stats.error = null;
      upload(m);
      perfMesh(m); // GPU1
      prePrx = []; // LNK3D (C): the detail is here
      modelPrx = [null, ...(m.links ?? []).map((l) => { const x = linkPrx.get(l.name); return x && x.p.nElements === l.elements ? x : null; })];
      for (const [name, x] of linkPrx) if (!(m.links ?? []).some((l) => l.name === name)) dropLinkPrx(x), linkPrx.delete(name); // a link gone
      seenLinkKey = ""; // LNK3D: asked again once against this mesh
      stats.linkDraw = (m.links ?? []).map((l) => ({ name: l.name, visible: l.visible, colour: l.colour ?? -1, xf: l.xf }));
      stats.loadSteps = { parse: tu - tp, upload: performance.now() - tu, lists: 0, colours: 0 }; // 3D2 (step 5)
      void loadProxies(m); // 3D2: after the first frame, in steps
    } catch (e) {
      if (e instanceof Superseded) { // SIZE2FIX: the links changed while their meshes were asked: asked again, quietly
        stats.linkSuperseded = (stats.linkSuperseded ?? 0) + 1;
        await new Promise((res) => setTimeout(res, 50));
        wantRev = null;
        return;
      }
      stats.error = e instanceof Error ? e.message : String(e);
      console.warn("gl viewport:", stats.error);
      await new Promise((res) => setTimeout(res, 1000)); // the host may be restarting: retry
      wantRev = null;
    } finally {
      loading = false;
    }
  };

  // ---- LNK3D (ADR 0078 amendment 3): a link shown or hidden, recoloured, moved or turned: its flag, its
  // elements' texels or its placement (one texture row and its boxes), never the mesh again ----
  let seenLinkKey = "";
  /** The scene's bounds from the elements' boxes (the clip planes, Fit all). */
  const refitBounds = () => {
    const m = mesh;
    if (!m) return;
    bounds = sceneBounds(m) ?? bounds;
    ro = roFor(bounds); // VIS2
  };
  /** SIZE3 (E): link k's proxies baked after it was ready: asked, then its batches made again by its proxies' leaves
   *  (its detail released: asked again when a frame wants it, grouped). */
  const refreshLinkProxies = async (k: number, r: LinkDraw) => {
    const ask = source.linkProxies, m = mesh;
    if (!ask || !m || !proxiesOn()) return;
    try {
      const a = await ask(k);
      if (a.status !== 200 || mesh !== m) return;
      const p = parseBprx(a.bytes);
      const l = m.links?.[k];
      if (!p.cells.length || !l || p.nElements !== l.elements) return;
      const x = makeLinkPrx(r.name, p);
      stats.linkProxiesLate = (stats.linkProxiesLate ?? 0) + 1;
      const sl = slots[k + 1];
      // VIS2: a link whose detail is on the GPU keeps it: its proxies wait (pendingPrx) and are swapped in, its batches
      // made again by their leaves, when the budget next releases it. Releasing it at once (SIZE3) made every link of a
      // project whose proxies come after the open (SHELL1's bake) ask its whole mesh again, then load it again
      // (kept only when the proxies would not stand for all of it from this camera: a far link drawn by its proxies
      // gives its detail back at once, as before)
      const standsAll = (() => {
        if (!sl?.gl || plan) return true;
        const { e, mvp } = matrices(), usable = usableCells(x.p, (el) => shown[l.e0 + el] === 1);
        const { replaced } = cutLinkPrx(x, l.xf, usable, frustumPlanes(mvp), e, STILL_PROXY_PX);
        return !!replaced && replaced.length > 0 && replaced.every((v) => v === 1);
      })();
      if (sl?.gl && !standsAll && stats.lateProxyRelease !== true) {
        const prev = pendingPrx.get(k + 1);
        if (prev) dropLinkPrx(prev.x);
        pendingPrx.set(k + 1, { x, name: r.name });
        return;
      }
      const old = linkPrx.get(r.name);
      if (old) dropLinkPrx(old);
      linkPrx.set(r.name, x);
      modelPrx[k + 1] = x;
      if (sl) releaseSlot(sl);
      ownOrd[k + 1] = undefined;
      visKey = "\u0000", dirty = true;
    } catch (err) { console.warn("gl viewport: link proxies:", err); }
  };
  const applyLinkDraw = (rows: LinkDraw[]) => {
    const m = mesh, L = m?.links;
    if (!m || !L || rows.length !== L.length || rows.some((r, k) => r.name !== L[k]!.name)) return; // the mesh follows (links_mesh)
    rows.forEach((r, k) => { // SIZE3 (E): proxies baked since
      if ((r.proxies ?? 0) === (prxGen.get(r.name) ?? 0)) return;
      prxGen.set(r.name, r.proxies ?? 0);
      void refreshLinkProxies(k, r);
    });
    let vis = false, col = false;
    const moved: number[] = [];
    rows.forEach((r, k) => {
      const l = L[k]!, xf: ModelXf = { c: r.cos, s: r.sin, t: [r.origin[0] - m.origin[0], r.origin[1] - m.origin[1], r.origin[2] - m.origin[2]] };
      if (l.visible !== r.visible) l.visible = r.visible, vis = true;
      if ((l.colour ?? -1) !== r.colour) l.colour = r.colour, col = true;
      if (!sameXf(l.xf, xf)) l.xf = xf, moved.push(k);
    });
    if (moved.length) {
      for (const k of moved) {
        const l = L[k]!;
        modelXf[k + 1] = l.xf;
        placeLinkBoxes(l.e0, l.e0 + l.elements, l.xf);
        for (const d of dbs) { // its batches' boxes from their items'
          if (d.model !== k + 1) continue;
          const bx = d.b.box;
          bx.set([Infinity, Infinity, Infinity, -Infinity, -Infinity, -Infinity]);
          for (const e of d.items) for (let a = 0; a < 3; a++) bx[a] = Math.min(bx[a]!, boxes[6 * e + a]!), bx[3 + a] = Math.max(bx[3 + a]!, boxes[6 * e + 3 + a]!);
        }
      }
      uploadModels();
      refitBounds();
      occReset();
      lastComplete = false;
      dirty = true;
    }
    if (vis) applyVisibility();
    else if (col) selKey = "\u0000", dirty = true;
    stats.linkDraw = L.map((l) => ({ name: l.name, visible: l.visible, colour: l.colour ?? -1, xf: l.xf }));
  };
  // LNK3D (E): notes on linked elements (query link_notes; ADR 0081 decision 8), drawn on the overlay in 3D
  // and on the live plan: a leader from the anchor up-right to a label. Each anchor is kept in its link's own
  // frame, so the note follows the link's placement (modelXf) without asking the core; hidden with its link
  // or its element; an orphan at its last point in the muted colour; a click on the label selects the note
  // (and its element).
  let notes: { n: LinkNote; model: number; local: number[]; el: number }[] = [];
  let notesKey = "", noteSel: number | null = null;
  let noteBoxes: { i: number; x0: number; y0: number; x1: number; y1: number }[] = [];
  const refreshNotes = () => {
    const m = mesh;
    if (!m) { notes = []; return; }
    const key = `${state.value.links ?? 0}|${m.rev}|${m.size}`;
    if (key === notesKey) return;
    notesKey = key;
    const L = m.links ?? [];
    notes = (L.length ? linkNotes() : []).map((n) => {
      const k = L.findIndex((l) => l.name === n.link), x = k >= 0 ? L[k]!.xf : IDENTITY_XF;
      const w = [n.at[0] - m.origin[0], n.at[1] - m.origin[1], n.at[2] - m.origin[2]];
      const loc = xfRayToLocal(x, w, [0, 0, 0]).e; // the anchor in the link's frame (the core placed it with this xf)
      let el = -1;
      if (k >= 0) for (let e = L[k]!.e0; e < L[k]!.e0 + L[k]!.elements; e++) if (m.ids[e] === n.element) { el = e; break; }
      return { n, model: k + 1, local: loc, el };
    });
    stats.notes = notes.map((x) => ({ link: x.n.link, element: x.n.element, text: x.n.text, drawn: false, x: 0, y: 0 }));
  };
  const NOTE_CSS = "rgb(31, 78, 140)", NOTE_MUTED_CSS = "rgba(110, 110, 116, 0.85)", NOTE_SEL_CSS = "rgb(200, 60, 90)";
  const drawNotes = (ctx: CanvasRenderingContext2D, b: DOMRect) => {
    noteBoxes = [];
    if (!notes.length) return;
    const m = mesh!;
    ctx.save();
    ctx.font = "12px system-ui, sans-serif";
    ctx.textBaseline = "middle";
    notes.forEach((x, i) => {
      const st = stats.notes?.[i];
      if (st) st.drawn = false;
      const l = x.model > 0 ? m.links?.[x.model - 1] : undefined;
      if (!l?.visible || (x.el >= 0 && !shown[x.el])) return; // hidden with its link or its element
      const w = xfPoint(modelXf[x.model] ?? IDENTITY_XF, x.local);
      const p = stats.toScreen?.(w[0] + m.origin[0], w[1] + m.origin[1], w[2] + m.origin[2]);
      if (!p) return;
      const ax = p[0] - b.left, ay = p[1] - b.top, lx = ax + 18, ly = ay - 18;
      const text = x.n.text.length > 60 ? `${x.n.text.slice(0, 57)}…` : x.n.text;
      const tw = ctx.measureText(text).width;
      const css = noteSel === i ? NOTE_SEL_CSS : x.n.orphan ? NOTE_MUTED_CSS : NOTE_CSS;
      ctx.strokeStyle = css, ctx.lineWidth = 1.25;
      ctx.beginPath(); ctx.moveTo(ax, ay); ctx.lineTo(lx, ly); ctx.lineTo(lx + tw + 8, ly); ctx.stroke();
      ctx.beginPath(); ctx.arc(ax, ay, 2.5, 0, 2 * Math.PI); ctx.fillStyle = css; ctx.fill();
      ctx.fillStyle = "rgba(255, 255, 255, 0.85)";
      ctx.fillRect(lx, ly - 15, tw + 8, 14);
      ctx.fillStyle = css;
      ctx.fillText(text, lx + 4, ly - 8);
      noteBoxes.push({ i, x0: lx, y0: ly - 16, x1: lx + tw + 8, y1: ly + 2 });
      if (st) st.drawn = true, st.x = p[0], st.y = p[1];
    });
    ctx.restore();
  };
  const noteAt = (x: number, y: number): number | null => {
    for (const q of noteBoxes) if (x >= q.x0 && x <= q.x1 && y >= q.y0 && y <= q.y1) return q.i;
    return null;
  };
  // ---- end LNK3D ----
  /** visibility.rs hidden_by over the whole sets. */
  const hiddenTest = () => {
    const st = sets;
    if (!st) return () => false;
    const byView = new Set(st.by_view ?? []); // SEC1: the view's class, family, storey, material lists
    const hid = new Set(st.hidden_in_view), iso = new Set(st.isolated_in_view), now = st.now ? new Set(st.now.ids) : null;
    const isolateNow = st.now?.isolate === true;
    return (id: string) => hid.has(id) || byView.has(id) || (iso.size > 0 && !iso.has(id)) || (now !== null && now.has(id) !== isolateNow);
  };
  /** LNK3D (ADR 0078 amendment 3): what is shown, per element, from the visibility sets and the links' own
   *  flags, without touching the draw lists: each batch's shown items (gl/cull.ts batchShown: a batch with
   *  none is skipped, the budget counts the shown ones) and the state texture (colours() next, by selKey).
   *  A linked element is hidden by its GlobalId as the core hid it (M1LINKS: the sets hold its id). */
  const applyVisibility = () => {
    feedbackFrame(); // VIS1: the input's own feedback a cheap frame on a large model (ADR 0074), the complete one after
    const m = mesh;
    if (!m) return;
    const t0 = performance.now();
    const hidden = hiddenTest(), reveal = sets?.reveal === true;
    shown = new Uint8Array(m.ids.length); // (a new array: GPU1's renderer makes its lists again when it changes)
    let nf = 0, nh = 0, lf = 0;
    for (let e = 0; e < m.ids.length; e++) {
      const lk = m.model?.[e] ?? 0;
      if (lk > 0 && !m.links?.[lk - 1]?.visible) { shown[e] = 0; continue; } // a link hidden (Manage links, unloaded)
      const h = hidden(m.ids[e] ?? "");
      shown[e] = !h ? 1 : reveal ? 2 : 0;
      if (shown[e]) { nf += m.ni[e] ?? 0; if (lk > 0) lf += m.ni[e] ?? 0; }
      else if (lk === 0) nh++;
    }
    for (const d of dbs) batchShown(d.b, d.items, shown, d.g ? () => d.g!.ni / 3 : (e) => (m.ni[e] ?? 0) / 3);
    visGen++;
    occReset(); // 3D2: what a batch shows changed: every answer is forgotten
    lastComplete = false;
    stats.linkTris = lf / 3;
    stats.hiddenElements = nh;
    stats.drawnTris = nf / 3;
    selKey = "\u0000"; // the state texture (colours, drawn flags) on this loop
    dirty = true;
    stats.visApplyMs = performance.now() - t0;
  };
  /** The draw lists: the triangles and edge segments of every element (LNK3D: hidden ones too, so a hide
   *  never rebuilds them; the state texture leaves them out), the open model's first, then each link's. */
  const drawLists = () => {
    const m = mesh;
    if (!m) return;
    if (shown.length !== m.ids.length) shown = new Uint8Array(m.ids.length).fill(1);
    let nf = 0, ne = 0;
    for (let e = 0; e < m.ids.length; e++) if (!m.shape?.[e]) nf += m.ni[e] ?? 0, ne += m.ns[e] ?? 0;
    // LNK3D (D): each model's lists in its own arrays (offsets from 0 in each; vertices from its first)
    const nfOf = slots.map(() => 0), neOf = slots.map(() => 0);
    for (let e = 0; e < m.ids.length; e++) if (!m.shape?.[e]) { const md = m.model?.[e] ?? 0; nfOf[md] = nfOf[md]! + (m.ni[e] ?? 0), neOf[md] = neOf[md]! + (m.ns[e] ?? 0); }
    let fi = new Uint32Array(0), ei = new Uint32Array(0);
    let f = 0, k = 0;
    dbs = [];
    const linkDbs: DrawBatch[] = [];
    // Edges: the open model's first, then the links' (drawn in their own colour).
    // 3D1 (step 2): in spatial batches, largest first inside each (gl/cull.ts)
    // 3D2: with proxies, batched by HLOD leaf too (a far cell's proxy stands for its leaves' batches)
    const p = prx, grp = (e: number) => {
      const mdl = m.model?.[e] ?? 0;
      if (mdl === 0) return p ? p.leafOf[e] ?? -1 : -1;
      const x = modelPrx[mdl], l = m.links?.[mdl - 1]; // LNK3D (C): a link's batches cut by its own leaves
      return x && l ? x.p.leafOf[e - l.e0] ?? -1 : -1;
    };
    listsGen++;
    occReset(); // 3D2: the batches change (a model): every answer is forgotten
    elFace = new Int32Array(m.ids.length).fill(-1), elEdge = new Int32Array(m.ids.length).fill(-1);
    elInst = new Int32Array(m.ids.length).fill(-1), elGroup = new Int32Array(m.ids.length).fill(-1);
    lastComplete = false;
    const nModels = 1 + (m.links?.length ?? 0);
    const byModel: number[][] = Array.from({ length: nModels }, () => []);
    for (let e = 0; e < m.ids.length; e++) if (!m.shape?.[e]) byModel[m.model?.[e] ?? 0]!.push(e);
    const fiOf: Uint32Array[] = [], eiOf: Uint32Array[] = [];
    for (let mdl = 0; mdl < nModels; mdl++) { // LNK3D: a model's batches never mix with another's (its matrix, its memory)
      const links = mdl > 0, sl = slots[mdl]!, sb = sl.s0;
      // SIZE2: a link's batches are made once per mesh and kept (its face list on the GPU follows that order; its
      // heavy arrays may be gone): made again only on a new mesh
      const P = partOf(mdl), saved = links ? ownOrd[mdl] : undefined;
      fi = new Uint32Array(P ? nfOf[mdl] ?? 0 : 0), ei = new Uint32Array(2 * (neOf[mdl] ?? 0)), f = 0, k = 0;
      const { order: ord, batches: bs } = saved ?? makeBatches(byModel[mdl]!, boxes, (e) => (m.ni[e] ?? 0) / 3, (e) => m.ns[e] ?? 0, BATCH_TRIS, grp);
      if (links) ownOrd[mdl] = { order: ord, batches: bs };
      for (const b of bs) {
        (links ? linkDbs : dbs).push({ b, f0: f, e0: k, link: links, model: mdl, items: Uint32Array.from(ord.slice(b.first, b.first + b.n)), g: null, k0: 0, k1: b.n });
        for (let j = b.first; j < b.first + b.n; j++) {
          const e = ord[j]!;
          const i0 = m.i0[e] ?? 0, n = m.ni[e] ?? 0;
          elFace[e] = f, elEdge[e] = k; // 3D2 (step 4)
          if (P) for (let i = 0, a = i0 - sl.i0; i < n; i++) fi[f + i] = P.idx[a + i]!; // (SIZE2: its own numbering)
          f += n;
          const s0 = m.s0[e] ?? 0, ns = m.ns[e] ?? 0;
          for (let q = s0 - sb; q < s0 - sb + ns; q++) ei[k++] = 2 * q, ei[k++] = 2 * q + 1;
        }
      }
      if (!links) edgeMainN = k, faceMainN = f;
      fiOf.push(fi), eiOf.push(ei);
    }
    faceN = nf;
    edgeN = 2 * ne;
    // ---- 3D1: the instances, grouped by (model, shape, mirrored) ----
    const order: number[] = [];
    if (m.shape) for (let e = 0; e < m.ids.length; e++) if (m.shape[e]) order.push(e);
    const P = m.place ?? new Float32Array(0);
    const mirroredOf = (e: number) => {
      const p = P.subarray(12 * e, 12 * e + 12);
      return p[0]! * (p[4]! * p[8]! - p[5]! * p[7]!) + p[1]! * (p[5]! * p[6]! - p[3]! * p[8]!) + p[2]! * (p[3]! * p[7]! - p[4]! * p[6]!) < 0;
    };
    const key = (e: number) => (m.model?.[e] ?? 0) * 2 ** 33 + (m.shape?.[e] ?? 0) * 2 + (mirroredOf(e) ? 1 : 0);
    const keys = new Map<number, number>();
    for (const e of order) keys.set(e, key(e));
    order.sort((a, b) => keys.get(a)! - keys.get(b)! || a - b);
    groups = [];
    const byGroup: number[][] = [];
    order.forEach((e) => {
      const g = groups[groups.length - 1], kk = keys.get(e)!;
      if (g && keys.get(byGroup[byGroup.length - 1]![0]!) === kk) { g.count++; byGroup[byGroup.length - 1]!.push(e); return; }
      byGroup.push([e]);
      const md = m.model?.[e] ?? 0; // LNK3D (D): its segments from its model's first
      groups.push({ shape: m.shape![e] ?? 1, i0: shapeI0[(m.shape![e] ?? 1) - 1] ?? 0, ni: m.ni[e] ?? 0, s0: (m.s0[e] ?? 0) - (slots[md]?.s0 ?? 0), ns: m.ns[e] ?? 0, first: 0, count: 1,
        mirrored: mirroredOf(e), link: md > 0, model: md, dbs: [] });
    });
    // 3D1 (step 2): each group's instances in spatial batches, largest first inside each (LNK3D (D): numbered
    // in its model's instance list)
    const inst: number[] = [], instOf: number[][] = slots.map(() => []);
    groups.forEach((g, gi) => {
      g.first = instOf[g.model]!.length;
      const { order: ord, batches: bs } = makeBatches(byGroup[gi]!, boxes, () => g.ni / 3, () => g.ns, BATCH_TRIS, grp);
      for (const b of bs) {
        const d: DrawBatch = { b, f0: 0, e0: 0, link: g.link, model: g.model, items: Uint32Array.from(ord.slice(b.first, b.first + b.n)), g, k0: 0, k1: b.n };
        g.dbs.push(d);
        (g.link ? linkDbs : dbs).push(d);
      }
      inst.push(...ord);
      instOf[g.model]!.push(...ord);
    });
    // LNK3D: the links' batches by model (merged then instanced each), after the open model's
    linkDbs.sort((x, y) => x.model - y.model);
    dbs.push(...linkDbs);
    instEls = Uint32Array.from(inst);
    for (const l of instOf) l.forEach((e, i) => { elInst[e] = i; }); // 3D2 (step 4) (LNK3D: in its model's list)
    groups.forEach((g, gi) => { const l = instOf[g.model]!; for (let i = g.first; i < g.first + g.count; i++) elGroup[l[i]!] = gi; });
    // LNK3D (D): each model's lists kept with its slot and uploaded into its buffers (a released one: when it comes back)
    listBytes = 0;
    slots.forEach((x, mdl) => {
      const li = instOf[mdl] ?? [], mats = new Float32Array(12 * li.length);
      li.forEach((e, i) => mats.set(P.subarray(12 * e, 12 * e + 12), 12 * i));
      instOfNow[mdl] = li; // SIZE2: a released link's lists are made again from these
      if (!partOf(mdl)) { // SIZE2: its heavy arrays gone: its face list stays as the GPU holds it, its instances again
        if (x.gl && x.listsOnGpu) {
          gl.bindBuffer(gl.ARRAY_BUFFER, x.gl.instMat);
          gl.bufferData(gl.ARRAY_BUFFER, mats, gl.STATIC_DRAW);
          gl.bindBuffer(gl.ARRAY_BUFFER, x.gl.instEl);
          gl.bufferData(gl.ARRAY_BUFFER, Uint32Array.from(li), gl.STATIC_DRAW);
        }
        listBytes += x.listBytes ?? 0;
        return;
      }
      if (x.gl && x.lists) x.bytes -= x.lists.fi.byteLength + x.lists.ei.byteLength + x.lists.mats.byteLength + x.lists.inst.byteLength;
      x.lists = { fi: fiOf[mdl] ?? new Uint32Array(0), ei: eiOf[mdl] ?? new Uint32Array(0), mats, inst: Uint32Array.from(li) };
      uploadLists(x);
      listBytes += x.lists.fi.byteLength + x.lists.ei.byteLength + mats.byteLength + 4 * li.length;
      dropPart(x); // SIZE2: a link on the GPU keeps no copy in the page
    });
    stats.partBytes = slots.map((x) => { const q = partOf(x.k); return q ? q.pos.byteLength + q.idx.byteLength + q.edges.byteLength + q.normals.byteLength : 0; });
    // ---- end 3D1 ----
    stats.gpuBytes = gpuNow();
    stats.listsBuilt = (stats.listsBuilt ?? 0) + 1;
    elDb = new Int32Array(m.ids.length).fill(-1);
    dbs.forEach((d, i) => { for (const e of d.items) elDb[e] = i; });
    // VIS1: each batch's box from its items' boxes as they are now (a link's batches are kept from its first list,
    // made before its boxes were placed: the frustum's test then left out elements on screen)
    for (const d of dbs) {
      const bx = d.b.box;
      bx.set([Infinity, Infinity, Infinity, -Infinity, -Infinity, -Infinity]);
      for (const e of d.items) for (let a = 0; a < 3; a++) bx[a] = Math.min(bx[a]!, boxes[6 * e + a]!), bx[3 + a] = Math.max(bx[3 + a]!, boxes[6 * e + 3 + a]!);
    }
    applyVisibility();
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
    if (gpuDraw()) return; // GPU1: the seam (else the WebGL2 frame below, untouched)
    if (tgt && !idMode) tgt.bind(canvas.width, canvas.height); // VIS1: the frame is drawn offscreen, then presented
    gl.viewport(0, 0, canvas.width, canvas.height);
    gl.clearColor(BACKGROUND_SRGB[0]! / 255, BACKGROUND_SRGB[1]! / 255, BACKGROUND_SRGB[2]! / 255, 1);
    if (idMode) gl.clearColor(0, 0, 0, 0); // VIS1: the ground-truth pass (no element: 0)
    gl.clearDepth(1);
    gl.depthMask(true);
    // 3D1: what this frame is: moving (the camera changed), a slice of the fill after a stop (added to
    // the kept picture), or the whole picture
    const key = camKey(), tNow = performance.now();
    const first = firstOfMesh && !!mesh; // 3D2: the first frame of a mesh, budgeted
    if (first) firstOfMesh = false;
    // VIS1 (P0-a): in motion when the camera changed, or changed within motionWin() (a frame for another reason
    // meanwhile); budgeted (movingNow) when this setting has a budget for it (moving_fps 0: only once capped)
    const camChanged = first || (lastCamKey !== null && key !== lastCamKey);
    if (camChanged) lastCamChangeAt = tNow;
    const motion = (camChanged || tNow - lastCamChangeAt < motionWin()) && !ghostN && !markN;
    const fps0 = (userSettings.value?.values.moving_fps ?? 30) <= 0, rate = trisPerMs || wholeRate;
    const sliceNext = !motion && (fill !== null || fillEdges);
    capped = fps0 && (motion || (sliceNext && capped)) && rate > 0 && lastAllTris / rate > CAP_MS;
    const movingNow = motion && frameBudget() < Infinity;
    lastCamKey = key;
    if (movingNow || (fill === null && !fillEdges)) fill = null, fillEdges = false;
    const slice = !movingNow && (fill !== null || fillEdges);
    if (!slice || fill?.t === Infinity) { // a fill's first slice starts over
      if (tgt && !idMode) tgt.clear(BG01);
      else gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    }
    const m = mesh;
    if (!m) { stats.boxHandles = []; drawProxyPreview(); if (tgt) tgt.present(screenEdgesNow(), E0, E1); drawOverlay(); return; } // ISS71: no handles (LNK3D: the links' proxies before their mesh)
    const { e, mvp } = matrices();
    ftime.begin(); // VIS1
    const light = norm(add(norm(sub(e, target)), [-0.3, -0.2, 0.9]));
    const box = clipBox();
    stats.clip = box;
    gl.useProgram(prog);
    gl.uniform1i(uId, idMode ? 1 : 0), gl.uniform1i(uCheap, stats.cheapFrag ? 1 : 0); // VIS1 (uCheap: a bench hook)
    gl.uniform3fv(uLight, light);
    // A plan's eye is at infinity: the back-edge test takes its direction (a vertical side's edges stay).
    setCamera(mvp, plan || ortho3d() ? norm(sub(e, target)) : e, !!plan || ortho3d()); // Q2: an orthographic 3D eye is at infinity too
    gl.uniform1f(uEyeDir, plan || ortho3d() ? 1 : 0);
    gl.uniform1f(uPoint, 1);
    gl.uniform1f(uAlpha, 1);
    gl.uniform1i(uClip, box ? 1 : 0);
    { // VIS2: the box's frame relative to the render origin (the fragment's position is): its bounds minus the origin turned by -yaw
      const cy = Math.cos(box?.yaw ?? 0), sy = Math.sin(box?.yaw ?? 0), q = [cy * ro[0] + sy * ro[1], cy * ro[1] - sy * ro[0], ro[2]];
      gl.uniform3fv(uClipMin, box ? box.min.map((v, k) => v - q[k]!) : [0, 0, 0]);
      gl.uniform3fv(uClipMax, box ? box.max.map((v, k) => v - q[k]!) : [0, 0, 0]);
      gl.uniform2f(uClipRot, cy, sy); // ISS71: the turned box
    }
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
    gl.disableVertexAttribArray(1); // LNK3D: a face's colour is its element's state (u_el_col)
    gl.uniform1i(uElCol, 1), setModel(0);
    // The open model's faces, then the links' pushed a hair behind (bimble-render LINKED_DEPTH_PUSH:
    // where both have a face, the model's wins).
    let fTris = 0, fDraws = 0, linkPrxTris = 0; // 3D1: what this frame submits (LNK3D: of it, links' proxies)
    // ---- 3D1 (steps 2, 3): what each batch draws this frame: none when its box is off screen (frustum
    // culling, the planes of the view-projection), else its items at least `minSize` big while the view
    // moves (ADR 0035), all of them when it is still. ----
    const planes = stats.noCull ? null : frustumPlanes(mvp);
    const pxK = (2 * Math.tan(FOV_Y / 2)) / Math.max(1, canvas.height), orthoMmPx = plan ? (2 * half) / Math.max(1, canvas.height) : distance * pxK; // LNK3D (F)
    let culled = 0, budget = Infinity, cutT = 0;
    const seen: { d: DrawBatch; c: Cut }[] = [];
    const orthoEye = plan !== null || ortho3d();
    // ---- 3D2 (ADR 0078; lib.rs:625 proxied): far cells drawn as their proxy, their leaves' batches not.
    // Not in a plan or with a section box (Rust: not with a clip), nor with the setting off. ----
    let prxCells: number[] = [], replaced: Uint8Array | null = null;
    const prxOn = prx !== null && !stats.noProxies && !box && !plan && proxiesOn();
    if (prxOn) {
      const p = prx!, uk = `${listsGen}|${visGen}|${selKey}`;
      if (uk !== prxUsableKey) { // proxies.rs usable: every element under the cell drawn plainly
        prxUsableKey = uk;
        const sel = new Set(state.value.linked ? [] : state.value.selection);
        prxUsable = usableCells(p, (el) => shown[el] === 1 && !sel.has(m.ids[el] ?? ""));
      }
      const k = (2 * Math.tan(FOV_Y / 2)) / Math.max(1, canvas.height);
      const mmPerPx = ortho3d() ? () => distance * k : (d: number) => d * k;
      ({ cells: prxCells, replaced } = cutProxies(p, prxUsable, planes ? (b) => boxInFrustum(planes, b, 0) : null, e, mmPerPx, movingNow ? MOVING_PROXY_PX : STILL_PROXY_PX));
      // while moving, the proxies take at most half the budget (coarser cells, else the largest give way)
      if (movingNow && prxCells.length) prxCells = fitProxies(p, prxUsable, prxCells, replaced, frameBudget() / 2);
    }
    // ---- end 3D2 ----
    // ---- LNK3D (C): each shown link's proxies, cut with the eye in its frame (same rules: not in a plan, not
    // with a section box, not over a hidden or selected element: usableCells) ----
    const linkCut: { x: LinkPrx; cells: number[]; mdl: number; colour: number }[] = [];
    const linkRep: (Uint8Array | null)[] = [], linkNeed: (Uint8Array | null)[] = []; // (D: the leaves the normal cut replaces)
    if (!stats.noProxies && !box && !plan && proxiesOn() && m.links) {
      const linked = state.value.linked ?? null;
      m.links.forEach((l, k) => {
        const x = modelPrx[k + 1];
        if (!x || !l.visible) return;
        const uk = `${listsGen}|${visGen}|${selKey}`;
        if (x.usableKey !== uk) {
          x.usableKey = uk;
          x.usable = usableCells(x.p, (el) => shown[l.e0 + el] === 1 && !(linked && linked.link === l.name && linked.id === m.ids[l.e0 + el]));
        }
        const normal = cutLinkPrx(x, l.xf, x.usable, planes, e, movingNow ? MOVING_PROXY_PX : STILL_PROXY_PX);
        // LNK3D (D): its detail released: its coarsest usable proxies stand for it until it is back
        // SIZE3 (B): a released link the last frame did not want in detail (its proxies' normal cut covers what is in
        // view) is drawn by that cut, as it would be with its detail on the GPU
        const { cells, replaced: r } = slots[k + 1]?.gl || wantedLast.get(k + 1) === false ? normal : cutLinkPrx(x, l.xf, x.usable, planes, e, Infinity);
        linkRep[k + 1] = r, linkNeed[k + 1] = normal.replaced;
        if (cells.length) linkCut.push({ x, cells, mdl: k + 1, colour: l.colour ?? -1 });
      });
    }
    for (const x of linkPrx.values()) x.drawnCells = x.drawnTris = 0;
    // ---- end LNK3D (C) ----
    const missing: ArrayLike<number>[] = []; // LNK3D (D): batches needed while their link's detail is away: their boxes
    drawFrame++; // SIZE2FIX: a link is needed by frames drawn, not by wall time
    const needNow = performance.now();
    const need = (mdl: number) => { const x = slots[mdl]; if (x && mdl > 0) x.needFrame = drawFrame, x.needAt = needNow; };
    for (const d of dbs) {
      if (d.b.shown === 0) { d.k0 = d.k1 = 0; continue; } // LNK3D: every element of it hidden (a link hidden too)
      const sl = slots[d.model];
      if (sl && !sl.gl) { // LNK3D (D): its link's detail released: asked again when this frame would draw it
        if (!planes || boxInFrustum(planes, d.b.box, 0)) {
          const nr = linkNeed[d.model], r2 = linkRep[d.model];
          if (!(nr && d.b.group >= 0 && nr[d.b.group])) {
            wanted.add(d.model), need(d.model);
            const bx = d.b.box, q = [0, 1, 2].map((a) => Math.min(Math.max(e[a]!, bx[a]!), bx[3 + a]!)); // SIZE3 (B): its size on screen
            const sz = Math.hypot(bx[3]! - bx[0]!, bx[4]! - bx[1]!, bx[5]! - bx[2]!) / (orthoEye ? 1 : Math.max(1, Math.hypot(q[0]! - e[0], q[1]! - e[1], q[2]! - e[2])));
            if (sz > (wantScore.get(d.model) ?? 0)) wantScore.set(d.model, sz);
          }
          if (!(r2 && d.b.group >= 0 && r2[d.b.group])) missing.push(d.b.box);
        }
        d.k0 = d.k1 = 0;
        continue;
      }
      const rp = d.model === 0 ? replaced : linkRep[d.model] ?? null; // LNK3D (C): a link's own leaves
      if (rp && d.b.group >= 0 && rp[d.b.group]) { d.k0 = d.k1 = 0; continue; } // 3D2: a proxy stands for it
      if (planes && !boxInFrustum(planes, d.b.box, 0)) { d.k0 = d.k1 = 0; culled++; continue; }
      // the nearest point of the batch's box: its items are at most that near (largest on screen first)
      const bx = d.b.box; // (VIS1 P0-c: no array, no Math.hypot a batch: 46,800 of them a frame on a project of links)
      const qx = Math.min(Math.max(e[0]!, bx[0]!), bx[3]!) - e[0]!, qy = Math.min(Math.max(e[1]!, bx[1]!), bx[4]!) - e[1]!, qz = Math.min(Math.max(e[2]!, bx[2]!), bx[5]!) - e[2]!;
      const dist = orthoEye ? 1 : Math.max(1, Math.sqrt(qx * qx + qy * qy + qz * qz));
      // LNK3D (F): edges only where they read: a batch whose box spans at least EDGE_MIN_PX on screen
      const sx = bx[3]! - bx[0]!, sy = bx[4]! - bx[1]!, sz = bx[5]! - bx[2]!;
      d.noEdge = Math.sqrt(sx * sx + sy * sy + sz * sz) / (orthoEye ? orthoMmPx : dist * pxK) < EDGE_MIN_PX;
      d.dist = dist; // (VIS1: front to back)
      seen.push({ d, c: { b: d.b, dist, k0: slice && fill ? (fill.id && d.fillDone === fill.id ? d.b.n : cutEnd({ b: d.b, dist, k0: 0 }, fill.t)) : 0 } });
      need(d.model); // SIZE2FIX: in view and not stood for by a proxy: needed, whatever this frame's budget or pass draws
    }
    // ---- VIS1: what the last picture at this camera hides (gl/hiz.ts, gl/visible.ts) is left out: still frames and the
    // fill's slices in 3D, when the pyramid's key is this frame's (camera, size, what is shown, lists, section box, links).
    // VIS2: decided before the budget's cut, which then counts only what may be drawn (a still picture's fill on a project
    // of links spent its slices on hidden items: hundreds of slices of 50 ms, the page never idle) ----
    const vkey = visKeyOf(key, box);
    const pyr = hiz && visibleOn() && !movingNow && !plan && !idMode ? hiz.ready : null;
    const useVis = pyr !== null && pyr.key === vkey;
    let visHid = 0, visHidTris = 0, visMasked = 0;
    const tVis = performance.now();
    const vk = useVis ? `${pyr.key}#${pyr.serial}` : "";
    if (useVis) {
      const kk = `${vk}|${visGen}`;
      for (const { d, c } of seen) {
        if (d.vkey !== vk || !d.vmask) { maskBatch(pyr, d, boxes, shown, d.g ? () => d.g!.ni / 3 : (i) => (m.ni[d.items[i]!] ?? 0) / 3); visMasked++; d.runsSerial = -1; }
        if (d.kcumKey !== kk || !d.kcum) {
          const base = d.b.vcum ?? d.b.cum, mask = d.vmask!, kc = d.kcum && d.kcum.length === d.b.n + 1 ? d.kcum : new Uint32Array(d.b.n + 1);
          for (let i = 0; i < d.b.n; i++) kc[i + 1] = kc[i]! + (mask[i] ? base[i + 1]! - base[i]! : 0);
          d.kcum = kc, d.kcumKey = kk;
        }
        c.cum = d.kcum;
      }
    }
    if (slice && fillEdges) for (const { d } of seen) d.k0 = d.k1 = 0; // the last pass: edges only (below)
    else {
      if (movingNow || slice) budget = frameBudget();
      // VIS2: a slice of the fill gets at least what a whole frame draws in a moving frame's time (wholeRate: whole frames'
      // triangles over their fenced time). Slices draw few triangles for a fixed script cost (the batches' loop: tens of
      // ms on 46,800 batches), so their own rate collapsed to the minimum budget and the fill took minutes
      if (slice && wholeRate > 0 && budget < Infinity) budget = Math.max(budget, wholeRate * (1000 / Math.max(1, userSettings.value?.values.moving_fps || 30)));
      // 3D2: while moving, the batches take what the proxies leave of the budget
      if (movingNow && prxCells.length && budget < Infinity) budget = Math.max(MIN_MOVING_TRIS, budget - prxCells.reduce((n, k) => n + prx!.tris[k]!, 0));
      // VIS1 (P0-c): many batches (a project of links): the bucketed cut (gl/cull.ts budgetCutFast), the same rule
      let cut = seen.length > 2000 && budget < Infinity && stats.exactCut !== true ? budgetCutFast(seen.map((x) => x.c), budget, `${visGen}|${listsGen}|${vk}`) : budgetCut(seen.map((x) => x.c), budget);
      // VIS2: a slice whose remainder (what may still be drawn) a whole frame draws within CAP_MS finishes the picture now
      // (decision 44's rule for the fill): on the owner's project the fill crawled a few thousand triangles a slice (items of
      // 22,000 triangles at nearly one size: each slice took one), the page awake for minutes after a zoom
      if (slice && cut.t > 0 && wholeRate > 0) {
        let rem = 0;
        for (const { c } of seen) { const k = c.cum ?? c.b.vcum ?? c.b.cum; rem += k[c.b.n]! - k[c.k0]!; }
        if (rem / wholeRate <= CAP_MS) cut = { t: 0, tris: rem };
      }
      cutT = cut.t;
      const stillPx = stats.stillPx ?? opt().still_px; // VIS1: the setting still_px (0: off; a bench hook overrides)
      if (!movingNow && !slice && !idMode && stillPx > 0) { // VIS1 (option 3): what is under stillPx px left out of a still frame
        cutT = stillPx * (orthoEye ? orthoMmPx : pxK);
        for (const { d, c } of seen) d.k1 = cutEnd(c, cutT);
      }
      if (!slice) { let all = 0; for (const { c } of seen) { const k = c.cum ?? c.b.vcum ?? c.b.cum; all += k[c.b.n]! - k[c.k0]!; } lastAllTris = all; } // VIS1 (P0-a) (VIS2: what may be drawn)
      for (const { d, c } of seen) d.k0 = c.k0, d.k1 = cutEnd(c, cut.t);
      // VIS2: a slice the size threshold stalls (the next size level alone is over the budget: thousands of copies of one
      // 22,000-triangle shape at one size on the owner's project, the slice took one of them, the fill crawled for half a
      // minute) is filled with whole batches, each then skipped by the rest of this fill (fillDone)
      if (slice && fill?.id && cut.t > 0 && budget < Infinity) {
        let kept = 0;
        for (const { d, c } of seen) { const k = c.cum ?? c.b.vcum ?? c.b.cum; kept += k[d.k1]! - k[d.k0]!; }
        if (kept < budget / 4) {
          let room = budget - kept;
          for (const { d, c } of seen) {
            if (room <= 0) break;
            if (d.k1 >= d.b.n) continue;
            const k = c.cum ?? c.b.vcum ?? c.b.cum, more = k[d.b.n]! - k[d.k1]!;
            if (more > room && kept > 0) continue;
            d.k1 = d.b.n, d.fillDone = fill.id, room -= more, kept += more;
          }
        }
      }
    }
    if (movingNow) for (const { d } of seen) if (d.k1 < d.b.n) d.noEdge = true; // LNK3D (F): a part drawn while moving: faces only
    // (VIS1: the runs of what may be seen, from the masks made above)
    for (const d of dbs) d.runs = null;
    if (useVis) for (const { d } of seen) {
      if (d.k1 <= d.k0) continue;
      // the runs and what they leave out, kept per mask and range (a still picture's frames and slices repeat them)
      if (d.runsSerial !== pyr.serial || d.runsK0 !== d.k0 || d.runsK1 !== d.k1) {
        const mask = d.vmask!;
        let h = 0, ht = 0;
        for (let i = d.k0; i < d.k1; i++) if (!mask[i]) h++, ht += d.g ? d.g.ni / 3 : (m.ni[d.items[i]!] ?? 0) / 3;
        d.runsMemo = runsOf(mask, d.k0, d.k1), d.runsSerial = pyr.serial, d.runsK0 = d.k0, d.runsK1 = d.k1, d.hid = h, d.hidTris = ht;
      }
      d.runs = d.runsMemo!;
      visHid += d.hid!, visHidTris += d.hidTris!;
    }
    if (!idMode) {
      let why: string | undefined;
      if (!useVis && hiz && visibleOn() && !movingNow && !plan) {
        const r = hiz.ready;
        if (!r) why = hiz.pending ? "pending" : "none";
        else { // (the key's last six parts are named; what is before them is the camera)
          const a = r.key.split("|"), b = vkey.split("|"), names = ["size", "shown", "lists", "box", "links", "detail"], out: string[] = [];
          for (let i = 1; i <= 6; i++) if (a[a.length - i] !== b[b.length - i]) out.push(names[6 - i]!);
          if (a.slice(0, -6).join("|") !== b.slice(0, -6).join("|")) out.push("camera");
          why = out.join(",") + (hiz.pending ? " (pending)" : "");
        }
      }
      stats.vis = { on: visibleOn(), used: useVis, pyramid: hiz?.ready ? hiz.ready.key === vkey : false, hidden: visHid, hiddenTris: visHidTris, masked: visMasked, ms: performance.now() - tVis, ...(why === undefined ? {} : { why }) };
    }
    // ---- end VIS1 ----
    // the outlines of what a moving frame could not draw (its batches not finished)
    let unDs = movingNow ? seen.filter(({ d }) => d.k1 < d.b.n) : [];
    // 3D2: a leaf it could not finish is drawn as the proxy of the deepest usable cell holding it instead
    // (its batches left out), where there is one; the outlines stay for the rest
    let nStand = 0;
    if (prxOn && replaced && unDs.length) {
      const leaves = new Set<number>();
      for (const { d } of unDs) if (d.b.group >= 0) leaves.add(d.b.group);
      const used = prxCells.reduce((n, k) => n + prx!.tris[k]!, 0);
      const extra = standIns(prx!, prxUsable, leaves, replaced, Math.max(0, frameBudget() / 2 - used));
      nStand = extra.length;
      if (extra.length) {
        prxCells.push(...extra);
        const r = replaced;
        for (const { d } of seen) if (d.b.group >= 0 && r[d.b.group]) d.k0 = d.k1 = 0;
        unDs = unDs.filter(({ d }) => !(d.b.group >= 0 && r[d.b.group]));
      }
    }
    // VIS1 (C-i): a link's batches a moving frame could not finish are drawn as its proxies (the deepest usable cell
    // holding each leaf, within half the budget), not as their boxes' outlines; outlines stay where no cell has one
    if (movingNow && unDs.length && !box && !plan && proxiesOn() && !stats.noProxies && m.links) {
      const byModel = new Map<number, Set<number>>();
      for (const { d } of unDs) if (d.model > 0 && d.b.group >= 0) { let set = byModel.get(d.model); if (!set) byModel.set(d.model, (set = new Set())); set.add(d.b.group); }
      let room = Math.max(0, frameBudget() / 2 - linkCut.reduce((n, c) => n + c.cells.reduce((t, k) => t + c.x.p.tris[k]!, 0), 0));
      for (const [mdl, leaves] of byModel) {
        const x = modelPrx[mdl], r = linkRep[mdl];
        if (!x || !r || !x.usable.length) continue;
        const extra = standIns(x.p, x.usable, leaves, r, room);
        if (!extra.length) continue;
        room = Math.max(0, room - extra.reduce((n, k) => n + x.p.tris[k]!, 0));
        const ent = linkCut.find((c) => c.mdl === mdl);
        if (ent) ent.cells.push(...extra);
        else linkCut.push({ x, cells: extra, mdl, colour: m.links[mdl - 1]?.colour ?? -1 });
        nStand += extra.length;
        for (const { d } of seen) if (d.model === mdl && d.b.group >= 0 && r[d.b.group]) d.k0 = d.k1 = 0;
        unDs = unDs.filter(({ d }) => !(d.model === mdl && d.b.group >= 0 && r[d.b.group]));
      }
    }
    const unfinished = [...unDs.map(({ d }) => d.b.box), ...missing];
    for (const x of slots) x.drawn = false; // LNK3D (D): a link drawn in detail by the last frame is needed (on screen)
    for (const d of dbs) if (d.k1 > d.k0 && d.model > 0) { const x = slots[d.model]; if (x) x.drawn = true; }
    let cutKept = 0, cutRem = 0; // VIS2: what the cut chose (its kept triangles) and what remained after it
    for (const { d, c } of seen) { const k = c.cum ?? c.b.vcum ?? c.b.cum; cutKept += k[d.k1]! - k[d.k0]!; cutRem += k[c.b.n]! - k[d.k1]!; }
    stats.moving = { moving: movingNow, slice, budget, t: cutT, batches: dbs.length, culled, outlines: unfinished.length, tris: 0, proxied: prxCells.length, motion, capped, cutKept, cutRem };
    // 3D2: the proxies' runs (cells in order: their parts lie side by side), faces in a frame that starts
    // over, edges then and in the last pass of a fill
    const drawPrx = prxCells.length > 0 && (!slice || fill?.t === Infinity);
    let prxTris = 0;
    const prxRuns = (seg: boolean): [number, number][] => {
      const runs: [number, number][] = [];
      for (const k of [...prxCells].sort((a, b) => a - b)) for (const q of prx!.cells[k]!.parts) {
        const a = seg ? q.s0 : q.i0, n = seg ? q.ns : q.ni, last = runs[runs.length - 1];
        if (!n) continue;
        if (last && last[0] + last[1] === a) last[1] += n;
        else runs.push([a, n]);
      }
      return runs;
    };
    const prxFaces = () => {
      if (!drawPrx) return;
      gl.bindVertexArray(prxVao);
      for (const [a, n] of prxRuns(false)) gl.drawElements(gl.TRIANGLES, n, gl.UNSIGNED_INT, 4 * a), fDraws++, fTris += n / 3, prxTris += n / 3;
      gl.bindVertexArray(vao);
    };
    // 3D1: the instanced groups after the merged faces of the same model (the open one, then the links);
    // a mirrored placement turns the winding, so its group takes the other front face.
    const front = box && mirror() ? gl.CW : gl.CCW;
    // ---- 3D2 (step 3): occlusion queries (stats.occlusion; not with a section box, in a plan or in a
    // slice of the fill). The open model's batches nearest first, one draw each: a batch found hidden
    // leaves its faces out (its edges stay, Rust's rule) and its box asks, without colour or depth,
    // when it shows again; anything else that changes forgets the answers (occReset). ----
    const occActive = stats.occlusion === true && !box && !plan && !slice;
    const ok = `${listsGen}|${JSON.stringify(box)}|${viewId}|${occActive}`;
    if (ok !== occKey) occKey = ok, occReset();
    else if (occActive) pollOcc();
    const hiddenDs: DrawBatch[] = [];
    let occAsked = 0;
    const ask = (d: DrawBatch, drawIt: () => void) => {
      const o = (d.o ??= { q: null, box: false, qAt: "", hidden: false, hidAt: "" });
      if (o.hidden) { hiddenDs.push(d); return; }
      if (o.q) { drawIt(); return; }
      o.q = gl.createQuery(), o.box = false, o.qAt = key, occAsked++;
      gl.beginQuery(gl.ANY_SAMPLES_PASSED_CONSERVATIVE, o.q);
      drawIt();
      gl.endQuery(gl.ANY_SAMPLES_PASSED_CONSERVATIVE);
    };
    const occFaces = () => {
      let inInst = false;
      for (const { d } of seen.filter((x) => !x.d.link && x.d.k1 > x.d.k0).sort((x, y) => x.c.dist - y.c.dist)) {
        const g = d.g;
        ask(d, () => {
          if (!g) {
            if (inInst) gl.bindVertexArray(vao), inInst = false;
            const s = d.f0 + 3 * d.b.cum[d.k0]!, t = d.f0 + 3 * d.b.cum[d.k1]!;
            gl.drawElements(gl.TRIANGLES, t - s, gl.UNSIGNED_INT, 4 * s), fDraws++, fTris += (t - s) / 3;
            return;
          }
          if (!inInst) gl.bindVertexArray(instVao), inInst = true;
          const s = g.first + d.b.first + d.k0, t = g.first + d.b.first + d.k1;
          instAttrs(s, true);
          if (g.mirrored) gl.frontFace(front === gl.CCW ? gl.CW : gl.CCW);
          gl.drawElementsInstanced(gl.TRIANGLES, g.ni, gl.UNSIGNED_INT, 4 * g.i0, t - s);
          if (g.mirrored) gl.frontFace(front);
          fTris += (g.ni / 3) * (t - s), fDraws++;
        });
      }
      if (inInst) gl.bindVertexArray(vao);
      if (!hiddenDs.length) return;
      const v = new Float32Array(108 * hiddenDs.length); // each hidden batch's box: 12 triangles
      hiddenDs.forEach((d, i) => {
        const b = d.b.box, c = (k: number) => [k & 1 ? b[3]! : b[0]!, k & 2 ? b[4]! : b[1]!, k & 4 ? b[5]! : b[2]!];
        const F = [[0, 1, 3, 2], [4, 6, 7, 5], [0, 4, 5, 1], [2, 3, 7, 6], [0, 2, 6, 4], [1, 5, 7, 3]];
        let o = 108 * i;
        for (const f of F) for (const k of [f[0]!, f[1]!, f[2]!, f[0]!, f[2]!, f[3]!]) v.set(c(k), o), o += 3;
      });
      gl.bindVertexArray(probeVao);
      gl.bindBuffer(gl.ARRAY_BUFFER, probeBuf);
      gl.bufferData(gl.ARRAY_BUFFER, v, gl.DYNAMIC_DRAW);
      gl.colorMask(false, false, false, false), gl.depthMask(false);
      hiddenDs.forEach((d, i) => {
        const o = d.o!;
        if (o.q) return;
        o.q = gl.createQuery(), o.box = true, o.qAt = key, occAsked++;
        gl.beginQuery(gl.ANY_SAMPLES_PASSED_CONSERVATIVE, o.q);
        gl.drawArrays(gl.TRIANGLES, 36 * i, 36), fDraws++;
        gl.endQuery(gl.ANY_SAMPLES_PASSED_CONSERVATIVE);
      });
      gl.colorMask(true, true, true, true), gl.depthMask(true);
      gl.bindVertexArray(vao);
    };
    // ---- end 3D2 (step 3) ----
    const instFaces = (mdl: number) => { // (LNK3D (D): one model's, in its buffers)
      let any = false;
      for (const g of groups) {
        if (g.model !== mdl || !g.ni) continue;
        let a = -1, z = -1; // a run of instances [a, z)
        const flush = () => {
          if (z <= a) return;
          if (!any) gl.bindVertexArray(instVao), any = true;
          instAttrs(a, true);
          if (g.mirrored) gl.frontFace(front === gl.CCW ? gl.CW : gl.CCW);
          gl.drawElementsInstanced(gl.TRIANGLES, g.ni, gl.UNSIGNED_INT, 4 * g.i0, z - a);
          if (g.mirrored) gl.frontFace(front);
          fTris += (g.ni / 3) * (z - a), fDraws++;
        };
        for (const d of g.dbs) {
          if (d.k1 <= d.k0) continue;
          const rr = d.runs ?? [d.k0, d.k1]; // VIS1: the runs that may be seen
          // (a gap of hidden copies is drawn when it costs less than a draw call: about 2,000 triangles)
          const gapMax = Math.floor(2000 / Math.max(1, g.ni / 3));
          for (let r = 0; r < rr.length; r += 2) {
            const s = g.first + d.b.first + rr[r]!, t = g.first + d.b.first + rr[r + 1]!;
            if (s >= z && s - z <= gapMax && z > a) z = t;
            else flush(), a = s, z = t;
          }
        }
        flush();
      }
      if (any) gl.bindVertexArray(vao);
    };
    const merged = (mdl: number) => { // runs of the merged index list, neighbouring batches joined (LNK3D (D): one model's)
      let a = -1, z = -1;
      // VIS1: the ranges gathered, then one multi-draw (WEBGL_multi_draw) or a draw each
      const offs: number[] = [], cnts: number[] = [], near: number[] = [];
      let dn = 0;
      const flush = () => { if (z > a) offs.push(4 * a), cnts.push(z - a), near.push(dn), fTris += (z - a) / 3; };
      const add = (s: number, t: number, dd: number) => { if (s === z) z = t, dn = Math.min(dn, dd); else flush(), a = s, z = t, dn = dd; };
      for (const d of dbsOf(mdl)) { // (VIS1 P0-c: this model's batches, not every batch once per model)
        if (d.g || d.k1 <= d.k0) continue;
        if (d.runs) { for (let r = 0; r < d.runs.length; r += 2) add(d.f0 + 3 * d.b.cum[d.runs[r]!]!, d.f0 + 3 * d.b.cum[d.runs[r + 1]!]!, d.dist ?? 0); continue; }
        add(d.f0 + 3 * d.b.cum[d.k0]!, d.f0 + 3 * d.b.cum[d.k1]!, d.dist ?? 0);
      }
      flush();
      // VIS1 (option 6): the ranges nearest first, so the depth test rejects what is behind before shading it
      if (multiDraw && offs.length > 1 && stats.frontToBack === true) {
        const ord = [...offs.keys()].sort((i, j) => near[i]! - near[j]!);
        const o2 = new Int32Array(ord.length), c2 = new Int32Array(ord.length);
        ord.forEach((k, i) => { o2[i] = offs[k]!, c2[i] = cnts[k]!; });
        multiDraw.multiDrawElementsWEBGL(gl.TRIANGLES, c2, 0, gl.UNSIGNED_INT, o2, 0, ord.length), fDraws++;
      } else if (multiDraw && offs.length > 1) multiDraw.multiDrawElementsWEBGL(gl.TRIANGLES, Int32Array.from(cnts), 0, gl.UNSIGNED_INT, Int32Array.from(offs), 0, offs.length), fDraws++;
      else for (let i = 0; i < offs.length; i++) gl.drawElements(gl.TRIANGLES, cnts[i]!, gl.UNSIGNED_INT, offs[i]!), fDraws++;
    };
    const hostGl = slots[0]?.gl ?? g0;
    const faces = (push: number) => {
      gl.polygonOffset(1.5 + push, 2 + 2.4 * push); // (1.5, 2), and (4, 8) for the inward pass, as before
      useModel(hostGl), gl.bindVertexArray(vao); // LNK3D (D): the open model's buffers
      if (occActive) occFaces(); // 3D2 (step 3)
      else merged(0), instFaces(0);
      prxFaces(); // 3D2 (none with a section box)
      gl.polygonOffset(2.5 + push, 16 + 2.4 * push);
      // LNK3D (D): each link's loaded detail (VIS1: the nearest link first)
      const near = new Map<number, number>();
      if (stats.frontToBack === true) for (const { d } of seen) if (d.k1 > d.k0) near.set(d.model, Math.min(near.get(d.model) ?? Infinity, d.dist ?? Infinity));
      for (const x of [...slots].sort((p, q) => (near.get(p.k) ?? Infinity) - (near.get(q.k) ?? Infinity))) if (x.k > 0 && x.gl) { useModel(x.gl), setModel(x.k), gl.bindVertexArray(vao); merged(x.k); instFaces(x.k); }
      if (!slice || fill?.t === Infinity) for (const c of linkCut) { const n = drawLinkCells(c.x, c.cells, c.mdl, c.colour); fTris += n, fDraws++, linkPrxTris += n; } // LNK3D (C)
      useModel(hostGl), setModel(0), gl.bindVertexArray(vao);
    };
    if ((stats.cullBack ?? opt().cull_back) && !box && !idMode) gl.enable(gl.CULL_FACE), gl.cullFace(gl.BACK), gl.frontFace(gl.CCW); // VIS1: the setting cull_back (option 4; a bench hook overrides)
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
    gl.disable(gl.CULL_FACE); // (VIS1: the bench hook's)
    if (idMode) { // VIS1: faces only
      gl.uniform1i(uId, 0), gl.uniform1i(uClip, 0), gl.bindVertexArray(null); stats.frameTris = fTris, stats.frameDraws = fDraws;
      ftime.end({ at: tNow, kind: "i", js: performance.now() - tNow, draws: fDraws, tris: fTris, culled, batches: dbs.length, proxied: 0, outlines: 0, linkPrxTris: 0, budget: Infinity, gpu: null, fence: null });
      return;
    }
    // Edges: their VAO (with the face normals for the back-edge test), a constant colour.
    gl.uniform1f(uAmbient, -1);
    gl.uniform1i(uElCol, 0); // LNK3D: not the element's colour (its drawn flag and model still apply)
    gl.bindVertexArray(edgeVao);
    gl.vertexAttrib3f(1, EDGE_SRGB[0]! / 255, EDGE_SRGB[1]! / 255, EDGE_SRGB[2]! / 255);
    gl.depthMask(false);
    // A plan: edges 2 mm ahead, so the outline of a cut drawn where it stands on something stays.
    if (plan) gl.uniform1f(uZbias, (2 * 2) / (2 * (4 * (bounds?.radius ?? distance) + 1000) - 1));
    const edgesOn0 = pressed("edges", true) && !stats.noEdges && opt().edge_mode !== "none"; // (VIS1: edge_mode none) Q2: ribbon.rs:695 Edges (opts.edges): no edge drawn when off (3D2: a bench hook too)
    // VIS1: in 3D, edges found on screen (gl/screenedges.ts) instead of the line pass, when the depth can be copied
    const scrEdges = edgesOn0 && !idMode && screenEdgesNow();
    if (tgt) tgt.colourOnly(true); // VIS1: lines, outlines, the box and the ghost: colour only
    const edgesOn = edgesOn0 && !scrEdges;
    let instSegs = 0, segs = 0;
    stats.linkProxies = [...linkPrx.values()].map((x) => ({ name: x.name, cells: x.p.cells.length, tris: x.p.tris.reduce((a, b) => a + b, 0), bytes: x.bytes,
      drawnCells: x.drawnCells, drawnTris: x.drawnTris })); // LNK3D (C)
    const instEdges = (mdl: number) => { // 3D1: the instanced groups' segments, the batches drawn (LNK3D (D): one model's)
      let any = false;
      for (const g of groups) {
        if (g.model !== mdl || !g.ns) continue;
        let a = -1, z = -1;
        const flush = () => {
          if (z <= a) return;
          if (!any) gl.bindVertexArray(instEdgeVao), any = true;
          instAttrs(a, false);
          gl.drawArraysInstanced(gl.LINES, 2 * g.s0, 2 * g.ns, z - a);
          instSegs += g.ns * (z - a), fDraws++;
        };
        for (const d of g.dbs) {
          if (d.k1 <= d.k0 || d.noEdge) continue; // LNK3D (F)
          const s = g.first + d.b.first + d.k0, t = g.first + d.b.first + d.k1;
          if (s === z) z = t;
          else flush(), a = s, z = t;
        }
        flush();
      }
      if (any) gl.bindVertexArray(edgeVao);
    };
    const mergedEdges = (mdl: number) => {
      let a = -1, z = -1;
      const flush = () => { if (z > a) gl.drawElements(gl.LINES, z - a, gl.UNSIGNED_INT, 4 * a), fDraws++, segs += (z - a) / 2; };
      for (const d of dbsOf(mdl)) {
        if (d.g || d.k1 <= d.k0 || d.noEdge) continue; // LNK3D (F)
        const s = d.e0 + 2 * d.b.ecum[d.k0]!, t = d.e0 + 2 * d.b.ecum[d.k1]!;
        if (s === z) z = t;
        else flush(), a = s, z = t;
      }
      flush();
    };
    if (slice && fillEdges) for (const { d } of seen) d.k0 = 0, d.k1 = d.b.n; // every edge once more over the filled picture
    const prxEdges = () => { // 3D2: the proxies' crease edges
      if (!prxCells.length || !(drawPrx || (slice && fillEdges))) return;
      gl.bindVertexArray(prxEdgeVao);
      for (const [a, n] of prxRuns(true)) gl.drawArrays(gl.LINES, 2 * a, 2 * n), fDraws++, segs += n;
      gl.bindVertexArray(edgeVao);
    };
    if (edgesOn) {
      mergedEdges(0);
      instEdges(0);
      if (stats.proxyEdges) prxEdges(); // LNK3D (F): never for proxies (a bench hook keeps 3D2's)
      // linked models: their edges in the reference edge colour
      gl.vertexAttrib3f(1, REFERENCE_EDGE_SRGB[0] / 255, REFERENCE_EDGE_SRGB[1] / 255, REFERENCE_EDGE_SRGB[2] / 255);
      for (const x of slots) if (x.k > 0 && x.gl) { useModel(x.gl), setModel(x.k), gl.bindVertexArray(edgeVao); mergedEdges(x.k); instEdges(x.k); } // LNK3D (D)
      useModel(hostGl), setModel(0), gl.bindVertexArray(edgeVao);
    }
    stats.screenEdges = scrEdges ? "on" : tgt ? "off" : "no target";
    stats.q2 = { edges: edgesOn0, ortho: ortho3d(), edgeSegs: edgesOn ? segs + instSegs : 0, edgeMode: !edgesOn0 ? "none" : scrEdges ? "screen" : "lines" };
    if (unfinished.length) drawOutlines(unfinished), fDraws++;
    stats.frameTris = fTris, stats.frameDraws = fDraws; // 3D1
    stats.moving.tris = fTris;
    if (occActive) occPending = occPending || occAsked > 0, stats.occ = { asked: occAsked, hidden: hiddenDs.length }; // 3D2 (step 3)
    if (stats.proxies) stats.proxies.drawnCells = drawPrx ? prxCells.length : 0, stats.proxies.drawnTris = prxTris, stats.proxies.standIns = nStand, stats.proxies.on = prxOn; // 3D2
    // 3D1: the triangles a ms (frame to frame, the frames that had a budget), the fill's next step
    // (3D2: the last frame budgeted too: a whole frame's triangles over the time to the next frame, its GPU
    // work not waited for, made a rate many times too high and a moving frame that drew everything)
    if ((movingNow || slice) && lastBudgeted && lastFrameAt && tNow - lastFrameAt < 250 && lastFrameTris > 0) {
      const r = lastFrameTris / Math.max(1, tNow - lastFrameAt);
      trisPerMs = trisPerMs ? 0.5 * trisPerMs + 0.5 * r : r;
    }
    lastFrameAt = tNow, lastFrameTris = fTris, lastBudgeted = movingNow || slice;
    if (movingNow) stillDue = first ? tNow : tNow + Math.max(STILL_MS, motionWin());
    if (feedbackPending && movingNow) wholeNext = true, wholeAt = tNow + 50; // the complete picture 50 ms later (no gesture goes on): the feedback is on screen first
    feedbackPending = false; // 3D2: after a first frame, the fill at once (VIS1: after the motion's window)
    if (slice) {
      if (fillEdges) fillEdges = false;
      else if (cutT === 0) fill = null, fillEdges = !scrEdges; // (VIS1: edges found on screen need no last pass of lines)
      else fill = { t: cutT, id: fill?.id ?? 0 };
    }
    // 3D2: the picture complete (every batch drawn): a whole frame, the end of a fill, a moving frame that drew all
    if (stats.fullFrameMs === null && (slice ? fill === null && !fillEdges && cutT === 0 : cutT === 0 && !unfinished.length && !(stats.moving?.proxied ?? 0)))
      stats.fullFrameMs = tNow;
    lastComplete = !movingNow && !(stats.moving?.outlines ?? 0) && (slice ? fill === null && !fillEdges && cutT === 0 : cutT === 0) && !box && !plan; // 3D2 (step 4)
    lastReplaced = replaced;
    lastLinkRep = linkRep; // LNK3D (C)
    if (first) stats.firstFrame = { moving: movingNow, tris: fTris, draws: fDraws, budget, t: cutT, ms: performance.now() - tNow }, stats.frameLog = [];
    if (stats.frameLog && stats.frameLog.length < 80) stats.frameLog.push(`${Math.round(tNow)} ${movingNow ? "m" : slice ? "s" : "w"} t${cutT.toPrecision(2)} ${fTris} f${fill ? fill.t : "-"} e${fillEdges ? 1 : 0}`);
    gl.uniform1f(uZbias, 0);
    gl.depthMask(true);
    gl.bindVertexArray(null);
    gl.uniform1i(uClip, 0);
    gl.uniform1i(uElCol, 1), setModel(0);
    if (box && !plan) drawBox(box);
    stats.boxHandles = box && !plan ? boxHandles(box) : []; // ISS71: drawn by drawOverlay
    if (!slice) drawGhost(); // (a slice adds to the kept picture: the ghost is drawn once)
    // VIS1 (C-iv): the fill's slices complete the picture offscreen: the canvas keeps the moving frame's picture
    // and shows the target when the fill is done, or every PRESENT_MS meanwhile (a long fill still shows progress)
    const holdSlice = !!tgt && slice && (fill !== null || fillEdges) && performance.now() - lastPresentAt < PRESENT_MS && stats.showSlices !== true && opt().refill_offscreen;
    if (tgt && !holdSlice) tgt.present(scrEdges, E0, E1), fDraws++, lastPresented = true, lastPresentAt = performance.now(); // VIS1: onto the canvas, with its edges
    // VIS1: this picture's pyramid, once per key (the masks are made once per pyramid: 40 ms for 83,000 elements), and
    // once more when the picture completes after a pyramid of a partial one (a moving frame's, a slice's: fewer occluders)
    const complete = cutT === 0 && !unfinished.length && (!slice || (fill === null && !fillEdges));
    if (hiz && tgt && !plan && visibleOn() && (hiz.ready?.key !== vkey || (!hiz.ready.complete && complete))) hiz.request(tgt.depth!, tgt.ids!, canvas.width, canvas.height, vkey, mvp, complete);
    // VIS1: a complete picture (nothing left out by the budget, no outlines, no proxies) answers picks from its element attachment
    // (a proxy's pixel answers nothing: the scan decides there)
    idPickKey = tgt && cutT === 0 && !unfinished.length && !(stats.stillPx ?? 0) && (!slice || (fill === null && !fillEdges))
      ? `${key}|${canvas.width}x${canvas.height}|${visGen}` : null;
    drawOverlay();
    stats.frames++;
    const rec: FrameRec = { at: tNow, kind: movingNow ? "m" : slice ? "s" : "w", js: performance.now() - tNow, draws: fDraws, tris: fTris, culled, batches: dbs.length,
      proxied: prxCells.length + linkCut.reduce((n, c) => n + c.cells.length, 0), outlines: unfinished.length, linkPrxTris, budget, gpu: null, fence: null };
    ftime.end(rec); // VIS1: the frame log
    if (!frameFence) frameFence = { s: gl.fenceSync(gl.SYNC_GPU_COMMANDS_COMPLETE, 0)!, t0: tNow, tris: fTris, whole: !movingNow && !slice && cutT === 0, rec }; // VIS1 (P0-a)
    perfFrame(motion, tNow, gl, movingNow && fps0); // GPU1 (VIS1: moving = in motion; capped: "draw everything" kept under 100 ms)
    if (stats.firstFrameMs === null) stats.firstFrameMs = performance.now();
  };

  /** 3D1: the boxes (min xyz, max xyz) as outlines, one instanced draw, depth-tested, in a light grey. */
  const drawOutlines = (bs: ArrayLike<number>[]) => {
    const mat = new Float32Array(12 * bs.length);
    bs.forEach((b, i) => mat.set([b[3]! - b[0]!, 0, 0, 0, b[4]! - b[1]!, 0, 0, 0, b[5]! - b[2]!, b[0]! - ro[0], b[1]! - ro[1], b[2]! - ro[2]], 12 * i)); // (VIS2: relative)
    gl.useProgram(prog);
    gl.uniform3f(uRo, 0, 0, 0);
    gl.bindVertexArray(outlineVao);
    gl.bindBuffer(gl.ARRAY_BUFFER, outlineMat);
    gl.bufferData(gl.ARRAY_BUFFER, mat, gl.DYNAMIC_DRAW);
    gl.uniform1f(uAmbient, -1);
    gl.vertexAttrib3f(1, OUTLINE_SRGB[0] / 255, OUTLINE_SRGB[1] / 255, OUTLINE_SRGB[2] / 255);
    gl.depthMask(false);
    gl.drawArraysInstanced(gl.LINES, 0, 24, bs.length);
    gl.depthMask(true);
    gl.bindVertexArray(null);
    gl.uniform3f(uRo, ro[0], ro[1], ro[2]);
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
    // ---- SLAB1 ---- a polygon slab: its outline so far and the closing edge (the hover back to the first corner)
    if ((t.kind === "slab" && placing.value?.shape === "polygon") || t.kind === "raft") { // CONC1: a raft too
      const P = t.points, seg = (a: [number, number], b: [number, number]) =>
        lines.push(a[0] - m.origin[0], a[1] - m.origin[1], z, b[0] - m.origin[0], b[1] - m.origin[1], z);
      for (let i = 1; i < P.length; i++) seg(P[i - 1]!, P[i]!);
      if (t.hover && P.length >= 2) seg(t.hover.at, P[0]!);
    }
    // ---- end SLAB1 ----
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
    // ---- LNK3D (E): notes on linked elements, in 3D and on the live plan ----
    if (mesh) { refreshNotes(); ctx.setTransform(dpr, 0, 0, dpr, 0, 0); drawNotes(ctx, canvas.getBoundingClientRect()); }
    // ---- VIEWSHEET: the section line being drawn, its look direction (an arrow to the facing) ----
    const sl = stats.section;
    if (mesh && sl.a && sl.hover && sectionNow()) {
      const b = canvas.getBoundingClientRect(), z = plan?.elevation ?? 0;
      const pa = stats.toScreen?.(sl.a[0] ?? 0, sl.a[1] ?? 0, z), pb = stats.toScreen?.(sl.hover[0] ?? 0, sl.hover[1] ?? 0, z);
      if (pa && pb) {
        ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
        const ax = pa[0] - b.left, ay = pa[1] - b.top, bx = pb[0] - b.left, by = pb[1] - b.top;
        ctx.strokeStyle = "#141414";
        ctx.lineWidth = 1.5;
        ctx.setLineDash([10, 4, 2, 4]);
        ctx.beginPath(); ctx.moveTo(ax, ay); ctx.lineTo(bx, by); ctx.stroke();
        ctx.setLineDash([]);
        const len = Math.hypot(bx - ax, by - ay);
        if (len > 1) { // screen right of A -> B (y down): (-dy, dx) / len
          const nx = -(by - ay) / len, ny = (bx - ax) / len, mx = (ax + bx) / 2, my = (ay + by) / 2;
          ctx.beginPath(); ctx.moveTo(mx, my); ctx.lineTo(mx + nx * 18, my + ny * 18); ctx.stroke();
          ctx.beginPath(); ctx.arc(mx + nx * 18, my + ny * 18, 3, 0, 2 * Math.PI); ctx.fill();
        }
      }
    }
    if (sl.clickAt !== null && sl.drawnAt === null && !sl.a) sl.drawnAt = performance.now(); // the frame after the view was made
    // ---- SETOUT: the base point, the survey point and north, in 3D and in a plan ----
    if (mesh) {
      const b = canvas.getBoundingClientRect();
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
      stats.setout = drawSettingOut(ctx, settingOut.value, (x, y, z) => {
        const p = stats.toScreen?.(x, y, z);
        return p ? [p[0] - b.left, p[1] - b.top] : null;
      });
      stats.setoutAt = performance.now();
      // ---- WAVE1B (A3): the grid axes in 3D, at the active level's elevation (Rust: on paper only) ----
      if (!plan) gridAxesLayer.draw(ctx, (x, y) => { const p = stats.toScreen?.(x, y, gridElev); return p ? [p[0] - b.left, p[1] - b.top] : null; });
      // ---- WAVE1B (B2): the selected opening's box, in the selection colour ----
      stats.openingDrawn = false;
      const so = selectedOpening.value;
      const okey = so ? `${so.host}/${so.id}/${state.value.rev}` : "";
      if (okey !== openingKey) { openingKey = okey; openingBox = so ? openingsOf(so.host).find((o) => o.id === so.id) : undefined; }
      const ob = openingBox;
      if (ob) {
        const q = ob.corners.map((c) => { const p = stats.toScreen?.(c[0], c[1], c[2]); return p ? [p[0] - b.left, p[1] - b.top] as [number, number] : null; });
        if (q.every((x) => x)) {
          ctx.save();
          ctx.strokeStyle = SELECTION_CSS;
          ctx.lineWidth = 2.5;
          ctx.beginPath();
          for (const [i, j] of [[0, 1], [2, 3], [4, 5], [6, 7], [0, 2], [1, 3], [4, 6], [5, 7], [0, 4], [1, 5], [2, 6], [3, 7]] as const) {
            ctx.moveTo(q[i]![0], q[i]![1]); ctx.lineTo(q[j]![0], q[j]![1]);
          }
          ctx.stroke();
          ctx.restore();
          stats.openingDrawn = true;
        }
      }
      // ---- WAVE1B: reference planes: the outline in 3D, the cut line in a plan or a section ----
      const sc = plan as (ViewClip & { origin?: number[]; dir?: number[] }) | null;
      const cut = !sc ? null : sc.type === "plan" ? { at: [0, 0, sc.cut ?? (sc.elevation ?? 0) + 1200] as [number, number, number], n: [0, 0, 1] as [number, number, number] }
        : sc.origin && sc.dir ? { at: [sc.origin[0] ?? 0, sc.origin[1] ?? 0, 0] as [number, number, number], n: [sc.dir[0] ?? 0, sc.dir[1] ?? 0, 0] as [number, number, number] } : null;
      stats.refplanes = drawRefPlanes(ctx, refPlanes.value, (x, y, z) => {
        const p = stats.toScreen?.(x, y, z);
        return p ? [p[0] - b.left, p[1] - b.top] : null;
      }, sc ? cut : null);
    }
    // ---- end SETOUT ----
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
  /** SETOUT: the setting-out answer last drawn. */
  let seenSetOut: unknown = null;
  let seenRefPlanes: unknown = null; // WAVE1B
  let gridKey3 = "", gridHidden = false, gridElev = 0; // WAVE1B (A3): grids in 3D
  let seenOpening: unknown = null; // WAVE1B (B2)
  let openingKey = "", openingBox: ReturnType<typeof openingsOf>[number] | undefined; // WAVE1B (B2): the box drawn, per rev
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
    const gridAxis = placing.value?.kind === "grid_axis"; // WAVE1B (A2): Shift locks the axis along x or y (it snaps always)
    const args: ToolPointer = { ray: rayAt(x, y), ray_tol: rayAt(x + SNAP_PX, y), free: gridAxis ? false : mods.shiftKey, ortho: gridAxis ? mods.shiftKey || mods.ctrlKey || mods.metaKey : mods.ctrlKey || mods.metaKey };
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
    overlayDirty = true; // MEAS1: what it draws is on the overlay
  };
  // ---- VIEWSHEET ---- the section tool (action draw_section, ADR 0076): in a plan, two clicks draw the section
  // line; the view looks to the line's right (A to B), snapped to the four facings the core's sections have; its cut
  // goes through the line's middle; depth 0 (all, Properties: Depth). One view_add, one undo step; the plan stays
  // shown with the new mark; the tool stops (Rust's tools stop after one view). Esc: the first point, then the tool.
  const sectionNow = () => plan?.type === "plan" && mesh !== null && pressed("draw_section", false);
  const worldAt = (cx: number, cy: number): number[] => {
    const r = canvas.getBoundingClientRect();
    const ray = rayAt(cx - r.left, cy - r.top), o = mesh?.origin ?? [0, 0, 0];
    return [Math.round((ray[0] ?? 0) + (o[0] ?? 0)), Math.round((ray[1] ?? 0) + (o[1] ?? 0))];
  };
  const sectionClick = (cx: number, cy: number) => {
    const p = worldAt(cx, cy), sec = stats.section;
    if (!sec.a) { sec.a = p; sec.hover = p; dirty = true; return; }
    const a = sec.a, dx = (p[0] ?? 0) - (a[0] ?? 0), dy = (p[1] ?? 0) - (a[1] ?? 0);
    if (Math.hypot(dx, dy) < 1) return; // the same point: wait for the end
    const nx = dy, ny = -dx; // the right of A -> B
    const facing = Math.abs(nx) >= Math.abs(ny) ? (nx > 0 ? "east" : "west") : (ny > 0 ? "north" : "south");
    sec.clickAt = performance.now();
    sec.drawnAt = null;
    const r = viewAdd({ kind: "section", facing, origin: [Math.round(((a[0] ?? 0) + (p[0] ?? 0)) / 2), Math.round(((a[1] ?? 0) + (p[1] ?? 0)) / 2)] });
    sec.error = r.error;
    if (r.error) console.warn("gl viewport: section:", r.error);
    sec.added = r.id;
    sec.a = sec.hover = null;
    runAction("draw_section"); // the tool stops
    dirty = true;
  };
  const sectionCancel = (): boolean => {
    if (!sectionNow()) { if (stats.section.a) { stats.section.a = stats.section.hover = null; dirty = true; } return false; }
    if (stats.section.a) stats.section.a = stats.section.hover = null;
    else runAction("draw_section");
    dirty = true;
    return true;
  };
  // ---- end VIEWSHEET ----
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
  const pickHit = (x: number, y: number, readout = false): { el: number; point: V3 } | null => {
    const t0 = performance.now();
    // VIS1: the status bar's readout (each frame the pointer moved, the camera moving too): the last frame's element
    // under the pixel, whatever its camera, tested alone; no scan of every element (it took 0.7 s a session of orbits)
    if (readout && tgt && lastPresented) {
      const X = (x * canvas.width) / Math.max(1, canvas.clientWidth), Y = canvas.height - 1 - (y * canvas.height) / Math.max(1, canvas.clientHeight);
      const a = tgt.elementAt(X, Y);
      const h = a && a.el < (mesh?.ids.length ?? 0) ? pickScan(x, y, a.el) : null;
      stats.pickMs = performance.now() - t0, stats.pickFrom = "readout";
      return h;
    }
    // VIS1: the element under the pixel from the last presented frame's element attachment (one pixel read), when
    // that frame was complete at this camera: then the ray is tested against that element alone (its point); the
    // background answers none at once. Else (a moving frame, a proxy, another camera) the scan of every element.
    let h: { el: number; point: V3 } | null | undefined;
    if (tgt && idPickKey !== null && idPickKey === `${camKey()}|${canvas.width}x${canvas.height}|${visGen}` && stats.idPick !== false) {
      const X = (x * canvas.width) / Math.max(1, canvas.clientWidth), Y = canvas.height - 1 - (y * canvas.height) / Math.max(1, canvas.clientHeight);
      const a = tgt.elementAt(X, Y);
      if (a === null && tgt.isBackground(X, Y)) h = null, stats.pickFrom = "id-background";
      else if (a && a.el < (mesh?.ids.length ?? 0)) { h = pickScan(x, y, a.el) ?? undefined; if (h) stats.pickFrom = "id"; }
    }
    if (h === undefined) h = pickScan(x, y), stats.pickFrom = "scan";
    stats.pickMs = performance.now() - t0; // 3D2 (step 4): the page's ray scan alone (VIS1: or the id read)
    return h;
  };
  /** SIZE2: element el's triangles for a pick: from its model's heavy arrays, or (a link's, dropped) read back
   *  from its GPU buffers (getBufferSubData of its vertices and its face or shape indices), kept in a small
   *  cache; null when its link's detail is released (that link is asked back: the next pick finds it). */
  const trisOf = (m: Bmsh, el: number): { P: Float32Array; I: Uint32Array; k0: number; k1: number } | null => {
    const mdl = m.model?.[el] ?? 0, x = slots[mdl], part = partOf(mdl), ni = m.ni[el] ?? 0;
    if (part) { const k0 = (m.i0[el] ?? 0) - (x?.i0 ?? 0); return { P: part.pos, I: part.idx, k0, k1: k0 + ni }; }
    if (!x) return null;
    if (!x.gl || !x.listsOnGpu) { wanted.add(mdl); return null; }
    const sh = m.shape?.[el] ?? 0, key = sh ? `s${sh}` : `e${el}`;
    const hit = pickCache.get(key);
    if (hit) { pickCache.delete(key), pickCache.set(key, hit); return { P: hit.pos, I: hit.idx, k0: 0, k1: hit.idx.length }; }
    const lv = (m.v0[el] ?? 0) - x.v0, nv = m.nv[el] ?? 0, at = sh ? shapeI0[sh - 1] ?? -1 : elFace[el] ?? -1;
    if (at < 0) return null;
    if (gpuR?.tris && gpuOwned.has(x.gl)) { const r = gpuR.tris(mdl, key, lv, nv, at, ni, !!sh); return r ? { P: r.pos, I: r.idx, k0: 0, k1: ni } : null; } // GPU2: WebGPU holds its buffers
    const pos = new Float32Array(3 * nv), idx = new Uint32Array(ni);
    gl.bindBuffer(gl.COPY_READ_BUFFER, x.gl.pos);
    gl.getBufferSubData(gl.COPY_READ_BUFFER, 12 * lv, pos);
    gl.bindBuffer(gl.COPY_READ_BUFFER, sh ? x.gl.shapeIdx : x.gl.idx);
    gl.getBufferSubData(gl.COPY_READ_BUFFER, 4 * at, idx);
    gl.bindBuffer(gl.COPY_READ_BUFFER, null);
    for (let i = 0; i < ni; i++) idx[i] = idx[i]! - lv;
    stats.gpuPickReads = (stats.gpuPickReads ?? 0) + 1;
    pickCache.set(key, { pos, idx }), pickCacheBytes += pos.byteLength + idx.byteLength;
    for (const [k, v] of pickCache) { if (pickCacheBytes <= PICK_CACHE_BYTES) break; pickCache.delete(k), pickCacheBytes -= v.pos.byteLength + v.idx.byteLength; }
    return { P: pos, I: idx, k0: 0, k1: ni };
  };
  const pickScan = (x: number, y: number, only = -1): { el: number; point: V3 } | null => {
    const m = mesh;
    if (!m) return null;
    const r = rayAt(x, y);
    const e: V3 = [r[0] ?? 0, r[1] ?? 0, r[2] ?? 0], dir: V3 = [r[3] ?? 0, r[4] ?? 0, r[5] ?? 0];
    let best = Infinity, hit = -1;
    let P = m.pos, I = m.idx;
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
    for (let el = only >= 0 ? only : 0; el < (only >= 0 ? only + 1 : m.ids.length); el++) { // (VIS1: one element, from the id)
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
      const src = trisOf(m, el); // SIZE2: its model's arrays, or its triangles read back from the GPU
      if (!src) continue;
      P = src.P, I = src.I;
      const k0 = src.k0, k1 = src.k1;
      // 3D1: a placed shape: the ray in the shape's own coordinates (an affine map keeps the ray's parameter)
      let eo = e, dl = dir;
      const lk = m.model?.[el] ?? 0;
      if (lk > 0) { const r = xfRayToLocal(modelXf[lk] ?? IDENTITY_XF, e, dir); eo = r.e, dl = r.d; } // LNK3D: the link's own frame
      if (m.shape?.[el]) {
        const pl = m.place!.subarray(12 * el, 12 * el + 12);
        const cx: V3 = [pl[0]!, pl[1]!, pl[2]!], cy: V3 = [pl[3]!, pl[4]!, pl[5]!], cz: V3 = [pl[6]!, pl[7]!, pl[8]!];
        const r0 = cross(cy, cz), r1 = cross(cz, cx), r2 = cross(cx, cy), dt = dot(cx, r0);
        if (Math.abs(dt) < 1e-30) continue;
        const toLocal = (v: V3): V3 => [dot(r0, v) / dt, dot(r1, v) / dt, dot(r2, v) / dt];
        eo = toLocal(sub(eo, [pl[9]!, pl[10]!, pl[11]!])), dl = toLocal(dl);
      }
      for (let k = k0; k + 2 < k1; k += 3) {
        // Möller–Trumbore, two-sided.
        const a = vtx(I[k] ?? 0), b = vtx(I[k + 1] ?? 0), c = vtx(I[k + 2] ?? 0);
        const e1 = sub(b, a), e2 = sub(c, a), p = cross(dl, e2), det = dot(e1, p);
        if (Math.abs(det) < 1e-12) continue;
        const inv = 1 / det, tv = sub(eo, a), uu = dot(tv, p) * inv;
        if (uu < 0 || uu > 1) continue;
        const q = cross(tv, e1), vv = dot(dl, q) * inv;
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
    if (plan && e.button === 0 && !e.shiftKey && !placingNow() && !annotatingNow() && !measuringNow() && !sectionNow()) {
      const r = canvas.getBoundingClientRect();
      const id = annotationAt(e.clientX - r.left, e.clientY - r.top);
      if (id) drag.annot = id;
    }
  });
  canvas.addEventListener("pointermove", (e) => {
    if (!drag) {
      if (placingNow() || annotatingNow() || measuringNow()) pendingMove = e;
      if (sectionNow() && stats.section.a) { stats.section.hover = worldAt(e.clientX, e.clientY); dirty = true; } // VIEWSHEET
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
    if (sectionNow()) { sectionClick(e.clientX, e.clientY); return; } // VIEWSHEET: the section tool
    if (selAnn) { selAnn = stats.selectedAnnotation = null; dirty = true; } // a click elsewhere: the annotation is let go
    if (d.pan) return;
    if (plan && Math.hypot(e.clientX - d.x0, e.clientY - d.y0) > CLICK_PX) return;
    const r = canvas.getBoundingClientRect();
    { // LNK3D (E): a click on a note's label selects it (and its element, when it has one)
      const ni = noteAt(e.clientX - r.left, e.clientY - r.top);
      if (ni !== null || noteSel !== null) { noteSel = ni; stats.noteSelected = ni; dirty = true; }
      const nt = ni !== null ? notes[ni] : undefined;
      if (nt) { if (!nt.n.orphan) { const err = selectLinked(nt.n.link, nt.n.element); if (err) console.warn("gl viewport: note:", err); } return; }
    }
    const k = pick(e.clientX - r.left, e.clientY - r.top);
    stats.picks++;
    const id = k >= 0 ? mesh?.ids[k] : undefined;
    const link = k >= 0 ? linkOf(k) : null;
    // ---- WAVE1B (B2): a click on an opening (its reveal, or through it) selects it: its host is the
    // element selected, Properties shows the opening (bimble.ts selectedOpening) ----
    const op = link === null && state.value.editable !== false ? openingAt(rayAt(e.clientX - r.left, e.clientY - r.top)) : null;
    selectedOpening.value = op;
    const pickId = op ? op.host : id;
    const err = pickId && link !== null && !op ? selectLinked(link, pickId) : select(pickId ? [pickId] : []);
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
    if (sectionCancel()) return true; // VIEWSHEET
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
  // ---- SLAB1 ---- Enter closes the polygon slab being clicked (lib.rs:2772 close_polygon)
  viewportKeys.enterTool = () => {
    if (!placingNow() || !((placing.value?.kind === "slab" && placing.value?.shape === "polygon") || placing.value?.kind === "raft")) return false; // CONC1: a raft too
    stats.toolCalls++;
    const { state: t, error } = toolCall("close");
    stats.toolError = error;
    if (error) console.warn("gl viewport: tool close:", error);
    if (t?.placed) stats.placed = t.placed;
    showTool(t);
    return true;
  };
  // ---- end SLAB1 ----
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
    const h = pickHit(x, y, true); // VIS1: from the element attachment
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
    // FIRST1: the frames drawn in the key too: a still pointer's point changes when the view turns,
    // fits or the model is drawn under it (before its first frame the pick misses: it read "nothing
    // under the pointer" for good before).
    const key = `${ptrAt[0]},${ptrAt[1]}|${stats.frames}`;
    if (key === ptrDone) return;
    ptrDone = key;
    stats.pointerQueries++;
    const w0 = pointerWorld(ptrAt[0], ptrAt[1]);
    // A point that is not three finite numbers (a pick on a frame whose mesh is being replaced) is
    // no point: the core would refuse it (400), and there is nothing to read out.
    const w = w0 && w0.every((v) => Number.isFinite(v)) ? w0 : null;
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

  // ---- 3D1 (ccore/tools/frames.mjs): one frame drawn now, finished (a 1x1 readPixels waits for the pixels) ----
  const px1 = new Uint8Array(4);
  // ---- VIS1: the ground truth's helpers: each element's triangles (its shape's for a placed copy), the last
  // idCount's visible elements, and the elements the last frame drew (its batches' items [k0, k1) not left out) ----
  let lastIdPx: { px: Uint8Array; w: number; h: number } | null = null; // VIS2 (diagnosis)
  let visibleSet: Uint8Array | null = null, elTrisOf: { dbs: DrawBatch[]; t: Float64Array } | null = null;
  const elTris = (): Float64Array => {
    if (elTrisOf && elTrisOf.dbs === dbs) return elTrisOf.t;
    const m = mesh, t = new Float64Array(m?.ids.length ?? 0);
    if (m) for (const d of dbs) for (let i = 0; i < d.b.n; i++) { const e = d.items[i]!; t[e] = d.g ? d.g.ni / 3 : (m.ni[e] ?? 0) / 3; }
    elTrisOf = { dbs, t };
    return t;
  };
  const drawnSet = (): Uint8Array => {
    const out = new Uint8Array(mesh?.ids.length ?? 0);
    for (const d of dbs) for (let i = d.k0; i < d.k1; i++) { const e = d.items[i]!; if (!visSkip(d, i)) out[e] = 1; }
    return out;
  };
  // ---- end VIS1 ----
  stats.bench = {
    frame: (dyaw: number, moving: boolean) => {
      void moving;
      const t0 = performance.now();
      yaw += dyaw;
      if (!moving) fill = null, fillEdges = false, lastCamChangeAt = -Infinity; // a still frame: the whole picture (VIS1: not in motion)
      draw();
      const t1 = performance.now();
      if (!gpuR) gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px1); // GPU2: under WebGPU the caller waits for its queue (WebGL2 drew nothing)
      const t2 = performance.now();
      (stats as unknown as { split?: number[] }).split = [t1 - t0, t2 - t1];
      return t2 - t0;
    },
    /** The camera's distance times k (a close orbit: 0.2). */
    zoom: (k: number) => { turn = null; distance *= k; },
    /** LNK3D: the camera looking at world point `t` (mm) from `d` mm, home angles; the next frame draws it. */
    look: (t: number[], d: number) => {
      turn = null;
      const o = mesh?.origin ?? [0, 0, 0];
      target = [t[0]! - o[0], t[1]! - o[1], t[2]! - o[2]], distance = d, yaw = HOME_YAW + projectNorth(), pitch = HOME_PITCH;
      dirty = true;
    },
    /** LNK3D (B): what a visibility change costs for n elements of v vertices each, the two ways weighed:
     *  the state texels (n x 4 bytes, texSubImage2D of their rows) against a flag per vertex (n x v x 4 bytes,
     *  bufferSubData), each with a 1x1 readPixels after it (the GPU's copy done); ms, best of 5. */
    stateCost: (n: number, v: number) => {
      const tex = gl.createTexture(), buf = gl.createBuffer(), rows = Math.ceil(n / STATE_TEX_W);
      const texels = new Uint8Array(4 * rows * STATE_TEX_W), flags = new Uint32Array(n * v);
      gl.activeTexture(gl.TEXTURE3);
      gl.bindTexture(gl.TEXTURE_2D, tex);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, STATE_TEX_W, rows, 0, gl.RGBA, gl.UNSIGNED_BYTE, texels);
      gl.bindBuffer(gl.ARRAY_BUFFER, buf);
      gl.bufferData(gl.ARRAY_BUFFER, flags, gl.DYNAMIC_DRAW);
      const best = (f: () => void) => { let b = Infinity; for (let i = 0; i < 5; i++) { const t = performance.now(); f(); gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px1); b = Math.min(b, performance.now() - t); } return b; };
      const texMs = best(() => { for (let e = 0; e < n; e++) texels[4 * e + 3] = texels[4 * e + 3]! ^ 1; gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, STATE_TEX_W, rows, gl.RGBA, gl.UNSIGNED_BYTE, texels); });
      const bufMs = best(() => { for (let i = 0; i < flags.length; i++) flags[i] = flags[i]! ^ 1; gl.bufferSubData(gl.ARRAY_BUFFER, 0, flags); });
      gl.deleteTexture(tex), gl.deleteBuffer(buf);
      gl.activeTexture(gl.TEXTURE0);
      return { texMs, texBytes: texels.byteLength, bufMs, bufBytes: flags.byteLength };
    },
    /** VIS1: the ground truth of what is visible from the current camera: every shown element's faces drawn
     *  (no culling but the frustum's, no proxies, no budget, the section box's clip) as its index + 1 into an
     *  offscreen target the canvas's size, read back: the pixels covered, the distinct elements with at least one
     *  pixel and the sum of their triangles. `visibleSet` keeps the elements (a flag each) for `missing`. */
    idCount: () => {
      const w = canvas.width, h = canvas.height;
      if (!idFbo || idFbo.w !== w || idFbo.h !== h) {
        if (idFbo) gl.deleteFramebuffer(idFbo.fb), gl.deleteRenderbuffer(idFbo.col), gl.deleteRenderbuffer(idFbo.dep);
        const fb = gl.createFramebuffer()!, col = gl.createRenderbuffer()!, dep = gl.createRenderbuffer()!;
        gl.bindRenderbuffer(gl.RENDERBUFFER, col), gl.renderbufferStorage(gl.RENDERBUFFER, gl.RGBA8, w, h);
        gl.bindRenderbuffer(gl.RENDERBUFFER, dep), gl.renderbufferStorage(gl.RENDERBUFFER, gl.DEPTH_COMPONENT24, w, h);
        gl.bindFramebuffer(gl.FRAMEBUFFER, fb);
        gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.RENDERBUFFER, col);
        gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_ATTACHMENT, gl.RENDERBUFFER, dep);
        idFbo = { fb, col, dep, w, h };
      }
      const keep = { np: stats.noProxies, ne: stats.noEdges, cb: stats.cullBack, sp: stats.stillPx, fill, fillEdges };
      gl.bindFramebuffer(gl.FRAMEBUFFER, idFbo.fb);
      stats.noProxies = stats.noEdges = true, stats.cullBack = false, stats.stillPx = 0, idMode = true;
      lastCamKey = camKey(), lastCamChangeAt = -Infinity, fill = null, fillEdges = false;
      const t0 = performance.now();
      try { draw(); } finally { idMode = false; }
      const px = new Uint8Array(4 * w * h);
      gl.readPixels(0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, px);
      const ms = performance.now() - t0;
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      const put = <K extends "noProxies" | "noEdges" | "cullBack" | "stillPx">(k: K, v: GlStats[K] | undefined) => { if (v === undefined) delete stats[k]; else stats[k] = v; };
      put("noProxies", keep.np), put("noEdges", keep.ne), put("cullBack", keep.cb), put("stillPx", keep.sp);
      fill = keep.fill, fillEdges = keep.fillEdges;
      const n = mesh?.ids.length ?? 0, vis = new Uint8Array(n), tris = elTris();
      let pixels = 0, elements = 0, t = 0;
      for (let i = 0; i < px.length; i += 4) {
        const k = px[i]! | (px[i + 1]! << 8) | (px[i + 2]! << 16);
        if (!k) continue;
        pixels++;
        const e = k - 1;
        if (e < n && !vis[e]) vis[e] = 1, elements++, t += tris[e]!;
      }
      visibleSet = vis, lastIdPx = { px, w, h };
      dirty = true; // the canvas is drawn again (this pass drew offscreen)
      return { w, h, pixels, elements, tris: t, submitted: stats.frameTris ?? 0, ms };
    },
    /** VIS1 (draw-call cost): every loaded model's merged faces (the open model's and each link's whole face list,
     *  not the instanced copies) drawn as `n` draw calls in all (each model's range cut in equal parts, at least one a
     *  model), `multi`: one multiDrawElementsWEBGL a model with the same parts; the script's ms to submit them and
     *  the ms to the pixels (a 1x1 readPixels), best of `reps`. The canvas is drawn again after. */
    drawCalls: (n: number, reps = 3, multi = false) => {
      const rng = new Map<number, [number, number]>();
      for (const d of dbs) {
        if (d.g || !slots[d.model]?.gl && d.model > 0) continue;
        const a = d.f0, z = d.f0 + 3 * d.b.cum[d.b.n]!, r = rng.get(d.model);
        rng.set(d.model, r ? [Math.min(r[0], a), Math.max(r[1], z)] : [a, z]);
      }
      const T = [...rng.values()].reduce((t, [a, z]) => t + z - a, 0);
      const md = multi ? gl.getExtension("WEBGL_multi_draw") : null;
      if (multi && !md) return null;
      let best = { submit: Infinity, total: Infinity }, draws = 0;
      gl.useProgram(prog), gl.uniform1f(uAmbient, AMBIENT), gl.uniform1i(uElCol, 1), gl.enable(gl.DEPTH_TEST);
      for (let r = 0; r < reps; r++) {
        gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
        const t0 = performance.now();
        draws = 0;
        for (const [k, [a, z]] of rng) {
          const x = k === 0 ? (slots[0]?.gl ?? g0) : slots[k]!.gl!;
          useModel(x), setModel(k), gl.bindVertexArray(vao);
          const parts = Math.max(1, Math.round((n * (z - a)) / Math.max(1, T))), step = 3 * Math.ceil((z - a) / 3 / parts);
          const offs: number[] = [], cnts: number[] = [];
          for (let s0 = a; s0 < z; s0 += step) offs.push(4 * s0), cnts.push(Math.min(step, z - s0));
          if (md) md.multiDrawElementsWEBGL(gl.TRIANGLES, Int32Array.from(cnts), 0, gl.UNSIGNED_INT, Int32Array.from(offs), 0, cnts.length), draws++;
          else for (let i = 0; i < cnts.length; i++) gl.drawElements(gl.TRIANGLES, cnts[i]!, gl.UNSIGNED_INT, offs[i]!), draws++;
        }
        const t1 = performance.now();
        gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px1);
        const t2 = performance.now();
        if (t2 - t0 < best.total) best = { submit: t1 - t0, total: t2 - t0 };
      }
      useModel(slots[0]?.gl ?? g0), setModel(0), gl.bindVertexArray(null);
      dirty = true;
      return { n, draws, tris: T / 3, models: rng.size, ...best };
    },
    /** VIS1: the elements of the last idCount that the last frame drawn did not draw (their faces): the
     *  never-missing assertion (0 expected); with their triangles. */
    missing: () => {
      const vis = visibleSet, drawnEls = drawnSet();
      if (!vis) return null;
      const out: number[] = [];
      for (let e = 0; e < vis.length; e++) if (vis[e] && !drawnEls[e]) out.push(e);
      return { n: out.length, first: out.slice(0, 10).map((e) => mesh?.ids[e] ?? String(e)) };
    },
    /** VIS2 (diagnosis): why each missing element (last idCount, not drawn by the last frame) was left out. */
    whyMissing: (max = 40) => {
      const vis = visibleSet, drawnEls = drawnSet(), m = mesh;
      if (!vis || !m) return null;
      const planes = frustumPlanes(matrices().mvp);
      const out: unknown[] = [];
      const kinds: Record<string, number> = {};
      for (let e = 0; e < vis.length; e++) {
        if (!vis[e] || drawnEls[e]) continue;
        const di = elDb[e] ?? -1, d = dbs[di];
        const mdl = m.model?.[e] ?? 0;
        let why = "no batch";
        const r: Record<string, unknown> = { e, mdl, shape: m.shape?.[e] ?? 0, ni: m.ni[e], box: Array.from(boxes.subarray(6 * e, 6 * e + 6)).map((v) => Math.round(v)) };
        if (d) {
          const i = d.items.indexOf(e);
          r.i = i, r.k0 = d.k0, r.k1 = d.k1, r.n = d.b.n, r.shownB = d.b.shown, r.gl = !!slots[d.model]?.gl, r.inst = !!d.g;
          r.elIn = boxInFrustum(planes, boxes, 6 * e), r.bIn = boxInFrustum(planes, d.b.box, 0);
          r.bbox = Array.from(d.b.box).map((v) => Math.round(v));
          let inside = true;
          for (let a = 0; a < 3; a++) if (boxes[6 * e + a]! < d.b.box[a]! || boxes[6 * e + 3 + a]! > d.b.box[3 + a]!) inside = false;
          r.inBox = inside, r.mask = d.vmask ? d.vmask[i] : null, r.runs = d.runs ? d.runs.length / 2 : null;
          why = !r.gl ? "detail away" : i < d.k0 || i >= d.k1 ? (r.bIn ? "cut (k range)" : "batch outside frustum") : r.runs !== null && visSkip(d, i) ? "occlusion" : "?";
        }
        kinds[why] = (kinds[why] ?? 0) + 1;
        r.why = why;
        if (lastIdPx && out.length < max) { // where the truth drew it (pixels, bottom row first) against its box's projection
          const { px, w, h } = lastIdPx;
          let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity, np = 0;
          for (let i = 0; i < w * h; i++) { const k = px[4 * i]! | (px[4 * i + 1]! << 8) | (px[4 * i + 2]! << 16); if (k - 1 !== e) continue; const x = i % w, y = (i / w) | 0; np++; x0 = Math.min(x0, x), x1 = Math.max(x1, x), y0 = Math.min(y0, y), y1 = Math.max(y1, y); }
          const mv = matrices().mvp;
          let bx0 = Infinity, by0 = Infinity, bx1 = -Infinity, by1 = -Infinity;
          for (let c = 0; c < 8; c++) {
            const q = [c & 1 ? boxes[6 * e + 3]! : boxes[6 * e]!, c & 2 ? boxes[6 * e + 4]! : boxes[6 * e + 1]!, c & 4 ? boxes[6 * e + 5]! : boxes[6 * e + 2]!];
            const cl = [0, 1, 3].map((rr) => mv[rr]! * q[0]! + mv[4 + rr]! * q[1]! + mv[8 + rr]! * q[2]! + mv[12 + rr]!);
            const sx = ((cl[0]! / cl[2]! + 1) / 2) * w, sy = ((cl[1]! / cl[2]! + 1) / 2) * h;
            bx0 = Math.min(bx0, sx), bx1 = Math.max(bx1, sx), by0 = Math.min(by0, sy), by1 = Math.max(by1, sy);
          }
          r.truthPx = [np, x0, y0, x1, y1], r.boxPx = [bx0, by0, bx1, by1].map(Math.round);
        }
        if (d?.g && slots[d.model]?.gl && out.length < max) { // the instance as the GPU holds it
          const sg = slots[d.model]!.gl!, ii = elInst[e]!, mat = new Float32Array(12), el = new Uint32Array(1);
          gl.bindBuffer(gl.ARRAY_BUFFER, sg.instMat), gl.getBufferSubData(gl.ARRAY_BUFFER, 48 * ii, mat);
          gl.bindBuffer(gl.ARRAY_BUFFER, sg.instEl), gl.getBufferSubData(gl.ARRAY_BUFFER, 4 * ii, el);
          gl.bindBuffer(gl.ARRAY_BUFFER, null);
          const pl = m.place!.subarray(12 * e, 12 * e + 12);
          r.gpuInst = { ii, el: el[0], matOk: mat.every((v, k) => v === pl[k]), gi0: d.g.i0, shapeI0: shapeI0[(m.shape![e] ?? 1) - 1], gni: d.g.ni, ni: m.ni[e], first: d.g.first, bFirst: d.b.first, idx: d.items.indexOf(e), instEls: instOfNow[d.model]?.[ii] };
        }
        if (out.length < max) out.push(r);
      }
      return { kinds, first: out };
    },
    /** VIS2 (diagnosis): link k's mesh asked again, each element's box from its real vertices (its own frame, then placed)
     *  against the page's (lboxes, boxes): how many lie outside, with the first ones. */
    boxAudit: async (k: number, tol = 1) => {
      const m = mesh, ask = source.linkMesh, l = m?.links?.[k];
      if (!m || !ask || !l) return null;
      const r = await ask(k, l.name, linkKeys[k] ?? 0);
      if (!r.bytes) return { err: "no bytes" };
      const lm = parseBmsh(r.bytes, 0, true);
      if (lm.ids.length !== l.elements) return { err: "count" };
      const tb = new Float32Array(6), pb = new Float64Array(6);
      let bad = 0, badPlaced = 0, empty = 0;
      const first: unknown[] = [];
      for (let i = 0; i < lm.ids.length; i++) {
        const e = l.e0 + i;
        if (!lm.nv[i]) continue;
        elementBox(lm.pos, lm.v0[i] ?? 0, lm.nv[i] ?? 0, lm.shape?.[i] ? lm.place!.subarray(12 * i, 12 * i + 12) : null, tb, 0);
        let out = false;
        for (let a = 0; a < 3; a++) if (tb[a]! < lboxes[6 * e + a]! - tol || tb[3 + a]! > lboxes[6 * e + 3 + a]! + tol) out = true;
        if (!(lboxes[6 * e]! <= lboxes[6 * e + 3]!)) empty++;
        xfBox(l.xf, tb, 0, pb, 0);
        let outP = false;
        for (let a = 0; a < 3; a++) if (pb[a]! < boxes[6 * e + a]! - tol || pb[3 + a]! > boxes[6 * e + 3 + a]! + tol) outP = true;
        if (out) bad++;
        if (outP) badPlaced++;
        if ((out || outP) && first.length < 12) first.push({ e, i, shape: lm.shape?.[i] ?? 0, pageShape: m.shape?.[e] ?? 0, nv: lm.nv[i], pageNv: m.nv[e], real: Array.from(tb).map(Math.round), page: Array.from(lboxes.subarray(6 * e, 6 * e + 6)).map(Math.round), place: lm.shape?.[i] ? Array.from(lm.place!.subarray(12 * i, 12 * i + 12)).map((v) => +v.toFixed(3)) : null });
      }
      return { elements: lm.ids.length, bad, badPlaced, empty, first };
    },
    /** LNK3D: the canvas as last drawn (RGBA, bottom row first), no new frame. */
    read: () => {
      const out = new Uint8Array(4 * canvas.width * canvas.height);
      gl.readPixels(0, 0, canvas.width, canvas.height, gl.RGBA, gl.UNSIGNED_BYTE, out);
      return { w: canvas.width, h: canvas.height, rgba: out };
    },
    /** The picture at the home camera fitted (yaw, pitch offsets in radians), as RGBA bytes (bottom row first). */
    snap: (dyaw: number, dpitch: number) => {
      turn = null;
      yaw = HOME_YAW + projectNorth() + dyaw, pitch = HOME_PITCH + dpitch;
      fit();
      fitCanvas();
      lastCamKey = camKey(); // 3D1: a still picture (not a moving frame)
      lastCamChangeAt = -Infinity; // VIS1 (P0-a)
      fill = null, fillEdges = false;
      draw();
      const out = new Uint8Array(4 * canvas.width * canvas.height);
      gl.readPixels(0, 0, canvas.width, canvas.height, gl.RGBA, gl.UNSIGNED_BYTE, out);
      return { w: canvas.width, h: canvas.height, rgba: out };
    },
  };
  // ---- end 3D1 ----

  // ---- LNK3D (D, ADR 0078 amendment 3): the GPU memory budget across links. Each frame after drawing: a link
  // whose detail was released and that the frame needed is uploaded again (one a frame, from the page's
  // copy: its slices; a link no longer needed by then is not: cancelled); over the budget, the loaded links
  // least recently needed (hidden ones, those drawn as proxies, those out of view) give their buffers back,
  // never the selected element's link nor one needed in the last NEED_MS. ----
  const wanted = new Set<number>();
  /** SIZE3 (B): per wanted link the largest of its wanted batches on screen (box diagonal over distance). */
  const wantScore = new Map<number, number>();
  /** SIZE3 (B): per link whether the last frame wanted its detail (unknown until a frame ran). */
  const wantedLast = new Map<number, boolean>();
  const NEED_MS = 2000, NEED_FRAMES = 3;
  const budgetStep = () => {
    const m = mesh;
    if (!m || slots.length < 2) return;
    const now = performance.now(), sel = state.value.linked?.link ?? null;
    const selK = sel === null ? -1 : 1 + (m.links ?? []).findIndex((l) => l.name === sel);
    if (selK > 0 && slots[selK]) slots[selK]!.needAt = now, slots[selK]!.needFrame = drawFrame;
    let changed = false;
    const w = [...wanted].filter((k) => slots[k] && !slots[k]!.gl && !slots[k]!.fetching && m.links?.[k - 1]?.visible); // (SIZE3: not one on its way)
    const score = new Map(wantScore);
    for (const x of slots) if (x.k > 0) wantedLast.set(x.k, wanted.has(x.k)); // SIZE3 (B)
    wanted.clear(), wantScore.clear();
    // SIZE3 (B): links' meshes asked in a pipeline of 2 at most (fetch, read in a worker, upload, its arrays dropped:
    // then the next), the largest on screen first (one at a time made a link shown again wait for another's)
    if (w.length && slots.filter((x) => x.fetching).length < 2) {
      const x = slots[w.sort((a, b) => (score.get(b) ?? 0) - (score.get(a) ?? 0) || slots[b]!.needAt - slots[a]!.needAt)[0]!]!;
      const t0 = performance.now();
      if (partOf(x.k)) loadSlot(x); // (its lists with it, when the page still has them)
      else void refetchSlot(x); // SIZE2: its mesh asked again, then uploaded and dropped from the page
      x.needAt = now, changed = true, dirty = true;
      stats.linkReloadMs = performance.now() - t0;
    }
    const budget = stats.gpuBudget ?? gpuBudget(userSettings.value?.values.link_gpu_mb); // SIZE3: a setting in MB
    let total = gpuNow();
    if (total > budget) {
      const shownK = (x: Slot) => (m.links?.[x.k - 1]?.visible ? 1 : 0);
      // SIZE2FIX: in view or needed: never released. Released only when hidden, or not needed for NEED_FRAMES frames
      // drawn AND NEED_MS, never while its mesh is being asked nor within NEED_FRAMES frames of its detail coming back
      // (a slow machine's frames are few: wall time alone released the link on screen, then asked it again)
      const idle = (x: Slot) => drawFrame - (x.needFrame ?? -Infinity) >= NEED_FRAMES && now - x.needAt > NEED_MS;
      const cand = slots.filter((x) => x.k > 0 && x.gl && x.k !== selK && !x.fetching && drawFrame - (x.loadFrame ?? -Infinity) >= NEED_FRAMES && (!shownK(x) || idle(x)))
        .sort((a, b) => shownK(a) - shownK(b) || a.needAt - b.needAt); // the hidden ones first, then the least recently needed
      for (const x of cand) {
        if (total <= budget) break;
        total -= x.bytes;
        releaseSlot(x);
        changed = true, dirty = true;
      }
    }
    stats.gpuBytes = gpuNow();
    { // SIZE3 (C): the links' detail on the GPU against the budget (the performance panel)
      const used = slots.reduce((n, x) => n + (x.k > 0 ? x.bytes : 0), 0);
      stats.linkGpuBudget = budget, stats.linkGpuUsed = used;
      if (linkGpu.value?.used !== used || linkGpu.value?.budget !== budget) linkGpu.value = { used, budget };
    }
    if (!w.length && !slots.some((x) => x.fetching) && !loading && stats.linksDetailedAt === null) stats.linksDetailedAt = performance.now(); // SIZE3 (B)
    stats.decodes = { ...bmsh4Stats };
    if (changed || Object.keys(linkDetail.value).length !== (m.links?.length ?? 0)) {
      const d: Record<string, boolean> = {};
      (m.links ?? []).forEach((l, k) => { d[l.name] = !!slots[k + 1]?.gl; });
      linkDetail.value = d;
      stats.linkDetail = (m.links ?? []).map((l, k) => ({ name: l.name, detail: !!slots[k + 1]?.gl, bytes: slots[k + 1]?.bytes ?? 0, loads: slots[k + 1]?.loads ?? 0 }));
    }
  };
  // ---- end LNK3D (D) ----
  let seenAnnot: AnnotateState | null = null;
  const loop = () => {
    if (openViews.value !== seenViews) { // a tab, a view or the model changed: the view shown
      seenViews = openViews.value;
      const cur = openViews.value.current;
      const vc = cur?.type === "view" ? viewClip() : null;
      switchView(vc, cur?.type === "view" ? cur.id : null);
    }
    // Q2: a switch turned. VIEWSHEET: only the switches the viewport draws with (orthographic, edges, the section
    // tool), not any change of the action list (an action run, a dialog opened): a frame costs tens to hundreds of ms.
    if (actions.value !== seenActions) {
      seenActions = actions.value;
      const sig = `${pressed("orthographic", false)}${pressed("edges", true)}${pressed("draw_section", false)}`;
      if (sig !== seenSwitches) { seenSwitches = sig; dirty = true; }
    }
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
    } else if (!loading && mesh && wantRev === rev && mesh.rev !== state.value.rev && rev.startsWith("d")) {
      mesh.rev = stats.meshRev = state.value.rev; // LNK3D: a new rev drawing the same (a hide in a view): its frame is this rev's
      dirty = true;
    }
    if (visibility.value !== seenVis) { // a change (or the box tool): what is hidden, the box
      seenVis = visibility.value;
      vis = seenVis;
      sets = visibilitySets();
      dirty = true;
      const key = JSON.stringify(sets);
      if (key !== visKey && visKey !== "\u0000") { // LNK3D: the state texture, not the draw lists
        visKey = key;
        applyVisibility();
      }
    }
    // LNK3D: any change of the links (a hide, a move, a colour, a rule): how each is drawn, asked once
    const lkey = `${state.value.links ?? 0}`;
    if (mesh?.links?.length && !loading && lkey !== seenLinkKey) { seenLinkKey = lkey; applyLinkDraw(linkDraw()); }
    let previewNow = false;
    if (mesh && visKey === "\u0000" && firstOfMesh && !previewed) previewed = previewNow = true, previewBoxes(); // 3D2 (step 5)
    else if (mesh && visKey === "\u0000" && treeWait !== mesh) {
      visKey = JSON.stringify(sets);
      const tl = performance.now();
      drawLists();
      if (stats.loadSteps && !stats.loadSteps.lists) stats.loadSteps.lists = performance.now() - tl; // 3D2 (step 5)
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
    if (settingOut.value !== seenSetOut) { seenSetOut = settingOut.value; dirty = true; } // SETOUT: shown, moved, turned
    if (refPlanes.value !== seenRefPlanes) { seenRefPlanes = refPlanes.value; dirty = true; } // WAVE1B
    if (selectedOpening.value !== seenOpening) { seenOpening = selectedOpening.value; dirty = true; } // WAVE1B (B2)
    if (annotate.value !== seenAnnot) { // started or stopped (bimble.ts startDimension, startText)
      seenAnnot = annotate.value;
      stats.annotate = seenAnnot;
      dirty = true;
    }
    if (annotate.value?.active === true && !plan) annotCancel(); // the plan was left
    refreshAnnotations();
    if (refLayer.refresh(`${state.value.rev}|${viewId}`, plan !== null && !isSection(plan))) dirty = true; // L1 (SEC1: plans)
    // ---- G2: grid axes, again when the model, its links or the view change ----
    // WAVE1B (A3): in 3D too, at the active level's elevation, unless the view hides IfcGrid (its visibility)
    const gElev = storeys.value.find((x) => x.id === placing.value?.storey)?.elevation ?? 0;
    const gKey = `${state.value.rev}|${state.value.file}|${state.value.links ?? 0}|${viewId}|${plan ? "p" : `3d${gElev}`}`;
    if (gKey !== gridKey3) { gridKey3 = gKey; gridHidden = !plan && (viewVisibility()?.hidden_classes ?? []).includes("IfcGrid"); gridElev = gElev; }
    if (gridAxesLayer.refresh(gKey, (plan !== null && !isSection(plan)) || (!plan && !gridHidden))) dirty = true; // SEC1: plans
    // ---- end G2 ----
    if (tool && placing.value?.active !== true) showTool(null); // placing stopped elsewhere
    const key = mesh ? `${state.value.selection.join(",")}|${state.value.linked?.link ?? ""}` : "";
    if (mesh && key !== selKey) {
      selKey = key;
      const tc = performance.now();
      colours(); // LNK3D: the state texture
      if (stats.loadSteps && !stats.loadSteps.colours) stats.loadSteps.colours = performance.now() - tc; // 3D2 (step 5)
      // 3D2 (step 4): only colours changed and nothing else asks for a frame: over the kept picture
      if (dirty || fill !== null || fillEdges || !recoloured || !overpaint(recoloured)) dirty = true;
    }
    fitCanvas(); // BUG1
    // 3D1: STILL_MS after the camera stopped (no button held), the picture is completed in slices, one a
    // frame; anything else that redraws (a move, a selection) abandons the fill
    let fillStep = false;
    pollFence(); // VIS1 (P0-a)
    if (wholeNext && !dirty && performance.now() >= wholeAt) wholeNext = false, lastCamChangeAt = -Infinity, stillDue = 0, fill = null, fillEdges = false, dirty = true; // VIS1: after a feedback frame
    hiz?.poll(); // VIS1: the last picture's pyramid, when the GPU has it
    if (!dirty && stillDue && !drag && performance.now() >= stillDue && performance.now() - lastInputAt >= motionWin()) {
      stillDue = 0;
      if ((stats.moving?.t ?? 0) > 0 || (stats.moving?.outlines ?? 0) > 0 || (stats.moving?.proxied ?? 0) > 0) fill = { t: Infinity, id: ++fillSerial }, fillEdges = false, dirty = fillStep = true; // 3D2: proxies at 3 px
    } else if (!dirty && (fill !== null || fillEdges)) dirty = fillStep = true;
    if (!dirty && occPending && pollOcc()) dirty = true; // 3D2 (step 3): a hidden batch showed again
    if (dirty && !fillStep) fill = null, fillEdges = false;
    if (dirty && !previewNow && (treeWait === null || treeWait !== mesh)) { // (3D2: a box preview is this loop's frame; the lists, after the octree, and the first frame come next)
      dirty = false;
      overlayDirty = false; // draw() draws the overlay too
      draw();
      stats.drawn = { frame: stats.frames, rev: mesh?.rev ?? null, w: canvas.width, h: canvas.height }; // BUG1
    } else if (overlayDirty) { // MEAS1: the overlay alone
      overlayDirty = false;
      drawOverlay();
      stats.overlayFrames++;
    }
    budgetStep(); // LNK3D (D)
    stats.loops = (stats.loops ?? 0) + 1; // BUG1
    requestAnimationFrame(loop);
  };
  requestAnimationFrame(loop);
  return () => { dirty = true; };
}
