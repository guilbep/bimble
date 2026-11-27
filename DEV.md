# Developing Bimble

The fast loop for the C core (`ccore/`) behind the Vue lab (`web-ui/`). Every command below was run
on the dev machine (16 GB aarch64 Linux, 25 Nov 2026). A time in the table comes from the report it
names, with that report's conditions; a command with no report has no time. Run the commands from
the repository root unless a line says otherwise.

| Task | Command | Time |
|---|---|---|
| Edit C, see it in the lab | `make dev` (or `cd web-ui && npm run dev`) | edit written → new pixels 0.23 s median: 72 elements, host built by tcc, SwiftShader, the watcher's delay not timed (`ccore/results/bench.md`); 2.1 s at 20,000 elements (`large.md`) |
| One suite | `ccore/test/run.sh json fmt` | seconds (no report) |
| The gate of every change | `make gate` (`ccore/ci/run.sh fast`) | minutes (no report; the last line of the run prints its time) |
| Everything CI runs on Linux | `make full` (`ccore/ci/run.sh full`) | 836 s, 57 suites, machine loaded (`ccore/results/ci.md`, ticket WIN2) |
| Editor include paths | nothing: `ccore/compile_flags.txt` is committed | |
| Edit the native app, see pixels | `make dev-native` | edit → offscreen shot 188–258 ms (section 2b; no report file) |

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
`*.h` in `ccore/src`, `ccore/session` or `ccore/host`: the host is rebuilt (`build.sh host`, 73 ms
median in `ccore/results/bench.md`) and restarted on the same port, token and model; the open page
re-syncs by itself. A compile error is printed and the old host keeps running. Edit written → new
pixels: 233 ms median on the 72-element frame, headless Chromium with SwiftShader, timed from the
edit written (the watcher's delay is not in it); 2.1 s at 20,000 elements (`large.md`).

- Another model: `ccore/dev.sh path/to/model.bimble` (or `.ifc`, `.json`).
- Other ports: `CCORE_DEV_PORT=47500 CCORE_DEV_VITE=5180 ccore/dev.sh`.
- Logs: `ccore/out/dev/host.log`, `ccore/out/dev/vite.log`.
- Vue edits (`web-ui/src`) reload through Vite as usual.
- The wasm lab instead of the native host (no server, as on GitHub Pages):
  `cd web-ui && npm run dev:wasm`, then open `http://localhost:5173/` (rebuild the wasm with
  `npm run ccore:wasm` after a C change).

## 2b. The native Linux app (`bimble`, ADR 0084)

`bimble` is the same C core with a native 3D view (SDL3's GPU API on Vulkan) and the lab's ribbon,
navigator and panels drawn by Dear ImGui, in one process (`ccore/native/`). It needs SDL 3.4 or newer
and FreeType (`pkg-config sdl3 freetype2`). Its shaders' SPIR-V is committed (`ccore/native/shaders/gen/`):
`glslc` (shaderc) only to change a shader (the build makes the header again; `native/build.sh shaders --check`
in `ci/run.sh native` says a header is not made from today's source). Where SDL 3.4
is not packaged: `ccore/ci/sdl3.sh ~/.local/sdl3`, then `PKG_CONFIG_PATH=~/.local/sdl3/lib/pkgconfig`.

```sh
make native                       # = ccore/build.sh native-app: ccore/out/app/bimble (clang -O2, 5 MB)
ccore/out/app/bimble project.bimble          # a window; no file: Home; --en / --fr
make dev-native                   # = ccore/native/build.sh watch: tcc rebuild on every save under
                                  #   ccore/native/, then an offscreen shot of the fixture
make native-test                  # = ccore/ci/run.sh native (the CI job ccore-native)
```

- The edit loop: a save of a shell C file → tcc build (one object per changed file, the core's clang
  -O2 objects cached) 67–110 ms → the offscreen shot written: **188–258 ms** (6 saves, 27 Nov 2026,
  load 7–9). `native/build.sh watch <cmd…>` runs another command after each build.
- Offscreen and scripted (no window, no display): `bimble --offscreen [--size WxH --scale k]
  [--script steps.txt | --preset orbit-zoom] --out dir file` writes `frames.csv`, `inputs.csv` (event →
  the app's work done, event → the GPU's fence), `memory.csv`, `summary.md` and the shots; the steps
  are listed at the top of `ccore/native/ui/drive.c`. `--shot f.png --quit-after n` for one picture.
- Under ASan and UBSan: `ccore/native/build.sh asan` → `ccore/out/app-asan/bimble`.
- **The session log** (NUI2, local only): each windowed session writes `session.log` (versions, GPU, the window's
  and swapchain's sizes at each change, settings, files opened and their times, SDL errors and assertions, a crash's
  stack addresses), `frames.csv` (a row a second), `inputs.csv` (inputs over 100 ms) and `report.txt` (every 10 s)
  under `$XDG_STATE_HOME/bimble/sessions/<UTC time>-<pid>/` (default `~/.local/state/…`; `BIMBLE_LOG_DIR`
  overrides, and turns it on offscreen); the last 20 kept; `bimble --last-log` prints the newest folder; off:
  Settings › Local session log (`native.json` `session_log`). An SDL assertion is logged and the session goes on.
- `--resize-script 1280x720,640x400,…` (offscreen): a size every 3 frames, the window first and the target a frame
  later, as a tiling compositor on Wayland does; every size of a frame comes from the target acquired that frame.
- `--set key=value`: one of the app's own settings (`native.json`) for this run only.
- The shell's data (ribbon, keys, strings, themes, the machine's shape) comes from the TypeScript:
  after a change there, `node web-ui/scripts/export-shell-data.mjs` and commit
  `ccore/native/ui/data/` (`ccore/ci/run.sh unit` checks it).
- Its own environment: `BIMBLE_NATIVE_LOADERS` (link loader threads, default 2), `BIMBLE_LINK_GPU_MB`
  (the links' GPU budget for one run; else the setting), and the host's `BIMBLE_CONFIG_DIR`,
  `BIMBLE_CACHE_DIR`, `BIMBLE_LINK_CACHE_BESIDE=0`, `BIMBLE_LINK_CACHE_MIN`.
- **The tile path** (NUI2, ADR 0084 amendment): `--set renderer_path=tiles` draws every link with a section `TILE`
  (and the open model) by tiles, with exact two-pass occlusion; its settings `draw_visible`, `occlusion_groups`,
  `error_px`, `lod`, `still_px`, `cull_back` (`--set key=value` for a run; Settings › Optimisations).
  `--truth` draws each 3D frame without then with occlusion and compares the id and depth buffers (summary.md:
  frames, frames with ids missing, ids missing, coplanar ties). `--compare k=v,k=v --compare …` runs the same
  script once per set and prints the summaries side by side (`<out>/compare.md`). Driver steps for tiles:
  `look_box fx fy fz tx ty tz` (a camera inside the scene's box), `check_tiles` (every leaf's elements inside
  its box). `BIMBLE_TILES_DEBUG=1`: one line a frame (walk, commands, tests, tiles loaded and wanted);
  `BIMBLE_TILES_SURPLUS=1`: at each `shot`, where the leaves' triangles went (of elements shown, pass 1, pass 2,
  and of elements not shown by cause). `frames_in_flight` (2): the next frame recorded while the GPU draws this one.
- A crash: `ccore/native/symbolize.sh [session folder]` turns the session log's raw stack into functions and
  lines with `ccore/out/app/debug/<build id>.debug` (kept by `native/build.sh app`, the last 20 builds).

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

The host program under the sanitizers (`ci/run.sh san` and `full` do it): `ccore/build.sh asan`
(the host alone, clang -O1, ASan and UBSan, in `ccore/out/asan/`), then
`ccore/test/san_host.sh host_test fs_test` (each node suite on that host, one at a time; any
sanitizer report fails the suite and is printed).

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
- `node ccore/tools/parity.mjs [--write|--check]`: the C host against the committed Rust answers
  (`ccore/golden/parity/`, dumped from a development build that reports 0.18.1); writes
  `ccore/results/parity.md`, whose header says what the counts support and what they do not.
  `--check` (in `ci/run.sh unit|full`) fails only on a crash, a 5xx or malformed JSON. `--rust`
  needs the Rust tree (section 8).
- `node ccore/tools/ux.mjs [--check] [--force] [--builds wasm,native] [--models small,large]`:
  every control of the lab, enumerated from the page and activated with real input in one headless
  Chromium (the production wasm build, then the native host), its verdict (works, says not
  available, dead, error, disabled, not exercised) and the response time of each one that works
  against the 200 ms goal (ADR 0074), plus the Measure tool per pointer move; writes
  `ccore/results/ux.md`. Refuses to write numbers when the 1-minute load is above 4 (`--force`).
  `UX_EXTRA_MODEL=<file>` adds a local model, reported to `ccore/out/ux/ux-local.md` only.
  `--check` (verdicts only, wasm, small model) fails on more dead or error controls than the
  committed report; not in `ci/run.sh` yet (`docs/research/ux-map.md`).
- `ccore/bench/corpus.sh fetch|asan|run|ios|report`: the public IFC corpora (617 STEP files; the
  38 non-STEP files are refused and 173 git-lfs pointers are not fetched) read by C (and by Rust and
  IfcOpenShell when present); `ccore/results/ifc_corpus.md`.
- `node ccore/tools/status.mjs [--write|--check]`: the README's Status block from the reports above
  (each number with its source and conditions). After a report changes, run `--write` and commit
  README.md with it: `ci/run.sh unit|full` runs `--check` and fails on a stale block.
- `ccore/tools/ifc_dump`: built by `build.sh tcc|native`, dumps what C reads or imports from any
  IFC file (`ccore/README.md`).

### 3D timings on the machine's GPU, not SwiftShader (spike NAT1, ticket REAL1, 26 Nov 2026)

Headless Chromium as the lab tests start it by default draws WebGL2 with **SwiftShader** (a CPU
rasteriser) and has **no WebGPU adapter**: every frame time committed before REAL1 is SwiftShader's.
On the dev machine (Apple M1 Pro, Asahi Linux, Mesa) these flags give headless Chromium 153 the real
GPU (checked with `WEBGL_debug_renderer_info` and `navigator.gpu.requestAdapter()`):

| flags (with `--headless=new`) | WebGL2 renderer | WebGPU adapter |
|---|---|---|
| none / `LAB_GPU=software` (the default) | ANGLE, Vulkan 1.3 (SwiftShader Device) | none (with `--enable-unsafe-webgpu --enable-features=Vulkan --use-vulkan=swiftshader`: SwiftShader, `isFallbackAdapter`) |
| `--use-angle=gl --ignore-gpu-blocklist` (`LAB_GPU=gl`) | ANGLE (Mesa, AGX G13/G14, OpenGL ES 3.2): **hardware** | none |
| `--use-angle=vulkan --enable-features=Vulkan --ignore-gpu-blocklist` (+ `--enable-unsafe-webgpu`) (`LAB_GPU=vulkan`) | ANGLE (Mesa, Vulkan 1.4.363 (Apple M1 Pro (G13S C0)), Borealis): **hardware** | vendor `mesa`, architecture `software` (Dawn's name for Mesa's vendor id), **not** a fallback adapter: the M1 Pro (fill 5,800-12,900 Mpx/s against SwiftShader's 720-1,100) |
| `--use-gl=egl` | none (refused: "not found in allowed implementations") | none |

One place decides: `ccore/test/lib/chromium.mjs` (`LAB_GPU=software|gl|vulkan`, default `software` so CI
and the committed SwiftShader numbers do not change). It gives the flags (`gpuFlags()`, `webgpuFlags()`)
and, once the page is up, `rendererInfo()` → `{renderer, hardware}`; `checkGpu()` stops a run whose GPU
is not the one asked (Chromium falls back to SwiftShader silently when a flag is refused). Used by
`tools/frames.mjs`, `tools/ux.mjs`, `tools/perf.mjs`, `test/lib/lab.mjs` (lab_perf, lab_webgpu) and the
lab tests that time inputs or are run on the GPU (`lab_first_input`, `lab_links_3d`,
`lab_quantity_lists`, `lab_measure`, `lab_visibility`); the other `lab_*.mjs` still start SwiftShader
themselves. Any of them on the GPU:

```sh
LAB_GPU=vulkan CCORE_PORT_BASE=auto node ccore/test/lab_first_input.mjs     # input_time.mjs then asserts 200 ms presented
LAB_GPU=vulkan CCORE_PORT_BASE=auto node ccore/test/lab_webgpu.mjs          # the real adapter accepted, pictures = WebGL2's
LAB_GPU=vulkan CCORE_PORT_BASE=auto node ccore/test/lab_webgpu_links.mjs    # GPU2: six links, WebGPU = WebGL2 in 7 states, 10 live switches
LAB_GPU=vulkan node ccore/tools/frames.mjs --force --models fixture,heavy,large                 # WebGL2: the real-GPU section of results/frames.md
LAB_GPU=vulkan node ccore/tools/frames.mjs --force --models fixture,heavy,large --renderer webgpu
LAB_GPU=vulkan HOST_BIN=$PWD/ccore/out/native/bimble-ccore node ccore/tools/frames.mjs --links 17  # a row of that section
LAB_GPU=vulkan HOST_BIN=$PWD/ccore/out/native/bimble-ccore FRAMES_LINK_FOLDER=<a folder of IFC files> \
  FRAMES_CACHE_DIR=<scratch> node ccore/tools/frames.mjs --steps 5,10,14,17   # your own folder, out/frames/folder.md only
```

**The everyday browser (GPU2, 27 Nov 2026).** Chromium 153 as the desktop starts it (no flags) on this machine draws
WebGL2 with "ANGLE (Mesa, AGX G13/G14, OpenGL ES 3.2)" and gives WebGPU **no adapter** (the owner's report); with
`--enable-unsafe-webgpu` alone a SwiftShader fallback adapter; with `--enable-unsafe-webgpu --enable-features=Vulkan
--use-angle=vulkan` the M1 Pro (WebGL2 and WebGPU on Vulkan). Checked with a non-headless browser process on Ozone's
headless display (`--ozone-platform=headless`: no window), which reproduces the desktop's GL renderer; the
`chrome://flags` equivalents (Unsafe WebGPU Support, Vulkan, ANGLE backend: Vulkan) were not checked here.
`bimble-ccore open --app` (or the setting "Open Bimble in an app window") starts Chromium with exactly these three
flags, an app window and its own profile (`<config dir>/browser`); `ccore/test/open_app_test.mjs` checks the arguments
with a fake browser.

A run on the GPU writes only the section "On the machine's GPU" of `ccore/results/frames.md`; a
SwiftShader run keeps it. A timing taken without these flags measures the CPU, not a GPU: say which in
every report. The page's performance panel shows the adapter's names as the API gives them and the fill
rate measured when WebGPU opened its device (`web-ui/src/gpu/device.ts`).

### Investigating performance yourself (ticket VIS1, 27 Nov 2026)

`ccore/tools/drive.mjs` opens the lab headless on the machine's GPU (`--gpu vulkan` by default, `gl` for ANGLE on
OpenGL ES as Chromium on Linux draws by default) and plays real gestures (CDP mouse, wheel and key events at 60 Hz,
not awaited, as the OS sends them). It records everything into `ccore/out/drive/<name>/` (git-ignored; files may
hold your file names: never commit them): the performance panel's report, every frame (kind, script ms, GPU ms by
timer query, draws, triangles, culled batches, proxies, outlines), every input and the frame that showed it, a CPU
profile with its top 20, memory every 250 ms, the `/mesh` requests, console errors, screenshots and `summary.md`.

```sh
ccore/build.sh native                                                     # the host it starts
node ccore/tools/drive.mjs --links 17 --preset orbit-zoom                 # 17 synthetic buildings (kept in ~/.cache/bimble-drive/)
node ccore/tools/drive.mjs --project <your .bimble of links> --size 1492x790 --dpr 2 --settings '{"moving_fps":0}' --preset orbit-zoom
node ccore/tools/drive.mjs --model <a model> --do 'wait ready; orbit 400 0 2000; sleep 150; screenshot stop; wait idle; screenshot still'
DRIVE_UI=<another worktree>/web-ui node ccore/tools/drive.mjs --links 17 --name before   # the page of another tree
node ccore/tools/drive.mjs --compare ccore/out/drive/before ccore/out/drive/after
```

The steps (`wait ready|first-picture|complete|idle`, `orbit DX DY MS [pause]`, `pan`, `zoom STEPS MS`, `click X Y |
element GLOBALID | centre`, `key`, `fit`, `action NAME JSON`, `open panel` / `close panel`, `screenshot`, `sleep`,
`mark`, `repeat N {a, b}`, `drawcalls`) and the presets (`orbit-zoom`, `open`, `walk-inside`) are listed in the
tool's header. A project of over 1 GB of files (or `--lock`) waits for `~/.cache/bimble-measure.lock`; a watchdog
stops Chromium under 2.5 GB available; temp folders go on any exit. `--strip N` takes a screenshot every N moves
and through the refill after a stop (each costs tens of ms: it changes what it measures).

The same functions as an MCP server (stdio), for an agent that wants to hold one session open and poke at it:

```json
{ "mcpServers": { "bimble-drive": { "command": "node", "args": ["ccore/tools/drive.mjs", "--mcp"] } } }
```

Tools: `open {model|project|links, size, dpr, settings, gpu, panel, name}`, `gesture {script: [steps]}`,
`screenshot {name}`, `report`, `profile {seconds}`, `close`.

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
