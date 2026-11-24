# Rust slab spike: the slab.c scope in Rust, edit → rebuild and edit → pixels

`rust-slab/` is a standalone crate (empty `[workspace]`, zero dependencies) that ports `slab.c` line
for line: the same algorithm, fixed-capacity arrays, no allocation in the mesh path. The native bin
`slab_dump` prints `local` and `scene`, both `cmp` equal to `expected/`. The cdylib exports the
`main_wasm.c` ABI with `#[unsafe(no_mangle)] extern "C"`, no wasm-bindgen, no imports, and the dev and
release modules both give the golden `v/i/e` lines. `f32::cos` (libm in compiler-builtins on wasm32)
gives the glibc bits for cos(20°), so no hard-coded constant was needed.

2026-11-24, aarch64, 8 cores. rustc 1.100.0 (0ef1bc386 2026-11-09). Linker: cc (GCC) 16.1.1 with
GNU ld 2.46.0 (native, no mold); rust-lld (wasm). Dev profile: `debug = "line-tables-only"` (ADR 0065);
release profile: defaults. Chromium headless (SwiftShader), the same page and CDP loop as C.
10 runs each, 1 warm-up. Script: `bench/rust_slab_bench.sh` (hyperfine, `--prepare` appends a fresh
dead `fn spike_touch_N() -> u32 { N }` to `src/lib.rs`); `bench/rust_pixels_loop.mjs` is
`bench/pixels_loop.mjs` with only the edited file and the build command changed.
Raw JSON: `results/out/rust-slab-*.json`. C rows: `results/bench.md` (same machine, same day).

## edit → rebuild

| build | median ms | min ms | peak RSS MB |
|---|---|---|---|
| C: tcc (native) | 12 | 10 | 3.6 |
| C: clang -O0 (native) | 97 | 89 | 100.8 |
| **Rust: cargo build + run slab_dump (native)** | **224** | 212 | 118.6 |
| C: clang wasm -O0 (wasm-dev) | 60 | 53 | 99.4 |
| **Rust: cargo build --target wasm32 (dev, lib + bin)** | **118** | 111 | 115.2 |
| Rust: cargo build --target wasm32 --lib (dev, cdylib only) | 73 | 64 | 114.5 |
| C: clang wasm -O2 (wasm) | 264 | 239 | 115.3 |
| **Rust: cargo build --release --target wasm32** | **290** | 255 | 129.5 |

The plain wasm dev build also links the `slab_dump` bin as a second `.wasm` (Cargo has no
per-target bin filter); `--lib` builds only the module the page loads, like `build.sh wasm-dev`.
The native C rows time the build only; the Rust native row also runs the dump (about 1 ms).

## edit → pixels (SPIKE_K flip, cargo build --target wasm32 dev, slab.reload, first changed frame)

| step | C median | C p95 | Rust median | Rust p95 |
|---|---|---|---|---|
| build (t1−t0) | 63 | 77 | 120 | 135 |
| reload→pixels (t2−t1) | 42 | 82 | 87 | 101 |
| total edit→pixels (t2−t0) | **118** | 145 | **209** | 220 |

## Ratios (Rust / C, medians)

| | ratio |
|---|---|
| native: Rust vs clang -O0 | 2.3× (224 / 97) |
| native: Rust vs tcc | 18.7× (224 / 12) |
| wasm dev, lib + bin | 2.0× (118 / 60) |
| wasm dev, `--lib` only | 1.2× (73 / 60) |
| wasm release vs clang -O2 | 1.1× (290 / 264) |
| edit → pixels total | 1.8× (209 / 118) |

## Size and lines

| | C | Rust |
|---|---|---|
| `.wasm` dev | 11,475 B | 1,867,989 B (122,975 B after `wasm-strip`) |
| `.wasm` release | 9,655 B | 33,360 B (28,126 B after `wasm-strip`) |
| source lines | `slab.c` 257 (+ `main_wasm.c` 76 for the ABI and libc shims) | `src/lib.rs` 412 after `cargo fmt` (ABI included), `src/main.rs` 53 |
| peak RSS, wasm dev build | 99.4 MB | 114.5–115.2 MB |

## What this shows and does not show

At equal scope, Rust is slower to iterate than C, but by a small constant, not by an order of
magnitude: edit → pixels is 209 ms against 118 ms. The build itself is close when Cargo builds
only the module the page loads (73 ms against 60 ms); building the dump bin too costs another
~45 ms, and Cargo's fixed overhead (fingerprints, a second link) is most of the gap. About half
of the remaining difference is in the browser, not the compiler: the dev module is 1.87 MB
(line tables plus std's panic and formatting code) against 11 KB, so reload → pixels takes 87 ms
against 42 ms. Release builds are on par (290 ms against 264 ms). tcc stays in a class of its
own for native (12 ms). This does not show how either language scales: one 257-line file is the
best case for C's "no build system" and for Cargo's per-crate overhead alike, and the 3.96 s and
251 ms measured earlier for the editor and `bimble-geom` come from scope (dependencies, generics,
code size), not from Rust against C on the same code. One machine, 10 runs, no tuning of the dev
profile (`debug = 0`, `panic = "abort"` or a smaller module could close part of the
reload → pixels gap; not measured).
