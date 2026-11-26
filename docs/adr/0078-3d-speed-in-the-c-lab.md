# ADR 0078 — 3D speed in the C lab: copies instanced, batches culled, a moving budget

- Status: Accepted, 26 Nov 2026
- Decided by: Pierre delegated the decision ("don't wait for me to validate the ADR, just do your
  best, I'll review once it's implemented"); the choices below by Claude on ticket 3D1, to be
  reviewed by Pierre on the implementation.
- Reversal cost: low (a new BMSH version beside version 1, asked by the page; the viewport's draw
  path; no file format, no golden)

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

`ccore/results/frames.md`, wasm build, SwiftShader, before → after (loads 3.6–11.8: parallel gates):

| model | orbit frame median | still frame | close orbit | GPU bytes | pictures (3 cameras) |
|---|---|---|---|---|---|
| 20,026 elements | 187.5 → 39.9 ms | 182 → 156 ms | 358 → 352 ms | 17.2 → 17.3 MB | identical |
| 772 elements, mesh-heavy | 146.1 → 112.5 ms | 178 → 120 ms | 146.5 → 90.2 ms | 13.0 → 0.1 MB | identical |

The mesh-heavy model's 700 proxies are one shape: BMSH 12.6 MB → 96 KB. The 20,026-element model's
boxes are copies too, but of 12 triangles: drawn instanced, SwiftShader took 1.2 s a frame against
0.19 s merged (its cost is per instance), so shapes under 64 triangles are placed into the merged
buffers (`INSTANCE_MIN_TRIS`). A 1×1 readPixels after a frame takes most of the frame time once the
budget leaves things out (49 of 50 ms): SwiftShader's work is per pixel there, not per triangle.

Regressions measured and left open (next ticket): the drag input → frame and the pick click → frame
times rose (large: 317 → 394 ms, 32 → 189 ms). The pick: a selection opens a panel that resizes the
canvas, which counted as a camera move (a moving frame, then the still one 150 ms later); fixed
after the run (the canvas size left out of the camera key), not re-measured. The drag: the test
waits for each move's frame before the next, so the still frame (150 ms after) falls between moves.
At 48 px (Rust's cap) a moving frame of the 20,026-element model can draw nothing at the fitted
distance: Rust's rule, kept; proxies (below) are what kept far parts visible in Rust.

## What is left

- **HLOD proxies** (`hlod.rs`, `proxies.rs`, ADR 0036 #9): Rust's own bench at 3 px of error, whole
  model orbit, M4 Pro: full frames 5.26 → 0.79 ms (200 MB model), 29.1 → 1.31 ms (415 MB), 12.7 →
  0.88 ms (758 MB); build and upload 0.19–0.89 s and 19–92 MB of GPU memory per model. On SwiftShader
  the moving budget already drops what is small; proxies would keep far parts visible while moving
  instead of dropping them.
- **GPU occlusion** (ADR 0036 #1): last frame's occlusion queries; Rust ran it on native backends
  only (wgpu's WebGL2 backend read results too early) and it changed nothing on a fast GPU.
- **A box tree** for picks and the measure tool, if a model with very large single meshes is slow.
