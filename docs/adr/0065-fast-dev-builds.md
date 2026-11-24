# ADR 0065 — Fast dev builds: line tables only, dependencies optimized, a dev wasm for the Vue lab

- Status: Accepted
- Decided by: Pierre ("do 1. Tonight: turn on the profile settings in Cargo.toml … and give the
  lab a dev build mode", 24 Nov 2026). Measurements by Claude, same evening.
- Reversal cost: low (two Cargo.toml sections, one script mode)
- Extends: ADR 0020 (web version), ADR 0048 (Vue lab), ADR 0050 (two web builds)

## Context / Problem

The edit → wasm loop was slow, and it used a lot of RAM and disk. Measured on 24 Nov (8-core aarch64,
16 GB; `spikes/compile-speed/results/REPORT.md` on branch `spike/compile-speed`):

- The Vue lab copied its wasm from `scripts/build-web.sh`, which defaults to release (LTO, one
  codegen unit, wasm-opt), and built the viewer as well as the editor. One edit in
  `bimble-viewer` cost cargo 24.9 s, wasm-bindgen 0.9 s and wasm-opt 13.7 s: **about 40 s**. The
  clean build took 94 s.
- The dev profile had Cargo's defaults: full debug info. The dev wasm was **415 MB**.
  wasm-bindgen took 4.2 s on every edit, on top of cargo's 2.2–3.4 s. Run next to other builds, it
  filled 16 GB of RAM and swap, and systemd-oomd killed a terminal.
- A spike ported the STEP parser to C, Zig, V and TypeScript. Each rebuilds in about 0.1 s, like
  Rust. The language is not what makes the loop slow; the build is.

## Decision

1. `[profile.dev] debug = "line-tables-only"`. Panics and backtraces keep file and line. Local
   variables are not visible in a debugger: set `CARGO_PROFILE_DEV_DEBUG=2` for a debugging
   session.
2. `[profile.dev.package."*"] opt-level = "s", debug = false`. Dependencies (wgpu, egui, naga…)
   are optimized for size, once. Bimble's own crates stay at opt-level 0, so an edit rebuilds fast.
3. `PROFILE=dev LAB=1 scripts/build-web.sh` builds the editor only, in the dev profile, without
   wasm-opt. `npm run wasm:dev` in `web-ui/` runs it and copies the files. This is the lab's
   loop. The release build (`scripts/build-web.sh`) is unchanged and still builds both, for Pages,
   releases and the performance budgets.

## Measurements (24 Nov, editor wasm, three edits each)

| | Before: lab release build | Before: dev, full debug | **After** |
|---|---|---|---|
| cargo, edit in `bimble-viewer` | 24.9 s | 2.2 s | 1.3 s |
| cargo, edit in `bimble-core` | — | 3.4 s | 2.4 s |
| wasm-bindgen | 0.9 s | 4.2 s | 2.0 s |
| wasm-opt | 13.7 s | — | — |
| **Edit → files ready** | **≈ 40 s** | **6.4–7.7 s** | **3.3–4.4 s** |
| wasm | 16 MB | 415 MB | 61 MB |
| Clean build | 94 s | 39 s (viewer) | 89 s (viewer with `debug = 0`: 46 s) |
| Largest process | 1.1 GB (rustc) | — | 1.25 GB (wasm-bindgen) |

The "full debug" column is the viewer build (no `editor` feature); the other two are the editor.
`debug = 0` was also measured: 0.1 s faster, a 43 MB wasm, no file and line in panics. Its 0.1 s
does not pay for losing the line numbers.
End to end (edit → the new value in the lab page, wasm build + page reload, 5 runs, 24 Nov night):
line tables 4.66 s, `debug = 0` 4.60 s; the reload is 0.98 s in both (the served wasm is ~45 MB in
both after wasm-bindgen). Line numbers cost about 0.05 s.

## Consequences

- The lab's loop is about 10× faster than with the release build, and the dev wasm is 7× smaller.
- A clean dev build takes longer (viewer: 46 s against 39 s, measured with `debug = 0`), because the
  dependencies are optimized.
  It happens once per toolchain or dependency update.
- Native dev builds and `cargo test` also use these settings. Tests run on optimized
  dependencies. A debugger shows no local variables unless `CARGO_PROFILE_DEV_DEBUG=2` is set.
- The lab no longer checks the release wasm. Performance numbers come from the release build only
  (ADR 0011, `web-ui/perf/`), never from the dev wasm.

## Not decided here

The browser load time of the dev wasm, and restoring the model and tab after a reload, are not
measured. The architecture of the dev loop (a native `bimble serve` behind `bimble.ts`, wasm as a
release artifact only) is the subject of the experiments on branch `spike/native-twin`.
