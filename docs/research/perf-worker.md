# The page while a model loads: Worker, yield or hybrid (ticket PERF2, part A)

26 Nov 2026. Question: how to keep the C lab responsive while a model opens, and where the editor
wasm's weight goes. Inputs: PERF1 (`ccore/results/perf.md`), the code (sources as file:line, at
commit 9368218), and timings measured here. Decision at the end.

## 1. How the page talks to the core today

- **One instance, synchronous calls.** `ccoreClient` fetches the wasm and the model, instantiates,
  then opens with `host_open_text` (web-ui/src/ccore.ts:300-321, before PERF2). Every call is
  `call(op, name, body)` (ccore.ts:332-340): copy in (`put`, ccore.ts:106), call, copy the answer
  out (`run`, ccore.ts:114-129), read the change feed, all on the main thread, all synchronous.
- **The client interface is synchronous.** `CcoreClient` (ccore.ts:79-97): every method returns its
  value at once, except `bytes` (the PDF) and `default`. The native client keeps the same contract
  with a **synchronous XHR** (web-ui/src/bimble.ts:274 and :289-290), so the page was written for it.
- **Call sites.** bimble.ts has 66 exported functions that call the core synchronously
  (`wasmOrThrow()` 23 times, `tryQuery` 57 times); 54 files import bimble.ts; about 211 call
  expressions of those helpers outside bimble.ts (grep, approximate). The viewport calls the core
  on **pointer move**: the place tool (`toolCall`, web-ui/src/glviewport.ts:1341, from the
  `pointermove` handler at :1556), the measure tool (`measureCall`), the section box; picking is on
  the CPU in JS (glviewport.ts:1377-1430) and needs no call. PERF1's pointer p95 is 1-2 ms.
- **The mesh.** The viewport's `load()` runs in its frame loop (glviewport.ts:753-777, started at
  :1826): `source.mesh()` = `ccoreMesh()` (ccore.ts:138) = `host_mesh` (ccore/src/wasm_api.c:661),
  then `parseMesh` and `upload` in the same task.

## 2. Where the long task comes from (measured)

`node` on the dev machine, the wasm builds of 9368218, the synthetic large model
(`bench/large_model.py 20000`: 20,026 elements, 266,520 triangles), one run each (script in the
report, not committed):

| step (large model) | editor wasm | viewer wasm |
|---|---:|---:|
| compile + instantiate | 3 ms | 2 ms |
| `host_open_text` (parse, validate) | 133-169 ms | 211 ms |
| `query status` (builds the scene: calls.c `scene_now`, once per rev) | 447 ms | - |
| `host_mesh` after it (BMSH: `cc_scene_world_mesh` per element, session/mesh.c:46-99) | 567 ms | 697 ms |
| `host_mesh` again (cached per rev, session/mesh.c:150) | 4 ms | 7 ms |

The sample model: open 8-13 ms, mesh 11 ms. In the page, the first change runs `refresh()` in a
microtask of the same task as the parse (bimble.ts `on_change`, `queueMicrotask`), and its queries
build the scene: parse + scene was **one task** (PERF1 base run here: the editor's large load has
2 long tasks, 625 + 168 ms... see the table in section 5). The BMSH, its parse in JS and the upload
were a second one, in the viewport's frame.

The scene (not the BMSH) is what the page's queries need: `scene_now` is called from 22 places
(ccore/session/calls.c, views.c:374, schedules.c:132, mesh.c:156): status, visibility, views,
sheets, schedules, the tools. Whatever is done off the main thread, the main-thread core must hold
the scene before the page can answer those queries.

## 3. The options

**(W) The whole core in a Worker, async client.** Every synchronous call becomes a promise: the 66
functions of bimble.ts, their ~211 call expressions in 54 files, the place and measure tools on
pointer move (a round trip per move: postMessage both ways, ~0.1-1 ms plus queueing behind any
long call). The 60+ lab tests drive the real page and read answers synchronously through
`window.__bb*` hooks. A synchronous facade over `SharedArrayBuffer` + `Atomics.wait` needs
cross-origin isolation (`Cross-Origin-Opener-Policy` and `Cross-Origin-Embedder-Policy` headers):
GitHub Pages cannot set response headers, so not on the deployed lab (a service-worker shim that
re-serves the page with the headers exists, but it reloads the page on first visit). Cost: weeks,
high risk. Buys: no long task at all on the main thread.

**(Y) Main thread, the open in steps with a yield between.** Parse, then the scene, then the BMSH,
each in its own task (`scheduler.yield()`, or a `MessageChannel` message, never `setTimeout`, which
background tabs throttle to 1 s). The caches already exist (scene per rev, BMSH per rev), so a step
is "call it once now"; the page is told of the new model only after the steps, so its first
queries and the viewport's first `/mesh` are answered from the caches. Touches `ccore.ts` (the
open functions) and two lines of bimble.ts; no C change; no call site changes. Buys: the longest
task drops from parse + scene (+ more) to the longest single step (on the large model: the BMSH,
~570-700 ms in node). Does not lower the total. Finer slicing needs a resumable C API:
`bmsh_of` is already a loop per element (mesh.c:55-99) and `cc_scene_build` builds per element
after one pass for the origin (ccore/src/scene.c:1142-1167): both can be cut into slices of N
elements with a cursor in the session, one wasm export (`host_mesh_step(budget)`).

**(H) A Worker for the stateless part.** A second instance in a Worker opens the same bytes and
builds the BMSH; the main core seeds its mesh cache with them. It removes the BMSH step (~570 ms)
from the main thread, not the scene (~450 ms), which the queries need (section 2), and it doubles
the parse and the memory (the large model: 133 MB of wasm memory per instance, PERF1). The
`.bimbleview` path (a read-only scene) does not help an editable model. Cost: a worker bundle, a
C export to seed the cache, the second instance's 1 MB fetch. Buys: one step of three.

## 4. Decision

**(Y) now; the resumable BMSH and scene next; (W) not.** (Y) removes the merged task with no
interface change and no risk to the tests; (W) costs every call site for a gain (Y) plus slicing
mostly gets. Built in part B (each measured, section 5):

1. The open in steps (start, Open, Import; Link: the BMSH in its own task after the link) with the
   step shown: a determinate bar of the four steps (read, parse, scene, mesh) in the shell's
   `opening` state, with Cancel (the file is dropped when Cancel comes before the parse; after it
   the core already holds the new model), and the step over the viewport while the page starts.
2. `<main>` landmark and the 24 px close target of the view tabs (Lighthouse a11y).

Not built, and why:

- **The lazy editor wasm** (boot on `ccore-view.wasm`, swap on the first edit). The view build
  answers `query capabilities` with the editing pages off (ccore/session/capabilities.c:8-12), so
  the page hides what would trigger the swap; the swap must be synchronous for the lab tests (they
  edit right after `data-bb-ready`), so it needs the editor compiled in advance, a synchronous
  instantiation, a second parse + scene + BMSH (~1.2 s on the large model, on the main thread) and
  the session's view state replayed (tabs, visibility, section box, selection). Not safe in the
  time. The LCP element on mobile is the "3D view" pill (a 49 x 20 px span shown after the model
  opened: Lighthouse `largest-contentful-paint-element`, before and after PERF2), so the editor's
  extra ~215 KB (gzip) of wasm is on its critical path: mobile LCP moves only with the lazy editor
  (or a viewer-first boot). The start step line (item 1) does not change it: on the unthrottled
  load Lighthouse observes, the wasm is there before the first paint, so the line is never the
  largest content (and LCP is simulated from that trace).
- **Code-splitting pages and dialogs.** The page table imports every page eagerly to read its
  exports (`layout`, `hint`, `side`, `capability`: web-ui/src/pages/index.ts:23) and dialogs
  register routes when imported (e.g. dialogs/Export.vue:10). A split needs those exports moved
  out of each `.vue` file (a metadata file per page), files the parallel tickets edit.

## 5. Measured (ccore/tools/perf.mjs --full --no-remote, same machine)

Two pairs of runs on the same two site builds (base 9368218; PERF2), Chromium 153 headless,
SwiftShader. Pair 1 with Lighthouse (3 runs each), load average at start 3.72 (base) and 3.98
(PERF2); pair 2 without Lighthouse, back to back, load 8.99 and 9.61 (other builders were running).
Long tasks: n / total / longest, ms, median of 3 loads.

| | base, pair 1 | PERF2, pair 1 | base, pair 2 | PERF2, pair 2 |
|---|---|---|---|---|
| editor, large model | 2 / 904 / 635 | 6 / 858 / 429 | 2 / 846 / 620 | 5 / 796 / 431 |
| viewer, large model | 3 / 1,005 / 764 | 4 / 940 / 545 | 3 / 1,012 / 762 | 4 / 935 / 545 |
| editor, sample | 1 / 119 / 119 | 2 / 350 / 289 | 1 / 215 / 215 | 1 / 294 / 231 |
| viewer, sample | 1 / 137 / 137 | 1 / 63 / 63 | 2 / 177 / 96 | 1 / 145 / 80 |

| Lighthouse (pair 1, median of 3) | base | PERF2 |
|---|---|---|
| LCP desktop / mobile, editor | 390 / 4,360 ms | 385 / 4,352 ms |
| LCP desktop / mobile, viewer | 395 / 3,302 ms | 382 / 3,154 ms |
| transfer, editor (desktop) | 647 KB | 648 KB |
| unused JS, editor (desktop / mobile) | 53 / 62 KB | 53 / 61 KB |
| a11y desktop / mobile, editor | 95 / 100 | 100 / 100 |

Reading: on the large model the longest task drops by ~190 ms (editor) and ~220 ms (viewer)
in both pairs; the total does not drop (the same work, in more tasks). The new longest task is
the scene or the BMSH (each ~430-550 ms in the browser): the next cut is the resumable BMSH and
scene (section 3, Y). On the sample model the tasks are 60-300 ms and move with the machine's
load from run to run (the base's own two runs: 119 and 215 ms): no claim either way. LCP and the
transfer do not move (section 4: the lazy editor is what moves them); a11y 95 -> 100.
