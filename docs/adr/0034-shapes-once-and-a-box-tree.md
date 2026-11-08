# ADR 0034 — Shapes meshed once and placed many times; a box tree over elements

- Status: Accepted (8 Nov 2026)
- Decided by: Pierre ("draw shared shapes once… like video games"; "anticipate clash detection
  between IFCs"; explore BoxTree); implementation by Claude
- Reversal cost: medium (the scene's geometry type changes; every reader of it goes through
  one accessor)

## Context / Problem

After ADR 0033, large IFC models fit by choosing a part; they should rather fit whole. Measured
on five real models, most triangles are copies of a few shapes (IfcMappedItem over
IfcRepresentationMap: types, fittings, furniture), each meshed and stored again per element:

| file | triangles | in single-mapped-item bodies | unique maps | their triangles |
|---|---|---|---|---|
| 200 MB electrical | 8.84 M | 99 % | 695 | 0.25 M |
| 335 MB architecture | 5.43 M | 86 % | 1 519 | 1.35 M |
| 872 MB plumbing | 9.10 M | 45 % | 942 | 0.51 M |
| 758 MB sanitary | 20.99 M | 85 % | 2 568 | 1.21 M |
| 415 MB sprinklers | 35.80 M | 96 % | 1 180 | 1.27 M |

(Mapped bodies with openings: 3 in all, kept expanded.) Picking tested every triangle: 36 ms a
ray on the 200 MB model, at each mouse move. Clash detection between models is coming and needs a
spatial index anyway.

## Decision

1. **Shapes once.** A product whose Body is exactly one IfcMappedItem (and that has no opening)
   points to its map's mesh, meshed once in the map's coordinates (mm), with its own transform
   (product placement × mapping target, relative to the scene origin). Everything else keeps its
   own mesh. `SceneElement`'s geometry becomes `Own(Mesh)` or `Instance { shape, to_scene }`;
   code that needs world triangles asks the element for them (`world_mesh()`), so the compiler
   found every reader when the field changed.
2. **Phase 1, CPU:** the scene holds shapes once (RAM, meshing time). The GPU upload still expands
   instances into world vertices. **Phase 2, GPU:** one vertex buffer per shape and an instance
   buffer of transforms (instanced draws work on WebGL2 and WebGPU), so GPU memory follows unique
   triangles too; shapes with few copies are baked into the merged buffers instead (a draw call
   per shape costs more than it saves there — xeokit's measured pitfall).
3. **A box tree over elements** (`bimble_geom::bvh`): a binary bounding-volume hierarchy (a
   BoxTree, Barequet–Chazelle–Guibas 1996; median split, 4 boxes a leaf, flat arrays, no
   recursion). Queries: nearest along a ray (picking, snapping, wheel and orbit pivots), boxes
   overlapping a box (searching, culling), and pairs between two trees (the broad phase of clash
   detection between models). It is the top level of a two-level structure (as ray-tracing APIs'
   TLAS/BLAS): per-shape trees over triangles come when one element is big enough to need it
   (terrain, clash narrow phase), and are shared by every instance of a shape — rays and boxes
   go into the shape's frame, not the shape into the world's.
4. Estimates (ADR 0033) keep counting every instance's triangles until phase 2, so they stay on
   the safe side; recalibrate after each phase.

## Alternatives considered

- **Merge copies into big buffers (batching), no instances:** fewest draw calls, but memory grows
  with copies — the problem.
- **Hash-deduplicate geometry the file didn't instance:** later, on top of shapes (same
  representation); measure the duplicate ratio first.
- **Better trees (SAH builds, wide BVH4/8, GPU LBVH/PLOC builds):** worth it for triangle-level
  trees and clash narrow phases; for 30 000 element boxes the median split already answers a ray
  in ~10 µs, so the build quality isn't what limits.
- **GPU ID-buffer picking:** exact and free once rendered, but a GPU read-back per pick and no
  help for snapping, searching or clashes; may complement the tree later.

## Evidence

- Tree on the 200 MB model (29 935 elements, release): build 15 ms; a ray 35.7 ms brute force →
  10.6 µs (same answers on 100 rays); a 10 m box query 4 µs; the model against itself (clash
  broad phase) 26 878 overlapping pairs in 10 ms.
- Test `picking_through_the_tree_matches_testing_everything` (400 rays, with and without a section
  box) found a real picking bug: with the box on, any element wholly inside the box behind the
  cut counted as cut (its back face lay past the box entry), so a far element could be picked.
  An element now counts as cut only if the nearest of its triangles the ray meets faces away.
- `bvh::tests::queries_match_brute_force` (rays, boxes, pairs).

Results (9 Nov):
- Phase 1 (CPU): load 2.5 → 0.46 s (200 MB model); every one of its 28 318 instances has the
  bounds expanding gave (0.0 mm).
- Phase 2 (GPU, every draw instanced; ≥ 4 copies): render peak 1.71 → 1.01 GB, upload 255 → 17 ms,
  frame unchanged, renders pixel-identical to phase 1. The 415 MB model shows all its 35.8 M
  triangles (cut at 10 M before) at 30 fps; test `instanced_shapes_draw_like_placed_copies`
  (mirror and section box; it fails with 463 pixels if the mirror's winding is ignored).
- Memory estimates (ADR 0033) count stored triangles: own, each shape once, one per copy.
- The guard against hostile files counts **drawn** triangles (10 a displayed byte): a shape stored
  once but placed by thousands of small products would otherwise pass it (test
  `many_copies_of_one_shape_stop_at_the_drawn_triangle_guard`; 3 624 drawn against 1 000 before).
- Plans expand only what their view range shows: a storey plan of the 415 MB model peaks at
  1.67 GB, under `render`'s 2.06 GB.

## Prior art

- IfcOpenShell's wgpu viewer (`src/ifcviewer`, Oct 2026): local-coordinate shapes from the
  geometry iterator, keyed by representation (+ material, + openings: an element with openings is
  its own shape); `MeshDedup` (bucket by counts, rotation-invariant check, Kabsch fit) for copies
  the file didn't instance; instanced draws; section planes as a fragment discard on the world
  position. IfcClash builds a box tree per set and runs tree against tree, then triangles.
- That Open Fragments (representations + samples + transforms), web-ifc `PlacedGeometry`,
  xeokit SceneModel (`createGeometry` + `createMesh`; their warning: many shapes with few copies
  each cost a draw call apiece, so those are better baked into merged batches), glTF
  `EXT_mesh_gpu_instancing`, iTwin.js tiles (instancing on by default), Autodesk SVF2.
- Two-level BVH as in DXR/Vulkan (TLAS over instances, BLAS per shape); Wald et al. 2017
  (rebraiding) on its cost when instance boxes overlap.

## Revisit when

- Phase 2 lands: measure GPU memory and frame time; recalibrate ADR 0033.
- Clash detection starts: per-shape triangle trees and the narrow phase (triangle–triangle,
  tolerances), in their own ADR.
- Culling while navigating (draw order by size, frame-rate target) uses the same tree.
