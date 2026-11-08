# Architecture decision records

Short records of decisions: the problem, what we chose, what we rejected, evidence, and when to revisit.
Humans and agents read these before changing anything structural (see `CLAUDE.md`). New decision → new file;
a decision that changes an old one says which one it supersedes.

| # | Decision | Status |
|---|---|---|
| [0001](0001-iteration-1-stack.md) | Rust core, own wgpu renderer, IFC-typed native format | Accepted |
| [0002](0002-pure-rust-ifc-reader-for-display.md) | Pure-Rust IFC reader for display; IfcOpenShell as reference | Accepted |
| [0003](0003-scope-and-data-model.md) | Structural scope; IFC as the model; `.bimble` as a superset; `.b3d` cache | Accepted |
| [0004](0004-versioning-model.md) | Stable ids, canonical form, hashes, semantic diff ("GitHub for building models") | Accepted; store in 0017 |
| [0005](0005-commands-undo-transactions.md) | All changes are Commands; undo via inverses; atomic groups | Accepted |
| [0006](0006-rendering.md) | Drawing-style rendering, offscreen target, redraw on change | Accepted |
| [0007](0007-units-precision-tolerances.md) | Units, precision, tolerances | Accepted |
| [0008](0008-untrusted-input-and-client-data.md) | Untrusted-input limits; client data stays out of the repo | Accepted |
| [0009](0009-testing-ci-releases.md) | IfcOpenShell oracle, CI with software GPU, release workflow | Accepted |
| [0010](0010-geometry-kernel.md) | Geometry kernel: manifold3d for mesh booleans; exact B-rep later | Accepted |
| [0011](0011-performance-regressions.md) | Performance: geometry budgets fail the build, timing trend warns | Accepted |
| [0012](0012-parallel-loading.md) | Parallel loading: chunked parse, per-element geometry on all cores, mimalloc | Accepted |
| [0013](0013-graphic-style.md) | Graphic style: every colour, hatch and light value is data (JSON, `--style`, live editor) | Accepted |
| [0014](0014-section-box.md) | Section box: GPU clipping, back faces as cut surfaces, back edges hidden | Accepted |
| [0015](0015-drawings-plans-at-scale.md) | Drawings on paper: one sheet model (SVG, PDF); plans as exact sections at scale | Accepted |
| [0016](0016-linked-models-and-comparison.md) | Linked models (layers) and comparison by geometry, not by id | Accepted |
| [0017](0017-history-store.md) | History next to the model: content-addressed objects, branches, field-level merge | Accepted |
| [0018](0018-plan-templates-and-conventions.md) | Plans like the office's: templates, seen fills, grid axes, dimension chains | Accepted |
| [0019](0019-editing-in-the-viewer.md) | Editing .bimble models in the viewer through Commands; rebuild; history | Accepted |
| [0020](0020-web-version.md) | Bimble in a browser: the same Rust as WebAssembly; WebGPU or WebGL2; manifold's own wasm | Accepted |
| [0021](0021-views-and-reference-plans.md) | Views: 3D, plans looking down/up with a view range, sections; reference plans (DXF, PDF) as dead geometry; navigator layout | Accepted |
| [0022](0022-one-button-release.md) | Releases from one button: the workflow sets the version, commits, tags, builds | Accepted |
| [0023](0023-paper-drawings-of-views.md) | Paper drawings of any view (turned scene); seen from above = upward faces; own hidden edges dropped | Accepted |
| [0024](0024-sheets.md) | Sheets: views placed at their own scales, one title block; dimensions and texts belong to views | Accepted |
| [0025](0025-layouts-in-the-main-view.md) | Layouts in the main view: the sheet rendered from its own drawing; tools in paper mm | Accepted |
| [0026](0026-office-standards-on-paper.md) | Office standards on paper: pens per scale, line, text, dimension and hatch types, all in the style | Accepted |
| [0027](0027-main-window-ribbon-palette-standards.md) | Main window: ribbon, command palette (Ctrl+K), standards in a side panel | Accepted |
| [0028](0028-explicit-state.md) | Explicit state: status bar chips, unsaved work in words, confirm before close and reset | Accepted |
| [0029](0029-schedules.md) | Schedules: element tables measured from the geometry, grouped by level, CSV | Accepted |
| [0030](0030-project-information.md) | Project information: one map in the model, for every title block | Accepted |
| [0031](0031-material-library.md) | Material library (EN/SIA grades with values), material browser, mass in schedules | Accepted |
| [0032](0032-ifc-import-for-editing.md) | IFC import for editing: extend the own reader, convert to generators; openings reported, not cut | Accepted |
| [0033](0033-models-too-large-for-the-machine.md) | Models too large for the machine: an 80 % memory budget, estimate, census, choose storeys/classes/materials before a cut | Accepted |
| [0034](0034-shapes-once-and-a-box-tree.md) | Shapes meshed once and placed many times (instancing of mapped items); a box tree over elements for picking, culling and clashes | Accepted |
| [0035](0035-simplify-while-the-view-moves.md) | While the view moves, leave out what is smallest on screen; a frame-rate target (setting, 30 fps) decides how much; stills whole | Accepted |

Session journals (what happened, in order, with measurements) are in [`docs/journal`](../journal).

## Template

```markdown
# ADR NNNN — Title

- Status: Proposed | Accepted | Superseded by NNNN (date)
- Decided by: who, and on what basis
- Reversal cost: low | medium | high

## Context / Problem
## Decision
## Alternatives considered
## Evidence
## Revisit when
```
