# Experiment 2, T6: the dev loop end to end, native twin against wasm

Question: is the Vue lab's dev loop faster against a native Bimble process (the headless twin)
than against the wasm build? This measures both paths with the same edit, on the same branch
and the same machine.

## The edit

`loop.sh` adds one field to the `"state"` query in `crates/bimble-viewer/src/calls.rs`:

```rust
"spike": "bimblespike<N>".trim_start_matches("bimblespike").parse::<u64>().ok(),
```

N is random for each run. The JSON is `"spike":N` (a number). The digits come from a string
literal, so the wasm binary holds the bytes `bimblespike<N>` and `grep` can find them.
The script restores `calls.rs` on exit (trap). Do not commit a change to `crates/`.

## The two loops

- **native**: t0 = edit; `cargo build -p bimble-cli` with mold (t1); kill the old twin, start
  `BIMBLE_SERVE=7881 BIMBLE_TOKEN=t BIMBLE_HEADLESS=1 bimble view frame.bimble` (no display),
  poll `POST /query/state?t=t` every 20 ms until the body has `"spike":N` (t2).
  This is "the new behaviour is visible" to a client of the twin.
- **wasm**: t0 = edit; `npm run wasm:dev` in `web-ui/` (t1, files ready in
  `web-ui/public/bimble/`); then `grep -c bimblespike<N>` on `bimble_web_bg.wasm`.
  **The browser reload is not measured**: add the page reload and the wasm instantiation for
  the full wasm loop.

Peak RSS: `resource.getrusage(RUSAGE_CHILDREN).ru_maxrss` of a fresh Python around the build
(the largest single process, not the sum).

Fairness: one untimed warm-up of each path, then the runs alternate native and wasm.
The frame is `bimble demo` (2 storeys, 3×2 bays), written under `target/exp2-loop/`.

## Run

```
MOLD_DIR=<mold>/libexec/mold CMAKE_BIN=<cmake>/bin spikes/native-twin/exp2/loop.sh [runs]
```

Results are appended to `results.txt`.

## Result (24 Nov 2026, aarch64, 8 cores, rustc 1.100.0; 5 runs each, medians)

| loop | build | restart → value | total | peak RSS |
|---|---|---|---|---|
| native (mold) | 1.56 s | 0.06 s | 1.72 s | 549 MB |
| wasm (`wasm:dev`) | 3.75 s | not measured (browser reload) | ≥ 3.75 s | 1267 MB |

All runs are in `results.txt`.
