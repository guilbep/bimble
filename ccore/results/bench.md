# The C core's edit → pixels loop in the Vue lab (ticket T12)

Question: with the C core behind the unchanged Vue lab, how long from "edit a constant in C" to
"the lab page shows the change"? Same question as the Rust baselines (ADR 0065, ADR 0066).

## Result (25 Nov 2026, 10 runs per loop, after one untimed warm-up each)

Times in ms. "page" starts when the build ends (stop the host, start it, the page notices).
"total" is edit → visible = build + page.

| loop | build | stop | spawn → listening | page: query | page: text | page: pixels | **total: query** | **total: text** | **total: pixels** |
|---|---|---|---|---|---|---|---|---|---|
| C, `build.sh tcc`, median | 218 | 5 | 11 | 31 | 1105 | 1179 | **251** | **1328** | **1404** |
| C, `build.sh tcc`, p95 | 252 | 8 | 15 | 32 | 1123 | 1200 | 287 | 1358 | 1448 |
| C, `build.sh native` (clang -O2), median | 4339 | 3 | 16 | 33 | 1122 | 1186 | **4375** | **5462** | **5527** |
| C, `build.sh native`, p95 | 4466 | 6 | 25 | 51 | 1135 | 1216 | 4501 | 5587 | 5648 |
| probe: tcc, page retry 20 ms, median | 217 | 5 | 14 | 32 | 91 | 155 | 253 | 308 | 373 |
| probe: tcc, page retry 20 ms, p95 | 233 | 9 | 17 | 53 | 99 | 172 | 274 | 328 | 399 |

Next to the baselines (all on this machine, edit → the new value visible in the lab page):

| loop | edit → visible | end point |
|---|---|---|
| Rust release wasm (before ADR 0065) | ≈ 40 s | page |
| Rust dev wasm (ADR 0065) | 4.66 s | in-page `query('state')` after a reload |
| Rust native headless twin + mold (ADR 0066) | 1.66 s | in-page `query('state')`, no reload |
| C slab spike (one slab, not the lab) | 0.118 s | pixels, wasm |
| **C core, tcc, this ticket** | **0.25 s** | in-page `query('status')` (same end point as 1.66 s) |
| **C core, tcc, this ticket** | **1.33 s / 1.40 s** | DOM text / WebGL pixels (the lab as it is) |
| C core, clang -O2, this ticket | 4.38 s / 5.46 s / 5.53 s | query / text / pixels |

All raw runs: `out/t12-lab_loop.json` (not committed); the log `out/t12-lab_loop.log`.

## What dominates

- **tcc, text and pixels: ~1.0 s of the 1.3–1.4 s is the page waiting.** When the host stops, the
  lab's change long-poll fails and `pollChanges` (`web-ui/src/bimble.ts`) sleeps 1000 ms before it
  asks again. The host itself is back 15 ms after the build (stop 5 + spawn 11) and answers the
  page's `query('status')` 31 ms after the build. The probe row makes that wait 20 ms (only in the
  page, by wrapping `setTimeout`; not the lab as it is): text 91 ms, pixels 155 ms after the
  build, **0.31 s / 0.37 s** edit → visible. A retry at once after the first failure in
  `pollChanges` would give this; that file is not in this ticket.
- After the change reaches the page: text needs the Vue refresh (queries over sync XHR); pixels
  need one more `/change` (the epoch check), `GET /mesh` and a draw in SwiftShader: ~65 ms more.
- **tcc build: 218 ms**, of which `build.sh` reports (medians): library 69 ms (all `src/*.c`),
  archive 18 ms, 8 test binaries 65 ms, host 30 ms; the rest is bash and process start.
  `build.sh` always rebuilds everything; a host-only build would skip the test binaries. Not
  measured as a loop: no claim.
- **clang -O2 build: 4.34 s**, of which library 2.67 s (20 files, one after the other), host
  0.60 s, test binaries 1.0 s. Serial -O2 of everything; with clang the build is 80 % of the loop.

## Method

`RUNS=10 bench/lab_loop.sh` (runs `bench/lab_loop.mjs`; method in its header). One Vite (5197),
one headless Chromium 153 (raw CDP, `--use-angle=swiftshader`, 1280×800), the lab opened once at
`/?native=1&gl=1&port=47342&token=…`; no page reload in any run. The host runs with `--session`
on a temporary copy of `golden/frame.json`. Before the runs the page places a column and selects it
(through `bimble.ts`); after every run the script checks that the page and the host still have it
(73 elements, the selection, one undo step) and that `/mesh` holds the new build's byte. The loops
alternate tcc, clang, probe; builds never overlap.

Each run makes two edits at once, then reverts them after the run (the source is restored on exit,
and the binaries rebuilt from it):

- pixels: `host/mesh.c` writes a constant MaterialCategory byte (1…6, a new one each run) instead
  of `e->category`: every element changes colour in the WebGL canvas.
- text: `host/calls.c` `225.0` (bytes per stored triangle, `query status` `model_bytes`) becomes a
  constant chosen so the status bar's `{mb} MB` shows a new number (101, 102, …).

Timing: t0 = edits written; build = `build.sh <mode>` wall time; stop = SIGTERM → the old host
exited; spawn = spawn → "listening" (includes the session restore); then three in-page recorders,
stamped with `Date.now()` (same clock as Node):

- query: `query('status')` from the page's `bimble.ts` (sync XHR, every 20 ms, from the restart)
  returns the new `model_bytes`. This is the baselines' end point.
- text: a MutationObserver sees `[data-bb=status-counts]` contain the new `M MB` (the DOM changed;
  the paint follows within one frame).
- pixels: a `requestAnimationFrame` watcher hashes the canvas (`readPixels`) after each glviewport
  draw; the first new hash after the restart began (frame drawn; shown within one frame).

## Differences from the baselines' method (`spikes/native-twin/exp2/loop_browser.mjs`)

- Same: one Vite, one headless Chromium with SwiftShader, raw CDP, one untimed warm-up, alternated
  loops, wall clock in Node, the page not reloaded for the native loops, in-page `query` over the
  sync XHR transport polled every 20 ms with failed XHR retried.
- Different:
  - The edit: the baselines added a `"spike": N` field to `query('state')`, which the page does
    not show. Here one edit changes the canvas pixels and one the status bar text; the `query`
    column is the baselines' end point, measured on the same runs.
  - 10 runs per loop (the baselines had 5).
  - The C host keeps its session across the restart (`--session`); the Rust twin reopened the file
    (its state was lost on restart). The restore is inside "spawn → listening" (11–16 ms).
  - `build.sh` rebuilds the whole C core and its tests each time; cargo builds incrementally.
  - The page uses the C host's WebGL viewport (`&gl=1`); the Rust native lab draws nothing in 3D.
- The baselines did not measure text or pixels. The Rust twin's page uses the same `pollChanges`,
  so its 1.66 s measured to the DOM would likely also include the 1 s retry (not measured; the
  journal of 24 Nov: "the feed recovers in 0.8–0.9 s").

## Machine and noise

Apple M1 Pro (8 cores), Asahi Linux aarch64, 15.2 GiB; tcc 0.9.28rc, clang 23.1.3, node 26.12.0,
Chromium 153.0.8010.36. Load average 3.0 at the start, 2.0 at the end: other agents were building
and testing in other worktrees during the run (a `test_*` binary at 100 % of one core was seen).
The pixels column includes SwiftShader's CPU rendering (no GPU in headless Chromium here).

## The session file (what makes the comparison fair)

`bimble-ccore --session [port] [model]` (or `--session=<file>`, or `CCORE_SESSION=<file>`) keeps
`<model>.ccore-session`: the model text, the undo and redo steps (id, label, inverse commands),
the step counters (unsaved state), rev and the selection. It is written after each call that
changed one of them (tmp + rename) and at exit, and restored on start when it names the same model
path and that file's size and mtime are unchanged. It is untrusted input: any invalid content is
ignored and logged, and the host starts from the model file. Off by default (tests unchanged).
Checked by `test/session_test.mjs` (SIGTERM and SIGKILL restarts, undo after restart, four
corruptions, a model file changed since). Because rev is restored, the lab's WebGL viewport now
also refetches `/mesh` on a new host epoch; with the old `glviewport.ts` this benchmark times out
(no new pixels), checked once by hand.
