# Bimble — Roadmap (v1)

_5 Nov 2026. Supersedes v0 (Python / IFC-file-as-native). Sources: the Bimble PRD/RFC and the
"Feuille de route – Logiciel BIM structurel open source" (v0.1)._

## 1. Product principles

1. **Fast on old machines.** Architects and engineers often have old laptops with weak GPUs. 3D must stay
   fluid on an integrated GPU. This is the #1 differentiator against Revit.
2. **Drawing-style rendering, not photorealism.** Black and white plus a palette of about 12 colours, flat
   shading and edges. Materials are told apart by **hatch patterns** (concrete, steel, timber, insulation…),
   not textures. Cheap on old GPUs and close to how engineers read drawings. Ray-traced images (shadows,
   ambient occlusion) are a later option, for still images only.
3. **IFC is the data model and the vocabulary.** Every object is an IFC class (IfcColumn, IfcBeam, IfcSlab…),
   and the UI uses IFC names and concepts. No invented taxonomy where IFC already has one.
4. **Our own native format, `.bimble`, is a superset of IFC.** Anything IFC can express maps 1:1. On top of that
   we store what IFC can't: generators and parameters, constraints, history, branches. IFC export always works,
   and lists what it couldn't carry.
5. **"GitHub for building models."** Commits, branches, element-level diffs, review and merge are designed in from
   day 1, not added later.
6. **The drawing is the product** (formwork, then rebar). Structural concrete first, then steel, then timber.
7. **Agent- and script-first.** Every action is a command, the same for the UI, the CLI, scripts and MCP.

## 2. Stack

| Layer | Choice | Why / status |
|---|---|---|
| Core language | **Rust** (workspace of small crates) | Native speed, compiles to WebAssembly for the browser later, safe for untrusted files |
| Rendering | **Own renderer on `wgpu`** | wgpu is the low-level GPU API (Vulkan / Metal / DX12, with an **OpenGL fallback for old machines**, and WebGPU/WebGL2 in the browser). Flat shading, edge lines, procedural hatching in the shader, instancing, culling. No textures, no game engine. |
| UI (POC) | `egui` + `winit` | Fast to build, same code native and web. Can be replaced later. |
| Geometry (POC) | Own mesher: profile extrusion + `earcutr` | Covers columns, beams, walls, slabs, footings. |
| Geometry (later) | Booleans and exact B-Rep: evaluate `manifold`, OCCT (C++ bindings), `truck` | ADR before Phase 2 (openings, rebar) |
| IFC export | Own IFC4 STEP writer | Validated in tests against IfcOpenShell |
| IFC import | **IfcOpenShell (C++) called from Rust** is the main option: it parses and validates untrusted IFC files for us. Alternative: `web-ifc` | ADR at I3 |
| Native format | `.bimble`: canonical JSON for the POC → content-addressed object store (git-like) | Format spec in `docs/format/` |
| Geometry cache | **`.b3d`**: chunked binary meshes (per storey / spatial chunk, instanced), so the viewer opens without re-meshing. A derived cache, never the source of truth | Built when the viewer needs it (I1–I2). Spec in `docs/format/` before any code |
| Test oracle | IfcOpenShell (Python) **in tests only** | Proves our IFC opens in a reference tool |
| Architecture reference | **CADCraft** (Rust): crate layout, command registry, undo/redo, CLI/MCP access | Read before I2. Reference only, not a dependency |
| CI | GitHub Actions, Linux / macOS / Windows | |
| Licence | MIT (repo). Legal check before 1st release | ⚠️ |

**Performance target (proposed, to validate with users):** 20,000 elements at 60 fps on a ~2016 laptop with
an integrated GPU (Intel HD 520 class) and 8 GB RAM. Measured on a generated fixture from I1.

## 3. Versioning model ("GitHub for architects")

- Every element has a **stable IFC GlobalId**. It never changes on edit.
- Elements serialize **canonically** (sorted keys, fixed float formatting) → a **content hash** per element.
- Model hash = hash of the sorted (GlobalId, element hash) pairs. Same model → same hash, on any machine.
- **Semantic diff**: added / removed / modified elements, field by field ("IfcBeam B12: height 500 → 600").
- Next: commits (parent, author, message, model hash), branches, merge with element-level conflicts, review UI.
- Plain Git merges of text files are not enough for BIM. Merges are done on elements, not lines.

## 4. Iterations

**I0 — Core (now)**
- Crates: `bimble-core` (IFC-typed model, GlobalIds, generators, materials, commands + undo, hash, diff),
  `bimble-geom` (extrusion meshes), `bimble-ifc` (IFC4 export), `bimble-cli`
- Demo: the 2-storey 3×2-bay concrete frame → `.bimble` → IFC that IfcOpenShell opens → diff two versions

**I1 — Viewer (fast 3D)**
- `bimble-viewer`: wgpu renderer, flat shading + edges, hatch pattern per material, 12-colour palette,
  instanced meshes, orbit camera, picking, storey filter
- Benchmark fixture: 20k elements; frame time measured on a real old laptop

**I2 — Edit and history** (after reading CADCraft's command and undo design)
- Command palette in the viewer, undo/redo, property panel with IFC names
- Object store: commit, log, branch, checkout, diff between commits; CLI first (`bimble commit / log / diff`)

**I3 — IFC import**
- IfcOpenShell from Rust; an architect's IFC as a background reference; round-trip tests with Bonsai, FreeCAD, Revit
- First real fixture: `LA24070_LMP_STR_32_R25` (Revit 2025 structural export, IFC4, metres, 881 elements).
  Measured with IfcOpenShell 0.9 (5 Nov 2026):
  - Geometry is light: 809 products tessellate to ~21k triangles in 3.4 s on 2 cores. The viewer's challenge
    is element count and draw calls, not triangles.
  - ~90 % of bodies are `IfcExtrudedAreaSolid` with `IfcArbitraryClosedProfileDef`: Revit writes even
    rectangular beams as polylines. We need an **extruded-polygon generator** (with voids), and rectangle
    detection to map back to `rect_beam` / `rect_column`.
  - The rest is `IfcPolygonalFaceSet` (parts, some walls and footings, roofs): we need an **explicit-mesh**
    element kind for non-parametric imports.
  - Also present: `IfcBuildingElementPart` (153), `IfcRoof`, 33 `IfcOpeningElement` (openings → booleans needed),
    `IfcGeographicElement` (terrain).
  - Materials: GL24h glulam (420 elements) alongside C30/37 concrete. Timber is in real projects from day 1.
  - Units are metres: import converts to mm.

**I4 — First formwork plan**
- Storey section → 2D lines → SVG/PDF with dimensions; compared against a real office drawing

**Then** (from the Feuille de route): concrete formwork MVP + sub-models/locking → rebar → steel and timber →
node editor. Merge/review UI and a sync server come along with collaboration. Ray-traced still images when useful.

## 5. Open decisions
1. Exact-geometry/boolean kernel (before openings and rebar)
2. IFC import: IfcOpenShell via C++ bindings vs web-ifc
3. Desktop shell: native winit app vs Tauri + web
4. Performance target: confirm the reference "old machine" with real users
5. Palette and hatch patterns per material: agree with the engineer (office drawing conventions)
6. Rebar code (SIA 262 / EC2) — engineer
7. Licence: legal check

## 6. Working rules
- Short spec per feature (input, output, edge cases, reference drawing) before code.
- Decisions in `docs/adr/`. `CLAUDE.md` = agent memory.
- UI never mutates the model directly: commands only.
- No performance claim without a benchmark. Project files are untrusted input; parameters are never executed as code.
