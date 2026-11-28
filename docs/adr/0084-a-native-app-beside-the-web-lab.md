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

9. **Every optimisation is a setting with a good default and an off value** (Pierre's rule, 27 Nov 2026),
   the lab's own keys, shown as "Optimisations" in Settings and in the Performance panel, and in the
   report's `optimisations:` line: `moving_fps` (the moving budget, 30; 0 off), `moving_proxies` (far
   parts as their proxies, true; false off), `link_gpu_mb` (the links' detail on the GPU, 768 MB; 65536
   off: no limit), and the View tab's Edges (the session's `edges` action; screen-space, on). The cull
   pass on the GPU is how this renderer draws: always on, no setting (the lab's `gpu_cull` is WebGPU's
   culling of copies, another thing).

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

The owner's folder was not measured in ticket NUI1 (the machine's measurement lock stayed held by other
runs for the whole window); NAT2's numbers on it (prototype I: first picture 71 ms, moving 18 ms, still
54 ms, input → frame 14.5 ms median) remain the reference until the next run. The edit loop (`make dev-native`): an edit of a shell C file → tcc build 67–110 ms → an
offscreen shot written: 188–258 ms.

## Alternatives considered

- **H, the lab in a web view over the native picture**: the whole UX on day 1, but every input through
  WebKit's page, every mesh held twice, three processes, a glue layer per OS (spike NAT2, section 6).
- **The web lab only, faster**: the browser's ceiling on 100 M triangles; WebGPU on Asahi reports a
  software adapter (NAT1); Pierre asked for native.
- **Generating the native shell from the Vue components**: no tool reads `.vue` into ImGui; the shared
  data covers what is declarative today.

## Amendment (27 Nov 2026, ticket NUI2): the 3D by tiles, occlusion exact to groups of elements, what the window needs

Decided by Claude on ticket NUI2 under the owner's delegation ("performance is everything"; "every optimisation a
setting with a good default and an off value"; "60fps is fine"), on measured facts only. Code: `ccore/native/tiles_gpu.c`
(tile sets, pages, the frame's commands, the passes), `shaders/{face_proxy.vert, hiz0, hizn, ttest, gate, seen}.comp`,
`loader.c` (a tile's bytes from the mapped cache, the open model's pyramid), `ui/view3d.c` (tile sets per link and for
the open model), `ui/slog.c` (the session log), `ui/nset.c` (the app's own settings), `ui/compare.c` (`--compare`).

10. **The first window, made robust by construction** (the owner's first launch: Wayland, display scale 2, a tiling
    compositor; SDL's assertions on the viewport and scissor, one abort). Every size of a frame comes from the swapchain
    texture acquired at the frame's start (the composed picture, ImGui's display size and framebuffer scale, the 3D
    targets); a minimised or lost swapchain skips the frame; resize events only log. SDL's assertion handler logs and
    continues. `--resize-script` reproduces a compositor's sizes offscreen (the window first, the target a frame later,
    smaller than the last); with the old sizing it raises SDL's own assertions, with this none. `--selftest-window`
    checks a hidden window (claimed by the GPU, its scale, sizes, present modes) without showing anything.
11. **A local session log** (`slog.c`, on in a window, off: Settings): versions and build id, GPU, sizes, settings,
    files and their times, actions, SDL's errors and assertions, a crash's raw stack; `frames.csv` a row a second,
    `inputs.csv` over 100 ms, `report.txt` every 10 s; the last 20 sessions; `bimble --last-log`; `native/symbolize.sh`
    with the debug file kept by build id. Nothing leaves the machine. **Copy** writes the report to a file too.
12. **Tiles are the unit** (`renderer_path=tiles`). A link whose cache holds a section `TILE` is a tile set (its table of
    contents copied at open); the open model too (its pyramid built by the session after the open, in steps on a
    loader thread; a view file opened as it is, which the parts path could not draw, now draws). Per frame, per set,
    one call of the walker (`nv_tiles_walk`: SHELL1's parameter is a one-line change) with `error_px` (1), the moving
    budget's triangles, the levels of detail (`lod`) and `descend` marks under hidden or selected elements. A tile not
    loaded is covered by its nearest loaded ancestor's proxy (drawn in pass 2 only: it deviates from the detail by its
    error and must not hide the tiles it stands for). Tiles are loaded on the loader threads (a proxy's `TPX1` as stored,
    its u16 positions decoded by the vertex shader; a leaf's `TLF1` merged and instanced as the parts were, mirrored
    placements' triangles reversed), the visible first, within `link_gpu_mb` (tiles on their way counted; full: the
    least recently used released; the status bar says when the budget holds parts simplified, one click raises it).
13. **Pages and buckets, not a draw per tile.** Tiles live in shared pages (merged vertices, indices, shapes' vertices,
    placements, proxies' positions and their table), one multi-draw per (pass, page, pipeline). Measured on the owner's
    project: recording thousands of per-tile draws cost 5-45 ms of CPU a frame; with pages, 0.3-1.5 ms (37 draws). Every
    indirect command still costs about 1 us on this driver (Mesa's multi-draw is emulated): pass 1's runs are joined.
    Leaves keep f32 positions (16 bytes a vertex) and u32 indices: the 21-bit lattice (8 bytes) is left (decision 36 of
    ADR 0078): GPU bytes are not the bound on the owner's project with occlusion (the visible tiles only are loaded).
14. **Occlusion, exact, in two passes on this frame's depth** (`draw_visible`, on): pass 1 draws what was seen last
    frame; a depth pyramid (the farthest of 4 x 4 pixels, then of 2 x 2, levels side by side) is built from it; every
    other candidate's box is tested on the GPU (hidden when its nearest point is behind the farthest depth over the
    texels it covers) and the commands are gated on the GPU, the visible drawn in pass 2 of the same frame; no depth
    pre-pass, no sorting. **Down to groups of elements** (`occlusion_groups`, on; `occlusion_group`, elements a group):
    the id buffer gives the elements seen (`seen.comp`, read back); a loaded leaf's elements in groups are drawn in pass 1
    when one was seen, else tested on the group's box (its elements' boxes; the leaf's elements kept in space order,
    Morton order of their centres, so that a group is compact). Tiles not loaded and proxies keep the tile's box. Tiles
    are loaded only when their box is found visible. **Measured where the triangles go** (`BIMBLE_TILES_SURPLUS`):
    every surplus triangle is in a pass 2 group whose box passed while none of its elements shows, or in a pass 1 group
    hidden since; smaller groups draw near the need (18 buildings, outside: need 0.68 M, group 1 1.26 M, group 8 3.92 M,
    group 32 5.15 M) but each group is a command, and a command costs about 1 us of CPU here: group 2 by default (the
    owner's project's A/B). What is left of the surplus is boxes passing the test while their element is hidden
    (elements touching a visible facade, slabs spanning a floor): a finer depth pyramid (`hiz_block` 1 or 2 pixels a
    texel instead of 4) changed nothing (3.91 against 3.92 M); a tighter test (a few points of the element, an oriented
    box) is the next step, not a finer pyramid.
15. **The truth test** (`--truth`): each 3D frame drawn without occlusion (and without back-face culling) then as set,
    the id and depth buffers compared; an element is missing when one of its pixels shows something farther as set.
    Another element at the same depth winning a pixel is a coplanar tie (the order of the passes decides), counted apart.
    Synthetic sets and the owner's project: 0 missing with occlusion (tables below and in the report).
16. **Back faces** (`cull_back`): closed outward elements drawn with back faces culled. The truth test finds elements
    lost with it (17 synthetic links: 312,010 element-frames over 2,233 frames; the fixture: 35 frames of 1): the flag or
    its orientation is wrong for some elements. **Off** by default until that is found.
17. **The section box by clip distances** on the tile path (`section_clip_planes`, on): six clip planes in the vertex
    stage, the fragment stage without `discard` (what a tile-based GPU prefers); `discard` kept for the A/B.
18. **Settings, the owner's rule.** The web lab's keys where they exist (`moving_fps`, `moving_proxies`,
    `link_gpu_mb`; VIS1's `draw_visible`, `still_px`, `cull_back` read from the session when it knows them); the native
    ones (`renderer_path`, `error_px`, `lod`, `occlusion_groups`, `occlusion_group`, `section_clip_planes`,
    `session_log`, `bench_skip`) in `<config>/native.json` (the session's settings refuse an unknown key). Each in
    Settings › Optimisations and the report's `optimisations:` line; `--set key=value` for a run; `--compare` runs a
    script under several sets.

### Defaults, from the tables (NUI2)

Synthetic tables in `ccore/results/native/nui2.md`; the owner's project's in the ticket's report (private data).
`renderer_path=tiles`: on the owner's project the tile path's moving and still frames beat the parts path's at p95;
the parts path draws nothing of a view file opened as it is. `draw_visible` (occlusion) on: it removes about half of
the triangles there, and the truth test finds 0 element missing on his data (inside and outside cameras) and on
the synthetic sets. `occlusion_groups` on, `occlusion_group` 2: on the owner's project groups of 2 / 8 / 32 give moving
p95 17.5 / 17.9 / 18.4 ms and still medians 12.8 / 16.5 / 15.2 ms (the 18 buildings: 14.1 / 13.1 / 13.3 ms moving p95,
1.1 / 2.5 / 3.7 M triangles). `hiz_block` 4 (1 and 2 measured equal). `still_px` 1: on the owner's project the still
frame's triangles fall by about a third with it. `error_px` 1: 2 changed the triangles by under 5 % there (the cost is
the leaves, not the proxies). `moving_fps` 60 (the owner's target) when the settings file does not hold it.
`cull_back` off (decision 16). `section_clip_planes` on. `frames_in_flight` 2: on the owner's project moving
16.6/22.4 to 12.0/17.9 ms (median/p95), still 19.0/27.9 to 16.5/25.5 ms; 18 buildings, moving p95 13.4 to 12.4 ms.
**The owner's 60 fps line** (p95 16.7 ms moving and still, input to frame under 33 ms, 0 missing): with the defaults
input to frame max 23-27 ms and 0 missing (356 frames, inside and outside) are met; moving p95 17.5 ms and still p95
24 ms (frames while tiles arrive included) are not yet. A key the user set in his settings file wins (one key, shared with the web lab).

### What is left (NUI2)

The element-level pass entirely on the GPU (a command list built by a compute pass needs an indirect count SDL does
not offer; today pass 2's groups are one command each). Back-face culling's loss with occlusion (decision 16). The
21-bit lattice for leaves (8 bytes a vertex instead of 16). The baked occluders after a camera jump (pass 1 is empty
then: the first frame tests against nothing, exact but slower). The web lab's session log. `moving_fps` 60 and
`draw_visible` for the native renderer as the core's own defaults (settings.c). The open model's pyramid is rebuilt
at each open of a view file (4-6 s for the 18 buildings): kept in its cache later.

## Amendment (27 Nov 2026, ticket NUI3): tighter visibility, the scene origin, desktop Linux, CI on lavapipe

Decided by Claude on ticket NUI3 under the owner's delegation ("performance is everything"; decisions on facts measured on
his data; every optimisation a setting with a good default and an off value). Synthetic tables:
`ccore/results/native/nui3.md`;
the owner's project's numbers: the ticket's report (private data). Code: `ccore/native/tiles_gpu.c`, `shaders/ttest.comp`,
`ui/desktop.c`, `ui/view3d.c`, `ui/app.c`, `ui/drive.c`, `native/build.sh`, `native/linux/`.

19. **What the surplus was.** `BIMBLE_TILES_SURPLUS=2` lists the largest elements drawn but not shown: on the owner's project
    a few single meshes of hundreds of thousands of triangles whose boxes wrap the building (no box test can hide
    them), then long or diagonal members (their world boxes loose). **Clusters** (`cluster_tris`, 512; 0 off): a merged
    element of more triangles
    is cut at load into clusters of 512 in space order (Morton order of the triangles' centres), each with its own box, test
    and draw range; the element's sphere still decides `still_px`. **Each element tested by its own box**
    (`occlusion_test` 2; 0 NUI2's group box, 1 each element's world box): pass 1 draws exactly the elements (clusters) seen
    last frame; the others are drawn in groups of `occlusion_group` (2), each element tested by its own box, the group's gate
    shared (a gate is only ever set to 1); mode 2 takes the element's placed box (an oriented box) when its volume is under
    0.9 of its world box's. Tests are boxes by centre and half axes. **Pieces** (`occlusion_split`, `occlusion_span`,
    `occlusion_split_px`; 1: off): a box passed and large on screen tested again in up to N^3 pieces: exact (their union is
    the box), the surplus fell further, but the GPU paid more than it saved on the owner's project: off.
    **Leaves under their box** (`occlusion_tiles`, on): a loaded leaf whose box was hidden last frame is drawn under that
    box's test alone (one gate, a few commands, no test an element). The truth test: 0 missing on the owner's project inside
    and outside and on the synthetic sets with these defaults.
20. **The scene origin where the triangles are.** A project of links alone took its origin from the first link ready: a small
    link in a local frame beside links at national coordinates (2,600 km) put every other link at f32 steps of
    distance / 2^24 (155 mm). The origin is now the ready link's placed origin with the most triangles (else elements) of
    ready links within 1 km; a heavier group ready later moves it (links re-placed, the camera shifted). `native_geo_test`:
    the same building at 0 and at 2,600 km beside a local link draws the same picture (0.13 % of pixels differ; 1.7 % with
    the old origin); Measure 18,300 mm there (MEAS2).
21. **Desktop Linux.** The system's file dialogs (SDL: the portal, else zenity; failed: the app's own for the session,
    `system_dialog`); a file dropped opens (Shift: an IFC linked), a folder is linked; the title is the file's name, the app
    id `ch.bimble.bimble` (Wayland's app_id: a compositor's rules); a mouse's wheel and Ctrl + wheel zoom, a touchpad's two
    fingers pan (`touchpad_pan`), a pinch zooms; the section box by numbers (`box_panel`); a command palette (Ctrl+K); the
    UI's clipboard calls logged; `desktop/` in the release archive (the .desktop file, MIME types for .bimble, .bimbleview,
    .ifc, icons, `install.sh` for the user only). Offscreen checks: `native_app_test` 3f.
22. **Loading.** Four loader threads (`loaders`; 2 before): on the owner's project complete and the first correct picture
    after a reset sooner, frame times unchanged within the noise. Exterior first
    (`shell_mode` priority, the cross-link shell attached to the tiles) orders the fetch; measured no sooner there (the
    loaders' throughput is the bound), kept (it never removes anything).
23. **Back faces stay drawn** (`cull_back` off). Not a convention bug (an inverted winding or an unflipped mirror would lose
    whole elements without occlusion too). Two causes measured: the rasterizer's tie rule on silhouettes (a pixel centre on
    the edge between a front and a back triangle of a closed mesh belongs to one of them: when it is the back one, culling
    drops it; an element seen only through such pixels, 8 px or fewer, vanishes: the fixture without occlusion loses 19
    elements in 7,373 frames, all of 8 px or fewer), and an interaction with the element-level occlusion not yet explained:
    on the 17 synthetic links culling loses about 100,000 element-frames with it, about 140 (all of 8 px or fewer) with
    occlusion by tile only or none, about 15,000 when only one of the two passes culls; frames in flight and group sizes
    change nothing. It did not pay either (the same frame times on the owner's project).
24. **Still frames** (`occlusion_cold` 1): each test's own answer is read back; in a still frame the elements a leaf's tests
    found hidden last frame wait under one gate (their own tests or-ed: exact), one command a run instead of one a group,
    so the CPU's recording and the GPU's indirect commands fall where most elements are hidden; moving frames keep their
    small groups (the gate shared by everything hidden made moving frames slower). Two levels of the depth pyramid a
    dispatch (`hiz_pair`, on). Measured and kept off: `seen_step` 2, `still_ramp`, `pace_tiles`, `page_grow` 1.
25. **CI on lavapipe.** The job's glob missed Mesa 25's `lvp_icd.json` (no driver: "No supported SDL_GPU backend"); with it,
    lavapipe 25.2 crashed in `ttest.comp` reading the pyramid as a read-only storage image (a null function in its JIT code,
    found by `BIMBLE_SKIP_CS`); read by a sampler it runs (an ubuntu:24.04 container: `native_app_test` passes). The SPIR-V
    is committed (`native/shaders/gen`, checked by `native/build.sh shaders --check`): the release job on Ubuntu 22.04 needs
    no glslc.

### The 60 fps line (NUI3)

On the owner's project (numbers in the ticket's report): input to frame under 33 ms and 0 missing are met; moving p95 is at
the line, within the noise of a shared machine; **still p95 is not met**. The slow still frames are the full-detail frames
after a zoom while tiles keep arriving (millions of triangles: several ms of CPU recording, tens of thousands of indirect
commands, and the GPU's drawing) and a GPU spike when a 202 MB page is made. Where an empty frame's GPU time goes, by
leaving passes out (`bench_skip`, `BIMBLE_SKIP_CS`): the depth pyramid, the tests and gates, the elements seen and the
edges, each between a quarter and one third of it. Measured and not kept: pacing frames for tiles to the GPU
(`pace_tiles`, worse), one frame in flight (worse), groups of 4 or 8 (the same), 51 MB pages (`page_grow`, the same),
the elements seen from one pixel in four (`seen_step` 2, the same), a still frame ramping from the moving budget
(`still_ramp`, the same). Kept: the cold elements of a still frame under one gate (decision 24): still p95 lower by
several ms there, still above the line.

### What is left (NUI3)

The still frame: fewer commands (an element-level list built on the GPU needs an indirect count), the whole depth pyramid
in one dispatch. Back-face culling with the element-level occlusion (decision 23: a loss not yet explained). Baked
occluders after a camera jump (not built). The section box at national coordinates (MEAS3's `base`). A session in a real window on the owner's machine (Wayland, Hyprland): the portal dialog, the clipboard, pinch events.

## Revisit when

- The tile pyramid (TILE1) lands: the parts become tiles (Decision 4).
- A screen the owner uses daily is still "not yet (native)" after NUI3.
- macOS or Windows users ask for it (SDL_shadercross, Decision 7).
