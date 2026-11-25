# C slab spike: the Rust baseline (edit → rebuild in bimble-geom)

2026-11-24, aarch64, 8 cores. rustc 1.100.0 (0ef1bc386 2026-11-09), dev profile (ADR 0065), mold linker. hyperfine 2.0, 1 warm-up, 5 runs each. Same machine and day as `bench.md`.

Method: `bench/rust_loop.sh`. Before each run, `--prepare` replaces the line
`#[allow(dead_code)] fn spike_touch() -> u32 { N }` at the end of `crates/bimble-geom/src/lib.rs`
with a fresh `N` (the same kind of edit as `bench/touch.sh` makes in `slab.c`). A trap restores `lib.rs`.
`bimble-geom` holds `extrude_tris`, `mesh_from_tris` and the slab path that the C slice ports.

## edit→rebuild (Rust)

| build | median ms | min ms | peak RSS MB |
|---|---|---|---|
| (1) `cargo build -q -p bimble-geom` (edited crate only) | 251 | 235 | 185.8 |
| (2) `cargo build -q -p bimble-geom --example slab_dump && slab_dump scene` | 343 | 336 | 186.2 |
| (3) `cd web-ui && npm run -s wasm:dev` (whole editor to wasm, files ready) | 3975 | 3957 | 1265.9 |

## Side by side with C (`bench.md`, 10 runs)

| loop | C | median ms | Rust | median ms | Rust / C |
|---|---|---|---|---|---|
| native: edit → mesh | tcc (native) | 12 | (2) slab_dump | 343 | 28.6× |
| native: edit → mesh | clang -O0 (native) | 97 | (2) slab_dump | 343 | 3.5× |
| floor: edited code only | tcc (native) | 12 | (1) bimble-geom | 251 | 20.9× |
| wasm: edit → files ready | clang wasm -O0 (wasm-dev) | 60 | (3) wasm:dev | 3975 | 66× |

Peak RSS: tcc 3.6 MB vs (2) 186 MB (52×); clang wasm -O0 99.4 MB vs (3) 1266 MB (12.7×).

## Caveat: language and scope are mixed

The C slice is about 335 lines (`slab.c` 257 + `slab.h` 27 + the wasm glue) and does one slab.
The Rust workspace is about 63k lines (62,882 in `crates/**/*.rs`; `bimble-geom` alone is 4,486).
Row (3) rebuilds and links the whole editor (every crate that depends on `bimble-geom`, then
`wasm-bindgen` and the copy into `web-ui`), so its 66× mixes the language and toolchain effect
with the scope effect: it is the real cost of the lab loop today, not a fair C-vs-Rust number.
Rows (1) and (2) are the closer like-for-like: one crate (plus, for (2), one small binary that
links `bimble-core`), but `bimble-geom` is still about 13× more code than the C slice.

Not measured: Rust edit→pixels in the browser (the C side has 118 ms median); a Rust crate cut
to the same 335-line scope; release or `web` profile builds; `cargo check` or rust-analyzer latency.
