# ADR 0036 — Detail levels (Nanite-like clusters): not yet; what would make them worth it

- Status: Proposed (9 Nov 2026) — Nanite itself undecided until measured (Pierre: "only facts-based
  decisions"); the cheaper steps and the list below accepted to do (9 Nov)
- Decided by: — (Pierre asked to look at it: "6 (Nanite-style detail levels)")
- Reversal cost: n/a

## Context / Problem

Virtualized geometry (Unreal's Nanite; open versions: Bevy's meshlets, meshoptimizer's
`clusterlod`, nanite-webgpu) splits meshes into ~128-triangle clusters arranged in a hierarchy
of simplified levels, and draws, per frame, the coarsest level whose error stays under a pixel,
culled per cluster (frustum, occlusion). It keeps frame time near-constant whatever the triangle
count.

Where Bimble stands after ADR 0034/0035 (1080p, MSAA 4, M4 Pro, `bimble bench`):

| model | triangles drawn | stored | full frame | moving (3 px) |
|---|---|---|---|---|
| 200 MB electrical | 8.8 M | 0.38 M | 7.7 ms | 0.6 ms |
| 872 MB plumbing | 9.1 M | 5.5 M | 7.5 ms | 1.6 ms |
| 758 MB sanitary | 21.0 M | 4.3 M | 17.8 ms | 1.6 ms |
| 415 MB sprinklers | 35.8 M | ~2.7 M | 33.9 ms | 3.2 ms |

## Assessment (not decided)

- **What it would add:** stills of the largest models at full rate (today 30 fps for 35.8 M
  triangles) and smooth orbiting without things disappearing (ADR 0035 drops small elements).
- **Cost:** high. A cluster builder and simplifier (meshoptimizer is C: building it for
  `wasm32-unknown-unknown` is unverified), a cluster hierarchy per shape (shared by instances),
  per-cluster culling, and for real gains GPU-driven draws — compute and indirect draws that
  WebGL2 doesn't have (multi-draw-indirect is not in WebGPU either, only native wgpu).
- **Fit for BIM:** IFC elements are low-poly and many; simplifying each one gains little. The
  triangles are in fittings and pipes (already instanced) and in faceted breps. Hierarchical LOD of
  *spatial cells* (merged proxies of what is far away) fits better than per-mesh clusters.
- **Cheaper steps first,** each measurable:
  1. Frustum culling per element and per shape draw through the box tree (ADR 0034): nothing is
     drawn off screen (today every buffer is drawn; the GPU clips).
  2. Indexed shapes (positions welded, 16-bit indices for shapes under 65 536 vertices):
     about half the GPU bytes per shape triangle.
  3. Dedup of geometry the file didn't instance (IfcOpenShell's `MeshDedup`: bucket, invariants,
     Kabsch fit) — the 872 MB model is 55 % unshared breps.
  4. Measure on a weak GPU (an integrated GPU, a software adapter, WebGL2) before going further.

## Step 1 done: frustum culling (9 Nov)

Merged geometry is cut into spatial clusters (Morton order of element centres, ~32 768
triangles each, within a buffer; largest first inside each, so the moving size filter is a prefix
per cluster). Each frame tests clusters and every shape instance against the frustum (planes from
the layer's matrix), the section box and the size threshold; shapes with copies left out draw from
a packed copy of their visible instances. `DrawOptions::cull` turns it off to compare.

`bimble bench` (1080p, MSAA 4, M4 Pro), new "close" orbit at 1/5 of the fitted distance:

| model | close before | close after | drawn |
|---|---|---|---|
| 200 MB electrical | 7.75 ms | 4.86 ms | 46 % |
| 335 MB architecture | 5.23 ms | 4.16 ms | 60 % |
| 415 MB sprinklers | 42.0 / 43.0 / 44.5 ms | 22.2 / 21.8 / 20.8 ms | 44 % |
| 758 MB sanitary | 24.1 ms | 13.4 ms | 50 % |
| 872 MB plumbing | 14.4 ms | 4.2 ms | 45 % |

The whole-model orbit is unchanged within run-to-run noise (415 MB: before 58.2 / 47.0 ms,
after 48.2 / 47.8 ms, 120 frames). Moving frames too (758 MB, 120 frames, interleaved: before 2.17 / 2.22 ms,
after 2.10 / 2.15 ms). Test `culling_changes_nothing_on_screen`: 12 cameras (close,
orthographic, mirrored, section box, the same model linked half a model aside) give identical
pixels with and without culling, and close up it does leave triangles out. It caught a bug: a
scene drawn in two layers packed its visible instances twice into one buffer before the frame's
single submit, so one layer drew the other's (1 244 pixels); now only its first layer packs.

## Step 2 done: indexed faces, normals from the screen (9 Nov; with #2 below)

Face vertices were three per triangle, each with the face's normal (16 bytes, 48 a triangle).
The meshes are already welded per element, so the upload now sends their shared vertices
(position and style slot, 16 bytes) and u32 indices into the same buffer (no base vertex:
WebGL2 has none). The shader finds each face's normal per pixel from screen derivatives of the
position relative to the eye (`cross(dpdx, dpdy)`), so faces stay flat. Elements larger than a
buffer stay unindexed (their triangles may straddle two buffers). Skipped: 16-bit indices for
shapes (shapes are stored once, so it saves little) and meshoptimizer's vertex-cache order
(C, unverified on wasm).

`bimble bench` (1080p, MSAA 4, M4 Pro, 120 frames), before → after:

| model | GPU geometry | full frame | close-up | moving | upload |
|---|---|---|---|---|---|
| 200 MB electrical | 28 → 18 MB | 7.98 → 5.22 ms | 4.26 → 2.93 ms | 0.64 → 0.53 ms | 35 → 17 ms |
| 335 MB architecture | 143 → 80 MB | 5.56 → 4.12 ms | 3.81 → 2.86 ms | 3.96 → 2.80 ms | 107 → 65 ms |
| 415 MB sprinklers | 181 → 103 MB | 33.8 → 25.1 ms | 15.2 → 11.1 ms | 3.22 → 2.89 ms | 144 → 86 ms |
| 758 MB sanitary | 258 → 136 MB | 20.9 → 12.1 ms | 10.3 → 6.7 ms | 1.76 → 1.23 ms | 156 → 91 ms |
| 872 MB plumbing | 304 → 155 MB | 7.75 → 5.19 ms | 3.92 → 2.59 ms | 1.66 → 1.14 ms | 185 → 96 ms |

Peak process memory unchanged (872 MB model, two runs each: 2.90 / 2.91 GB before, 2.91 / 2.93
after). Pixels change slightly: the normal was 8 bits a component, now exact. Test
`welded_faces_are_shaded_flat` (a column's three sides and edges: 98 % of its pixels in four
colours; it fails with a gradient). Without edges, the one-pixel line where two faces meet takes
in-between shades; the edges cover it. Not yet measured on a weak GPU: shading moved from the
vertex to the pixel shader, which costs more where the GPU is short of pixel throughput.

## Step 3 done: copies the file didn't mark (9 Nov)

`bimble_geom::dedup::share_copies` runs when a scene is built (IFC and native). Elements with
their own mesh and as many vertices, triangles and edges are fitted from four vertices (matched
by order) with a linear map and a translation; every vertex must land within 0.1 mm. Unlike
IfcOpenShell's `MeshDedup` (rotations only), copies may be stretched: most own meshes of MEP
models are pipes of other lengths. Counted on the five models, of their own triangles:

| model | own triangles | became copies | their shapes |
|---|---|---|---|
| 200 MB electrical | 0.10 M | 0.08 M | 0.006 M |
| 335 MB architecture | 0.73 M | 0.32 M | 0.03 M |
| 415 MB sprinklers | 1.39 M | 1.33 M | 0.003 M (15 shapes) |
| 758 MB sanitary | 3.09 M | 1.55 M | 0.10 M |
| 872 MB plumbing | 4.97 M | 2.71 M | 0.38 M |

Five lessons, each measured:
- Same indices is too strict: a polygon's diagonals depend on its orientation (the triangulation
  projects onto the plane its normal points most along), so turned copies triangulate
  differently. Vertices in the same order are matched; a copy takes the shape's triangulation.
- The shape lies in its own frame (longest feature edge, then the longest across it). In scene
  axes, a turned first element gave a loose box; its copies' placed boxes were looser still,
  which widened the scene bounds (the fitted camera drew the model on 2.5× fewer pixels) and the culling
  spheres (77 % drawn close up instead of 60 %).
- A copy whose placed box reaches over twice its own keeps its own mesh (sheared fits: 1 % of
  the 415 MB model's, up to 6×).
- Instance spheres: the placed box's centre and longest placed half-diagonal, exact for turned
  and stretched copies (the longest column times the half-diagonal was 6× too large for a
  stretched pipe).
- Mirrored copies wind their faces the other way, so their vertices come in another order: they
  keep their own mesh.

`bimble bench`, step 2 → step 3 (interleaved runs): GPU geometry 18 → 15, 80 → 71, 103 → 63,
136 → 95, 155 → 98 MB (−11 to −39 %); frames unchanged within noise (±3 %); upload 15–30 %
faster; load time unchanged. `render` pictures: identical on the 335 MB model, within 699 pixels
of 1.6 M on three others; the 758 MB model frames 0.1 % wider (its bounds), so its pixels shift.
Test `rotated_copies_share_one_shape` (turned and stretched share; mirrored and a corner moved
50 mm don't; every element's vertices stay within 0.2 mm). Tried and dropped: cutting clusters at
a sixteenth of the scene (3× the draws, slower) and not instancing shapes under 32 triangles
(the 200 MB model's fittings back in the merged buffers, +40 % GPU memory, no faster).

## #3 half done: highlights over the kept picture; the redraw in bands dropped (9 Nov)

- **Done: highlights drawn over the kept picture.** The element under the cursor and a selected
  linked element were drawn with the model, so each hover change redrew the whole model. They
  now draw into a transparent overlay (`Renderer::render_overlay`), depth-tested against the
  model's depth, which is kept (stored instead of discarded), and egui lays it over the picture.
  `bimble bench` "hover ms": 0.24–0.33 ms on the five models, where a hover change cost a full
  frame (4.4–33 ms). Test `highlights_drawn_over_the_kept_picture_match`: pixels wholly in the
  overlay, and pixels outside it, match drawing the highlight with the model (it fails with 14
  pixels if the overlay ignores the depth). A partly covered edge pixel blends with the resolved
  picture under it rather than per sample: a slightly darker fringe. Keeping the depth costs
  nothing measurable on the M4 Pro (within the noise of the day's runs).
- **Dropped, measured: completing a moving picture in size bands.** After the view stops, add
  only what moving left out, over the kept picture, a size band a frame (halving down to half a
  pixel), then all edges again (pixel-identical to a whole picture in a test). On the 415 MB
  model it took 54 ms in all against a 31 ms whole frame, and one band alone 49 ms: most
  triangles are small, so they fall in the last band, and every pass reloads the MSAA colour and
  depth and goes over every draw again. To try again only if a weak GPU shows a still frame long
  enough to block input, with slices of equal cost rather than size bands.
- **#4 done: no MSAA while moving, when the machine is too slow.** On the M4 Pro neither MSAA 1
  nor half resolution made frames faster. On a simulated weak GPU (Mesa's llvmpipe, a CPU
  Vulkan driver, in a Linux container capped to 2 cores; not a real old GPU, which pays less for
  MSAA) at 1080p:

  | model | MSAA 4 full / moving | MSAA 1 full / moving | MSAA 1, half resolution |
  |---|---|---|---|
  | 200 MB | 2.43 s / 283 ms | 1.94 s / 105 ms | 1.76 s / 78 ms |
  | 335 MB | 2.80 s / 1.79 s | 1.41 s / 975 ms | 1.31 s / 816 ms |

  So the viewer drops MSAA for the rest of a movement as soon as a moving frame misses the
  frame-rate target (ADR 0035), drawing with a single-sample renderer into its own target, and
  draws the still picture with MSAA once the view stops. A machine that keeps the target never
  drops it. Half resolution was not built: 15–25 % more, for a blurred picture. Highlights are
  hidden while such a picture is shown (they are depth-tested against the MSAA picture).

## How much is hidden: the ceiling of occlusion culling (#1, #5; 9 Nov)

`Renderer::hidden_share` draws the picture, then each merged element and each shape copy again
against its depth with an occlusion query: those with no sample left are hidden. `bimble bench`
"hidden" / "hid.cl" (whole model / close up, 8 views each), share of the drawn triangles:

| model | per element: whole / close | per cluster (merged) and per copy |
|---|---|---|
| 200 MB electrical | 36 % / 20 % | – |
| 335 MB architecture | 92 % / 97 % | 72 % / 83 % |
| 415 MB sprinklers | 75 % / 9 % | 74 % / 8 % |
| 758 MB sanitary | 63 % / 41 % | – |
| 872 MB plumbing | 51 % / 42 % | 40 % / 25 % |

These are ceilings (perfect culling, no cost). Test `hidden_share_counts_what_is_behind` (an
element alone: 0 %).

## #8 done: a geometry codec (9 Nov)

`bimble_geom::codec::{encode, decode}`: a `Mesh` to bytes and back, pure Rust, no new
dependency, builds for wasm32. Not yet used by any file or download (that waits for the viewing
format). Positions are quantized to 1/16 mm from the mesh's lower corner, as differences from
the previous vertex (zigzag varints); a mesh keeps its raw f32 when a vertex isn't finite or it
spans over 268 km (and, as a guard, when a vertex would come back over 0.05 mm away). Indices are
differences from the previous index. Edge endpoints that are vertices become vertex indices
(decoded bit for bit like the vertex); others are quantized. Edge normals become indices into
the mesh's table of distinct normals. `decode` reads untrusted bytes: counts are checked against
the bytes left before allocating, indices against the vertex count, nothing may be left over.

`bimble codec-bench` (hidden; M4 Pro, one thread, each own mesh and each shared shape once;
raw = the bytes in memory: positions 12 B, indices 4 B, edges 24 B and their normals 8 B;
speeds in raw MB/s):

| model | raw | encoded | ratio | encode | decode | gzip raw / gzip encoded |
|---|---|---|---|---|---|---|
| 49 MB (Schependomlaan) | 5.6 MB | 1.3 MB | 4.4× | 615 MB/s | 2.1 GB/s | 1.3 / 0.7 MB |
| 342 MB (Sixty5) | 12.8 MB | 2.9 MB | 4.4× | 618 MB/s | 2.2 GB/s | 2.7 / 1.4 MB |
| 394 MB | 61.1 MB | 17.3 MB | 3.5× | 400 MB/s | 1.5 GB/s | 20.0 / 9.6 MB |
| 415 MB | 54.0 MB | 15.3 MB | 3.5× | 429 MB/s | 1.6 GB/s | 19.1 / 9.8 MB |
| 709 MB | 307 MB | 70.5 MB | 4.4× | 613 MB/s | 2.2 GB/s | 83.7 / 31.0 MB |
| 758 MB | 86.6 MB | 24.5 MB | 3.5× | 596 MB/s | 1.8 GB/s | 28.8 / 15.7 MB |
| 872 MB | 88.2 MB | 25.5 MB | 3.5× | 627 MB/s | 1.8 GB/s | 31.0 / 16.0 MB |

So 3.5–4.4× smaller, decoding at 1.5–2.2 GB/s (meshopt publishes 3–6 GB/s, with SIMD);
without the edge normals in either count, 3.0–3.8×. About 5 bytes a vertex, 1.1–1.25 an index,
5 an edge with its normals. Every mesh of the seven comes back with the same indices and edge
count, no point moved more than 0.031 mm, and none was kept raw. Gzip, which HTTP adds anyway,
halves the encoded bytes again (gzip of the raw bytes alone reaches 2.7–3.7×); gunzip then
decode runs at 0.65–1 GB/s. A second run, with other work on the machine, gave encode and
decode speeds up to half these on the 709 MB model; sizes are exact. Tests: `round_trip_within_tolerance` (demo frame, empty, far from
the origin, out-of-order indices, edges off the vertices, NaN), `corrupt_bytes_are_errors_not_panics`
(every prefix, byte flips, random bytes, a huge count with nothing behind it).

Tried and dropped, measured:
- Indices against the next new vertex (welding numbers vertices by first use): 1.10–1.30 bytes
  an index, against 1.07–1.25 for the previous index. Per triangle (first index against the
  previous triangle's, the other two against the first): the same as the previous index.
- Edge normals as their 6 bytes: 8.3 bytes an edge; with the table of distinct normals 4.8–5.0
  (the 49 and 342 MB models went from 3.4× to 4.4× in all).
- Deflate inside the codec: not added (`miniz_oxide` isn't a dependency of `bimble-geom`); a
  cache file or HTTP can gzip the encoded bytes, as measured above.

## Further optimizations to do (research, 9 Nov; one issue each)

Gains below are other systems' published figures or estimates (est.), not Bimble's. Each one is
measured with `bimble bench` before and after (CLAUDE.md), on the M4 Pro and on a weak GPU.

| # | Score /10 | Optimization | Published gain (system) | WebGL2 | Cost |
|---|---|---|---|---|---|
| 1 | 8 | Occlusion culling: GPU queries on box-tree nodes, reusing last frame's results (CHC++); buffers ordered by tree leaf | 7–40× interior, 2–6× exterior views, frustum-only baseline (Revit models, GRAPP 2013) | wgpu's GL backend reads results too early (to check); else #5 | M–L |
| 2 | 8 | Face normals from screen derivatives in the shader; welded indexed vertices; meshoptimizer vertex-cache order | est. 2–3× fewer vertex-shader runs on boxy shapes | yes | S |
| 3 | 8 | Full redraw spread over frames into a kept image; hover and selection drawn over the copy | Autodesk viewer "progressive display" (no figure) | yes | S–M |
| 4 | 7 | While moving: no MSAA and lower resolution; still: averaged jittered frames | FSR1 +22 % on an Intel iGPU (game); est. 4× less colour bandwidth without MSAA 4 | yes | M |
| 5 | 7 | CPU occlusion culling: walls and slabs rasterized coarsely (Masked Occlusion Culling) | culls 98 % of what a full depth buffer does (HPG 2016) | yes (SIMD128) | L |
| 6 | 6 | Edges found in screen space (depth, normal, element id) instead of line geometry, while moving | est.; Speckle's choice | yes | M |
| 7 | 5 | Visible draw ranges merged; multi-draw where it exists | 1.25–1.7× from fewer draw calls (GRAPP 2013) | Chrome only, not through wgpu | S |
| 8 | 5 | Compressed geometry (meshopt codec) for caches and downloads | 2–4× smaller than packed data, decodes 3–6 GB/s | yes | S–M |
| 9 | 5 | Simplified proxies per box-tree cell for what is far (HLOD) | est.; iTwin.js and 3D Tiles do it | yes | L |
| 10 | 4 | GPU-driven culling: depth pyramid, compute, indirect draws | ~2× (VTK WebGPU) | no | L |

Order: steps 1–3 above first (all done: frustum culling, indexed shapes with #2, dedup), then #3, #4, #1/#5
(after a weak-GPU measurement), then the rest. #3: highlights done, bands dropped; #4 waits for the
weak-GPU measurement; #8: the codec done, not yet used.

## Revisit when

- A weak GPU or WebGL2 measurement shows full frames over 50 ms on a real model after steps 1–3.
- Models over ~100 M drawn triangles arrive (multi-GB IFC).
- WebGPU gets multi-draw-indirect, or the browser target is dropped for such models.
