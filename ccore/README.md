# ccore

ccore is Bimble's core (ADR 0067–0069, 0072): a small C core behind the lab's `Wasm` interface,
built with tcc in the edit loop (builds in tens of milliseconds) and with clang for release and
wasm32. Rust v0.19.0 is the frozen oracle: C output matches the references it wrote
(`golden/`) byte for byte, and the Rust sources are on the branch `oracle/rust`, not on `main`.
The first plan and tickets are in `docs/research/c-core-spec.md`; the commands in `../DEV.md`.

Today it holds the base every later slice uses: an arena, a growable byte buffer, a JSON writer
(keys in the caller's order, compact or pretty like `serde_json::to_string_pretty`) and two exact
f64 formatters on top of vendored Ryu (`vendor/ryu`, see `SOURCE`): `fmt_f64_json` prints what
`serde_json` prints, `fmt_f64_debug` prints what Rust's `{:?}` prints (the IFC writer uses it).

## Layers

ADR 0068 sets seven layers (`docs/research/core-architecture.md` §5). A file includes only its
own layer and lower ones. L0–L3 have no files, clock, threads or globals; platform code lives only
in the carriers (L6), which hand the session a `cc_platform` (`session/platform.h`: read file,
write file atomically, exists, now, wall clock, random bytes, log, list a folder). One exception, from §5
("parallel pure functions on a frozen model, one scratch arena per thread"): `src/jobs` (L0) runs
independent tasks on pthreads natively (serial in wasm), with one global, the thread count
(`CC_THREADS`, else the online cores). The IFC display reader and the STEP parser use it.

| Layer | What | Files today |
|---|---|---|
| L0 base | arena, buffers, JSON, CBOR, inflate, number formatting and parsing, GlobalIds, maths, parallel jobs | `src/arena`, `buf`, `json_r`, `json_w`, `cbor`, `inflate`, `fmt`, `strtod`, `guid`, `math`, `jobs`, `src/freestanding/` |
| L1 model | model, validate, canonical, Commands, History | `src/model`, `command`, `history`, `library` |
| L2 io / geom | file (v1, v2), STEP, IFC in and out; scene | `src/file`, `step`, `ifc_in`, `ifc_out`, `scene`, `earcut` |
| L3 api | verbs, query | `src/verbs`, `query` |
| L4 store | journal, repo, remote | `src/repo` (the history store, read only: ticket H1), `diff` (`version::diff`) |
| L5 session | call router, sessions (handles: `cc_session *`), selection, placing, scene and /mesh caches, session file, platform interface | `session/calls`, `session`, `mesh`, `mesh_model`, `platform`, `versions` (the history queries), `settings_file` (the recent files, FS1), `settings` (the user's settings, About: SET1) |
| L6 carriers | wasm module, HTTP host (native platform, `main`), MCP over stdio, later cli | `src/wasm_api.c` (+ `wasm_stub.c`), `host/server.c`, `host/static.c`, `host/platform_native.c`, `host/main.c`, `host/mcp.c`, `host/fs.c` (the page's file dialog, `POST /stage`: FS1); the native Linux app `bimble` (`native/`: ADR 0084, NUI1; SDL3, its GPU, threads) |

A process holds any number of sessions (`cc_session_new(platform)`); the native host serves one
(`main.c` creates it and passes it to `server.c` in `cc_host_config`), the wasm module's `host_*`
exports and `call` work on its default session. `test/two_sessions_test.c` opens two at once.

Build (from the repo root; output goes to `ccore/out/`, which git ignores):

    ccore/build.sh tcc      # debug library and test binaries, needs tcc
    ccore/build.sh native   # clang -O2
    ccore/build.sh release  # the Linux release archive (see "Release")
    ccore/build.sh wasm     # clang freestanding wasm32, no libc
    ccore/build.sh wasm-view  # the viewer-only wasm32 (ADR 0069), fails over 1,000,000 bytes
    ccore/build.sh native-app # the native Linux app, out/app/bimble (ADR 0084; DEV.md section 2b)

The viewer-only wasm (`out/wasm/ccore-view.wasm`, ticket W1) is the wasm build without the files in
`VIEW_DENY` (build.sh: verbs, the verb table, the IFC import and writer), with `-DCC_VIEW_ONLY` and
`-Os`. It reads `.bimble`, `.json`, IFC (read-only) and `.bimbleview`, draws, selects, hides, clips
and answers the read queries as the editor build, byte for byte (`node test/wasm_view_run.mjs`),
but `verbs` (`[]`) and `describe` (no verb).
The calls that edit, save or export answer "this build is view-only" (`session/view_only.c`, one
line at the top of `cc_call`; `VIEW_ONLY` in `src/wasm_api.c`). Sizes per file and what is left
out: `results/wasm-size.md` (`build.sh wasm-size [view]` prints the table). In the lab: `npm run
ccore:wasm-view` in `web-ui/` copies it to `public/ccore/ccore-view.wasm`.

Drawings on paper go out as SVG (`src/paper.c`) and as a one-page vector PDF (`src/pdf_out.c`,
ticket D6; not `pdf_in`, the reader of links): the same paper items, Rust's `Sheet::to_pdf` byte
for byte but the packed Roboto files (`golden/DEVIATIONS.md` 12). A Roboto PDF embeds the two
font files, which the caller hands over (`cc_pdf_options.roboto`, `cc_paper_sheet_pdf` of
`session/sheet_svg.h`; 350 KB each, not in the wasm); Helvetica needs none and is the app's default
(ticket SIZE1: no font file is shipped; `sheet_pdf {"font":"roboto"}` and `query pdf_fonts`,
`host/fonts.c` for where the native host looks). Suite `pdf_out`.

They also go out as DXF (`src/dxf_out.c`, ticket DXF1, ADR 0082; not `dxf_in`, the reader of
links): the same items, AC1015 ASCII, a view in model space at 1:1 in its own millimetres, a sheet
in paper millimetres, one layer per IFC class and role (`query view_dxf`, `sheet_dxf`; not in the
viewer-only wasm). C-own goldens `golden/dxf_c/`. Suite `dxf_out`.

Run the tests (tcc by default, or any compiler in `CC`). The script exits non-zero on a failure:

    ccore/test/run.sh             # all suites
    ccore/test/run.sh fmt json    # named suites
    CC=clang ccore/test/run.sh
    CC=clang CFLAGS="-fsanitize=address,undefined" ccore/test/run.sh

`test/run.sh` compiles the library once (one object per file, in parallel), then compiles, links
and runs each suite as a job of its own, as many at once as there are CPUs (`CCORE_TEST_JOBS=1`:
one by one). Each suite's output is printed whole, in name order, with its compile and run seconds;
the last line gives the totals. Every suite runs, even after a failure, and any failure (a FAIL
check, a crash, a sanitizer report, a compile error) makes the exit code 1. On the dev machine: all
26 suites in 12 s with tcc, 29 s with ASan+UBSan (`results/ci.md`).

The network tests (`test/*.mjs`, `bench/*.mjs`) take their ports from `test/ports.mjs` by name
(`port("lab_gl")`): no table row (ticket TEST1). Every port is locked for the life of the process (a
file per port under `<tmpdir>/bimble-ports/`) and bind-tested, so tests running at once never share
one. `CCORE_PORT_BASE=auto` (ci/run.sh's default): a free port in 20000..31999. Unset: a stable port
per name (a hash), the next free one if taken. `CCORE_PORT_BASE=48000`: base + a hashed offset in
0..159. Never 47400 or 5173 (`dev.sh`). `test/host.sh` passes the variable through.

CI: `ccore/ci/run.sh fast` is the gate every ticket passes (build.sh tcc, test/run.sh with tcc and
with clang ASan+UBSan, build.sh wasm, wasm_run, host.sh); `ccore/ci/run.sh full` adds the Chromium
lab tests and every other `test/*.mjs` suite. `unit`, `san` and `lab` are the three parts of `full`:
`unit` all but the sanitizers and the lab, `san` the C suites under ASan+UBSan, `lab` the Chromium
suites; `clang` is macOS's (ticket MAC1: test/run.sh with clang and the kernel, test/host.sh on the
clang host). One line per suite, logs in `out/ci/`, a last line `ci: N suites, M failed`, exit 1 on
a failure; `CCORE_PORT_BASE` defaults to `auto`. GitHub runs the three parts on Linux as three jobs
side by side (`.github/workflows/ccore.yml`: `ccore-linux`, `ccore-san`, `ccore-lab`; tcc built
from source by `ci/tcc.sh`, manifold by `deps.sh`, both cached) and `clang` on macOS
(`ccore-macos`). Times and what blocks macOS and Windows: `results/ci.md`.

macOS (ticket MAC1): clang only. The scripts run under macOS's own bash 3.2 and BSD tools
(`portable.sh`). `build.sh native` and `release`, `deps.sh`, `test/run.sh`, `test/host.sh`
(`HOST_BUILD=native`) and `ci/run.sh clang` work there; tcc, `build.sh tcc|host|wasm*` (Apple clang
has no `wasm-ld`) and `dev.sh` (`inotifywait`, `setsid`) are not ported: the edit loop and the wasm
builds stay on Linux. The lab tests take their browser from `CHROMIUM` (`test/ports.mjs`).

The float goldens (`golden/floats/*.txt`) were written by a standalone Rust crate that pins the
Rust release's serde_json version. It is on the branch `oracle/rust` (`ccore/golden/floats/`): to
regenerate, run `CARGO_TARGET_DIR=../../out/cargo cargo run -j 2` there and commit the changed
`.txt` files here.

## Release

The desktop is the user's browser and a local C server (ADR 0069 rule 2, ticket R1).
`ccore/build.sh release` builds the host with clang `-O2`, with manifold and libstdc++ linked
static (it fails without the kernel: run `deps.sh` first), builds the lab with Vite (`web-ui/`,
needs `node_modules`; what is in `web-ui/public/` goes in too), and writes
`out/release/bimble-ccore-<VERSION>-<arch>-<os>.tar.gz`: `bimble-ccore`, `web/`, `LICENSE` and
`README.txt` in one folder. `<os>` is `linux`, or `macos` (libc++ is the system's; stripped, then
signed ad hoc; `MACOSX_DEPLOYMENT_TARGET`, default 11.0; `CMAKE_OSX_ARCHITECTURES=x86_64` for
`deps.sh` and `build.sh release` makes the Intel archive on an arm64 Mac). `node test/release_test.mjs` unpacks it in a temp folder and checks it
(`ci/run.sh full` builds and runs both).

    ./bimble-ccore open [--port N] [--no-browser] [--web dir] [--session[=file]] [model]
    ./bimble-ccore --version

`open` reads the files under `<exe dir>/web/` (or `--web`) once at start (`host/static.c`: regular
files only, no symbolic link, no hidden name, at most 4096 files and 64 MiB in all, a content type
by extension). It listens on a free port of 127.0.0.1 (or N), prints
`http://127.0.0.1:<port>/?native=1&gl=1&port=<port>&token=<t>` and starts `xdg-open` on it
(`open` on macOS). A GET
of any path but `/change`, `/frame` and `/mesh` is a file of that list or 404, without the token: a
request path is matched against the list, never joined to a file path. The calls, `/change` and
`/mesh` keep the token; the Host and Origin checks hold for every request, and the host's own
origin is allowed beside the others. The old form (`bimble-ccore <port> [model]`) serves no files.

The calls that write a file (the token is their only gate; `host/fs.h` has the list with the
code): at a path the caller gives, `action save_as`, `export_ifc4`, `export_ifc2x3` and
`share_for_viewing` with `{path}`, and `query sheet_pdf`, `view_pdf`, `sheets_pdf` with `{path}`;
next to the open model, `action save`, the exports and `share_for_viewing` without a path
(`<stem>.ifc`, `<stem>.bimbleview`; refused for a new model, never over the open file), `action
commit`, `branch_create`, `branch_switch` and `merge` (the folder `<model>.bimble-history/`, and the
model file on a switch or merge), and with `--session` the session file `<model>.ccore-session`
(a file holding changes not saved that the host cannot restore is renamed
`<session>.orphan-<time>`, never overwritten: `query state` `session_set_aside`); in the host's
own folders, `POST /stage` and the settings (`BIMBLE_CONFIG_DIR`). `link_model`, `link_pdf`,
`link_dxf` and `manage_links {op: "locate", path}` only read the path given.

`ccore/VERSION` is the archive's version (`--version`); the IFC writer's `CC_IFC_VERSION` and the
`version` of `query state` stay as they are (the goldens hold them). GitHub (`ccore.yml`): job
`ccore-release` builds and checks the archives on Ubuntu 22.04 (glibc 2.35), x86_64 and aarch64,
and on macOS 15, arm64 and x86_64 (Rosetta 2 runs the Intel one's test), on a manual run, on a tag
`ccore-v<VERSION>` and on a push whose head commit message holds `[ccore-release]`; on such a tag
`ccore-prerelease` attaches them and their `SHA256SUMS` to a GitHub prerelease and prints the
Homebrew formula of that release (`ci/formula.sh <version> SHA256SUMS`), to commit on `main` as
`Formula/bimble-ccore.rb` (`brew tap guilbep/bimble https://github.com/guilbep/bimble && brew
install bimble-ccore`). Never a tag `v*`: that prefix is the Rust releases' (v0.19.0, the site root
of GitHub Pages and `Formula/bimble.rb`). The steps: `docs/RELEASING.md`.

## MCP over stdio

`out/tcc/bimble-ccore mcp [--read-only] model.bimble` is an MCP server for agents (ADR 0048, ADR
0068 rule 5), the twin of Rust's `bimble mcp`: JSON-RPC 2.0, one message per line on stdin (at
most 1 MiB), answers on stdout, logs on stderr. Tools `list_verbs`, `describe_verb`, `run_verb`,
`query`, `undo`, `redo`, `save`, from the verb table and the model queries, on the server's own
model and history; nothing is written until `save`. `run_verb` also takes `dry_run` and `if_rev`,
and `--read-only` refuses every change (`golden/DEVIATIONS.md` §3). Test: `node
test/mcp_test.mjs` (Rust's transcript in `golden/mcp/`, made by `tools/mcp_golden.sh`). For
Claude Code: `claude mcp add bimble -- <repo>/ccore/out/tcc/bimble-ccore mcp <model>`.

## Comparing with Rust on any IFC file

`build.sh native` (and `tcc`) also builds `out/<mode>/ifc_dump` (`tools/ifc_dump.c`), the C twin of
the golden dumper's dump modes. Same files in both output folders, nothing masked. The `cargo`
lines run in a worktree of the branch `oracle/rust` (Rust is not on `main`, ADR 0072):

    cargo run --release -p bimble-api --example ccore_golden -- --dump-read <in.ifc> <rust-dir>
    ccore/out/native/ifc_dump read <in.ifc> <c-dir>          # scene.txt, meshes.txt, time.txt
    cargo run --release -p bimble-api --example ccore_golden -- --dump-import <in.ifc> <rust-dir>
    ccore/out/native/ifc_dump import <in.ifc> <c-dir> [<export.ifc>]   # report, model, export

`meshes.txt` holds one FNV-1a hash per element mesh, so a large file is compared without writing
every mesh. Rust's `model.json` leaves out the georeference (the C model has none). Never commit
the outputs of a client file.

The public IFC corpora (buildingSMART certification and sample files, IfcOpenShell's test files and
others; ticket IFC1, `results/ifc_corpus.md`): `bench/corpus.sh fetch` clones them at pinned commits
into `$BIMBLE_IFC_CORPUS` (default `~/.cache/bimble-ifc-corpus`, never the repo), `asan` builds an
ASan+UBSan `ifc_dump`, `run` reads every file with C, Rust (`$RUST_DUMP`) and ASan (120 s, 6 GB
each), `ios` compares with IfcOpenShell on a sample, `report` prints the tables.

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

The same lab is on GitHub Pages at https://guilbep.github.io/bimble/c/ (ticket PG1), beside the Rust
lab at the site root. `npm run build:ccore` in `web-ui/` builds both wasm modules, copies them and
the demo frame, and runs `vite build --base ./` with `VITE_CCORE=1`: the page starts on the C core
without parameters (`ccore=1&gl=1`), `?view=1` loads `ccore-view.wasm` (a verb answers "this build is
view-only"), and the status bar shows "Preview, C core <commit>". The wasm has no boolean kernel, so
openings are drawn uncut. `pages.yml` builds it in every deployment, from `main` unless the input
`ccore_ref` names another; a failed C build leaves the root (the v0.19.0 lab) published without
`c/`. Checked by `node test/lab_pages.mjs`: the build served from a sub-path, place
and undo, the viewer-only build, and no request outside `/c/`.
