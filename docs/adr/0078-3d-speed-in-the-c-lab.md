# ADR 0078 — 3D speed in the C lab: copies instanced, batches culled, a moving budget

- Status: Accepted, 26 Nov 2026
- Decided by: Pierre delegated the decision ("don't wait for me to validate the ADR, just do your
  best, I'll review once it's implemented"); the choices below by Claude on ticket 3D1, to be
  reviewed by Pierre on the implementation.
- Reversal cost: low (a new BMSH version beside version 1, asked by the page; the viewport's draw
  path; no file format, no golden). Amendment 2 (ticket 3D2) adds the BPRX answer beside BMSH and
  `src/hlod.c`, `src/bvh.c`: as low. Amendment 3 (ticket LNK3D) adds a section to the link cache
  (format 4: older caches are converted again), two queries and a key in the change JSON with links: low.
  Amendment 6 (ticket TILE1) adds a section `TILE` to the link cache (format 7: a format 6 cache is
  indexed again from itself, not converted), three `/mesh` answers and one wasm call; nothing on the page's
  draw path yet: low.

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

## Amendment 5 (27 Nov 2026, ticket SIZE3): copies shared as Rust, a folder opened within bounds, the GPU budget a setting

The Rust and C comparison on a real folder (`docs/research/2026-11-27-rust-vs-c-on-a-real-folder.md`) left four gaps for
the page: an imported `.bimble` model stored 2.0 M triangles where Rust stores 0.30 M, the tab's JS heap peaked at 2.0 GB
while a folder of 17 links opened, the GPU held 1.62 GB of links (the budget never bit), the first detailed frame came
after 10.5 s. Choices by Claude on ticket SIZE3 under the same delegation.

27. **A model's copies are shared as Rust's `Scene::from_model` shares them** (`ccore/src/scene.c` `shared_shapes`,
   `scene.rs:266-312`): elements whose mesh generators hold one positions array (the binary `.bimble`'s table of mesh
   arrays, ADR 0042 decision 4: one array per IfcMappedItem's shape), uncut, are one shape meshed once (the first one's
   `element_mesh`: crease edges, no edge normals) and placed by their placement and storey; the scene's origin takes
   their placed boxes' corners (`Body::Instance`). C meshed each element alone and relied on `share_copies` alone,
   whose fit finds turned, scaled and stretched copies but not those its bucket cap (16 shapes of one vertex, triangle
   and edge count) leaves out. Per kind of copy (`test/scene_dedup_test.c`), dedup shares translated, turned (90° about
   z, 30° about an oblique axis), scaled, stretched and within-0.1-mm copies, not mirrored ones (faces kept outward),
   faces in another order, nor a corner 0.2 mm off: as `dedup.rs`, whose test is ported. Every Rust golden unchanged.
   **Alternative:** raise or drop the bucket cap (hostile buckets of thousands of distinct meshes: quadratic fits).
28. **A folder opens with bounded page memory, its detail on demand.** Each link's mesh (BMSH version 4) is read in a
   Web Worker (`gl/bmsh4.worker.ts`, the bytes transferred both ways), one link at a time, the smallest first. A link
   whose version 3 bytes fit what is left of 128 MB (`OPEN_KEEP_BYTES`) is decoded whole and uploaded with the open;
   the first that does not is read as its rows and its elements' boxes only (`bmsh4Rows`: one block decoded at a time
   into a scratch, the boxes in `upload`'s arithmetic, `bake`'s rows), and every later one is asked as the host's rows
   answer (`GET /mesh?link=k&rows=1`, `cc_link_rows_answer`: version 4 with no block, then each element's box computed
   by the host in the page's f32 arithmetic: no mesh moves), never their whole arrays: its rows, boxes and batches stand without
   them (`Bmsh.dropped`, `preBox`). Its detail is asked again only when a frame wants it (its batches in view and not
   replaced by its proxies), two links at most at a time, the largest on screen first (`wantScore`), then uploaded and dropped
   from the page (amendment 4); until then its proxies stand for it, by their normal cut when the frame does not want its
   detail (`wantedLast`), else by its coarsest. **Alternative:** a pipeline of 2 (twice the transient memory for little:
   the fetch is not the slow part); decode on the page's thread in slices (25 ms slices: over a frame).
29. **The links' GPU budget is a setting in MB** (`link_gpu_mb`, 64 to 65536, 768 by default; `ccore/session/settings.c`,
   not Rust's: written to the file only once set, as `renderer`), in Settings and in the performance panel and report
   ("linked files on the GPU: used of budget"). Before: `memory_share` of half of `navigator.deviceMemory` (Chromium caps
   it at 8 GiB: 3.2 GB here); 1.62 GB of links never reached it. **Alternative:** keep the automatic share (it cannot know
   the GPU's own memory; the owner asked for a number he can see).

30. **A link's proxies may arrive after it is drawn** (ADR 0079 amendment, decision 23: a link is ready before its
   proxies are baked): `query link_draw`'s `proxies` counter moves, the page asks that link's proxies, releases its
   detail and makes its batches again by their leaves (`refreshLinkProxies`); its detail comes back grouped when a frame
   wants it. **Alternative:** ask the whole mesh again (every link's rows and boxes: 227 MB on the local folder, per
   link indexed).

### Evidence (ticket SIZE3)

A local imported model (`.bimble`, 1,749 elements, 2.2 M triangles; `scene.c` before and after, one process):
elements placed as copies 1,081 → 1,472 (Rust: 1,472), stored triangles 1,674,526 → 302,909 (Rust: 304,381), BMSH
version 3 66.8 → 12.4 MB, version 4 17.4 → 2.7 MB, its scene built in 1,067 → 170 ms (Rust reads and meshes it in
180-220 ms).

`ccore/tools/frames.mjs --links 17` (the synthetic set), native host, `LAB_GPU=vulkan` (ANGLE, Mesa, Apple M1 Pro), load
8.7: first picture (proxies) 475 ms, the links in view detailed at 1,074 ms, complete 1,335 ms (SIZE2: 359 → 869 ms: the
detail now follows the frames' wants), the open's JS heap and array buffers at most 57.5 MB (polled every 250 ms), every
decode in the worker, GPU 34.4 MB, still 6.4 ms, moving 4.8 ms (SIZE2: 6.6 / 4.7). `lab_links_3d`: an open with a budget
of 0 decodes no link whole and gives the same picture as an open keeping them all (0 pixels differ by more than 40 levels), once the
links in view are back.

A local folder of 17 IFC files (4.18 GB, 252,189 elements, 22.5 M stored triangles), from its caches, the same tool's
folder mode, `LAB_GPU=vulkan`, load 2.6, against SIZE2's run of the day before: first picture 0.76 → 0.88 s (proxies),
the links in view detailed 10.5 → 5.0 s (14 of 17 links' detail on the GPU, the other three out of view or far), GPU
1.62 GB → 0.94 GB (the links' detail 0.95 GB against a budget of 0.77 GB: the links on screen are kept past it), still
frame 104 → 113 ms (67.9 M triangles), moving 77 → 80 ms, JS heap and array buffers after the open 59 + 76 → 62 + 85 MB.
Their peak: 2.0 GB → 0.59, 0.93 and 1.33 GB in three runs: the open itself stays within 128 MB of decoded arrays plus
the rows, but a wanted large link's detail is still decoded and uploaded whole (one link of 420 MB of arrays: its
version 3 bytes, its index lists and its vertex tables at once), and the collector frees the one before late.
With the host's rows answers (the folder converted fresh by the same build, load 9.2): first picture 0.79 s, the links
in view detailed at 3.3 s, complete 3.8 s; still 109 ms, moving 84 ms; the links' detail took 1.68 GB at the first
quiet frame (every link on screen is "needed" and kept past the budget: the budget releases only links off screen or
far; a tile pyramid per link, not a whole link, is the unit that would bound it), 0.94 GB of GPU in all later.

## Amendment 6 (27 Nov 2026, ticket TILE1): a tile pyramid per link, baked at conversion, served by tile

The owner, on his folder of 17 linked IFC files (110.5 M triangles): the lab draws 68 to 74 M triangles per still frame
for 2.4 M pixels; the page's cost is triangle throughput; whole links are the unit of fetch, upload and release. "we should
have several resolutions like for tiling maps!". Choices by Claude on ticket TILE1 under the same delegation; the page's
frame over tiles is ticket VIS1's, a native viewer's NAT2's: both read what is below. Code: `ccore/src/tiles.{h,c}`
(builder, reader, walker), `hlod_grid.h` (hlod.c's clusters shared), `session/link_cache.c` (indexing),
`session/mesh.c` + `host/server.c` (answers), `src/wasm_api.c` (`host_tiles`), `web-ui/src/gl/tiles.ts` (decoder).

31. **The tree: a loose octree per link, large elements high.** Over the link's elements in its own frame (scene mm, the
   frame of its rows). A cell is split while it holds more than 40,000 triangles, copies counted (`CC_TILES_LEAF_TRIS`;
   the ticket's 20 to 60 k), at most 16 levels deep; an element goes to the child holding its centre when its largest
   extent fits the child's side (so it lies in the child's loose cube, twice the child), else it stays in the cell: such
   elements (slabs, facades, long walls) become the cell's OWN leaves, cut in Morton order of their centres into chunks
   of the same target. A cell with one octant and nothing of its own is not kept (the chain of empty levels collapsed).
   Every element with geometry is in exactly one leaf; each tile's subtree is one range of an element list in depth
   first order. Above the octree's root, a chain of coarser proxies of the whole, each at most a quarter of the one
   below, ends at 512 triangles or fewer (`CC_TILES_ROOT_TRIS`): a link seen from afar costs a few hundred.
   **Alternative:** hlod.c's cells (every element in a leaf by its centre: a slab in a leaf whose box is the slab's, the
   leaves' boxes overlapping) or a box tree (cells that do not halve: the error does not either).
32. **Proxies: vertex clustering on one lattice, each cluster placed where its quadric is least.** hlod.c's clusters
   (Rossignac & Borrel on power-of-two grids from the root's corner; a parent clusters its children's clusters) now carry
   their members' box and the sum of their faces' plane quadrics (area weighted), merged by addition; a cluster's point
   is the quadric's minimum by a pseudo inverse around the mean (Lindstrom 2000, "out-of-core simplification"), kept in
   the members' box. Each proxy at most a quarter of its children's triangles and 16,384 (`CC_TILES_PROXY_TRIS`), the grid
   coarsened until it fits; per material family, its parts' categories kept (the colour of far geometry).
   **Alternative:** quadric edge collapse per tile (Garland & Heckbert): not built. Measured why it would not change the
   numbers that matter: on the 20,026-element frame and the buildings scene the largest deviation of every level comes
   from parts that vanish below the grid (a 2.5 m wall, a 0.8 m chair: 70 to 100 % of each level's error, `TILES_DEBUG`),
   which edge collapse at the same triangle count removes too; quadric placement left those maxima unchanged (to the mm)
   and keeps corners where the cluster is not one face. Edge collapse is a few hundred lines more per tile and slower
   (a heap of edges per tile, against a sort of keys); left for when a picture shows the difference.
33. **The error: a sampled two-sided distance, not the grid's bound.** A proxy's error (mm) = the largest distance, over
   up to 16,384 points of the level below (each child's vertices and triangle centres, strided), to the proxy's
   triangles, and over the proxy's points to the level below's triangles (its children's clusters), plus the largest
   child error and half the 16-bit quantum's diagonal. A point of an element that vanished counts at most that
   element's middle extent (its omission draws no more wrong than its size). The walker needs it monotone: a parent's
   error includes its children's. Measured on the test's buildings scene against the real detail: proxy points at most
   0.62 of the stated error from the detail, detail points of surface-like elements at most 0.37 of it from the proxy
   (720 samples, none outside). **Alternative 1:** hlod.c's `grid × √3` (the bound of a point's move): measured 4.2 times
   the real worst; every site view of the synthetic sets then drew every leaf. **Alternative 2:** the farthest member of
   a cluster from its point (rigorous one way): 1.7 times the grid bound's tightness only, still 3 to 10 times what the
   picture shows on beams (points move along the surface, which changes nothing on screen). Cost: the build's time
   rises about five times on the synthetic sets (the distance queries: a 32³ grid of triangles per tile, rings searched).
34. **Leaves are not copied: a leaf is its elements' blocks in `ELEM`.** The tile table lists a leaf's elements; the answer
   (`TLF1`) gathers their BMG1 blocks from the mapped cache (each shape once per leaf, its copies as placements: copies
   stay instanced). Measured on the synthetic links (17 x 93 k triangles): the leaves as served 9.40 MB (5.9 bytes a
   triangle, edges included); a copy of them in the section would add those bytes to a cache of 22.36 MB (+42 %), or
   15.82 MB (+71 %) in a raw 21-bit layout; decoding every leaf in JavaScript took 142 ms (11 M triangles a second; a
   raw layout's copy 2.8 ms). **Alternative:** the leaves duplicated in tile order (one read a tile; a native viewer maps
   it raw): chosen against by bytes (the cache budget: under 15 % of the IFC) since a leaf's blocks are contiguous enough
   in the mapping and the decode happens off the drawing thread (worker, native thread).
35. **Proxies raw and uncompressed: `TPX1`.** u16 positions in the tile's box, u16 indices under 65,536 vertices: 9 bytes
   a triangle measured (synthetic links: 1.84 MB for 204,425 proxy triangles), uploadable as they are by a native viewer
   mapping the file. Deflate per tile saves 37 % on proxies (1.16 MB) but 91 % on the leaves' answers (BMG1's regular
   varints): left for HTTP (`Content-Encoding` on `tile=` answers when the web version streams over a network), not on
   disk. **Alternative:** BMG1 for proxies too (one codec): about the same bytes, a decode pass for every reader.
36. **Compact vertices for the GPU** (`tiles.ts` `pack16`, `pack21`; C `cc_tiles_lattice`, `cc_tiles_pack21`): proxies
   their stored u16 (6 bytes a vertex; the quantum ≤ 1/65,535 of the tile, far under its error); leaves on the link's
   1/16 mm lattice (BMG1's step), 21 bits an axis in two u32 (8 bytes), the shift growing only past 131 m: two leaves
   put a shared vertex on one lattice point (no crack; the test and `link_tiles_test` check it), within 1/32 mm of the
   cache's point. Bytes per triangle on the GPU, synthetic links (0.50 vertex a triangle): today's merged f32 positions,
   a u32 element id per vertex and u32 indices, 20 bytes; leaves as above with u16 indices, 10; proxies, 9.
   **Alternative:** u16 for leaves too (6 bytes a vertex): a 60 m leaf's quantum is 0.9 mm and a vertex shared by two
   leaves of different boxes lands up to 0.5 mm apart in each: a crack of a pixel at half a metre.
37. **Occluders: a separate conservative set per tile; proxies are never occluders.** Up to 64 real triangles a tile, the
   largest by area (at least 1/64 of the tile's extent squared), of opaque classes (`IfcWall*`, `IfcSlab`, `IfcRoof`,
   `IfcFooting`, `IfcColumn`, `IfcBeam`, `IfcCovering`; never the glass family); an inner tile keeps its children's
   largest. A subset of real surfaces never hides what is in front of one. 36 bytes a triangle, at most 2.3 KB a tile; built
   while the leaves are read (no extra pass). **Alternative:** shrunken proxies of opaque elements (a second simplified
   mesh per tile, never outside the solid: a constrained simplification); boxes of elements whose volume fills them (a
   wall with a window is not conservative).
38. **Back faces:** a flag per element (in the list's order) "closed and outward": the mesh welded by position, every
   directed edge met once by its reverse, the signed volume positive (a mirrored placement turns it: the reader flips
   the front face, as for any mirrored copy). Proxies' parts carry the field, 0 today (clustered meshes are not closed).
39. **The section and the cache's life** (ADR 0079 amendment): `TILE` written by the indexing job after `HLOD`, before
   `PRPZ`, from the cache's bytes read in place (one mesh decoded at a time; a shape once per leaf), in steps (cancellable;
   a crash leaves the incomplete cache, indexed again at the next open), deterministic (a build in steps of 777 or 1,000
   triangles gives the bytes of one build). `CC_LINK_CACHE_FORMAT` 7; a format 6 cache matches (its geometry sections are
   format 7's), is served at once and indexed again from itself (HLOD and TILE baked, SRCE turned complete and 7): never
   read again from the IFC (`link_tiles_test`). Readers skip `TILE` (Rust's and C's: a later addition); this one keeps
   its place (`has_tiles`, `tiles_off`, `tiles_len`) and leaves it on disk as `HLOD`. **The `HLOD` section stays** until the
   page draws by tiles (VIS1): the page is unchanged by this ticket; its BPRX could later be served from the pyramid's
   levels, then `HLOD` dropped.
40. **Serving** (`session/mesh.h`): `GET /mesh?link=k&name=&key=&tiles=toc` the table of contents; `…&tile=a,b,c` (4,096
   at most) a `TILS` answer; the link rule of SIZE2FIX (a named or keyed request is never a 404: 204 superseded, with
   `X-Superseded: 1`); 202 "indexing" while its job runs; 204 with `X-Tiles: none` for a large link whose cache has no
   `TILE`; a link without a cache (an IFC under 1 MiB, a `.bimbleview` linked as it is) gets its pyramid built at once
   when it has at most 2 M triangles. The open scene (a model, an IFC, a view file): `GET /mesh?tiles=toc&step=N` builds
   it in steps after the open (202 "done total"), then answers as a link; an opened view file that holds a `TILE` (a
   cache opened as it is) is served from it. The section is checked (`cc_tiles_check`) on every answer. The wasm call
   `host_tiles(link, budget, req)` gives the same answers (`ccore.ts` `ccoreTiles`; native: `bimble.ts` `fetchTiles`).
   `GET /mesh?tiles_frame=…` runs the reference walker over every link for one camera (the tools' and NAT2's check).
41. **The reference walker** (`cc_tiles_select`): the frontier starts at the root; the tile of largest projected error
   (error × k / distance from the eye to its box, k = viewport height / (2 tan(fov/2)); orthographic: × px per mm) is
   replaced by its children in view while that error is over the threshold and the frontier's triangles stay within the
   budget; a tile without a proxy, or one the reader marks (a hidden or selected element under it: proxies keep no
   element boundary), is always replaced; boxes out of the frustum are dropped; the answer front to back. 1 to 30 ms for
   the synthetic sets' cameras (including a JSON answer). It ignores occlusion: VIS1's depth pyramid over the occluders.

### The contract (VIS1, NAT2)

Section `TILE` (little-endian; offsets from the section's first byte):

| offset | type | field |
|---|---|---|
| 0 | char[4] | "TIL1" |
| 4 | u32 | version, 1 |
| 8 | u32 | nT, tiles |
| 12 | u32 | nE, the link's rows (BMSH row order = the view file's element order) |
| 16 | u32 | nL, the element list's length (elements with geometry) |
| 20 | u32 | nO, occluder triangles |
| 24 | u32 | toc bytes = 48 + 80 nT + 4 nL + pad4(nL) + 36 nO; the blobs start at pad8(toc) |
| 28 | u32 | the leaf target (triangles) |
| 32 | u64 | blob bytes |
| 40 | u32 | levels |
| 44 | u32 | 0 |
| 48 | 80 × nT | tiles, depth-first preorder (tile 0 the root; a subtree is ids [i, i + skip)) |
| … | u32 × nL | element rows, depth first (a subtree's elements one range) |
| … | u8 × nL, pad 4 | per list entry: 1 closed and outward, 2 opaque occluder class |
| … | f32[9] × nO | occluder triangles, link scene mm |
| pad8(toc) | blobs | a `TPX1` per proxy tile, in post-order (a subtree's proxies one byte range) |

A tile (80 bytes):

| offset | type | field |
|---|---|---|
| 0 | u32 | parent (0xFFFFFFFF: the root) |
| 4 | u32 | skip: tiles in its subtree, itself included (first child id + 1; next sibling c + skip(c)) |
| 8 | u16 | children |
| 10 | u8 | level (root 0) |
| 11 | u8 | flags: 1 LEAF (content: its elements' detail, `TLF1`), 2 PROXY (content: `TPX1`), 4 OWN (a leaf of its parent's large elements), 8 has occluders |
| 12 | f32[6] | box, min xyz, max xyz, link scene mm: holds its content and every descendant's |
| 36 | f32 | error mm: 0 a leaf; a proxy's (decision 33); +inf an inner tile without proxy (descend) |
| 40 | u32, u32 | el0, nel: its subtree's elements list[el0 .. el0 + nel) |
| 48 | u64 | its blob's offset from the blobs' start (4-aligned); 0 for a leaf |
| 56 | u32 | blob bytes |
| 60 | u32 | triangles its content draws (a leaf: copies counted) |
| 64 | u32 | vertices of its content (a leaf: own meshes and each distinct shape once) |
| 68 | u32, u16 | first occluder, occluders |
| 74 | u16 | 0 |
| 76 | u32 | detail triangles under it (saturating) |

`TPX1` (a proxy): "TPX1", u32 nv, u32 ni, u32 parts; per part {u32 category (concrete 0 … other 7), u32 flags (1 closed
and outward: 0 today), u32 first index, u32 indices}; u16[3] × nv (p = lo + q (hi − lo) / 65535, the tile's box); pad 4;
indices u16 × ni (nv ≤ 65,536) else u32. No edges, no normals (edges are found on screen).

Answers: `tiles=toc` the section's bytes [0, toc); `tile=a,b,c`: "TILS", u32 count, per tile {u32 id, u32 kind (0
none, 1 leaf, 2 proxy), u32 bytes, the bytes, pad 4}. `TLF1` (a leaf): "TLF1", u32 id, u32 nel, u32 blocks; per element
{u32 row, u32 block, f32[12] placement: columns x, y, z, then t; the identity for an own mesh}; u32 × blocks (bytes); the
BMG1 blocks (`meshcodec.h`; edges included, a decoder skips them). `tiles.ts` gives per leaf the own meshes merged
(one vertex block, one index block, runs of {row, first index, count}) and per shape its vertices, indices and instances.

What a reader does: read the table once per link (a few hundred kB: 0.32 MB for 17 synthetic links), walk it each frame
by `cc_tiles_select`'s rule (or its own with occlusion), ask the tiles it lacks in batches, draw proxies as they are
(u16 positions, the tile box as uniforms), leaves with their element runs (hide, select, pick by the state texel),
descend under a hidden or selected element (proxies have no element boundary), and use each tile's occluders before
deciding what to draw.

### Evidence (ticket TILE1)

`ccore/test/tiles_test.c` (C, also under ASan and UBSan), on a synthetic buildings scene made by the test (6 buildings of
4 storeys: slabs, facades, glass, furniture as two shared shapes placed turned and mirrored, thin pipes; 1,608 elements,
173,664 triangles), built from the scene and from its view file read in place: every element in exactly one leaf; every
leaf's answer is its elements' world meshes within the codec's 0.05 mm; the proxies' error honoured (above); each proxy at
most a quarter of its children, the root under 512; slabs no deeper than furniture; the same bytes twice and in steps;
the walker front to back, within a budget, a marked tile descended; 4,000 damaged sections and nine named damages (a
cycle, a child outside its parent, a range past the list, an element twice, absurd counts…) refused or read without a
crash; a 397,488-triangle element and non-finite vertices bounded; a build cancelled after one step freed.
`ccore/test/link_tiles_test.mjs` (host and `tiles.ts`): the phases (202 while indexing), the cache format 7 with `TILE`,
the table and every tile decoded by the page's decoder, the request rules, the walker's frame, a format 6 cache indexed
again from itself, a small link and the open model, 2,000 damaged answers in `tiles.ts`.

`ccore/tools/tiles.mjs`, native host (clang), 1920 × 1080, 60°: far = everything in view, close = a fifth of that
distance, inside = a third into the box at eye height (`ccore/out/tiles/tiles.md`, per-level pictures beside it):

| set | detail triangles | tiles (leaves / proxies), levels | triangles per level, root first | far at 1 px / 2 px | close at 1 px | inside at 1 px | pyramid bytes |
|---|---|---|---|---|---|---|---|
| 17 synthetic links (`links_synth.mjs`, 30 m apart) | 1,581,408 | 136 (85 / 51), 4 | 748 / 19,652 / 184,025 / 1,581,408 | 184,025 / 184,025 (1.66 MB to fetch) | 680,184 | 631,536 | 2.16 MB |
| mesh-heavy (700 × 1,000) | 695,264 | 88 (69 / 19), 5 | 148 / 3,936 / 32,438 / 230,146 / 584,040 | 302,102 / 230,146 | 391,112 | 375,072 | 1.46 MB |
| 20,026 elements | 270,288 | 38 (32 / 6), 4 | 152 / 1,108 / 46,568 / 270,288 | 270,288 / 270,288 | 227,040 | 161,844 | 0.55 MB |
| buildings (18 x 10 storeys, 400 parts a storey) | 6,691,680 | 627 (529 / 98), 7 | 60 / 1,584 / 7,620 / 52,968 / 318,410 / 3.3 M / 3.5 M | 6,691,680 / 6,165,312 | 3,965,580 | 3,771,300 | 4.86 MB |

The synthetic links' caches: 22.36 MB for 271.83 MB of IFC (8.2 %; `TILE` 2.16 MB, 0.8 %); indexing 4.0 s for the 17 (of
which the pyramids 3.6 s), the largest process's peak 77 MB. At 1 px the 20,026-element frame and the buildings draw
all their detail even from the site view: their beams, walls and furniture are 0.3 to 2.5 m and a pixel there is 0.15 to
0.25 m: the error rule is right to keep them; what is inside the buildings (95 % of the buildings scene's triangles) is
hidden, which is occlusion's work (VIS1's depth pyramid over decision 37's occluders), not the levels'.

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
  done (amendment 4); the open bounded and the detail on demand (amendment 5). Left: every link's mesh is still fetched
  once at open for its rows and boxes (a rows-only answer from the host would spare 227 MB of transfer on the local
  folder); a large link's detail is uploaded whole (slices of its blocks into `bufferSubData` would bound the page's
  peak to a block); tiles streamed by view (the link's HLOD leaves as the unit: the cache already orders them) would
  fetch only what the view needs. Proxies of a shared shape instanced, levels coarser than a cell's 4,096 triangles (a
  link at 50 px should cost a few hundred), 16-bit positions per cell: not built. Quantised vertices (16-bit positions in each leaf's box,
  8-bit normals) would halve the GPU bytes of positions; the state texel per element is already 4 bytes.
- **Screen-space edges** (in place of the edge pass): a pass of the element id and depth into a render
  target (WebGL2: an R32UI colour attachment beside the depth), then a full-screen pass drawing a line
  where the id or the depth's slope changes between neighbours; it needs the element id as a vertex
  output (amendment 3 gives it), `EXT_color_buffer_float` or integer targets, and a decision for
  coplanar faces of one element (the crease angle from the normals). Not built: amendment 3's rule
  (decision 20) halves the edge pass where it matters.
