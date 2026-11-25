# ccore

ccore is an experiment: a small C core behind the lab's `Wasm` interface, built with tcc in the
edit loop (builds in tens of milliseconds) and with clang for release and wasm32. The Rust crates
stay the oracle: C output must match Rust byte for byte. The plan and the tickets are in
`docs/research/c-core-spec.md`.

Today it holds the base every later slice uses: an arena, a growable byte buffer, a JSON writer
(keys in the caller's order, compact or pretty like `serde_json::to_string_pretty`) and two exact
f64 formatters on top of vendored Ryu (`vendor/ryu`, see `SOURCE`): `fmt_f64_json` prints what
`serde_json` prints, `fmt_f64_debug` prints what Rust's `{:?}` prints (the IFC writer uses it).

## Layers

ADR 0068 sets seven layers (`docs/research/core-architecture.md` §5). A file includes only its
own layer and lower ones. L0–L3 have no files, clock, threads or globals; platform code lives only
in the carriers (L6), which hand the session a `cc_platform` (`session/platform.h`: read file,
write file atomically, exists, now, wall clock, random bytes, log). One exception, from §5
("parallel pure functions on a frozen model, one scratch arena per thread"): `src/jobs` (L0) runs
independent tasks on pthreads natively (serial in wasm), with one global, the thread count
(`CC_THREADS`, else the online cores). The IFC display reader and the STEP parser use it.

| Layer | What | Files today |
|---|---|---|
| L0 base | arena, buffers, JSON, CBOR, inflate, number formatting and parsing, GlobalIds, maths, parallel jobs | `src/arena`, `buf`, `json_r`, `json_w`, `cbor`, `inflate`, `fmt`, `strtod`, `guid`, `math`, `jobs`, `src/freestanding/` |
| L1 model | model, validate, canonical, Commands, History | `src/model`, `command`, `history`, `library` |
| L2 io / geom | file (v1, v2), STEP, IFC in and out; scene | `src/file`, `step`, `ifc_in`, `ifc_out`, `scene`, `earcut` |
| L3 api | verbs, query | `src/verbs`, `query` |
| L4 store | journal, repo, remote | (none yet) |
| L5 session | call router, sessions (handles: `cc_session *`), selection, placing, scene and /mesh caches, session file, platform interface | `session/calls`, `session`, `mesh`, `mesh_model`, `platform` |
| L6 carriers | wasm module, HTTP host (native platform, `main`), later mcp and cli | `src/wasm_api.c` (+ `wasm_stub.c`), `host/server.c`, `host/platform_native.c`, `host/main.c` |

A process holds any number of sessions (`cc_session_new(platform)`); the native host serves one
(`main.c` creates it and passes it to `server.c` in `cc_host_config`), the wasm module's `host_*`
exports and `call` work on its default session. `test/two_sessions_test.c` opens two at once.

Build (from the repo root; output goes to `ccore/out/`, which git ignores):

    ccore/build.sh tcc      # debug library and test binaries, needs tcc
    ccore/build.sh native   # clang -O2
    ccore/build.sh wasm     # clang freestanding wasm32, no libc

Run the tests (tcc by default, or any compiler in `CC`). The script exits non-zero on a failure:

    ccore/test/run.sh             # all suites
    ccore/test/run.sh fmt json    # named suites
    CC=clang ccore/test/run.sh

The network tests (`test/*.mjs`, `bench/*.mjs`) take their ports from `test/ports.mjs`, one table
with a fixed default and a unique offset per port. `CCORE_PORT_BASE` unset: the defaults (host
473xx, Vite 51xx/52xx). `CCORE_PORT_BASE=48000`: base + offset, so worktrees that run the same test at
once do not collide (give each its own base, 160 ports apart). `CCORE_PORT_BASE=auto`: a free base from
a hash of the worktree path. `test/host.sh` passes the variable through.

Regenerate the goldens (a standalone Rust crate, not in the workspace; it pins the repo's
serde_json version). Commit the changed `.txt` files:

    cd ccore/golden/floats && CARGO_TARGET_DIR=../../out/cargo cargo run -j 2

## Comparing with Rust on any IFC file

`build.sh native` (and `tcc`) also builds `out/<mode>/ifc_dump` (`tools/ifc_dump.c`), the C twin of
the golden dumper's dump modes. Same files in both output folders, nothing masked:

    cargo run --release -p bimble-api --example ccore_golden -- --dump-read <in.ifc> <rust-dir>
    ccore/out/native/ifc_dump read <in.ifc> <c-dir>          # scene.txt, meshes.txt, time.txt
    cargo run --release -p bimble-api --example ccore_golden -- --dump-import <in.ifc> <rust-dir>
    ccore/out/native/ifc_dump import <in.ifc> <c-dir> [<export.ifc>]   # report, model, export

`meshes.txt` holds one FNV-1a hash per element mesh, so a large file is compared without writing
every mesh. Rust's `model.json` leaves out the georeference (the C model has none). Never commit
the outputs of a client file.

## Dev loop

One command runs the C core behind the unchanged Vue lab and restarts it on every save (needs
tcc, `inotifywait` from inotify-tools, and `npm ci` once in `web-ui/`):

    ccore/dev.sh [model]          # or, in web-ui/: npm run ccore:dev

It builds the host with `build.sh host` (tcc for the files changed since the last build, cached clang -O2 objects for the rest, 15–100 ms), starts Vite (`web-ui/`, port 5173) and the host (port 47400) and
prints the lab URL (`lab: http://localhost:5173/?native=1&gl=1&port=47400&token=…`). Open it once.
Then every save of a `*.c` or `*.h` file in `src/`, `session/` or `host/` rebuilds (50 ms debounce) and
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

Test: `node ccore/test/dev_test.mjs` (host 47411, Vite 5199) edits `session/calls.c` five times, then
breaks and reverts it, and restores it byte for byte. Measured on the 16 GB aarch64 dev machine,
three runs of 5 cycles: rebuild 190-283 ms (all of `build.sh tcc`: library, the test binaries and the host),
host up 9-26 ms.

## The lab on the wasm module (no server)

`npm run ccore:wasm` in `web-ui/` builds `ccore.wasm` and copies it and the demo frame into
`web-ui/public/ccore/`. Then open the lab with `?ccore=1&gl=1`: the page loads the module (244 KB,
112 KB gzip) and runs the same call code as the native host (`session/calls.c`, `session.c`,
`mesh.c` compiled in). `?model=<url>` opens another `.bimble` text file; `window.ccoreSave()`
returns the saved text. Checked by `node test/lab_wasm.mjs`; native and wasm answers are compared
byte for byte by `node test/wasm_run.mjs` (suite `host`).
