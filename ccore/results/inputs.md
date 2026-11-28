# Inputs within 200 ms on a project of links (lab_inputs_links)

Written by hand from the test's output (ticket VIS2, ADR 0074, ADR 0078 amendment 9), 28 Nov 2026, at commit 073a0de's
page. The command:

    CCORE_PORT_BASE=auto node ccore/test/lab_inputs_links.mjs                          # software rasteriser (the gate)
    LAB_SLOW=1 LAB_GPU=vulkan LAB_LOCK=1 CCORE_PORT_BASE=auto node ccore/test/lab_inputs_links.mjs   # by hand, the real GPU

The scene: six synthetic buildings (`bench/buildings_synth.mjs`, 2.7 M triangles) placed 2,600 km away (`drive.mjs
--georef`), linked into a project on the native host, 1280 × 800 at device pixel ratio 1, `draw_visible` on. Each input
starts after the complete picture, the page idle and the GPU settled (`gl.finish()`). A time is the page's part, the
input to the viewport frame drawn for it (`lib/input_time.mjs`); it must be 200 ms or less. On the real GPU the presented
frame is the same within 0.1 ms.

The machine: Linux 7.2 aarch64, 8 CPUs, Apple M1 Pro (Asahi), 16 GB; this is the reference machine (a fixed loop in the
page: 25.0 ms). "4×" and "6×" are a machine 4 and 6 times slower than the reference: this host's own slowness is
measured first (the same loop, median of 5), then the CPU is throttled by what is left (CDP
`Emulation.setCPUThrottlingRate`).

## One run each, the machine otherwise quiet

| input | software, unthrottled | software, 4× (throttled 3.24×) | software, 6× (4.85×) | real GPU, unthrottled | real GPU, 4× (3.73×) | real GPU, 6× (5.6×) |
|---|---|---|---|---|---|---|
| press that selects | 51.9 ms | 88.2 ms | 192.8 ms | 58.0 ms | **211.8 ms (fails)** | 93.0 ms |
| Escape (the selection cleared) | 17.5 ms | 99.2 ms | 71.2 ms | 27.2 ms | **204.2 ms (fails)** | 80.6 ms |
| wheel (a zoom step) | 36.2 ms | 32.4 ms | 34.8 ms | 32.2 ms | 23.1 ms | 42.2 ms |
| orbit start (a drag) | 37.4 ms | 35.8 ms | 38.9 ms | 40.9 ms | 35.6 ms | 41.7 ms |
| pan start (a press) | 36.2 ms | – | – | 25.2 ms | – | – |
| a click 60 ms after Escape | 26.3 ms | – | – | 81.5 ms | – | – |

Software: ANGLE on SwiftShader (the CPU draws). Real GPU: ANGLE on Mesa Vulkan (Apple M1 Pro). The 4× and 6× columns are
one run each; the press and Escape vary by run (6× below 4× on the real GPU is that spread, not a faster path).

The open requirement (ADR 0078 amendment 9, "What is left"): the selection change within 200 ms on a CPU 4× slower than
the reference. Until it passes 10 of 10 on the real GPU, the gate skips the 4× and 6× cases and says so (`LAB_SLOW=1` runs
them).

## Under load (software, the gate's form)

Beside the fast gate (`ci/run.sh fast` in a loop, `CCORE_TEST_JOBS=2`), 10 runs, before the slow-machine cases left the
gate, while a second gate also ran on the machine (1-minute load 6.6–13.4): 7 of 10 green. The failing cases: 6× press
230.5 ms (load 9.8); unthrottled pan start 794 ms and 4× Escape 205 ms (load 13.4); unthrottled orbit start 445 ms
(load 8.3). With every core busy (8 busy loops) the page's part took 2–5 s: its frame waits on the starved SwiftShader
GPU process.

The gate's form (the slow-machine cases out, `LAB_SLOW` unset), 10 runs beside ONE `ci/run.sh fast` in a loop
(`HOST_LAB=0 CCORE_TEST_JOBS=2`, three gates passed meanwhile; 1-minute load 7.0–12.2):

- the first series: 9 of 10; run 9 failed on the wheel, 924 ms of page part at load 8.8;
- the test then waits for a settled machine before each input, within a bound (no frame, `gl.finish()` back within 30 ms,
  the main thread answering every message within 50 ms over 300 ms; tried again after a still picture up to 20 times),
  the assertion unchanged: 9 of 10; run 3 failed on the host re-ask check (a re-ask while the components' key was
  unchanged; its detail was not kept);
- the re-ask check then also skips, saying so, when the viewport's notes key changed (the mesh merged again when a
  link's detail arrived: `link_notes` is asked again for that, rightly): **10 of 10**. Twice the settle needed one more
  still picture; once the links' counter moved during the selection and the re-ask check printed its skip.

## One run with the slow-machine cases, the real GPU, after the settle (`LAB_SLOW=1 LAB_GPU=vulkan`, load 8.5)

17 of 17: unthrottled 21–44 ms; 4× (throttled 3.34×) press 149.0, Escape 63.1, wheel 29.5, orbit start 38.4 ms; 6×
(5.02×) press 78.7, Escape 98.1, wheel 30.2, orbit start 40.4 ms; a click 60 ms after Escape 70.7 ms. One green run;
the requirement stays open until 10 of 10.
