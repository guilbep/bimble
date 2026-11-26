# Developing Bimble

The fast loop for the C core (`ccore/`) behind the Vue lab (`web-ui/`). Every command below was run
on the dev machine (16 GB aarch64 Linux, 25 Nov 2026); the times are from those runs. Run them from
the repository root unless a line says otherwise.

| Task | Command | Time |
|---|---|---|
| Edit C, see it in the lab | `make dev` (or `cd web-ui && npm run dev`) | edit → pixels ≈ 0.23 s |
| One suite | `ccore/test/run.sh json fmt` | 0.4 s (tcc), 3.4 s (clang) |
| The gate of every change | `make gate` (`ccore/ci/run.sh fast`) | 68 s |
| Everything CI runs on Linux | `make full` (`ccore/ci/run.sh full`) | 12 min (machine loaded) |
| Editor include paths | nothing: `ccore/compile_flags.txt` is committed | |

## 1. Prerequisites (once)

- **clang and lld** (`wasm-ld` for the wasm32 builds; clang 23 here, the distribution's on CI),
  **cmake** (manifold), **node 24** and npm, **inotify-tools** (`inotifywait`, for `dev.sh`), **Chromium** at
  `/usr/bin/chromium` (or set `CHROMIUM`), **gdb**. On Debian or Ubuntu:
  `scripts/apt-install.sh clang lld cmake inotify-tools poppler-utils` (`pdftotext`: lab_export).
- **tcc at the pinned commit.** A distribution's tcc may be too old (no `stdatomic.h`):
  `ccore/ci/tcc.sh ~/.local/tcc` and put `~/.local/tcc/bin` on `PATH`. `ccore/ci/tcc.sh --commit`
  prints the pin.
- **binaryen (wasm-opt) at the pinned release:** `ccore/ci/binaryen.sh ~/.local/binaryen`, then
  `~/.local/binaryen/bin` on `PATH`. Without it `build.sh wasm-view` misses its size budget.
- **The boolean kernel (manifold), once:** `ccore/deps.sh` (minutes; into `ccore/out/deps/`, not
  committed). Without it the core builds and runs, but openings are drawn uncut.
- **The lab's packages:** `cd web-ui && npm ci`.

Check: `make doctor` prints what is found and what is missing.

No Rust toolchain is needed. Rust is the frozen oracle (ADR 0067, ADR 0072): see section 8.

## 2. The fast loop: edit C, see pixels

```sh
make dev                    # = ccore/dev.sh = cd web-ui && npm run dev
```

`dev.sh` builds the host with tcc, starts Vite on 5173 and the native host on 47400 on a scratch
copy of `ccore/golden/frame.json`, and prints the lab URL
(`http://localhost:5173/?native=1&gl=1&port=47400&token=…`). Open it once. Then save any `*.c` or
`*.h` in `ccore/src`, `ccore/session` or `ccore/host`: the host is rebuilt (41 ms here) and
restarted on the same port, token and model; the open page re-syncs by itself. A compile error is
printed and the old host keeps running. Edit → pixels: 233 ms median (`ccore/results/bench.md`).

- Another model: `ccore/dev.sh path/to/model.bimble` (or `.ifc`, `.json`).
- Other ports: `CCORE_DEV_PORT=47500 CCORE_DEV_VITE=5180 ccore/dev.sh`.
- Logs: `ccore/out/dev/host.log`, `ccore/out/dev/vite.log`.
- Vue edits (`web-ui/src`) reload through Vite as usual.
- The wasm lab instead of the native host (no server, as on GitHub Pages under `/c/`):
  `cd web-ui && npm run dev:wasm`, then open `http://localhost:5173/` (rebuild the wasm with
  `npm run ccore:wasm` after a C change).

## 3. Tests

**One suite (seconds).** The C suites are `ccore/test/<name>_test.c`; name them without `_test`:

```sh
ccore/test/run.sh json fmt          # tcc, the named suites
CC=clang ccore/test/run.sh json     # the same with clang -Werror
make test S="json fmt"              # the same through make
```

**One node or browser test.** Build first (`ccore/build.sh tcc`), then:

```sh
CCORE_PORT_BASE=auto node ccore/test/host_test.mjs
CCORE_PORT_BASE=auto node ccore/test/lab_gl.mjs      # starts Vite, the host and Chromium
```

`CCORE_PORT_BASE=auto` takes a free port per name in 20000..31999, so the same test can run several
times at once, in one worktree or many: `ccore/test/ports.mjs` locks every port it gives
(`<tmpdir>/bimble-ports/<port>`, the pid inside, freed at exit) and bind-tests it. Without the variable
a name gets a stable port (a hash of the name), the next free one if taken. The test's log names each
port it took (`ports: host lab_gl 23456 (auto)`). Two runs of `ci/run.sh` at once in one tree:
`CCORE_CI_LOGS=out/ci2 CCORE_CI_BUILD=0 ccore/ci/run.sh lab` for the second (no rebuild of the
binaries the first one runs).

**The gate.** `make gate` = `ccore/ci/run.sh fast`: build.sh tcc, the C suites with tcc and with
clang ASan+UBSan, build.sh wasm, wasm_run, host.sh. `make full` = `ccore/ci/run.sh full`: also the
Chromium lab suites, every other `test/*.mjs`, the release archive, the weight table and the short
performance report. One line per suite, logs in `ccore/out/ci/<suite>.log`, the last line
`ci: N suites, M failed`. CI runs the same script (`.github/workflows/ccore.yml`).

**Sanitizers.**

```sh
CC=clang CFLAGS="-fsanitize=address,undefined" ccore/test/run.sh json      # ASan + UBSan (the gate runs all suites so)
CC=clang CFLAGS="-fsanitize=memory -fno-omit-frame-pointer" CC_NO_KERNEL=1 ccore/test/run.sh json   # MSan
```

MSan needs every byte of code instrumented, so it runs without the kernel (`CC_NO_KERNEL=1`:
manifold is C++ built without MSan). All 38 C suites under MSan: no MSan report (25 Nov); `ifc_in`
and `ifc_read` fail their `deep_nesting` golden there, as with plain clang and `CC_NO_KERNEL=1`
(a boolean result drawn uncut). Search the logs (`ccore/out/run-clang-*/log/`) for
`MemorySanitizer`, not for FAIL.

## 4. Adding things

Every addition ships with its test in the same commit. A rule of the port: the C core answers what
it serves, and the page never guesses (FAKE1, `web-ui/src/shell/capability.ts`).

- **A verb (an edit by name).** The handler in `ccore/src/verbs.c` (or `verbs_levels.c`,
  `verbs_sheets.c`) returns `Command`s, never edits the model itself (validate, then `canonical()`).
  Its row: `cc_verb_c_additions` in `ccore/src/verb_table.c` (name, one-line summary, compact JSON
  schema, handler), declared in `verb_table.h`. The 14 rows of `cc_verb_table` are Rust's and stay
  byte for byte. Test: a case in `ccore/test/verbs_test.c` (or `verbs_all_test.c`); a golden only
  when it is a file format others read (`ccore/golden/`, with its line in
  `ccore/golden/DEVIATIONS.md` when it differs from Rust on purpose).
- **A query.** A model query: `ccore/src/query.c` (the `NAMES` list and its branch); a session
  query (selection, views, tools): `ccore/session/calls.c`. Test: `ccore/test/query_test.c` or a
  `.mjs` suite that asks the host.
- **An action, its enable rule and its capability.** The row in `ACTIONS` of
  `ccore/session/calls.c` (name, label, tab, shortcut, pressed); its `enabled` rule in
  `action_enabled` of the same file (what `query actions` answers). A page, panel, data set or export that is not an
  action is a capability: a row in `ccore/session/capabilities.c` (true only when this build serves
  it), the list in `capabilities.h`'s header, and `capabilities_test.c`. The page reads both and
  shows "not available yet" otherwise.
- **A page and its shell row.** `web-ui/src/pages/<Name>.vue` is a page by being there
  (`pages/index.ts`); its plain `<script>` block exports `capability` (and `editable` when it
  edits). Navigation goes only through `send(event)` (`web-ui/src/shell/send.ts`) and the
  transition table of `web-ui/src/shell/machine.ts` (the shell reducer: one function moves between
  states, components never write shell state). A control is shown only where its event has a row:
  add the control to `CONTROLS` and the rows to `TRANSITIONS`; `node ccore/test/shell_graph.mjs`
  proves it.
- **A C file.** `ccore/src` (pure: no files, clock, threads or globals; ADR 0068 layers in
  `ccore/README.md`), `ccore/session`, or `ccore/host` (platform code). `build.sh` and
  `test/run.sh` find every `.c` by itself (and every `test/<name>_test.c` as a suite); add it to
  `VIEW_DENY` in `ccore/build.sh` if the viewer wasm must not carry it. Then map its object to a
  feature in `FEATURES` of `ccore/tools/weight.mjs`: an unclassified object fails `ci/run.sh full`.
- **A lab test.** `ccore/test/lab_<name>.mjs` (copy one near it, for example `lab_gl.mjs`). Its
  ports: `port("lab_<name>")` and `vitePort("lab_<name>")`. You do not need a row in
  `ccore/test/ports.mjs`: the name is enough, the lock and the bind test keep two tests apart (ticket
  TEST1). The old second argument (a fixed default) is deprecated and ignored under `auto`; the table
  keeps only the fixed ports a human opens (`tools/perf.mjs`, `tools/parity.mjs`). `ci/run.sh lab|full`
  picks up a new `test/lab_*.mjs` without an edit.

## 5. Debugging

- **gdb on the native host.** tcc's `-g` writes stabs, which gdb no longer reads: debug a clang
  build. `make debug` builds `ccore/out/debug/bimble-ccore` (clang -O0 -g, without the kernel, 4 s),
  then:
  `cd ccore && BIMBLE_TOKEN=dev gdb -ex 'break cc_call' -ex run --args out/debug/bimble-ccore 47400 golden/frame.json`
  and open `http://localhost:5173/?native=1&gl=1&port=47400&token=dev` with Vite running
  (`cd web-ui && npx vite`). Every call of the page stops in `cc_call` (`session/calls.c`).
- **gdb on a suite:** `CC=clang CFLAGS="-O0 -g" ccore/test/run.sh json` builds
  `ccore/out/run-clang-<hash>/test_json` (the newest `run-clang-*`); run it from `ccore/` with its
  argument: `cd ccore && gdb --args out/run-clang-<hash>/test_json golden/floats`.
- **ASan, UBSan, MSan:** section 3. A report names the file and line; `ASAN_OPTIONS=halt_on_error=1`
  stops at the first one.
- **Replay a session.** `bimble-ccore --session[=file] [port] [model]` keeps the session (edits,
  undo steps, selection, rev) in an append-only log, `<model>.ccore-session` by default. Start the
  host again with the same flags, even after `kill -9`, and it prints `session restored from … (rev
  2, 1 undo, 0 redo, 0 selected, 1 entries replayed)`. Keep the model and its session file of a bug
  and replay them under gdb (`dev.sh` runs its host with `--session` too: `ccore/out/dev/`).
- **In the page.** `window.__bbGl` (WebGL viewport: frames, picks, triangles drawn, errors, tool
  calls), `window.__bbCcore` (wasm: load and open times), `window.__bbKeys` (shortcuts),
  `__bbGrids`, `__bbRefs`. Elements the tests click carry `data-bb="…"` markers; `data-bb-shell` on
  the app root names the shell state, `data-bb-ready` is set when a deep link has finished.
- **The host's answers by hand:** `POST /<op>/<name>?t=<token>` with the JSON body
  (`session/calls.c` `cc_call`): `curl -s -X POST 'http://127.0.0.1:47400/query/state?t=<token>'`,
  `curl -s -X POST 'http://127.0.0.1:47400/verb/delete?t=<token>' -d '{"ids":["<GlobalId>"]}'`
  (the token is in the lab URL dev.sh prints, or `BIMBLE_TOKEN`).

## 6. Tools

- `node ccore/tools/weight.mjs`: lines, wasm and native bytes per feature and per file, after the
  builds (`ci/run.sh unit|full` runs it); writes `ccore/results/weight.md`.
- `node ccore/tools/perf.mjs [--full]`: Lighthouse and our own marks on the production build of the
  C lab; writes `ccore/results/perf.md`.
- `node ccore/tools/parity.mjs [--write]`: the C host against the committed Rust answers
  (`ccore/golden/parity/`); writes `ccore/results/parity.md`. `--rust` needs the Rust tree
  (section 8).
- `ccore/bench/corpus.sh fetch|asan|run|ios|report`: the public IFC corpora (617 STEP files)
  read by C (and by Rust and IfcOpenShell when present); `ccore/results/ifc_corpus.md`.
- `ccore/tools/ifc_dump`: built by `build.sh tcc|native`, dumps what C reads or imports from any
  IFC file (`ccore/README.md`).

## 7. Common failures

- **`EADDRINUSE`** on a test port: a process outside the tests took the port between the bind test
  and the start of the host (rare), or a fixed port (`dev.sh`, `tools/perf.mjs`) is taken by another
  run: use `CCORE_PORT_BASE=auto`. A shared offset in `ccore/test/ports.mjs` no longer throws (ticket
  TEST1): rows are hints, the locks decide. A stale lock (a test killed with SIGKILL) is taken over
  when its pid is dead.
- **`weight: … unclassified`** (the `weight` suite fails): a new C file has no feature in
  `FEATURES` of `ccore/tools/weight.mjs`. Add it to the right feature.
- **The lab shows an old C core in wasm mode**, or the release archive holds a `web/ccore/`: a stale
  `web-ui/public/ccore/` (copied by `npm run ccore:wasm`). Run `npm run ccore:wasm` again, or delete
  `web-ui/public/ccore/` (git ignores it).
- **Chromium's first start times out a lab test** (a fresh profile on a new machine or CI job):
  the lab tests wait 60 s for it (15 s was too short on a fresh GitHub job, 3c96892). Run the test
  again once Chromium has started once.
- **A brace lost in a hand merge** (seen twice on 25 Nov when two tickets touched the same block):
  the compiler error points far below the merge. Read the merge hunks first; `git diff --check`
  and `clang -fsyntax-only` with the flags of `ccore/compile_flags.txt` on the merged file name the
  first bad line faster than a full build.
- **Green with tcc, broken with clang and wasm:** tcc accepts a call to an undeclared function
  (it broke every clang and wasm build once, round 4). `make dev` and `ccore/test/run.sh` use tcc:
  run the gate (`make gate`: clang ASan+UBSan and the wasm build) before calling a change done.
- **The editor reports `'buf.h' file not found`:** clangd reads `ccore/compile_flags.txt`
  (committed). Open the repository folder; with the Microsoft C/C++ extension instead, run
  `ccore/tools/compile_commands.sh` and set `C_Cpp.default.compileCommands` to
  `ccore/compile_commands.json`.
- **`dev.sh: tcc not found` / `inotifywait not found`:** section 1.

## 8. Where Rust went

Rust v0.19.0 is the frozen oracle (ADR 0067). Its sources are not on `main` (ADR 0072): tag
`v0.19.0` (the release, installed by `Formula/bimble.rb`) and branch `oracle/rust` (v0.19.0's
sources with the golden dumpers: `crates/bimble-api/examples/ccore_golden.rs`,
`ccore/tools/parity.mjs --rust`, `ccore/tools/mcp_golden.sh`). The C suites check the committed
references under `ccore/golden/` without Rust. To regenerate one: check out `oracle/rust` in a
worktree, run its dumper there, and copy the changed files back in a commit that says why.
