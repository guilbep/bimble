# ccore

ccore is an experiment: a small C core behind the lab's `Wasm` interface, built with tcc in the
edit loop (builds in tens of milliseconds) and with clang for release and wasm32. The Rust crates
stay the oracle: C output must match Rust byte for byte. The plan and the tickets are in
`docs/research/c-core-spec.md`.

Today it holds the base every later slice uses: an arena, a growable byte buffer, a JSON writer
(keys in the caller's order, compact or pretty like `serde_json::to_string_pretty`) and two exact
f64 formatters on top of vendored Ryu (`vendor/ryu`, see `SOURCE`): `fmt_f64_json` prints what
`serde_json` prints, `fmt_f64_debug` prints what Rust's `{:?}` prints (the IFC writer uses it).

Build (from the repo root; output goes to `ccore/out/`, which git ignores):

    ccore/build.sh tcc      # debug library and test binaries, needs tcc
    ccore/build.sh native   # clang -O2
    ccore/build.sh wasm     # clang freestanding wasm32, no libc

Run the tests (tcc by default, or any compiler in `CC`). The script exits non-zero on a failure:

    ccore/test/run.sh             # all suites
    ccore/test/run.sh fmt json    # named suites
    CC=clang ccore/test/run.sh

Regenerate the goldens (a standalone Rust crate, not in the workspace; it pins the repo's
serde_json version). Commit the changed `.txt` files:

    cd ccore/golden/floats && CARGO_TARGET_DIR=../../out/cargo cargo run -j 2
