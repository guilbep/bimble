# ADR 0083 — A WebGPU renderer beside WebGL2, and a performance panel

- Status: Accepted, 26 Nov 2026
- Decided by: Pierre chose the direction ("ok go with A": WebGPU in the page, WebGL2 kept as the
  tested fallback, out of three options: WebGPU in the page, a native viewport in C, back to Rust)
  and delegated the design; the choices below by Claude on ticket GPU1, to be reviewed by Pierre on
  his own GPU with the performance panel.
- Reversal cost: low (new files under `web-ui/src/gpu/`, one seam in `glviewport.ts`, a setting
  that is written only once set; no model format, no golden). Amended 27 Nov 2026 (ticket GPU2): the toggle, WebGPU
  draws links, `open --app`, the optimisations registry.

## Context / Problem

Pierre felt the C lab's 3D slower than the Rust app's and asked why it is not WebGPU. Rust v0.19.0
drew through wgpu (native APIs, WebGPU on the web with a WebGL2 fallback, ADR 0020) and could cull
copies on the GPU with a compute shader (ADR 0036 #10, the setting `gpu_cull`), which WebGL2 cannot
(no compute shaders); the lab stored `gpu_cull` and said "does not apply". Mission 1 asks for a whole
folder of IFC files as links, over 100 million triangles. Nobody could compare the two renderers on
the owner's machine: the numbers of `ccore/results/frames.md` are SwiftShader's (a CPU rasteriser).

## Decision

1. **A performance panel first** (`web-ui/src/panels/Performance.vue`, `web-ui/src/perf.ts`): opt-in
   from the status bar's "Performance" or `?perf=1`. It shows the renderer in use and why WebGPU is
   not (when asked), the adapter (`GPUAdapterInfo`, `WEBGL_debug_renderer_info`; "software" said
   plainly for SwiftShader, llvmpipe and the like), frame time draw-to-pixels (last, median, p95 of
   the last 120 frames, moving and still apart), triangles, draw calls, culled batches, proxies, GPU
   bytes, JS heap, wasm memory, open → first frame and → complete picture of the last file opened,
   per link its elements and triangles, the slowest 10 input → frame times of the session. "Copy
   report" (plain text, no file names unless ticked; nothing is sent) and "Record 10 s". One source:
   `perf.ts` reads the counters `frames.mjs` and `ux.mjs` read (`window.__bbGl`). While it is open the
   viewport waits for the GPU after each frame (a 1×1 `readPixels`, `onSubmittedWorkDone`), so the
   times include the GPU; closed, nothing waits. **Alternative:** times from `requestAnimationFrame`
   intervals (a 2 ms frame shows as 16 ms), or always waiting (a stall per frame for everyone).
2. **The renderer in new files, the viewport kept** (`web-ui/src/gpu/`): `device.ts`, `scene.ts`,
   `pipelines.ts` + `shaders.wgsl`, `cull.ts` + `cull.wgsl`, `render.ts`, `switch.ts`. It only turns
   the scene, the camera and the states into pixels; camera, tools, snapping, picking, overlays,
   measure, section box handles, view cube, grids, reference planes and notes stay `glviewport.ts`'s,
   shared. One seam in `glviewport.ts` `draw()` (`gpuDraw`: the view state handed over, the overlay
   drawn, the counters), and the selection's overpaint off under WebGPU. WebGPU draws on a second
   canvas stacked over `#bimble` under the overlay, without pointer events (a canvas cannot change
   its context type). It reuses `gl/cull.ts` (batches, frustum, budget) and `gl/proxies.ts` (cut,
   fit, stand-ins) as they are; the frame is ADR 0078's (moving with the triangle budget and outlines
   or proxies, the fill in slices 150 ms after the stop, never an empty picture), the slices drawn
   into a kept offscreen target (a canvas texture keeps nothing between frames). **Alternative:**
   restructure `glviewport.ts` behind a renderer interface (a parallel ticket edits its draw path;
   the seam is a few lines), or a WebGPU library (three.js WebGPURenderer: a second scene graph).
3. **The real-adapter check** (ADR 0020's no-adapter Chrome, ADR 0040's black picture): an adapter
   must come; a fallback (software) adapter is refused unless `?gpuSoftware=1` (tests and benches);
   a 4×4 render is read back and must give the right pixel. Any refusal keeps WebGL2 and the reason
   for the panel; a device lost or a frame that throws goes back to WebGL2 live.
4. **GPU culling of copies (the setting `gpu_cull`, Rust's #10) applies with WebGPU**: a compute pass
   tests each copy of a shape group (its rank in its batch's range this frame, its element's box
   against the six planes, and while moving its own screen size against the budget's threshold),
   compacts the kept ones per group and counts them into `drawIndexedIndirect` arguments: **one
   indirect draw per group** (faces) and one `drawIndirect` (edges), since WebGPU has no
   multi-draw-indirect; the instance's region comes from a dynamic uniform offset (no
   `indirect-first-instance` needed). Off, copies are direct instanced draws with `firstInstance`.
   The merged lists stay CPU-culled per batch (as WebGL2). `query settings` `applies.gpu_cull` is
   true when the renderer setting is WebGPU; the Settings dialog shows the checkbox then.
   **Alternative:** per-element culling of the merged lists in compute (an index compaction a frame).
5. **The switch**: a setting `renderer` (automatic, WebGL2, WebGPU), stored with the others but not
   one of Rust's fields: written to the file only once set, so every existing file keeps its bytes;
   `?gpu=webgpu|webgl2` overrides it. **Automatic is WebGL2 for now**; it becomes WebGPU where an
   adapter passes the check once Pierre has compared both on his machine with the panel (then this
   ADR is amended). The WebGPU code is a dynamic import: a WebGL2 user does not download it.
6. **The look is the same**: flat faces from the screen derivatives, ambient 0.62, the palette per
   element through a colour table (a selection rewrites 4 bytes an element, not the vertices), the
   section box's discard, cut shade and back-edge test, links in the reference colours, WebGL2's
   `polygonOffset` as the pipelines' depth bias. No anti-aliasing in either.

## Amendment (27 Nov 2026, ticket GPU2): the toggle where Pierre measures, and WebGPU draws links

Pierre: "would be nice to have in the ux/ui a toggle for webgpu so we can measure properly!" His case is a
local folder of 17 IFC files (252,189 elements, 110.5 M triangles) linked into one project; with links loaded
the page fell back to WebGL2 (ADR 0078 decision 22), so a toggle would have changed nothing there. His
everyday Chromium (153, Asahi Linux) gives WebGPU no adapter at all ("navigator.gpu gave no adapter"). The
choices below by Claude on ticket GPU2 under the same delegation; the orchestrator added the app window and
the rule for "automatic" (points 10 and 12).

7. **The toggle**: a two-state switch "WebGL2 | WebGPU" in the performance panel's header, the adapter and what
   draws now under it, a progress line while the other renderer gets ready, and when WebGPU was asked and does
   not draw, why in plain words (no WebGPU, no adapter, a software adapter, the device lost, a feature it does not
   draw yet, named) and what to do on this platform (Linux Chromium: the three flags of point 10, or `bimble-ccore
   open --app`). A chip in the status bar ("WebGL2" / "WebGPU", `*` while WebGPU is asked and not drawing)
   toggles on a click; the palette has "3D renderer: WebGPU" and "3D renderer: WebGL2". All three switch live (no
   reload) and write the setting `renderer`. The checked device is kept between switches (the probe and the
   fill-rate test once). **Alternative:** the Settings dialog only (GPU1: three clicks and a dialog in front of
   the view he measures).
8. **WebGPU draws the viewport's own models** (`gpu/switch.ts` `GpuModels`, handed over by the seam): the
   batches, instanced groups, per-element state texels, placements and link proxies glviewport.ts made for
   WebGL2, so the two renderers draw the same picture by construction and a hide, isolate, colour or move costs
   the same small writes on both: a model's placement is a 64-byte row of a storage buffer (a move rewrites it;
   the compute cull turns its planes and eye into the model's frame), the state is the one RGBA8 texel per
   element (`gl/cull.ts` `packState`) read in the vertex stage by faces and edges (an edge vertex carries its
   element: 4 bytes a vertex, WebGL2's `edgeEl` taken over), a model all shown in its colour reads no state. A
   link whose detail is away (released by the budget, or not taken over yet) is drawn as its coarsest proxies,
   the budget's need/want clock is fed by WebGPU's frames, and the place tool's ghost and markers are drawn by
   WebGPU too (left by GPU1). GPU1's own lists (`Scene.setLists`) are gone. **Alternative:** WebGPU's own lists
   and buffers per link (GPU1's way extended: a second copy of every decision, two of every buffer).
9. **One copy of each model on the GPU**: WebGPU takes a model's buffers over from WebGL2's (`getBufferSubData`
   once, then WebGL2's buffer emptied: `bufferData(0)`; the links' and the open model's proxies' WebGL2 buffers
   emptied too, filled again from the page's copies), the open model at once (the WebGL2 picture stays until
   then), the links within 24 ms a frame (drawn as their proxies until theirs is in, with a progress line). Back
   to WebGL2, each model is read back from WebGPU (`mapAsync` on a copy) into WebGL2's buffers while WebGPU still
   draws; then WebGL2 draws. A device lost: the open model uploaded again from the page, the links released (the
   budget asks them back). The budget counts the same bytes (a model's bytes are the same on either renderer).
   Picks on a link whose arrays were dropped (SIZE2) read its triangles back from WebGPU's buffers: asynchronous,
   so the first ray over a new element starts the read and misses it, the next finds it (WebGL2's read is
   synchronous). **Alternative:** upload to WebGPU directly from the link's fetch (no WebGL2 upload, no read
   back): it needs a seam inside `loadSlot`/`uploadLists`, which ticket SIZE3 is rewriting; left for after it.
   Asking the core for picks (one path for both): asynchronous too, and every synchronous caller would change.
10. **`bimble-ccore open --app`** (and the setting "Open Bimble in an app window", `open_app`, a lab field written
   only once set): a Chromium-family browser in an app window with its own profile (`<config dir>/browser`) and,
   on Linux, `--use-angle=vulkan --enable-features=Vulkan --enable-unsafe-webgpu`: the three flags that give
   WebGPU the GPU on Chromium 153 / Asahi (checked with a non-headless browser process on Ozone's headless display,
   which without flags gives "ANGLE (Mesa, AGX G13/G14, OpenGL ES 3.2)" and no adapter, as Pierre's browser did;
   `--enable-unsafe-webgpu` alone gives a SwiftShader fallback adapter). None found: the usual opener.
   **Alternative:** tell him the flags only (a command line to remember, every time).
11. **Compare renderers** (the panel, beside Record 10 s): the same camera path (an orbit of the current view, 240
   frames of 0.0125 rad, then 2.5 s still and 10 whole still frames) on WebGL2 then WebGPU, switched live, each
   frame waited for; the copied report holds both blocks side by side.
12. **"Automatic"** follows the adapter check, never flags the user did not set; on the numbers below it stays
   WebGL2 for now (Evidence (GPU2)).
13. **Optimisations in the panel** (the owner's rule of 27 Nov: every optimisation is a user setting with a good default
   and an off value, shown in the panel): a section of the performance panel lists the rows of a registry
   (`web-ui/src/optimisations.ts`: key, FR/EN label, toggle / choice / number, default, off value; other tickets add
   rows with `registerOptimisation` in their own modules), seeded with `renderer`, `moving_fps`, `moving_proxies`
   (simplified far parts), `memory_share` (the links' GPU budget) and `gpu_cull`; the copied report lists each value.

What still falls back to WebGL2 (the panel names it): nothing that the lab draws today; WebGL2-only remain the
occlusion queries (off by default, a bench hook), the selection's overpaint over the kept picture, the
large-mesh box preview before the lists.

GPU culling of the merged lists per element (point 4's alternative) is not built: it needs an index compaction
per frame (a compute pass writing each kept element's index range into an indirect draw list per batch, WebGPU
has no multi-draw-indirect: a draw per kept batch, or the elements' indices copied into one compacted index
buffer per frame, 4 bytes an index moved). The CPU cut per batch costs a binary search a batch; on the real GPU
the frames below show where the time goes before building it.

## Evidence

`ccore/test/lab_webgpu.mjs`, this machine (aarch64, Chromium 153 headless). Chromium as the lab tests
start it exposes `navigator.gpu` with **no adapter** (ADR 0020's case): the fallback is tested there
(WebGL2 draws, the panel and the report say "navigator.gpu gave no adapter"). With
`--enable-unsafe-webgpu --enable-features=Vulkan --use-vulkan=swiftshader` Dawn gives a **software
adapter** (google / swiftshader, a fallback adapter) whose canvas presents; with
`--use-webgpu-adapter=swiftshader` alone the adapter renders offscreen but its canvas fails ("A valid
external Instance reference no longer exists"). ADR 0040's black picture did not happen.

Pixels, WebGPU against WebGL2, 3 cameras × (plain, a selection, an element hidden, a section box) and
with GPU culling on, 984 × 547 px: on the 72-element frame 74-761 pixels differ (0.01-0.14 %), on the
mesh-heavy model (772 elements, 700 copies of one shape: one instanced group, GPU-culled) 2-730 pixels
differ, of which at most 225 by more than 24 levels (≤ 0.07 %); the largest difference is 221 levels
(an edge pixel drawn by one API and not the other). Tolerance stated in the test: ≤ 1 % of the pixels
over 24 levels. A WebGPU moving frame on the mesh-heavy model with the minimum budget (20,000
triangles): proxies, outlines, 42,847 drawn pixels against the still picture's 38,646.

Frames (`ccore/tools/frames.mjs --builds native --models heavy --frames 40 --renderer webgl2|webgpu`,
one run each, 1-minute load 3.5 and 4.8, both on SwiftShader: WebGL2 through ANGLE, WebGPU through
Dawn's Vulkan; a CPU rasteriser both times, so this compares the two code paths, not a GPU):

| mesh-heavy, native host | WebGL2 | WebGPU |
|---|---|---|
| orbit frame median / p95 | 6.0 / 20.8 ms | 10.7 / 58.5 ms |
| still frame (695,264 triangles, 3 draws) | 155.7 ms | 150.4 ms |
| close orbit / close still | 43.0 / 76.4 ms | 51.0 / 99.2 ms |
| drag input → frame median / p95 | 22.7 / 86.6 ms | 4.4 / 20.3 ms |
| pick click → frame median | 94.8 ms | 66.2 ms |
| GPU bytes (the renderer's own) | 9.66 MB | 9.80 MB |

No conclusion for a real GPU follows from SwiftShader; the panel's report from Pierre's machine is
the measurement that decides decision 5.

Bundle (`VITE_CCORE=1 vite build`, bytes, gzip -9 in brackets), before → after: the main chunk
554,765 (156,156) → 573,815 (162,571): the panel, `perf.ts`, the setting and their FR/EN copy;
`glviewport` 87,842 (31,467) → 90,896 (32,703): the seam and `gpu/switch.ts`; new, loaded only when
WebGPU is asked: `render` 30,404 (11,349) and `device` 2,873 (1,548).

### Evidence (ticket GPU2)

`ccore/test/lab_webgpu_links.mjs`, six synthetic links (`bench/links_synth.mjs`: 160 unique closed meshes of about
600 triangles each), five in a row and one 1.5 km behind, link_0 red, link_1 purple; WebGPU's picture against
WebGL2's at three cameras, the renderer switched live between them, per state the pixels over 24 levels (of
1280 x 800; the tolerance as above, at most 1 %):

| state (3 cameras) | SwiftShader (`LAB_GPU=software`, `?gpuSoftware=1`) | the M1 Pro (`LAB_GPU=vulkan`) |
|---|---|---|
| plain (the far link from its proxies) | 0.27 / 0.37 / 0.35 % | 0.004 / 0.012 / 0.013 % |
| a link hidden | 0.23 / 0.32 / 0.29 % | 0.003 / 0.011 / 0.011 % |
| an element of a link hidden | 0.29 / 0.40 / 0.34 % | 0.004 / 0.013 / 0.013 % |
| a class isolated | 0.032 / 0.035 / 0.023 % | 0 / 0.002 / 0.000 % (1 px) |
| a link moved, another recoloured | 0.26 / 0.39 / 0.35 % | 0.003 / 0.011 / 0.014 % |
| the budget at 0 (the far link released: its coarsest proxies) | 0.27 / 0.37 / 0.35 % | 0.004 / 0.012 / 0.013 % |
| a section box around a linked element | 0.024 / 0.035 / 0.007 % | 0 / 0 / 0 % |

The differing pixels are edge pixels (the largest difference 195-207 levels: a 1-px line drawn by one API and
not the other, as GPU1 found). Under WebGPU every loaded model is WebGPU's and WebGL2 keeps 8 kB (its state
texture): 12.45 MB on WebGPU against 12.72 MB on WebGL2 for the same scene. Ten live switches each way: each
renderer's bytes the same every time (12.72 and 12.45 MB, on both rasterisers); a switch (the toggle to the end of
the other renderer's first frame, the six links taken over or given back included) 135-221 ms on the M1 Pro,
165-439 ms on SwiftShader. No draw list rebuilt and
no whole `/mesh` asked by a switch. A pick on a link under WebGPU finds the element WebGL2 picked once its
triangles are read back (the first ray misses it). `lab_webgpu.mjs` (GPU1's 52 checks, GPU culling included) and
`lab_perf.mjs` (the switch, the chip, the palette, Compare renderers, each click timed by `input_time.mjs`:
SwiftShader page 25-139 ms, presented 50-308 ms; the first switch to WebGPU 584-659 ms with the device check,
the next ones 90-162 ms) pass.

**On the machine's GPU** (27 Nov 2026, `LAB_GPU=vulkan`: ANGLE on Vulkan for WebGL2 and Dawn's Vulkan for WebGPU,
both on Mesa's driver for the Apple M1 Pro; `ccore/tools/frames.mjs --links N --renderer webgl2|webgpu`, native host
built by clang, 1280 x 800, the frame finished by a 1x1 `readPixels` (WebGL2) or `onSubmittedWorkDone` (WebGPU); the
two waits alone cost 0.7 ms and 0.1 ms here, so they do not explain the gap). The last of three batteries, after the
two fixes they found (the bench timed a WebGL2 read inside every WebGPU frame; the placement rows read from a storage
buffer by every vertex), 1-minute load 5.7-7.2 (other tickets' builds and browsers on the machine):

| synthetic links | WebGL2 6 | WebGPU 6 | WebGL2 17 | WebGPU 17 |
|---|---|---|---|---|
| open → first picture (the links' proxies) | 384 ms | 257 ms | 244 ms | 353 ms |
| open → complete picture | 515 ms | 461 ms | 599 ms | 759 ms |
| still frame (558,144 / 1,581,408 triangles; 12 / 34 draws) | 2.8 ms | 3.5 ms | 4.3 ms | 5.3 ms |
| moving frame median (p95) | 1.7 (3.6) ms | 3.8 (6.0) ms | 2.3 (4.2) ms | 4.2 (7.9) ms |
| close still frame | 1.6 ms | 4.1 ms | 3.1 ms | 5.5 ms |
| GPU MB (the scene; half hidden, budget ½) | 12.1 (6.2) | 12.1 (6.2) | 34.4 (16.7) | 34.4 (16.7) |

The earlier batteries (load 4.5-11) gave the same order: WebGPU's still and moving frames 1-4 ms longer on these
scenes, its first picture and complete picture within ±40 % (the take-over of WebGL2's buffers is in WebGPU's
"complete"). `ccore/results/frames.md` holds the rows. On the local folder of 17 IFC files (110.5 M triangles) WebGL2
was measured (its still frame of 67.9 M triangles and 5,553 draws about a tenth of a second, as SIZE2 found); WebGPU
was not: a second page of that project, and the first switch before the transfer was streamed, took the machine
under its 2.5 GB floor (the watchdog stopped them), and the streamed version's run was stopped when the orchestrator
asked every run on that folder to use a scratch cache directory (none of these runs wrote to the folder: no file in
it changed during them).

**Decision for "automatic" (point 12): it stays WebGL2** (`AUTO_WEBGPU = false` in `gpu/switch.ts`). On the only GPU
measured, WebGPU draws the same pictures but no frame faster, and opens a project of links later (it takes WebGL2's
buffers over after their upload); no measurement shows a scene where it wins. The mechanism the orchestrator asked
for is in place: with `AUTO_WEBGPU` true, automatic takes WebGPU only on a hardware adapter that passes the check
(never a fallback or software one, never flags the user did not set: an everyday Linux Chromium has no adapter, so
WebGL2). Revisit with Pierre's own Compare renderers report from his folder, and once WebGPU uploads a link's mesh
directly (no WebGL2 upload and read back).

## What is left

- (GPU2: the ghost and markers are WebGPU's too, and a model's buffers live on one renderer at a time.)
- GPU2: WebGPU receives a link's mesh through WebGL2's upload and a read back (a seam inside `loadSlot` and
  `uploadLists`, after ticket SIZE3, would upload it directly); a pick on a dropped link's element under WebGPU misses
  the first ray (an asynchronous read back); WebGPU's frames on small scenes are 1-3 ms longer than WebGL2's on the M1
  Pro (not profiled to the end: the page's own work is under 1 ms of it); the local folder not measured on WebGPU.
- Occlusion queries (off by default on WebGL2), the large-mesh box preview before the lists and the
  selection overpaint are WebGL2's only.
- Per-element GPU culling of the merged lists, a depth pyramid (Rust #10's full form), and the frame
  times on a real GPU: Pierre's measurement with the panel decides automatic → WebGPU.

## Revisit when

- Pierre's report from his machine (Copy report, both renderers) is in: amend decision 5.
- WebGPU gets multi-draw-indirect (one call for every group).
