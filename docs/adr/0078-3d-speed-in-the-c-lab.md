# ADR 0078 — 3D speed in the C lab: copies instanced, batches culled, a moving budget

- Status: Accepted, 26 Nov 2026
- Decided by: Pierre delegated the decision ("don't wait for me to validate the ADR, just do your
  best, I'll review once it's implemented"); the choices below by Claude on ticket 3D1, to be
  reviewed by Pierre on the implementation.
- Reversal cost: low (a new BMSH version beside version 1, asked by the page; the viewport's draw
  path; no file format, no golden). Amendment 2 (ticket 3D2) adds the BPRX answer beside BMSH and
  `src/hlod.c`, `src/bvh.c`: as low. Amendment 3 (ticket LNK3D) adds a section to the link cache
  (format 4: older caches are converted again), two queries and a key in the change JSON with links: low.

## Context / Problem

On an imported model (1,749 elements, 2.2 million triangles, mostly `IfcBuildingElementProxy` with
heavy meshes) Pierre found the C lab slower in 3D than the Rust app: "I think it was faster with our
rust implementation, we probably had better 3d handling". He was right. Rust v0.19.0 drew copies
instanced (ADR 0034), culled clusters against the view (ADR 0036 step 1), left out what is smallest
while the view moved (ADR 0035), drew far cells as proxies (ADR 0036 #9) and could cull by GPU
occlusion (ADR 0036 #1). The C port kept the copies in the scene (`ccore/src/scene.c` share_copies,
a port of `dedup.rs`) but the BMSH it sent placed every copy, and the page drew the whole model in
two draw calls per frame, every frame, still or moving.

The record of the numbers is `ccore/results/frames.md` (`ccore/tools/frames.mjs`): headless Chromium
with SwiftShader (WebGL2 on the CPU) at 1280×800, the wasm build and the native host, the
20,026-element synthetic model and the mesh-heavy synthetic model (772 elements, 695 k triangles).
SwiftShader's numbers compare before and after on this machine; a GPU draws the same frames many
times faster.

## Decision

1. **Copies become instances: BMSH version 3.** `ccore/src/scene_bmsh.h`: the rows of version 1,
   each shared shape's vertices, indices and edges once (in its own coordinates), and per element a
   placement (the scene's `Placement32`). The page asks it (`GET /mesh?shapes=1`, wasm `host_mesh`
   flag 2); version 1 stays for every other reader (tests, links of older pages, tools).
   **Alternative:** expand the copies in the page into the merged buffers (the bytes sent fall, the
   GPU bytes do not), or change version 1 in place (every reader of version 1 to update at once).
2. **Shapes with 4 copies or more are drawn instanced; fewer are placed into the merged buffers in
   the page** (Rust's threshold, ADR 0034 phase 2: a draw call per shape costs more than it saves
   for few copies). One `drawElementsInstanced` per run of a shape's instances (faces) and one
   `drawArraysInstanced` (edges), the placement and colour per instance as attributes (divisor 1),
   a mirrored placement in its own group with the other front face. **Alternative:** instance every
   shape (thousands of draw calls on a model of many shapes with two copies each).
3. **Batches culled as wholes.** The merged lists and each shape's instances are cut into spatial
   batches (Morton order of the element centres, at most 32,768 triangles a batch, Rust's cluster
   size); each frame a batch whose box lies outside one of the six planes of the view is not drawn;
   neighbouring batches drawn whole are joined into one draw call (Rust's #7). The setting `gpu_cull`
   is Rust's compute-shader test of copies (ADR 0036 #10), not this: it still does not apply.
   **Alternative:** cull per element (thousands of draw calls) or repack the visible elements'
   indices every frame (a buffer upload per frame).
4. **A moving budget in triangles, never an empty picture** (amended the same day, see below).
   Inside each batch the elements go largest first (half their box diagonal), so "everything at
   least r big" is a prefix. A moving frame (the camera changed) draws, over all batches in view, the
   elements largest on screen first (size over the distance from the eye to the batch's nearest
   point) until its triangle budget is spent: `moving_fps` times the triangles a millisecond the last
   frames drew, never under 20,000 (`MIN_MOVING_TRIS`), and at least the largest element on screen
   when even that one is over it. The batches it could not finish are drawn as the outlines of their
   boxes (one instanced draw of a unit cube's 12 edges), so the building's volume stays on screen.
   150 ms after the camera stops (no button held) the rest is added in slices of the same budget, one
   a frame, without clearing, then every edge once more (an edge of an element drawn early may be
   covered by a coplanar face drawn later); any redraw for another reason (a move, a selection)
   abandons the fill and starts over, so a still frame never delays the next move (ADR 0074).
   **Alternative:** Rust's rule (`next_moving_px`: leave out what is under px pixels at the target,
   px rising to 48 while frames are slow). Rust could afford it because its far cells were drawn as
   HLOD proxies; without them, at 48 px the 20,026-element model drew nothing while moving (0
   triangles, the first run of this ticket), which the owner refused. **Alternative 2:** one whole
   still frame 150 ms after the stop (the first run): under SwiftShader it blocked the next move for
   150-350 ms.
5. **Picks stay in the page, box first, no box tree.** A placed shape's triangles are tested in the
   shape's own coordinates (the ray mapped by the inverse placement keeps its parameter).
   **Alternative:** port `bvh.rs` (into the core or the page); not needed while a pick answers in
   the time `frames.md` shows (see Evidence).
6. **HLOD proxies and GPU occlusion are the next ticket**, not this one (see "What is left").

## Amendment (26 Nov 2026, same ticket, on the orchestrator's review)

"Never an empty or gutted picture while moving": decision 4 above replaces Rust's threshold-only
rule with the triangle budget and the outlines, and the whole still frame with slices. Tests:
`ccore/test/cull_test.mjs` (on the 20,026-element model at the fitted distance a moving frame draws
more than 0 triangles and at most the budget, the largest on screen first; slices draw every
triangle once), `ccore/tools/frames.mjs` (the canvas's non-background pixels while dragging against
the still picture's; it fails under 20 %).

## Evidence

`ccore/results/frames.md`, wasm build, SwiftShader, before → after the amendment (run without
`--force`: 1-minute load 3.9 at the start, 3.9-9.1 at each model's start from the run's own
SwiftShader threads and a parallel ticket):

| model | orbit frame median | still frame | close orbit | GPU bytes | drag input → frame (median) | pick → frame | pictures (3 cameras) |
|---|---|---|---|---|---|---|---|
| 20,026 elements | 187.5 → 31.1 ms | 182 → 174 ms | 358 → 80.5 ms | 17.2 → 17.3 MB | 317 → 23.7 ms | 32 → 32 ms | identical |
| 772 elements, mesh-heavy | 146.1 → 31.7 ms | 178 → 152 ms | 146.5 → 31.5 ms | 13.0 → 0.1 MB | 139 → 42.3 ms | 43 → 17 ms | identical |

The drag latency before was measured one move at a time; after, with 60 Hz streamed moves (the
first run's one-at-a-time method gave 394 ms on the large model, the still frame falling between
moves). While dragging, the canvas keeps 72 % (large) and 82 % (heavy) of the still picture's drawn
pixels (budget plus outlines).

The mesh-heavy model's 700 proxies are one shape: BMSH 12.6 MB → 96 KB. The 20,026-element model's
boxes are copies too, but of 12 triangles: drawn instanced, SwiftShader took 1.2 s a frame against
0.19 s merged (its cost is per instance), so shapes under 64 triangles are placed into the merged
buffers (`INSTANCE_MIN_TRIS`). A 1×1 readPixels after a frame takes most of the frame time once the
budget leaves things out (49 of 50 ms): SwiftShader's work is per pixel there, not per triangle.

The first run's regressions (pick 32 → 189 ms: a selection's panel resized the canvas, which counted
as a camera move; drag 317 → 394 ms; an empty moving picture at 48 px) are gone in the table above.

## Amendment 2 (26 Nov 2026, ticket 3D2): the rest of Rust's 3D layer

Pierre: "ok go with 3D1, port all the rust 3d layer don't wait for me". The choices below were made
by Claude on ticket 3D2 under the same delegation, each with its alternative, to be reviewed on the
implementation.

7. **HLOD proxies built in the core, in steps, served on their own** (`ccore/src/hlod.{h,c}`, a port
   of `hlod.rs` with Rust's options: leaves of 65,536 triangles, a 64-cell grid per side, proxies of
   at most 65,536 triangles kept under a quarter of what they stand for, depth 16; `GET
   /mesh?proxies=N`, wasm `host_proxies`, BPRX bytes; `N` = 0: the octree alone, asked before the
   first draw lists so they are cut by leaf once). The page asks the proxies after the mesh's first frame,
   one step a frame of about 25 ms (20,000 triangles first, then as many as fit, at most twice the
   last), so opening is not slower and no step blocks the page long (ADR 0074).
   **Alternative:** a section of BMSH version 3 (opening pays the whole build before the first
   frame), or the build in the page (a second copy of the triangles in JS).
8. **The batches are cut by octree leaf** (a batch never spans two leaves, leaves in depth-first
   order so a cell's batches lie side by side): a cell drawn as its proxy leaves out its leaves'
   batches as wholes. **Alternative:** the batches as the octree's cells (Rust's clusters are
   separate from its cells; the moving budget's prefix per batch would then be per cell, with cells
   of up to 65,536 triangles against batches of 32,768).
9. **Thresholds: 3 px while moving (Rust's `MOVING_PROXY_PX`), 1 px still**; Rust drew stills exact.
   Asked for models of at least 250,000 triangles (Rust: 1,000,000, and only on slow GPUs: a CPU
   rasteriser is the slow GPU here). While moving, a leaf the budget could not finish is drawn as the
   proxy of the deepest usable cell holding it instead of its box outline (outlines stay where no
   cell has a proxy). Not in a plan, nor with a section box (Rust: not with a clip); not over a
   hidden, revealed or selected element (Rust's `usable`). The setting `moving_proxies` (Rust's)
   turns them off. **Alternative:** Rust's rule, proxies only while moving on a machine that missed
   the target: the still frame would not change.
10. **A mesh of a million triangles or more is shown in three stages**: its 4,096 largest element
   boxes as outlines as soon as it is parsed (before its draw lists, whose index upload is the
   slowest step under SwiftShader), then a frame within the moving budget, then the rest in slices at
   once (the fill of decision 4). A smaller mesh is drawn whole at once, as before (its whole frame
   is short). **Alternative:** the whole picture first (0.6 s more under SwiftShader on the local
   imported model before anything shows), or proxies first (they come seconds later: they are built
   after the first frame).
11. **The box tree in the core for the measure tool's ray; the page's pick stays linear**: the
   page's scan takes 2-5 ms on every model measured (frames.md, "the page's ray scan"), the box
   test per element already skips almost every triangle. **Alternative:** serve the tree to the page (BVH bytes) or
   build it there: no time to win in what frames.md measures.
12. **Occlusion queries per batch, off by default** (as Rust on WebGL2): the open model's batches
   nearest first, one draw each in an `ANY_SAMPLES_PASSED_CONSERVATIVE` query; a batch found hidden
   leaves its faces out (its edges stay: Rust's rule for coplanar ties), its box asks without colour
   or depth when it shows again, and a box seen from the view that found it hidden does not bring it
   back (Rust's fix of a still view redrawing forever); any change of the batches, the section box
   or the view forgets every answer. Not in a slice of the fill (the still picture stays exact).
   **Alternative:** Rust's CPU occlusion (#5): a software depth buffer of occluders in JS each frame.
13. **A selection is drawn over the kept picture** (Rust's #3, "highlights over the kept picture"):
   when only colours changed and the last frame was a complete picture (no section box, plan,
   ghost, or proxy over the element), the elements whose colour changed are drawn again, faces then
   edges, at the depth they had; else the whole frame as before. **Alternative:** a whole frame for
   every selection (a whole still frame a click; when the properties panel resizes the canvas the
   frame is still whole: see "What is left").
14. **The moving rate counts only budgeted frames after budgeted frames**: a whole frame's triangles
   over the time to the next frame (its GPU work not waited for) gave a rate many times too high,
   and the next moving frame drew everything (a 3D1 fault the new benches showed: the close orbit
   drew 236,940 triangles with a budget of 1.35 M). **Alternative:** time the GPU (a fence each
   frame: a stall under SwiftShader).

### Evidence (ticket 3D2)

`ccore/results/frames.md`, run "after 3D2 (1, 2, 3, 5)" (`--force`: 1-minute load 3.45-6.35 at
each model's start, a parallel ticket's browser tests running), against the 3D1 rows:

| wasm, SwiftShader | orbit | still | close orbit | drag input → frame | first frame | GPU |
|---|---|---|---|---|---|---|
| 20,026 elements | 31.1 → 31.6 ms | 174.2 → 162.5 ms | 80.5 → 48.3 ms | 23.7 → 23.0 ms | 1,696 → 1,769 ms | 17.3 → 19.6 MB |
| 772 elements, mesh-heavy | 31.7 → 6.6 ms | 152.1 → 133.9 ms | 31.5 → 45.1 ms | 42.3 → 15.2 ms | 1,781 → 1,597 ms | 0.1 → 9.2 MB |

A local imported model (1,749 elements, 2.2 M triangles; the file is not in the repository, its
report only under `ccore/out/`), wasm build, 1-minute load 7.3, against the owner's numbers after
3D1: still frame 599 → 81 ms (180,479 triangles in 62 draws instead of 2.2 M in 108; the same run
without proxies: 693 ms), orbit 29 → 15 ms, drag input → frame 15 → 9.9 ms (p95 41 ms), GPU 106 →
127 MB (the proxies: 156 cells, 128 with a proxy, 500,305 triangles, 17.8 MB of BPRX, built in 62
steps of at most 127 ms, uploaded 3.8 s after the first ask). The still picture with proxies at
1 px against the full detail: 20 of 538,248 pixels differ by more than 40 levels (27 cells drawn,
143,974 triangles). The open: the boxes 299 ms after the mesh's data, the first detailed frame
1.35 s, the picture complete 3.2 s (the face index upload: 0.3-0.6 s of it); open → first frame
6.8 s at that load (5.8 s after 3D1 on a quieter machine): the core's read, parse, scene and mesh
take 5.3 s of it.

- The edge pass: the 20,026-element model's still frame without it 162 → 82 ms (256,692
  segments): edges cost half the still frame under SwiftShader, as on Rust's M4 Pro (#6).
- The still frame at the fitted distance on the synthetic models: no cell's error is under a pixel
  there (Rust's finding too: "stills stay exact"), so proxies change nothing; they stand for far
  cells on the local imported model, whose leaves are small next to the whole.
- Occlusion queries, settled still frames: 162.5 → 135.2 ms on the 20,026-element model (4 of 20
  batches hidden), 133.9 → 148.2 on the mesh-heavy one, 81 → 81 on the local imported model (11
  of 36 hidden); every orbit slower (31.6 → 36.9, 6.6 → 25.8, 15 → 31 ms: one draw a batch and a
  query each). Pictures identical once settled. So off by default, as Rust did on WebGL2.
- Picks: the page's ray scan 0.2-4.7 ms on every model; the 20,026-element model's pick → frame
  202 ms on the native host, one frame after the click, the canvas resized by the properties panel
  (984 × 547 → 984 × 356; see "What is left").
- Wasm sizes: `ccore.wasm` 1,129,321 bytes, `ccore-view.wasm` 473,334 bytes (budget 1,000,000).

## Amendment 3 (26 Nov 2026, ticket LNK3D): a folder of links drawn as one building

Pierre: "our goal is to display the whole ifc folder as links in the viewer" (17 IFC files, 4.18 GB, over
100 million triangles). What 3D2, LINKF and M1LINKS left in the draw path: no proxies of links, a hide or a
link's move fetched and merged the whole mesh again, notes on links not drawn, every link's detail on the
GPU at once. The choices below by Claude on ticket LNK3D under the same delegation.

15. **A state per element on the GPU, not draw lists per visibility** (`gl/cull.ts` `packState`,
   `batchShown`): one RGBA8 texel per element (its colour; `a` = model << 1 | drawn), read in the vertex
   shader by the element id each vertex (and each instance) carries; a hidden element's vertices go
   outside the clip volume. The draw lists hold every element and are built once per mesh. A hide,
   isolate, show all, show previous, reveal, a class, storey or link rule, a selection or a link's colour
   writes texels (`texSubImage2D` of the rows that changed) and each batch's shown count (`vcum`, `shown`:
   a batch with none is skipped, the moving budget counts shown triangles only). A link whose elements are
   all shown in its own colour reads no texel (`u_plain`). **Alternative:** a per-vertex flag buffer
   updated with `bufferSubData` (a class hidden touches thousands of ranges, 4 bytes a vertex against 4 a
   element); index lists repacked (3D2's way: 0.3-0.6 s of upload on the local model).
16. **The core stops leaving hidden linked elements out of the mesh** (M1LINKS decision 5 reversed): the
   page hides them by GlobalId from `visibility_sets`, as the core hid them. `/mesh` is cached on
   `links_mesh_gen` (a link's geometry: added, read, converted, removed, unloaded; the change JSON's
   `links_mesh`); `query link_draw` gives each link's flag, colour and placement; `query mesh_key` says
   whether a new rev may change what is drawn (`draw`: not a change of views, sheets or schedules alone),
   so a hide in a view (an undoable view change) asks no mesh either. **Alternative:** a mesh rebuilt
   without the hidden elements (the core's time and the page's refetch on every click).
17. **Each model in its own buffers, placed by uniforms** (`makeModelGl`, `setModel`): a link's vertices
   stay in its own frame; its draws set `u_model` and its placement's three rows (`xfRows`), so a move, a
   turn or an offset changes those numbers and the boxes used for culling, picking, the budget and the
   proxies (`xfBox`; picks map the ray into the link's frame, `xfRayToLocal`), never a buffer.
   **Alternative:** one merged buffer with the placement read per vertex from a texture (the first
   version of this ticket: no per-link release possible, a texel fetch more a vertex).
18. **Proxies baked into each link's cache** (section `HLOD` before `PROP`, cache format 4): built by
   3D2's builder in the background conversion from the view file read back (its element order is the
   link's BMSH rows), kept on disk by the reader, served by `GET /mesh?link_proxies=k` after
   `cc_hlod_bprx_check` (untrusted). The page asks them before the mesh and draws them at once: the first
   picture of a project of links; then each frame cuts each link's tree with the eye in the link's frame
   (3D2's pixel rule, its cells' boxes placed for culling); a link's batches are cut by its own leaves.
   A link's proxies take its colour (reference or override). **Alternative:** build them in the core on
   open (3D2's way: seconds per link, every open).
19. **A GPU memory budget across links**: after each frame, a link the frame needed whose detail was
   released is uploaded again from the page's copy (one a frame; one no longer needed is not: the
   cancel), and over the budget the loaded links give their buffers back, hidden ones first, then the
   least recently needed (not drawn in detail for 2 s: far, as proxies, out of view), never the selected
   element's link. A released link is drawn as its coarsest usable proxies, else its batches' boxes; the
   Links panel says "detail loaded" or "proxies only". The budget: `memory_share` of half the device's
   memory (`navigator.deviceMemory`, 4 GiB when unknown; `gl/linkmem.ts`). **Alternative:** a stored
   number of MB (a new setting); the page's copy dropped too and the link's mesh fetched again (a
   `/mesh?link=k` route: left, see below).
20. **Edges where they read**: a batch's edges are drawn when its box spans at least 16 device pixels and,
   while moving, when the frame drew it whole; never a proxy's (3D2 drew their crease edges; a bench hook
   keeps them). **Alternative:** every edge of every batch drawn (half the still frame under SwiftShader).
21. **Notes on linked elements on the overlay** (3D and the live plan): a leader and a label, anchored in
   the link's frame (they follow a move without asking the core), hidden with their link or element,
   orphans in a muted colour; a click on the label selects the note and its element.

22. **The WebGPU renderer (ADR 0083) leaves links to WebGL2 for now**: it reads the parsed mesh, the
   element colours and `shown`, but not a model's placement (a link's vertices now stay in its own
   frame), the links' proxies nor the memory budget. While links are loaded (or their proxies drawn
   before the mesh) the viewport draws with WebGL2, live, and the performance panel says "linked models
   are drawn by WebGL2 for now"; WebGPU takes over again when the links go. Without links it stays
   correct: a visibility change gives `shown` a new array, so its lists are made again. **Alternative:**
   the placement rows and a model index in its colour table (`0xMMRRGGBB`, `gl/cull.ts` `xfRows`) and an
   element per edge vertex in `gpu/` (its edges know no element yet): the follow-up.

### Evidence (ticket LNK3D)

`ccore/tools/frames.mjs --links 6` (six synthetic links of 160 unique closed meshes, 558,144 triangles,
native host, SwiftShader 1280×800), the code before (`next`) and after, the 1-minute load 5.9-7.4 for
both (other tickets' browsers running):

| | open → first picture | still frame (without edges) | moving frame: triangles | close still | GPU | half hidden, budget ½ |
|---|---|---|---|---|---|---|
| before | 2,357 ms (the first frame) | 141.8 ms (103.0) | 31.9 ms: 169,344 | 87.6 ms | 11.4 MB | 8.1 MB (nothing released) |
| after | 1,556 ms (the links' proxies; complete 2,032) | 124.1 ms (90.7) | 12.6 ms: 64,950 | 71.8 ms | 12.9 MB | 7.0 MB (3 of 6 links' detail) |

The pictures of three fixed cameras: 0 of 538,248 pixels differ (at all) between before and after.
`--links 17` (1,581,408 triangles; load 3.3-4.3 before, 3.7-6.1 after): first picture 3,401 ms (the
first frame) → 1,921 ms (proxies), complete 4,201 → 2,631 ms, still 342.4 → 260.8 ms (2 → 34 draws),
moving 32.2 → 33.2 ms (147,060 → 184,025 triangles), close still 206.3 → 191.2 ms, GPU 32.2 → 36.6 MB
(proxies 7.8 MB of the caches' 52.8; 1.3 s of conversion for the 17), half hidden with the budget at
half: 22.4 → 18.9 MB (8 of 17 links' detail kept); pictures: 0 pixels over 40 levels, 1 pixel at all.
A class hidden over the six links (1,392 elements): 1.4 ms in the page, 0.1 ms of state upload, no mesh
fetched. Decision 15's choice by cost (`bench.stateCost(30000, 100)`): 30,000 elements' texels (120 kB)
upload in 0.2 ms, a flag per vertex for them (100 vertices each, 12 MB) in 8.5 ms.
The caches: 15.9 → 18.6 MB (the proxies 2.8 MB, 460 ms of conversion for the six). `lab_links_3d.mjs`:
each input's feedback 29-120 ms (hide a link, show it, hide an element, a class, move a link, its
colour, the note following it), no `/mesh` request and no draw list rebuilt for any of them; shown
again, the same picture (0 pixels differ).

## Amendment 4 (27 Nov 2026, ticket SIZE2): a link's mesh on its own, compact, not kept in the page

The owner, on a local folder of 17 IFC files (3.9 GB) linked into a project: "how is that that our bimble-view are so
big?"; the tab's renderer process at 3.8 GB, the host serving a /mesh of 728,838,144 bytes for 16 links whose caches'
geometry sections sum to about 230 MB. Measured here: /mesh did send each link's shapes once (BMSH version 3: no copy
expanded); its size was the raw form, 12 bytes a vertex, 12 a triangle, 32 a segment, against the cache's mesh codec
(BMG1: 1/16 mm varint deltas): 3.2 times the caches' geometry on the synthetic set as on the owner's. The page then held
that answer, a merged copy of every array (`parseMesh`) and each link's draw lists, after the GPU had them. Choices by
Claude on ticket SIZE2 under the same delegation.

23. **A link's mesh on its own, in the cache's codec: BMSH version 4** (`ccore/src/scene_bmsh.h`; `GET /mesh?link=k`,
   `cc_link_mesh`): version 3's rows, the shape of each element (a placement only for an element that places a shape),
   then each block (an own mesh or a shape, version 3's first-use order) as one BMG1 mesh. The page reads it back into
   version 3 (`web-ui/src/gl/bmsh4.ts`, in slices of 25 ms; the C reader `cc_bmsh4_to_bmsh3` gives the same bytes and
   is the hostile-input test's). `GET /mesh?shapes=1&lazy=1` sends the open model and the LNKS trailer version 3: each
   link's head (name, flags, placement, colour, its element count, the links' mesh key), no mesh. **Alternative:** gzip
   over HTTP (the core's deflate on 700 MB per open; f32 positions compress badly) or 16-bit positions per element's box
   in the GPU buffers (halves the GPU bytes too; a shader and every reader of positions to change: left).
24. **The page keeps no link's heavy arrays once on the GPU.** The mesh is one table of rows (ids, ranges, shapes,
   placements: merged as before) and per model its own arrays (`MeshPart`, never merged); a link's are dropped with its
   draw lists once its buffers and lists are uploaded (`dropPart`). What stays per element: its row (22-byte id as a JS
   string, category, six u32 ranges), its shape and placement (52 bytes), its box twice (scene and own frame, 48), its
   batch, face, edge, instance and group slots (20), its state texel (4) and shown flag (1): about 200 bytes an element
   with the id, against about 40 bytes a triangle before. Each link's batches are made once per mesh and kept (their
   order is what its face list on the GPU follows), so a later `drawLists` (the open model's proxies) needs no link's
   arrays. **Picks read the GPU**: an element of a dropped link has its vertices and its face or shape indices read back
   (`getBufferSubData`, `trisOf`), kept in a cache of 8 MB; a released link (no GPU buffers) is not picked and is asked
   back. **Alternative:** ask the core for picks on links (it has the box tree): asynchronous, and the sync callers (the
   wheel's pivot, the pointer readout, the tests' scans of thousands of points) would each wait for the host.
25. **A released link's detail comes back by asking its mesh again** (`refetchSlot`: `GET /mesh?link=k`, then its lists
   from its kept order, then dropped again), not from a page copy. Shown again after a release: 158 ms (lab_links_3d,
   SwiftShader), the same pixels.
26. **Proxies baked into a link's cache within a budget** (cache format 5, ADR 0079 amendment): at most 15 % of its
   geometry section (256 KiB to 32 MiB), the finest cells' proxies left out first (the coarsest kept: a released link is
   drawn by its coarsest usable proxies), 4,096 triangles at most a cell (65,536 for the open model's), gzipped on disk
   (served inflated, bounded). **Alternative:** 16-bit positions per cell, proxies of a shape instanced, coarser levels
   below the root: left (see "What is left").

### Evidence (ticket SIZE2)

`ccore/tools/frames.mjs --links 17` (the synthetic set: 17 links of 160 unique meshes, 272 MB of IFC), native host
(clang), `LAB_GPU=vulkan` (headless Chromium on the machine's GPU: ANGLE, Mesa, Apple M1 Pro), before (`next`) and after:

| | caches (ELEM / HLOD / PROP) | /mesh bytes served | host RSS | tab RSS (GPU process) | JS heap + array buffers | GPU | first picture → complete | still / moving frame |
|---|---|---|---|---|---|---|---|---|
| before | 55.4 MB (9.37 / 8.18 / 37.8) | 30.06 MB | 211 MB | 888 MB (110) | 4.1 + 56.7 MB | 36.6 MB | 355 → 534 ms | 4.7 / 1.7 ms |
| after | 20.2 MB (9.37 / 1.12 / 9.71 PRPZ) | 9.43 MB | 138 MB | 835 MB (113) | 4.2 + 3.9 MB | 34.4 MB | 359 → 869 ms | 6.6 / 4.7 ms |

The pictures of three fixed cameras: 0 of 538,248 pixels differ (at all). The tab's RSS is mostly Chromium's own and its
GPU driver's on this set; the page's array buffers fall 56.7 → 3.9 MB (the part that grows with triangles). "Complete"
is later: the links' meshes are asked one after the other and read back in slices.

A local folder of 17 IFC files (4.18 GB, 252,189 elements, 22.5 M stored triangles), the same tool's folder mode
(`FRAMES_LINK_FOLDER`, `LAB_GPU=vulkan`, a watchdog at 2.5 GB available): /mesh and the links' meshes 728.8 MB (the
owner's host log, before) → 227.2 MB served; the tab's renderer process 3.8 GB (the owner's, before; not measured again
here: the machine's memory rule) → 671 MB after the open, JS heap 59 MB + 76 MB array buffers, GPU 1.62 GB (all 17
links' detail within the default budget); still frame 104 ms (67.9 M triangles drawn, 5,553 draws), moving 77 ms
(535,514 triangles); first picture (the links' proxies) 0.76 s, the first detailed frame 10.5 s. Left: the JS heap
peaked at 2.0 GB during the open (every link's mesh read back before the first upload: decision 24 drops them after);
uploading each link as it arrives would bound that peak.

## What is left

Done by ticket 3D2 (amendment 2 above): HLOD proxies, the box tree (in the core), GPU occlusion
(off by default). Left, with the reason:

- **The close-up still frame**: from a fifth of the fitted distance no cell's error is under a pixel,
  so the local imported model's close still frame stays 2.0 M triangles (1.0 s under SwiftShader);
  occlusion queries would help there most (Rust's ceiling: 20-97 % hidden close up) but cost more
  than they save while moving. A GPU draws it in milliseconds.
- **The draw lists' index upload**: on the local imported model the face index buffer (23.6 MB)
  takes 0.3-0.6 s to upload under SwiftShader, on open (the boxes are on screen meanwhile) and after
  every visibility change: a hide or isolate on that model answers in that time, over ADR 0074's
  200 ms. Lists uploaded in slices (or kept whole, the hidden elements skipped by their ranges)
  would remove it. (The proxies' arrival no longer cuts the lists again: the page asks the octree
  alone first, `GET /mesh?proxies=0`.)
- **The native pick → frame** (about 200 ms on the 20,026-element model): the click's selection
  opens the properties panel, which shrinks the canvas (984 × 547 → 984 × 356): the resized canvas
  is drawn whole (a still frame, ~150 ms under SwiftShader) after the host's answer. The page's ray
  scan takes 2-5 ms; a selection without a resize is now drawn over the kept picture (decision 13).
- **Rust's CPU occlusion (#5), screen edges while moving (#6), GPU culling of copies (#10)**: not
  ported (the queries stand for #5; #6 needs a depth read-back pass; #10 needs compute shaders).
- **Proxies of linked models**: done by amendment 3 (baked in each link's cache).
- **For 100-200 million triangles** (amendment 3's numbers): a mesh per link, fetched and dropped from the page, is
  done (amendment 4). Left: every link's mesh is still fetched at open (one after the other) and uploaded before the
  budget releases some; tiles streamed by view (the link's HLOD leaves as the unit: the cache already orders them) would
  fetch only what the view needs. Proxies of a shared shape instanced, levels coarser than a cell's 4,096 triangles (a
  link at 50 px should cost a few hundred), 16-bit positions per cell: not built. Quantised vertices (16-bit positions in each leaf's box,
  8-bit normals) would halve the GPU bytes of positions; the state texel per element is already 4 bytes.
- **Screen-space edges** (in place of the edge pass): a pass of the element id and depth into a render
  target (WebGL2: an R32UI colour attachment beside the depth), then a full-screen pass drawing a line
  where the id or the depth's slope changes between neighbours; it needs the element id as a vertex
  output (amendment 3 gives it), `EXT_color_buffer_float` or integer targets, and a decision for
  coplanar faces of one element (the crease angle from the normals). Not built: amendment 3's rule
  (decision 20) halves the edge pass where it matters.
