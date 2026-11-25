# The C core on large synthetic models (ticket T16)

Question: the C core is proven on a 72-element frame (edit → pixels 233 ms, `bench.md`). How does
every stage behave at 2,000, 20,000 and 60,000 elements? Synthetic models only.

## Result in short (25 Nov 2026, after the two fixes below)

- **The host scales about linearly and stays small.** 20,026 elements (15.2 MB file): load and
  validate 77 ms (clang -O2) / 158 ms (tcc), RSS 50 MiB; scene + BMSH 257 / 747 ms, 14.4 MB;
  queries and verbs 1–2 ms (`query elements` with 10,000 rows 6–11 ms). Peak RSS 92 MiB.
- **Edit → pixels in the lab (dev.sh loop, tcc host): 0.39 s at 2k, 2.1 s at 20k, 6.0 s at 60k**
  (median of 5). At 20k and above the time is the tcc host restoring its session (0.7 s / 2.0 s)
  and building the scene again (tcc code is ~3× slower than clang -O2), then SwiftShader drawing.
- **Orbit in the page (WebGL2 in SwiftShader, 1280×800): 27 / 135 / 437 ms per frame (p50)** at
  2k / 20k / 60k. Smooth at 2k, about 7 fps at 20k, 1.5–2 fps at 60k. This is CPU rasterization
  in headless Chromium; a GPU was not measured.
- **The session file costs a full rewrite per change**: 100 ms (clang) / 260 ms (tcc) at 20k,
  314 / 799 ms at 60k, on every verb, undo and select with `--session` (off by default; dev.sh
  uses it). Not fixed here (see "What is left").
- Two fixes made, each a separate commit with its numbers: the scene kept ~10 KB of working memory
  per element (peak RSS 1.4 GiB at 60k → 270 MiB), and /mesh built a second scene that query status
  had already built (first status + /mesh at 20k, tcc: 1321 → 747 ms).

## The models

`python3 bench/large_model.py <target> out/t16-large-<target>.bimble` (deterministic: same target,
same bytes; GlobalIds = BLAKE2b of a counter in the IFC alphabet, first char 0–3, checked unique).
golden/frame.json's structure scaled up: a concrete grid frame, NX × NY bays of 6.0 × 5.0 m, K
storeys of 3.0 m: pad footings under every grid point, rect columns on every grid point, rect beams
both ways and one slab per bay per storey, perimeter walls, two walls rotated by atan(0.8/0.6) per
storey, and an `extruded_polygon` (L shape, two rectangular voids) in every 8th bay. The C host
loads and validates all three (and `test/large_test.mjs` loads one of 300).

| target | elements | grid × storeys | file | beams / columns / slabs / walls / footings / polygons |
|---|---|---|---|---|
| 2,000 | 1,982 | 5 × 4 × 16 | 1.5 MB | 784 / 480 / 320 / 320 / 30 / 48 |
| 20,000 | 20,026 | 18 × 17 × 14 | 15.2 MB | 9,058 / 4,788 / 4,284 / 1,008 / 342 / 546 |
| 60,000 | 59,832 | 24 × 23 × 24 | 45.5 MB | 27,624 / 14,400 / 13,248 / 2,304 / 600 / 1,656 |

Every element is simple (a box: 8 vertices, 12 triangles; the polygons: some tens). A real model
of the same count has heavier geometry (IFC meshes, openings), so its scene and draw times will be
higher than these.

## Method

`node bench/large.mjs` (method in its header; raw: `out/t16-large-final.json`, log
`out/t16-large-final.log`, not committed). Wall clock in Node unless noted; host times are
`spawn → "listening"` and HTTP round trips on 127.0.0.1. Per size, for `out/native/bimble-ccore`
(clang -O2) and `out/tcc/bimble-ccore` (tcc -g):

1. Host: 5 starts (median); RSS and peak (VmRSS / VmHWM from `/proc`) after the load and after the
   first /mesh; `query elements` (default 1,000 rows, and `limit` 10,000, the hard cap as in Rust),
   `query element`, `verb place` (a column) + `POST /undo/-`: median of 5.
2. Scene and /mesh: the first `query status` (it builds the scene), the first `GET /mesh` (the
   host's "built in N ms" line and the Node fetch), a cached fetch (median of 5), and the rebuild
   after each place.
3. Session: the same place / undo pairs with `--session` on a fresh copy; the write per change is
   the median call time with `--session` minus without. Then SIGTERM and a restart with the restore
   (the log says "session restored").
4. Lab (clang host, one Vite on 5205, one headless Chromium 153 over raw CDP, SwiftShader,
   1280×800, `?native=1&gl=1`): an untimed first navigation (Vite compiles the modules), then the
   timed one; first frame = `__bbGl.firstFrameMs` (page clock from navigation start). Orbit: 60
   left-drag moves of 8 px, each waited to a new glviewport frame plus a 1×1 `readPixels` (forces
   SwiftShader to finish the frame), timed in the page from the `pointermove` and in Node from the
   CDP dispatch; then moves streamed for 3 s and the intervals between drawn frames. Click-pick:
   5 clicks on the canvas centre; window capture/bubble `pointerup` listeners around glviewport's
   handler (CPU ray pick over all triangles + `select()` sync XHR), and the time to the recoloured
   frame. Edit → pixels: `host/mesh.c` edited (a constant category byte), `build.sh host`, stop the
   host, start `out/tcc/bimble-ccore --session` on the same port, token and model, until an in-page
   rAF watcher sees a new canvas hash (`lab_loop.mjs`'s method); 5 runs after one warm-up.

Machine: Apple M1 Pro (8 cores), Asahi Linux aarch64, 15.2 GiB; clang 23.1.3, tcc 0.9.28rc,
node 26.12.0, Chromium 153.0.8010.36. **Load average 7–8 during the runs** (other agents building
in other worktrees). Host numbers repeat within ~10 % across the three runs made (one 60k clang start median was
30 % higher in one run); the lab numbers
are noisier (the 20k loop: 1.9–3.3 s; first frame at 2k: 0.55 s in one run, 1.18 s in another).

## Results (after both fixes)


### 1982 elements

| ms unless noted | native | tcc |
|---|---|---|
| file (MB) | 1.5 | 1.5 |
| spawn → listening (load + validate), median | 14.3 | 27.5 |
| host load_ms (query status) | 11.2 | 17.1 |
| RSS after load / peak (MiB) | 7 / 7.6 | 7 / 8 |
| first query status (builds the scene) | 57.6 | 65.1 |
| query elements, 1000 rows (default), median | 1.8 | 2.3 |
| query elements, limit 10000, median | 3 | 3.6 |
| query element, median | 1.1 | 1.6 |
| verb place, median | 0.4 | 1.5 |
| undo, median | 0.6 | 0.9 |
| /mesh build (host log), first | 13.2 | 25.1 |
| /mesh first fetch (Node) | 17.2 | 33.1 |
| /mesh cached fetch, median | 3.9 | 7.7 |
| /mesh rebuild after a place (host log), median | 21.1 | 70.9 |
| BMSH bytes (MB) | 1.4 | 1.4 |
| RSS after /mesh / peak (MiB) | 11.4 / 11.4 | 11.6 / 11.6 |
| --session: place / undo, median | 13.3 / 12.9 | 26.7 / 25.8 |
| --session: file write per change (estimate) | 12.9 | 25.2 |
| session file (MB) | 1.6 | 1.6 |
| restart with session restore (spawn → listening) | 40 | 70.3 |

### 20026 elements

| ms unless noted | native | tcc |
|---|---|---|
| file (MB) | 15.2 | 15.2 |
| spawn → listening (load + validate), median | 77.4 | 157.8 |
| host load_ms (query status) | 72.7 | 154.2 |
| RSS after load / peak (MiB) | 50.3 / 65.4 | 50.4 / 65.9 |
| first query status (builds the scene) | 165.8 | 547.5 |
| query elements, 1000 rows (default), median | 0.9 | 1.4 |
| query elements, limit 10000, median | 6.4 | 10.6 |
| query element, median | 0.9 | 1.1 |
| verb place, median | 1.7 | 1.6 |
| undo, median | 1.8 | 2.6 |
| /mesh build (host log), first | 67.4 | 177.9 |
| /mesh first fetch (Node) | 90.9 | 199.7 |
| /mesh cached fetch, median | 25.5 | 24.3 |
| /mesh rebuild after a place (host log), median | 220 | 715.4 |
| BMSH bytes (MB) | 14.4 | 14.4 |
| RSS after /mesh / peak (MiB) | 91.7 / 91.7 | 91.9 / 91.9 |
| --session: place / undo, median | 101.4 / 100.6 | 262 / 260.3 |
| --session: file write per change (estimate) | 99.7 | 260.4 |
| session file (MB) | 16.7 | 16.7 |
| restart with session restore (spawn → listening) | 285.4 | 657.8 |

### 59832 elements

| ms unless noted | native | tcc |
|---|---|---|
| file (MB) | 45.5 | 45.5 |
| spawn → listening (load + validate), median | 235.1 | 466 |
| host load_ms (query status) | 228.9 | 459.7 |
| RSS after load / peak (MiB) | 146.5 / 193.4 | 146.5 / 193.9 |
| first query status (builds the scene) | 495.2 | 1637 |
| query elements, 1000 rows (default), median | 1.1 | 2 |
| query elements, limit 10000, median | 6.7 | 15 |
| query element, median | 1.2 | 1.3 |
| verb place, median | 3.8 | 4 |
| undo, median | 1.8 | 2.8 |
| /mesh build (host log), first | 199.8 | 529.4 |
| /mesh first fetch (Node) | 284.2 | 637.3 |
| /mesh cached fetch, median | 69.4 | 60.6 |
| /mesh rebuild after a place (host log), median | 683.8 | 2153.7 |
| BMSH bytes (MB) | 43.1 | 43.1 |
| RSS after /mesh / peak (MiB) | 270 / 270 | 270.1 / 270.1 |
| --session: place / undo, median | 317.5 / 315.4 | 802.8 / 818.4 |
| --session: file write per change (estimate) | 313.7 | 798.8 |
| session file (MB) | 50.1 | 50.1 |
| restart with session restore (spawn → listening) | 882.4 | 2263.6 |

### lab

| | 1982 | 20026 | 59832 |
|---|---|---|---|
| navigation → first frame (page clock) | 1182.3 | 466.7 | 1236 |
| viewport mesh fetch (ms) / bytes | 13.2 / 1.4 MB | 20.7 / 14.4 MB | 166.8 / 43.1 MB |
| orbit: move → finished frame, in page p50 / p95 | 27 / 33.5 | 135.1 / 182.4 | 437.3 / 1235.3 |
| orbit: move → finished frame, Node p50 / p95 | 33 / 47.3 | 143.5 / 197.1 | 448.3 / 1240.3 |
| orbit: drawn-frame interval p50 / p95 (frames in 3 s) | 16.7 / 33.4 (132) | 133.2 / 250 (25) | 675 / 1216.7 (5) |
| click-pick (pointerup handler: pick + select), median; first two | 4.3; 15.6, 16.7 | 26; 78.9, 85.8 | 61.7; 215.4, 209.3 |
| click → recoloured frame | 143.4 | 415.7 | 1320.4 |
| edit → pixels: build.sh host, median | 59.4 | 77.5 | 83.6 |
| edit → pixels: stop, median | 0.9 | 3.6 | 10.6 |
| edit → pixels: spawn → listening (session restore), median | 93.8 | 680.5 | 2011 |
| edit → pixels: build end → new pixels, median / p95 | 331 / 337 | 2029 / 3212 | 5898 / 7399 |
| **edit → pixels total, median / p95** | **391** / 396 | **2090** / 3281 | **5955** / 7479 |

Notes on the host tables: "native" is clang -O2, "tcc" is tcc -g. The first /mesh build is the
BMSH serialization only (the first query status built the scene, fix 2); the rebuild after a place
includes the scene (no status in between). The session write is an estimate (call time with
`--session` minus without).

Notes on the lab table: the viewport's mesh fetch is small because the host had already built the
mesh for the untimed first navigation (cached per rev). First frame is mostly the page's own boot
(Vue shell, sync queries), not the mesh: 0.47–1.24 s at all sizes. The first two picks are slower
(JIT warm-up); the pick scans every triangle in JS (240k at 20k, 720k at 60k).

## What dominates

- **Host load** (parse + validate + canonical): ~3.9 µs per element clang, ~7.8 µs tcc; linear.
  RSS 50 MiB at 20k, 147 MiB at 60k (the model plus the file text while parsing: peak 65 / 194).
- **Scene**: built once per rev, for query status and (since fix 2) /mesh. 20k: 166 ms clang,
  548 ms tcc; 60k: 495 / 1637 ms. BMSH serialization on top: 67 / 178 ms (20k).
- **Edit → pixels at 20k (tcc, median 2.09 s)**: build 78 ms, stop 4 ms, restart with session
  restore 681 ms (the restore parses the 16.7 MB session JSON and then the model inside it: 4×
  a plain start), then ~1.35 s until new pixels: the tcc scene + BMSH (~750 ms), fetching and
  uploading 14.4 MB, the page's queries, and one SwiftShader frame (~135 ms) plus the watcher's
  full-canvas `readPixels`. At 60k: restore 2.0 s, then ~3.9 s (scene + BMSH 2.3 s in tcc).
  At 2k it is 0.39 s, against 0.23 s on the 72-element frame.
- **Orbit**: SwiftShader rasterizes all triangles and edges every frame (no culling, no LOD):
  ~6.7 µs per element per frame at 20k.
- **Verbs with `--session`**: the session file write (a full model save + JSON escape + write +
  rename) dwarfs the verb (2 ms → 100 ms clang / 260 ms tcc at 20k).

## Fixes made (each its own commit; all suites green)

1. `src/scene.c`, commit `ff29ce5`: every element's working memory (local mesh, weld and edge
   hash tables, edge uses, world triangles) stayed in the scene's arena until the scene was
   freed, ~10 KB per element; the host keeps one scene per rev and /mesh built another, so both
   were alive at once. Now a scratch arena reset per element holds the working memory, and the
   world triangles live in their own arena until the meshes are built. BMSH bytes identical
   (compared byte for byte on the 20k and 60k models and golden/frame2.json).

   | out/t16-large-{before,fix1}.json | before | after |
   |---|---|---|
   | peak RSS after /mesh, 20k (both binaries) | 472 MiB | 103 MiB |
   | peak RSS after /mesh, 60k | 1,402 MiB | 282 MiB |
   | /mesh build 20k, clang / tcc | 277 / 775 ms | 242 / 728 ms |
   | /mesh build 60k, clang / tcc | 839 / 2,331 ms | 700 / 2,199 ms |

2. `host/mesh.c`, `host/calls.c`, `host/mesh.h`, commit `527f17c`: GET /mesh built its own
   scene of the same model and rev that `query status` had just built (`scene_now`), and the lab
   asks for both after every change. Now /mesh serializes calls.c's scene (`cc_host_scene`); with
   none (no model, or a failed build) it builds its own as before, which gives the error text.

   | out/t16-large-{fix1-lab,final}.json | before | after |
   |---|---|---|
   | first status + first /mesh, 20k, clang / tcc | 455 / 1,321 ms | 257 / 747 ms |
   | first status + first /mesh, 60k, clang / tcc | 1,295 / 3,998 ms | 779 / 2,274 ms |
   | edit → pixels, 20k, median (p95) | 2,548 (2,773) ms | 2,090 (3,281) ms |
   | edit → pixels, 60k, median (p95) | 7,384 (7,518) ms | 5,955 (7,479) ms |

   The loop rows are under load average 7–8; the host rows are the cleaner measure.

Checked after each fix: `test/run.sh`, `CC=clang test/run.sh`, `test/host.sh`,
`node test/mesh_http_test.mjs`, `lab_gl.mjs`, `session_test.mjs`, `wasm_run.mjs` (after
`build.sh wasm`), and `lab_wasm.mjs` after fix 2.

## Next to the Rust references

| what | Rust | C core here | same method? |
|---|---|---|---|
| reopen the model after a restart | ≈ 1.0 s, 13,000 elements (ADR 0066, the native twin) | 20,026 elements: plain start 77 ms clang / 158 ms tcc; with session restore 285 / 658 ms | **No.** Rust: "the model is opened again" in the twin (a real model, its own timing; whether it includes a scene is not stated). Here: spawn → listening on a synthetic model of boxes; the scene is built later, on the first status or /mesh (+166 / 548 ms). |
| viewport while dragging | pointer → changed pixels p95 86 ms full resolution, 55–56 ms half (20,715 elements; ADR 0066, `spike/native-viewport`) | move → finished frame p95 182 ms in page, 197 ms from CDP (20,026) | **No.** Rust renders natively and streams frames to the page; here WebGL2 in the page on SwiftShader. Both headless Chromium, software GL, 50 vs 60 drags. The end points differ: changed pixels seen by the page vs a finished frame (1×1 readPixels). Different models. |
| edit → pixels | 1.66 s to the value, not pixels (Rust twin, 72 elements) | 0.39 / 2.09 / 5.96 s (2k / 20k / 60k) | No Rust number on a large model. |

## What is left (not done here)

- The session file: a full rewrite per change (100–800 ms at 20k–60k). An append-only log of
  steps, or writing after a short idle, would take it off the verb's path.
- The restore: parses the session JSON, then the model text inside it (4× a plain start).
- The tcc host in the dev loop runs the scene ~3× slower than clang -O2; a loop that keeps the
  geometry code in a prebuilt -O2 object would cut most of the 20k/60k loop (not measured).
- The page: pick is a JS scan over every triangle (26 ms at 20k, 62 ms at 60k warm; 80–215 ms
  cold); drawing has no culling. Neither was changed.
- A GPU: all page numbers are SwiftShader.
