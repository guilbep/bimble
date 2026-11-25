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

## Dev loop

One command runs the C core behind the unchanged Vue lab and restarts it on every save (needs
tcc, `inotifywait` from inotify-tools, and `npm ci` once in `web-ui/`):

    ccore/dev.sh [model]          # or, in web-ui/: npm run ccore:dev

It builds with `build.sh tcc`, starts Vite (`web-ui/`, port 5173) and the host (port 47400) and
prints the lab URL (`lab: http://localhost:5173/?native=1&gl=1&port=47400&token=…`). Open it once.
Then every save of a `*.c` or `*.h` file in `src/` or `host/` rebuilds (50 ms debounce) and
restarts the host on the same port, token and model; the open page sees the new epoch in the
change feed and re-syncs by itself. Each cycle prints `rebuilt in N ms, host up in M ms` (host up:
from the stop of the old host to "listening" of the new one). A build error prints the compiler
output and keeps the old host running. Ctrl-C stops Vite, the host and the watcher.

- model: default, a fresh copy of `golden/frame.json` in `out/dev/` per run (the golden is never
  written). The host log is `out/dev/host.log`, Vite's is `out/dev/vite.log`.
- `CCORE_DEV_PORT` (host, default 47400), `CCORE_DEV_VITE` (Vite, default 5173; other ports are
  passed to the host in `BIMBLE_ORIGINS`), `BIMBLE_TOKEN` (default: random, one per run).
- The host runs from a copy of the binary (`out/dev/bimble-ccore`), so a build never writes the
  file of the running host.

Test: `node ccore/test/dev_test.mjs` (host 47411, Vite 5199) edits `host/calls.c` five times, then
breaks and reverts it, and restores it byte for byte. Measured on the 16 GB aarch64 dev machine,
three runs of 5 cycles: rebuild 190-283 ms (all of `build.sh tcc`: library, the test binaries and the host),
host up 9-26 ms.
