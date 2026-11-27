# Cluster hierarchy or clustering? Measured on our scene (ticket EXP1)

27 Nov 2026. Experiment, throw-away code (not in the repo). Question: would a cluster hierarchy (Nanite-style meshlet
DAG) or edge-collapse simplification pay for our scene, against today's vertex clustering on a grid (`cc/tile1`,
ADR 0078 amendment 6)? The literature review (`2026-11-27-massive-model-rendering-literature.md`, rows 8, 9, 14) rated
it "low: small disjoint parts do not simplify" by inference. This document replaces that inference with measurements.
Every number below comes from one of the runs described in "Method"; nothing is estimated unless the text says so.

## Method

**Data.**
- The local folder: a folder of 17 IFC files on this machine (client data: no names here). Files are numbered #1 to
  #17 by ascending IFC size. Input: the link caches (`.bimbleview`, format 6) made today from that folder. File #1
  (the smallest) has no cache in that set, so it is not measured. 16 files: 252,172 elements,
  110,459,790 triangles drawn (each instanced copy counted). File #2 (6,132 triangles) uses a local frame; the 15 others
  share one georeferenced frame. Item 5 therefore uses files #3 to #17 (110,453,658 triangles).
  Items 1 to 3 cover all 16 files. The tables show #5 and #7 (two small files), #11, #14, #15, #16, #17 (94.9 M of the
  110.5 M triangles) and the five large files together.
- The synthetic scene (reproducible): `python3 ccore/bench/mesh_heavy_model.py 400 2000 heavy.json` (the frame of
  `golden/frame.json` plus 400 closed UV spheres of 1,936 triangles, `IfcBuildingElementProxy`), opened in
  the `bimble-ccore` host binary of this worktree (`ccore/out/dev/bimble-ccore`, dated 27 Nov 10:50), exported with the action `export_ifc4`, converted
  with `bimble-ccore link-convert --cache-dir <dir> heavy.ifc`: 472 elements, 775,264 triangles, 2 shared shapes.
  Its numbers are reproducible; they describe a smooth closed mesh, not our data.

**Tools.** A scratch C++ tool (about 1,400 lines: a `.bimbleview` reader for positions and indices, a weld, statistics,
a CPU z-buffer, a point-to-surface distance), built with g++ 16.1.1 `-O2 -march=native` (the z-buffer `-O3`). meshoptimizer **v1.3**, commit
`9e1f07b159d3cb777f1c67ed31fc11fd117986f4` (MIT), built from source the same way. Machine: Apple M1 Pro, 16 GB,
aarch64 Linux, one thread for every timing, each file in its own process, memory capped at 6 GB.

**Weld.** Positions are put on the cache's lattice (round to 1/16 mm) and merged when equal. Triangles with two equal
welded corners are dropped. A shared shape is welded once in its own frame, and its numbers are counted once for each
element that draws it. Shares are of **triangles drawn** (copies counted), unless the column says "stored".

**Item 1, statistics.** Components: vertices joined by triangle edges, after the weld. A component is *closed* when
each of its edges has exactly 2 triangles with opposite directions. *Coplanar neighbour*: a triangle with an edge
neighbour (an edge with exactly 2 triangles) whose normal is within 0.1° or 1°. *Lossless coplanar merge*: triangles
are flooded into planar patches (normal within 0.1° of the seed's, corners within 0.1 mm of its plane, across
manifold edges). A patch is triangulated again with its boundary vertices only: n_b + 2L − 4 triangles (n_b
boundary vertices, L boundary loops), never more than it had. Boundary vertices are kept (no T-junction, no crack), so
this is the lossless lower bound of the merge. *Border vertices*: on an edge with one triangle.

**Item 2, per element.** Each unique mesh is simplified alone after the weld, at a target error E = 1, 5, 20, 100,
500 mm, with `target_index_count = 0` (stop at the error, not at a count):
- edge collapse: `meshopt_simplify`, `meshopt_SimplifyErrorAbsolute`; the same with `| meshopt_SimplifyLockBorder`;
  the same with `| meshopt_SimplifyPrune` (removes small disconnected parts under the error);
- `meshopt_simplifySloppy` (a grid search inside meshoptimizer), its relative error = E / `meshopt_simplifyScale`. When
  the error it reports is above E (it cannot go below its coarsest-to-finest grid range), the original count is
  taken: the yield at "error at most E";
- vertex clustering (today's coarse levels), reimplemented: one vertex per occupied grid cell, degenerate and
  duplicate triangles (same three cells, either orientation) removed. Two cell sizes: diagonal = E (cell E/√3: every
  point moves at most E, the bound) and cell = E (moves up to 1.73 E). The tile branch's measured error was 4.2 times
  under the bound (ADR 0078, amendment 6, point 33), so the true error of "cell = E" is likely under E; it is not
  measured here. `tiles.c` also places each cluster at its quadric minimum and builds per material family: the
  triangle counts here are of the plain grid.
- "Error" for edge collapse is what `meshopt_simplify` reports (`result_error`, absolute mm).

**Item 3, per region.** The elements of a file are cut into regions by a recursive median split of element centres
along the longest axis, until a region holds at most 40,000 drawn triangles (an element is never cut; an element of
more than 40,000 triangles is a region of its own). Each region: placed triangles gathered in scene mm, welded on the
1/16 mm lattice (elements that touch share vertices), then the same six simplifiers at the same E. All regions of
every file are simplified (no sampling).

**Item 4, meshlets.** File #14: each unique mesh welded, `meshopt_buildMeshlets` (64 vertices, 124 triangles,
cone weight 0.25), `meshopt_computeMeshletBounds`. Bytes: raw meshlets (16 B header, u32 vertex index, u8×3 triangle,
padded to 4) plus f32 positions; codecs: `meshopt_encodeMeshlet` (+16 B header per meshlet) and
`meshopt_encodeVertexBuffer` on lattice integer positions (int32×3, lossless against the cache); and, for reference,
`meshopt_encodeIndexBuffer` (after `meshopt_optimizeVertexCache`) with the same vertex codec. Cache bytes: the BMG1
blocks of the same meshes, positions and indices only, and whole (with the edge lines). Cone test, per element
copy: apex placed, axis by the cofactor of the placement, rejected when
`dot(normalize(apex − eye), axis) >= cutoff`. Back faces: every drawn triangle, normal by the cofactor, back facing
when `dot(centroid − eye, n) > 0`. All meshlets and triangles of the file count (no frustum).

**Item 5, what a view needs.** A CPU z-buffer at **2984 × 1580** (full resolution, not reduced), 60° vertical field of
view, near plane 50 mm, both faces drawn, pixel centres, an id per pixel (file, element, triangle). Files streamed
one at a time into the 12 buffers. Per viewpoint: (a) triangles with a part in the frustum (not all corners outside one
plane of it); (b) triangles of the elements with at least one pixel in the final image (all their triangles: an
element-level occlusion culling that is exact); (c) distinct triangles with at least one pixel; (d) triangles of the
elements whose box is in the frustum and projects to less than 1 px and less than 4 px (largest side of the projected
box; a box crossing the near plane counts as large). (b) can exceed (a): an element partly in view counts whole.
Viewpoints (world mm, the files' frame):
- outside: 6 around the site's box centre, every 60° (from 15°), 25° above the horizon, at 2 R (R = half the box's
  diagonal, 118.5 m: the bounding sphere fills the vertical field of view);
- inside: candidates in the largest building (the one connected footprint of 2 m cells with at least 3 slab levels:
  4,860 m², slab tops spanning 106 m in height), eye 1.6 m above a slab top in the cell, horizontal, heading from a fixed
  pseudo-random sequence (seed 12345). 24 candidates were drawn in the same pass as the outside views. A candidate was kept when fewer than 1 % of pixels
  were empty (enclosed), fewer than 5 % were nearer than 0.3 m (not inside a wall) and the median depth was above 2 m.
  The first 6 kept, in candidate order.

## Item 1: what decides simplifiability

### Table 1: mesh statistics per file

| file | elements | triangles drawn | stored (unique) | tris/element median / p90 / p99 | share of triangles in elements <100 / 100-1k / 1k-10k / >10k | components/element median / p90 | tris/component median / p90 | tris in components <100 tris | tris in closed components | coplanar neighbour 0.1° / 1° | after lossless coplanar merge | border vertices |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| synthetic | 472 | 0.78 M | 0.00 M | 1936 / 1936 / 1936 | 0.1 / 0.0 / 99.9 / 0.0 % | 1 / 1 | 1936 / 1936 | 0.1 % | 100.0 % | 86.8 / 100.0 % | 100.0 % | 0.0 % |
| #5 | 6510 | 0.33 M | 0.20 M | 12 / 116 / 370 | 32.7 / 46.0 / 14.6 / 6.6 % | 1 / 1 | 12 / 108 | 40.3 % | 83.3 % | 89.0 / 94.6 % | 98.1 % | 0.2 % |
| #7 | 5965 | 0.63 M | 0.30 M | 12 / 108 / 3034 | 15.0 / 22.6 / 55.7 / 6.7 % | 1 / 1 | 12 / 116 | 21.2 % | 73.0 % | 85.0 / 91.0 % | 98.3 % | 4.4 % |
| #11 | 29935 | 8.84 M | 0.27 M | 76 / 776 / 1172 | 7.2 / 75.0 / 17.3 / 0.5 % | 1 / 3 | 60 / 384 | 18.4 % | 44.1 % | 73.2 / 76.9 % | 99.9 % | 2.8 % |
| #14 | 32766 | 35.80 M | 1.34 M | 92 / 7369 / 7371 | 4.8 / 3.4 / 80.2 / 11.6 % | 1 / 7 | 92 / 1342 | 7.2 % | 8.5 % | 27.2 / 32.5 % | 99.7 % | 26.4 % |
| #15 | 26814 | 20.14 M | 7.86 M | 12 / 1493 / 10078 | 2.2 / 4.1 / 75.4 / 18.2 % | 1 / 5 | 44 / 264 | 16.5 % | 63.8 % | 77.1 / 82.1 % | 99.9 % | 2.3 % |
| #16 | 48794 | 20.99 M | 2.85 M | 307 / 844 / 1728 | 6.2 / 81.0 / 12.1 / 0.7 % | 1 / 3 | 192 / 648 | 10.1 % | 71.4 % | 52.8 / 67.5 % | 99.5 % | 0.3 % |
| #17 | 35243 | 9.09 M | 3.14 M | 124 / 474 / 2092 | 14.1 / 63.6 / 21.9 / 0.3 % | 1 / 1 | 124 / 452 | 16.1 % | 77.9 % | 43.7 / 60.9 % | 99.8 % | 1.3 % |
| five largest together | 173552 | 94.86 M | 15.46 M | - | 5.7 / 33.1 / 52.7 / 8.5 % | - | - | 11.7 % | 44.1 % | 49.3 / 57.6 % | 99.7 % | 11.6 % |
| all 16 files | 252172 | 110.46 M | 22.49 M | - | 6.1 / 33.4 / 50.9 / 9.5 % | - | - | 12.9 % | 46.4 % | 53.5 / 62.2 % | 99.7 % | 10.5 % |

### Table 1b: per IFC class, five largest files together (classes with at least 0.5 % of their triangles)

| IFC class | elements | share of triangles | tris/element (mean) | tris in components <100 | closed | coplanar 1° | after lossless merge | edge collapse @20 mm | edge collapse + prune @20 mm | sloppy @20 mm | grid diag=20 mm | edge collapse @100 mm | edge collapse + prune @100 mm | grid diag=100 mm |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| IfcFlowTerminal | 13487 | 36.2 % | 2543 | 4.1 % | 9.2 % | 34.2 % | 99.8 % | 38.4 % | 31.7 % | 3.6 % | 9.6 % | 37.9 % | 0.1 % | 1.6 % |
| IfcFlowFitting | 61916 | 24.2 % | 370 | 7.2 % | 66.7 % | 62.3 % | 99.5 % | 3.7 % | 3.4 % | 35.4 % | 38.4 % | 1.1 % | 0.2 % | 5.7 % |
| IfcBeam | 2737 | 18.8 % | 6505 | 16.0 % | 64.7 % | 81.4 % | 99.9 % | 5.2 % | 5.2 % | 33.2 % | 49.9 % | 1.8 % | 1.9 % | 11.5 % |
| IfcCovering | 19048 | 6.3 % | 311 | 5.4 % | 78.8 % | 63.9 % | 99.9 % | 5.3 % | 5.3 % | 57.0 % | 75.7 % | 1.4 % | 0.5 % | 35.8 % |
| IfcFlowController | 7975 | 5.1 % | 610 | 11.1 % | 17.3 % | 53.0 % | 99.7 % | 25.0 % | 21.8 % | 12.6 % | 20.5 % | 24.2 % | 5.7 % | 3.9 % |
| IfcFlowSegment | 39784 | 3.8 % | 91 | 97.7 % | 99.7 % | 99.5 % | 100.0 % | 17.9 % | 17.9 % | 54.8 % | 81.3 % | 0.1 % | 0.1 % | 14.3 % |
| IfcDistributionControlElement | 1973 | 1.4 % | 661 | 9.9 % | 22.6 % | 64.5 % | 99.8 % | 4.2 % | 4.2 % | 17.9 % | 47.8 % | 0.6 % | 0.0 % | 3.4 % |
| IfcColumn | 710 | 1.0 % | 1295 | 0.4 % | 34.6 % | 89.5 % | 99.9 % | 10.3 % | 10.3 % | 41.0 % | 53.0 % | 5.6 % | 5.6 % | 14.1 % |
| IfcDistributionElement | 201 | 0.7 % | 3346 | 0.2 % | 88.9 % | 67.8 % | 99.0 % | 2.1 % | 2.1 % | 10.8 % | 23.1 % | 0.1 % | 0.1 % | 1.9 % |
| IfcMember | 1709 | 0.7 % | 377 | 5.2 % | 70.3 % | 74.3 % | 99.7 % | 6.2 % | 6.2 % | 26.2 % | 38.1 % | 0.3 % | 0.3 % | 12.1 % |
| IfcBuildingElementProxy | 1879 | 0.6 % | 323 | 18.8 % | 35.4 % | 84.5 % | 99.8 % | 16.4 % | 16.4 % | 41.9 % | 62.1 % | 3.8 % | 0.7 % | 22.2 % |
| IfcWall | 20332 | 0.6 % | 28 | 76.3 % | 61.6 % | 95.6 % | 99.4 % | 35.5 % | 35.5 % | 75.6 % | 80.9 % | 12.3 % | 12.3 % | 55.8 % |
| IfcEnergyConversionDevice | 402 | 0.5 % | 1185 | 13.2 % | 64.2 % | 70.3 % | 99.3 % | 7.0 % | 6.9 % | 18.0 % | 29.4 % | 4.3 % | 3.2 % | 6.3 % |

What Table 1 shows (five large files; the all-16 row is close):
- **Instancing:** 15.46 M triangles stored for 94.86 M drawn (6.1 copies a triangle); #14 stores 1.34 M for 35.80 M
  (26.8). Every per-element cost below (simplification, meshlets) is paid on the stored triangles; drawing pays the
  drawn ones.
- **Elements are mid-sized, not tiny:** 52.7 % of triangles are in elements of 1,000 to 10,000 triangles, 33.1 % in
  100 to 1,000, 8.5 % above 10,000, 5.7 % below 100. The median element has 12 to 307 triangles depending on the file.
- **Small disjoint parts:** 11.7 % of triangles are in connected components of fewer than 100 triangles. Most
  elements are one component (median 1; p90 up to 7, in #14).
- **Closed parts:** 44.1 % of triangles are in closed components; 8.5 % in #14, 63.8 % to 77.9 % in #15 to #17.
  Open borders: 11.6 % of vertices (26.4 % in #14).
- **Coplanar triangles are already minimal:** 49.3 % of triangles have an edge neighbour within 0.1° (57.6 % within
  1°), but the lossless merge that keeps every boundary vertex leaves 99.7 %. A coplanar pair is a quad's two
  triangles: the planar faces have no interior vertices to remove.
- Per class (Table 1b): `IfcFlowTerminal` holds 36.2 % of the triangles, 2,543 per element, 9.2 % closed. With
  `IfcFlowController` (5.1 %) it is one of the two large classes where vertex clustering keeps fewer triangles than
  edge collapse (9.6 % against 38.4 % at 20 mm).

## Item 2: simplification per element

### Table 2: per element (each element alone), triangles remaining (share of drawn), at the error each simplifier reports

| file | E (mm) | edge collapse | edge collapse, borders locked | edge collapse + prune | sloppy | grid, diagonal = E | grid, cell = E | edge collapse error mean / max (mm) |
|---|---|---|---|---|---|---|---|---|
| synthetic | 1 | 83.3 % | 83.3 % | 83.3 % | 100.0 % | 100.0 % | 100.0 % | 1.0 / 1 |
| synthetic | 5 | 50.7 % | 50.7 % | 50.7 % | 100.0 % | 100.0 % | 100.0 % | 5.0 / 5 |
| synthetic | 20 | 8.1 % | 8.1 % | 8.1 % | 91.3 % | 96.7 % | 91.7 % | 19.8 / 20 |
| synthetic | 100 | 1.7 % | 1.7 % | 1.7 % | 20.5 % | 56.0 % | 25.6 % | 97.5 / 98 |
| synthetic | 500 | 0.0 % | 0.0 % | 0.0 % | 100.0 % | 3.2 % | 0.7 % | 461.1 / 461 |
| #5 | 1 | 81.7 % | 81.7 % | 81.7 % | 96.7 % | 98.1 % | 98.1 % | 0.5 / 1 |
| #5 | 5 | 64.5 % | 64.5 % | 64.5 % | 93.6 % | 96.6 % | 94.1 % | 2.6 / 5 |
| #5 | 20 | 54.6 % | 54.6 % | 54.6 % | 82.8 % | 87.6 % | 81.9 % | 10.6 / 20 |
| #5 | 100 | 38.3 % | 38.4 % | 38.3 % | 59.9 % | 67.9 % | 60.1 % | 63.6 / 100 |
| #5 | 500 | 7.8 % | 7.9 % | 7.8 % | 46.1 % | 44.9 % | 35.6 % | 323.6 / 500 |
| #7 | 1 | 60.5 % | 61.6 % | 60.5 % | 97.0 % | 97.9 % | 97.2 % | 0.8 / 1 |
| #7 | 5 | 41.6 % | 42.9 % | 41.6 % | 75.8 % | 87.0 % | 75.9 % | 3.9 / 5 |
| #7 | 20 | 33.1 % | 34.6 % | 33.0 % | 53.5 % | 62.9 % | 55.6 % | 14.6 / 20 |
| #7 | 100 | 22.6 % | 24.1 % | 22.4 % | 73.0 % | 41.9 % | 35.6 % | 61.0 / 100 |
| #7 | 500 | 8.1 % | 10.0 % | 6.6 % | 20.1 % | 23.8 % | 17.5 % | 220.3 / 500 |
| #11 | 1 | 59.7 % | 60.2 % | 59.7 % | 95.6 % | 96.4 % | 95.6 % | 0.9 / 1 |
| #11 | 5 | 21.7 % | 22.5 % | 21.7 % | 76.0 % | 87.6 % | 77.4 % | 4.7 / 5 |
| #11 | 20 | 6.4 % | 7.9 % | 6.3 % | 25.8 % | 52.4 % | 34.9 % | 17.8 / 20 |
| #11 | 100 | 1.4 % | 2.9 % | 0.2 % | 48.8 % | 9.1 % | 4.5 % | 52.1 / 100 |
| #11 | 500 | 1.2 % | 2.7 % | 0.0 % | 2.7 % | 2.0 % | 1.9 % | 61.0 / 495 |
| #14 | 1 | 48.7 % | 56.9 % | 48.7 % | 69.2 % | 86.8 % | 69.3 % | 0.9 / 1 |
| #14 | 5 | 42.6 % | 51.3 % | 42.5 % | 17.8 % | 28.8 % | 16.9 % | 4.8 / 5 |
| #14 | 20 | 40.5 % | 49.1 % | 33.6 % | 4.9 % | 8.6 % | 5.2 % | 11.3 / 20 |
| #14 | 100 | 39.5 % | 48.2 % | 0.9 % | 4.6 % | 1.5 % | 1.3 % | 20.0 / 100 |
| #14 | 500 | 39.5 % | 48.2 % | 0.1 % | 9.7 % | 1.2 % | 1.2 % | 23.8 / 486 |
| #15 | 1 | 65.3 % | 65.7 % | 65.3 % | 99.5 % | 99.1 % | 99.0 % | 1.0 / 1 |
| #15 | 5 | 31.8 % | 32.3 % | 31.8 % | 73.7 % | 89.9 % | 78.1 % | 4.9 / 5 |
| #15 | 20 | 7.0 % | 7.8 % | 7.0 % | 35.2 % | 51.0 % | 32.9 % | 19.5 / 20 |
| #15 | 100 | 2.4 % | 3.5 % | 2.4 % | 8.5 % | 13.7 % | 7.2 % | 94.3 / 100 |
| #15 | 500 | 1.2 % | 2.4 % | 1.2 % | 4.0 % | 2.8 % | 1.6 % | 302.2 / 500 |
| #16 | 1 | 40.6 % | 40.7 % | 40.6 % | 98.5 % | 99.3 % | 98.0 % | 0.9 / 1 |
| #16 | 5 | 13.4 % | 13.5 % | 13.4 % | 68.1 % | 81.8 % | 68.0 % | 4.5 / 5 |
| #16 | 20 | 3.9 % | 4.0 % | 3.9 % | 38.5 % | 42.3 % | 26.6 % | 12.7 / 20 |
| #16 | 100 | 0.6 % | 0.7 % | 0.3 % | 45.8 % | 8.8 % | 6.0 % | 35.2 / 100 |
| #16 | 500 | 0.6 % | 0.7 % | 0.0 % | 7.2 % | 3.1 % | 2.7 % | 38.8 / 497 |
| #17 | 1 | 49.9 % | 50.3 % | 49.9 % | 97.7 % | 98.8 % | 97.5 % | 0.9 / 1 |
| #17 | 5 | 16.5 % | 17.1 % | 16.5 % | 79.4 % | 91.1 % | 80.9 % | 4.6 / 5 |
| #17 | 20 | 5.7 % | 6.4 % | 5.2 % | 42.3 % | 57.3 % | 38.7 % | 16.8 / 20 |
| #17 | 100 | 2.0 % | 2.7 % | 0.4 % | 46.6 % | 18.2 % | 12.4 % | 47.8 / 100 |
| #17 | 500 | 1.8 % | 2.5 % | 0.1 % | 13.4 % | 5.4 % | 4.1 % | 60.3 / 497 |
| five largest together | 1 | 51.6 % | 54.9 % | 51.6 % | 87.3 % | 94.2 % | 87.1 % | 0.9 / 1 |
| five largest together | 5 | 29.4 % | 32.9 % | 29.4 % | 52.1 % | 65.0 % | 53.0 % | 4.7 / 5 |
| five largest together | 20 | 18.8 % | 22.5 % | 16.1 % | 24.3 % | 33.8 % | 21.8 % | 14.5 / 20 |
| five largest together | 100 | 15.9 % | 19.6 % | 1.0 % | 22.7 % | 8.0 % | 5.0 % | 44.8 / 100 |
| five largest together | 500 | 15.6 % | 19.3 % | 0.3 % | 7.7 % | 2.4 % | 2.0 % | 93.2 / 500 |
| all 16 files | 1 | 52.4 % | 55.3 % | 52.4 % | 88.8 % | 94.8 % | 88.5 % | 0.9 / 1 |
| all 16 files | 5 | 30.1 % | 33.4 % | 30.1 % | 55.7 % | 67.8 % | 56.0 % | 4.6 / 5 |
| all 16 files | 20 | 19.0 % | 22.4 % | 16.7 % | 27.6 % | 37.0 % | 24.8 % | 14.9 / 20 |
| all 16 files | 100 | 15.1 % | 18.6 % | 2.2 % | 23.3 % | 10.4 % | 6.9 % | 49.1 / 100 |
| all 16 files | 500 | 14.0 % | 17.5 % | 0.7 % | 10.6 % | 3.5 % | 2.8 % | 120.9 / 500 |

What Table 2 shows (five large files, per element, at the error each simplifier states):
- At 20 mm: edge collapse keeps **18.8 %**, with prune 16.1 %, with locked borders 22.5 %; sloppy 24.3 %; the grid
  keeps 33.8 % at diagonal = E and 21.8 % at cell = E.
- At 1 mm, edge collapse removes 48.4 % of the triangles (51.6 % left); the grid removes 5.8 % to 12.9 %.
- Edge collapse stops near 15.6 % to 15.9 % from 100 mm up: it keeps the topology, and in `IfcFlowTerminal` (38 % left
  at any error) it cannot go further. With `meshopt_SimplifyPrune` the small parts go: 1.0 % at 100 mm, 0.3 % at 500 mm.
- Per class (Table 1b), at 20 mm edge collapse keeps less than the grid at diagonal = E for every class except
  `IfcFlowTerminal` and `IfcFlowController`: for example `IfcBeam` 5.2 % against 49.9 %, `IfcFlowFitting` 3.7 %
  against 38.4 %, `IfcCovering` 5.3 % against 75.7 %.
- "The error each simplifier states" is not the same quantity for each: meshoptimizer reports its quadric error, the
  grid its vertex bound. Item 3 bis measures one deviation for all of them.

## Item 3: simplification per region (element boundaries dropped)

### Table 3: per region (elements merged, at most 40 k drawn triangles a region), triangles remaining

| file | E (mm) | edge collapse | edge collapse, borders locked | edge collapse + prune | sloppy | grid, diagonal = E | grid, cell = E | edge collapse error mean / max (mm) |
|---|---|---|---|---|---|---|---|---|
| synthetic | 1 | 82.8 % | 82.8 % | 82.8 % | 100.0 % | 100.0 % | 100.0 % | 1.0 / 1 |
| synthetic | 5 | 46.7 % | 46.7 % | 46.7 % | 99.7 % | 100.0 % | 100.0 % | 5.0 / 5 |
| synthetic | 20 | 8.4 % | 8.4 % | 8.4 % | 93.0 % | 97.4 % | 93.4 % | 19.9 / 20 |
| synthetic | 100 | 1.5 % | 1.5 % | 1.5 % | 23.4 % | 61.2 % | 23.0 % | 99.4 / 100 |
| synthetic | 500 | 0.0 % | 0.0 % | 0.0 % | 0.9 % | 3.7 % | 1.4 % | 492.3 / 500 |
| #5 | 1 | 96.4 % | 96.4 % | 96.4 % | 98.4 % | 96.3 % | 96.2 % | 1.0 / 1 |
| #5 | 5 | 75.7 % | 75.7 % | 75.7 % | 98.4 % | 95.1 % | 93.0 % | 5.0 / 5 |
| #5 | 20 | 55.0 % | 55.0 % | 55.0 % | 95.2 % | 87.9 % | 83.3 % | 19.9 / 20 |
| #5 | 100 | 38.3 % | 38.4 % | 38.3 % | 61.4 % | 67.8 % | 59.6 % | 99.6 / 100 |
| #5 | 500 | 6.1 % | 6.2 % | 6.1 % | 29.5 % | 41.3 % | 23.2 % | 495.2 / 500 |
| #7 | 1 | 93.3 % | 93.4 % | 93.3 % | 98.9 % | 97.4 % | 96.5 % | 1.0 / 1 |
| #7 | 5 | 51.8 % | 52.4 % | 51.4 % | 98.9 % | 87.1 % | 75.6 % | 5.0 / 5 |
| #7 | 20 | 32.7 % | 34.2 % | 32.7 % | 82.6 % | 60.3 % | 53.5 % | 19.8 / 20 |
| #7 | 100 | 22.4 % | 23.9 % | 22.2 % | 36.2 % | 41.8 % | 35.4 % | 98.5 / 100 |
| #7 | 500 | 6.9 % | 8.8 % | 5.7 % | 15.6 % | 22.6 % | 12.9 % | 487.1 / 500 |
| #11 | 1 | 68.2 % | 68.4 % | 68.2 % | 100.0 % | 96.5 % | 95.5 % | 1.0 / 1 |
| #11 | 5 | 21.9 % | 22.8 % | 21.9 % | 97.0 % | 87.4 % | 76.9 % | 5.0 / 5 |
| #11 | 20 | 6.6 % | 8.1 % | 6.5 % | 33.3 % | 49.4 % | 31.2 % | 19.9 / 20 |
| #11 | 100 | 1.4 % | 2.9 % | 0.2 % | 4.4 % | 8.5 % | 3.8 % | 95.8 / 100 |
| #11 | 500 | 1.3 % | 2.7 % | 0.0 % | 1.2 % | 0.7 % | 0.3 % | 233.2 / 497 |
| #14 | 1 | 52.3 % | 58.9 % | 52.3 % | 96.9 % | 87.3 % | 68.7 % | 1.0 / 1 |
| #14 | 5 | 41.4 % | 50.1 % | 41.2 % | 38.7 % | 28.9 % | 16.7 % | 5.0 / 5 |
| #14 | 20 | 39.7 % | 48.4 % | 33.0 % | 4.0 % | 6.5 % | 3.3 % | 19.6 / 20 |
| #14 | 100 | 39.4 % | 48.2 % | 1.0 % | 1.3 % | 0.7 % | 0.3 % | 59.7 / 100 |
| #14 | 500 | 39.4 % | 48.2 % | 0.1 % | 54.2 % | 0.0 % | 0.0 % | 148.6 / 500 |
| #15 | 1 | 67.9 % | 68.2 % | 67.9 % | 100.0 % | 98.8 % | 98.7 % | 1.0 / 1 |
| #15 | 5 | 31.6 % | 32.1 % | 31.6 % | 79.1 % | 90.1 % | 77.7 % | 5.0 / 5 |
| #15 | 20 | 7.3 % | 8.1 % | 7.3 % | 32.9 % | 50.6 % | 31.4 % | 19.9 / 20 |
| #15 | 100 | 2.6 % | 3.6 % | 2.6 % | 8.0 % | 11.2 % | 7.4 % | 99.0 / 100 |
| #15 | 500 | 1.3 % | 2.4 % | 1.3 % | 1.3 % | 2.4 % | 1.2 % | 454.7 / 500 |
| #16 | 1 | 45.2 % | 45.3 % | 45.2 % | 99.9 % | 99.3 % | 98.2 % | 1.0 / 1 |
| #16 | 5 | 12.4 % | 12.5 % | 12.4 % | 74.1 % | 82.2 % | 64.6 % | 5.0 / 5 |
| #16 | 20 | 3.2 % | 3.3 % | 3.2 % | 26.7 % | 37.1 % | 23.0 % | 19.9 / 20 |
| #16 | 100 | 0.6 % | 0.7 % | 0.5 % | 5.0 % | 8.0 % | 4.4 % | 92.3 / 100 |
| #16 | 500 | 0.5 % | 0.7 % | 0.1 % | 6.4 % | 0.6 % | 0.2 % | 182.8 / 500 |
| #17 | 1 | 55.1 % | 55.3 % | 55.1 % | 100.0 % | 97.8 % | 96.1 % | 1.0 / 1 |
| #17 | 5 | 15.9 % | 16.5 % | 15.9 % | 89.6 % | 90.0 % | 78.1 % | 5.0 / 5 |
| #17 | 20 | 4.7 % | 5.4 % | 4.2 % | 39.6 % | 54.0 % | 37.9 % | 19.8 / 20 |
| #17 | 100 | 1.8 % | 2.5 % | 0.6 % | 10.7 % | 16.0 % | 9.1 % | 93.1 / 100 |
| #17 | 500 | 1.6 % | 2.3 % | 0.3 % | 0.7 % | 1.7 % | 0.4 % | 329.4 / 499 |
| five largest together | 1 | 55.8 % | 58.4 % | 55.8 % | 98.8 % | 94.3 % | 86.7 % | 1.0 / 1 |
| five largest together | 5 | 28.6 % | 32.2 % | 28.6 % | 65.4 % | 65.0 % | 51.7 % | 5.0 / 5 |
| five largest together | 20 | 18.3 % | 22.0 % | 15.7 % | 21.3 % | 31.2 % | 19.5 % | 19.7 / 20 |
| five largest together | 100 | 15.9 % | 19.6 % | 1.1 % | 4.7 % | 6.8 % | 3.9 % | 81.8 / 100 |
| five largest together | 500 | 15.5 % | 19.3 % | 0.3 % | 22.3 % | 0.9 % | 0.4 % | 246.4 / 500 |
| all 16 files | 1 | 58.6 % | 60.9 % | 58.6 % | 98.9 % | 94.8 % | 88.1 % | 1.0 / 1 |
| all 16 files | 5 | 29.6 % | 32.8 % | 29.5 % | 69.8 % | 67.9 % | 55.1 % | 5.0 / 5 |
| all 16 files | 20 | 18.5 % | 21.9 % | 16.2 % | 26.0 % | 34.7 % | 22.9 % | 19.7 / 20 |
| all 16 files | 100 | 14.8 % | 18.3 % | 2.1 % | 7.0 % | 9.2 % | 5.7 % | 84.1 / 100 |
| all 16 files | 500 | 13.9 % | 17.4 % | 0.7 % | 19.9 % | 1.8 % | 0.9 % | 277.9 / 500 |

What Table 3 shows (five large files, regions, at the stated error):
- **Dropping element boundaries changes almost nothing.** Edge collapse on regions keeps 18.3 % at 20 mm against
  18.8 % per element; 28.6 % against 29.4 % at 5 mm; the same 15.9 % at 100 mm; 55.8 % against 51.6 % at 1 mm (worse
  merged). The grid gains 2 to 3 points (31.2 % against 33.8 % at 20 mm, diagonal = E).
- At 20 mm: edge collapse 18.3 %, with prune 15.7 %, with locked borders 22.0 %; sloppy 21.3 %; grid 31.2 %
  (diagonal = E) and 19.5 % (cell = E).

### Item 3 bis: the measured deviation (one metric for every simplifier)

The errors in Tables 2 and 3 are each simplifier's own statement. To compare at the same error, the deviation was
measured on a sample of the regions of the five large files: every k-th region in partition order (9 to 12 regions a
file: #11: 10 of 335 regions, 277,070 triangles; #14: 10 of 1351 regions, 227,601 triangles; #15: 12 of 949 regions, 285,147 triangles; #16: 10 of 799 regions, 272,611 triangles; #17: 9 of 331 regions, 275,850 triangles). In each region, up to 20,000 points of the original surface (welded vertices and triangle
centroids, every n-th) are taken; a point's deviation is its distance to the nearest triangle of the simplified mesh
(exact point-to-triangle distance, a hierarchical grid of the triangles, checked against brute force on 910 points of
file #7: identical to 0.0001 mm). A point with no triangle within 8 E is "vanished" (its part was removed). Grid clusters are
placed at the mean of their vertices (not at the quadric minimum of `tiles.c`). Edge collapse was also run with a
target of E/2 and E/4, sloppy with E/2, the grid with cells E/√3, E, 2E and 4E.

Each cell: triangles left (share of the region's welded triangles) · p99 of the deviation in mm · share of points
farther than E. Rows "five": triangles left · share of points farther than E · share vanished (shares weighted by the regions'
triangles; p99 is not combined across files).

| file | E | edge collapse, target E/4 | edge collapse, target E/2 | edge collapse, target E | edge collapse + prune, target E | sloppy, target E/2 | sloppy, target E | grid, cell E/√3 | grid, cell E | grid, cell 2E | grid, cell 4E |
|---|---|---|---|---|---|---|---|---|---|---|---|
| #11 | 5 | 63.5 % · 2 · 0.0 % | 42.4 % · 4 · 0.2 % | 24.8 % · 7 · 3.6 % | 24.8 % · 7 · 3.6 % | 52.0 % · 10 · 3.3 % | 52.0 % · 10 · 3.3 % | 88.3 % · 1 · 0.0 % | 78.7 % · 1 · 0.0 % | 60.4 % · 3 · 0.0 % | 36.9 % · 7 · 4.2 % |
| #11 | 20 | 24.8 % · 7 · 0.1 % | 14.6 % · 14 · 0.3 % | 8.9 % · 29 · 4.0 % | 8.7 % · 29 · 4.0 % | 51.1 % · 10 · 0.1 % | 34.3 % · 11 · 0.1 % | 53.3 % · 3 · 0.0 % | 36.9 % · 7 · 0.0 % | 19.2 % · 15 · 0.2 % | 8.6 % · 40 · 12.9 % |
| #11 | 100 | 7.5 % · 79 · 0.9 % | 3.1 % · 800 · 32.5 % | 1.5 % · 800 · 58.5 % | 0.2 % · 800 · 92.6 % | 16.9 % · 19 · 0.0 % | 7.4 % · 70 · 0.8 % | 12.8 % · 25 · 0.1 % | 6.6 % · 800 · 1.7 % | 1.3 % · 800 · 36.4 % | 0.4 % · 800 · 63.5 % |
| #14 | 5 | 55.2 % · 1 · 0.0 % | 49.8 % · 2 · 0.0 % | 44.5 % · 5 · 0.7 % | 44.3 % · 5 · 0.7 % | 22.0 % · 8 · 1.7 % | 19.9 % · 8 · 1.7 % | 34.8 % · 1 · 0.0 % | 23.4 % · 2 · 0.0 % | 15.3 % · 4 · 0.4 % | 11.6 % · 10 · 16.8 % |
| #14 | 20 | 44.5 % · 5 · 0.0 % | 41.8 % · 10 · 0.1 % | 40.4 % · 160 · 4.6 % | 34.2 % · 160 · 5.2 % | 13.8 % · 8 · 0.1 % | 10.8 % · 17 · 0.2 % | 14.2 % · 5 · 0.0 % | 11.6 % · 10 · 0.0 % | 8.5 % · 160 · 3.9 % | 5.1 % · 160 · 31.3 % |
| #14 | 100 | 40.0 % · 483 · 2.1 % | 38.4 % · 800 · 4.2 % | 37.3 % · 800 · 9.8 % | 0.1 % · 800 · 96.6 % | 7.3 % · 800 · 14.6 % | 4.2 % · 800 · 37.2 % | 6.6 % · 800 · 15.6 % | 3.9 % · 800 · 48.6 % | 1.7 % · 800 · 66.6 % | 0.6 % · 800 · 86.5 % |
| #15 | 5 | 63.8 % · 2 · 0.0 % | 51.0 % · 3 · 0.0 % | 33.9 % · 7 · 3.1 % | 33.9 % · 7 · 3.1 % | 67.5 % · 4 · 0.3 % | 67.5 % · 4 · 0.3 % | 91.6 % · 0 · 0.0 % | 79.8 % · 2 · 0.0 % | 59.3 % · 3 · 0.0 % | 35.7 % · 7 · 8.6 % |
| #15 | 20 | 33.9 % · 7 · 0.1 % | 19.7 % · 13 · 0.4 % | 9.0 % · 94 · 48.0 % | 9.0 % · 94 · 48.0 % | 57.5 % · 4 · 0.0 % | 35.5 % · 8 · 0.0 % | 54.6 % · 4 · 0.0 % | 35.7 % · 7 · 0.0 % | 19.5 % · 64 · 2.7 % | 11.5 % · 94 · 26.5 % |
| #15 | 100 | 7.2 % · 103 · 3.0 % | 4.5 % · 107 · 3.7 % | 2.5 % · 406 · 7.9 % | 2.5 % · 406 · 8.5 % | 17.5 % · 91 · 0.0 % | 10.0 % · 95 · 0.5 % | 15.1 % · 93 · 0.1 % | 9.4 % · 95 · 0.5 % | 3.4 % · 108 · 1.6 % | 1.4 % · 190 · 5.3 % |
| #16 | 5 | 41.6 % · 2 · 0.0 % | 25.4 % · 3 · 0.0 % | 12.5 % · 7 · 3.4 % | 12.5 % · 7 · 3.4 % | 62.5 % · 4 · 0.7 % | 58.6 % · 4 · 0.7 % | 82.2 % · 1 · 0.0 % | 64.8 % · 1 · 0.0 % | 41.6 % · 3 · 0.0 % | 22.9 % · 9 · 4.5 % |
| #16 | 20 | 12.5 % · 7 · 0.3 % | 6.2 % · 19 · 0.9 % | 3.2 % · 160 · 8.1 % | 3.2 % · 160 · 8.1 % | 43.0 % · 5 · 0.0 % | 25.3 % · 10 · 0.0 % | 37.4 % · 4 · 0.0 % | 22.9 % · 9 · 0.0 % | 11.2 % · 15 · 0.6 % | 5.1 % · 160 · 18.6 % |
| #16 | 100 | 2.5 % · 434 · 1.8 % | 1.0 % · 800 · 16.8 % | 0.6 % · 800 · 57.7 % | 0.5 % · 800 · 66.3 % | 9.8 % · 22 · 0.1 % | 4.5 % · 187 · 2.8 % | 7.5 % · 27 · 0.5 % | 4.0 % · 219 · 1.4 % | 1.5 % · 800 · 12.2 % | 0.3 % · 800 · 48.2 % |
| #17 | 5 | 46.6 % · 2 · 0.0 % | 27.2 % · 3 · 0.1 % | 16.1 % · 6 · 3.2 % | 16.1 % · 6 · 3.2 % | 60.6 % · 4 · 0.2 % | 58.8 % · 4 · 0.2 % | 90.6 % · 0 · 0.0 % | 77.1 % · 1 · 0.0 % | 55.1 % · 3 · 0.0 % | 33.4 % · 7 · 4.5 % |
| #17 | 20 | 16.1 % · 6 · 0.0 % | 8.9 % · 12 · 0.1 % | 3.6 % · 160 · 13.7 % | 3.6 % · 160 · 13.8 % | 52.6 % · 4 · 0.0 % | 34.4 % · 7 · 0.0 % | 50.6 % · 3 · 0.0 % | 33.4 % · 7 · 0.0 % | 17.6 % · 18 · 0.5 % | 8.2 % · 124 · 17.1 % |
| #17 | 100 | 2.7 % · 800 · 10.9 % | 1.2 % · 800 · 27.7 % | 0.7 % · 800 · 57.0 % | 0.5 % · 800 · 63.1 % | 15.0 % · 27 · 0.1 % | 8.0 % · 800 · 3.4 % | 12.3 % · 48 · 0.3 % | 6.3 % · 325 · 2.5 % | 2.0 % · 800 · 9.7 % | 0.5 % · 800 · 28.1 % |
| five | 5 | 54.2 % · 0.0 % · 0.0 % | 38.9 % · 0.1 % · 0.0 % | 25.8 % · 2.9 % · 0.1 % | 25.8 % · 2.9 % · 0.1 % | 54.1 % · 1.2 % · 0.0 % | 52.6 % · 1.2 % · 0.0 % | 79.1 % · 0.0 % · 0.0 % | 66.4 % · 0.0 % · 0.0 % | 47.6 % · 0.1 % · 0.0 % | 28.7 % · 7.4 % · 0.0 % |
| five | 20 | 25.8 % · 0.1 % · 0.0 % | 17.4 % · 0.3 % · 0.1 % | 12.0 % · 16.3 % · 2.4 % | 10.9 % · 16.4 % · 2.5 % | 44.8 % · 0.0 % · 0.0 % | 28.8 % · 0.1 % · 0.0 % | 43.1 % · 0.0 % · 0.0 % | 28.7 % · 0.0 % · 0.0 % | 15.5 % · 1.5 % · 0.6 % | 7.8 % · 21.0 % · 4.1 % |
| five | 100 | 10.9 % · 3.8 % · 1.4 % | 8.6 % · 17.4 % · 9.1 % | 7.4 % · 39.0 % · 17.1 % | 0.8 % · 63.9 % · 39.9 % | 13.5 % · 2.5 % · 0.6 % | 6.9 % · 7.9 % · 3.3 % | 11.1 % · 2.8 % · 0.6 % | 6.2 % · 9.5 % · 5.7 % | 2.0 % · 23.7 % · 16.3 % | 0.7 % · 44.6 % · 30.6 % |

| file | E | fewest triangles with p99 ≤ E: edge collapse (any target) | sloppy (any target) | grid (any cell) |
|---|---|---|---|---|
| #11 | 5 | 42.4 % (edge collapse, target E/2) | none passes | 60.4 % (grid, cell 2E) |
| #11 | 20 | 14.6 % (edge collapse, target E/2) | 34.3 % (sloppy, target E) | 19.2 % (grid, cell 2E) |
| #11 | 100 | 7.5 % (edge collapse, target E/4) | 7.4 % (sloppy, target E) | 12.8 % (grid, cell E/√3) |
| #14 | 5 | 44.3 % (edge collapse + prune, target E) | none passes | 15.3 % (grid, cell 2E) |
| #14 | 20 | 41.8 % (edge collapse, target E/2) | 10.8 % (sloppy, target E) | 11.6 % (grid, cell E) |
| #14 | 100 | none passes | none passes | none passes |
| #15 | 5 | 51.0 % (edge collapse, target E/2) | 67.5 % (sloppy, target E) | 59.3 % (grid, cell 2E) |
| #15 | 20 | 19.7 % (edge collapse, target E/2) | 35.5 % (sloppy, target E) | 35.7 % (grid, cell E) |
| #15 | 100 | none passes | 10.0 % (sloppy, target E) | 9.4 % (grid, cell E) |
| #16 | 5 | 25.4 % (edge collapse, target E/2) | 58.6 % (sloppy, target E) | 41.6 % (grid, cell 2E) |
| #16 | 20 | 6.2 % (edge collapse, target E/2) | 25.3 % (sloppy, target E) | 11.2 % (grid, cell 2E) |
| #16 | 100 | none passes | 9.8 % (sloppy, target E/2) | 7.5 % (grid, cell E/√3) |
| #17 | 5 | 27.2 % (edge collapse, target E/2) | 58.8 % (sloppy, target E) | 55.1 % (grid, cell 2E) |
| #17 | 20 | 8.9 % (edge collapse, target E/2) | 34.4 % (sloppy, target E) | 17.6 % (grid, cell 2E) |
| #17 | 100 | none passes | 15.0 % (sloppy, target E/2) | 12.3 % (grid, cell E/√3) |

| E | fewest triangles with at most 1 % of points farther than E, five files together: edge collapse (any target) | sloppy (any target) | grid (any cell) |
|---|---|---|---|
| 5 | 38.9 % (edge collapse, target E/2; 0.1 % over E) | none | 47.6 % (grid, cell 2E; 0.1 % over E) |
| 20 | 17.4 % (edge collapse, target E/2; 0.3 % over E) | 28.8 % (sloppy, target E; 0.1 % over E) | 28.7 % (grid, cell E; 0.0 % over E) |
| 100 | none | none | none |

What item 3 bis shows:
- **meshoptimizer's stated error is not the surface deviation.** With target E = 20 mm, 16.3 % of the points (five files)
  lie farther than 20 mm from the result (48 % in #15); with E = 100 mm, 39.0 %, and 17.1 % vanished. With target E/2
  the share beyond E falls to 0.3 % at 20 mm. The grid's vertex bound (cell E/√3) holds at 5 and 20 mm (0.0 %) but not
  at 100 mm (2.8 %: thin parts become degenerate and vanish).
- **At the same measured deviation** (at most 1 % of points farther than E), merged regions, five files: at 20 mm edge
  collapse (target E/2) keeps 17.4 %, the grid 28.7 % (cell E) and sloppy 28.8 %: the grid keeps 1.65 times edge
  collapse's triangles. At 5 mm: 38.9 % against 47.6 % (1.22 times). At 100 mm no simplifier keeps 99 % of the points
  within 100 mm (last table).
- The ranking flips per file: in #14 (dense `IfcFlowTerminal` meshes, 26 % open-border vertices) the grid and sloppy
  keep 11 % at 20 mm where edge collapse keeps 42 %; in #11, #15, #16 and #17 edge collapse keeps 0.51 to 0.76
  times the grid's triangles at 20 mm (per-file table, p99 ≤ E).

### Table 3c: timing (single thread, seconds per million input triangles)

| file | weld | topology stats | edge collapse | edge collapse, borders locked | edge collapse + prune | sloppy | grid, diagonal = E | grid, cell = E | regions: edge collapse | sloppy | grid, diagonal = E |
|---|---|---|---|---|---|---|---|---|---|---|---|
| synthetic | 0.000 | 0.000 | 0.169 | 0.164 | 0.175 | 0.025 | 0.019 | 0.019 | 0.191 | 0.030 | 0.024 |
| #5 | 0.000 | 0.408 | 0.211 | 0.204 | 0.213 | 0.036 | 0.026 | 0.021 | 0.327 | 0.042 | 0.028 |
| #7 | 0.000 | 0.430 | 0.241 | 0.230 | 0.241 | 0.035 | 0.022 | 0.021 | 0.372 | 0.030 | 0.022 |
| #11 | 0.037 | 0.476 | 0.224 | 0.179 | 0.237 | 0.029 | 0.021 | 0.018 | 0.306 | 0.031 | 0.021 |
| #14 | 0.030 | 0.568 | 0.502 | 0.418 | 0.502 | 0.037 | 0.035 | 0.029 | 0.308 | 0.022 | 0.016 |
| #15 | 0.014 | 0.422 | 0.256 | 0.251 | 0.267 | 0.031 | 0.019 | 0.016 | 0.286 | 0.032 | 0.021 |
| #16 | 0.014 | 0.484 | 0.228 | 0.216 | 0.228 | 0.029 | 0.022 | 0.019 | 0.302 | 0.030 | 0.021 |
| #17 | 0.013 | 0.478 | 0.220 | 0.209 | 0.219 | 0.028 | 0.021 | 0.018 | 0.310 | 0.031 | 0.022 |

## Item 4: meshlets and cone culling (file #14)

File #14: 1,244 unique meshes (1.33 M triangles stored after the weld) drawn as 32,766 elements (35.80 M triangles).

| measure | value |
|---|---|
| meshlets (64 v / 124 t, cone weight 0.25) | 15,237 for the stored meshes; 397,811 drawn (copies counted) |
| triangles per meshlet | 87.6 mean; meshlets with < 32 / 32–63 / 64–95 / 96–124 triangles: 46 / 859 / 9,792 / 4,540 |
| build time | 0.53 s (0.40 s per million triangles); bounds 0.05 s; codecs 0.28 s |
| cache bytes, BMG1, same meshes | 8,066,364 positions and indices; 15,320,841 with the edge lines |
| meshlets raw + f32 positions | 8,038,076 + 8,882,700 = 16,920,776 |
| meshlet codec + vertex codec (lattice int32, lossless) | 3,557,188 + 2,534,765 = **6,091,953** (0.76 × BMG1 positions and indices) |
| index codec + vertex codec (no meshlets, for reference) | 2,706,261 + 2,534,765 = 5,241,026 (0.65 ×) |

Cone culling and back faces, all 397,811 meshlets and 35.80 M triangles of #14 (no frustum), 17 viewpoints: the 6 site
views of item 5, 4 around #14's own box (every 90° from 45°, 25° up, 2 R) and the 7 inside viewpoints that passed the
check of item 5.

| viewpoints | meshlets rejected by their cone | triangles in rejected meshlets | triangles back facing (upper bound of any back-face scheme) |
|---|---|---|---|
| 10 outside | 0.2 % to 0.5 % | 0.2 % to 0.4 % | 48.7 % to 49.3 % |
| 7 inside | 0.2 % to 0.3 % | 0.2 % to 0.3 % | 49.6 % to 50.7 % |
| synthetic scene, 6 outside | 25.3 % to 29.7 % | 23.5 % to 29.9 % | 50.4 % |
| synthetic scene, 6 inside | 18.3 % to 20.4 % | 15.5 % to 19.7 % | 52.2 % to 52.8 % |

Cone test time: 5.8 s for 17 viewpoints (meshlet cones and every triangle's normal, one thread).

## Item 5: what a view needs

The 15 georeferenced files (#3 to #17), 110,453,658 triangles, 2984 × 1580, 60° vertical field of view. Rasterising the
30 viewpoints (6 outside, 24 inside candidates) streamed file by file took 118 s (one thread, loads included).
Inside candidates kept (fewer than 1 % of pixels empty, fewer than 5 % nearer than 0.3 m, median depth over 2 m): 7 of
24; the first 6 are below. A visual check of the id buffer at 746 × 395 (files #3 to #13) showed a corridor, a large
room and walls at a few metres; two of them (in01, in16) face a large element that fills most of the image.

| viewpoint | (a) in frustum | (b) elements with a visible pixel: their triangles | (c) triangles with a visible pixel | (d) elements < 1 px | (d) elements < 4 px | visible elements | pixels empty | median depth |
|---|---|---|---|---|---|---|---|---|
| outside out0 | 110.45 M (100.00 %) | 1.27 M (1.15 %) | 14,320 (0.013 %) | 48.96 M (44.33 %) | 69.59 M (63.01 %) | 1,041 | 94.3 % | 217.8 m |
| outside out1 | 110.45 M (100.00 %) | 1.28 M (1.16 %) | 12,529 (0.011 %) | 50.13 M (45.38 %) | 71.00 M (64.28 %) | 854 | 94.3 % | 223.9 m |
| outside out2 | 110.45 M (100.00 %) | 1.36 M (1.23 %) | 10,332 (0.009 %) | 48.34 M (43.77 %) | 71.84 M (65.04 %) | 1,010 | 94.7 % | 220.4 m |
| outside out3 | 110.45 M (100.00 %) | 1.87 M (1.69 %) | 12,481 (0.011 %) | 51.22 M (46.38 %) | 72.71 M (65.83 %) | 1,407 | 94.2 % | 226.6 m |
| outside out4 | 110.45 M (100.00 %) | 1.85 M (1.68 %) | 13,641 (0.012 %) | 49.91 M (45.19 %) | 69.44 M (62.87 %) | 1,590 | 94.7 % | 232.9 m |
| outside out5 | 110.45 M (100.00 %) | 1.82 M (1.64 %) | 15,720 (0.014 %) | 47.20 M (42.73 %) | 68.86 M (62.34 %) | 1,645 | 94.9 % | 217.0 m |
| inside in01 | 21.31 M (19.29 %) | 16,120 (0.015 %) | 237 (0.000 %) | 6,296 (0.006 %) | 6.64 M (6.01 %) | 27 | 0.0 % | 6.7 m |
| inside in04 | 20,578 (0.019 %) | 0.23 M (0.21 %) | 304 (0.000 %) | 0 (0.000 %) | 0 (0.000 %) | 28 | 0.0 % | 3.7 m |
| inside in06 | 37.68 M (34.12 %) | 6,324 (0.006 %) | 74 (0.000 %) | 10,068 (0.009 %) | 11.85 M (10.73 %) | 17 | 0.0 % | 9.0 m |
| inside in14 | 5.19 M (4.70 %) | 0.29 M (0.26 %) | 2,789 (0.003 %) | 0 (0.000 %) | 24,553 (0.022 %) | 34 | 0.0 % | 2.0 m |
| inside in16 | 24.02 M (21.75 %) | 9,639 (0.009 %) | 377 (0.000 %) | 27,884 (0.025 %) | 6.95 M (6.30 %) | 36 | 0.0 % | 7.3 m |
| inside in18 | 8.93 M (8.08 %) | 89,357 (0.081 %) | 216 (0.000 %) | 7,892 (0.007 %) | 2.94 M (2.67 %) | 22 | 0.0 % | 2.5 m |

Shares are of the 110.45 M triangles of the 15 files. The site fills 5 to 6 % of the image from the outside viewpoints
(the bounding sphere fills the field of view; one pixel is about 0.16 m at 220 m). In (d), a box that crosses the near
plane counts as large.

Synthetic scene (775,264 triangles, same method): outside, (a) 100 %, (b) 50.0 % to 88.0 %, (c) 9.5 % to 20.3 %, (d) 0;
inside (eye 1.6 m under the first floor, among the spheres), (a) 16.8 % to 80.7 %, (b) 17.8 % to 77.4 %, (c) 3.7 % to
10.2 %, (d) 0. The method gives large visible shares when the scene is
open: the small shares of the local folder come from the data, not from the rasteriser.

## Conclusion: what the numbers show

Triangle shares are of the triangles drawn. "Five" is files #11, #14, #15, #16, #17 (94.86 M of the 110.46 M
triangles); item 5 uses the 15 georeferenced files (110.45 M). Times are one thread on this machine.

| technique | measured triangle reduction on this data | measured cost | verdict |
|---|---|---|---|
| **Edge collapse per element** (each element simplified alone, stays selectable) | At the stated error: 48.4 % removed at 1 mm, 70.6 % at 5 mm, 81.2 % at 20 mm (five). Edge collapse keeps less than the grid in every large IFC class but `IfcFlowTerminal` and `IfcFlowController` (36.2 % and 5.1 % of the triangles). The measured surface deviation exceeds the stated error (item 3 bis, on regions); per element it was not measured. | 0.21 to 0.50 s per million stored triangles and level (15.46 M stored in the five files: 3 to 8 s a level) | **Pays** as a per-element LOD for the classes other than `IfcFlowTerminal` and `IfcFlowController`; target E/2 to hold the error measured on regions. Undecided for the error per element: needs item 3 bis on single elements. |
| **Edge collapse on merged regions** (what a cluster hierarchy adds: element boundaries dropped) | Against per element: −0.5 points at 20 mm, −0.8 at 5 mm, 0 at 100 mm, +4.2 at 1 mm (worse). Against the grid at the same measured deviation (at most 1 % of points beyond E, sampled regions): 0.61 times the grid's triangles at 20 mm, 0.82 at 5 mm; at 100 mm no simplifier holds the bound. | 0.29 to 0.37 s per million drawn triangles and level, against 0.02 for the grid (15 times); 94.86 M drawn: about 30 s a level against 2 s | **Does not pay** as a cluster hierarchy: crossing element boundaries removes nothing more. As the simplifier of coarse tiles in place of the grid: **pays at 5 and 20 mm** (18 % and 39 % fewer triangles than the grid, five files; loses on #14, where the grid keeps 0.28 times as many at 20 mm), **undecided at 100 mm and above**: needs a deviation rule for vanished parts (the tile builder's rule: a vanished part counts its own size), then the same measurement. |
| **Meshlets + cone culling** | Cone culling: 0.2 % to 0.5 % of #14's meshlets (0.2 % to 0.4 % of its triangles), 17 viewpoints; 25 % to 30 % on the synthetic spheres. Triangles back facing: 48.7 % to 50.7 % (the bound of any back-face scheme; per triangle, as the GPU's own culling). | Build 0.40 s per million triangles; bytes with meshoptimizer's codecs 0.76 times BMG1's positions and indices | **Does not pay** (cone culling). Meshlets as a codec: 24 % fewer bytes than BMG1 for positions and indices; a plain index codec 35 % fewer. |
| **Lossless coplanar merge** (no point moves, boundaries kept) | 0.3 % (five files). 49.3 % of triangles have a coplanar neighbour, but as the two triangles of a quad. | Topology statistics 0.42 to 0.57 s per million stored triangles | **Does not pay.** Near-lossless is different: edge collapse with target 1.25 mm keeps 54.2 % of region triangles with 0.0 % of the sampled points beyond 5 mm. |
| **Element-level occlusion** (draw only elements with a visible pixel) | Outside: the visible elements hold 1.15 % to 1.69 % of the triangles (98.3 % to 98.9 % removed). Inside: 6,324 to 287,756 triangles against 0.02 M to 37.7 M in the frustum. Triangles with a visible pixel: 10,332 to 15,720 outside, 74 to 2,789 inside. | Not measured at runtime. The exact visibility here (a CPU z-buffer of everything) took about 4 s a view: a bound, not a method. | **Pays** in triangles: the largest reduction measured. Cost **undecided**: needs a runtime method (occluders, Hi-Z, queries) measured on the GPU. |
| **Contribution culling** (drop elements whose box projects under N px) | Outside: < 1 px 42.7 % to 46.4 %, < 4 px 62.3 % to 65.8 % of the triangles. Inside: < 1 px under 0.03 %, < 4 px 0 % to 10.7 %. | One box projection per element (251,661 elements in the 15 files) | **Pays** for site views; small inside. |

Ranking on this data, outside views: element-level occlusion leaves 1.2 % to 1.7 %, simplification of every element at a stated
100 mm (about 0.6 px at 220 m) leaves 1.0 % (edge collapse + prune, per element) to 8.0 % (grid), contribution
culling under 4 px leaves 34 % to 38 %. Inside views: occlusion removes almost everything in the frustum; contribution
culling removes 0 % to 10.7 %; LOD per viewpoint was not measured. The combined effect of the three was not measured.

## What was not measured

- File #1 (no cache in the set) and, in item 5, file #2 (its own local frame; 6,132 triangles).
- The tile branch's own coarse levels (quadric placement, per material family, its sampled error, `cc/tile1`): the
  grid here is a plain reimplementation; its counts are not the tile files' counts.
- The measured deviation per element (item 3 bis is on regions) and on all regions (9 to 12 regions a file sampled).
- A full Nanite-style DAG (grouping, simplifying and splitting clusters level by level): only its ingredients
  (cross-element simplification, item 3; meshlets and cones, item 4).
- Anything on the GPU: frame times, occlusion culling at runtime, LOD switching, the cost of meshlets on the GPU.
- Whether back faces can be culled: 44.1 % of triangles are in closed components (five files); open meshes need both
  faces.

The scratch code and its raw outputs were deleted after this document; the anonymous result tables above are the
record.

