# ADR 0010 — Geometry kernel: manifold3d for mesh booleans; exact B-rep later

- Status: Accepted (5 Nov 2026); amended 11 Nov 2026 (sewing, unclosed boundaries)
- Decided by: Pierre asked to settle the kernel question; candidates and evidence by Claude
- Reversal cost: low (one module, `bimble-geom::boolean`, with a three-function interface)

## Problem
Real files need boolean geometry: openings in walls and slabs (`IfcRelVoidsElement`), clipped beams and
walls (`IfcBooleanClippingResult` with half-spaces), solid differences (`IfcBooleanResult`). Later, rebar
and formwork drawings need cut sections of these solids. We need robust results at Swiss site coordinates
(millions of metres), fast enough for thousands of elements, building on macOS, Windows and Linux.

## Candidates (checked 5 Nov 2026)
| Candidate | What | Finding |
|---|---|---|
| **manifold3d** via `manifold-csg` 0.4 | C++ mesh boolean kernel (used by OpenSCAD), safe Rust bindings, f64, MIT/Apache | Spike: wall with 5 openings exact (rel. error 0); at site coordinates 4e-11; 2000 walls cut in 187 ms. Links statically. Build clones the pinned upstream tag (v3.5.3) and needs cmake + C++ |
| `csgrs` 0.20 | Pure-Rust BSP CSG | Doesn't resolve: depends on a yanked crate; last release Jul 2025. BSP CSG is also known to be slow and fragile on large meshes |
| `truck` / `monstertruck` | Rust B-rep with NURBS and booleans | Exact geometry; maturity for IFC-scale use unproven. Candidate for exact quantities and curved drawings later |
| Fornjot | Rust B-rep | Archived June 2026 ("shut down") |
| `BREP_kernel` 0.5 | New Rust B-rep kernel (Sep 2026) | Too new to depend on |
| OCCT via bindings | Industry B-rep (IfcOpenShell uses it) | Heavy C++ build on four targets. Remains the reference through IfcOpenShell in tests |

## Decision
- **manifold3d through `manifold-csg`** (default features off: no TBB), behind `bimble_geom::boolean::apply(op, a, &[b…])`.
  Inputs are welded exactly (bitwise vertices), shifted to a local origin, and must be closed solids;
  otherwise the call fails and the reader draws the element uncut **with a warning** (never silently).
- Half-spaces are built as finite prisms sized from the operand; polygon-bounded half-spaces are the
  intersection with the boundary prism.
- Exact B-rep (truck/monstertruck or OCCT) is deferred until exact curved geometry is needed. Quantities
  should then come from generators analytically, not from display meshes.

## Evidence
- Synthetic fixture `crates/bimble-ifc/tests/fixtures/booleans.ifc`: tilted half-space clip,
  polygonal bounded half-space, cylinder hole, wall with door opening. All exact against hand calculation
  and IfcOpenShell 0.9 (regression test).
- Real Revit IFC2x3 file (2735 elements, 71 booleans): all elements match IfcOpenShell with openings and
  booleans applied (volumes 0.18 %, bounding boxes 0.19 mm).
- Real Revit IFC4 reference-view file: 3 walls have openings that aren't closed solids, so they're not cut
  (reported). Volumes still match IfcOpenShell, which suggests the exporter already cut those bodies.
  Not verified further.

## Costs and risks
- First build compiles manifold (~1–4 min); CI needs cmake + a C++ compiler (present on GitHub runners).
- The build fetches the pinned manifold tag from GitHub at build time. Offline builds can point
  `MANIFOLD_CSG_LIB_DIR` at a prebuilt library.
- Cross-compiling macOS x86_64 on arm64 runners depends on cmake honouring the target. Watch CI.

## Amendment, 11 Nov 2026 (the office's files; Pierre: "propose fixes")

Running every IFC file of the office through import and pack showed two cases the kernel
refused:

- **Solids not quite closed.** Revit Breps (slabs with holes in their faces) have corners
  written twice, 0.03–0.07 mm apart: not closed for manifold, so their openings weren't cut (in
  the viewer and pack with a warning; in a `.bimble`, silently). When the kernel refuses an
  operand as not closed, `boolean::apply` **sews** it: points within `SEW_MM` = 0.1 mm of an
  earlier one are moved onto it (each coordinate, first one kept, in order: deterministic),
  collapsed triangles dropped, then tries once more. Valid solids never take this path, so
  results on them don't change. On one file: 387 openings not cut → 4. Most of those openings
  were also holes in the Brep already: the volumes changed by at most 0.02 m³ per slab.
  Not sewn: open surface models and faces turned inside out (they need faces reoriented).
- **Polygonal bounded half-spaces whose boundary doesn't close** (a composite curve whose
  arcs and lines are hundreds of metres apart; a polyline that doesn't return to its first
  point): Bimble closed them with a guessed line, or gave up and didn't cut. IfcOpenShell 0.9
  cuts by the plane alone in every such case tried (a 10 mm gap, a missing side, the broken
  composite, the open polyline); Bimble now does the same, with a warning. A wall of the
  office's had been drawn 4.0 m tall instead of 3.4 m (2.47 vs 2.10 m³; IfcOpenShell
  2.10260, Bimble now 2.102604). Fixture cases 5 and 6 in `booleans.ifc`.

## Revisit when
- Exact curved sections, fillets on solids, or exact quantities are needed → B-rep ADR.
- manifold-csg stops being maintained, or a pure-Rust kernel proves robust on our fixtures.
