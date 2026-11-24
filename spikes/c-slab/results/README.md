# C slab spike: how the timings are taken

Numbers go in `results/bench.md`, written by `bench/bench.sh`. Run it on a quiet machine (no other
build, no browser in use). Raw data goes in `results/out/` (hyperfine JSON, `pixels.json`). Do not
commit numbers from a smoke run.

```
RUNS=10 bench/bench.sh
```

## Edit → rebuild

- Tool: hyperfine 2.0, `-N` (no shell), `RUNS` runs, `--prepare bench/touch.sh`.
- The edit: `touch.sh` removes the previous `int spike_touch_N(void){return N;}` line from
  `slab.c` and appends a new one with a new `N`. Each run compiles a real source change.
- Builds: `./build.sh tcc`, `./build.sh native` (clang -O0), `./build.sh wasm-dev`
  (clang wasm32 -O0), `./build.sh wasm` (clang wasm32 -O2). Each is built once before timing.
- Reported: median and min wall time, peak RSS (max over runs, `memory_peak_resident`).
- `slab.c` and `main_wasm.c` are restored by a trap, also on failure.

## Edit → pixels

`bench/pixels_loop.mjs` (Node, no packages). One static server, one headless Chromium over raw CDP
(`test/cdp.mjs`, the same flags as `test/pixels.mjs`: SwiftShader WebGL). The page loads
`build/slab_dev.wasm` once. Then, per run:

1. t0.
2. Flip `#define SPIKE_K` in `main_wasm.c` between 1.0 and 2.0 (slab thickness multiplier).
3. `./build.sh wasm-dev`. t1.
4. `slab.reload(url)` with a cache-busting query. Each `requestAnimationFrame`, compute a frame
   signature (hash of the centre pixel column, plus `countNear` of the face grey) and stop when it
   differs from the previous frame's. t2 (taken in Node, after the CDP round trip).

Reported: build (t1−t0), reload→pixels (t2−t1), total (t2−t0), median and p95 (nearest rank).
`SPIKE_K` is restored to 1.0 at the end. The default 1.0 leaves `slab_mesh` output identical
(`test/wasm_dump.mjs` still matches `expected/scene.txt`).

## Limits

- SwiftShader is a CPU rasteriser: reload→pixels is not the time on a real GPU.
- t2 includes one CDP round trip and the readback of the frame signature (a full `readPixels`).
- The compare target is the Rust → wasm loop, 3.3–4.4 s from edit to files ready (ADR 0065),
  which stops before the browser. Compare it with the build column, not the total.
