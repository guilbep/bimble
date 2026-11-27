# ADR 0084 — A native Linux app beside the web lab

- Status: Accepted, 27 Nov 2026
- Decided by: Pierre asked for it ("I think you need to work on a native ui based on our core/rendering
  engine. reuse the full web-ui ux for now. (two renderers: the web-ui + native linux for now)") and
  delegated the choices ("I trust you to make the right call … performance is everything"), on 27 Nov
  2026. The toolkit and the decision to build prototype I, not H, come from spike NAT2
  (`ccore/native/RESULTS-NAT2.md` on the branch `spike/native-app`); the design below is ticket NUI1's.
- Extends: ADR 0072 (main is the C core), 0074 (200 ms), 0078 (3D speed), 0079 (links read in place).
- Reversal cost: low. The app is a carrier of its own (`ccore/native/`): deleting the folder, its build
  mode, its CI job and its rows in the release takes the core and the web lab back to what they were.
  `ccore/src` and `ccore/session` are unchanged by it.

## Context / Problem

On his project of 17 linked IFC files (252,189 elements, 110.5 M triangles) the web lab gives Pierre
moving frames of 87–96 ms, still frames of 141–261 ms and clicks of 0.6–0.7 s. Spike NAT1 drew the
same folder natively through SDL3's GPU API in a fraction of that; spike NAT2 compared two ways to put
the lab's UX around it (H: the Vue lab in a WebKitGTK view over the native picture; I: a native shell in
C on Dear ImGui) and recommended I. The spike's app was not a program anyone could install: it loaded
every link one after the other (6.5 s), kept the session's copy of each link beside the renderer's
(3.3 GB at load on the owner's folder), read its data from the source tree, and had no Home, no file
menu, no release, no test.

## Decision

1. **Two products, one core.** The web lab (`web-ui/`, served by `bimble-ccore open` or the wasm
   module) stays the reference UI and the browser product: screens are designed there first. `bimble`,
   the native Linux app, is the desktop app for large projects. Both run the same C session
   (`ccore/session`, `cc_call`): the native app calls it in-process, with no HTTP.
2. **Toolkit: Dear ImGui 1.93 (docking) through cimgui, the shell in C, on SDL3's GPU API** (spike NAT2,
   section 1: it draws in the renderer's own command buffer, tcc builds the shell's C, MIT; Nuklear and
   microui lack a renderer and widgets, Clay draws nothing, GTK4 brings a second compositor, egui is Rust).
   ImGui, cimgui and nanosvg are pinned by commit and checked file by file (SHA-256) by
   `ccore/native/deps.sh`; SDL 3.4+ and FreeType are the system's (Linux), SDL built from source at a
   pinned release where it is not packaged (`ccore/ci/sdl3.sh`).
3. **What keeps the two UIs in step.** The ribbon (tabs, groups, tools, icons as their SVG), the
   capability map, the key table, the strings (FR, EN), the six themes' tokens and the shell machine's
   shape are exported from the TypeScript (`web-ui/scripts/export-shell-data.mjs`) to
   `ccore/native/ui/data/*.json`, committed, and built into the binary; `ccore/ci/run.sh unit` fails when
   they differ from the sources (`--check`). The shell machine (`web-ui/src/shell/machine.ts`) is ported
   by hand to C (`native/ui/machine.c`), because its rows are functions (payloads, the state to go back
   to), not data; the export runs every row and records where it goes, and
   `ccore/test/native_machine_test.mjs` compares the port row by row. Refactoring `machine.ts` into data
   for the native app's sake was rejected: it would change the lab's reducer for a second client.
   Everything else (each screen) is ported on purpose, screen by screen; a control the native shell
   has not built says "not yet (native)" (its tooltip, a slate dot) and File › Open in the web lab
   starts `bimble-ccore open` on the same file.
4. **The 3D view is made of parts** (`ccore/native/render.c`): the open model, each link, a link's
   proxies. A part is uploaded on a loader thread with its own command buffers and added to the picture
   between two frames; the id-buffer pick and the screen-space edges do not depend on how geometry is
   chunked. This is the seam for the tile pyramid (ticket TILE1): a part becomes a tile, and "what to
   draw this frame" stays one walk per part (`hlod_walk`, the cull pass) taking the camera and budgets.
5. **Links load off the UI thread, one link per thread at a time** (`ccore/native/loader.c`): first
   each link's proxies from its cache's HLOD section (the first picture), then its detail, the largest
   on screen first, until the links' GPU budget (the setting `link_gpu_mb`, the lab's) is full; the
   others keep their proxies. A link read in place (SIZE3) is decoded from its cache's mesh blocks into
   the loader's own scratch, batch by batch under the session's read lock, and freed after the upload:
   the session never holds a link decoded for drawing (its decode budget stays for measure, boxes and
   volumes). The UI thread takes the lock as the writer around each call; a loader waits at most one
   batch (512 elements). The open itself runs on a thread of its own, with its progress on Home.
6. **Plans and sections** show in the native viewport with an orthographic camera and the view's box
   (`query view_clip`) as the clip; the cut drawn as on paper (hatches, cut lines) stays the web lab's.
7. **The release.** `ccore/build.sh native-app` (clang -O2, `-Werror` on the app's own files; ImGui,
   the kernel and libstdc++ static; shaders compiled by `glslc` at build time and embedded; the data and
   the lab's fonts embedded): one 5 MB binary needing SDL 3.4+ and FreeType. The Linux archive holds it
   beside `bimble-ccore` and `web/` when it builds (`NATIVE_APP=1` in the release job). Without a Vulkan
   device it exits 3 and prints the web lab's command. macOS and Windows are not built: each needs the
   shaders as MSL or DXIL (SDL_shadercross from the SPIR-V), SDL 3 there, and (macOS) a signed .app.
8. **CI.** The job `ccore-native` builds it and drives it offscreen on Mesa's lavapipe
   (`ccore/ci/run.sh native`: the data check, the deps check, the build, `test/native_app_test.mjs`,
   the same under ASan and UBSan). It is not a required check yet.

## What is native today, and what is not

Native: Home (recent files from the web lab's settings file, New, Open), the File menu (New, Open, Save,
Save as, Close, Link IFC… with several files, Link a folder…, Performance, Settings, keys, Open in the
web lab, Quit), the file dialog on the core's own lists (`fs_list`, its roots rule), the title row, tabs
and every tab's ribbon with the lab's enabled / pressed / "not available yet" states, the navigator
(views, links with their eyes and object trees, notes), Properties (with property sets, a linked
element's link), the Links panel (state and progress, eye, pin, colour, positioning, Reload, Unload,
Cancel, Report, Remove, Reload all), orbit / pan / zoom / pick, the view cube with Home and Fit, Top,
orthographic, edges, hide / isolate / show all / show previous for the model and its links, the section
box with its handles and Box to selection, Measure across links, plans and sections (orthographic,
clipped), Quantities, Settings (theme, "While the view moves", far parts simplified, the links' GPU
memory, name, e-mail), the Performance panel (the lab's report shape, Record 10 s, Copy), FR / EN, the
key table, the right-click menu, the six themes.

Not yet (native), each with the web lab beside it: placing and editing tools (their ghost), the 2D
drawings on paper, sheets, the other 20 pages and 15 dialogs, the palette (Ctrl K), drag and drop,
accessibility (no AT-SPI), IME beyond SDL's text input, portal file dialogs, a turned section box.

## Evidence

Synthetic, offscreen 1492×790 at scale 2 (2984×1580 px), Apple M1 Pro (Asahi, Mesa Vulkan), the
spike's orbit-zoom gesture (`bimble --preset orbit-zoom`), 17 synthetic links (8,024 elements,
3.93 M triangles), load 6.9–9.0:

| | NUI1 `bimble` | spike NAT2 prototype I | web lab (spike's Chromium run) |
|---|---|---|---|
| first picture | 69–93 ms | 47 ms | 1,595 ms |
| complete | 408–491 ms | 556 ms | 1,604 ms |
| moving frame median / p95 | 7.2–7.5 / 11–12 ms | 6.8 / 9.8 ms | 17.1 / 154 ms (interval) |
| still frame median / p95 | 5.4–6.5 / 9.4–9.9 ms | 6.5 / 10.6 ms | – |
| input → frame median / p95 / max | 7.3–7.7 / 11–12 / 15–19 ms | 6.4 / 10.3 / 14.5 ms | 9.5 / 42.2 / 87.6 ms |
| peak process memory | 85 MB | 334 MB | 1,818 MB |

The owner's folder was measured locally, under the shared measure lock, in a memory cap and with a
private cache folder; those numbers stay out of the repository (private data) and are in the ticket's
report. The edit loop (`make dev-native`): an edit of a shell C file → tcc build 67–110 ms → an
offscreen shot written: 188–258 ms.

## Alternatives considered

- **H, the lab in a web view over the native picture**: the whole UX on day 1, but every input through
  WebKit's page, every mesh held twice, three processes, a glue layer per OS (spike NAT2, section 6).
- **The web lab only, faster**: the browser's ceiling on 100 M triangles; WebGPU on Asahi reports a
  software adapter (NAT1); Pierre asked for native.
- **Generating the native shell from the Vue components**: no tool reads `.vue` into ImGui; the shared
  data covers what is declarative today.

## Revisit when

- The tile pyramid (TILE1) lands: the parts become tiles (Decision 4).
- A screen the owner uses daily is still "not yet (native)" after NUI3.
- macOS or Windows users ask for it (SDL_shadercross, Decision 7).
