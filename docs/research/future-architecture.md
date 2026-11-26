# Bimble after the move to C: architecture and port order

25 Nov 2026. Fable 5.2. Study only: no build was run. **M** = measured (source cited).
**E** = estimate. **I** = inference, not checked in code or on a machine.
Paths are relative to this worktree (branch `rewrite/c-core`). Web facts cite the page opened.

Pierre decided (25 Nov): go fully to C after one final Rust release; do not keep two cores.
This note answers his three questions and ranks the port.

My earlier note (`spikes/compile-speed/results/path-upward.md`, rung 9) advised against a C
rewrite and estimated 300–600 h. Since then: 12,136 lines of C, equal to Rust byte for byte, in
one day (`docs/adr/0067-c-core-for-the-lab-loop.md:44-66`). The estimate was too high for the
core. The dense parts (plans, PDF, large files) are still untested.

## 1. The idea in one picture

```
                       web-ui (Vue): the only UI, 100 % shared
                 bimble.ts: one `Wasm` interface (web-ui/src/bimble.ts:200)
                    |                                   |
        wasm in the page                     local HTTP, 127.0.0.1 + token
        (ccore.ts, no server)                (the twin protocol, ccore/host/)
           |            |                               |
   ccore-view.wasm  ccore-edit.wasm            bimble (native C): threads, files,
   web viewer       web editor / PWA           history, CLI, MCP; serves web-ui itself
                                                        |
                                    shown in: the user's browser, or a thin window
        glviewport.ts (WebGL2) draws BMSH meshes in every case
```

Three rules:

1. **The page is the app.** No native GUI code. egui and wgpu are not ported (27,837 Rust lines
   in `bimble-viewer` and `bimble-render`, M by `wc -l`).
2. **Two attachments only**: wasm in the page, or local HTTP. Both exist and pass the same
   tests (`ccore/README.md:57-64`).
3. **Logic is in C; TypeScript draws and forwards.** This is ADR 0048 point 4, kept.

One constraint drives the shell choice. Every `Wasm` call is synchronous
(`web-ui/src/bimble.ts:200-213`, `:261`). A webview "bind" call is asynchronous (I: true for
`webview_bind` and for Electron IPC). So in-process FFI would force a rewrite of `bimble.ts` and
of every caller. Wasm and local HTTP (sync XHR) keep the interface as it is.

## 2. Desktop shell

Answer to question 1: yes, Electron works and reuses 100 % of web-ui. So does every option
below. The choice is about size, old machines and packaging.

| Option | Size added | RAM | GPU on old machines | Core attaches by | Packaging | web-ui reuse |
|---|---|---|---|---|---|---|
| A. Browser + local C server (`bimble file.ifc` opens `http://127.0.0.1:<port>`) | 0. The binary is the C core plus the web-ui files: 1–3 MB (E) | the user's browser tab | Best: the user's Chrome, Edge or Firefox, with their driver workarounds | HTTP (exists) | one binary per OS | 100 % |
| B. Thin window on the system webview (`webview/webview`: WebView2, WKWebView, WebKitGTK) | under 1 MB (E) | one webview | Windows: Chromium (good). macOS: WebKit (good, I). **Linux: risk** | HTTP, same as A | installer per OS; WebView2 check on Windows 10 | 100 % |
| C. Electron | 244 MiB app on macOS in one test (M by its author, N=1) | 409 MB with 6 windows, same test | Good and the same on every OS: one Chromium | HTTP child process, or wasm | electron-builder; Node toolchain | 100 % |
| D. CEF | Chromium size, like C (I) | like C | like C | C API, in-process | heavy build | 100 % |
| E. Tauri | 8.6 MiB in the same test | 172 MB | same webviews as B | Rust shell | good tooling, but **Rust returns** | 100 % |
| F. Wasm-only PWA | 0 | browser tab | like A | wasm | none | 100 % |

Facts behind the table:

- Electron and Tauri numbers: one macOS test, two demo apps
  (https://dev.to/gethopp/tauri-vs-electron-performance-bundle-size-and-the-real-trade-offs-1el4).
  Treat them as rough.
- `webview/webview`: MIT, C API (`webview_create`…), 14.3k stars, WebKitGTK 6.0/GTK 4 or
  4.1/GTK 3 on Linux, WebView2 on Windows (https://github.com/webview/webview). The release date
  was not visible on the page: check before adopting.
- WebView2 is part of Windows 11. "The vast majority" of Windows 10 devices have it. The
  bootstrapper is about 2 MB. A fixed version is over 250 MB: do not ship that
  (https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/distribution).
- WebKitGTK: "WebGL2 context creation succeeds even when the result is backed by a software
  rasterizer", and the app cannot detect it because the renderer string is masked
  (https://v2.tauri.app/develop/debug/linux-graphics/). For a 3D app on old Linux laptops this
  is the main risk of B and E.
- File System Access API (open and save in place): Chrome and Edge 86+ only. Firefox: negative.
  Safari: opposed (https://web-platform-dx.github.io/web-features-explorer/features/file-system-access).
  So F alone cannot save in place for Firefox and Safari users. It downloads instead.
- A page served from localhost that calls localhost does not trigger Chrome's Local Network
  Access prompt. A public site calling 127.0.0.1 does
  (https://developer.chrome.com/docs/modern-web-guidance/guides/security/local-network-access).
  So the C host must serve web-ui itself. Never point the public web page at a local port.

Security of the local server. It already binds 127.0.0.1 and checks token, `Origin` and `Host`
(`ccore/host/server.c:473-479`). To add before shipping: a random port, the token moved out of
the URL after the first load (I: a cookie or `sessionStorage`), a body limit on every route,
exit when the last page closes, fuzzing of the HTTP parser.

**Recommendation, ranked by evidence and cost:**

1. **Ship A first.** It exists, costs days, and gives the best GPU path. Weak points: it looks
   like a tab; the browser owns Ctrl+W and Ctrl+S; process lifetime needs care.
2. **Then B on Windows and macOS** (one small C file around `webview/webview`). On Linux keep A
   until WebKitGTK is measured. Cheap middle step (I, not tested): start an installed Chrome
   or Edge with `--app=<url>` for a window with no browser chrome.
3. **Electron is the fallback, not the plan.** Choose it only if B fails the old-machine gate
   (section 7) on two real machines. It costs about 100–250 MB and a Node toolchain. That
   fits badly with "fast on old machines" (`ROADMAP.md:8`).
4. **Not Tauri** (it brings Rust back). **Not CEF** (Electron's size, more build work).
5. **F is not a shell. It is the web editor** (section 3), installable from Chrome and Edge.

This closes ROADMAP open decision 3 (`ROADMAP.md:199`) and ADR 0048 point 6
(`docs/adr/0048-one-api-for-ui-cli-and-agents.md:32-33`). It needs an ADR.

Dev loop: unchanged. `ccore/dev.sh` and Vite stay. The shell is never in the loop.

## 3. Cores and products

Answer to question 3: yes to several products, no to several cores. One C codebase, one web-ui,
build flags per module.

| Build | Compiler, target | Contains | Left out | Size |
|---|---|---|---|---|
| `ccore-view.wasm` (web viewer) | clang, wasm32 freestanding | read `.bimble`, IFC, `.bimbleview`; scene; queries; hide, isolate, section, measure; light edits of ADR 0064 (rename, material, note) | drawing, PDF, fonts (700 KB of TTF, `crates/bimble-drawing/fonts/`), export, history, map table | today 244 KB, 112 KB gzip (M, `ccore/README.md:60`). Budget: 400 KB, 160 KB gzip (E) |
| `ccore-edit.wasm` (web editor, PWA) | same | everything but threads, local files, collaboration | — | 0.6–1.2 MB (E). Fonts and the boolean kernel fetched on first use |
| `bimble` (desktop, CLI, MCP) | clang -O2 per OS | everything, pthreads, files, history, HTTP host, embedded web-ui | — | 1–3 MB plus the boolean kernel (E) |
| dev host | tcc | same sources as `bimble` | optimisation | builds in 73 ms today (M, `ccore/results/bench.md`) |

For scale: the Rust release wasm is 16 MB (task brief; 18 MB raw in
`docs/research/2026-11-20-wasm-size.md:30`).

- Flags select **files**, not lines inside functions. A missing feature answers "not in this
  build", as `verbs.c:915` does today. web-ui reads what is enabled from `query actions`. It
  never tests a build name.
- This keeps ADR 0050 (two web builds). If `ccore-edit.wasm` measures under about 500 KB gzip,
  merge the two builds into one. Decide by measurement.
- Web editor feasibility. wasm32 memory tops out at 4 GB (https://v8.dev/blog/4gb-wasm-memory).
  Threads need `SharedArrayBuffer`, which needs cross-origin isolation
  (https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/SharedArrayBuffer).
  GitHub Pages cannot set those headers (I). Proposal: no shared memory. Parse a large IFC in a
  Worker with its own module instance and post the result back. The module stays
  single-threaded (`ccore/src/wasm_api.h:17-18`). Not built, not measured.
- Large models (the 5 GB files of ADR 0039) are desktop only. The web gets `.bimbleview`
  (`docs/adr/README.md:47-48`).
- Long calls block the page (sync). `open` and `export` must become jobs that report through
  the change feed, before any release.

## 4. Editing in web-ui

Answer to question 2: this can start now. It is milestone M2.

What the lab has today:

| Feature | Lab | With the C core |
|---|---|---|
| Select by click, properties | `glviewport.ts` (CPU pick), `Properties.vue` | works |
| Rename, sizes, material, level offset | `Properties.vue:37`, `:58`, `:66`, `:77` | works (6 verbs) |
| Place panel: kind, storey, material, sizes | `shell/PlaceSide.vue:24` | panel works; **click-to-place in the GL viewport is missing** (I: no placing code in `glviewport.ts`) |
| Undo, redo with labels, history list | `QuickBar.vue`, `pages/History.vue` | works |
| Sheets: add, rename | `pages/SheetsSide.vue:32`, `:42` | verbs not in C |
| Rotate, mirror, copy | calls exist (`bimble.ts:747-750`) | verbs not in C (`verbs.c:860-915`) |
| Small screens with light edits | `small/` | works |

What only the egui desktop has (module, lines): snapping (`snap.rs`, 293), polygon slabs
(`edit.rs:361`), measure (`measure.rs`, 383), view cube (347), section box handles, 2D views and
reference plans (`viewing.rs`, 2,034), the paper view (`paper.rs`, 541), sheet layout tools
(`layout.rs`, 1,389), style editor (966), schedules (599), materials page (541), collaboration
(332, desktop only: `collab.rs:4`).

Order to build in Vue. Each step is usable alone:

1. **Click-to-place** with a ghost and snapping. Snap and the click state machine go in C
   (as `Editor::click`, `crates/bimble-viewer/src/edit.rs:318`). TypeScript sends the ray.
2. **Modify**: multi-select, delete, move, copy, rotate, mirror, with typed values first.
3. **See**: section box, hide and isolate, measure, view cube, plan and section views as cameras.
4. **Paper**: the drawing shown as the SVG the core writes; then sheet layout tools.
5. **Standards**: style editor, schedules, materials.

## 5. Port order

Left in Rust, not in C (`docs/adr/0067-c-core-for-the-lab-loop.md:109-112`), with Rust sizes (M):

| Capability | Rust lines | User value | Risk | Milestone |
|---|---|---|---|---|
| IFC display reader, all classes, mapped shapes (`ifc/read.rs`, `import.rs`) | 4,080 | very high (most use is viewing, ROADMAP principle 9) | medium: breadth | M1 |
| Binary `.bimble` (CBOR + gzip), `.bimbleview` (`geom/viewfile.rs`, `codec.rs`) | 1,092 + part of `file.rs` | high | low | M1 |
| Viewer rendering: hatches, section box, hidden edges, instancing (TypeScript + GLSL) | replaces 5,882 | high | **high: unmeasured on old GPUs** | M1 |
| One plan as SVG (`drawing/cut.rs` + a slice of `plan.rs`) | spike | proves the product is portable | high | M1 (spike) |
| Remaining 8 verbs, views, sheets, schedules in the model | about 2,500 | high | low | M2 |
| Openings and booleans (`geom/boolean.rs`) | 345 + kernel | high | medium: kernel binding | M2 |
| IFC4 and IFC2x3 export (`ifc/lib.rs`, `ifc2x3.rs`) | 899 | high | low: text, IfcOpenShell checks it | M2 |
| Drawings: plans, sections, sheets, templates, schedules, pens, hatches, style | 6,635 + 1,473 | **the product** | high: `plan.rs` is 2,755 dense lines | M3 |
| PDF and font metrics (`drawing/sheet.rs`, `fonts.rs`) | about 600 | the product | medium | M3 |
| Parallel loading, memory budget, 5 GB files (ADR 0012, 0033, 0039) | spread | high on desktop | high | M4 |
| History store, collaboration (`core/repo.rs`, `remote.rs`) | 2,219 | medium | medium: file formats | M4 |
| Map projections (`core/georef.rs`; 403 systems in `crs_table.rs:4`) | 317 + data | medium (Swiss LV95 first) | medium: float parity | M4 |
| CLI and MCP (`bimble-cli`) | 2,879 | high for agents | low | M4 |
| Reference plans DXF, PDF (`drawing/reference.rs`) | 707 + two libraries | low | high | M5 |
| Model comparison, detail levels (`geom/compare.rs`, `hlod.rs`) | 904 | low | low | M5, only when measured |

### Milestones

| | Ships | Exit criteria |
|---|---|---|
| **M0 Freeze** | The final Rust release and the oracle (section 6) | Goldens for every row above are committed. CI runs the C tests without a Rust toolchain. |
| **M1 Web viewer on C** | The public viewer moves from 16 MB to under 400 KB | `scripts/compare_ifcopenshell.py` passes on the same fixtures as Rust. Meshes byte-equal on the goldens. The 20,000-element fixture meets the frame budget on the reference old laptop (`ROADMAP.md:57`). `web-ui/perf` budgets pass. One plan SVG byte-equal (spike). |
| **M2 Editing** | Full editing in the lab; desktop preview as option A | All 14 verb transcripts equal. No file is refused any more (openings, views, sheets, schedules, georef kept on round trip). IFC exports byte-equal and valid in IfcOpenShell. |
| **M3 The drawing** | Plans, sections, sheets, PDF from C, in the web editor and on the desktop | Every SVG golden byte-equal. Every PDF equal object by object after inflating streams. Pierre's office prints one real sheet set from C. |
| **M4 Desktop parity, Rust retired** | Installers; window shell B; history; large files; CLI; MCP | Opening benchmarks within the budget against the final Rust release. History folders written by Rust read and merge in C. A parity checklist against the egui desktop, signed by Pierre. Then `crates/` is deleted. |
| **M5 Later** | Reference plans, comparison, new features (rebar, LCA…) | Per feature. |

Why this order. M1 first: most users view, the viewer is nearest to done, and it tests the
riskiest unknown (WebGL2 on old machines) early. The plan spike is in M1: if plans cannot
reach parity, learn it in week one, not in M3. M2 comes before M3 because drawings need views,
sheets and openings in the model. The frozen Rust desktop stays available until M4.

### Dependencies in C

Keep the rule: vendor small single-purpose files; write what is small; bind only what is
research-grade.

| Need | Rust today | In C | Why |
|---|---|---|---|
| Mesh booleans | `manifold-csg` (`geom/boolean.rs:9`, `:167-172`) | **Keep manifold through its C binding.** Do not port it. Native: a prebuilt library, loaded dynamically in tcc dev builds (I). Web: manifold's own wasm, called through JS as today (`crates/bimble-geom/Cargo.toml`), fetched only when a model has openings | Manifold has C FFI (`MANIFOLD_CBIND`), no required dependency, Apache-2.0 (https://github.com/elalish/manifold). Its wasm is 541 kB (https://app.unpkg.com/manifold-3d@3.5.1/files). It is the only C++ in the tree. |
| PDF | `pdf-writer` (`drawing/sheet.rs:376`, `:544-606`) | **Own writer**, 600–900 lines (E) | Bimble uses a small subset: objects, content streams, one embedded TrueType font. |
| Font metrics | `skrifa` (`drawing/fonts.rs`) | **Own reader** of five TrueType tables for the shipped Roboto, about 300 lines (E). Fallback: `stb_truetype.h` 1.28, 5,321 lines, public domain or MIT (https://github.com/nothings/stb) | Only advances and a few metrics are needed. The font is our own file. |
| Rasterizing sheets | `resvg` (`drawing/sheet.rs:626-688`) | **Do not port.** The page shows the SVG. PNG from the CLI waits for a need | Largest dependency, least value. |
| Deflate, inflate | `flate2` | One vendored file or own code. Not evaluated here | Compressed bytes need not equal Rust's. Compare after inflating. |
| CBOR | `ciborium` | Own, about 300 lines (E) | Small format. Must read every file Rust wrote. |
| Projections | `proj4rs` (`core/georef.rs:133-139`) | Own, one projection family at a time, Swiss first. Refuse the others with a clear error | Goldens with a tolerance: results of `sin` and `cos` may differ in the last bit between libraries (I). |
| Threads | `rayon` | pthreads natively; Workers on the web | |
| DXF, PDF reading | `dxf`, `lopdf` | Last. Option: parse in the page and hand polylines to the core as data (I) | Reference plans are dead geometry (ADR 0021). |
| Hash, floats | `blake3`, serde's float printer | Done (vendored BLAKE3 and Ryu) | |

## 6. The final Rust release as the frozen oracle

It must contain:

1. Everything Pierre wants on `main` (the release branch and the fixes). The last installers and
   the last Rust web build, published as before.
2. **A general dump command**, not only the fixed example. Today
   `crates/bimble-api/examples/ccore_golden.rs` writes a fixed set. Promote it to
   `bimble oracle <model> <out-dir>`: one model in, every derived output out (canonical JSON,
   meshes, queries, verb transcripts from a script, IFC4, IFC2x3, each view as SVG and as PDF
   with streams uncompressed, schedules as CSV, `.bimbleview`, CBOR). Fixed timestamp, seeded
   GlobalIds. Without this, parity can only be checked on models chosen before the freeze.
3. Goldens for every row of section 5, dumped and committed before the tag, on more synthetic
   models than today's two (openings, views, sheets, georef, mapped shapes, large counts).
4. A recorded transcript of the viewer-level calls from the headless twin, for the session layer.
5. Fixture folders of the history store, written by Rust.

How it stays usable:

- Tag `rust-final`. The release attaches a static Linux `bimble` with its SHA-256. CI downloads
  it by hash and runs `bimble oracle … --check`. No cargo in CI after that.
- The oracle is frozen, so it can be wrong. Keep `ccore/golden/DEVIATIONS.md`: each place where
  C differs on purpose, with the reason and who decided. A golden is edited by hand only there.
- New features after the freeze have no oracle. They get their own goldens and IfcOpenShell,
  which stays the reference (`CLAUDE.md`).
- Promise to users: C opens every file the final Rust release wrote.

ADR 0067 was amended on 25 Nov with Pierre's decision (full move to C after the final Rust
release, which becomes the frozen oracle). ADR 0001 ("Rust core, own wgpu renderer") still needs
a superseding ADR, together with the port order of section 5.

## 7. Risks, gates, and what not to do

| Risk | Gate or mitigation | Change course when |
|---|---|---|
| WebGL2 in a browser on an old laptop replaces the native wgpu path. Never measured (`ROADMAP.md:148`). | Measure in M1: 20,000 elements on an HD 520-class machine, in Chrome and in each system webview. | Under 30 fps in Chrome: stop and study a native GL viewport before deleting `bimble-render`. Fine in Chrome but not in a webview: Electron on that OS. |
| C reads untrusted files (ADR 0008). Rust gave memory safety for free. | Web: the wasm sandbox. Native: ASan and UBSan on every test run in CI; a fuzzer per parser (STEP, JSON, CBOR, inflate, TrueType, HTTP). | A memory bug reaches a release, or fuzzing still finds crashes after a week on one parser. |
| Plans do not reach byte parity (float order, hidden lines). | The M1 spike. | No equal SVG after an agreed time box: accept a tolerance in an ADR, or stop. |
| Float parity where Rust calls `sin`, `cos`, `atan2` (rotate, arcs, projections). The freestanding wasm has no libm. | One own implementation, used by native and wasm; goldens with a stated tolerance where needed. | — |
| The freeze is long. Users wait for features. | Web viewer first; the Rust desktop stays downloadable; no new Rust features. | M3 slips past the date Pierre sets: review the scope, not the rule. |
| The 233 ms loop grows with the code. 12k lines build in 73 ms (M); 60k would be about 350 ms (E, linear). | Keep `ccore/bench/lab_loop.sh` as a budget in CI. | Median over 500 ms: split the host build. |
| Windows and macOS: the C host never ran there (`docs/adr/0067-c-core-for-the-lab-loop.md:98`). | Both in CI in M0. | — |
| Agents lose `render view to PNG` (MCP) with wgpu gone. | Give agents the SVG of a view (a ROADMAP idea, `ROADMAP.md:32-33`). | Agents need pixels: a headless browser, in CI only. |

What not to do:

- Do not write a native GUI, and do not port egui or wgpu.
- Do not add Rust features after the freeze. Two living cores is the one thing Pierre ruled out.
- Do not port manifold or resvg.
- Do not make the `Wasm` interface asynchronous now. It touches every Vue caller.
- Do not ship a fixed WebView2 (over 250 MB).
- Do not let TypeScript grow logic. Snap, placing and view state live in C.
- Do not claim a size or a speed before its benchmark. Every **E** above is unproven.

## 8. Decisions needed from Pierre

1. Shell: A first, then B on Windows and macOS, Electron only as the measured fallback?
2. Order: M1 web viewer before M3 drawings, with the plan spike inside M1?
3. Booleans: keep manifold (C++) through its C binding?
4. The oracle: `bimble oracle` in the final Rust release, and the deviations file?
5. Which old laptop is the reference machine, and who measures on it?
