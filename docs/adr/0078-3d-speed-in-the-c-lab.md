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
   element per edge vertex in `gpu/` (its edges know no element yet): the follow-up. *(Superseded on 27 Nov 2026 by ADR
   0083's amendment, ticket GPU2: WebGPU draws links from these same structures; nothing falls back for links.)*

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
   below, ends at 512 triangles or fewer (`CC_TILES_ROOT_TRIS`): a link seen from afar costs a few hundred. More than 8
   own leaves are grouped 8 at a time under tiles of their own (no tile has more than 16 children: a parent's proxy and
   error read its children at once; a link whose elements are all larger than its cells once made a tile of hundreds).
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
   picture shows on beams (points move along the surface, which changes nothing on screen). Cost: the distance queries
   (a 32³ grid of triangles per tile, rings searched, a vanished part's search stopped at its size) took 55 % of the
   build on the buildings scene. **The build is parallel** (`cc_jobs`, as the IFC reader: pthreads natively, in order in
   wasm): runs of leaves (post-order, a run bounded so that memory stays a few leaves' clusters) are read by workers with
   their own state, and the error's queries are split in chunks; each writes only its own slots, so the bytes do not
   depend on the threads (the test's builds in steps equal the one build). Buildings scene (6.7 M triangles): 12.9 s
   single-threaded to 3.5 s.
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
   while the leaves are read (no extra pass). Kept after the table of contents, not in it (`tiles=occ`): on the synthetic
   links they were 90 % of the table (0.29 of 0.32 MB), and the table is what every reader loads at open. **Alternative:** shrunken proxies of opaque elements (a second simplified
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

42. **Every optimisation a setting** (the owner's rule, 27 Nov): the walker's inputs are parameters, none baked in: the
   error in pixels (default 1 still), a triangle budget (default none), levels of detail (default on; off =
   `CC_TILES_SEL_LEAVES`, `tiles_frame&lod=0`: every leaf in the frustum, tested), occluders (default off in the reference
   frame query, `occ=1`; the renderers choose). The bake settings (`cc_tiles_opts`: leaf target 40,000, proxy cap
   16,384, root 512, grid 64 a side, occluders a tile 64, 0 = none) are recorded in the cache's `SRCE` ("tiles":
   "leaf=40000 proxy=16384 root=512 res=64 occ=64"); a cache baked with other settings is indexed again from itself
   (`link_tiles_test`). They come from `BIMBLE_TILES` today; their rows in Settings are left to the page's ticket.
43. **Room for the exterior shell (ticket SHELL1, not built):** element flag 4 EXTERIOR, tile flag 16 "has exterior
   elements" and the header's bake flags (offset 44, bit 1 "shell computed": a cache without it is unknown, never "found
   empty") are reserved; readers accept them; a proxy may later depict a subset of its elements.

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
| 24 | u32 | toc bytes = 48 + 80 nT + 4 nL + pad4(nL): the table of contents; the occluders follow it, the blobs start at pad8(toc + 36 nO) |
| 28 | u32 | the leaf target (triangles) |
| 32 | u64 | blob bytes |
| 40 | u32 | levels |
| 44 | u32 | bake flags: 1 the exterior shell computed (reserved for SHELL1; 0: not computed, the exterior unknown, never "found empty") |
| 48 | 80 × nT | tiles, depth-first preorder (tile 0 the root; a subtree is ids [i, i + skip)) |
| … | u32 × nL | element rows, depth first (a subtree's elements one range) |
| … | u8 × nL, pad 4 | per list entry: 1 closed and outward, 2 opaque occluder class, 4 exterior (reserved, SHELL1) |
| toc | f32[9] × nO | occluder triangles, link scene mm (outside the table: asked apart, `tiles=occ`) |
| pad8(toc + 36 nO) | blobs | a `TPX1` per proxy tile, in post-order (a subtree's proxies one byte range) |

A tile (80 bytes):

| offset | type | field |
|---|---|---|
| 0 | u32 | parent (0xFFFFFFFF: the root) |
| 4 | u32 | skip: tiles in its subtree, itself included (first child id + 1; next sibling c + skip(c)) |
| 8 | u16 | children |
| 10 | u8 | level (root 0) |
| 11 | u8 | flags: 1 LEAF (content: its elements' detail, `TLF1`), 2 PROXY (content: `TPX1`), 4 OWN (a leaf of its parent's large elements), 8 has occluders, 16 has exterior elements (reserved, SHELL1) |
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

A proxy may later be built from a subset of its elements (SHELL1: the exterior shell only): its element range is what it
stands for (what to descend into under a hidden or selected element), not necessarily what it depicts.

`TPX1` (a proxy): "TPX1", u32 nv, u32 ni, u32 parts; per part {u32 category (concrete 0 … other 7), u32 flags (1 closed
and outward: 0 today), u32 first index, u32 indices}; u16[3] × nv (p = lo + q (hi − lo) / 65535, the tile's box); pad 4;
indices u16 × ni (nv ≤ 65,536) else u32. No edges, no normals (edges are found on screen).

Answers: `tiles=toc` the section's bytes [0, toc); `tiles=occ` the occluders (36 nO bytes); `tile=a,b,c`: "TILS", u32 count, per tile {u32 id, u32 kind (0
none, 1 leaf, 2 proxy), u32 bytes, the bytes, pad 4}. `TLF1` (a leaf): "TLF1", u32 id, u32 nel, u32 blocks; per element
{u32 row, u32 block, f32[12] placement: columns x, y, z, then t; the identity for an own mesh}; u32 × blocks (bytes); the
BMG1 blocks (`meshcodec.h`; edges included, a decoder skips them). `tiles.ts` gives per leaf the own meshes merged
(one vertex block, one index block, runs of {row, first index, count}) and per shape its vertices, indices and instances.

What a reader does: read the table once per link (80 bytes a tile and 5 an element: 32 kB for 17 synthetic links; their
occluders, 0.29 MB, apart), walk it each frame
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

## Amendment 7 (27 Nov 2026, ticket VIS1): draw only what can be seen, edges found on screen, moving follows input

Pierre, on his project of 17 linked IFC files (252,189 elements, 110.5 M triangles): "it feels like it showing way way
too many triangles", then four performance reports (moving: none; 4.3-74 M triangles a frame in 1,136-6,233 draws;
input → frame 400-730 ms; proxies drawn 0; boxes while orbiting, then a refill "one by one ... taking ages"). He
delegated the choices ("performance is everything"; "do what games do"); they were made by Claude on ticket VIS1 with
the orchestrator, each measured on the machine's GPU (Apple M1 Pro, Mesa, ANGLE on Vulkan, headless Chromium 153;
`ccore/tools/drive.mjs`, the GPU's time by `EXT_disjoint_timer_query_webgl2`), to be reviewed on the implementation.

44. **Moving follows input** (P0-a). His stored settings held `moving_fps` 0 ("Draw everything"): `frameBudget()` was
   Infinity, so no frame was ever a moving one and each orbit or wheel step drew the whole picture; the panel said
   "moving: none" truthfully. A frame within max(250 ms, twice the last frame's time) of a camera change is now in
   motion, and the fill waits as long after the last input (a held pointer, a wheel step, a key). ADR 0074's 200 ms
   is a floor for every setting: with "Draw everything" the whole picture is drawn while moving as long as it is
   estimated under 100 ms (its triangles over the rate a fence measures on whole frames); above, a budget sized to
   60 ms applies and the complete picture comes once the input stops. The stored value is unchanged; the Settings
   note under that choice says so; the panel shows those frames on their own row. **Alternative:** the camera key
   alone (the cause: a slow frame or a setting turned it off), or ignoring the setting (silent).
45. **Edges found on screen** (in 3D; plans keep their lines). The frame's faces are drawn into an offscreen target
   (`gl/target.ts`: colour, the element (index and model), the face's normal, depth), then one full-screen pass
   presents the colour and draws an edge where the element changes between neighbouring pixels (the nearer one's
   side), where an element bends by more than the core's crease angle (20°) and at silhouettes. The line pass was
   55-65 % of a whole frame's GPU time: 17.2 → 7.7 ms on 17 synthetic buildings (7.67 M triangles, his window
   1492 × 790 at device pixel ratio 2), 2.25 → 1.0 ms a million triangles; the pictures look the same
   (`drive.mjs` screenshots). A coplanar joint between two elements (facade panels) stays an edge, which a
   depth-only test (Rust's `edges.wgsl`, tried first: depth copied by `blitFramebuffer`) loses. **Alternative:** line
   edges only where the batch is large on screen (decision 20 already did; still 55 %).
46. **The pick reads the element attachment**: one pixel of the last complete picture at this camera, then the ray
   against that element alone (its point); the background answers at once; a proxy's pixel or another camera falls
   back to the scan. The status bar's readout while orbiting reads it too (without the scan: it took 0.7 s of a
   session). **Alternative:** the core's box tree (asynchronous; SIZE3 decodes a cold link on demand).
47. **What the last picture hides is left out** (`gl/hiz.ts`, `gl/visible.ts`; the setting `draw_visible`, OFF by default:
   see below; not one of Rust's fields, written once set). After a picture is presented its depth is reduced on the GPU to the farthest
   depth of each 4 × 4 pixels (a proxy's pixel and the background count as the farthest: a proxy is not a safe
   occluder), read back without waiting (a pixel-pack buffer and a fence), the coarser levels made on the CPU. A still
   frame or a slice of the fill at the same key (camera, size, what is shown, lists, section box, links) leaves out
   each element whose box's nearest depth is farther than the farthest over the blocks it covers, or which lies wholly
   outside the picture; the kept items go as ranges in one `multiDrawElementsWEBGL` per model (copies: a draw a run,
   a gap under 2,000 triangles drawn). Every element that drew a pixel passes its own test, so the picture is the
   same: 0 pixels differ and 0 seen elements are missing on every camera measured (`lab_visible.mjs`: 60 cameras
   with fast turns). The pyramid is made once per key, and again when the picture completes after a partial one (a
   moving frame's: its largest elements are the occluders). Not in a plan; a section box's cut depth is the one
   tested. Moving frames are not tested (another camera: reprojection is not built). **Alternatives:** occlusion
   queries per batch (decision 12: the open model only, a draw and a query per batch, a frame late); a CPU raster of
   occluders (Rust's #5: 1-3 ms a frame of CPU, 42 → 38 % drawn close up); the baked occluder triangles of TILE1
   (below: the next step).
   **Off by default**: exact on every synthetic camera, it is not yet on a real project of 17 links (a local folder of
   IFC files, the ground truth with proxies off: a close camera left thousands of seen elements undrawn, long members
   missing from the picture, the same on a second pass); the cause is not found (see "What is left"). Until it is, the
   setting is the user's choice and the panel shows what it leaves out.
48. **A moving frame shows proxies, not boxes**: a link's batch it could not finish is drawn as the proxy of the
   deepest usable cell holding its leaf (half the budget at most); outlines stay where no cell has one. The fill after
   a stop draws offscreen: the canvas keeps the moving picture and shows the target when the fill is complete or every
   250 ms meanwhile (no batch-by-batch refill on screen); with edges found on screen the fill needs no last pass of
   lines. **Alternative:** reprojecting the last complete picture under the moving one (not built: a frame of the
   current budget already shows the building; measured worth less than the triangles saved).
49. **The budget's cut for tens of thousands of batches** (`gl/cull.ts budgetCutFast`): budgetCut bisected 60 times
   over every batch (a project of 17 links has tens of thousands: 39 ms a frame for 46,800 in `cull_test`; on such a
   project the fill's slices became script-bound and the picture was never complete at moving_fps 30). Items are counted once per state in buckets of a sixteenth of an octave of their size,
   a frame adds them shifted by each batch's distance, reads the threshold and checks it exactly (one search a batch):
   the same triangles as the bisection on the 20,026-element model, 7.8 ms against 39 ms for 46,800 batches
   (`cull_test`). The batch loop allocates nothing a batch; each model's ranges come from its own batches.
50. **No front-to-back order** (a bench hook keeps it): Apple's GPUs remove hidden surfaces per tile, so the order gains
   nothing there (the literature review of 27 Nov), and with culling it changed which of two coplanar faces won the
   depth tie: the edges found on screen moved (up to 1,528 pixels on a close camera of the sweep, 0 elements missing).
   Without it the culled frame draws the kept items in the order the whole frame does.
51. **Every optimisation a setting with an exact value** (Pierre: "we should provide good default but allow user a lot
   of latitude"): `draw_visible` (on), `edge_mode` (`screen`, the default; `lines`, exact; `none`), `cull_back` (off:
   exact only for closed solids, and the core does not say which shapes are), `still_px` (0 = off .. 8 pixels: what a
   still frame leaves out; 0 by default: the proxies at 1 px already stand for the far cells), `refill_offscreen` (on),
   with the existing `moving_fps` (0 = draw everything, kept under 100 ms); `draw_visible` is off by default (decision
   47), `moving_proxies` and `link_gpu_mb`. None
   is one of Rust's fields: each is written to settings.json only once set (as `renderer`). The Settings dialog has
   their rows; the performance report and every `drive.mjs` summary name the values in force (its A/B axes).
52. **`ccore/tools/drive.mjs`**: the lab driven by real gestures on the machine's GPU and recorded (frames, inputs,
   CPU profile, memory, network, screenshots, summary; `--compare`; an MCP face). It waits for memory outside the
   shared measuring lock, takes the lock with `flock -n` (its holder ends with the tool), gives up after 20 minutes
   ("machine busy: not measured"), caps Chromium in a cgroup for a project, and writes nothing beside the user's files
   (a project's link caches in its own folder, `BIMBLE_LINK_CACHE_BESIDE=0`: a first version wrote them beside the
   owner's IFC files once, 27 Nov 13:51, after a rebase changed the cache format). Chromium gets a short TMPDIR of
   its own (`test/lib/chromium.mjs chromiumEnv`: its singleton socket's path overflowed under a deep scratch folder).

### Evidence (ticket VIS1)

The cost of a draw call here (`drive.mjs drawcalls`: every link's whole face list, 4.85 M triangles, in N calls; the
GPU, ANGLE Vulkan): 6,001 calls 10.4 ms (2.4 ms of script), 595 calls 4.1 ms, 68 calls 4.5 ms, 17 calls 5.2 ms, 17
multi-draws of 6,000 ranges 5.7 ms. Draw calls cost about 5 ms at his 6,000: his frame is bound by triangles, not
draws; "merge all the 3D in one" saves at most that.

Whole frames on the 17 synthetic buildings (GPU ms; `drive.mjs throughput`): line edges 17.2, faces only 7.4-7.7,
edges on screen 7.7, without the state texel 7.3 (17 of 18 models skip it already), back faces culled 5.5-5.9
(−25 %: not on, the core says which shapes are closed only with TILE1), device pixel ratio 1 instead of 2: 5.8 (faces
only): not built.

What a still frame submits against what can be seen (the ground truth: every shown element's faces as ids, read back):
17 synthetic buildings (83,419 elements, 7.67 M triangles), his window at dpr 2, ANGLE Vulkan, Chromium 153; still ms is a whole
frame then a 1×1 readPixels (median of 5), GPU by timer query; "preview.6 like" = line edges, nothing left out; the
pixel count compares each picture with "all" of its camera (edges on screen, nothing left out):

| camera, option | still ms (GPU) | triangles submitted | elements seen (their triangles) | submitted / seen | pixels differing from all | seen, not drawn |
|---|---|---|---|---|---|---|
| far preview.6 like | 17.9 (15.0) | 7,665,028 | 1,638 (58,208) | 132 | (lines) | 0 |
| far all | 9.2 (6.9) | 7,665,028 | 1,638 (58,208) | 132 | 0 | 0 |
| far visible | 21.4 (5.0) | 3,119,740 | 1,638 (58,208) | 54 | 0 | 0 |
| far visible px1 | 12.6 (4.3) | 3,191,264 | 1,638 (58,208) | 55 | 0 | 0 |
| far visible cullback | 19.2 (3.7) | 3,191,264 | 1,638 (58,208) | 55 | 2,041 (not exact) | 0 |
| close preview.6 like | 13.9 (8.0) | 1,349,300 | 123 (1,476) | 914 | (lines) | 0 |
| close all | 12.0 (5.8) | 1,349,300 | 123 (1,476) | 914 | 0 | 0 |
| close visible | 11.9 (6.0) | 208,448 | 123 (1,476) | 141 | 0 | 0 |
| close visible px1 | 7.1 (4.3) | 208,448 | 123 (1,476) | 141 | 0 | 0 |
| close visible cullback | 6.4 (3.4) | 208,448 | 123 (1,476) | 141 | -4,427 (not exact) | 0 |
| inside preview.6 like | 10.0 (7.9) | 380,872 | 9 (392) | 972 | (lines) | 0 |
| inside all | 10.1 (7.4) | 380,872 | 9 (392) | 972 | 0 | 0 |
| inside visible | 11.4 (6.4) | 200,384 | 9 (392) | 511 | 0 | 0 |
| inside visible px1 | 13.3 (8.4) | 200,384 | 9 (392) | 511 | 0 | 0 |
| inside visible cullback | 10.1 (6.3) | 200,384 | 9 (392) | 511 | -2,252 (not exact) | 0 |
| far visible, 4 × 4 blocks | 21.3 (4.9) | 2,331,824 | 1,638 (58,208) | 40 | 0 | 0 |
| close visible, 4 × 4 blocks | 9.0 (4.5) | 174,108 | 123 (1,476) | 118 | 0 | 0 |

The rows above were drawn with 8 × 8 blocks but the last two (4 × 4, the default). A first version read the background as the
nearest depth (an encoding of 1.0 that overflowed 24 bits): 216-225 seen elements were not drawn on the far camera; found by
the ground truth, fixed before anything was turned on. The inside camera keeps 200 k triangles: elements of a batch in view
but beside the picture or behind the eye were not left out then (they are now: `boxHidden`).

The 60-camera sweep (`lab_visible.mjs`, `LAB_GPU=vulkan`, Chromium 158, 6 synthetic buildings at 1280 × 800, proxies off:
the culling's own truth): 0 seen elements missing, 0 pixels differ (at all) on every camera, this camera's pyramid used
on 60 of 60, the moving frame's pyramid used by the first still frame after fast turns on 12 of 12; triangles submitted
far 54.1 → 31.4 M (the seen elements' 0.68 M), close 17.0 → 2.7 M (13,380), inside 5.4 → 0.24 M (24,604). With the far
cells' proxies at 1 px (still) the far view differs from its full detail on 289 of its 12,310 drawn pixels (2.3 %; the
test's bound: 5 %; 3D2's proxies, not this amendment's culling). The first sweep showed 2,777 "missing" elements on far cameras: elements stood for by those
proxies (not culled: 0 pixels changed), and up to 1,528 pixels changed on close cameras: decision 50's sort.

### What the WebGPU renderer needs to use the same visible set

`gl/visible.ts` (the masks and runs) and `gl/hiz.ts`'s `boxHidden` and pyramid levels take no WebGL: they read a
`Pyramid` (key, the finest level's depths, the view-projection) and the batches. The WebGPU renderer (`gpu/`, ticket
GPU2) needs: (1) its own offscreen target with the element and normal attachments and the same present pass (WGSL)
for edges found on screen (until then lab_webgpu compares it with WebGL2's line pass); (2) the reduce of its depth to
4 × 4 blocks (a compute pass) read back by `mapAsync` (no fence to poll), giving the same `Pyramid`; (3) the masks
fed to its GPU culling: a bit per element in the storage buffer its compute pass already reads (a hidden element's
copies and ranges dropped there), or the runs turned into its indirect draws; (4) the pick: one texel of the element
attachment (`copyTextureToBuffer`, `mapAsync`).

## Amendment 8 (27 Nov 2026, ticket SHELL1): the exterior shell, per link and across the links

The orchestrator's ticket, on the owner's folder of 17 linked IFC files (110.5 M triangles): the pyramid alone leaves an
outside view at 76.1 M triangles at 1 px (32.0 M with the per-tile occluders): the buildings' parts are too large on
screen to simplify and most triangles are inside. The research note's first technique (`docs/research/2026-11-27-massive-model-rendering-literature.md`):
bake which elements can be seen from outside at all. Choices by Claude on ticket SHELL1 under the owner's rules ("performance
is everything"; decisions on facts measured on his data; every optimisation a setting with a good default and an off value).
Code: `ccore/src/shell.{h,c}` (the classification, pure), `src/tiles.{h,c}` (the section, the walk, the answers),
`session/link_cache.c` (the bake), `session/xshell.{h,c}` (the cross-link pass), `session/mesh.c` + `host/server.c`
(answers, the frame query, the truth render), `ccore/tools/tiles.mjs --shell` (the numbers).

53. **The classification: orthographic depth views over the sphere, an element seen in one of them is exterior.** For N
   directions (the six sides, the eight corner diagonals, then a Fibonacci sphere; no libm: the golden angle's rotation by
   recurrence) an orthographic view of the link's bounding sphere at R x R pixels. Pass 1: the opaque elements write their
   depth at the pixel centres they cover (both faces). Pass 2: every element not yet exterior is tested, its box first (hidden
   when every 8 x 8 block it covers holds only surfaces nearer than its nearest point by more than d), then its triangles:
   seen when one is no more than d mm behind the buffer at a pixel centre it covers; a triangle that covers no pixel centre
   (a part thinner than a pixel) is tested with conservative coverage at the depth of its point nearest the pixel's centre,
   unless it faces the view edge on (|n . f| < 0.05: its neighbours show, or not). **Opaque = the occluders' rule** (decision
   37: `IfcWall*`, `IfcSlab`, `IfcRoof`, `IfcFooting`, `IfcColumn`, `IfcBeam`, `IfcCovering`, never the glass category): windows,
   curtain walls' plates and members, doors, railings, stairs, furniture and MEP never hide anything, so what is behind a
   window is exterior. **Why orthographic and complete**: a ray from an eye outside the link's box to a point inside meets the
   box's hull once, so the whole line in front of the point lies outside the hull: the view along that direction sees what the
   eye sees. Only the sampling can miss (a direction between the samples, a sliver behind an edge). The depth pass skips an
   element whose box is behind every block it covers (exact: it would write nothing; the pyramid's order draws a cell's large
   elements first). Bounded: a batch of views' buffers (at most 64 MB), a run of leaves' placed triangles (400,000); work views
   x triangles; deterministic (a view drawn by one worker in the elements' order, marks or-ed: the same bytes for 1 and 4
   threads and in steps, `shell_test`). **Alternatives:** perspective viewpoints on a sphere (Pixyz: 256 at 512 px), which
   sample eye positions where the orthographic views sample lines (complete in the limit); ray casting from the outside
   (two-stage recursive tracing); voxels (needs closed solids, a voxel's size of error); an id buffer with one pass (what is
   behind glass and thin parts need the second pass anyway); the "touch" safeguard (an element reaching the link's box faces
   counted exterior: built, `cross_touch=1`, measured below, off).
54. **The defaults, by measurement: 64 views at 1024 px, d = 100 mm.** On the synthetic buildings (6.69 M triangles, 18
   buildings of 10 storeys with interiors, walls 300 mm in a 200 m scene) 64, 256 and 1,024 views at 1024 px mark the same
   8,298 of 73,260 elements (14.7 % of the triangles), 0 missing in 31 truth frames; at 512 px the walls (about a pixel) leak:
   15.6 k (24 %), still 0 missing: the resolution matters, not the views. On the local folder the cross-link pass's views do
   not converge (below): more directions keep finding gaps (glazing, joinery, open fronts).
55. **Stored in the reserved places, the layout unchanged** (`tiles.h`): element flag 4 EXTERIOR, tile flag 16 "holds
   exterior elements", the header's bake flag 1 "shell computed". The exterior triangle counts and the shell proxies do not fit
   the record (one free u16): the blobs end with a SHELL TABLE (`TSH1`: per tile its exterior detail triangles and its shell
   proxy: offset, bytes, triangles, vertices, error) found by a 16-byte trailer (`TSHT`, table bytes, offset). Readers of the
   layout before it read the new sections unchanged (the blobs only longer; the reserved flags already accepted): the cache's
   format stays (8 since TESS1); the SRCE record of the bake settings gains `shell=64x1024 shell_d=100`, so a cache baked
   before is indexed again from itself, never converted. The shell proxies (a tile with both kinds: a proxy of its exterior
   elements only, the same tree built with the others left out) are kept beside the tiles' own and used by the experimental
   filter only: **the coarse levels the renderers draw stay the tiles' own proxies, from all elements** (a proxy of the
   exterior only would hide what a section or a window reveals; nothing measured shows it safe). Bytes: about 12 % of a
   `TILE` section on a real project, 0.95 MB on the buildings set.
56. **Across the links.** On the local folder (17 links of one project split by discipline) the links' own shells marked
   97.6 % of the elements exterior, **99.2 % of the triangles**: an MEP, structure or joinery file has no envelope of its
   own. So a cross-link pass is built (`session/xshell.{h,c}`, the same code: `cc_shell_start_parts`): the visible links
   classified together, each link's cache read in place and placed, every link's opaque elements hiding the others', in
   groups of links whose world boxes overlap (grown by 1 m: a link far from the others, a georeference kilometres away, made
   every view's pixel metres wide: the first run left 26 elements exterior, the truth render caught it), in a background job
   started when the visible links, their caches, placements or the settings change; per link a mark per entry of its element
   list (`toc.xmask`), the link's own shell the fallback; kept in the cache folder (`links/xshell-<key>.bin`, read untrusted),
   not beside the `.bimble` (the project's folder may be the user's and read only; it is a cache); `tiles=xshell` (202 while it
   runs); settings `cross=0|1|<views>x<res>`, `cross_d=`, `cross_touch=` in `BIMBLE_TILES` (not in a cache's SRCE). On the
   local folder it marks about two elements in five, in under half a minute and 106 MB of buffers (the numbers: the ticket's
   report, private data).
57. **The shell orders, it never removes (the orchestrator's decision on these numbers).** As a FILTER (the outside walk:
   tiles without exterior elements skipped, leaves drawn by their exterior elements) the cross-link shell selects 29 M
   triangles at the site view instead of 77 M (17–19 M with the occluders), but **misses up to 1.2 % of the elements seen**,
   and large ones (coverings, slabs, a pipe, tens of metres long). Neither 2048 px nor d = 400 mm nor the box-touch safeguard brings it to zero (fewer misses,
   never none), and the views curve does not converge (from 32 to 256 views the marks keep growing: more directions find more
   gaps). The native renderer's run-time occlusion on the
   same project draws a few million triangles with 0 missing (its truth frames, ticket NUI2). So: **`shell_mode` = `priority` (the default) | `off` |
   `filter` (EXPERIMENTAL, off: "can hide visible elements: measured 0–1.2 % on a real project")**. In priority the walk
   (`CC_TILES_SEL_PRIORITY`) returns the same selection as without the shell, ordered exterior first (the tiles holding an
   exterior element, front to back, then the others); `&tile=...&shell=1` answers a leaf's exterior elements and a proxy whole
   (the first fetch), `&shell=2` the leaf's other elements (what follows): after an open or a camera jump a renderer fetches
   and draws what is probably seen first, and its run-time occlusion decides what is seen. A miss of the shell is a later
   arrival, never a hole. What it buys (triangles fetched before every element the truth render sees is drawn, plain order
   against exterior first): synthetic buildings, site view 6.57 M against 0.98 M, approach at 20 m 2.14 M against 0.29 M;
   on the local folder 1.1 to 3.4 times fewer (the shell's misses arrive with the rest, front to back: they set the figure).
   Rows in the page's Optimisations registry and the native app's Optimisations group.
58. **What the shell does not guarantee.** It is sampled: a part seen only through a gap narrower than a pixel of the views,
   or only from a direction between two sampled ones, is not marked (in priority it arrives later). It assumes the occluder
   classes are opaque where the model says so: an open door leaf (an `IfcDoor` never hides) makes more exterior; a wall standing
   where an opening was not cut hides what is behind it. Its marks hold for an eye outside the link's (or group's) box only; a
   moved, added, hidden or re-read link starts the cross-link pass again (the links' own shells meanwhile). It decides an order,
   not visibility: the renderers' occlusion does.
59. **Back-face culling proven per element.** The element flag 1 "closed and outward" (decision 38) is checked by
   `cc_shell_cull_proof`: each flagged element drawn alone from 26 sphere directions, whole and culled; no pixel may be deeper
   culled, at most 1 % of the pixel centres left by edge rounding (a centre on an edge both faces leave). `shell_test` proves
   every flagged element of the golden IFC files and fixtures, on the reader's meshes and on the cache's (BMG1 read in place),
   and any folder named by `SHELL_PROOF_DIR`: 397 and, on the 17 synthetic links, 8,285 flags hold, 0 fail; at a few pixels
   across, 0 of 187,106 views lose an element by culling. A renderer losing elements with culling has another cause (the
   front face's convention under a flipped Y, mirrored copies not flipped, the flag read by row instead of list entry).

### Evidence (ticket SHELL1)

`ccore/test/shell_test.c` (also under ASan and UBSan): a closed room's contents interior, a window's view exterior, an open
courtyard, nested shells, the section with its shell (flags, table, walks, answers, 2,000 damaged tables), hostile links
(a zero-size link, non-finite points, 100,000 tiny elements in 1.0 s within 31 MB), the same bytes with 1 and 4 threads and in
steps, the cross-link pass (a thing inside another scene's room, turned and moved: exterior alone, interior together), the
priority walk, the culling proofs. `ccore/test/link_tiles_test.mjs`: the bake, the table, the first fetch and the rest
together each leaf, the walker's modes, the truth render (0 missing in priority), the invalidations (a section box, hidden
exterior elements), the cross-link pass (two links), `shell=0`.

`ccore/tools/tiles.mjs --shell` (native host, 1920 x 1080, 60°, 1 px; orbits at the site distance, a half and a quarter of it,
8 azimuths, 25° up; an approach to 2 m of the box's face; one inside camera):

| set | camera | off | off + occ | filter: own shells | filter: cross-link (+ occ) | filter missing | priority missing | first picture plain / priority |
|---|---|---|---|---|---|---|---|---|
| buildings (synthetic) | site orbit | 5.5–6.7 M | 3.9–5.4 M | 0.82–0.94 M | (one scene) | 0 | 0 | 6.57 M / 0.98 M |
| buildings | approach 20 m | 3.90 M | 1.00 M | 0.57 M (0.17 M) | | 0 | 0 | 2.14 M / 0.29 M |

Local folder: priority misses 0 in every frame (by construction: the same tiles); the filter's figures above; the rest in
the ticket's report (private data). Bake: a link's own shell takes seconds per 10 M triangles at 64 views x 1024 (buffers at
most 106 MB; the indexing peak stays the pyramid's); the cross-link pass about as long over the links together. Synthetic links (17 x 93 k, objects apart): 100 % exterior, as they are. The viewer wasm:
637,787 bytes after the merge with `next` at 9161472 (shell 635,764 against 612,384 on next at a490b49; budget 1,000,000).

## Amendment 9 (27 Nov 2026, ticket VIS2): the page's hidden-surface skipping exact on a real project; the tile path planned

The orchestrator's ticket: find why `draw_visible` dropped visible elements on the owner's project of 17 linked IFC files
(the truth test: thousands of seen elements not drawn, 17 lost by the frustum test alone), fix it at the source, then draw
links from the baked tile pyramid. Choices by Claude on ticket VIS2 under the owner's rules (decisions on facts measured
on his data; every optimisation a setting with a good default and an off value).

60. **The box fault was float32 at georeferenced coordinates.** The page's scene frame is the open model's; the owner's
   open model is empty (origin 0) and 16 of his 17 links are placed at Swiss national coordinates, 2,536 km east and
   1,153 km north, where f32 steps are 256 mm (2^31 to 2^32 mm). The GPU computed `R p + t` and `u_mvp p` in f32 there,
   the CPU's view and projection were `Float32Array`s (a rotation entry rounded at 1e-7 is 250 mm at 2.5e9), and the
   element and batch boxes were `Float32Array`s rounded to 256 mm (one missing element's box had zero width in x and y).
   The depth pyramid (the GPU's picture) and the boxes (the CPU's) disagreed by up to hundreds of millimetres: thin and
   flush elements were found "behind" what they are in front of, boxes at the picture's edges outside it. Ruled out by
   measurement first (`bench.boxAudit`: every element's box rebuilt from its link's mesh asked again, placed by its
   placement: 0 of 21,192 / 783 / 30,295 elements outside the page's own-frame box): SIZE3's `preBox`, turned and
   instanced copies, the host's rows answer. **Fix: the render origin** (`gl/origin.ts`). The CPU keeps the scene frame in
   f64 (matrices, `boxes`, batch boxes; the own-frame `lboxes` stay f32: small numbers); the GPU draws relative to a point
   near the geometry, the scene's bounds' centre on a 1 m grid when it is more than 1 km from the origin (else 0, nothing
   changes): `u_mvp = f32(mvp T(ro))`, a link's translation `t - ro` (f64, then f32: small), the open model's vertices
   `p - u_ro`, the eye, the section box's frame and the outlines relative to it. The WebGPU renderer takes the same
   (`Frame.ro`, its rows' `t - ro`). **Alternative:** the open model's origin moved to the links (every coordinate the
   page exchanges with the host would move: section box, measure, notes, place tool); f64 emulation on the GPU (two
   floats a coordinate: twice the vertex bytes).
61. **Copies drew their link's first shape** (the second fault, found behind the first). A link opened from its rows only
   (SIZE3's path: every large link) had its instanced groups made before its shapes were packed (`shapeI0` still 0): each
   group's first index 0, every copy drew the link's first shape at its own placement, and the truth test counted pixels
   where those elements' boxes are not. The groups' offsets are read again whenever their model's buffers load.
62. **A link's detail coming back keeps the pyramid.** `detailGen` (part of the pyramid's key) moved on every load and
   release of a link's detail, so the first still frame after fast turns often had no pyramid (the real-GPU check's
   "11 of 12"). A load only adds geometry to a picture that drew that link as proxies, which the pyramid counts as the
   farthest depth: that pyramid stays conservative. A release, a move or a change of lists still makes it stale.
   `stats.vis.why` says which part of the key differed. The check is the invariant: reused unless made stale.
63. **An item larger than the budget is drawn alone, always.** `budgetCut`/`budgetCutFast`'s never-empty branch set the
   threshold to the largest item's size over its distance; `(s / d) * d` can round above `s`, and that item was then left
   out: a still picture's fill drew 0 triangles every frame, forever (seen on the owner's project after a zoom out: 50 ms
   slices, the page never idle). The threshold is now a hair under.
64. **`draw_visible` on by default** (the core's default: the page and the native app read it; written to the file only
   once set off). The owner's project, 6 cameras (close, far, inside, each at two azimuths), proxies off: 0 missing, 0
   pixels differ.
65. **The web session log** (the owner's "so that when I try the app you have observability"): with the native host the
   page posts its start, errors and warnings, a row of frame and input statistics a second, inputs over 100 ms and its
   Performance report every 10 s; the host writes them under `$XDG_STATE_HOME/bimble/web-sessions/<UTC>-<pid>/`
   (`session.log`, `frames.csv`, `inputs.csv`, `report.txt`: the native app's session shape), local only (the token and
   Origin rules of every route), at most 20 MB a session, the last 20 kept; the setting `session_log` (on; off: nothing
   written); nothing in the pure web or wasm build. Not on Windows yet.
66. **A fix found on the way:** `cc_settings.shell_mode` was `char[8]` for "priority" (9 bytes with its NUL): the NUL
   landed in the next field (harmless while it was padding; the new `session_log` field made it visible). Now 12.

67. **The page reaches idle on a project of links.** After an orbit on the owner's project the page drew a fill of slices
   for minutes: each slice cost about 50 ms of script for its 46,800 batches whatever it drew, so the triangles-per-ms
   rate the slices measured on themselves fell to the minimum budget (20,000) and each slice drew a few thousand
   triangles; and the cut counted the triangles of items the visibility test then leaves out. Now the visibility test is
   decided before the cut, which counts only what may be drawn (`Cut.cum`: the shown and not hidden items' triangles),
   and a slice gets at least a whole frame's rate (fenced whole frames) times a moving frame's time. **Alternative:** a
   rate model with a fixed cost per frame (needs the script's share measured per frame: left for the tile path, which
   removes most of that cost).
68. **Late proxies do not discard detail on screen.** A link whose proxies come after its detail is on the GPU keeps that
   detail when its proxies would not stand for all of it from the camera of the moment; the proxies wait and are swapped
   in (its batches made again by their leaves) when the budget next releases it. A link the proxies stand for entirely
   (a far one) gives its detail back at once, as before. (`lateProxyRelease`, a test hook, restores SIZE3's way.)
69. **The status bar says when the budget holds links at their proxies**: "N links simplified: 768 MB GPU budget reached"
   (with a tip: raise "Links' GPU memory" in Settings), shown only while the links' detail on the GPU is at the budget.
70. **The opening card for a link in the address is drawn by `index.html`** (an inline script, while the HTML is parsed;
   the app's mount replaces it before the next paint): GitHub's runner measured 217.8 ms to the card when it waited for
   the bundle (ADR 0074's 200 ms). `lab_open_url` checks it with the CPU throttled 4, 6 and 10 times.

71. **Inputs on a project of links** (measured on the owner's project with `drive.mjs`, each cause in a CPU profile):
   the status bar's pointer readout read a pixel with a synchronous `readPixels` every frame the pointer rested on the
   canvas, waiting for the GPU behind the frame in flight (up to 840 ms of a click's 2.6 s): it now waits while a frame
   is in flight or about to be drawn. Components read `state.value.rev` inside computeds and re-ran on every state change:
   a click's selection re-asked the host, synchronously, for the links' tree (90 ms), levels and notes: `stateRev` and
   keyed computeds. The wheel's and an orbit's pivots read the last picture's element attachment (that element alone)
   instead of scanning every element. `budgetCutFast` keeps two size histograms per batch (moving and still keys had
   flipped one entry: 46,800 rebuilt at each motion start) and the moving ones are made while idle. The feedback rule
   estimates a frame by everything shown (the change that asks for feedback makes the pyramid stale), and a canvas resize
   or a selection's redraw asks for the feedback frame first. Masks are made for the batches a frame draws only.
72. **A pyramid serves a picture of the same camera at another canvas size** (a click opens the properties panel, Escape
   closes it): the same eye and direction give the same rays; what lies beyond the old picture is never hidden
   (`boxHidden`'s `beyond`). The whole frame after such a resize drew 22 M triangles instead of 105 M.
73. **The pyramid's read-back waits for the last frame's fence**: its `getBufferSubData` is a synchronous round trip to
   the GPU process, which waited behind the frame drawn since.

### Evidence (ticket VIS2)

`ccore/test/cull_test.mjs` (VIS2 section): a link placed 2,600 km from the scene's origin, a 12 m beam turned 30 degrees
and tilted, the GPU's f32 arithmetic emulated: vertices within 0.008 mm of their f64 projection with the render origin,
14,518 mm off with the old arithmetic (asserted too, so the case keeps catching it); a box placed into f64 holds its placed
corners, into f32 it does not; a plate 20 mm in front of a wall is not hidden by the wall's depth, a part 200 mm behind it
is; an item over the budget whose size over distance rounds up is drawn alone (0 triangles on the old code).

`ccore/test/lab_visible.mjs`, `LAB_GPU=vulkan` (ANGLE, Mesa, Apple M1 Pro), 6 synthetic buildings now with braces, tilted
instanced beams and risers, placed 2,600 km away (`drive.mjs --georef`): 60 cameras, 0 seen elements missing, 0 pixels
differ, this camera's pyramid used on 60 of 60, the moving frame's pyramid reused after fast turns on 12 of 12 (none
stale); triangles submitted far 54.4 → 27.8 M (the seen elements' 0.72 M), close 18.8 → 3.05 M (31,488), inside
9.90 → 0.40 M (24,884); an open from the links' rows only gives the same picture as an open keeping them whole. The
same test against the page before VIS2: 9,788 seen elements missing, 566,692 pixels differ. Software: 7 of 7, three
times while the fast gate ran alongside (load 11 to 17).

The owner's project (a local folder of 17 linked IFC files, 110.5 M triangles; his window 1492 × 790 at device pixel ratio
2, `drive.mjs` truth frames, proxies off, occlusion on): 0 seen elements missing and 0 pixels differing on 6 cameras
(close, far and inside, each at two azimuths); before the render origin, the close camera missed 4,365 and its picture
differed on 237,853 pixels. A close still frame drew 72 M triangles without the test and 7.4 M with it (GPU 35 → 10 ms),
a far one 110 M and 14 M (52 → 12 ms). "wait idle" after a gesture (default settings): before 83 to over 115 s after a
zoom; after 0.5 to 2.2 s for each of 8 gestures.

Inputs on the owner's project (his window, 11 gestures: orbit, zooms, clicks, a pan, Escape; `drive.mjs`; median / worst
ms, `draw_visible` on), the commit of decision 67 against decisions 71 to 73: press 257 / 801 → 195 / 619, Escape 296 → 137,
wheel 139 / 255 → 95 / 190, drag 10 / 213 → 9 / 173 (off: press 257 / 737 → 140 / 627, Escape 302 → 100). Run to run
the worst cases move by tens of ms (a repeat: press 255 / 740, wheel 189, drag 195). Over 200 ms still: the press after
Escape (619–754 ms: Escape's resize, the whole frame of the complete picture after its feedback frame and the pyramid's
read-back, about 1.1 s with no frame) and the press that starts an orbit (about 250 ms: the first moving frame's 160 ms
of script). `lab_inputs_links.mjs` (six georeferenced buildings, 2.7 M triangles, a whole frame 380–520 ms under
SwiftShader): press, Escape, wheel, orbit start, pan start, each answered within 18–96 ms; a click 60 ms after Escape
answered within 200 ms with the same element; a selection and Escape ask the host nothing that does not depend on the
selection, and the wheel's pivot comes from the readout (these two fail on the page before).

`lab_open_url` (local SwiftShader, the page's part, navigation start to the opening card): before 89–170 ms, CPU
throttled 4x 124–147, 6x 157–184, 10x 294.7 ms (fails); after 39–41, 59–73, 78–115, 105.9 ms. The viewer wasm: 638,521
bytes.

### The tile path (planned, not built: its own tickets)

What the measurements say the page needs next, for links: a project's frame on this path is bound by script, not by the
GPU: 46,800 batches walked every frame (a moving frame 35 to 80 ms of script for 4 to 8 ms of GPU on the owner's
project; over a session of 11 gestures the profile gives the frame's own loops 4.0 s, `prefixCount` 1.6 s,
`boxInFrustum` 1.1 s, the cut's `at` 0.8 s, `drawOutlines` 0.6 s, `budgetCutFast` 0.5 s, `cutEnd` 0.5 s), the first
moving frame of a gesture 160 ms; every link's whole mesh asked at open (339 MB on the wire), the tab up to 2.4 GB.
The baked pyramid (amendment 6) holds 8,190 leaves for that project (0.76 GB of leaves, 44 MB of proxies, a 2 MB table).
Planned as its own tickets, each behind the setting `tiles` (off until measured better on the owner's project):

1. **Tiles drawn** (`gl/tiles.ts` decodes already): each link's table read once (`tiles=toc`), a walk each frame by
   projected error (`cc_tiles_select`'s rule in TypeScript; measure against asking the host), tiles fetched in batches
   (`tile=a,b,c`, `&shell=1` then `&shell=2` in priority), decoded in the worker, uploaded per tile in their compact
   vertices (leaves 21-bit on the link's lattice, proxies 16-bit in the tile's box: new vertex inputs for both renderers),
   released least recently selected first within `link_gpu_mb`; a tile not there is covered by its nearest uploaded
   ancestor's proxy (never a hole, never a box). Element state texels and the id buffer per element as today.
2. **Occlusion per tile**: the depth pyramid tested per tile and per element run; the occluders (`tiles=occ`) drawn first
   after a camera jump, so the first frame of a new camera is culled too.
3. **Parity, then the switch**: hide, isolate, colour, pick, section and measure on tiles (their lab tests), the A/B of
   `drive.mjs --compare` (tiles off / on × occlusion off / on: first picture, complete, frames, inputs per kind,
   triangles against the measured need, tab memory, GPU bytes, bytes fetched); then whole-link buffers and HLOD cells
   dropped for links. The open model keeps today's path unless tiles come out simpler. WebGPU follows (its panel row says
   WebGL2 only until then).

## What is left

From ticket TILE1 (amendment 6): the page drawing by tiles (VIS1) and a native viewer (NAT2); then `HLOD` dropped (or
served from the pyramid's levels). The exterior shell: done as an order, not a filter (amendment 8, SHELL1); left: the renderers' first fetch exterior first (VIS1, NUI2 read `&shell=1` then `&shell=2`). Edge collapse with locked borders for the
coarse levels (better shape per triangle than clustering; decision 32). A vertex-cache and fetch order at bake
(meshoptimizer's, C++: not in the core now). Leaf answers without BMG1's edges (`&edges=0`: re-encoded by the host) and
compressed over HTTP for the web version (deflate saves 91 % of leaves' bytes on the synthetic links; decision 35).
Out-of-core indexing for a 5 GB IFC: the indexing job reads its cache in place, but `HLOD` still decodes the whole link
(the indexing peak; decision 39). Occluder sets measured on real models in a renderer.

From ticket VIS1 (amendment 7):

- **From ticket VIS2 (amendment 9)**: the press after Escape (619–754 ms on the owner's project): the complete picture
  after a feedback frame as budgeted slices, the pyramid's read-back never synchronous on the input path, an input
  pre-empting a whole frame's slices; the press that starts an orbit (the per-batch loops: the tile path). Web session log
  on Windows. A mirrored copy in the synthetic buildings (the model JSON's placement cannot mirror).
- **OPEN requirement: the selection change within 200 ms on a CPU 4× slower than the reference** (28 Nov 2026,
  `ccore/results/inputs.md`): on the real GPU, CPU throttled to 4× the reference machine, the press that selects takes
  212 ms and Escape 204 ms of the page's own part (6×: 93 and 81 ms; wheel and orbit start 23–42 ms). The gate runs
  `lab_inputs_links.mjs`'s unthrottled cases and its invariants; the 4× and 6× cases run with `LAB_SLOW=1
  LAB_GPU=vulkan` and the gate prints the skip with these numbers. Leads: the host's query batch per selection change,
  the masks for the resized pyramid on the feedback frame, the canvas resize itself (the panel over the canvas instead);
  the skip is removed when the cases pass 10 of 10 on the real GPU. The same path is the lead for the press after
  Escape on the owner's project.
- **`draw_visible` on a real project of links**: done by amendment 9 (VIS2: f32 at georeferenced coordinates, copies'
  shape offsets; on by default). **The fill counts what culling left out**: done (amendment 9, decision 67).

- **The page drawing by tiles (VIS1's follow-up; amendment 6's contract)**, planned in amendment 9 ("The tile path"): the walk of each link's tiles by projected error
  replaces the per-batch tests (46,800 batches on Pierre's project); its occluder triangles (real surfaces, up to 64
  a tile) drawn first into the depth pyramid give the first frame of a new camera its culling (today a camera's
  first still frame is drawn whole and the next ones culled); a parent tile is the moving picture (no outlines).
  This amendment's target, pyramid, masks, multi-draw and present pass are the frame those tiles go into.
- **Moving frames are not culled** (another camera than the pyramid's): reprojecting the last pyramid, or the tiles'
  occluders drawn first each frame, would; and device pixel ratio 1 while moving (5.8 against 7.7 ms a whole frame
  at 2) is not built.
- **Back faces**: −25 % of a frame's GPU time measured, not on: a shape's "closed and outward" flag comes with TILE1.
- **Copies are culled a run at a time** (a draw a run; a gap under 2,000 triangles drawn): an instance list per
  frame (the placement read by index) would make each group one draw again.



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
