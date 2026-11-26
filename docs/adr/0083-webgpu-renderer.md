# ADR 0083 — A WebGPU renderer beside WebGL2, and a performance panel

- Status: Accepted, 26 Nov 2026
- Decided by: Pierre chose the direction ("ok go with A": WebGPU in the page, WebGL2 kept as the
  tested fallback, out of three options: WebGPU in the page, a native viewport in C, back to Rust)
  and delegated the design; the choices below by Claude on ticket GPU1, to be reviewed by Pierre on
  his own GPU with the performance panel.
- Reversal cost: low (new files under `web-ui/src/gpu/`, one seam in `glviewport.ts`, a setting
  that is written only once set; no model format, no golden).

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

## What is left

- The place tool's ghost and markers are drawn by WebGL2 (the frame switches canvas while they show).
- The WebGL2 path's buffers stay allocated while WebGPU draws (the seam does not touch
  `glviewport.ts`'s uploads): GPU memory is the sum while WebGPU is on.
- Occlusion queries (off by default on WebGL2), the large-mesh box preview before the lists and the
  selection overpaint are WebGL2's only.
- Per-element GPU culling of the merged lists, a depth pyramid (Rust #10's full form), and the frame
  times on a real GPU: Pierre's measurement with the panel decides automatic → WebGPU.

## Revisit when

- Pierre's report from his machine (Copy report, both renderers) is in: amend decision 5.
- WebGPU gets multi-draw-indirect (one call for every group).
