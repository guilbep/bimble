# ADR 0010 — Geometry kernel: manifold3d for mesh booleans; exact B-rep later

- Status: Accepted (5 Nov 2026)
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

## Revisit when
- Exact curved sections, fillets on solids, or exact quantities are needed → B-rep ADR.
- manifold-csg stops being maintained, or a pure-Rust kernel proves robust on our fixtures.
