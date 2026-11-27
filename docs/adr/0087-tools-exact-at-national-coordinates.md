# ADR 0087 — The core's tools exact at national coordinates

- Status: Accepted, 27 Nov 2026
- Decided by: Pierre delegated the design on 27 Nov 2026 (Mission 1: a folder of IFC files linked at Swiss
  national coordinates, 2,600 km from 0, into an empty model); the choices below by Claude on tickets MEAS2 and
  MEAS3, to be reviewed on the implementation.
- Concerns: CLAUDE.md ("f64 in the model; f32 only in GPU meshes, relative to the scene origin"), ADR 0068 rule 2
  (tools live in C), ADR 0074 (200 ms), ADR 0081 (Mission 1 across links), ADR 0082 (DXF export).
- Reversal cost: low (session state and its JSON: an optional key `base`; no file format, no golden changed).

## Context

A real project is an empty open model (scene origin 0) with 16 links whose own coordinates are about
(2.536e9, 1.153e9, 4.7e5) mm. An f32 step there is 256 mm. The renderers already draw relative to their own
origin; the core's tools did not. Measured on a synthetic building at (2.6e9, 1.2e9, 5e5) mm turned 30 degrees:
measure found no corner and no edge, its face point was 37.9 mm off, the section box 150.6 mm, a box face could not
be dragged, and the plan's DXF put the slab's 18,300 mm edge at 18,353.6 mm (when it could be exported at all).

## Decision

1. **Positions that cross the API are f64 scene millimetres. Every f32 structure lives in a local frame.**
   Each scene is already local: its origin is the centre of its bounds. The rule below makes the tools keep that
   frame instead of adding a link's offset in f32.
2. **Measure (MEAS2).** A layer's offset is f64. The pointer ray is moved into the link's frame in f64, and corners,
   edges and faces are found there. A snap is f32 relative to the origin of the scene it was taken on
   (`cc_msnap.org`, f64), so two points of one link are measured exactly as at 0. A work-plane point more than 1 km
   out moves its frame to the point. A layer with a non-finite offset is not hit.
3. **The section box (MEAS3): a frame, `cc_vis.base` (f64).** The box's corners stay f32, as Rust's ClipBox, but
   relative to `base`. `base` is 0 unless the box's centre is more than 1 km (`CC_VIS_FAR_MM`) from the scene origin;
   then it is that centre in 1 m steps. The fits over far links, Box to selection, the turn to project or true north,
   `box_set`, the face drag (rays moved into the frame), a plan's and a section's clip, the measure clip and the
   tiles' shell test all read and write through it. Near the origin every path is Rust's f32 arithmetic, unchanged.
4. **JSON: additive.** A box without a base prints exactly as before. A box with a base prints `"base": [x, y, z]`
   first, and `min`/`max` still mean scene coordinates: `base + corner`, written as f64 (shortest digits). This
   applies to `query visibility` (`section_box.box`), the box tool's answers and `query view_clip`. A reader that
   takes `min`/`max` as f64 and subtracts its render origin in f64 is exact. `base` is only needed to know the
   box's own frame. `box_set` takes scene coordinates within ±1e13 mm (it was ±1e7) and finds the base itself.
5. **Paper through links (MEAS3).** When the open model has no elements and the first drawn link is more than 1 km
   away, the merged paper scene takes that link's placed origin. Plans and sections are then cut in a local frame;
   DXF model space still writes world coordinates (the true national ones), in f64. `view_dxf` finds a plan from
   links as `view_svg` does (it answered "no view" before).

## Alternatives considered

- **f64 boxes everywhere (`double min[3], max[3]`).** Rejected: every near-origin path would change its rounding,
  and the Rust-origin goldens (section box, plan clip) would move.
- **Moving the empty open model's scene origin to the links.** Rejected: the BMSH origin and the rays of both
  renderers depend on it, and it would change whenever a link loads.
- **A per-link box frame.** Rejected: one box spans several links; one frame per box is enough at 1 m steps.

## Evidence

`ccore/test/measure_geo_test.c`, with the placement removed in f64:
- measure: corner, edge, face and distance within 2e-7 mm of the result at 0;
- section box: within 0.0014 mm (one f32 step at 18 m; before: 150.6 mm);
- a face dragged 1,000 mm moves 1,000.000 mm (before: it did not move);
- the plan's DXF: points within 0.001 mm, the slab's edge 18,300.0001 mm (before: 18,353.6 mm, points up to 907 mm
  off).

`measure_test.c`: a layer 2,600 km away gives the same snaps as at 0. Timings are unchanged (MEAS2's bench). The
fast gate and the named suites pass; no golden changed.

## Revisit when

- A section view at national coordinates is needed: `view_add` limits coordinates to ±10 km (the model's own
  check). A section through links far away cannot be stored yet.
- The open model has elements near 0 and links far away. Such a box spans 2,600 km and its corners stay f32
  there; there is one frame per box, not per side.
